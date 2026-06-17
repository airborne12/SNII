#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/section_framer.h"
#include "snii/format/format_constants.h"
#include "snii/format/logical_index_directory.h"

using namespace snii;
using namespace snii::format;

namespace {

// Serialize a list of refs via the builder and return the framed section bytes.
std::vector<uint8_t> Build(const std::vector<LogicalIndexRef>& refs) {
  LogicalIndexDirectoryBuilder builder;
  for (const auto& r : refs) {
    builder.add(r);
  }
  ByteSink sink;
  builder.finish(&sink);
  return sink.buffer();
}

// Assert that two LogicalIndexRef structs are equal field by field.
void ExpectRefEq(const LogicalIndexRef& a, const LogicalIndexRef& b) {
  EXPECT_EQ(a.index_id, b.index_id);
  EXPECT_EQ(a.index_suffix, b.index_suffix);
  EXPECT_EQ(a.meta_off, b.meta_off);
  EXPECT_EQ(a.meta_len, b.meta_len);
}

}  // namespace

TEST(LogicalIndexDirectory, RoundTripMultipleEntries) {
  std::vector<LogicalIndexRef> refs = {
      {1, "", 0, 4096},
      {2, "fulltext", 4096, 8192},
      {7, "phrase_v2", 12288, 2048},
  };
  auto bytes = Build(refs);

  LogicalIndexDirectoryReader reader;
  ASSERT_TRUE(LogicalIndexDirectoryReader::open(Slice(bytes), &reader).ok());
  ASSERT_EQ(reader.size(), 3u);
  for (uint32_t i = 0; i < refs.size(); ++i) {
    LogicalIndexRef out{};
    ASSERT_TRUE(reader.get(i, &out).ok());
    ExpectRefEq(out, refs[i]);
  }
}

TEST(LogicalIndexDirectory, GetOrdinalOutOfRangeRejected) {
  std::vector<LogicalIndexRef> refs = {{1, "a", 0, 100}};
  auto bytes = Build(refs);

  LogicalIndexDirectoryReader reader;
  ASSERT_TRUE(LogicalIndexDirectoryReader::open(Slice(bytes), &reader).ok());
  ASSERT_EQ(reader.size(), 1u);
  LogicalIndexRef out{};
  // ordinal == size is out of range
  EXPECT_EQ(reader.get(1, &out).code(), StatusCode::kNotFound);
  EXPECT_EQ(reader.get(1000, &out).code(), StatusCode::kNotFound);
}

TEST(LogicalIndexDirectory, FindHit) {
  std::vector<LogicalIndexRef> refs = {
      {1, "", 0, 4096},
      {2, "fulltext", 4096, 8192},
      {7, "phrase_v2", 12288, 2048},
  };
  auto bytes = Build(refs);

  LogicalIndexDirectoryReader reader;
  ASSERT_TRUE(LogicalIndexDirectoryReader::open(Slice(bytes), &reader).ok());

  bool found = false;
  LogicalIndexRef out{};
  ASSERT_TRUE(reader.find(2, "fulltext", &found, &out).ok());
  EXPECT_TRUE(found);
  ExpectRefEq(out, refs[1]);

  // hit on the empty-suffix entry
  found = false;
  ASSERT_TRUE(reader.find(1, "", &found, &out).ok());
  EXPECT_TRUE(found);
  ExpectRefEq(out, refs[0]);
}

TEST(LogicalIndexDirectory, FindMissByIdAndBySuffix) {
  std::vector<LogicalIndexRef> refs = {
      {1, "", 0, 4096},
      {2, "fulltext", 4096, 8192},
  };
  auto bytes = Build(refs);

  LogicalIndexDirectoryReader reader;
  ASSERT_TRUE(LogicalIndexDirectoryReader::open(Slice(bytes), &reader).ok());

  bool found = true;
  LogicalIndexRef out{};
  // unknown index_id
  ASSERT_TRUE(reader.find(99, "fulltext", &found, &out).ok());
  EXPECT_FALSE(found);

  // known index_id but wrong suffix
  found = true;
  ASSERT_TRUE(reader.find(2, "wrong", &found, &out).ok());
  EXPECT_FALSE(found);

  // known index_id but suffix mismatch (empty vs non-empty)
  found = true;
  ASSERT_TRUE(reader.find(1, "fulltext", &found, &out).ok());
  EXPECT_FALSE(found);
}

