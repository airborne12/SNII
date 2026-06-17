#include <gtest/gtest.h>

#include <vector>

#include "snii/encoding/crc32c.h"

using namespace snii;

// leveldb/RocksDB standard CRC32C(Castagnoli) test vectors.
TEST(Crc32c, KnownVectors) {
  std::vector<uint8_t> zeros(32, 0x00);
  EXPECT_EQ(crc32c(Slice(zeros)), 0x8a9136aau);
  std::vector<uint8_t> ff(32, 0xff);
  EXPECT_EQ(crc32c(Slice(ff)), 0x62a8ab43u);
  std::vector<uint8_t> ramp(32);
  for (int i = 0; i < 32; ++i) ramp[i] = static_cast<uint8_t>(i);
  EXPECT_EQ(crc32c(Slice(ramp)), 0x46dd794eu);
}

TEST(Crc32c, ExtendEqualsContiguous) {
  std::vector<uint8_t> v{1, 2, 3, 4, 5, 6, 7, 8};
  uint32_t whole = crc32c(Slice(v));
  uint32_t part = crc32c(Slice(v.data(), 4));
  part = crc32c_extend(part, Slice(v.data() + 4, 4));
  EXPECT_EQ(whole, part);
}
