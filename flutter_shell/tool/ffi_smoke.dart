// Headless FFI smoke test. Run with:
//
//   CROSSDESK_CORE_LIB=$(pwd)/../build/macosx/arm64/release/libcrossdesk_core.dylib \
//   CROSSDESK_CONFIG_DIR=/tmp/cd_smoke \
//   dart run tool/ffi_smoke.dart
//
// Verifies: lib loads, create/destroy, read/write/persist via ConfigCenter.

import 'dart:io';

import 'package:crossdesk_shell/core/core.dart';

void _log(String s) => stdout.writeln(s);

Future<void> main() async {
  final dir = Platform.environment['CROSSDESK_CONFIG_DIR'] ?? '/tmp/cd_smoke';
  Directory(dir).createSync(recursive: true);
  // Remove any stale config so we start from defaults.
  final ini = File('$dir/config.ini');
  if (ini.existsSync()) ini.deleteSync();

  _log('--- First open: read defaults, mutate, close ---');
  {
    final core = Core.open(configDir: dir);
    _log('version          = ${core.version}');
    _log('initial language = ${core.language}');
    _log('initial fps      = ${core.videoFps}');
    _log('initial signalH  = "${core.signalHost}"');

    final ok1 = core.setLanguage(Language.english);
    final ok2 = core.setVideoFps(VideoFps.fps30);
    final ok3 = core.setSignalHost('signaling.example.test');
    final ok4 = core.setSignalPort(8443);
    if (!ok1 || !ok2 || !ok3 || !ok4) {
      stderr.writeln('FAIL: setter returned false');
      exit(1);
    }
    _log('after writes: language=${core.language} fps=${core.videoFps} '
        'host=${core.signalHost} port=${core.signalPort}');
    core.close();
  }

  _log('\n--- Second open: values should be persisted ---');
  {
    final core = Core.open(configDir: dir);
    _log('language  = ${core.language}');
    _log('fps       = ${core.videoFps}');
    _log('signalH   = ${core.signalHost}');
    _log('signalP   = ${core.signalPort}');
    final pass = core.language == Language.english &&
        core.videoFps == VideoFps.fps30 &&
        core.signalHost == 'signaling.example.test' &&
        core.signalPort == 8443;
    core.close();
    if (!pass) {
      stderr.writeln('FAIL: persisted values did not match');
      exit(2);
    }
  }
  _log('\nOK: FFI roundtrip + persistence verified.');
}