TEST(LogicalIndexDirectory, EmptyVersusNonEmptySuffixSameId) {
  // Same index_id, different suffix: both must be addressable distinctly.
  std::vector<LogicalIndexRef> refs = {
      {5, "", 0, 100},
      {5, "tokenized", 100, 200},
  };
  auto bytes = Build(refs);

  LogicalIndexDirectoryReader reader;
  ASSERT_TRUE(LogicalIndexDirectoryReader::open(Slice(bytes), &reader).ok());
  ASSERT_EQ(reader.size(), 2u);

  bool found = false;
  LogicalIndexRef out{};
  ASSERT_TRUE(reader.find(5, "", &found, &out).ok());
  EXPECT_TRUE(found);
  ExpectRefEq(out, refs[0]);

  found = false;
  ASSERT_TRUE(reader.find(5, "tokenized", &found, &out).ok());
  EXPECT_TRUE(found);
  ExpectRefEq(out, refs[1]);
}

TEST(LogicalIndexDirectory, DuplicateIdDifferentSuffix) {
  // Multiple entries share index_id with distinct suffixes (Doris sub-column indexes).
  std::vector<LogicalIndexRef> refs = {
      {10, "a", 0, 10},
      {10, "b", 10, 20},
      {10, "c", 30, 30},
  };
  auto bytes = Build(refs);

  LogicalIndexDirectoryReader reader;
  ASSERT_TRUE(LogicalIndexDirectoryReader::open(Slice(bytes), &reader).ok());
  ASSERT_EQ(reader.size(), 3u);

  for (const auto& want : refs) {
    bool found = false;
    LogicalIndexRef out{};
    ASSERT_TRUE(reader.find(10, want.index_suffix, &found, &out).ok());
    EXPECT_TRUE(found);
    ExpectRefEq(out, want);
  }
}

TEST(LogicalIndexDirectory, EmptyDirectory) {
  std::vector<LogicalIndexRef> refs;  // 0 entries
  auto bytes = Build(refs);

  LogicalIndexDirectoryReader reader;
  ASSERT_TRUE(LogicalIndexDirectoryReader::open(Slice(bytes), &reader).ok());
  EXPECT_EQ(reader.size(), 0u);

  LogicalIndexRef out{};
  EXPECT_EQ(reader.get(0, &out).code(), StatusCode::kNotFound);

  bool found = true;
  ASSERT_TRUE(reader.find(1, "x", &found, &out).ok());
  EXPECT_FALSE(found);
}

TEST(LogicalIndexDirectory, LargeOffsetsRoundTrip) {
  const uint64_t kBig = (1ull << 48) - 1;  // near 2^48
  std::vector<LogicalIndexRef> refs = {
      {0xFFFFFFFFFFFFFFFFull, "edge", kBig, kBig - 1},
      {12345, "", kBig + 99, 1},
  };
  auto bytes = Build(refs);

  LogicalIndexDirectoryReader reader;
  ASSERT_TRUE(LogicalIndexDirectoryReader::open(Slice(bytes), &reader).ok());
  ASSERT_EQ(reader.size(), 2u);
  LogicalIndexRef out0{};
  ASSERT_TRUE(reader.get(0, &out0).ok());
  ExpectRefEq(out0, refs[0]);
  LogicalIndexRef out1{};
  ASSERT_TRUE(reader.get(1, &out1).ok());
  ExpectRefEq(out1, refs[1]);
}

TEST(LogicalIndexDirectory, FramedAsLogicalIndexDirectoryType) {
  std::vector<LogicalIndexRef> refs = {{1, "x", 0, 10}};
  auto bytes = Build(refs);
  ASSERT_GE(bytes.size(), 1u);
  // The first byte is the SectionFramer type and must be kLogicalIndexDirectory.
  EXPECT_EQ(bytes[0], static_cast<uint8_t>(SectionType::kLogicalIndexDirectory));
}

