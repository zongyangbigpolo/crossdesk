#include "ipc_frame.h"

#include <cstring>

namespace crossdesk::ipc {

std::vector<uint8_t> EncodeFrame(const std::string& payload) {
  if (payload.size() > kMaxFrameBytes) return {};
  std::vector<uint8_t> out(4 + payload.size());
  const uint32_t n = static_cast<uint32_t>(payload.size());
  out[0] = static_cast<uint8_t>((n >> 24) & 0xFF);
  out[1] = static_cast<uint8_t>((n >> 16) & 0xFF);
  out[2] = static_cast<uint8_t>((n >> 8) & 0xFF);
  out[3] = static_cast<uint8_t>(n & 0xFF);
  std::memcpy(out.data() + 4, payload.data(), payload.size());
  return out;
}

DecodeResult TryDecodeFrame(const uint8_t* buf, size_t len) {
  DecodeResult r;
  if (len < 4) return r;
  const uint32_t n = (static_cast<uint32_t>(buf[0]) << 24) |
                     (static_cast<uint32_t>(buf[1]) << 16) |
                     (static_cast<uint32_t>(buf[2]) << 8) |
                     static_cast<uint32_t>(buf[3]);
  if (n > kMaxFrameBytes) {
    r.status = DecodeResult::Status::Oversize;
    return r;
  }
  if (len < 4 + n) return r;
  r.status = DecodeResult::Status::Ok;
  r.payload.assign(reinterpret_cast<const char*>(buf + 4), n);
  r.consumed = 4 + n;
  return r;
}

}  // namespace crossdesk::ipc
