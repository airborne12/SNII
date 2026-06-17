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

// Build a SampledTermIndex byte buffer from an ordered set of first_terms.
std::vector<uint8_t> BuildIndex(const std::vector<std::string>& first_terms) {
  SampledTermIndexBuilder builder;
  for (const auto& t : first_terms) {
    builder.add_block_first_term(t);
  }
  ByteSink sink;
  builder.finish(&sink);
  return sink.buffer();
}

// Convenience wrapper to open a reader.
SampledTermIndexReader OpenOrDie(const std::vector<uint8_t>& bytes) {
  SampledTermIndexReader reader;
  Status s = SampledTermIndexReader::open(Slice(bytes), &reader);
  EXPECT_TRUE(s.ok()) << s.to_string();
  return reader;
}

}  // namespace

// Multiple blocks: locate returns the correct ordinal for each first_term.
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

// target falls between two first_terms → returns the ordinal of the lower one.
TEST(SampledTermIndex, LocateBetweenReturnsLowerOrdinal) {
  const std::vector<std::string> terms = {"alpha", "delta", "kappa", "omega"};
  auto bytes = BuildIndex(terms);
  auto reader = OpenOrDie(bytes);

  bool maybe_present = false;
  uint32_t ord = 0;
  // "echo" is between "delta"(1) and "kappa"(2) → should land in block 1.
  ASSERT_TRUE(reader.locate("echo", &maybe_present, &ord).ok());
  EXPECT_TRUE(maybe_present);
  EXPECT_EQ(ord, 1u);

  // "beta" is between "alpha"(0) and "delta"(1) → block 0.
  ASSERT_TRUE(reader.locate("beta", &maybe_present, &ord).ok());
  EXPECT_TRUE(maybe_present);
  EXPECT_EQ(ord, 0u);

  // "zzz" > "omega"(3) is outside [min,max] (> max_term) → out of range.
  ASSERT_TRUE(reader.locate("zzz", &maybe_present, &ord).ok());
  EXPECT_FALSE(maybe_present);
}

// target < min_term → maybe_present=false (not-present signal).
TEST(SampledTermIndex, LocateBelowMinIsOutOfRange) {
  const std::vector<std::string> terms = {"banana", "cherry", "mango"};
  auto bytes = BuildIndex(terms);
  auto reader = OpenOrDie(bytes);

  bool maybe_present = true;
  uint32_t ord = 12345;
  ASSERT_TRUE(reader.locate("apple", &maybe_present, &ord).ok());
  EXPECT_FALSE(maybe_present);
}

// target > max_term → maybe_present=false.
TEST(SampledTermIndex, LocateAboveMaxIsOutOfRange) {
  const std::vector<std::string> terms = {"banana", "cherry", "mango"};
  auto bytes = BuildIndex(terms);
  auto reader = OpenOrDie(bytes);

  bool maybe_present = true;
  uint32_t ord = 0;
  ASSERT_TRUE(reader.locate("zebra", &maybe_present, &ord).ok());
  EXPECT_FALSE(maybe_present);
}

// target == min_term / == max_term boundary cases should both hit and be in-range.
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

// Single block: min==max==the only first_term.
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

  // "solz" > "solo" → out of range (> max_term).
  ASSERT_TRUE(reader.locate("solz", &maybe_present, &ord).ok());
  EXPECT_FALSE(maybe_present);

  // "sola" < "solo" → out of range (< min_term).
  ASSERT_TRUE(reader.locate("sola", &maybe_present, &ord).ok());
  EXPECT_FALSE(maybe_present);
}

// Terms sharing a long common prefix (verify front coding round-trip correctness).
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

  // "internationalizes" is between idx2 and idx3 → lower ordinal 2.
  bool maybe_present = false;
  uint32_t ord = 0;
  ASSERT_TRUE(reader.locate("internationalizes", &maybe_present, &ord).ok());
  EXPECT_TRUE(maybe_present);
  EXPECT_EQ(ord, 2u);
}

// Terms containing null bytes / high-bit bytes, compared by unsigned byte order.
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

// The first byte is the SectionFramer type and must be kSampledTermIndex.
TEST(SampledTermIndex, FramedAsSampledTermIndexType) {
  auto bytes = BuildIndex({"alpha", "beta"});
  ASSERT_GE(bytes.size(), 1u);
  EXPECT_EQ(bytes[0], static_cast<uint8_t>(SectionType::kSampledTermIndex));
}

// CRC checksum: flipping one byte in the payload → open returns Corruption.
TEST(SampledTermIndex, DetectsCorruption) {
  auto bytes = BuildIndex({"alpha", "delta", "kappa"});
  ASSERT_GE(bytes.size(), 4u);
  bytes[3] ^= 0xFF;  // Flip a byte in the payload region (skip the type+len prefix)
  SampledTermIndexReader reader;
  Status s = SampledTermIndexReader::open(Slice(bytes), &reader);
  EXPECT_EQ(s.code(), StatusCode::kCorruption) << s.to_string();
}

// Truncation: drop the trailing byte → open fails.
TEST(SampledTermIndex, DetectsTruncation) {
  auto bytes = BuildIndex({"alpha", "delta", "kappa"});
  bytes.pop_back();
  SampledTermIndexReader reader;
  Status s = SampledTermIndexReader::open(Slice(bytes), &reader);
  EXPECT_FALSE(s.ok());
}

// A wrong section type should be rejected.
TEST(SampledTermIndex, WrongSectionTypeRejected) {
  ByteSink sink;
  const uint8_t p[] = {0, 0};
  SectionFramer::write(sink, static_cast<uint8_t>(SectionType::kXFilter),
                       Slice(p, 2));
  SampledTermIndexReader reader;
  Status s = SampledTermIndexReader::open(sink.view(), &reader);
  EXPECT_FALSE(s.ok());
}

// Empty builder (n_blocks=0): valid build, locate on any term is out of range.
TEST(SampledTermIndex, EmptyIndexLocateOutOfRange) {
  auto bytes = BuildIndex({});
  auto reader = OpenOrDie(bytes);
  ASSERT_EQ(reader.n_blocks(), 0u);

  bool maybe_present = true;
  uint32_t ord = 7;
  ASSERT_TRUE(reader.locate("anything", &maybe_present, &ord).ok());
  EXPECT_FALSE(maybe_present);
}
