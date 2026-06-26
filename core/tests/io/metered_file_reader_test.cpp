#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "snii/common/status.h"
#include "snii/io/local_file.h"
#include "snii/io/metered_file_reader.h"

using namespace snii;
using snii::io::LocalFileReader;
using snii::io::LocalFileWriter;
using snii::io::MeteredFileReader;
using snii::io::Range;

namespace {

// Writes 256 bytes (byte[i] = i) to a temp file and returns its path.
std::string MakeRampFile() {
  const std::string path = "/tmp/snii_metered_ramp.bin";
  LocalFileWriter w;
  EXPECT_TRUE(w.open(path).ok());
  std::vector<uint8_t> data(256);
  for (int i = 0; i < 256; ++i) data[i] = static_cast<uint8_t>(i);
  EXPECT_TRUE(w.append(Slice(data)).ok());
  EXPECT_TRUE(w.finalize().ok());
  return path;
}

}  // namespace

// Single reads: first read to a block is a cache miss (1 round, 1 GET, 1 block of
// remote bytes); a second read to the same 16-byte block is a hit (no new round).
TEST(MeteredFileReader, SingleReadCacheAccounting) {
  LocalFileReader inner;
  ASSERT_TRUE(inner.open(MakeRampFile()).ok());
  MeteredFileReader m(&inner, /*block_size=*/16);

  std::vector<uint8_t> out;
  ASSERT_TRUE(m.read_at(0, 4, &out).ok());
  EXPECT_EQ(out[0], 0u);
  EXPECT_EQ(out[3], 3u);
  EXPECT_EQ(m.metrics().read_at_calls, 1u);
  EXPECT_EQ(m.metrics().serial_rounds, 1u);
  EXPECT_EQ(m.metrics().range_gets, 1u);
  EXPECT_EQ(m.metrics().remote_bytes, 16u);

  // Same block (offset 8..11) -> cache hit.
  ASSERT_TRUE(m.read_at(8, 4, &out).ok());
  EXPECT_EQ(m.metrics().read_at_calls, 2u);
  EXPECT_EQ(m.metrics().serial_rounds, 1u);  // unchanged
  EXPECT_EQ(m.metrics().range_gets, 1u);
  EXPECT_EQ(m.metrics().remote_bytes, 16u);

  // Different block (offset 20 -> block 1) -> miss.
  ASSERT_TRUE(m.read_at(20, 4, &out).ok());
  EXPECT_EQ(out[0], 20u);
  EXPECT_EQ(m.metrics().serial_rounds, 2u);
  EXPECT_EQ(m.metrics().range_gets, 2u);
  EXPECT_EQ(m.metrics().remote_bytes, 32u);
}

// A read spanning 3 contiguous blocks is one round and one coalesced GET.
TEST(MeteredFileReader, SpanMultipleBlocksCoalesced) {
  LocalFileReader inner;
  ASSERT_TRUE(inner.open(MakeRampFile()).ok());
  MeteredFileReader m(&inner, 16);

  std::vector<uint8_t> out;
  ASSERT_TRUE(m.read_at(0, 40, &out).ok());  // blocks 0,1,2
  EXPECT_EQ(out.size(), 40u);
  EXPECT_EQ(m.metrics().serial_rounds, 1u);
  EXPECT_EQ(m.metrics().range_gets, 1u);
  EXPECT_EQ(m.metrics().remote_bytes, 48u);  // 3 * 16
}

