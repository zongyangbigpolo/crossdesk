#include "session_app.h"

#include <SDL3/SDL.h>
#include <unistd.h>

#include <cmath>
#include <cstring>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

#include "ipc_messages.h"

namespace crossdesk::session {

namespace {
constexpr int kDefaultWidth  = 1280;
constexpr int kDefaultHeight = 720;

// Mirrors src/device_controller/device_controller.h enums. Keep in sync.
enum ControlType { kCtrlMouse = 0, kCtrlKeyboard = 1 };
enum MouseFlag   { kMove = 0, kLeftDown, kLeftUp, kRightDown, kRightUp,
                   kMiddleDown, kMiddleUp, kWheelV, kWheelH };
enum KeyFlag     { kKeyDown = 0, kKeyUp };

// Normalize cursor x,y to [0,1] relative to window size. Returns false if
// the window has no usable size yet.
bool NormalizeCursor(SDL_Window* win, float in_x, float in_y,
                     float* out_x, float* out_y) {
  int w = 0, h = 0;
  SDL_GetWindowSize(win, &w, &h);
  if (w <= 0 || h <= 0) return false;
  float nx = in_x / static_cast<float>(w);
  float ny = in_y / static_cast<float>(h);
  if (nx < 0.0f) nx = 0.0f; if (nx > 1.0f) nx = 1.0f;
  if (ny < 0.0f) ny = 0.0f; if (ny > 1.0f) ny = 1.0f;
  *out_x = nx; *out_y = ny;
  return true;
}

// Build a mouse RemoteAction JSON. Returns empty string for unhandled events.
std::string MouseActionJson(SDL_Window* win, const SDL_Event& ev) {
  float x = 0.0f, y = 0.0f;
  int s = 0;
  int flag = -1;
  switch (ev.type) {
    case SDL_EVENT_MOUSE_MOTION:
      if (!NormalizeCursor(win, ev.motion.x, ev.motion.y, &x, &y)) return {};
      flag = kMove;
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      const bool down = (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
      if (!NormalizeCursor(win, ev.button.x, ev.button.y, &x, &y)) return {};
      switch (ev.button.button) {
        case SDL_BUTTON_LEFT:   flag = down ? kLeftDown   : kLeftUp;   break;
        case SDL_BUTTON_RIGHT:  flag = down ? kRightDown  : kRightUp;  break;
        case SDL_BUTTON_MIDDLE: flag = down ? kMiddleDown : kMiddleUp; break;
        default: return {};
      }
      break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
      // Wheel events lack cursor coords; use the last known mouse pos.
      float mx = 0.0f, my = 0.0f;
      SDL_GetMouseState(&mx, &my);
      if (!NormalizeCursor(win, mx, my, &x, &y)) return {};
      const float sx = ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -ev.wheel.x : ev.wheel.x;
      const float sy = ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -ev.wheel.y : ev.wheel.y;
      if (std::abs(sy) >= std::abs(sx)) { flag = kWheelV; s = (int)(sy > 0 ? std::ceil(sy) : std::floor(sy)); }
      else                              { flag = kWheelH; s = (int)(sx > 0 ? std::ceil(sx) : std::floor(sx)); }
      break;
    }
    default: return {};
  }
  if (flag < 0) return {};
  nlohmann::json j = {
      {"type", kCtrlMouse},
      {"mouse", {{"x", x}, {"y", y}, {"s", s}, {"flag", flag}}},
  };
  return j.dump();
}

std::string KeyActionJson(const SDL_Event& ev) {
  const bool down = (ev.type == SDL_EVENT_KEY_DOWN);
  if (!down && ev.type != SDL_EVENT_KEY_UP) return {};
  // key_value uses SDL3 scancode (platform-portable). scan_code is the raw
  // hardware scancode (same thing in SDL3). Receiver maps SDL scancode → VK.
  nlohmann::json j = {
      {"type", kCtrlKeyboard},
      {"keyboard", {
          {"key_value", (size_t)ev.key.scancode},
          {"scan_code", (uint32_t)ev.key.raw},
          {"extended", false},
          {"flag", down ? kKeyDown : kKeyUp},
      }},
  };
  return j.dump();
}
}  // namespace

bool ParseArgs(int argc, char** argv, Options* out) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](std::string* dst) -> bool {
      if (i + 1 >= argc) return false;
      *dst = argv[++i];
      return true;
    };
    if (a == "--ipc") { if (!next(&out->ipc_endpoint)) return false; }
    else if (a == "--peer-id") { if (!next(&out->peer_id)) return false; }
    else if (a == "--debug") { out->debug = true; }
    else if (a == "-h" || a == "--help") {
      std::cout << "crossdesk_session [--ipc <endpoint>] [--peer-id <id>] [--debug]\n";
      return false;
    } else {
      std::cerr << "unknown arg: " << a << "\n";
      return false;
    }
  }
  return true;
}

