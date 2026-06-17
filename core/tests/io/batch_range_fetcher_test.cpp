#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "snii/common/slice.h"
#include "snii/io/batch_range_fetcher.h"
#include "snii/io/local_file.h"
#include "snii/io/metered_file_reader.h"

using namespace snii;
using snii::io::BatchRangeFetcher;
using snii::io::LocalFileReader;
using snii::io::LocalFileWriter;
using snii::io::MeteredFileReader;

namespace {

std::string MakeRampFile() {
  const std::string path = "/tmp/snii_brf_ramp.bin";
  LocalFileWriter w;
  EXPECT_TRUE(w.open(path).ok());
  std::vector<uint8_t> data(256);
  for (int i = 0; i < 256; ++i) data[i] = static_cast<uint8_t>(i);
  EXPECT_TRUE(w.append(Slice(data)).ok());
  EXPECT_TRUE(w.finalize().ok());
  return path;
}

}  // namespace

// Disjoint ranges: each handle returns its own bytes; the whole fetch is one
// serial round on the metered reader.
TEST(BatchRangeFetcher, DisjointRanges) {
  LocalFileReader inner;
  ASSERT_TRUE(inner.open(MakeRampFile()).ok());
  MeteredFileReader m(&inner, 16);

  BatchRangeFetcher f(&m);
  size_t h0 = f.add(0, 4);
  size_t h1 = f.add(100, 4);
  size_t h2 = f.add(200, 4);
  ASSERT_TRUE(f.fetch().ok());

  EXPECT_EQ(f.get(h0)[0], 0u);
  EXPECT_EQ(f.get(h1)[0], 100u);
  EXPECT_EQ(f.get(h2)[3], 203u);
  EXPECT_EQ(m.metrics().serial_rounds, 1u);  // single batched round
}

// Overlapping requests coalesce into one physical read; bytes still map back.
TEST(BatchRangeFetcher, OverlappingCoalesced) {
  LocalFileReader inner;
  ASSERT_TRUE(inner.open(MakeRampFile()).ok());
  MeteredFileReader m(&inner, 16);

  BatchRangeFetcher f(&m);
  size_t h0 = f.add(0, 4);   // [0,4)
  size_t h1 = f.add(2, 4);   // [2,6) overlaps
  size_t h2 = f.add(5, 3);   // [5,8) adjacent/overlaps
  ASSERT_TRUE(f.fetch().ok());

  EXPECT_EQ(f.get(h0)[0], 0u);
  EXPECT_EQ(f.get(h1)[0], 2u);
  EXPECT_EQ(f.get(h2)[0], 5u);
  EXPECT_EQ(f.get(h2)[2], 7u);
  // Coalesced into a single physical read -> one read_at_call on the metered reader.
  EXPECT_EQ(m.metrics().read_at_calls, 1u);
}

// clear() lets the fetcher be reused for a new round.
TEST(BatchRangeFetcher, ClearAndReuse) {
  LocalFileReader inner;
  ASSERT_TRUE(inner.open(MakeRampFile()).ok());
  MeteredFileReader m(&inner, 16);

  BatchRangeFetcher f(&m);
  f.add(0, 4);
  ASSERT_TRUE(f.fetch().ok());
  f.clear();
  EXPECT_EQ(f.pending(), 0u);
  size_t h = f.add(64, 8);
  ASSERT_TRUE(f.fetch().ok());
  EXPECT_EQ(f.get(h)[0], 64u);
}
