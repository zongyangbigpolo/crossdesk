/*
 * CrossDesk Core C ABI
 *
 * Stable C interface to the CrossDesk runtime, consumed by:
 *   - crossdesk_shell  (Flutter Desktop via dart:ffi)
 *   - crossdesk_session (native session window)
 *   - crossdesk        (legacy ImGui binary, during migration)
 *
 * ABI rules:
 *   - Never break an existing function signature. Add new functions instead.
 *   - All strings are UTF-8 C strings.
 *   - All returned int functions: 0 == success, negative == error code.
 *   - Output buffers: caller passes buf + len; if buf is NULL the call
 *     returns the required length via *len (without writing). If buf is
 *     non-NULL, *len is updated to the bytes actually written (incl. NUL).
 */

#ifndef CROSSDESK_CORE_H_
#define CROSSDESK_CORE_H_

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(CROSSDESK_CORE_BUILD)
#    define CD_API __declspec(dllexport)
#  else
#    define CD_API __declspec(dllimport)
#  endif
#else
#  define CD_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ----- Error codes ---------------------------------------------------- */
#define CD_OK                  0
#define CD_ERR_INVALID_ARG    -1
#define CD_ERR_NOT_INITIALIZED -2
#define CD_ERR_BUFFER_TOO_SMALL -3
#define CD_ERR_NOT_FOUND      -4
#define CD_ERR_IO             -5
#define CD_ERR_INTERNAL       -100

/* ----- Lifecycle ------------------------------------------------------ */
typedef struct cd_core cd_core_t;

/* Create a core instance. `config_dir` may be NULL — in that case the
 * platform default (resolved by path_manager) is used. */
CD_API cd_core_t* cd_core_create(const char* config_dir);
CD_API void       cd_core_destroy(cd_core_t* core);

/* Returns library version string, e.g. "1.0.0". */
CD_API const char* cd_core_version(void);

/* ----- Config (typed accessors, mirroring ConfigCenter) --------------- */
/* Enum values must stay in sync with crossdesk::ConfigCenter::*. */
typedef enum {
  CD_LANG_CHINESE = 0,
  CD_LANG_ENGLISH = 1,
  CD_LANG_RUSSIAN = 2,
} cd_language_t;

typedef enum {
  CD_VQ_LOW    = 0,
  CD_VQ_MEDIUM = 1,
  CD_VQ_HIGH   = 2,
} cd_video_quality_t;

typedef enum {
  CD_FPS_30 = 0,
  CD_FPS_60 = 1,
} cd_video_frame_rate_t;

typedef enum {
  CD_CODEC_H264 = 0,
  CD_CODEC_AV1  = 1,
} cd_video_codec_t;

/* getters */
CD_API cd_language_t          cd_cfg_get_language(cd_core_t*);
CD_API cd_video_quality_t     cd_cfg_get_video_quality(cd_core_t*);
CD_API cd_video_frame_rate_t  cd_cfg_get_video_fps(cd_core_t*);
CD_API cd_video_codec_t       cd_cfg_get_video_codec(cd_core_t*);
CD_API int                    cd_cfg_get_hw_codec(cd_core_t*);
CD_API int                    cd_cfg_get_turn(cd_core_t*);
CD_API int                    cd_cfg_get_srtp(cd_core_t*);
CD_API int                    cd_cfg_get_self_hosted(cd_core_t*);
CD_API int                    cd_cfg_get_minimize_to_tray(cd_core_t*);
CD_API int                    cd_cfg_get_autostart(cd_core_t*);
CD_API int                    cd_cfg_get_daemon(cd_core_t*);
CD_API int                    cd_cfg_get_signal_port(cd_core_t*);
CD_API int                    cd_cfg_get_coturn_port(cd_core_t*);
/* String getters: see "Output buffers" rule above. */
CD_API int cd_cfg_get_signal_host(cd_core_t*, char* buf, size_t* len);
CD_API int cd_cfg_get_file_save_path(cd_core_t*, char* buf, size_t* len);

