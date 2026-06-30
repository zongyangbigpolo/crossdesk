/*
 * CrossDesk Core C ABI — implementation.
 *
 * Thin shim that wraps existing C++ modules so Flutter (via dart:ffi)
 * and crossdesk_session can share one runtime.
 */

#define CROSSDESK_CORE_BUILD 1
#include "crossdesk_core.h"
#include "crossdesk_core_internal.h"

#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>

#include "config_center.h"
#include "device_presence.h"
#include "nlohmann/json.hpp"

#ifndef CROSSDESK_CORE_VERSION
#define CROSSDESK_CORE_VERSION "0.1.0"
#endif

namespace {

struct CoreImpl {
  std::unique_ptr<crossdesk::ConfigCenter> config;
  DevicePresence presence;

  std::mutex presence_cb_mu;
  cd_presence_cb presence_cb = nullptr;
  void* presence_cb_user = nullptr;

  // Local-identity state (persisted in <config_dir>/identity.json).
  std::string config_dir;
  std::mutex identity_mu;
  std::string client_id;
  std::string password;
  bool identity_loaded = false;
};

inline CoreImpl* impl(cd_core_t* c) { return reinterpret_cast<CoreImpl*>(c); }

/* Helper for "fill buf or report required length" string returns. */
int copy_string_out(const std::string& src, char* buf, size_t* len) {
  if (!len) return CD_ERR_INVALID_ARG;
  const size_t needed = src.size() + 1;
  if (!buf) {
    *len = needed;
    return CD_OK;
  }
  if (*len < needed) {
    *len = needed;
    return CD_ERR_BUFFER_TOO_SMALL;
  }
  std::memcpy(buf, src.c_str(), needed);
  *len = needed;
  return CD_OK;
}

}  // namespace

/* ----- Lifecycle ------------------------------------------------------ */

extern "C" cd_core_t* cd_core_create(const char* config_dir) {
  auto* c = new (std::nothrow) CoreImpl();
  if (!c) return nullptr;
  std::string path = "config.ini";
  if (config_dir && *config_dir) {
    path = std::string(config_dir) + "/config.ini";
  }
  c->config = std::make_unique<crossdesk::ConfigCenter>(path);
  c->config->Load();
  c->config_dir = (config_dir && *config_dir) ? config_dir : ".";
  return reinterpret_cast<cd_core_t*>(c);
}

namespace {
std::string IdentityPath(const std::string& dir) {
  return dir + "/identity.json";
}

void LoadIdentity_NoLock(CoreImpl* c) {
  if (c->identity_loaded) return;
  c->identity_loaded = true;
  std::ifstream f(IdentityPath(c->config_dir));
  if (!f.good()) return;
  try {
    nlohmann::json j; f >> j;
    c->client_id = j.value("client_id", std::string());
    c->password  = j.value("password",  std::string());
  } catch (...) { /* corrupt file → treat as missing */ }
}

bool SaveIdentity_NoLock(CoreImpl* c) {
  nlohmann::json j = {{"client_id", c->client_id}, {"password", c->password}};
  std::ofstream f(IdentityPath(c->config_dir), std::ios::trunc);
  if (!f.good()) return false;
  f << j.dump();
  return f.good();
}

std::string RandomDigits(size_t n) {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<int> d(0, 9);
  std::string s; s.reserve(n);
  for (size_t i = 0; i < n; ++i) s.push_back('0' + d(rng));
  return s;
}
}  // namespace

extern "C" void cd_core_destroy(cd_core_t* core) {
  if (!core) return;
  delete impl(core);
}

extern "C" const char* cd_core_version(void) {
  return CROSSDESK_CORE_VERSION;
}

/* ----- Config getters ------------------------------------------------- */

#define REQUIRE_CORE(c, ret) do { if (!(c)) return (ret); } while (0)

