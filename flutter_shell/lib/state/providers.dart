// Riverpod providers — single source of truth for the shell.

import 'dart:io';

import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../core/core.dart';
import '../session/session_manager.dart';

/// Owns the C++ core handle for the process lifetime. We don't dispose
/// it because the platform tears down on process exit; if we ever need
/// hot-restart support we'll wire ref.onDispose to core.close().
final coreProvider = Provider<Core>((ref) {
  // Dev convenience: env-var override; otherwise fall back to CWD.
  final dir = Platform.environment['CROSSDESK_CONFIG_DIR'];
  return Core.open(configDir: dir);
});

/// Monotonically-increasing tick UI watches to re-read config after writes.
final configRevProvider = StateProvider<int>((_) => 0);

/// Path to the `crossdesk_session` binary. Override via env var in dev;
/// in shipped builds we resolve adjacent to the shell binary.
final sessionBinaryProvider = Provider<String>((_) {
  final env = Platform.environment['CROSSDESK_SESSION_BIN'];
  if (env != null && env.isNotEmpty) return env;
  // Adjacent to the running executable (e.g. inside Contents/MacOS).
  final exe = Platform.resolvedExecutable;
  final dir = exe.substring(0, exe.lastIndexOf(Platform.pathSeparator));
  final name = Platform.isWindows ? 'crossdesk_session.exe' : 'crossdesk_session';
  return '$dir${Platform.pathSeparator}$name';
});

/// Map of peer_id → active SessionManager. Mutable via [sessionsProvider.notifier].
class SessionsNotifier extends StateNotifier<Map<String, SessionManager>> {
  SessionsNotifier(this._sessionBin, this._core) : super(const {});

  final String _sessionBin;
  final Core _core;

  Future<SessionManager> open(String peerId, {required String password}) async {
    final existing = state[peerId];
    if (existing != null) return existing;
    final mgr = SessionManager(
      peerId: peerId,
      sessionBinaryPath: _sessionBin,
      core: _core,
      password: password,
    );
    state = {...state, peerId: mgr};
    // Re-emit on every internal state change so UI rebuilds.
    mgr.stateStream.listen((_) {
      state = {...state}; // identity change triggers Riverpod rebuild
    });
    try {
      await mgr.start();
    } catch (_) {
      // Leave the manager in state so UI can show the failure; caller decides whether to close().
    }
    return mgr;
  }

  Future<void> close(String peerId) async {
    final mgr = state[peerId];
    if (mgr == null) return;
    await mgr.dispose();
    final next = {...state}..remove(peerId);
    state = next;
  }

  @override
  void dispose() {
    for (final m in state.values) {
      // ignore: discarded_futures
      m.dispose();
    }
    super.dispose();
  }
}

final sessionsProvider =
    StateNotifierProvider<SessionsNotifier, Map<String, SessionManager>>(
  (ref) => SessionsNotifier(ref.watch(sessionBinaryProvider), ref.watch(coreProvider)),
);
