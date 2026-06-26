#include "snii/query/internal/docid_set_ops.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

std::vector<uint32_t> Range(uint32_t begin, uint32_t end, uint32_t step = 1) {
  std::vector<uint32_t> out;
  for (uint32_t v = begin; v < end; v += step) out.push_back(v);
  return out;
}

}  // namespace

TEST(DocIdSetOps, UnionSortedManyDeduplicatesHighOverlapLists) {
  std::vector<std::vector<uint32_t>> lists;
  lists.reserve(257);
  lists.push_back(Range(0, 10000));
  for (uint32_t i = 0; i < 256; ++i) {
    lists.push_back(Range(i % 17, 10000, 17));
  }

  const std::vector<uint32_t> got = snii::query::internal::union_sorted_many(lists);

  EXPECT_EQ(got, Range(0, 10000));
  EXPECT_TRUE(std::is_sorted(got.begin(), got.end()));
  EXPECT_EQ(std::adjacent_find(got.begin(), got.end()), got.end());
}

TEST(DocIdSetOps, UnionSortedManyMergesDisjointLists) {
  const std::vector<std::vector<uint32_t>> lists = {
      {0, 3, 6}, {1, 4, 7}, {}, {2, 5, 8}};

  const std::vector<uint32_t> got = snii::query::internal::union_sorted_many(lists);

  EXPECT_EQ(got, Range(0, 9));
}
