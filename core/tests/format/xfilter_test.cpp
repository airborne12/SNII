#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "snii/encoding/byte_sink.h"
#include "snii/encoding/section_framer.h"
#include "snii/format/format_constants.h"
#include "snii/format/xfilter.h"

using namespace snii;
using namespace snii::format;

namespace {

// Generate a deterministic set of distinct terms.
std::vector<std::string> MakeTerms(const std::string& prefix, size_t count) {
  std::vector<std::string> terms;
  terms.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    terms.push_back(prefix + std::to_string(i) + "_term");
  }
  return terms;
}

// Build an XFilter byte buffer from a term set.
std::vector<uint8_t> BuildFilter(const std::vector<std::string>& terms) {
  ByteSink sink;
  Status s = build_xfilter(terms, &sink);
  EXPECT_TRUE(s.ok()) << s.to_string();
  return sink.buffer();
}

// Open a reader or fail.
XFilterReader OpenOrDie(const std::vector<uint8_t>& bytes) {
  XFilterReader reader;
  Status s = XFilterReader::open(Slice(bytes), &reader);
  EXPECT_TRUE(s.ok()) << s.to_string();
  return reader;
}

}  // namespace

// Zero false negatives: every inserted term must report maybe_contains == true.
TEST(XFilter, NoFalseNegativesLargeSet) {
  const auto terms = MakeTerms("ins_", 5000);
  auto bytes = BuildFilter(terms);
  auto reader = OpenOrDie(bytes);
  EXPECT_GT(reader.fingerprint_count(), 0u);

  for (const auto& t : terms) {
    EXPECT_TRUE(reader.maybe_contains(t)) << "false negative for term=" << t;
  }
}

// False-positive rate over a disjoint non-inserted set must stay low (< 2%).
TEST(XFilter, LowFalsePositiveRate) {
  const auto inserted = MakeTerms("ins_", 5000);
  auto bytes = BuildFilter(inserted);
  auto reader = OpenOrDie(bytes);

  const auto probes = MakeTerms("absent_", 20000);
  size_t false_positives = 0;
  for (const auto& t : probes) {
    if (reader.maybe_contains(t)) ++false_positives;
  }
  const double fp_rate = static_cast<double>(false_positives) / probes.size();
  EXPECT_LT(fp_rate, 0.02) << "fp_rate=" << fp_rate;
}

// Empty term set: build succeeds, every probe is absent.
TEST(XFilter, EmptyTermSet) {
  auto bytes = BuildFilter({});
  auto reader = OpenOrDie(bytes);
  EXPECT_FALSE(reader.maybe_contains("anything"));
  EXPECT_FALSE(reader.maybe_contains(""));
}

// Single term: present, and a disjoint probe is absent.
TEST(XFilter, SingleTerm) {
  const std::vector<std::string> terms = {"lonely_term"};
  auto bytes = BuildFilter(terms);
  auto reader = OpenOrDie(bytes);
  EXPECT_TRUE(reader.maybe_contains("lonely_term"));
  EXPECT_FALSE(reader.maybe_contains("some_other_term_xyz"));
}

// Duplicate terms must dedup internally without breaking membership.
TEST(XFilter, DuplicateTermsDedup) {
  std::vector<std::string> terms;
  for (int i = 0; i < 200; ++i) {
    terms.push_back("dup_a");
    terms.push_back("dup_b");
    terms.push_back("dup_c");
  }
  auto bytes = BuildFilter(terms);
  auto reader = OpenOrDie(bytes);
  EXPECT_TRUE(reader.maybe_contains("dup_a"));
  EXPECT_TRUE(reader.maybe_contains("dup_b"));
  EXPECT_TRUE(reader.maybe_contains("dup_c"));
}

// Unsorted input order must not affect membership.
TEST(XFilter, UnsortedInput) {
  const std::vector<std::string> terms = {"zebra", "apple", "mango", "cherry", "banana"};
  auto bytes = BuildFilter(terms);
  auto reader = OpenOrDie(bytes);
  for (const auto& t : terms) {
    EXPECT_TRUE(reader.maybe_contains(t)) << "term=" << t;
  }
}

// Binary-safe terms (null bytes / high bits) round-trip correctly.
TEST(XFilter, BinarySafeTerms) {
  std::vector<std::string> terms;
  terms.push_back(std::string("\x01\x00z", 3));
  terms.push_back(std::string("\x80\x00", 2));
  terms.push_back(std::string("\xFF\xFF\xFF", 3));
  auto bytes = BuildFilter(terms);
  auto reader = OpenOrDie(bytes);
  for (const auto& t : terms) {
    EXPECT_TRUE(reader.maybe_contains(t));
  }
}

