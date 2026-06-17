#include <gtest/gtest.h>

#include "snii/common/slice.h"

using snii::Slice;

TEST(Slice, BasicAccess) {
  const uint8_t buf[] = {1, 2, 3, 4};
  Slice s(buf, 4);
  EXPECT_EQ(s.size(), 4u);
  EXPECT_EQ(s[2], 3u);
  Slice sub = s.subslice(1, 2);
  EXPECT_EQ(sub.size(), 2u);
  EXPECT_EQ(sub[0], 2u);
}

TEST(Slice, EmptyDefault) {
  Slice s;
  EXPECT_TRUE(s.empty());
  EXPECT_EQ(s.size(), 0u);
}
