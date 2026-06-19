#include "snii/format/prx_pod.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/byte_source.h"
#include "snii/encoding/crc32c.h"
#include "snii/format/format_constants.h"

using snii::ByteSink;
using snii::ByteSource;
using snii::Slice;
using snii::Status;
using snii::StatusCode;
using snii::format::build_prx_window;
using snii::format::build_prx_window_flat;
using snii::format::read_prx_window;

namespace {

using PerDoc = std::vector<std::vector<uint32_t>>;

// Flattens per-doc lists into (flat positions, freqs) the way the accumulator
// stores them, so the flat builder can be checked for byte-identity.
void Flatten(const PerDoc& in, std::vector<uint32_t>* flat, std::vector<uint32_t>* freqs) {
  flat->clear();
  freqs->clear();
  for (const auto& doc : in) {
    freqs->push_back(static_cast<uint32_t>(doc.size()));
    flat->insert(flat->end(), doc.begin(), doc.end());
  }
}

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

// The FLAT builder (used by the writer to avoid materializing a vector-of-vectors
// for high-df terms) must produce BYTE-IDENTICAL window bytes to the per-doc
// builder for the same logical positions, at every codec level. This is the
// load-bearing guarantee that the flat refactor keeps .prx byte-identical.
TEST(PrxPod, FlatBuilderMatchesPerDocBytes) {
  const std::vector<PerDoc> cases = {
      {},                                    // 0 docs
      {{}, {3}, {}, {}, {1, 2}},             // empty docs interleaved
      {{0, 5, 12}, {1}, {2, 2, 9, 9, 40}},   // duplicate positions (delta 0)
      {{3, 7, 7, 10, 100}},                  // single doc
  };
  for (const auto& in : cases) {
    std::vector<uint32_t> flat, freqs;
    Flatten(in, &flat, &freqs);
    for (int level : {-1, 0, 3}) {
      ByteSink per_doc_sink, flat_sink;
      ASSERT_TRUE(build_prx_window(in, level, &per_doc_sink).ok());
      ASSERT_TRUE(build_prx_window_flat(flat, freqs, level, &flat_sink).ok());
      const Slice a = per_doc_sink.view();
      const Slice b = flat_sink.view();
      ASSERT_EQ(a.size(), b.size()) << "level=" << level;
      EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size())) << "level=" << level;
      // The flat-built window still decodes back to the original per-doc lists.
      PerDoc out;
      ByteSource src(flat_sink.view());
      ASSERT_TRUE(read_prx_window(&src, &out).ok());
      EXPECT_EQ(out, in) << "level=" << level;
    }
  }
}

// A large window built flat (auto codec) must equal the per-doc build AND must
// take the PFOR branch (codec byte == kPfor): the auto path now bit-packs deltas
// instead of zstd, so this proves the flat path is byte-identical through PFOR.
TEST(PrxPod, FlatBuilderMatchesPerDocLargePfor) {
  PerDoc in;
  for (uint32_t d = 0; d < 300; ++d) in.push_back({d, d + 1u, d + 2u});
  std::vector<uint32_t> flat, freqs;
  Flatten(in, &flat, &freqs);
  ByteSink per_doc_sink, flat_sink;
  ASSERT_TRUE(build_prx_window(in, -1, &per_doc_sink).ok());
  ASSERT_TRUE(build_prx_window_flat(flat, freqs, -1, &flat_sink).ok());
  const Slice a = per_doc_sink.view();
  const Slice b = flat_sink.view();
  ASSERT_EQ(a.size(), b.size());
  EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size()));
  EXPECT_EQ(a.data()[0], static_cast<uint8_t>(snii::format::PrxCodec::kPfor));
  // Round-trips losslessly back to the per-doc lists.
  PerDoc out;
  ByteSource src(flat_sink.view());
  ASSERT_TRUE(read_prx_window(&src, &out).ok());
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

// Small window (level<0 auto) now uses PFOR: the auto codec always bit-packs
// deltas (PFOR is cheap and competitive even for small windows; no zstd, no
// size-based raw fallback). codec byte == kPfor and it round-trips.
TEST(PrxPod, SmallWindowUsesPfor) {
  PerDoc in = {{1, 2, 3}, {4, 5}};
  ByteSink sink;
  ASSERT_TRUE(build_prx_window(in, -1, &sink).ok());

  ByteSource src(sink.view());
  uint8_t codec = 0xFF;
  ASSERT_TRUE(src.get_u8(&codec).ok());
  EXPECT_EQ(codec, static_cast<uint8_t>(snii::format::PrxCodec::kPfor));

  PerDoc out;
  ByteSource rt(sink.view());
  ASSERT_TRUE(read_prx_window(&rt, &out).ok());
  EXPECT_EQ(out, in);
}

