#include <gtest/gtest.h>

#include <cstdint>

#include "snii/encoding/byte_sink.h"
#include "snii/encoding/byte_source.h"
#include "snii/format/stats_block.h"

using namespace snii;
using namespace snii::format;

namespace {

// Encode then decode; assert every field is equal one by one.
void ExpectRoundTrip(const StatsBlock& in) {
  ByteSink sink;
  encode_stats_block(in, &sink);
  ByteSource src(sink.view());
  StatsBlock out{};
  ASSERT_TRUE(decode_stats_block(&src, &out).ok());
  EXPECT_EQ(out.doc_count, in.doc_count);
  EXPECT_EQ(out.indexed_doc_count, in.indexed_doc_count);
  EXPECT_EQ(out.term_count, in.term_count);
  EXPECT_EQ(out.sum_total_term_freq, in.sum_total_term_freq);
  EXPECT_EQ(out.null_count, in.null_count);
  EXPECT_TRUE(src.eof());
}

}  // namespace

TEST(StatsBlock, RoundTripTypicalValues) {
  StatsBlock sb{};
  sb.doc_count = 1000;
  sb.indexed_doc_count = 980;
  sb.term_count = 54321;
  sb.sum_total_term_freq = 1234567;
  sb.null_count = 20;
  ExpectRoundTrip(sb);
}

TEST(StatsBlock, RoundTripZeroes) {
  StatsBlock sb{};  // All zeros: empty segment is valid
  ExpectRoundTrip(sb);
}

TEST(StatsBlock, RoundTripNear2Pow63) {
  StatsBlock sb{};
  const uint64_t kBig = (1ull << 63) - 1;  // Large-value boundary
  sb.doc_count = kBig;
  sb.indexed_doc_count = kBig - 1;
  sb.term_count = (1ull << 63) + 7;  // High bit set must also round-trip correctly
  sb.sum_total_term_freq = UINT64_MAX;
  sb.null_count = (1ull << 62);
  ExpectRoundTrip(sb);
}

TEST(StatsBlock, FramedAsStatsBlockType) {
  StatsBlock sb{};
  sb.doc_count = 7;
  ByteSink sink;
  encode_stats_block(sb, &sink);
  // First byte is the SectionFramer type field and must equal kStatsBlock.
  ASSERT_GE(sink.size(), 1u);
  EXPECT_EQ(sink.buffer()[0],
            static_cast<uint8_t>(SectionType::kStatsBlock));
}

TEST(StatsBlock, DetectsCorruption) {
  StatsBlock sb{};
  sb.doc_count = 42;
  sb.indexed_doc_count = 40;
  sb.term_count = 9;
  sb.sum_total_term_freq = 1000;
  sb.null_count = 2;
  ByteSink sink;
  encode_stats_block(sb, &sink);

  auto bytes = sink.buffer();
  // Flip one byte in the payload area (skip the type+len prefix); CRC must detect the corruption.
  ASSERT_GE(bytes.size(), 3u);
  bytes[2] ^= 0xFF;
  Slice corrupted(bytes);
  ByteSource src(corrupted);
  StatsBlock out{};
  EXPECT_EQ(decode_stats_block(&src, &out).code(), StatusCode::kCorruption);
}

TEST(StatsBlock, DetectsTruncation) {
  StatsBlock sb{};
  sb.doc_count = 100;
  ByteSink sink;
  encode_stats_block(sb, &sink);
  auto bytes = sink.buffer();
  bytes.pop_back();  // Truncate the last byte
  Slice truncated(bytes);
  ByteSource src(truncated);
  StatsBlock out{};
  EXPECT_FALSE(decode_stats_block(&src, &out).ok());
}

TEST(StatsBlock, WrongSectionTypeRejected) {
  // Write a non-StatsBlock section via the framer; decode must reject it.
  ByteSink sink;
  const uint8_t p[] = {1, 2, 3};
  SectionFramer::write(
      sink, static_cast<uint8_t>(SectionType::kXFilter), Slice(p, 3));
  ByteSource src(sink.view());
  StatsBlock out{};
  Status s = decode_stats_block(&src, &out);
  EXPECT_FALSE(s.ok());
}
