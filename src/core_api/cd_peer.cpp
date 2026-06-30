/*
 * cd_peer_* implementation: thin wrapper around minirtc.
 *
 * Owns the PeerPtr lifetime, marshals minirtc's C callbacks into the
 * cd_peer_* C ABI, and keeps a private cache of the strings stored in
 * minirtc Params (which uses fixed char buffers, so it's fine to fill
 * Params, hand it to CreatePeer, and let it go out of scope — but we
 * also need to keep the *consumer*'s callback pointers alive, hence
 * the wrapper struct).
 */

#include "crossdesk_core.h"

#include <cstring>
#include <mutex>
#include <string>

#include "minirtc.h"

namespace {

struct PeerCallbacks {
    cd_peer_video_cb  video = nullptr;  void* video_user  = nullptr;
    cd_peer_conn_cb   conn  = nullptr;  void* conn_user   = nullptr;
    cd_peer_signal_cb signal = nullptr; void* signal_user = nullptr;
    cd_peer_stats_cb  stats  = nullptr; void* stats_user  = nullptr;
    std::mutex mu;  // protects setter/dispatch races
};

constexpr const char* kVideoStreamPrimary = "primary_display";
constexpr const char* kAudioStream        = "audio";
constexpr const char* kDataStream         = "data";
constexpr const char* kMouseStream        = "mouse";
constexpr const char* kKeyboardStream     = "keyboard";

}  // namespace

struct cd_peer {
    PeerPtr* rtc = nullptr;
    Params   params{};                 // strings pointed to by params live here:
    std::string local_id;
    std::string log_path;
    std::string remote_id;
    PeerCallbacks cb;
    bool connected = false;            // set after JoinConnection
    bool closed = false;
};

static VideoQuality MapQuality(cd_video_quality_t q) {
    switch (q) {
        case CD_VQ_LOW:    return QualityLow;
        case CD_VQ_MEDIUM: return QualityMedium;
        case CD_VQ_HIGH:   return QualityHigh;
    }
    return QualityMedium;
}

static cd_conn_status_t MapConn(ConnectionStatus s) {
    return static_cast<cd_conn_status_t>(s);  // enum values intentionally aligned
}

static cd_signal_status_t MapSignal(SignalStatus s) {
    return static_cast<cd_signal_status_t>(s);
}

/* ----- minirtc → cd_peer callback bridges ----- */

static void RtcVideoFrameCb(const XVideoFrame* f,
                            const char* /*remote_peer_id*/,
                            const size_t /*remote_peer_id_size*/,
                            const char* source_id,
                            const size_t source_id_size,
                            void* user_data) {
    auto* p = static_cast<cd_peer*>(user_data);
    cd_peer_video_cb cb;
    void* cb_user;
    {
        std::lock_guard<std::mutex> lock(p->cb.mu);
        cb = p->cb.video;
        cb_user = p->cb.video_user;
    }
    if (!cb || !f) return;
    cd_peer_video_frame_t out{};
    out.data        = reinterpret_cast<const uint8_t*>(f->data);
    out.size        = f->size;
    out.width       = f->width;
    out.height      = f->height;
    out.captured_us = f->captured_timestamp;
    out.decoded_us  = f->decoded_timestamp;
    std::string sid(source_id ? source_id : "", source_id_size);
    cb(&out, sid.c_str(), cb_user);
}

static void RtcConnStatusCb(ConnectionStatus s,
                            const char* /*rid*/, const size_t /*rid_size*/,
                            void* user_data) {
    auto* p = static_cast<cd_peer*>(user_data);
    cd_peer_conn_cb cb; void* cb_user;
    { std::lock_guard<std::mutex> lk(p->cb.mu); cb = p->cb.conn; cb_user = p->cb.conn_user; }
    if (cb) cb(MapConn(s), cb_user);
}

static void RtcSignalStatusCb(SignalStatus s, const char* /*pid*/,
                              const size_t /*pid_size*/, void* user_data) {
    auto* p = static_cast<cd_peer*>(user_data);
    cd_peer_signal_cb cb; void* cb_user;
    { std::lock_guard<std::mutex> lk(p->cb.mu); cb = p->cb.signal; cb_user = p->cb.signal_user; }
    if (cb) cb(MapSignal(s), cb_user);
}

static cd_net_mode_t MapMode(TraversalMode m) {
    switch (m) {
        case P2P:   return CD_NET_MODE_P2P;
        case Relay: return CD_NET_MODE_RELAY;
        default:    return CD_NET_MODE_UNKNOWN;
    }
}

static void RtcNetStatsCb(const char* /*peer_id*/, const size_t /*peer_id_size*/,
                          TraversalMode mode, const XNetTrafficStats* st,
                          const char* /*rid*/, const size_t /*rid_size*/,
                          void* user_data) {
    auto* p = static_cast<cd_peer*>(user_data);
    cd_peer_stats_cb cb; void* cb_user;
    { std::lock_guard<std::mutex> lk(p->cb.mu); cb = p->cb.stats; cb_user = p->cb.stats_user; }
    if (!cb || !st) return;
    cd_peer_stats_t out{};
    out.video_inbound_kbps  = st->video_inbound_stats.bitrate  / 1000;
    out.video_outbound_kbps = st->video_outbound_stats.bitrate / 1000;
    out.total_inbound_kbps  = st->total_inbound_stats.bitrate  / 1000;
    out.total_outbound_kbps = st->total_outbound_stats.bitrate / 1000;
    out.video_inbound_loss  = st->video_inbound_stats.loss_rate;
    out.mode                = MapMode(mode);
    cb(&out, cb_user);
}

