#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "snii/encoding/byte_sink.h"
#include "snii/encoding/section_framer.h"
#include "snii/format/format_constants.h"
#include "snii/format/sampled_term_index.h"

using namespace snii;
using namespace snii::format;

namespace {

// 用一组有序 first_term 构建一个 SampledTermIndex 字节缓冲。
std::vector<uint8_t> BuildIndex(const std::vector<std::string>& first_terms) {
  SampledTermIndexBuilder builder;
  for (const auto& t : first_terms) {
    builder.add_block_first_term(t);
  }
  ByteSink sink;
  builder.finish(&sink);
  return sink.buffer();
}

// 打开 reader 的便捷封装。
SampledTermIndexReader OpenOrDie(const std::vector<uint8_t>& bytes) {
  SampledTermIndexReader reader;
  Status s = SampledTermIndexReader::open(Slice(bytes), &reader);
  EXPECT_TRUE(s.ok()) << s.to_string();
  return reader;
}

}  // namespace

// 多 block：locate 命中每个 first_term 的正确 ordinal。
TEST(SampledTermIndex, LocateExactFirstTermHitsOrdinal) {
  const std::vector<std::string> terms = {"alpha", "delta", "kappa", "omega"};
  auto bytes = BuildIndex(terms);
  auto reader = OpenOrDie(bytes);
  ASSERT_EQ(reader.n_blocks(), 4u);

  for (uint32_t i = 0; i < terms.size(); ++i) {
    bool maybe_present = false;
    uint32_t ord = 0xFFFFFFFFu;
    ASSERT_TRUE(reader.locate(terms[i], &maybe_present, &ord).ok());
    EXPECT_TRUE(maybe_present) << "term=" << terms[i];
    EXPECT_EQ(ord, i) << "term=" << terms[i];
  }
}

// target 落在两个 first_term 之间 → 返回较小者的 ordinal。
TEST(SampledTermIndex, LocateBetweenReturnsLowerOrdinal) {
  const std::vector<std::string> terms = {"alpha", "delta", "kappa", "omega"};
  auto bytes = BuildIndex(terms);
  auto reader = OpenOrDie(bytes);

  bool maybe_present = false;
  uint32_t ord = 0;
  // "echo" 介于 "delta"(1) 与 "kappa"(2) → 应落在 block 1。
  ASSERT_TRUE(reader.locate("echo", &maybe_present, &ord).ok());
  EXPECT_TRUE(maybe_present);
  EXPECT_EQ(ord, 1u);

  // "beta" 介于 "alpha"(0) 与 "delta"(1) → block 0。
  ASSERT_TRUE(reader.locate("beta", &maybe_present, &ord).ok());
  EXPECT_TRUE(maybe_present);
  EXPECT_EQ(ord, 0u);

  // "zzz" > "omega"(3) 仍在 [min,max] 之外（> max_term）→ 越界。
  ASSERT_TRUE(reader.locate("zzz", &maybe_present, &ord).ok());
  EXPECT_FALSE(maybe_present);
}

// target < min_term → maybe_present=false（不存在信号）。
TEST(SampledTermIndex, LocateBelowMinIsOutOfRange) {
  const std::vector<std::string> terms = {"banana", "cherry", "mango"};
  auto bytes = BuildIndex(terms);
  auto reader = OpenOrDie(bytes);

  bool maybe_present = true;
  uint32_t ord = 12345;
  ASSERT_TRUE(reader.locate("apple", &maybe_present, &ord).ok());
  EXPECT_FALSE(maybe_present);
}

// target > max_term → maybe_present=false。
TEST(SampledTermIndex, LocateAboveMaxIsOutOfRange) {
  const std::vector<std::string> terms = {"banana", "cherry", "mango"};
  auto bytes = BuildIndex(terms);
  auto reader = OpenOrDie(bytes);

  bool maybe_present = true;
  uint32_t ord = 0;
  ASSERT_TRUE(reader.locate("zebra", &maybe_present, &ord).ok());
  EXPECT_FALSE(maybe_present);
}

// target == min_term / == max_term 边界都应命中且 in-range。
TEST(SampledTermIndex, LocateBoundaryTermsInRange) {
  const std::vector<std::string> terms = {"banana", "cherry", "mango"};
  auto bytes = BuildIndex(terms);
  auto reader = OpenOrDie(bytes);

  bool maybe_present = false;
  uint32_t ord = 0;
  ASSERT_TRUE(reader.locate("banana", &maybe_present, &ord).ok());
  EXPECT_TRUE(maybe_present);
  EXPECT_EQ(ord, 0u);

  ASSERT_TRUE(reader.locate("mango", &maybe_present, &ord).ok());
  EXPECT_TRUE(maybe_present);
  EXPECT_EQ(ord, 2u);
}

