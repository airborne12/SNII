#include <gtest/gtest.h>

#include "snii/encoding/byte_sink.h"
#include "snii/encoding/byte_source.h"

using namespace snii;

TEST(ByteSource, RoundTripWithSink) {
  ByteSink s;
  s.put_fixed32(0xDEADBEEF);
  s.put_varint64(123456789);
  s.put_zigzag(-42);
  ByteSource src(s.view());
  uint32_t a;
  uint64_t b;
  int64_t c;
  ASSERT_TRUE(src.get_fixed32(&a).ok());
  EXPECT_EQ(a, 0xDEADBEEFu);
  ASSERT_TRUE(src.get_varint64(&b).ok());
  EXPECT_EQ(b, 123456789u);
  ASSERT_TRUE(src.get_zigzag(&c).ok());
  EXPECT_EQ(c, -42);
  EXPECT_TRUE(src.eof());
}

TEST(ByteSource, GetBytesAdvances) {
  ByteSink s;
  const uint8_t p[] = {1, 2, 3, 4, 5};
  s.put_bytes(Slice(p, 5));
  ByteSource src(s.view());
  Slice got;
  ASSERT_TRUE(src.get_bytes(3, &got).ok());
  ASSERT_EQ(got.size(), 3u);
  EXPECT_EQ(got[0], 1u);
  EXPECT_EQ(src.remaining(), 2u);
}

TEST(ByteSource, OverrunFails) {
  uint8_t one[1] = {0x01};
  ByteSource src(Slice(one, 1));
  uint32_t a;
  EXPECT_FALSE(src.get_fixed32(&a).ok());
}
