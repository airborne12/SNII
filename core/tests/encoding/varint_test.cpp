#include <gtest/gtest.h>

#include <cstdint>

#include "snii/encoding/varint.h"

using namespace snii;

TEST(Varint, RoundTrip32) {
  for (uint32_t v : {0u, 1u, 127u, 128u, 300u, 16384u, 0xFFFFFFFFu}) {
    uint8_t buf[10];
    size_t n = encode_varint32(v, buf);
    EXPECT_EQ(n, varint_len(v));
    uint32_t out;
    const uint8_t* next;
    ASSERT_TRUE(decode_varint32(buf, buf + n, &out, &next).ok());
    EXPECT_EQ(out, v);
    EXPECT_EQ(next, buf + n);
  }
}

TEST(Varint, RoundTrip64) {
  for (uint64_t v : {0ull, 1ull, 127ull, 128ull, 1ull << 35, 0xFFFFFFFFFFFFFFFFull}) {
    uint8_t buf[10];
    size_t n = encode_varint64(v, buf);
    uint64_t out;
    const uint8_t* next;
    ASSERT_TRUE(decode_varint64(buf, buf + n, &out, &next).ok());
    EXPECT_EQ(out, v);
  }
}

TEST(Varint, TruncatedFails) {
  uint8_t buf[1] = {0x80};  // continuation bit set but no subsequent byte
  uint32_t out;
  const uint8_t* next;
  EXPECT_FALSE(decode_varint32(buf, buf + 1, &out, &next).ok());
}

TEST(Varint, Overflow32Fails) {
  // encode a value > 2^32-1, decoding with decode_varint32 should fail
  uint8_t buf[10];
  size_t n = encode_varint64((1ull << 33), buf);
  uint32_t out;
  const uint8_t* next;
  EXPECT_FALSE(decode_varint32(buf, buf + n, &out, &next).ok());
}

TEST(Varint, ZigzagRoundTrip) {
  const int64_t cases[] = {0, -1, 1, -1000, 1000, INT64_MIN, INT64_MAX};
  for (int64_t v : cases) {
    EXPECT_EQ(zigzag_decode(zigzag_encode(v)), v);
  }
}
