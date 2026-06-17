#include <gtest/gtest.h>

#include "snii/encoding/byte_sink.h"
#include "snii/encoding/varint.h"

using namespace snii;

TEST(ByteSink, Fixed32LittleEndian) {
  ByteSink s;
  s.put_fixed32(0x04030201u);
  ASSERT_EQ(s.size(), 4u);
  const auto& b = s.buffer();
  EXPECT_EQ(b[0], 0x01);
  EXPECT_EQ(b[1], 0x02);
  EXPECT_EQ(b[3], 0x04);
}

TEST(ByteSink, Fixed64LittleEndian) {
  ByteSink s;
  s.put_fixed64(0x0807060504030201ull);
  ASSERT_EQ(s.size(), 8u);
  EXPECT_EQ(s.buffer()[0], 0x01);
  EXPECT_EQ(s.buffer()[7], 0x08);
}

TEST(ByteSink, VarintThenBytes) {
  ByteSink s;
  s.put_varint32(300);
  const uint8_t payload[] = {0xAA, 0xBB};
  s.put_bytes(Slice(payload, 2));
  EXPECT_EQ(s.size(), varint_len(300) + 2);
}
