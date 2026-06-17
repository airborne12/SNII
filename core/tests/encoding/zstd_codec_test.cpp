#include <gtest/gtest.h>

#include <vector>

#include "snii/encoding/zstd_codec.h"

using namespace snii;

TEST(Zstd, RoundTrip) {
  std::vector<uint8_t> in;
  for (int i = 0; i < 10000; ++i) in.push_back(static_cast<uint8_t>(i % 7));
  std::vector<uint8_t> comp, decomp;
  ASSERT_TRUE(zstd_compress(Slice(in), 3, &comp).ok());
  EXPECT_LT(comp.size(), in.size());
  ASSERT_TRUE(zstd_decompress(Slice(comp), in.size(), &decomp).ok());
  EXPECT_EQ(decomp, in);
}

TEST(Zstd, WrongLenFails) {
  std::vector<uint8_t> in(100, 7), comp, decomp;
  ASSERT_TRUE(zstd_compress(Slice(in), 3, &comp).ok());
  EXPECT_FALSE(zstd_decompress(Slice(comp), 99, &decomp).ok());
}

TEST(Zstd, EmptyInput) {
  std::vector<uint8_t> in, comp, decomp;
  ASSERT_TRUE(zstd_compress(Slice(in), 3, &comp).ok());
  ASSERT_TRUE(zstd_decompress(Slice(comp), 0, &decomp).ok());
  EXPECT_TRUE(decomp.empty());
}