// Large window (all-1 deltas) auto-encodes as PFOR; for tiny constant deltas the
// 1-bit-packed PFOR payload is much smaller than the forced raw varint encoding.
TEST(PrxPod, LargeWindowTriggersPforAndIsSmaller) {
  PerDoc in;
  in.reserve(64);
  for (int d = 0; d < 64; ++d) {
    std::vector<uint32_t> doc;
    uint32_t p = 0;
    for (int i = 0; i < 256; ++i) {
      p += 1;  // all-1 deltas: pack to ~1 bit each
      doc.push_back(p);
    }
    in.push_back(std::move(doc));
  }

  ByteSink auto_sink;
  ASSERT_TRUE(build_prx_window(in, -1, &auto_sink).ok());
  ByteSource probe(auto_sink.view());
  uint8_t codec = 0xFF;
  ASSERT_TRUE(probe.get_u8(&codec).ok());
  EXPECT_EQ(codec, static_cast<uint8_t>(snii::format::PrxCodec::kPfor));

  ByteSink raw_sink;
  ASSERT_TRUE(build_prx_window(in, /*level=*/0, &raw_sink).ok());
  ByteSource raw_probe(raw_sink.view());
  uint8_t raw_codec = 0xFF;
  ASSERT_TRUE(raw_probe.get_u8(&raw_codec).ok());
  EXPECT_EQ(raw_codec, static_cast<uint8_t>(snii::format::PrxCodec::kRaw));

  EXPECT_LT(auto_sink.size(), raw_sink.size());

  // The PFOR path restores losslessly.
  PerDoc out;
  ByteSource z(auto_sink.view());
  ASSERT_TRUE(read_prx_window(&z, &out).ok());
  EXPECT_EQ(out, in);
}

// PFOR round-trips arbitrary multi-doc windows (including empty docs, duplicate
// positions, and large jumps that force PFOR exceptions) losslessly.
TEST(PrxPod, PforRoundTripVariety) {
  const std::vector<PerDoc> cases = {
      {{0, 5, 12}, {1}, {2, 2, 9, 9, 40}, {7, 8, 9, 10}},
      {{}, {3}, {}, {}, {1, 2}},
      {{0, 1000000, 1000001}, {5}},  // large jump => PFOR exception
      {{3, 7, 7, 10, 100}},          // duplicate positions (delta 0)
  };
  for (const auto& in : cases) {
    ByteSink sink;
    ASSERT_TRUE(build_prx_window(in, -1, &sink).ok());
    ByteSource probe(sink.view());
    uint8_t codec = 0xFF;
    ASSERT_TRUE(probe.get_u8(&codec).ok());
    EXPECT_EQ(codec, static_cast<uint8_t>(snii::format::PrxCodec::kPfor));
    PerDoc out;
    ByteSource src(sink.view());
    ASSERT_TRUE(read_prx_window(&src, &out).ok());
    EXPECT_EQ(out, in);
  }
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

// DoS prevention: a CRC-VALID raw frame whose decoded doc_count is absurd must return
// Corruption, not a giant reserve()/assign() -> std::bad_alloc. Distinct from the CRC
// and uncomp_len tests above, which are caught BEFORE the inner doc_count is read.
TEST(PrxPod, OversizedDocCountRejected) {
  ByteSink payload;
  payload.put_varint32(0x02000000u);  // doc_count = 33M, > kMaxWindowDocs (1<<24)
  ByteSink framed;
  framed.put_u8(static_cast<uint8_t>(snii::format::PrxCodec::kRaw));
  framed.put_varint32(static_cast<uint32_t>(payload.view().size()));  // uncomp_len
  framed.put_bytes(payload.view());
  ByteSink full;
  full.put_bytes(framed.view());
  full.put_fixed32(snii::crc32c(framed.view()));  // valid crc over codec+uncomp_len+payload
  ByteSource src(full.view());
  PerDoc out;
  Status s = read_prx_window(&src, &out);
  EXPECT_EQ(s.code(), StatusCode::kCorruption) << s.message();
}
