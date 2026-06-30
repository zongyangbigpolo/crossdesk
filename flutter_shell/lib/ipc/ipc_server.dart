// Per-session IPC server.
//
// Listens on either a Unix-domain-socket path (POSIX) or a loopback TCP
// port (Windows — Dart does not natively support named pipes), accepts
// ONE inbound connection (the session process spawned by SessionManager),
// then frames messages in/out using [encodeFrame]/[tryDecodeFrame].
// Extra connections are rejected — there is only ever one session per
// server.
//
// Endpoint scheme:
//   - "tcp://127.0.0.1:<port>"  → TCP loopback (used on Windows)
//   - "unix:/abs/path" or bare "/abs/path" → Unix domain socket (POSIX)
//
// On TCP, the port is allocated by the OS when [endpointHint] omits one
// (port 0); the resolved endpoint is exposed via [endpoint].

import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'ipc_frame.dart';

typedef MessageHandler = void Function(Map<String, dynamic> message);

class IpcServer {
  IpcServer._(this.endpoint, this._server);

  final String endpoint;
  final ServerSocket _server;

  Socket? _peer;
  final _buf = BytesBuilder(copy: false);
  StreamSubscription<Uint8List>? _peerSub;
  final _connectedCompleter = Completer<void>();
  final _closedCompleter = Completer<void>();
  MessageHandler? _onMessage;
  void Function(Object error)? _onError;
  void Function()? _onPeerClosed;

  Future<void> get connected => _connectedCompleter.future;
  Future<void> get closed => _closedCompleter.future;
  bool get hasPeer => _peer != null;

  /// Bind to [endpointHint]. For TCP, pass "tcp://127.0.0.1:0" to let the
  /// OS pick a port; the resolved "tcp://host:port" is exposed as [endpoint].
  static Future<IpcServer> bind(String endpointHint) async {
    if (endpointHint.startsWith('tcp://')) {
      final rest = endpointHint.substring('tcp://'.length);
      final colon = rest.lastIndexOf(':');
      final host = colon < 0 ? rest : rest.substring(0, colon);
      final port = colon < 0 ? 0 : int.tryParse(rest.substring(colon + 1)) ?? 0;
      final server = await ServerSocket.bind(
          InternetAddress(host, type: InternetAddressType.IPv4), port);
      final resolved = 'tcp://${server.address.address}:${server.port}';
      return IpcServer._(resolved, server);
    }
    // Unix domain socket: accept "unix:/path" or bare "/path".
    final path = endpointHint.startsWith('unix:')
        ? endpointHint.substring('unix:'.length)
        : endpointHint;
    final f = File(path);
    if (f.existsSync()) f.deleteSync();
    final addr = InternetAddress(path, type: InternetAddressType.unix);
    final server = await ServerSocket.bind(addr, 0);
    return IpcServer._(path, server);
  }

  bool get _isTcp => endpoint.startsWith('tcp://');

  void listen({
    required MessageHandler onMessage,
    void Function(Object error)? onError,
    void Function()? onPeerClosed,
  }) {
    _onMessage = onMessage;
    _onError = onError;
    _onPeerClosed = onPeerClosed;
    _server.listen(_handleAccept, onError: (e) => _onError?.call(e));
  }

  void _handleAccept(Socket s) {
    if (_peer != null) {
      // Second connection attempt — reject. Only one session per server.
      s.destroy();
      return;
    }
    _peer = s;
    _peerSub = s.listen(
      _onBytes,
      onError: (e) => _onError?.call(e),
      onDone: _handlePeerDone,
      cancelOnError: false,
    );
    if (!_connectedCompleter.isCompleted) _connectedCompleter.complete();
  }

  void _onBytes(Uint8List chunk) {
    _buf.add(chunk);
    // Decode as many frames as possible.
    while (true) {
      final view = _buf.toBytes();
      final r = tryDecodeFrame(view);
      if (r.status == DecodeStatus.needMore) break;
      if (r.status == DecodeStatus.oversize || r.status == DecodeStatus.malformed) {
        _onError?.call(StateError('IPC decode failed: ${r.status}'));
        _peer?.destroy();
        return;
      }
      // Ok: pop consumed bytes and dispatch.
      final remaining = view.sublist(r.consumed);
      _buf
        ..clear()
        ..add(remaining);
      _onMessage?.call(r.payload!);
    }
  }

  void _handlePeerDone() {
    _peerSub = null;
    _peer = null;
    _onPeerClosed?.call();
  }

  bool send(Map<String, dynamic> message) {
    final p = _peer;
    if (p == null) return false;
    p.add(encodeFrame(message));
    return true;
  }

  Future<void> close() async {
    await _peerSub?.cancel();
    _peer?.destroy();
    _peer = null;
    await _server.close();
    if (!_isTcp) {
      final f = File(endpoint);
      if (f.existsSync()) {
        try { f.deleteSync(); } catch (_) {}
      }
    }
    if (!_closedCompleter.isCompleted) _closedCompleter.complete();
  }
}