/* ----- Public C ABI ----- */

extern "C" int cd_peer_open(cd_core_t* core,
                            const cd_peer_opts_t* opts,
                            cd_peer_t** out_peer) {
    (void)core;
    if (!opts || !out_peer) return CD_ERR_INVALID_ARG;
    if (!opts->local_id || !opts->remote_id || !opts->signal_host) {
        return CD_ERR_INVALID_ARG;
    }
    auto p = new cd_peer();
    p->local_id = opts->local_id;
    p->remote_id = opts->remote_id;
    p->log_path = opts->log_dir ? opts->log_dir : "";

    Params& pr = p->params;
    pr.use_cfg_file = false;
    std::strncpy(pr.signal_server_ip, opts->signal_host, sizeof(pr.signal_server_ip) - 1);
    pr.signal_server_port = opts->signal_port;
    std::strncpy(pr.stun_server_ip, opts->signal_host, sizeof(pr.stun_server_ip) - 1);
    pr.stun_server_port = opts->coturn_port;
    std::strncpy(pr.turn_server_ip, opts->signal_host, sizeof(pr.turn_server_ip) - 1);
    pr.turn_server_port = opts->coturn_port;
    std::strncpy(pr.turn_server_username, "crossdesk", sizeof(pr.turn_server_username) - 1);
    std::strncpy(pr.turn_server_password, "crossdeskpw", sizeof(pr.turn_server_password) - 1);
    std::strncpy(pr.log_path, p->log_path.c_str(), sizeof(pr.log_path) - 1);
    pr.hardware_acceleration = opts->hardware_acceleration != 0;
    pr.av1_encoding          = (opts->video_codec == CD_CODEC_AV1);
    pr.enable_turn           = opts->enable_turn != 0;
    pr.enable_srtp           = opts->enable_srtp != 0;
    pr.video_quality         = MapQuality(opts->video_quality);

    pr.on_receive_video_buffer = nullptr;
    pr.on_receive_audio_buffer = nullptr;
    pr.on_receive_data_buffer  = nullptr;
    pr.on_receive_video_frame  = RtcVideoFrameCb;
    pr.on_signal_status        = RtcSignalStatusCb;
    pr.on_signal_message       = nullptr;
    pr.on_connection_status    = RtcConnStatusCb;
    pr.on_net_status_report    = RtcNetStatsCb;
    pr.user_id                 = p->local_id.c_str();
    pr.user_data               = p;

    p->rtc = CreatePeer(&pr);
    if (!p->rtc) { delete p; return CD_ERR_INTERNAL; }
    *out_peer = p;
    return CD_OK;
}

extern "C" int cd_peer_set_video_cb(cd_peer_t* p, cd_peer_video_cb cb, void* user) {
    if (!p) return CD_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(p->cb.mu);
    p->cb.video = cb; p->cb.video_user = user;
    return CD_OK;
}

extern "C" int cd_peer_set_conn_cb(cd_peer_t* p, cd_peer_conn_cb cb, void* user) {
    if (!p) return CD_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(p->cb.mu);
    p->cb.conn = cb; p->cb.conn_user = user;
    return CD_OK;
}

extern "C" int cd_peer_set_signal_cb(cd_peer_t* p, cd_peer_signal_cb cb, void* user) {
    if (!p) return CD_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(p->cb.mu);
    p->cb.signal = cb; p->cb.signal_user = user;
    return CD_OK;
}

extern "C" int cd_peer_set_net_stats_cb(cd_peer_t* p, cd_peer_stats_cb cb, void* user) {
    if (!p) return CD_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lk(p->cb.mu);
    p->cb.stats = cb; p->cb.stats_user = user;
    return CD_OK;
}

extern "C" int cd_peer_connect(cd_peer_t* p) {
    if (!p || !p->rtc) return CD_ERR_NOT_INITIALIZED;
    if (p->connected) return CD_OK;
    int rc = Init(p->rtc);
    if (rc != 0) return CD_ERR_INTERNAL;
    rc = JoinConnection(p->rtc, p->remote_id.c_str());
    if (rc != 0) return CD_ERR_INTERNAL;
    AddVideoStream(p->rtc, kVideoStreamPrimary);
    AddAudioStream(p->rtc, kAudioStream);
    AddDataStream (p->rtc, kDataStream,     /*reliable=*/false);
    AddDataStream (p->rtc, kMouseStream,    /*reliable=*/false);
    AddDataStream (p->rtc, kKeyboardStream, /*reliable=*/true);
    p->connected = true;
    return CD_OK;
}

extern "C" int cd_peer_send_action(cd_peer_t* p, const char* action_json) {
    if (!p || !p->rtc) return CD_ERR_NOT_INITIALIZED;
    if (!action_json) return CD_ERR_INVALID_ARG;
    const size_t n = std::strlen(action_json);
    int rc = SendDataFrameToPeer(p->rtc, action_json, n, kMouseStream,
                                 p->remote_id.c_str(), p->remote_id.size());
    return rc == 0 ? CD_OK : CD_ERR_IO;
}

extern "C" int cd_peer_close(cd_peer_t* p) {
    if (!p) return CD_ERR_INVALID_ARG;
    if (p->closed) return CD_OK;
    if (p->rtc && p->connected) {
        LeaveConnection(p->rtc, p->remote_id.c_str());
    }
    p->closed = true;
    return CD_OK;
}

extern "C" void cd_peer_destroy(cd_peer_t* p) {
    if (!p) return;
    if (!p->closed) cd_peer_close(p);
    if (p->rtc) DestroyPeer(&p->rtc);
    delete p;
}
