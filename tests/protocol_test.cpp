#include "protocol.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>

using namespace kretrieve;

TEST(FrameCodec, RoundTripWithBody) {
  Frame in;
  in.type = MsgType::QueryRequest;
  in.req_id = 0x0102030405060708ULL;
  in.body = {'h', 'e', 'l', 'l', 'o', 0x00, 0xff};

  const auto wire = encode_frame(in);
  const auto out = decode_frame(wire);

  ASSERT_EQ(out.status, DecodeStatus::Ok);
  EXPECT_EQ(out.consumed, wire.size());
  EXPECT_EQ(out.frame.type, in.type);
  EXPECT_EQ(out.frame.req_id, in.req_id);
  EXPECT_EQ(out.frame.body, in.body);
}

TEST(FrameCodec, RoundTripEmptyBody) {
  Frame in;
  in.type = MsgType::QueryResponse;
  in.req_id = 42;

  const auto wire = encode_frame(in);
  const auto out = decode_frame(wire);

  ASSERT_EQ(out.status, DecodeStatus::Ok);
  EXPECT_EQ(out.consumed, wire.size());
  EXPECT_EQ(out.frame.type, MsgType::QueryResponse);
  EXPECT_EQ(out.frame.req_id, 42u);
  EXPECT_TRUE(out.frame.body.empty());
}

TEST(FrameCodec, WireFormatIsLittleEndianAndStable) {
  Frame in;
  in.type = MsgType::QueryResponse;
  in.req_id = 1;
  in.body = {0xaa};

  // clang-format off: keep the wire fields on separate rows.
  const std::vector<uint8_t> expected = {
      0x0b, 0x00, 0x00, 0x00,  // len = 10 + 1
      0x01,                    // version
      0x02,                    // type = QueryResponse
      0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // req_id = 1
      0xaa,                    // body
  };
  // clang-format on
  EXPECT_EQ(encode_frame(in), expected);
}

TEST(FrameCodec, DecodesTwoFramesFromOneBuffer) {
  Frame a;
  a.type = MsgType::QueryRequest;
  a.req_id = 7;
  a.body = {1, 2, 3};
  Frame b;
  b.type = MsgType::QueryResponse;
  b.req_id = 8;
  b.body = {4};

  auto wire = encode_frame(a);
  const auto second = encode_frame(b);
  wire.insert(wire.end(), second.begin(), second.end());

  const auto first_out = decode_frame(wire);
  ASSERT_EQ(first_out.status, DecodeStatus::Ok);
  EXPECT_EQ(first_out.consumed, first_out.frame.body.size() + kFramePrefixSize + kFrameMetaSize);
  EXPECT_EQ(first_out.frame.req_id, 7u);

  const auto second_out = decode_frame(std::span<const uint8_t>(wire).subspan(first_out.consumed));
  ASSERT_EQ(second_out.status, DecodeStatus::Ok);
  EXPECT_EQ(second_out.frame.req_id, 8u);
  EXPECT_EQ(second_out.frame.body, std::vector<uint8_t>({4}));
}

TEST(FrameCodec, NeedMoreWhenPrefixOrBodyIsTruncated) {
  Frame in;
  in.type = MsgType::QueryRequest;
  in.req_id = 99;
  in.body = {1, 2, 3, 4, 5};
  const auto wire = encode_frame(in);

  for (size_t len = 0; len < wire.size(); ++len) {
    const auto out = decode_frame(std::span<const uint8_t>(wire).first(len));
    EXPECT_EQ(out.status, DecodeStatus::NeedMore) << "len=" << len;
  }
}

TEST(FrameCodec, RejectsBadHeader) {
  Frame in;
  in.type = MsgType::QueryRequest;
  in.req_id = 1;
  in.body = {0xaa, 0xbb};
  const auto wire = encode_frame(in);

  auto with_version = wire;
  with_version[4] = 0xff;
  EXPECT_EQ(decode_frame(with_version).status, DecodeStatus::Invalid);

  auto with_type = wire;
  with_type[5] = 0x7f;
  EXPECT_EQ(decode_frame(with_type).status, DecodeStatus::Invalid);

  auto too_small = wire;
  too_small[0] = 0x09;  // len must cover at least the 10 meta bytes
  too_small[1] = 0x00;
  too_small[2] = 0x00;
  too_small[3] = 0x00;
  EXPECT_EQ(decode_frame(too_small).status, DecodeStatus::Invalid);

  auto too_large = wire;
  too_large[0] = 0xff;
  too_large[1] = 0xff;
  too_large[2] = 0xff;
  too_large[3] = 0x7f;
  EXPECT_EQ(decode_frame(too_large).status, DecodeStatus::Invalid);
}

TEST(MessageCodec, RequestRoundTripAndWireLayout) {
  const QueryRequest in{10, 0x0102030405060708ULL, "ab"};
  // clang-format off
  const std::vector<uint8_t> expected = {
      0x0a, 0x00, 0x00, 0x00,                          // k = 10
      0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,  // budget_us
      0x02, 0x00, 0x00, 0x00,                          // query_len = 2
      'a', 'b',
  };
  // clang-format on
  const auto wire = encode_request(in);
  EXPECT_EQ(wire, expected);
  const auto out = decode_request(wire);
  ASSERT_TRUE(out);
  EXPECT_EQ(out->k, in.k);
  EXPECT_EQ(out->budget_us, in.budget_us);
  EXPECT_EQ(out->query, in.query);
  EXPECT_TRUE(decode_request(encode_request({0, 0, ""})));
}

TEST(MessageCodec, RejectsMalformedRequests) {
  const auto wire = encode_request({10, 0, "cat"});
  EXPECT_FALSE(decode_request(std::span<const uint8_t>(wire).first(wire.size() - 1)));
  auto trailing = wire;
  trailing.push_back(0);
  EXPECT_FALSE(decode_request(trailing));
  EXPECT_FALSE(decode_request(std::span<const uint8_t>(wire).first(15)));
  EXPECT_FALSE(decode_request(encode_request({kMaxK + 1, 0, "cat"})));
}

TEST(MessageCodec, ResponseRoundTripIsBitExact) {
  const QueryResponse in{ResponseStatus::Ok,
                         {{10, 1.071445295349}, {UINT64_MAX, 4.9e-324}, {0, -0.0}}};
  const auto out = decode_response(encode_response(in));
  ASSERT_TRUE(out);
  EXPECT_EQ(out->status, ResponseStatus::Ok);
  ASSERT_EQ(out->hits.size(), in.hits.size());
  for (size_t i = 0; i < in.hits.size(); ++i) {
    EXPECT_EQ(out->hits[i].document_id, in.hits[i].document_id);
    EXPECT_EQ(std::bit_cast<uint64_t>(out->hits[i].score),
              std::bit_cast<uint64_t>(in.hits[i].score));
  }
  const auto error = decode_response(encode_response({ResponseStatus::BadRequest, {}}));
  ASSERT_TRUE(error);
  EXPECT_EQ(error->status, ResponseStatus::BadRequest);
}

TEST(MessageCodec, RejectsMalformedResponses) {
  const auto wire = encode_response({ResponseStatus::Ok, {{1, 1.0}}});
  EXPECT_FALSE(decode_response(std::span<const uint8_t>(wire).first(4)));
  EXPECT_FALSE(decode_response(std::span<const uint8_t>(wire).first(wire.size() - 1)));
  auto trailing = wire;
  trailing.push_back(0);
  EXPECT_FALSE(decode_response(trailing));
  auto bad_status = wire;
  bad_status[0] = 3;
  EXPECT_FALSE(decode_response(bad_status));
}