/* setters (persist immediately via ConfigCenter::Save) */
CD_API int cd_cfg_set_language(cd_core_t*, cd_language_t);
CD_API int cd_cfg_set_video_quality(cd_core_t*, cd_video_quality_t);
CD_API int cd_cfg_set_video_fps(cd_core_t*, cd_video_frame_rate_t);
CD_API int cd_cfg_set_video_codec(cd_core_t*, cd_video_codec_t);
CD_API int cd_cfg_set_hw_codec(cd_core_t*, int);
CD_API int cd_cfg_set_turn(cd_core_t*, int);
CD_API int cd_cfg_set_srtp(cd_core_t*, int);
CD_API int cd_cfg_set_self_hosted(cd_core_t*, int);
CD_API int cd_cfg_set_minimize_to_tray(cd_core_t*, int);
CD_API int cd_cfg_set_autostart(cd_core_t*, int);
CD_API int cd_cfg_set_daemon(cd_core_t*, int);
CD_API int cd_cfg_set_signal_host(cd_core_t*, const char*);
CD_API int cd_cfg_set_signal_port(cd_core_t*, int);
CD_API int cd_cfg_set_coturn_port(cd_core_t*, int);
CD_API int cd_cfg_set_file_save_path(cd_core_t*, const char*);

/* ----- Local identity ------------------------------------------------- *
 *
 * Identity persists in <config_dir>/identity.json as:
 *   { "client_id": "1234567890", "password": "654321" }
 *
 * client_id is a 10-digit numeric string assigned eventually by the signal
 * server. Until the signal-server client is wired in, cd_identity_ensure
 * generates a random one so multiple SessionManagers in the same install
 * do not collide on local_id. Once the signal server lands, it will
 * overwrite via cd_identity_set_client_id when a real id is assigned.
 *
 * Output buffer rule (same as cd_cfg_get_*): pass NULL buf to query size. */
CD_API int cd_identity_ensure(cd_core_t*);
CD_API int cd_identity_get_client_id(cd_core_t*, char* buf, size_t* len);
CD_API int cd_identity_get_password (cd_core_t*, char* buf, size_t* len);
CD_API int cd_identity_set_client_id(cd_core_t*, const char* id);
CD_API int cd_identity_set_password (cd_core_t*, const char* pw);

/* ----- Presence ------------------------------------------------------- */
CD_API int cd_presence_set(cd_core_t*, const char* device_id, int online);
CD_API int cd_presence_is_online(cd_core_t*, const char* device_id);
CD_API int cd_presence_clear(cd_core_t*);

/* Subscribe to presence changes. Callback fires on the thread that called
 * cd_presence_set — caller MUST marshal to its UI thread. Pass NULL cb to
 * unsubscribe. `user` is passed back verbatim. */
typedef void (*cd_presence_cb)(const char* device_id, int online, void* user);
CD_API int cd_presence_subscribe(cd_core_t*, cd_presence_cb cb, void* user);

/* ===== Peer (per-session WebRTC connection) =========================== *
 *
 * One cd_peer_t per remote connection. Lifecycle:
 *
 *   cd_peer_open(core, opts, &peer)
 *     -> registers callbacks via cd_peer_set_*_cb
 *     -> cd_peer_connect(peer)           // join transmission
 *     ... callbacks fire ...
 *     -> cd_peer_send_action(peer, json) // mouse / key
 *     -> cd_peer_close(peer)             // graceful leave
 *
 * Callback threads: minirtc invokes them on internal worker threads.
 * Consumers MUST marshal to their UI/render thread.
 *
 * NV12 video frame layout (cd_peer_video_frame_t.data, size = w*h*3/2):
 *   [ Y plane: w*h bytes ][ interleaved UV plane: w*h/2 bytes ]
 * ====================================================================== */

typedef struct cd_peer cd_peer_t;

typedef struct {
    /* Identity */
    const char* local_id;            /* this device's id+password (17 chars typically) */
    const char* remote_id;           /* peer to connect to */

    /* Signaling */
    const char* signal_host;         /* IP or hostname */
    int         signal_port;
    int         coturn_port;
    int         enable_turn;
    int         enable_srtp;

    /* Codec */
    cd_video_quality_t video_quality;
    cd_video_codec_t   video_codec;
    int                hardware_acceleration;

    /* Logging */
    const char* log_dir;             /* where minirtc dumps logs */
} cd_peer_opts_t;