TEST(LogicalIndexDirectory, DetectsCorruption) {
  std::vector<LogicalIndexRef> refs = {
      {1, "fulltext", 0, 4096},
      {2, "phrase", 4096, 8192},
  };
  auto bytes = Build(refs);
  ASSERT_GE(bytes.size(), 4u);
  // Flip one byte in the payload region; the section CRC must detect the corruption.
  bytes[3] ^= 0xFF;
  LogicalIndexDirectoryReader reader;
  EXPECT_EQ(LogicalIndexDirectoryReader::open(Slice(bytes), &reader).code(),
            StatusCode::kCorruption);
}

TEST(LogicalIndexDirectory, DetectsTruncation) {
  std::vector<LogicalIndexRef> refs = {{1, "fulltext", 0, 4096}};
  auto bytes = Build(refs);
  bytes.pop_back();  // truncate the last byte
  LogicalIndexDirectoryReader reader;
  EXPECT_FALSE(LogicalIndexDirectoryReader::open(Slice(bytes), &reader).ok());
}

TEST(LogicalIndexDirectory, WrongSectionTypeRejected) {
  // A section with a type other than kLogicalIndexDirectory must be rejected.
  ByteSink sink;
  const uint8_t p[] = {0, 1, 2};
  SectionFramer::write(sink, static_cast<uint8_t>(SectionType::kXFilter),
                       Slice(p, 3));
  auto bytes = sink.buffer();
  LogicalIndexDirectoryReader reader;
  EXPECT_EQ(LogicalIndexDirectoryReader::open(Slice(bytes), &reader).code(),
            StatusCode::kInvalidArgument);
}

TEST(LogicalIndexDirectory, TrailingBytesRejected) {
  // Extra trailing bytes after the declared entries must be detected.
  ByteSink payload;
  payload.put_varint32(1);   // n_entries = 1
  payload.put_varint64(1);   // index_id
  payload.put_varint32(1);   // suffix_len
  payload.put_u8('a');       // suffix bytes
  payload.put_varint64(0);   // meta_off
  payload.put_varint64(10);  // meta_len
  payload.put_u8(0xEE);      // extra trailing byte
  ByteSink sink;
  SectionFramer::write(sink,
                       static_cast<uint8_t>(SectionType::kLogicalIndexDirectory),
                       payload.view());
  LogicalIndexDirectoryReader reader;
  EXPECT_EQ(LogicalIndexDirectoryReader::open(Slice(sink.buffer()), &reader).code(),
            StatusCode::kCorruption);
}

TEST(LogicalIndexDirectory, OversizedNEntriesRejected) {
  // n_entries declares far more entries than the remaining payload can hold.
  ByteSink payload;
  payload.put_varint32(0xFFFFFFFFu);  // absurd n_entries
  payload.put_varint64(1);            // a tiny bit of real data
  ByteSink sink;
  SectionFramer::write(sink,
                       static_cast<uint8_t>(SectionType::kLogicalIndexDirectory),
                       payload.view());
  LogicalIndexDirectoryReader reader;
  EXPECT_EQ(LogicalIndexDirectoryReader::open(Slice(sink.buffer()), &reader).code(),
            StatusCode::kCorruption);
}

TEST(LogicalIndexDirectory, OversizedSuffixLenRejected) {
  // suffix_len declares more bytes than the payload contains.
  ByteSink payload;
  payload.put_varint32(1);            // n_entries = 1
  payload.put_varint64(1);            // index_id
  payload.put_varint32(0xFFFFFFFFu);  // absurd suffix_len
  payload.put_u8('a');                // only 1 byte present
  ByteSink sink;
  SectionFramer::write(sink,
                       static_cast<uint8_t>(SectionType::kLogicalIndexDirectory),
                       payload.view());
  LogicalIndexDirectoryReader reader;
  EXPECT_EQ(LogicalIndexDirectoryReader::open(Slice(sink.buffer()), &reader).code(),
            StatusCode::kCorruption);
}