SessionApp::SessionApp(Options opts) : opts_(std::move(opts)) {}

SessionApp::~SessionApp() { Shutdown(); }

bool SessionApp::InitWindow() {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
    return false;
  }
  if (!SDL_CreateWindowAndRenderer(
          "CrossDesk Session", kDefaultWidth, kDefaultHeight,
          SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE,
          &window_, &renderer_)) {
    std::cerr << "SDL_CreateWindowAndRenderer failed: " << SDL_GetError() << "\n";
    return false;
  }
  SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
  return true;
}

void SessionApp::DestroyPeer() {
  if (peer_) { cd_peer_destroy(peer_); peer_ = nullptr; }
  if (video_tex_) { SDL_DestroyTexture(video_tex_); video_tex_ = nullptr; tex_w_ = tex_h_ = 0; }
}

void SessionApp::Shutdown() {
  DestroyPeer();
  if (core_)     { cd_core_destroy(core_);          core_ = nullptr; }
  ipc_.Close();
  if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
  if (window_)   { SDL_DestroyWindow(window_);     window_ = nullptr; }
  SDL_Quit();
}

void SessionApp::OnVideoFrame(const cd_peer_video_frame_t* f,
                              const char* /*source_id*/, void* user) {
  auto* self = static_cast<SessionApp*>(user);
  if (!f || !f->data || f->size == 0) return;
  std::lock_guard<std::mutex> lock(self->frame_mu_);
  self->frame_pending_.assign(f->data, f->data + f->size);
  self->frame_w_ = f->width;
  self->frame_h_ = f->height;
  self->frame_dirty_ = true;
}

void SessionApp::OnConn(cd_conn_status_t s, void* user) {
  auto* self = static_cast<SessionApp*>(user);
  static const char* names[] = {
      "connecting", "connected", "gathering", "disconnected",
      "failed", "closed", "incorrect_password", "no_such_tx",
  };
  const int idx = static_cast<int>(s);
  const char* name = (idx >= 0 && idx < (int)(sizeof(names)/sizeof(*names))) ? names[idx] : "?";
  std::cout << "[session] conn -> " << name << "\n";
  if (self->ipc_.IsConnected()) {
    nlohmann::json m = {{"type", ipc::msg::kState}, {"value", name}};
    self->ipc_.Send(m.dump());
  }
}

void SessionApp::OnSignal(cd_signal_status_t s, void* user) {
  auto* self = static_cast<SessionApp*>(user);
  static const char* names[] = {
      "signal_connecting", "signal_connected", "signal_failed",
      "signal_closed", "signal_reconnecting", "signal_server_closed",
  };
  const int idx = static_cast<int>(s);
  const char* name = (idx >= 0 && idx < (int)(sizeof(names)/sizeof(*names))) ? names[idx] : "?";
  std::cout << "[session] signal -> " << name << "\n";
  if (self->ipc_.IsConnected()) {
    nlohmann::json m = {{"type", ipc::msg::kState}, {"value", name}};
    self->ipc_.Send(m.dump());
  }
}

