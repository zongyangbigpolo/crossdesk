// ignore_for_file: library_private_types_in_public_api
// Private typedef names are intentional — they're FFI implementation
// detail not meant to be implemented or extended by consumers.

// Hand-written FFI bindings for libcrossdesk_core C ABI.
// Mirrors src/core_api/crossdesk_core.h. Keep in sync.
//
// Lookup is one-off at module load — the DynamicLibrary stays open for
// the process lifetime, which matches our single-core-per-process model.

import 'dart:ffi' as ffi;
import 'dart:io' show File, Platform;

import 'package:ffi/ffi.dart';

// Opaque core handle.
final class CdCore extends ffi.Opaque {}

// Native function signatures (top = native ABI, bottom = Dart-callable).
typedef _CreateNative = ffi.Pointer<CdCore> Function(ffi.Pointer<Utf8>);
typedef _CreateDart   = ffi.Pointer<CdCore> Function(ffi.Pointer<Utf8>);

typedef _DestroyNative = ffi.Void Function(ffi.Pointer<CdCore>);
typedef _DestroyDart   = void     Function(ffi.Pointer<CdCore>);

typedef _VersionNative = ffi.Pointer<Utf8> Function();
typedef _VersionDart   = ffi.Pointer<Utf8> Function();

typedef _GetIntNative = ffi.Int32 Function(ffi.Pointer<CdCore>);
typedef _GetIntDart   = int       Function(ffi.Pointer<CdCore>);

typedef _SetIntNative = ffi.Int32 Function(ffi.Pointer<CdCore>, ffi.Int32);
typedef _SetIntDart   = int       Function(ffi.Pointer<CdCore>, int);

typedef _GetStrNative = ffi.Int32 Function(
    ffi.Pointer<CdCore>, ffi.Pointer<ffi.Uint8>, ffi.Pointer<ffi.Size>);
typedef _GetStrDart   = int Function(
    ffi.Pointer<CdCore>, ffi.Pointer<ffi.Uint8>, ffi.Pointer<ffi.Size>);

typedef _SetStrNative = ffi.Int32 Function(ffi.Pointer<CdCore>, ffi.Pointer<Utf8>);
typedef _SetStrDart   = int       Function(ffi.Pointer<CdCore>, ffi.Pointer<Utf8>);

/// One-time initialized binding table to libcrossdesk_core.
class CoreBindings {
  CoreBindings._(this._lib);

  static CoreBindings? _instance;
  static CoreBindings get instance => _instance ??= CoreBindings._(_open());

  final ffi.DynamicLibrary _lib;

  static ffi.DynamicLibrary _open() {
    // 1. Allow override for dev (point at xmake build output).
    final override = Platform.environment['CROSSDESK_CORE_LIB'];
    if (override != null && File(override).existsSync()) {
      return ffi.DynamicLibrary.open(override);
    }
    // 2. Adjacent to the executable in shipped builds.
    final name = Platform.isWindows
        ? 'crossdesk_core.dll'
        : (Platform.isMacOS ? 'libcrossdesk_core.dylib' : 'libcrossdesk_core.so');
    return ffi.DynamicLibrary.open(name);
  }

  late final _CreateDart  cdCoreCreate  = _lib.lookupFunction<_CreateNative,  _CreateDart>('cd_core_create');
  late final _DestroyDart cdCoreDestroy = _lib.lookupFunction<_DestroyNative, _DestroyDart>('cd_core_destroy');
  late final _VersionDart cdCoreVersion = _lib.lookupFunction<_VersionNative, _VersionDart>('cd_core_version');

