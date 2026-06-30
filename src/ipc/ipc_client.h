/*
 * Minimal blocking IPC client used by crossdesk_session to talk to the
 * Flutter shell. One connection per process. Thread-safe sends; reads
 * are owned by one reader thread.
 *
 * Transport: endpoint is either a Unix-domain-socket path (POSIX) or
 *   "tcp://<host>:<port>" for loopback TCP (used on Windows, where Dart
 *   does not natively support named pipes). The Connect() string is
 *   parsed by ipc_client.cpp; the rest of the class is transport-agnostic.
 */

#ifndef CROSSDESK_IPC_CLIENT_H_
#define CROSSDESK_IPC_CLIENT_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace crossdesk::ipc {

class Client {
 public:
  using MessageHandler = std::function<void(const std::string& json)>;

  Client();
  ~Client();

  // Connect to a server endpoint (UDS path on POSIX). Returns true on
  // success. Does not start the reader thread; call StartReader() after.
  bool Connect(const std::string& endpoint);

  // Start a background thread that reads frames and invokes handler.
  // Handler is called from the reader thread — caller must marshal.
  void StartReader(MessageHandler handler);

  // Send one JSON payload (will be wrapped in a length-prefixed frame).
  // Thread-safe.
  bool Send(const std::string& json);

  void Close();
  bool IsConnected() const { return fd_ >= 0; }

 private:
  void ReaderLoop();

  // Socket handle. On POSIX this is an fd; on Windows it stores a SOCKET
  // (cast through intptr_t so the header stays platform-clean).
  intptr_t fd_ = -1;
  std::mutex write_mu_;
  std::thread reader_;
  std::atomic<bool> stop_{false};
  MessageHandler handler_;
};

}  // namespace crossdesk::ipc

#endif
