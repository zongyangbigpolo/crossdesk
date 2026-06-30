/*
 * Shell ↔ Session IPC framing.
 *
 * Wire format: [4-byte big-endian uint32 length][UTF-8 JSON payload]
 * Length excludes the header. Max payload size enforced by caller.
 */

#ifndef CROSSDESK_IPC_FRAME_H_
#define CROSSDESK_IPC_FRAME_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace crossdesk::ipc {

constexpr uint32_t kMaxFrameBytes = 16 * 1024 * 1024;  // 16 MiB hard cap

// Encode payload into a heap buffer prefixed by length. Returns empty on
// oversize input.
std::vector<uint8_t> EncodeFrame(const std::string& json_payload);

// Result of a single Decode call.
struct DecodeResult {
  enum class Status { NeedMore, Ok, Oversize, Malformed };
  Status status = Status::NeedMore;
  std::string payload;  // valid only when status == Ok
  size_t consumed = 0;  // bytes consumed from input buffer
};

// Try to decode one frame from the front of `buf`. Caller is responsible
// for erasing `consumed` bytes from the front of its accumulator.
DecodeResult TryDecodeFrame(const uint8_t* buf, size_t len);

}  // namespace crossdesk::ipc

#endif
