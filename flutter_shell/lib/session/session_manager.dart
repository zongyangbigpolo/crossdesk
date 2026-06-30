// SessionManager: one instance per connection. Lifecycle:
//
//   1. start()    — pick endpoint path, bind IpcServer, spawn child, await hello
//   2. <runtime>  — push apply_settings / request_close; observe state/stats
//   3. stop()     — send request_close, wait for exit or timeout, kill if needed
//
// The shell may hold multiple SessionManagers in parallel (one per remote
// peer), each with its own socket and child process.

import 'dart:async';
import 'dart:io';

import '../core/core.dart';
import '../ipc/ipc_messages.dart';
import '../ipc/ipc_server.dart';

enum SessionPhase { idle, starting, helloed, running, closing, exited, failed }

class SessionStats {
  const SessionStats({this.rttMs = 0, this.bitrateKbps = 0, this.fps = 0, this.loss = 0});
  final int rttMs;
  final int bitrateKbps;
  final int fps;
  final double loss;
}

class SessionState {
  const SessionState({
    required this.peerId,
    this.phase = SessionPhase.idle,
    this.remoteState = '',
    this.lastError,
    this.stats = const SessionStats(),
    this.childPid,
    this.endpoint,
  });

  final String peerId;
  final SessionPhase phase;
  final String remoteState; // last 'state' message from session
  final String? lastError;
  final SessionStats stats;
  final int? childPid;
  final String? endpoint;

  SessionState copyWith({
    SessionPhase? phase,
    String? remoteState,
    String? lastError,
    SessionStats? stats,
    int? childPid,
    String? endpoint,
  }) =>
      SessionState(
        peerId: peerId,
        phase: phase ?? this.phase,
        remoteState: remoteState ?? this.remoteState,
        lastError: lastError ?? this.lastError,
        stats: stats ?? this.stats,
        childPid: childPid ?? this.childPid,
        endpoint: endpoint ?? this.endpoint,
      );
}

class SessionManager {
  SessionManager({
    required this.peerId,
    required this.sessionBinaryPath,
    this.runtimeDir,
    this.core,
    this.password,
    this.configDir,
  }) : _state = SessionState(peerId: peerId);

  final String peerId;
  final String sessionBinaryPath;
  final String? runtimeDir;
  /// Source-of-truth for bootstrap settings. If null, bootstrap is skipped
  /// (session window stays idle — useful for the M4 smoke test).
  final Core? core;
  /// 6-digit connection password. If null, bootstrap is skipped.
  final String? password;
  /// Config dir passed through to the session's own core instance. Defaults
  /// to whatever the session's path_manager picks if null.
  final String? configDir;

  final _stateCtrl = StreamController<SessionState>.broadcast();
  Stream<SessionState> get stateStream => _stateCtrl.stream;
  SessionState _state;
  SessionState get state => _state;

  IpcServer? _server;
  Process? _child;
  StreamSubscription? _stdoutSub;
  StreamSubscription? _stderrSub;
  Completer<void>? _helloCompleter;

  void _update(SessionState s) {
    _state = s;
    if (!_stateCtrl.isClosed) _stateCtrl.add(s);
  }

  Future<void> start({Duration helloTimeout = const Duration(seconds: 5)}) async {
    _helloCompleter = Completer<void>();
    _update(SessionState(peerId: peerId, phase: SessionPhase.starting));
    // Windows: Dart cannot bind to named pipes, so use loopback TCP with
    // an OS-assigned port. POSIX: stick with Unix domain sockets.
    final String endpointHint;
    if (Platform.isWindows) {
      endpointHint = 'tcp://127.0.0.1:0';
    } else {
      final dir = runtimeDir ?? Directory.systemTemp.createTempSync('cd_sess_').path;
      endpointHint = '$dir/sess_$peerId.sock';
    }

    try {
      _server = await IpcServer.bind(endpointHint);
    } catch (e) {
      _update(_state.copyWith(phase: SessionPhase.failed, lastError: 'bind: $e'));
      rethrow;
    }
    final endpoint = _server!.endpoint;

    _server!.listen(
      onMessage: _onMessage,
      onError: (e) => _update(_state.copyWith(lastError: 'ipc: $e')),
      onPeerClosed: () {
        if (_state.phase != SessionPhase.exited) {
          _update(_state.copyWith(phase: SessionPhase.closing));
        }
      },
    );

    try {
      _child = await Process.start(
        sessionBinaryPath,
        ['--ipc', endpoint, '--peer-id', peerId],
        mode: ProcessStartMode.normal,
      );
    } catch (e) {
      await _server?.close();
      _update(_state.copyWith(phase: SessionPhase.failed, lastError: 'spawn: $e'));
      rethrow;
    }

    _update(_state.copyWith(endpoint: endpoint, childPid: _child!.pid));

    // Pipe child stdio into our stdout so the dev sees session logs alongside Flutter.
    _stdoutSub = _child!.stdout.listen((b) => stdout.add(b));
    _stderrSub = _child!.stderr.listen((b) => stderr.add(b));

    // Reap the child when it exits.
    unawaited(_child!.exitCode.then((code) {
      _update(_state.copyWith(
        phase: SessionPhase.exited,
        lastError: code == 0 ? null : 'exit code $code',
      ));
      _cleanup();
    }));

    // Wait for actual hello message (TCP accept is not enough).
    try {
      await _helloCompleter!.future.timeout(helloTimeout);
    } on TimeoutException {
      _update(_state.copyWith(phase: SessionPhase.failed, lastError: 'no hello within $helloTimeout'));
      await stop();
      rethrow;
    }
    _sendBootstrap();
  }