// The first framed byte is the SectionFramer type and must be kXFilter.
TEST(XFilter, FramedAsXFilterType) {
  auto bytes = BuildFilter({"alpha", "beta"});
  ASSERT_GE(bytes.size(), 1u);
  EXPECT_EQ(bytes[0], static_cast<uint8_t>(SectionType::kXFilter));
}

// CRC checksum: flipping one byte in the payload region → open returns Corruption.
TEST(XFilter, DetectsCorruption) {
  auto bytes = BuildFilter(MakeTerms("c_", 100));
  ASSERT_GE(bytes.size(), 8u);
  bytes[5] ^= 0xFF;  // Flip a byte in the payload region (skip type+len prefix).
  XFilterReader reader;
  Status s = XFilterReader::open(Slice(bytes), &reader);
  EXPECT_EQ(s.code(), StatusCode::kCorruption) << s.to_string();
}

// Truncation: dropping the trailing byte → open fails.
TEST(XFilter, DetectsTruncation) {
  auto bytes = BuildFilter(MakeTerms("t_", 100));
  bytes.pop_back();
  XFilterReader reader;
  Status s = XFilterReader::open(Slice(bytes), &reader);
  EXPECT_FALSE(s.ok());
}

// A wrong section type must be rejected.
TEST(XFilter, WrongSectionTypeRejected) {
  ByteSink sink;
  const uint8_t p[] = {0, 0};
  SectionFramer::write(sink, static_cast<uint8_t>(SectionType::kStatsBlock), Slice(p, 2));
  XFilterReader reader;
  Status s = XFilterReader::open(sink.view(), &reader);
  EXPECT_FALSE(s.ok());
}

// Null sink / null out are rejected with InvalidArgument.
TEST(XFilter, NullPointersRejected) {
  EXPECT_EQ(build_xfilter({"a"}, nullptr).code(), StatusCode::kInvalidArgument);
  auto bytes = BuildFilter({"a"});
  EXPECT_EQ(XFilterReader::open(Slice(bytes), nullptr).code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(build_xfilter_hashed({1, 2, 3}, nullptr).code(),
            StatusCode::kInvalidArgument);
}

// build_xfilter_hashed (pre-hashed keys) must produce BYTE-IDENTICAL output to
// build_xfilter over the same term set: same dedup, params, seed search. This is
// what lets the writer collect 8-byte keys during the build instead of retaining
// every term string, with no change to the on-disk filter bytes.
TEST(XFilter, HashedMatchesStringBuildByteForByte) {
  for (size_t count : {size_t{0}, size_t{1}, size_t{7}, size_t{500}, size_t{5000}}) {
    const std::vector<std::string> terms = MakeTerms("hx", count);
    ByteSink str_sink;
    ASSERT_TRUE(build_xfilter(terms, &str_sink).ok());

    std::vector<uint64_t> keys;
    keys.reserve(terms.size());
    for (const auto& t : terms) keys.push_back(hash_term(t));
    ByteSink hash_sink;
    ASSERT_TRUE(build_xfilter_hashed(std::move(keys), &hash_sink).ok());

    EXPECT_EQ(str_sink.buffer(), hash_sink.buffer()) << "count=" << count;
  }
}

// build_xfilter_hashed normalizes (sorts+dedups) its input, so unsorted keys with
// duplicates yield the same filter as the clean set -- and the same membership.
TEST(XFilter, HashedNormalizesUnsortedDuplicateKeys) {
  const std::vector<std::string> terms = MakeTerms("dup", 64);
  std::vector<uint64_t> noisy;
  for (const auto& t : terms) {
    noisy.push_back(hash_term(t));
    noisy.push_back(hash_term(t));  // duplicate
  }
  std::reverse(noisy.begin(), noisy.end());  // unsorted

  ByteSink clean_sink, noisy_sink;
  std::vector<uint64_t> clean;
  for (const auto& t : terms) clean.push_back(hash_term(t));
  ASSERT_TRUE(build_xfilter_hashed(std::move(clean), &clean_sink).ok());
  ASSERT_TRUE(build_xfilter_hashed(std::move(noisy), &noisy_sink).ok());
  EXPECT_EQ(clean_sink.buffer(), noisy_sink.buffer());

  XFilterReader xf = OpenOrDie(noisy_sink.buffer());
  for (const auto& t : terms) EXPECT_TRUE(xf.maybe_contains(t)) << t;
  EXPECT_FALSE(xf.maybe_contains("definitely-absent-xyzzy"));
}
