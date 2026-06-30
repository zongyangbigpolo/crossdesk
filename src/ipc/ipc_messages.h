/*
 * Shell ↔ Session IPC message type registry.
 *
 * Every JSON payload MUST contain at least:
 *   { "type": "<one of the constants below>", ... }
 *
 * Adding new types: append a constant; never repurpose an existing one.
 */

#ifndef CROSSDESK_IPC_MESSAGES_H_
#define CROSSDESK_IPC_MESSAGES_H_

namespace crossdesk::ipc::msg {

// shell → session
constexpr const char* kBootstrap     = "bootstrap";      // initial blob + peer_id, password, settings snapshot
constexpr const char* kApplySettings = "apply_settings"; // runtime setting changes (quality/fps/codec)
constexpr const char* kRequestClose  = "request_close";  // user closed window in Flutter; graceful shutdown
constexpr const char* kSendFile      = "send_file";      // forward a file the user dropped on Flutter

// session → shell
constexpr const char* kHello         = "hello";          // session announces itself + pid
constexpr const char* kState         = "state";          // connecting / connected / failed / closed
constexpr const char* kStats         = "stats";          // rtt_ms, bitrate, fps, packet_loss
constexpr const char* kFileProgress  = "file_progress";  // token, sent, total, status
constexpr const char* kClipboard     = "clipboard";      // remote clipboard pushed to local
constexpr const char* kExit          = "exit";           // last gasp before process exit

}  // namespace crossdesk::ipc::msg

#endif