  void _sendBootstrap() {
    final c = core;
    final pw = password;
    if (c == null || pw == null || pw.isEmpty) return; // smoke-test path
    // local_id is "<client_id>@<password>" (legacy minirtc signal-server format).
    // The 10-digit client_id is persisted per install by cd_identity_ensure;
    // the signal server will overwrite it once a server-assigned id arrives.
    c.ensureIdentity();
    final localId  = '${c.clientId}@${c.password}';     // what others see for us
    final remoteId = '$peerId@$pw';                     // what we pass to JoinConnection
    _server?.send({
      'type': MsgType.bootstrap,
      if (configDir != null) 'config_dir': configDir,
      'local_id': localId,
      'remote_id': remoteId,
      'signal_host': c.signalHost.isEmpty ? 'api.crossdesk.cn' : c.signalHost,
      'signal_port': c.signalPort > 0 ? c.signalPort : 9099,
      'coturn_port': c.coturnPort > 0 ? c.coturnPort : 3478,
      'enable_turn': c.turnEnabled,
      'enable_srtp': c.srtpEnabled,
      'video_quality': c.videoQuality.index,
      'video_codec':   c.videoCodec.index,
      'hardware_acceleration': c.hardwareCodec,
    });
  }

  void _onMessage(Map<String, dynamic> m) {
    final type = m['type'] as String?;
    switch (type) {
      case MsgType.hello:
        _update(_state.copyWith(phase: SessionPhase.helloed, childPid: m['pid'] as int? ?? _state.childPid));
        if (_helloCompleter != null && !_helloCompleter!.isCompleted) {
          _helloCompleter!.complete();
        }
        break;
      case MsgType.state:
        _update(_state.copyWith(phase: SessionPhase.running, remoteState: (m['value'] ?? '').toString()));
        break;
      case MsgType.stats:
        _update(_state.copyWith(stats: SessionStats(
          rttMs: (m['rtt_ms'] as num?)?.toInt() ?? 0,
          bitrateKbps: (m['bitrate_kbps'] as num?)?.toInt() ?? 0,
          fps: (m['fps'] as num?)?.toInt() ?? 0,
          loss: (m['loss'] as num?)?.toDouble() ?? 0.0,
        )));
        break;
      case MsgType.exit:
        _update(_state.copyWith(phase: SessionPhase.exited));
        break;
      // file_progress / clipboard handled later when those features land.
    }
  }

  void applySettings(Map<String, dynamic> settings) {
    _server?.send({'type': MsgType.applySettings, ...settings});
  }

  Future<void> stop({Duration graceful = const Duration(seconds: 2)}) async {
    if (_state.phase == SessionPhase.exited) return;
    _update(_state.copyWith(phase: SessionPhase.closing));
    _server?.send({'type': MsgType.requestClose});
    final child = _child;
    if (child != null) {
      try {
        await child.exitCode.timeout(graceful);
      } on TimeoutException {
        child.kill(ProcessSignal.sigterm);
        try { await child.exitCode.timeout(const Duration(seconds: 2)); }
        on TimeoutException { child.kill(ProcessSignal.sigkill); }
      }
    }
    await _cleanup();
  }

  Future<void> _cleanup() async {
    await _stdoutSub?.cancel(); _stdoutSub = null;
    await _stderrSub?.cancel(); _stderrSub = null;
    await _server?.close(); _server = null;
    _child = null;
  }

  Future<void> dispose() async {
    await stop();
    await _stateCtrl.close();
  }
}
