// End-to-end SessionManager smoke test (M4).
//
// Spawns the real crossdesk_session binary, awaits its `hello`, sends
// `request_close`, observes graceful exit. No GUI needed.
//
// Usage:
//   CROSSDESK_SESSION_BIN=/abs/path/to/crossdesk_session \
//   dart run tool/session_smoke.dart

import 'dart:io';

import 'package:crossdesk_shell/session/session_manager.dart';

Future<void> main() async {
  final bin = Platform.environment['CROSSDESK_SESSION_BIN'];
  if (bin == null || !File(bin).existsSync()) {
    stderr.writeln('FAIL: set CROSSDESK_SESSION_BIN to crossdesk_session path');
    exit(1);
  }

  final mgr = SessionManager(peerId: 'smoke-12345', sessionBinaryPath: bin);
  mgr.stateStream.listen((s) {
    stdout.writeln('  state: phase=${s.phase.name} pid=${s.childPid} '
        'remote="${s.remoteState}" err=${s.lastError}');
  });

  stdout.writeln('--- starting session ---');
  await mgr.start(helloTimeout: const Duration(seconds: 5));
  if (mgr.state.phase != SessionPhase.helloed && mgr.state.phase != SessionPhase.running) {
    stderr.writeln('FAIL: expected helloed after start, got ${mgr.state.phase}');
    await mgr.dispose();
    exit(2);
  }

  stdout.writeln('--- requesting close ---');
  await mgr.stop(graceful: const Duration(seconds: 3));
  if (mgr.state.phase != SessionPhase.exited) {
    stderr.writeln('FAIL: expected exited after stop, got ${mgr.state.phase}');
    await mgr.dispose();
    exit(3);
  }

  await mgr.dispose();
  stdout.writeln('OK: spawn → hello → request_close → exit verified.');
  exit(0); // stream subscription would otherwise keep the isolate alive
}
