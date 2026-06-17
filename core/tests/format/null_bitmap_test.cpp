#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/section_framer.h"
#include "snii/format/null_bitmap.h"

using namespace snii;
using snii::format::NullBitmapReader;
using snii::format::NullBitmapWriter;
using snii::format::kNullBitmapSectionType;

namespace {

// Encode a set of null docids into a framed buffer using the writer.
std::vector<uint8_t> BuildBitmap(const std::vector<uint32_t>& nulls,
                                 uint32_t doc_count) {
  NullBitmapWriter writer;
  for (uint32_t d : nulls) {
    writer.add_null(d);
  }
  ByteSink sink;
  writer.finish(doc_count, &sink);
  return sink.buffer();
}

}  // namespace

// After adding nulls, is_null(docid) must match the input set for every doc.
TEST(NullBitmap, RoundTripPerDoc) {
  std::vector<uint32_t> nulls = {0, 3, 7, 11, 100, 4000};
  uint32_t doc_count = 5000;
  auto buf = BuildBitmap(nulls, doc_count);

  NullBitmapReader reader;
  ASSERT_TRUE(NullBitmapReader::open(Slice(buf), &reader).ok());
  EXPECT_EQ(reader.doc_count(), doc_count);
  EXPECT_EQ(reader.null_count(), nulls.size());

  std::vector<bool> expected(doc_count, false);
  for (uint32_t d : nulls) expected[d] = true;
  for (uint32_t docid = 0; docid < doc_count; ++docid) {
    EXPECT_EQ(reader.is_null(docid), expected[docid]) << "docid=" << docid;
  }
}

// Writer null_count reflects the number of distinct null docids added.
TEST(NullBitmap, WriterNullCount) {
  NullBitmapWriter writer;
  EXPECT_EQ(writer.null_count(), 0u);
  writer.add_null(5);
  writer.add_null(9);
  writer.add_null(5);  // duplicate is idempotent in a set
  EXPECT_EQ(writer.null_count(), 2u);
}

// Empty bitmap: no nulls. open succeeds, null_count == 0, nothing is null.
TEST(NullBitmap, EmptyNoNulls) {
  auto buf = BuildBitmap({}, 1000);

  NullBitmapReader reader;
  ASSERT_TRUE(NullBitmapReader::open(Slice(buf), &reader).ok());
  EXPECT_EQ(reader.doc_count(), 1000u);
  EXPECT_EQ(reader.null_count(), 0u);
  EXPECT_FALSE(reader.is_null(0));
  EXPECT_FALSE(reader.is_null(999));
}

// All-null bitmap: every doc in [0, doc_count) is null.
TEST(NullBitmap, AllNull) {
  uint32_t doc_count = 256;
  std::vector<uint32_t> nulls;
  for (uint32_t d = 0; d < doc_count; ++d) nulls.push_back(d);
  auto buf = BuildBitmap(nulls, doc_count);

  NullBitmapReader reader;
  ASSERT_TRUE(NullBitmapReader::open(Slice(buf), &reader).ok());
  EXPECT_EQ(reader.null_count(), doc_count);
  for (uint32_t docid = 0; docid < doc_count; ++docid) {
    EXPECT_TRUE(reader.is_null(docid)) << "docid=" << docid;
  }
}

// doc_count round-trips even when there are no nulls and a large doc_count.
TEST(NullBitmap, DocCountRoundTrips) {
  auto buf = BuildBitmap({42}, 1234567);

  NullBitmapReader reader;
  ASSERT_TRUE(NullBitmapReader::open(Slice(buf), &reader).ok());
  EXPECT_EQ(reader.doc_count(), 1234567u);
  EXPECT_TRUE(reader.is_null(42));
}

// is_null beyond doc_count is false (docid not in the null set).
TEST(NullBitmap, IsNullOutsideRangeIsFalse) {
  auto buf = BuildBitmap({1, 2, 3}, 10);

  NullBitmapReader reader;
  ASSERT_TRUE(NullBitmapReader::open(Slice(buf), &reader).ok());
  EXPECT_FALSE(reader.is_null(10));
  EXPECT_FALSE(reader.is_null(1000000));
}

// CRC corruption is detectable: flipping a payload byte fails open with Corruption.
TEST(NullBitmap, DetectsCorruption) {
  std::vector<uint32_t> nulls = {2, 4, 6, 8, 10, 12, 14};
  auto buf = BuildBitmap(nulls, 100);
  // Flip a byte inside the roaring payload region (near the end, before the trailing CRC).
  buf[buf.size() - 5] ^= 0xFF;

  NullBitmapReader reader;
  Status s = NullBitmapReader::open(Slice(buf), &reader);
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
}

// Truncated input returns an error rather than crashing.
TEST(NullBitmap, DetectsTruncation) {
  auto buf = BuildBitmap({1, 2, 3, 4, 5}, 100);
  buf.resize(buf.size() - 4);  // chop trailing CRC region

  NullBitmapReader reader;
  Status s = NullBitmapReader::open(Slice(buf), &reader);
  EXPECT_FALSE(s.ok());
}

// An oversized declared roaring_size (larger than the remaining payload bytes) is rejected (anti-DoS).
TEST(NullBitmap, RejectsOversizedRoaringSize) {
  // Manually construct a self-consistent frame whose declared roaring_size
  // exceeds the bytes actually present, to drive the guard branch.
  ByteSink payload;
  payload.put_varint64(100);            // doc_count
  payload.put_varint64(0xFFFFFFFFull);  // roaring_size: absurdly large
  payload.put_u8(0x00);                 // only 1 byte of roaring data present

  ByteSink sink;
  SectionFramer::write(sink, kNullBitmapSectionType, payload.view());

  NullBitmapReader reader;
  Status s = NullBitmapReader::open(sink.view(), &reader);
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
}

// doc_count overflowing uint32 is rejected.
TEST(NullBitmap, RejectsDocCountOverflow) {
  ByteSink payload;
  payload.put_varint64(0x1'0000'0000ull);  // doc_count > uint32 max
  payload.put_varint64(0);                  // roaring_size

  ByteSink sink;
  SectionFramer::write(sink, kNullBitmapSectionType, payload.view());

  NullBitmapReader reader;
  Status s = NullBitmapReader::open(sink.view(), &reader);
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
}

// A CRC-valid frame carrying malformed roaring container bytes must be rejected
// gracefully (Corruption) without throwing or aborting. The roaring bytes are not
// a valid portable serialization; SectionFramer stamps a correct crc so the framer
// check passes and the roaring pre-validation (deserialize_size probe) must catch it.
TEST(NullBitmap, RejectsMalformedRoaringContainer) {
  ByteSink payload;
  payload.put_varint64(10);  // doc_count
  payload.put_varint64(4);   // roaring_size
  const uint8_t garbage[] = {0xFF, 0xFF, 0xFF, 0xFF};  // invalid roaring cookie
  payload.put_bytes(Slice(garbage, 4));

  ByteSink sink;
  SectionFramer::write(sink, kNullBitmapSectionType, payload.view());

  NullBitmapReader reader;
  Status s = NullBitmapReader::open(sink.view(), &reader);
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
}
