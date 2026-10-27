// Public definition of the TCP wire-frame format
#pragma once

#include "index.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
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

// QueryRequest body: [u32 k][u64 budget_us][u32 query_len][query bytes]
// QueryResponse body: [u8 status][u32 hit_count] then hit_count x [u64 doc_id][f64 score]
// Decoders return nullopt for truncated, oversized, or trailing-byte bodies.
constexpr uint32_t kMaxK = 100'000;  // keeps a full response far below kMaxBodySize

enum class ResponseStatus : uint8_t {
  Ok = 0,
  BadRequest = 1,
  InternalError = 2,
};

struct QueryRequest {
  uint32_t k = 0;
  uint64_t budget_us = 0;  // reserved for deadlines (day 4); ignored by the shard
  std::string query;
};

struct QueryResponse {
  ResponseStatus status = ResponseStatus::Ok;
  std::vector<SearchHit> hits;
};

std::vector<uint8_t> encode_request(const QueryRequest& request);
std::optional<QueryRequest> decode_request(std::span<const uint8_t> body);
std::vector<uint8_t> encode_response(const QueryResponse& response);
std::optional<QueryResponse> decode_response(std::span<const uint8_t> body);

}  // namespace kretrieve
