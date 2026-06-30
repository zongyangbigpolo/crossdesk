// Idiomatic Dart wrapper around CoreBindings.
// One instance per process; constructed lazily by Riverpod provider.

import 'dart:ffi' as ffi;
import 'dart:io' show Platform;

import 'package:ffi/ffi.dart';

import '../ffi/core_bindings.dart';

enum Language { chinese, english, russian }
enum VideoQuality { low, medium, high }
enum VideoFps { fps30, fps60 }
enum VideoCodec { h264, av1 }

class Core {
  Core._(this._handle);

  final ffi.Pointer<CdCore> _handle;
  static final _b = CoreBindings.instance;

  /// Create a core instance. If [configDir] is null, the platform default
  /// (resolved by C++ path_manager) is used. Pass an absolute writable dir
  /// in development.
  factory Core.open({String? configDir}) {
    final ptr = configDir == null
        ? _b.cdCoreCreate(ffi.nullptr)
        : _withCStr(configDir, _b.cdCoreCreate);
    if (ptr == ffi.nullptr) {
      throw StateError('cd_core_create returned null');
    }
    return Core._(ptr);
  }

  void close() => _b.cdCoreDestroy(_handle);

  String get version => _b.cdCoreVersion().toDartString();

  // ------- typed config getters -------
  Language     get language     => Language.values[_b.cfgGetLanguage(_handle)];
  VideoQuality get videoQuality => VideoQuality.values[_b.cfgGetVideoQuality(_handle)];
  VideoFps     get videoFps     => VideoFps.values[_b.cfgGetVideoFps(_handle)];
  VideoCodec   get videoCodec   => VideoCodec.values[_b.cfgGetVideoCodec(_handle)];
  bool         get hardwareCodec      => _b.cfgGetHwCodec(_handle)        != 0;
  bool         get turnEnabled        => _b.cfgGetTurn(_handle)           != 0;
  bool         get srtpEnabled        => _b.cfgGetSrtp(_handle)           != 0;
  bool         get selfHosted         => _b.cfgGetSelfHosted(_handle)     != 0;
  bool         get minimizeToTray     => _b.cfgGetMinimizeToTray(_handle) != 0;
  bool         get autostart          => _b.cfgGetAutostart(_handle)      != 0;
  bool         get daemon             => _b.cfgGetDaemon(_handle)         != 0;
  int          get signalPort         => _b.cfgGetSignalPort(_handle);
  int          get coturnPort         => _b.cfgGetCoturnPort(_handle);
  String       get signalHost         => _readStr(_b.cfgGetSignalHost);
  String       get fileSavePath       => _readStr(_b.cfgGetFileSavePath);

  // ------- setters (return whether persist succeeded) -------
  bool setLanguage(Language v)         => _b.cfgSetLanguage(_handle, v.index) == 0;
  bool setVideoQuality(VideoQuality v) => _b.cfgSetVideoQuality(_handle, v.index) == 0;
  bool setVideoFps(VideoFps v)         => _b.cfgSetVideoFps(_handle, v.index) == 0;
  bool setVideoCodec(VideoCodec v)     => _b.cfgSetVideoCodec(_handle, v.index) == 0;
  bool setHardwareCodec(bool v)        => _b.cfgSetHwCodec(_handle, v ? 1 : 0) == 0;
  bool setTurn(bool v)                 => _b.cfgSetTurn(_handle, v ? 1 : 0) == 0;
  bool setSrtp(bool v)                 => _b.cfgSetSrtp(_handle, v ? 1 : 0) == 0;
  bool setSelfHosted(bool v)           => _b.cfgSetSelfHosted(_handle, v ? 1 : 0) == 0;
  bool setMinimizeToTray(bool v)       => _b.cfgSetMinimizeToTray(_handle, v ? 1 : 0) == 0;
  bool setAutostart(bool v)            => _b.cfgSetAutostart(_handle, v ? 1 : 0) == 0;
  bool setDaemon(bool v)               => _b.cfgSetDaemon(_handle, v ? 1 : 0) == 0;
  bool setSignalPort(int v)            => _b.cfgSetSignalPort(_handle, v) == 0;
  bool setCoturnPort(int v)            => _b.cfgSetCoturnPort(_handle, v) == 0;
  bool setSignalHost(String v)         => _withCStr(v, (p) => _b.cfgSetSignalHost(_handle, p)) == 0;
  bool setFileSavePath(String v)       => _withCStr(v, (p) => _b.cfgSetFileSavePath(_handle, p)) == 0;

  // ------- local identity (random client_id + 6-digit pw, persisted) -------
  /// Generate + persist client_id/password if missing. Idempotent.
  bool ensureIdentity()           => _b.identityEnsure(_handle) == 0;
  String get clientId             => _readStr(_b.identityGetClientId);
  String get password             => _readStr(_b.identityGetPassword);
  bool setClientId(String v)      => _withCStr(v, (p) => _b.identitySetClientId(_handle, p)) == 0;
  bool setPassword(String v)      => _withCStr(v, (p) => _b.identitySetPassword(_handle, p)) == 0;

  // ------- helpers -------

  String _readStr(int Function(ffi.Pointer<CdCore>, ffi.Pointer<ffi.Uint8>, ffi.Pointer<ffi.Size>) fn) {
    final lenP = calloc<ffi.Size>();
    try {
      // 1st call: ask for required length.
      lenP.value = 0;
      fn(_handle, ffi.nullptr, lenP);
      final needed = lenP.value;
      if (needed == 0) return '';
      final buf = calloc<ffi.Uint8>(needed);
      try {
        lenP.value = needed;
        final rc = fn(_handle, buf, lenP);
        if (rc != 0) return '';
        // payload is NUL-terminated UTF-8.
        return buf.cast<Utf8>().toDartString();
      } finally {
        calloc.free(buf);
      }
    } finally {
      calloc.free(lenP);
    }
  }
}

/// Run [body] with a temporary C string allocated from `calloc`.
T _withCStr<T>(String s, T Function(ffi.Pointer<Utf8>) body) {
  final p = s.toNativeUtf8();
  try {
    return body(p);
  } finally {
    calloc.free(p);
  }
}

// Suppress unused-import warning for Platform — kept for future
// platform-specific config-dir defaults.
// ignore_for_file: unused_import
