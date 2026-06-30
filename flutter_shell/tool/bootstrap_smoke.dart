// End-to-end bootstrap smoke test.
//
// Spawns crossdesk_session, sends a real bootstrap message with an
// unreachable signal host, then verifies the session reaches a state
// that proves the bootstrap path executed (signal_connecting / failed
// / connecting / etc.) — i.e. the session actually wired minirtc and
// asked it to connect, instead of silently no-op'ing.
//
// Usage:
//   CROSSDESK_CORE_LIB=$(pwd)/../build/macosx/arm64/release/libcrossdesk_core.dylib \
//   CROSSDESK_SESSION_BIN=$(pwd)/../build/macosx/arm64/release/crossdesk_session \
//   CROSSDESK_CONFIG_DIR=/tmp/cd_bootstrap_smoke \
//   dart run tool/bootstrap_smoke.dart

import 'dart:async';
import 'dart:io';

import 'package:crossdesk_shell/core/core.dart';
import 'package:crossdesk_shell/session/session_manager.dart';

const _validRemoteStates = {
  'signal_connecting', 'signal_connected', 'signal_failed',
  'signal_reconnecting', 'signal_server_closed', 'signal_closed',
  'connecting', 'gathering', 'connected', 'disconnected', 'failed', 'closed',
};

Future<void> main() async {
  final bin = Platform.environment['CROSSDESK_SESSION_BIN'];
  if (bin == null || !File(bin).existsSync()) {
    stderr.writeln('FAIL: set CROSSDESK_SESSION_BIN to crossdesk_session path');
    exit(1);
  }
  final cfgDir = Platform.environment['CROSSDESK_CONFIG_DIR']
      ?? '/tmp/cd_bootstrap_smoke';
  Directory(cfgDir).createSync(recursive: true);

  // Seed identity so the shell-side bootstrap actually has a local_id.
  final core = Core.open(configDir: cfgDir);
  if (!core.ensureIdentity()) {
    stderr.writeln('FAIL: ensureIdentity returned false');
    exit(2);
  }
  stdout.writeln('local identity: ${core.clientId}@${core.password}');

  // Use a deliberately unreachable host so the test doesn't hit the real
  // signal server. We only care that the session enters the connection
  // pipeline (i.e. emits at least one signal_/conn state).
  final unreachable = '127.0.0.1';
  core.setSignalHost(unreachable);
  core.setSignalPort(1);  // port 1 is reserved → connect will fail fast

  final mgr = SessionManager(
    peerId: '9999999999',          // arbitrary remote id
    sessionBinaryPath: bin,
    core: core,
    password: '123456',            // arbitrary remote password
    configDir: cfgDir,
  );

  final transitions = <String>[];
  final reached = Completer<String>();
  mgr.stateStream.listen((s) {
    final tag = '${s.phase.name}/${s.remoteState}';
    transitions.add(tag);
    stdout.writeln('  state: $tag err=${s.lastError}');
    if (_validRemoteStates.contains(s.remoteState) && !reached.isCompleted) {
      reached.complete(s.remoteState);
    }
  });

  stdout.writeln('--- starting session (with bootstrap) ---');
  await mgr.start(helloTimeout: const Duration(seconds: 5));

  // Wait briefly for the bootstrap message to round-trip through minirtc
  // and produce a state update. We don't expect a real connect — just
  // proof of life.
  String observed;
  try {
    observed = await reached.future.timeout(const Duration(seconds: 6));
  } on TimeoutException {
    stderr.writeln('FAIL: no signal/conn state observed after bootstrap');
    stderr.writeln('transitions seen: $transitions');
    await mgr.dispose();
    exit(3);
  }
  stdout.writeln('observed remote state: $observed');

  await mgr.stop(graceful: const Duration(seconds: 3));
  await mgr.dispose();
  core.close();
  stdout.writeln('OK: bootstrap reached cd_peer / minirtc and produced state.');
  exit(0);
}