// A batch of reads to non-adjacent blocks: one serial round, one GET per run.
TEST(MeteredFileReader, BatchNonAdjacent) {
  LocalFileReader inner;
  ASSERT_TRUE(inner.open(MakeRampFile()).ok());
  MeteredFileReader m(&inner, 16);

  std::vector<Range> ranges = {{0, 4}, {100, 4}, {200, 4}};  // blocks 0, 6, 12
  std::vector<std::vector<uint8_t>> outs;
  ASSERT_TRUE(m.read_batch(ranges, &outs).ok());
  ASSERT_EQ(outs.size(), 3u);
  EXPECT_EQ(outs[1][0], 100u);
  EXPECT_EQ(m.metrics().read_at_calls, 3u);
  EXPECT_EQ(m.metrics().serial_rounds, 1u);  // one batch = one round
  EXPECT_EQ(m.metrics().range_gets, 3u);     // 3 disjoint runs
  EXPECT_EQ(m.metrics().remote_bytes, 48u);
}

// A batch of reads to adjacent blocks coalesces into a single GET.
TEST(MeteredFileReader, BatchAdjacentCoalesced) {
  LocalFileReader inner;
  ASSERT_TRUE(inner.open(MakeRampFile()).ok());
  MeteredFileReader m(&inner, 16);

  std::vector<Range> ranges = {{0, 4}, {16, 4}, {32, 4}};  // blocks 0,1,2
  std::vector<std::vector<uint8_t>> outs;
  ASSERT_TRUE(m.read_batch(ranges, &outs).ok());
  EXPECT_EQ(m.metrics().read_at_calls, 3u);
  EXPECT_EQ(m.metrics().serial_rounds, 1u);
  EXPECT_EQ(m.metrics().range_gets, 1u);  // coalesced
  EXPECT_EQ(m.metrics().remote_bytes, 48u);
}

// reset_metrics clears both counters and the resident cache (cold query).
TEST(MeteredFileReader, ResetClearsCacheAndCounters) {
  LocalFileReader inner;
  ASSERT_TRUE(inner.open(MakeRampFile()).ok());
  MeteredFileReader m(&inner, 16);

  std::vector<uint8_t> out;
  ASSERT_TRUE(m.read_at(0, 4, &out).ok());
  m.reset_metrics();
  EXPECT_EQ(m.metrics().read_at_calls, 0u);
  EXPECT_EQ(m.metrics().serial_rounds, 0u);
  // Cache cleared -> same read misses again.
  ASSERT_TRUE(m.read_at(0, 4, &out).ok());
  EXPECT_EQ(m.metrics().serial_rounds, 1u);
  EXPECT_EQ(m.metrics().remote_bytes, 16u);
}

TEST(MeteredFileReader, InvalidRangeDoesNotPolluteMetrics) {
  LocalFileReader inner;
  ASSERT_TRUE(inner.open(MakeRampFile()).ok());
  MeteredFileReader m(&inner, 16);

  std::vector<uint8_t> out;
  const Status st = m.read_at(250, 16, &out);
  EXPECT_EQ(st.code(), StatusCode::kCorruption) << st.to_string();
  EXPECT_EQ(m.metrics().read_at_calls, 0u);
  EXPECT_EQ(m.metrics().serial_rounds, 0u);
  EXPECT_EQ(m.metrics().range_gets, 0u);
  EXPECT_EQ(m.metrics().remote_bytes, 0u);
  EXPECT_EQ(m.metrics().total_request_bytes, 0u);
}

TEST(MeteredFileReader, InvalidBatchRangeDoesNotPolluteMetrics) {
  LocalFileReader inner;
  ASSERT_TRUE(inner.open(MakeRampFile()).ok());
  MeteredFileReader m(&inner, 16);

  std::vector<std::vector<uint8_t>> outs;
  const Status st = m.read_batch({Range{0, 4}, Range{250, 16}}, &outs);
  EXPECT_EQ(st.code(), StatusCode::kCorruption) << st.to_string();
  EXPECT_EQ(m.metrics().read_at_calls, 0u);
  EXPECT_EQ(m.metrics().serial_rounds, 0u);
  EXPECT_EQ(m.metrics().range_gets, 0u);
  EXPECT_EQ(m.metrics().remote_bytes, 0u);
  EXPECT_EQ(m.metrics().total_request_bytes, 0u);
}