  // Typed-int getters (enum-valued ones return int; caller maps to enum).
  late final _GetIntDart cfgGetLanguage          = _bindGet('cd_cfg_get_language');
  late final _GetIntDart cfgGetVideoQuality      = _bindGet('cd_cfg_get_video_quality');
  late final _GetIntDart cfgGetVideoFps          = _bindGet('cd_cfg_get_video_fps');
  late final _GetIntDart cfgGetVideoCodec        = _bindGet('cd_cfg_get_video_codec');
  late final _GetIntDart cfgGetHwCodec           = _bindGet('cd_cfg_get_hw_codec');
  late final _GetIntDart cfgGetTurn              = _bindGet('cd_cfg_get_turn');
  late final _GetIntDart cfgGetSrtp              = _bindGet('cd_cfg_get_srtp');
  late final _GetIntDart cfgGetSelfHosted        = _bindGet('cd_cfg_get_self_hosted');
  late final _GetIntDart cfgGetMinimizeToTray    = _bindGet('cd_cfg_get_minimize_to_tray');
  late final _GetIntDart cfgGetAutostart         = _bindGet('cd_cfg_get_autostart');
  late final _GetIntDart cfgGetDaemon            = _bindGet('cd_cfg_get_daemon');
  late final _GetIntDart cfgGetSignalPort        = _bindGet('cd_cfg_get_signal_port');
  late final _GetIntDart cfgGetCoturnPort        = _bindGet('cd_cfg_get_coturn_port');

  late final _SetIntDart cfgSetLanguage          = _bindSet('cd_cfg_set_language');
  late final _SetIntDart cfgSetVideoQuality      = _bindSet('cd_cfg_set_video_quality');
  late final _SetIntDart cfgSetVideoFps          = _bindSet('cd_cfg_set_video_fps');
  late final _SetIntDart cfgSetVideoCodec        = _bindSet('cd_cfg_set_video_codec');
  late final _SetIntDart cfgSetHwCodec           = _bindSet('cd_cfg_set_hw_codec');
  late final _SetIntDart cfgSetTurn              = _bindSet('cd_cfg_set_turn');
  late final _SetIntDart cfgSetSrtp              = _bindSet('cd_cfg_set_srtp');
  late final _SetIntDart cfgSetSelfHosted        = _bindSet('cd_cfg_set_self_hosted');
  late final _SetIntDart cfgSetMinimizeToTray    = _bindSet('cd_cfg_set_minimize_to_tray');
  late final _SetIntDart cfgSetAutostart         = _bindSet('cd_cfg_set_autostart');
  late final _SetIntDart cfgSetDaemon            = _bindSet('cd_cfg_set_daemon');
  late final _SetIntDart cfgSetSignalPort        = _bindSet('cd_cfg_set_signal_port');
  late final _SetIntDart cfgSetCoturnPort        = _bindSet('cd_cfg_set_coturn_port');

  late final _GetStrDart cfgGetSignalHost        = _lib.lookupFunction<_GetStrNative, _GetStrDart>('cd_cfg_get_signal_host');
  late final _GetStrDart cfgGetFileSavePath      = _lib.lookupFunction<_GetStrNative, _GetStrDart>('cd_cfg_get_file_save_path');
  late final _SetStrDart cfgSetSignalHost        = _lib.lookupFunction<_SetStrNative, _SetStrDart>('cd_cfg_set_signal_host');
  late final _SetStrDart cfgSetFileSavePath      = _lib.lookupFunction<_SetStrNative, _SetStrDart>('cd_cfg_set_file_save_path');

  // Local identity (cd_identity_*).
  late final int Function(ffi.Pointer<CdCore>) identityEnsure =
      _lib.lookupFunction<ffi.Int32 Function(ffi.Pointer<CdCore>),
                          int Function(ffi.Pointer<CdCore>)>('cd_identity_ensure');
  late final _GetStrDart identityGetClientId =
      _lib.lookupFunction<_GetStrNative, _GetStrDart>('cd_identity_get_client_id');
  late final _GetStrDart identityGetPassword =
      _lib.lookupFunction<_GetStrNative, _GetStrDart>('cd_identity_get_password');
  late final _SetStrDart identitySetClientId =
      _lib.lookupFunction<_SetStrNative, _SetStrDart>('cd_identity_set_client_id');
  late final _SetStrDart identitySetPassword =
      _lib.lookupFunction<_SetStrNative, _SetStrDart>('cd_identity_set_password');

  _GetIntDart _bindGet(String s) => _lib.lookupFunction<_GetIntNative, _GetIntDart>(s);
  _SetIntDart _bindSet(String s) => _lib.lookupFunction<_SetIntNative, _SetIntDart>(s);
}