/* Frame handed to the video callback. Buffer is owned by the callee for
 * the duration of the call only — copy if you need it longer. */
typedef struct {
    const uint8_t* data;
    size_t         size;
    uint32_t       width;
    uint32_t       height;
    uint64_t       captured_us;
    uint64_t       decoded_us;
} cd_peer_video_frame_t;

/* Mirror of minirtc ConnectionStatus / SignalStatus for ABI stability. */
typedef enum {
    CD_CONN_CONNECTING        = 0,
    CD_CONN_CONNECTED         = 1,
    CD_CONN_GATHERING         = 2,
    CD_CONN_DISCONNECTED      = 3,
    CD_CONN_FAILED            = 4,
    CD_CONN_CLOSED            = 5,
    CD_CONN_INCORRECT_PWD     = 6,
    CD_CONN_NO_SUCH_TX        = 7,
} cd_conn_status_t;

typedef enum {
    CD_SIGNAL_CONNECTING   = 0,
    CD_SIGNAL_CONNECTED    = 1,
    CD_SIGNAL_FAILED       = 2,
    CD_SIGNAL_CLOSED       = 3,
    CD_SIGNAL_RECONNECTING = 4,
    CD_SIGNAL_SERVER_CLOSED = 5,
} cd_signal_status_t;

typedef void (*cd_peer_video_cb)(const cd_peer_video_frame_t* frame,
                                 const char* source_id, void* user);
typedef void (*cd_peer_conn_cb )(cd_conn_status_t status, void* user);
typedef void (*cd_peer_signal_cb)(cd_signal_status_t status, void* user);

/* Traversal mode mirrors minirtc TraversalMode. */
typedef enum {
    CD_NET_MODE_P2P     = 0,
    CD_NET_MODE_RELAY   = 1,
    CD_NET_MODE_UNKNOWN = 2,
} cd_net_mode_t;

/* Periodic net-stats snapshot delivered via cd_peer_set_net_stats_cb.
 * Bitrate values are kbps (bits/sec / 1000). loss is 0.0..1.0. */
typedef struct {
    uint32_t video_inbound_kbps;
    uint32_t video_outbound_kbps;
    uint32_t total_inbound_kbps;
    uint32_t total_outbound_kbps;
    float    video_inbound_loss;
    cd_net_mode_t mode;
} cd_peer_stats_t;

typedef void (*cd_peer_stats_cb)(const cd_peer_stats_t* stats, void* user);

/* Construct a peer. On success writes the handle to *out_peer. The opts
 * struct is copied; caller may free it after the call returns. */
CD_API int  cd_peer_open(cd_core_t* core,
                         const cd_peer_opts_t* opts,
                         cd_peer_t** out_peer);

/* Register callbacks. May be called before or after cd_peer_connect, but
 * BEFORE connect to avoid missing early frames. NULL cb unsubscribes. */
CD_API int  cd_peer_set_video_cb (cd_peer_t*, cd_peer_video_cb, void* user);
CD_API int  cd_peer_set_conn_cb  (cd_peer_t*, cd_peer_conn_cb,  void* user);
CD_API int  cd_peer_set_signal_cb(cd_peer_t*, cd_peer_signal_cb,void* user);
CD_API int  cd_peer_set_net_stats_cb(cd_peer_t*, cd_peer_stats_cb, void* user);

/* Join the transmission (calls minirtc Init + JoinConnection + AddStreams). */
CD_API int  cd_peer_connect(cd_peer_t*);

/* Send a JSON-encoded RemoteAction (mouse/key) on the "mouse" data stream. */
CD_API int  cd_peer_send_action(cd_peer_t*, const char* action_json);

/* Graceful close: leave connection, but keep the handle valid for inspection. */
CD_API int  cd_peer_close(cd_peer_t*);

/* Free the peer handle. Calls cd_peer_close if not already closed. */
CD_API void cd_peer_destroy(cd_peer_t*);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* CROSSDESK_CORE_H_ */