extern "C" cd_language_t cd_cfg_get_language(cd_core_t* c) {
  REQUIRE_CORE(c, CD_LANG_CHINESE);
  return static_cast<cd_language_t>(impl(c)->config->GetLanguage());
}
extern "C" cd_video_quality_t cd_cfg_get_video_quality(cd_core_t* c) {
  REQUIRE_CORE(c, CD_VQ_HIGH);
  return static_cast<cd_video_quality_t>(impl(c)->config->GetVideoQuality());
}
extern "C" cd_video_frame_rate_t cd_cfg_get_video_fps(cd_core_t* c) {
  REQUIRE_CORE(c, CD_FPS_60);
  return static_cast<cd_video_frame_rate_t>(impl(c)->config->GetVideoFrameRate());
}
extern "C" cd_video_codec_t cd_cfg_get_video_codec(cd_core_t* c) {
  REQUIRE_CORE(c, CD_CODEC_H264);
  return static_cast<cd_video_codec_t>(impl(c)->config->GetVideoEncodeFormat());
}
extern "C" int cd_cfg_get_hw_codec(cd_core_t* c) {
  REQUIRE_CORE(c, 0);
  return impl(c)->config->IsHardwareVideoCodec() ? 1 : 0;
}
extern "C" int cd_cfg_get_turn(cd_core_t* c) {
  REQUIRE_CORE(c, 0);
  return impl(c)->config->IsEnableTurn() ? 1 : 0;
}
extern "C" int cd_cfg_get_srtp(cd_core_t* c) {
  REQUIRE_CORE(c, 0);
  return impl(c)->config->IsEnableSrtp() ? 1 : 0;
}
extern "C" int cd_cfg_get_self_hosted(cd_core_t* c) {
  REQUIRE_CORE(c, 0);
  return impl(c)->config->IsSelfHosted() ? 1 : 0;
}
extern "C" int cd_cfg_get_minimize_to_tray(cd_core_t* c) {
  REQUIRE_CORE(c, 0);
  return impl(c)->config->IsMinimizeToTray() ? 1 : 0;
}
extern "C" int cd_cfg_get_autostart(cd_core_t* c) {
  REQUIRE_CORE(c, 0);
  return impl(c)->config->IsEnableAutostart() ? 1 : 0;
}
extern "C" int cd_cfg_get_daemon(cd_core_t* c) {
  REQUIRE_CORE(c, 0);
  return impl(c)->config->IsEnableDaemon() ? 1 : 0;
}
extern "C" int cd_cfg_get_signal_port(cd_core_t* c) {
  REQUIRE_CORE(c, 0);
  return impl(c)->config->GetSignalServerPort();
}
extern "C" int cd_cfg_get_coturn_port(cd_core_t* c) {
  REQUIRE_CORE(c, 0);
  return impl(c)->config->GetCoturnServerPort();
}
extern "C" int cd_cfg_get_signal_host(cd_core_t* c, char* buf, size_t* len) {
  if (!c) return CD_ERR_NOT_INITIALIZED;
  return copy_string_out(impl(c)->config->GetSignalServerHost(), buf, len);
}
extern "C" int cd_cfg_get_file_save_path(cd_core_t* c, char* buf, size_t* len) {
  if (!c) return CD_ERR_NOT_INITIALIZED;
  return copy_string_out(impl(c)->config->GetFileTransferSavePath(), buf, len);
}

/* ----- Config setters ------------------------------------------------- */

#define WRAP_SET(call) do { \
  if (!c) return CD_ERR_NOT_INITIALIZED; \
  return impl(c)->config->call == 0 ? CD_OK : CD_ERR_IO; \
} while (0)

extern "C" int cd_cfg_set_language(cd_core_t* c, cd_language_t v) {
  WRAP_SET(SetLanguage(static_cast<crossdesk::ConfigCenter::LANGUAGE>(v)));
}
extern "C" int cd_cfg_set_video_quality(cd_core_t* c, cd_video_quality_t v) {
  WRAP_SET(SetVideoQuality(static_cast<crossdesk::ConfigCenter::VIDEO_QUALITY>(v)));
}
extern "C" int cd_cfg_set_video_fps(cd_core_t* c, cd_video_frame_rate_t v) {
  WRAP_SET(SetVideoFrameRate(static_cast<crossdesk::ConfigCenter::VIDEO_FRAME_RATE>(v)));
}
extern "C" int cd_cfg_set_video_codec(cd_core_t* c, cd_video_codec_t v) {
  WRAP_SET(SetVideoEncodeFormat(static_cast<crossdesk::ConfigCenter::VIDEO_ENCODE_FORMAT>(v)));
}
extern "C" int cd_cfg_set_hw_codec(cd_core_t* c, int v)         { WRAP_SET(SetHardwareVideoCodec(v != 0)); }
extern "C" int cd_cfg_set_turn(cd_core_t* c, int v)             { WRAP_SET(SetTurn(v != 0)); }
extern "C" int cd_cfg_set_srtp(cd_core_t* c, int v)             { WRAP_SET(SetSrtp(v != 0)); }
extern "C" int cd_cfg_set_self_hosted(cd_core_t* c, int v)      { WRAP_SET(SetSelfHosted(v != 0)); }
extern "C" int cd_cfg_set_minimize_to_tray(cd_core_t* c, int v) { WRAP_SET(SetMinimizeToTray(v != 0)); }
extern "C" int cd_cfg_set_autostart(cd_core_t* c, int v)        { WRAP_SET(SetAutostart(v != 0)); }
extern "C" int cd_cfg_set_daemon(cd_core_t* c, int v)           { WRAP_SET(SetDaemon(v != 0)); }
extern "C" int cd_cfg_set_signal_host(cd_core_t* c, const char* v) {
  if (!v) return CD_ERR_INVALID_ARG;
  WRAP_SET(SetServerHost(v));
}
extern "C" int cd_cfg_set_signal_port(cd_core_t* c, int v) { WRAP_SET(SetServerPort(v)); }
extern "C" int cd_cfg_set_coturn_port(cd_core_t* c, int v) { WRAP_SET(SetCoturnServerPort(v)); }
extern "C" int cd_cfg_set_file_save_path(cd_core_t* c, const char* v) {
  if (!v) return CD_ERR_INVALID_ARG;
  WRAP_SET(SetFileTransferSavePath(v));
}

