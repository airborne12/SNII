#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/section_framer.h"
#include "snii/format/format_constants.h"
#include "snii/format/norms_pod.h"

using namespace snii;
using snii::format::NormsPodReader;
using snii::format::NormsPodWriter;

namespace {

// Use writer to encode a sequence of encoded_norms into a framed payload and return the buffer.
std::vector<uint8_t> BuildPod(const std::vector<uint8_t>& norms) {
  NormsPodWriter writer;
  for (uint8_t n : norms) {
    writer.add(n);
  }
  ByteSink sink;
  writer.finish(&sink);
  return sink.buffer();
}

}  // namespace

// After writing N norms, read them back per-doc and verify they match.
TEST(NormsPod, RoundTripValues) {
  std::vector<uint8_t> norms = {0, 1, 7, 42, 128, 200, 255};
  NormsPodWriter writer;
  for (uint8_t n : norms) {
    writer.add(n);
  }
  EXPECT_EQ(writer.count(), norms.size());

  ByteSink sink;
  writer.finish(&sink);

  NormsPodReader reader;
  ASSERT_TRUE(NormsPodReader::open(sink.view(), &reader).ok());
  ASSERT_EQ(reader.doc_count(), norms.size());
  for (uint32_t docid = 0; docid < norms.size(); ++docid) {
    EXPECT_EQ(reader.encoded_norm(docid), norms[docid]) << "docid=" << docid;
  }
}

// Large-scale round-trip covering the multi-byte varint doc_count path.
TEST(NormsPod, RoundTripLarge) {
  std::vector<uint8_t> norms;
  norms.reserve(5000);
  for (uint32_t i = 0; i < 5000; ++i) {
    norms.push_back(static_cast<uint8_t>((i * 31 + 7) & 0xFF));
  }
  auto buf = BuildPod(norms);

  NormsPodReader reader;
  ASSERT_TRUE(NormsPodReader::open(Slice(buf), &reader).ok());
  ASSERT_EQ(reader.doc_count(), 5000u);
  for (uint32_t docid = 0; docid < 5000; ++docid) {
    EXPECT_EQ(reader.encoded_norm(docid), norms[docid]) << "docid=" << docid;
  }
}

// Empty POD: count = 0 is valid and open should succeed.
TEST(NormsPod, EmptyPod) {
  NormsPodWriter writer;
  EXPECT_EQ(writer.count(), 0u);

  ByteSink sink;
  writer.finish(&sink);

  NormsPodReader reader;
  ASSERT_TRUE(NormsPodReader::open(sink.view(), &reader).ok());
  EXPECT_EQ(reader.doc_count(), 0u);
}

// CRC corruption is detectable: flipping a byte in the payload causes open to fail (Corruption).
TEST(NormsPod, DetectsCorruption) {
  std::vector<uint8_t> norms = {10, 20, 30, 40, 50};
  auto buf = BuildPod(norms);
  // Flip a byte near the end (within the norm byte region of the payload, not the framer header).
  buf[buf.size() - 3] ^= 0xFF;

  NormsPodReader reader;
  Status s = NormsPodReader::open(Slice(buf), &reader);
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
}

// Truncated input should return an error rather than crash.
TEST(NormsPod, DetectsTruncation) {
  std::vector<uint8_t> norms = {1, 2, 3, 4, 5, 6, 7, 8};
  auto buf = BuildPod(norms);
  buf.resize(buf.size() - 4);  // Chop off the trailing CRC region.

  NormsPodReader reader;
  Status s = NormsPodReader::open(Slice(buf), &reader);
  EXPECT_FALSE(s.ok());
}

// A mismatch between the declared doc_count and the actual payload byte count should be detected.
TEST(NormsPod, DetectsLengthMismatch) {
  // Manually construct: framer payload = [varint doc_count=4][only 2 norm bytes].
  ByteSink payload;
  payload.put_varint64(4);
  payload.put_u8(11);
  payload.put_u8(22);

  ByteSink sink;
  // Reuse the framer to ensure a self-consistent CRC, specifically to trigger the length-mismatch branch.
  snii::SectionFramer::write(
      sink, static_cast<uint8_t>(snii::format::SectionType::kStatsBlock),
      payload.view());

  NormsPodReader reader;
  Status s = NormsPodReader::open(sink.view(), &reader);
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
}

#ifndef NDEBUG
// In a debug build, an out-of-range docid triggers an assertion (death test).
TEST(NormsPodDeathTest, OutOfRangeDocidAsserts) {
  std::vector<uint8_t> norms = {3, 6, 9};
  auto buf = BuildPod(norms);
  NormsPodReader reader;
  ASSERT_TRUE(NormsPodReader::open(Slice(buf), &reader).ok());
  EXPECT_DEATH({ (void)reader.encoded_norm(3); }, "");
}
#endif

// Checked access: a valid docid returns the value, an out-of-range docid returns InvalidArgument (also effective in Release builds).
TEST(NormsPod, TryEncodedNormChecksBounds) {
  std::vector<uint8_t> norms = {3, 6, 9};
  auto buf = BuildPod(norms);
  NormsPodReader reader;
  ASSERT_TRUE(NormsPodReader::open(Slice(buf), &reader).ok());
  uint8_t v = 0;
  ASSERT_TRUE(reader.try_encoded_norm(1, &v).ok());
  EXPECT_EQ(v, 6u);
  Status s = reader.try_encoded_norm(3, &v);
  EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}