void SessionApp::OnNetStats(const cd_peer_stats_t* s, void* user) {
  auto* self = static_cast<SessionApp*>(user);
  if (!s || !self->ipc_.IsConnected()) return;
  nlohmann::json m = {
      {"type",         ipc::msg::kStats},
      {"rtt_ms",       0},  // minirtc does not expose RTT yet
      {"bitrate_kbps", s->total_inbound_kbps},
      {"fps",          0},  // computed shell-side from frame cadence in a later milestone
      {"loss",         s->video_inbound_loss},
      {"mode",         static_cast<int>(s->mode)},
  };
  self->ipc_.Send(m.dump());
}

void SessionApp::HandleBootstrap(const std::string& json) {
  // Bootstrap payload schema:
  //   {
  //     "type": "bootstrap",
  //     "config_dir": "...",          // for cd_core
  //     "local_id":   "abcdef1234567",
  //     "remote_id":  "abcdef",
  //     "signal_host": "host", "signal_port": 9099, "coturn_port": 3478,
  //     "enable_turn": true, "enable_srtp": false,
  //     "video_quality": 1, "video_codec": 0, "hardware_acceleration": true,
  //     "log_dir": "/tmp/cd-session-logs"
  //   }
  try {
    auto j = nlohmann::json::parse(json);
    if (!core_) {
      const std::string cfg = j.value("config_dir", std::string());
      core_ = cd_core_create(cfg.empty() ? nullptr : cfg.c_str());
      if (!core_) {
        std::cerr << "[session] cd_core_create failed\n";
        return;
      }
    }
    if (peer_) {
      std::cerr << "[session] bootstrap ignored: peer already open\n";
      return;
    }
    cd_peer_opts_t o{};
    std::string local_id  = j.value("local_id",  std::string());
    std::string remote_id = j.value("remote_id", std::string());
    std::string sigh      = j.value("signal_host", std::string("api.crossdesk.cn"));
    std::string logdir    = j.value("log_dir", std::string());
    if (logdir.empty()) {
      // minirtc crashes if log_dir is empty — it tries to open "/<file>".
      // Default to system tmp dir.
      const char* tmp = std::getenv("TMPDIR");
      if (!tmp) tmp = "/tmp";
      logdir = tmp;
    }
    o.local_id    = local_id.c_str();
    o.remote_id   = remote_id.c_str();
    o.signal_host = sigh.c_str();
    o.signal_port = j.value("signal_port", 9099);
    o.coturn_port = j.value("coturn_port", 3478);
    o.enable_turn = j.value("enable_turn", true) ? 1 : 0;
    o.enable_srtp = j.value("enable_srtp", false) ? 1 : 0;
    o.video_quality = static_cast<cd_video_quality_t>(j.value("video_quality", (int)CD_VQ_MEDIUM));
    o.video_codec   = static_cast<cd_video_codec_t>  (j.value("video_codec",   (int)CD_CODEC_H264));
    o.hardware_acceleration = j.value("hardware_acceleration", true) ? 1 : 0;
    o.log_dir = logdir.c_str();

    int rc = cd_peer_open(core_, &o, &peer_);
    if (rc != CD_OK) {
      std::cerr << "[session] cd_peer_open failed: " << rc << "\n";
      return;
    }
    cd_peer_set_video_cb    (peer_, &SessionApp::OnVideoFrame, this);
    cd_peer_set_conn_cb     (peer_, &SessionApp::OnConn,        this);
    cd_peer_set_signal_cb   (peer_, &SessionApp::OnSignal,      this);
    cd_peer_set_net_stats_cb(peer_, &SessionApp::OnNetStats,    this);
    rc = cd_peer_connect(peer_);
    if (rc != CD_OK) {
      std::cerr << "[session] cd_peer_connect failed: " << rc << "\n";
      DestroyPeer();
      return;
    }
    std::cout << "[session] peer connecting: local=" << local_id
              << " remote=" << remote_id << "\n";
  } catch (const std::exception& e) {
    std::cerr << "[session] bootstrap parse failed: " << e.what() << "\n";
  }
}

