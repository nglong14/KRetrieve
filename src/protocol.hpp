// Public definition of the TCP wire-frame format
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace kretrieve {
//`[u32 length][u8 version][u8 message type][u64 request ID][body]`

constexpr uint8_t kProtocolVersion = 1;
constexpr size_t kFramePrefixSize = 4;                // 4-byte length field
constexpr size_t kFrameMetaSize = 10;                 // version + type + requestID
constexpr uint32_t kMaxBodySize = 16u * 1024 * 1024;  // 16 MiB
constexpr uint32_t kMaxFrameLen = kFrameMetaSize + kMaxBodySize;

enum class MsgType : uint8_t {
  QueryRequest = 1,
  QueryResponse = 2,
};

struct Frame {
  MsgType type{};
  uint64_t req_id = 0;
  std::vector<uint8_t> body;
};

std::vector<uint8_t> encode_frame(const Frame& frame);

enum class DecodeStatus {
  Ok,
  NeedMore,
  Invalid,
};

struct DecodeResult {
  DecodeStatus status = DecodeStatus::NeedMore;
  Frame frame;
  size_t consumed = 0;
};

DecodeResult decode_frame(std::span<const uint8_t> data);

}  // namespace kretrieve
