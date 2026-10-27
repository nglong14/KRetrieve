// Binary frame encoder and decoder
#include "protocol.hpp"

#include <algorithm>
#include <bit>
#include <utility>

namespace kretrieve {
namespace {

// Write a 32-bit number to a raw byte buffer
void put_u32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

void put_u64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint32_t get_u32(const uint8_t* p) {
  return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24);
}

uint64_t get_u64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= uint64_t{p[i]} << (8 * i);
  return v;
}

bool is_known_type(MsgType type) {
  return type == MsgType::QueryRequest || type == MsgType::QueryResponse;
}

}  // namespace

std::vector<uint8_t> encode_frame(const Frame& frame) {
  const size_t len = kFrameMetaSize + frame.body.size();
  std::vector<uint8_t> out(kFramePrefixSize + len);
  put_u32(out.data(), static_cast<uint32_t>(len));
  uint8_t* meta = out.data() + kFramePrefixSize;
  meta[0] = kProtocolVersion;
  meta[1] = static_cast<uint8_t>(frame.type);
  put_u64(meta + 2, frame.req_id);
  std::copy(frame.body.begin(), frame.body.end(), out.begin() + kFramePrefixSize + kFrameMetaSize);
  return out;
}

DecodeResult decode_frame(std::span<const uint8_t> data) {
  if (data.size() < kFramePrefixSize) return {DecodeStatus::NeedMore};

  const uint32_t len = get_u32(data.data());
  if (len < kFrameMetaSize || len > kMaxFrameLen) return {DecodeStatus::Invalid};
  if (data.size() < kFramePrefixSize + len) return {DecodeStatus::NeedMore};

  const uint8_t* meta = data.data() + kFramePrefixSize;
  if (meta[0] != kProtocolVersion) return {DecodeStatus::Invalid};

  Frame frame;
  frame.type = static_cast<MsgType>(meta[1]);
  if (!is_known_type(frame.type)) return {DecodeStatus::Invalid};
  frame.req_id = get_u64(meta + 2);
  frame.body.assign(meta + kFrameMetaSize, meta + len);

  return {DecodeStatus::Ok, std::move(frame), kFramePrefixSize + len};
}

constexpr size_t kRequestHeaderSize = 4 + 8 + 4;
constexpr size_t kResponseHeaderSize = 1 + 4;
constexpr size_t kHitSize = 8 + 8;

std::vector<uint8_t> encode_request(const QueryRequest& request) {
  std::vector<uint8_t> out(kRequestHeaderSize + request.query.size());
  put_u32(out.data(), request.k);
  put_u64(out.data() + 4, request.budget_us);
  put_u32(out.data() + 12, static_cast<uint32_t>(request.query.size()));
  std::copy(request.query.begin(), request.query.end(), out.begin() + kRequestHeaderSize);
  return out;
}

std::optional<QueryRequest> decode_request(std::span<const uint8_t> body) {
  if (body.size() < kRequestHeaderSize) return std::nullopt;
  QueryRequest request;
  request.k = get_u32(body.data());
  request.budget_us = get_u64(body.data() + 4);
  const uint32_t query_len = get_u32(body.data() + 12);
  if (request.k > kMaxK || query_len != body.size() - kRequestHeaderSize) return std::nullopt;
  request.query.assign(body.begin() + kRequestHeaderSize, body.end());
  return request;
}

std::vector<uint8_t> encode_response(const QueryResponse& response) {
  std::vector<uint8_t> out(kResponseHeaderSize + response.hits.size() * kHitSize);
  out[0] = static_cast<uint8_t>(response.status);
  put_u32(out.data() + 1, static_cast<uint32_t>(response.hits.size()));
  uint8_t* p = out.data() + kResponseHeaderSize;
  for (const auto& hit : response.hits) {
    put_u64(p, hit.document_id);
    put_u64(p + 8, std::bit_cast<uint64_t>(hit.score));
    p += kHitSize;
  }
  return out;
}

std::optional<QueryResponse> decode_response(std::span<const uint8_t> body) {
  if (body.size() < kResponseHeaderSize) return std::nullopt;
  if (body[0] > static_cast<uint8_t>(ResponseStatus::InternalError)) return std::nullopt;
  const uint32_t count = get_u32(body.data() + 1);
  if (count > kMaxK || body.size() - kResponseHeaderSize != uint64_t{count} * kHitSize) {
    return std::nullopt;
  }
  QueryResponse response;
  response.status = static_cast<ResponseStatus>(body[0]);
  response.hits.resize(count);
  const uint8_t* p = body.data() + kResponseHeaderSize;
  for (auto& hit : response.hits) {
    hit.document_id = get_u64(p);
    hit.score = std::bit_cast<double>(get_u64(p + 8));
    p += kHitSize;
  }
  return response;
}

}  // namespace kretrieve
