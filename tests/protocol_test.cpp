#include "protocol.hpp"

#include <gtest/gtest.h>

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