void SessionApp::OnIpcMessage(const std::string& json) {
  try {
    auto j = nlohmann::json::parse(json);
    const std::string type = j.value("type", "");
    if (type == ipc::msg::kRequestClose) {
      close_requested_ = true;
    } else if (type == ipc::msg::kBootstrap) {
      HandleBootstrap(json);
    } else if (type == ipc::msg::kApplySettings) {
      // TODO: forward to peer (bitrate / fps / codec) once cd_peer exposes setters.
    } else {
      std::cout << "[session] unhandled ipc type: " << type << "\n";
    }
  } catch (const std::exception& e) {
    std::cerr << "[session] bad ipc json: " << e.what() << "\n";
  }
}

void SessionApp::RenderFrame() {
  SDL_SetRenderDrawColor(renderer_, 18, 18, 22, 255);
  SDL_RenderClear(renderer_);

  // Hand off a pending NV12 frame to the texture, if any.
  bool have_frame = false;
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> local;
  {
    std::lock_guard<std::mutex> lock(frame_mu_);
    if (frame_dirty_ && !frame_pending_.empty()) {
      local = std::move(frame_pending_);
      frame_pending_.clear();
      w = frame_w_; h = frame_h_;
      frame_dirty_ = false;
      have_frame = true;
    }
  }
  if (have_frame) {
    if (!video_tex_ || (int)w != tex_w_ || (int)h != tex_h_) {
      if (video_tex_) SDL_DestroyTexture(video_tex_);
      video_tex_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_NV12,
                                     SDL_TEXTUREACCESS_STREAMING,
                                     (int)w, (int)h);
      tex_w_ = (int)w; tex_h_ = (int)h;
    }
    if (video_tex_) {
      // NV12: Y plane (w*h) then UV plane (w*h/2). SDL_UpdateNVTexture wants
      // separate Y/UV pointers and pitches.
      const uint8_t* y_plane  = local.data();
      const uint8_t* uv_plane = local.data() + (size_t)w * h;
      SDL_UpdateNVTexture(video_tex_, /*rect=*/nullptr,
                          y_plane,  (int)w,
                          uv_plane, (int)w);
    }
  }
  if (video_tex_) {
    SDL_RenderTexture(renderer_, video_tex_, /*srcrect=*/nullptr, /*dstrect=*/nullptr);
  }
  SDL_RenderPresent(renderer_);
}

int SessionApp::Run() {
  if (!InitWindow()) return 1;

  if (!opts_.ipc_endpoint.empty()) {
    if (!ipc_.Connect(opts_.ipc_endpoint)) {
      std::cerr << "[session] ipc connect failed: " << opts_.ipc_endpoint << "\n";
      return 2;
    }
    ipc_.StartReader([this](const std::string& j) { OnIpcMessage(j); });
    nlohmann::json hello = {
        {"type", ipc::msg::kHello},
        {"pid", static_cast<long>(::getpid())},
        {"peer_id", opts_.peer_id},
    };
    ipc_.Send(hello.dump());
  } else {
    std::cout << "[session] standalone debug mode (no --ipc)\n";
  }

  SDL_Event ev;
  bool running = true;
  while (running && !close_requested_) {
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_EVENT_QUIT) { running = false; break; }
      if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) { running = false; break; }
      if (!peer_) continue;  // no peer yet — drop input
      std::string j;
      switch (ev.type) {
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL:
          j = MouseActionJson(window_, ev);
          break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
          j = KeyActionJson(ev);
          break;
        default: break;
      }
      if (!j.empty()) cd_peer_send_action(peer_, j.c_str());
    }
    RenderFrame();
    SDL_Delay(16);
  }

  if (ipc_.IsConnected()) {
    nlohmann::json bye = {{"type", ipc::msg::kExit}, {"code", 0}};
    ipc_.Send(bye.dump());
  }
  return 0;
}

}  // namespace crossdesk::session