// 单 block：min==max==唯一 first_term。
TEST(SampledTermIndex, SingleBlock) {
  const std::vector<std::string> terms = {"solo"};
  auto bytes = BuildIndex(terms);
  auto reader = OpenOrDie(bytes);
  ASSERT_EQ(reader.n_blocks(), 1u);

  bool maybe_present = false;
  uint32_t ord = 99;
  ASSERT_TRUE(reader.locate("solo", &maybe_present, &ord).ok());
  EXPECT_TRUE(maybe_present);
  EXPECT_EQ(ord, 0u);

  // "solz" > "solo" → 越界（> max_term）。
  ASSERT_TRUE(reader.locate("solz", &maybe_present, &ord).ok());
  EXPECT_FALSE(maybe_present);

  // "sola" < "solo" → 越界（< min_term）。
  ASSERT_TRUE(reader.locate("sola", &maybe_present, &ord).ok());
  EXPECT_FALSE(maybe_present);
}

// 共享长前缀的 term（验证前缀压缩往返正确）。
TEST(SampledTermIndex, SharedPrefixTermsRoundTrip) {
  const std::vector<std::string> terms = {
      "international", "internationalize", "internationalized",
      "internet", "interoperate"};
  auto bytes = BuildIndex(terms);
  auto reader = OpenOrDie(bytes);
  ASSERT_EQ(reader.n_blocks(), 5u);

  for (uint32_t i = 0; i < terms.size(); ++i) {
    bool maybe_present = false;
    uint32_t ord = 0;
    ASSERT_TRUE(reader.locate(terms[i], &maybe_present, &ord).ok());
    EXPECT_TRUE(maybe_present) << "term=" << terms[i];
    EXPECT_EQ(ord, i) << "term=" << terms[i];
  }

  // "internationalizes" 介于 idx2 与 idx3 → 较小者 ordinal 2。
  bool maybe_present = false;
  uint32_t ord = 0;
  ASSERT_TRUE(reader.locate("internationalizes", &maybe_present, &ord).ok());
  EXPECT_TRUE(maybe_present);
  EXPECT_EQ(ord, 2u);
}

// 含空字节 / 高位字节的 term，按无符号字节序比较。
TEST(SampledTermIndex, BinarySafeTerms) {
  std::vector<std::string> terms;
  terms.push_back(std::string("\x01\x00z", 3));
  terms.push_back(std::string("\x80\x00", 2));
  terms.push_back(std::string("\xFF", 1));
  auto bytes = BuildIndex(terms);
  auto reader = OpenOrDie(bytes);
  ASSERT_EQ(reader.n_blocks(), 3u);

  bool maybe_present = false;
  uint32_t ord = 0;
  ASSERT_TRUE(reader.locate(std::string("\x80\x00", 2), &maybe_present, &ord).ok());
  EXPECT_TRUE(maybe_present);
  EXPECT_EQ(ord, 1u);
}

// 首字节即 SectionFramer 的 type，应为 kSampledTermIndex。
TEST(SampledTermIndex, FramedAsSampledTermIndexType) {
  auto bytes = BuildIndex({"alpha", "beta"});
  ASSERT_GE(bytes.size(), 1u);
  EXPECT_EQ(bytes[0], static_cast<uint8_t>(SectionType::kSampledTermIndex));
}

// crc 校验：翻转 payload 中一个字节 → open 返回 Corruption。
TEST(SampledTermIndex, DetectsCorruption) {
  auto bytes = BuildIndex({"alpha", "delta", "kappa"});
  ASSERT_GE(bytes.size(), 4u);
  bytes[3] ^= 0xFF;  // 翻转 payload 区域字节（跳过 type+len 前缀）
  SampledTermIndexReader reader;
  Status s = SampledTermIndexReader::open(Slice(bytes), &reader);
  EXPECT_EQ(s.code(), StatusCode::kCorruption) << s.to_string();
}

// 截断：丢弃尾字节 → open 失败。
TEST(SampledTermIndex, DetectsTruncation) {
  auto bytes = BuildIndex({"alpha", "delta", "kappa"});
  bytes.pop_back();
  SampledTermIndexReader reader;
  Status s = SampledTermIndexReader::open(Slice(bytes), &reader);
  EXPECT_FALSE(s.ok());
}

// 错误 section type 应被拒绝。
TEST(SampledTermIndex, WrongSectionTypeRejected) {
  ByteSink sink;
  const uint8_t p[] = {0, 0};
  SectionFramer::write(sink, static_cast<uint8_t>(SectionType::kXFilter),
                       Slice(p, 2));
  SampledTermIndexReader reader;
  Status s = SampledTermIndexReader::open(sink.view(), &reader);
  EXPECT_FALSE(s.ok());
}

// 空 builder（n_blocks=0）：合法构建，locate 任何 term 都越界。
TEST(SampledTermIndex, EmptyIndexLocateOutOfRange) {
  auto bytes = BuildIndex({});
  auto reader = OpenOrDie(bytes);
  ASSERT_EQ(reader.n_blocks(), 0u);

  bool maybe_present = true;
  uint32_t ord = 7;
  ASSERT_TRUE(reader.locate("anything", &maybe_present, &ord).ok());
  EXPECT_FALSE(maybe_present);
}
