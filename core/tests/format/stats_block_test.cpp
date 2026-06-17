#include <gtest/gtest.h>

#include <cstdint>

#include "snii/encoding/byte_sink.h"
#include "snii/encoding/byte_source.h"
#include "snii/format/stats_block.h"

using namespace snii;
using namespace snii::format;

namespace {

// 编码后再解码，断言所有字段逐一相等。
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
  StatsBlock sb{};  // 全 0：空 segment 合法
  ExpectRoundTrip(sb);
}

TEST(StatsBlock, RoundTripNear2Pow63) {
  StatsBlock sb{};
  const uint64_t kBig = (1ull << 63) - 1;  // 大值边界
  sb.doc_count = kBig;
  sb.indexed_doc_count = kBig - 1;
  sb.term_count = (1ull << 63) + 7;  // 高位为 1 也必须可往返
  sb.sum_total_term_freq = UINT64_MAX;
  sb.null_count = (1ull << 62);
  ExpectRoundTrip(sb);
}

TEST(StatsBlock, FramedAsStatsBlockType) {
  StatsBlock sb{};
  sb.doc_count = 7;
  ByteSink sink;
  encode_stats_block(sb, &sink);
  // 首字节为 SectionFramer 的 type，应为 kStatsBlock。
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
  // 翻转 payload 区域的一个字节（跳过 type+len 前缀），crc 必须检出。
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
  bytes.pop_back();  // 截断尾字节
  Slice truncated(bytes);
  ByteSource src(truncated);
  StatsBlock out{};
  EXPECT_FALSE(decode_stats_block(&src, &out).ok());
}

TEST(StatsBlock, WrongSectionTypeRejected) {
  // 用 framer 写入非 StatsBlock 的 section，decode 必须拒绝。
  ByteSink sink;
  const uint8_t p[] = {1, 2, 3};
  SectionFramer::write(
      sink, static_cast<uint8_t>(SectionType::kXFilter), Slice(p, 3));
  ByteSource src(sink.view());
  StatsBlock out{};
  Status s = decode_stats_block(&src, &out);
  EXPECT_FALSE(s.ok());
}