/* ----- Local identity ------------------------------------------------- */

extern "C" int cd_identity_ensure(cd_core_t* c) {
  if (!c) return CD_ERR_NOT_INITIALIZED;
  auto* p = impl(c);
  std::lock_guard<std::mutex> lk(p->identity_mu);
  LoadIdentity_NoLock(p);
  bool dirty = false;
  if (p->client_id.empty()) { p->client_id = RandomDigits(10); dirty = true; }
  if (p->password.empty())  { p->password  = RandomDigits(6);  dirty = true; }
  if (dirty && !SaveIdentity_NoLock(p)) return CD_ERR_IO;
  return CD_OK;
}

extern "C" int cd_identity_get_client_id(cd_core_t* c, char* buf, size_t* len) {
  if (!c) return CD_ERR_NOT_INITIALIZED;
  auto* p = impl(c);
  std::lock_guard<std::mutex> lk(p->identity_mu);
  LoadIdentity_NoLock(p);
  return copy_string_out(p->client_id, buf, len);
}

extern "C" int cd_identity_get_password(cd_core_t* c, char* buf, size_t* len) {
  if (!c) return CD_ERR_NOT_INITIALIZED;
  auto* p = impl(c);
  std::lock_guard<std::mutex> lk(p->identity_mu);
  LoadIdentity_NoLock(p);
  return copy_string_out(p->password, buf, len);
}

extern "C" int cd_identity_set_client_id(cd_core_t* c, const char* id) {
  if (!c || !id) return CD_ERR_INVALID_ARG;
  auto* p = impl(c);
  std::lock_guard<std::mutex> lk(p->identity_mu);
  LoadIdentity_NoLock(p);
  p->client_id = id;
  return SaveIdentity_NoLock(p) ? CD_OK : CD_ERR_IO;
}

extern "C" int cd_identity_set_password(cd_core_t* c, const char* pw) {
  if (!c || !pw) return CD_ERR_INVALID_ARG;
  auto* p = impl(c);
  std::lock_guard<std::mutex> lk(p->identity_mu);
  LoadIdentity_NoLock(p);
  p->password = pw;
  return SaveIdentity_NoLock(p) ? CD_OK : CD_ERR_IO;
}

/* ----- Presence ------------------------------------------------------- */

extern "C" int cd_presence_set(cd_core_t* c, const char* id, int online) {
  if (!c || !id) return CD_ERR_INVALID_ARG;
  impl(c)->presence.SetOnline(id, online != 0);
  cd_presence_cb cb = nullptr;
  void* user = nullptr;
  {
    std::lock_guard<std::mutex> lock(impl(c)->presence_cb_mu);
    cb = impl(c)->presence_cb;
    user = impl(c)->presence_cb_user;
  }
  if (cb) cb(id, online, user);
  return CD_OK;
}

extern "C" int cd_presence_is_online(cd_core_t* c, const char* id) {
  if (!c || !id) return 0;
  return impl(c)->presence.IsOnline(id) ? 1 : 0;
}

extern "C" int cd_presence_clear(cd_core_t* c) {
  if (!c) return CD_ERR_NOT_INITIALIZED;
  impl(c)->presence.Clear();
  return CD_OK;
}

extern "C" int cd_presence_subscribe(cd_core_t* c, cd_presence_cb cb, void* user) {
  if (!c) return CD_ERR_NOT_INITIALIZED;
  std::lock_guard<std::mutex> lock(impl(c)->presence_cb_mu);
  impl(c)->presence_cb = cb;
  impl(c)->presence_cb_user = user;
  return CD_OK;
}

/* ----- Internal C++ accessors (in-tree only) -------------------------- */

extern "C" crossdesk::ConfigCenter* cd_internal_get_config_center(cd_core_t* c) {
  return c ? impl(c)->config.get() : nullptr;
}

extern "C" DevicePresence* cd_internal_get_device_presence(cd_core_t* c) {
  return c ? &impl(c)->presence : nullptr;
}
