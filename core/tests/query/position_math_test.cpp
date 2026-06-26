#include "snii/query/internal/position_math.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

TEST(PositionMath, AddsOffsetWhenRepresentable) {
  uint32_t out = 0;
  EXPECT_TRUE(snii::query::internal::add_position_offset(41, 1, &out));
  EXPECT_EQ(out, 42u);
  EXPECT_TRUE(snii::query::internal::add_position_offset(
      std::numeric_limits<uint32_t>::max() - 2, 2, &out));
  EXPECT_EQ(out, std::numeric_limits<uint32_t>::max());
}

TEST(PositionMath, RejectsWraparound) {
  uint32_t out = 7;
  EXPECT_FALSE(snii::query::internal::add_position_offset(
      std::numeric_limits<uint32_t>::max(), 1, &out));
  EXPECT_EQ(out, 7u);
}

TEST(PositionMath, BuildsDenseOffsets) {
  std::vector<uint32_t> offsets;
  EXPECT_TRUE(snii::query::internal::build_position_offsets(4, &offsets));
  EXPECT_EQ(offsets, (std::vector<uint32_t>{0u, 1u, 2u, 3u}));
}

TEST(PositionMath, RejectsUnrepresentableOffsetCount) {
  std::vector<uint32_t> offsets = {9};
  EXPECT_FALSE(snii::query::internal::build_position_offsets(
      std::numeric_limits<size_t>::max(), &offsets));
  EXPECT_EQ(offsets, (std::vector<uint32_t>{9u}));
}
