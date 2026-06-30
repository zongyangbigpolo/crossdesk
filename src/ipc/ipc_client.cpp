#include "ipc_client.h"

#include "ipc_frame.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
static int sock_close(socket_t s) { return ::closesocket(s); }
static int sock_errno() { return ::WSAGetLastError(); }
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
static int sock_close(socket_t s) { return ::close(s); }
static int sock_errno() { return errno; }
#endif

namespace crossdesk::ipc {

namespace {

// Parse endpoint: "tcp://host:port" → tcp connect; anything else → UDS path
// (with optional "unix:" prefix stripped).
struct Endpoint {
  bool tcp = false;
  std::string host;  // tcp only
  int  port = 0;     // tcp only
  std::string path;  // unix only
};

Endpoint ParseEndpoint(const std::string& s) {
  Endpoint e;
  constexpr const char* kTcp  = "tcp://";
  constexpr const char* kUnix = "unix:";
  if (s.rfind(kTcp, 0) == 0) {
    e.tcp = true;
    auto rest = s.substr(std::strlen(kTcp));
    auto colon = rest.find_last_of(':');
    if (colon != std::string::npos) {
      e.host = rest.substr(0, colon);
      try { e.port = std::stoi(rest.substr(colon + 1)); } catch (...) { e.port = 0; }
    }
  } else if (s.rfind(kUnix, 0) == 0) {
    e.path = s.substr(std::strlen(kUnix));
  } else {
    e.path = s;
  }
  return e;
}

#ifdef _WIN32
struct WinsockInit {
  WinsockInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
  ~WinsockInit() { WSACleanup(); }
};
static WinsockInit g_winsock_init;
#endif

bool ConnectTcp(socket_t* out_fd, const std::string& host, int port) {
  socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
  if (s == kInvalidSocket) return false;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    sock_close(s); return false;
  }
  if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    sock_close(s); return false;
  }
  *out_fd = s;
  return true;
}

#ifndef _WIN32
bool ConnectUnix(socket_t* out_fd, const std::string& path) {
  socket_t s = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (s < 0) return false;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) { sock_close(s); return false; }
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    sock_close(s); return false;
  }
  *out_fd = s;
  return true;
}
#endif

}  // namespace

Client::Client() = default;
Client::~Client() { Close(); }

bool Client::Connect(const std::string& endpoint) {
  if (fd_ >= 0) return false;
  auto ep = ParseEndpoint(endpoint);
  socket_t s = kInvalidSocket;
  bool ok;
  if (ep.tcp) {
    ok = ConnectTcp(&s, ep.host, ep.port);
  } else {
#ifdef _WIN32
    return false;  // POSIX-only path on Windows; use tcp://
#else
    ok = ConnectUnix(&s, ep.path);
#endif
  }
  if (!ok) return false;
  fd_ = static_cast<intptr_t>(s);
  return true;
}

void Client::StartReader(MessageHandler handler) {
  if (fd_ < 0 || reader_.joinable()) return;
  handler_ = std::move(handler);
  stop_ = false;
  reader_ = std::thread([this] { ReaderLoop(); });
}

bool Client::Send(const std::string& json) {
  if (fd_ < 0) return false;
  auto frame = EncodeFrame(json);
  if (frame.empty()) return false;
  std::lock_guard<std::mutex> lock(write_mu_);
  size_t off = 0;
  socket_t s = static_cast<socket_t>(fd_);
  while (off < frame.size()) {
#ifdef _WIN32
    int n = ::send(s, reinterpret_cast<const char*>(frame.data()) + off,
                   static_cast<int>(frame.size() - off), 0);
    if (n == SOCKET_ERROR) return false;
#else
    ssize_t n = ::send(s, frame.data() + off, frame.size() - off, 0);
    if (n <= 0) {
      if (n < 0 && sock_errno() == EINTR) continue;
      return false;
    }
#endif
    off += static_cast<size_t>(n);
  }
  return true;
}

void Client::Close() {
  stop_ = true;
  if (fd_ >= 0) {
    socket_t s = static_cast<socket_t>(fd_);
#ifdef _WIN32
    ::shutdown(s, SD_BOTH);
#else
    ::shutdown(s, SHUT_RDWR);
#endif
    sock_close(s);
    fd_ = -1;
  }
  if (reader_.joinable()) reader_.join();
}

void Client::ReaderLoop() {
  std::vector<uint8_t> buf;
  buf.reserve(64 * 1024);
  uint8_t tmp[8192];
  socket_t s = static_cast<socket_t>(fd_);
  while (!stop_) {
#ifdef _WIN32
    int n = ::recv(s, reinterpret_cast<char*>(tmp), sizeof(tmp), 0);
    if (n <= 0) break;
#else
    ssize_t n = ::recv(s, tmp, sizeof(tmp), 0);
    if (n <= 0) {
      if (n < 0 && sock_errno() == EINTR) continue;
      break;
    }
#endif
    buf.insert(buf.end(), tmp, tmp + n);
    for (;;) {
      auto r = TryDecodeFrame(buf.data(), buf.size());
      if (r.status == DecodeResult::Status::NeedMore) break;
      if (r.status != DecodeResult::Status::Ok) { stop_ = true; break; }
      buf.erase(buf.begin(), buf.begin() + r.consumed);
      if (handler_) handler_(r.payload);
    }
  }
}

}  // namespace crossdesk::ipc
