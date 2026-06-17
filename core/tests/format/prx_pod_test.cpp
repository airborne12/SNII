#include "snii/format/prx_pod.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/byte_source.h"
#include "snii/format/format_constants.h"

using snii::ByteSink;
using snii::ByteSource;
using snii::Slice;
using snii::Status;
using snii::StatusCode;
using snii::format::build_prx_window;
using snii::format::read_prx_window;

namespace {

using PerDoc = std::vector<std::vector<uint32_t>>;

// Build data above/below the raw threshold consistent with the production path; provides a stable, controlled round-trip helper.
Status RoundTrip(const PerDoc& in, int level, PerDoc* out) {
  ByteSink sink;
  SNII_RETURN_IF_ERROR(build_prx_window(in, level, &sink));
  ByteSource src(sink.view());
  SNII_RETURN_IF_ERROR(read_prx_window(&src, out));
  return Status::OK();
}

}  // namespace

// Single doc with ascending positions: delta-encoded round-trip must be lossless.
TEST(PrxPod, SingleDocRoundTrip) {
  PerDoc in = {{3, 7, 7, 10, 100}};  // includes duplicate positions (delta=0)
  PerDoc out;
  ASSERT_TRUE(RoundTrip(in, -1, &out).ok());
  EXPECT_EQ(out, in);
}

// Multiple docs, positions ascending within each doc, no shared baseline across docs.
TEST(PrxPod, MultiDocRoundTrip) {
  PerDoc in = {{0, 5, 12}, {1}, {2, 2, 9, 9, 40}, {7, 8, 9, 10}};
  PerDoc out;
  ASSERT_TRUE(RoundTrip(in, -1, &out).ok());
  EXPECT_EQ(out, in);
}

// Empty positions: supports both 0 docs and 0 positions within a doc.
TEST(PrxPod, EmptyPositions) {
  PerDoc empty_window;  // 0 docs
  PerDoc out1;
  ASSERT_TRUE(RoundTrip(empty_window, -1, &out1).ok());
  EXPECT_EQ(out1, empty_window);

  PerDoc with_empty_docs = {{}, {3}, {}, {}, {1, 2}};  // contains empty docs
  PerDoc out2;
  ASSERT_TRUE(RoundTrip(with_empty_docs, -1, &out2).ok());
  EXPECT_EQ(out2, with_empty_docs);
}

// Small window (level<0 auto) should use raw: codec byte == kRaw, and no comp_len.
TEST(PrxPod, SmallWindowUsesRaw) {
  PerDoc in = {{1, 2, 3}, {4, 5}};
  ByteSink sink;
  ASSERT_TRUE(build_prx_window(in, -1, &sink).ok());

  ByteSource src(sink.view());
  uint8_t codec = 0xFF;
  ASSERT_TRUE(src.get_u8(&codec).ok());
  EXPECT_EQ(codec, static_cast<uint8_t>(snii::format::PrxCodec::kRaw));
}

// Large window (highly compressible) automatically triggers zstd, and total bytes are smaller than forced raw encoding.
TEST(PrxPod, LargeWindowTriggersZstdAndIsSmaller) {
  PerDoc in;
  in.reserve(64);
  for (int d = 0; d < 64; ++d) {
    std::vector<uint32_t> doc;
    uint32_t p = 0;
    for (int i = 0; i < 256; ++i) {
      p += 1;  // all-1 deltas: highly compressible
      doc.push_back(p);
    }
    in.push_back(std::move(doc));
  }

  ByteSink auto_sink;
  ASSERT_TRUE(build_prx_window(in, -1, &auto_sink).ok());
  ByteSource probe(auto_sink.view());
  uint8_t codec = 0xFF;
  ASSERT_TRUE(probe.get_u8(&codec).ok());
  EXPECT_EQ(codec, static_cast<uint8_t>(snii::format::PrxCodec::kZstd));

  // Force raw (level cannot be negative, but using a very large threshold that avoids compression is impractical; compare directly against the raw path instead).
  ByteSink raw_sink;
  ASSERT_TRUE(build_prx_window(in, /*level=*/0, &raw_sink).ok());
  ByteSource raw_probe(raw_sink.view());
  uint8_t raw_codec = 0xFF;
  ASSERT_TRUE(raw_probe.get_u8(&raw_codec).ok());
  EXPECT_EQ(raw_codec, static_cast<uint8_t>(snii::format::PrxCodec::kRaw));

  EXPECT_LT(auto_sink.size(), raw_sink.size());

  // The compressed path can still be restored losslessly.
  PerDoc out;
  ByteSource z(auto_sink.view());
  ASSERT_TRUE(read_prx_window(&z, &out).ok());
  EXPECT_EQ(out, in);
}

// level=0 explicitly forces raw (no compression), useful for testing and the oversized single-doc degenerate path.
TEST(PrxPod, ExplicitRawLevelRoundTrip) {
  PerDoc in = {{10, 20, 30}, {40}};
  PerDoc out;
  ASSERT_TRUE(RoundTrip(in, /*level=*/0, &out).ok());
  EXPECT_EQ(out, in);
}

// CRC corruption is detectable: flipping any byte in the payload causes read to return Corruption.
TEST(PrxPod, CrcCorruptionDetected) {
  PerDoc in = {{1, 2, 3, 4, 5}, {6, 7, 8}};
  ByteSink sink;
  ASSERT_TRUE(build_prx_window(in, -1, &sink).ok());

  std::vector<uint8_t> bytes = sink.buffer();
  ASSERT_GT(bytes.size(), 1u);
  bytes.back() ^= 0xFF;  // corrupt the last byte of the payload

  Slice corrupt(bytes);
  ByteSource src(corrupt);
  PerDoc out;
  Status s = read_prx_window(&src, &out);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
}

// A corrupted codec byte (invalid codec value) should also be rejected.
TEST(PrxPod, InvalidCodecRejected) {
  PerDoc in = {{1, 2}};
  ByteSink sink;
  ASSERT_TRUE(build_prx_window(in, -1, &sink).ok());

  std::vector<uint8_t> bytes = sink.buffer();
  bytes[0] = 0x7F;  // invalid codec (bits 0-5 exceed known values)

  Slice corrupt(bytes);
  ByteSource src(corrupt);
  PerDoc out;
  Status s = read_prx_window(&src, &out);
  EXPECT_FALSE(s.ok());
}

// Truncated input (insufficient payload) should return an error rather than crash.
TEST(PrxPod, TruncatedInputRejected) {
  PerDoc in = {{1, 2, 3, 4, 5, 6, 7, 8}};
  ByteSink sink;
  ASSERT_TRUE(build_prx_window(in, -1, &sink).ok());

  std::vector<uint8_t> bytes = sink.buffer();
  bytes.resize(bytes.size() / 2);  // truncate to half

  Slice truncated(bytes);
  ByteSource src(truncated);
  PerDoc out;
  Status s = read_prx_window(&src, &out);
  EXPECT_FALSE(s.ok());
}

// DoS prevention: an uncomp_len corrupted to a huge value must be rejected before allocation/decompression.
TEST(PrxPod, OversizedUncompLenRejected) {
  ByteSink sink;
  sink.put_u8(static_cast<uint8_t>(snii::format::PrxCodec::kZstd));
  sink.put_varint32(300u * 1024 * 1024);  // > 256MiB window limit
  // No need to construct the subsequent comp_len/payload/crc — the cap check triggers immediately after reading uncomp_len.
  ByteSource src(sink.view());
  PerDoc out;
  Status s = read_prx_window(&src, &out);
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
}
