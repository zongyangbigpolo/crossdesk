/*
 * SessionApp — owner of the per-connection session window process.
 *
 * Responsibilities:
 *   - parse command-line (--ipc <endpoint>, --peer-id <id> for standalone debug)
 *   - open an SDL3 window
 *   - connect to shell IPC, send hello, await bootstrap
 *   - on bootstrap: cd_peer_open + cd_peer_connect, render incoming NV12 video
 *   - run an SDL event loop until window close OR request_close arrives
 *
 * Future work:
 *   - mouse / keyboard SDL → cd_peer_send_action (M2.2f)
 *   - audio sink, file transfer, clipboard (M5)
 */

#ifndef CROSSDESK_SESSION_APP_H_
#define CROSSDESK_SESSION_APP_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "crossdesk_core.h"
#include "ipc_client.h"

struct SDL_Renderer;
struct SDL_Window;
struct SDL_Texture;

namespace crossdesk::session {

struct Options {
  std::string ipc_endpoint;   // empty == standalone debug mode
  std::string peer_id;        // debug only
  bool debug = false;
};

bool ParseArgs(int argc, char** argv, Options* out);

class SessionApp {
 public:
  explicit SessionApp(Options opts);
  ~SessionApp();

  int Run();

 private:
  bool InitWindow();
  void Shutdown();
  void OnIpcMessage(const std::string& json);
  void HandleBootstrap(const std::string& json);
  void RenderFrame();                 // called every tick from main loop
  void DestroyPeer();

  // Peer video callback (invoked on minirtc thread). Copies into video_buf_.
  static void OnVideoFrame(const cd_peer_video_frame_t* f,
                           const char* source_id, void* user);
  static void OnConn  (cd_conn_status_t s, void* user);
  static void OnSignal(cd_signal_status_t s, void* user);
  static void OnNetStats(const cd_peer_stats_t* s, void* user);

  Options opts_;
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* video_tex_ = nullptr;
  int tex_w_ = 0, tex_h_ = 0;
  ipc::Client ipc_;
  std::atomic<bool> close_requested_{false};

  cd_core_t* core_ = nullptr;
  cd_peer_t* peer_ = nullptr;

  std::mutex frame_mu_;
  std::vector<uint8_t> frame_pending_;  // NV12 plane (Y then UV interleaved)
  uint32_t frame_w_ = 0, frame_h_ = 0;
  bool frame_dirty_ = false;
};

}  // namespace crossdesk::session

#endif
