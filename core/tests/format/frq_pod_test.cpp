#include "snii/format/frq_pod.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
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
using snii::format::build_frq_window;
using snii::format::read_frq_window;
using snii::format::read_frq_window_docs;

namespace {

using U32Vec = std::vector<uint32_t>;

// Round-trip helper: reconstruct ascending docids relative to win_base and compare against the originals.
Status RoundTrip(const U32Vec& docs, const U32Vec& freqs, uint64_t win_base, bool has_freq,
                 int level, U32Vec* out_docs, U32Vec* out_freqs) {
  ByteSink sink;
  SNII_RETURN_IF_ERROR(
      build_frq_window(docs, freqs, win_base, has_freq, level, &sink));
  ByteSource src(sink.view());
  return read_frq_window(&src, win_base, out_docs, out_freqs);
}

}  // namespace

// Basic round-trip: first window win_base=0, both doc and freq are restored.
TEST(FrqPod, BasicDocFreqRoundTrip) {
  U32Vec docs = {0, 3, 5, 10, 11, 50, 200};
  U32Vec freqs = {1, 2, 1, 7, 3, 1, 9};
  U32Vec out_docs, out_freqs;
  ASSERT_TRUE(RoundTrip(docs, freqs, /*win_base=*/0, /*has_freq=*/true, -1, &out_docs,
                        &out_freqs)
                  .ok());
  EXPECT_EQ(out_docs, docs);
  EXPECT_EQ(out_freqs, freqs);
}

// Non-first window: win_base != 0, dd[0]=first-win_base, cross-window delta rebuild must be correct.
TEST(FrqPod, NonFirstWindowDeltaRebuild) {
  uint64_t win_base = 1000;  // last docid of the previous window
  U32Vec docs = {1001, 1005, 1006, 2000, 2001};
  U32Vec freqs = {3, 1, 1, 2, 8};
  U32Vec out_docs, out_freqs;
  ASSERT_TRUE(RoundTrip(docs, freqs, win_base, /*has_freq=*/true, -1, &out_docs, &out_freqs)
                  .ok());
  EXPECT_EQ(out_docs, docs);
  EXPECT_EQ(out_freqs, freqs);
}

// has_freq=false (docs-only): only docs are read, freq section is empty.
TEST(FrqPod, DocsOnlyNoFreq) {
  U32Vec docs = {0, 1, 2, 3, 100, 101};
  U32Vec freqs;  // empty
  ByteSink sink;
  ASSERT_TRUE(build_frq_window(docs, freqs, /*win_base=*/0, /*has_freq=*/false, -1, &sink)
                  .ok());

  U32Vec out_docs, out_freqs;
  ByteSource src(sink.view());
  ASSERT_TRUE(read_frq_window(&src, /*win_base=*/0, &out_docs, &out_freqs).ok());
  EXPECT_EQ(out_docs, docs);
  EXPECT_TRUE(out_freqs.empty());
}

// bitmap-only path: read_frq_window_docs decodes only dd, skips freq, result is correct.
TEST(FrqPod, BitmapOnlyReadsDocsOnly) {
  uint64_t win_base = 500;
  U32Vec docs = {600, 700, 701, 900};
  U32Vec freqs = {5, 1, 9, 2};
  ByteSink sink;
  ASSERT_TRUE(build_frq_window(docs, freqs, win_base, /*has_freq=*/true, -1, &sink).ok());

  U32Vec out_docs;
  ByteSource src(sink.view());
  ASSERT_TRUE(read_frq_window_docs(&src, win_base, &out_docs).ok());
  EXPECT_EQ(out_docs, docs);
}

// bitmap-only also works on a docs-only window.
TEST(FrqPod, BitmapOnlyOnDocsOnlyWindow) {
  U32Vec docs = {0, 2, 4, 6, 8};
  U32Vec freqs;
  ByteSink sink;
  ASSERT_TRUE(build_frq_window(docs, freqs, /*win_base=*/0, /*has_freq=*/false, -1, &sink)
                  .ok());

  U32Vec out_docs;
  ByteSource src(sink.view());
  ASSERT_TRUE(read_frq_window_docs(&src, /*win_base=*/0, &out_docs).ok());
  EXPECT_EQ(out_docs, docs);
}

// Single-doc window.
TEST(FrqPod, SingleDocWindow) {
  U32Vec docs = {42};
  U32Vec freqs = {7};
  U32Vec out_docs, out_freqs;
  ASSERT_TRUE(RoundTrip(docs, freqs, /*win_base=*/0, /*has_freq=*/true, -1, &out_docs,
                        &out_freqs)
                  .ok());
  EXPECT_EQ(out_docs, docs);
  EXPECT_EQ(out_freqs, freqs);
}

// Large window (2048 docs, multiple 256-runs): auto mode triggers zstd and is still lossless.
TEST(FrqPod, LargeWindowAutoZstdRoundTrip) {
  U32Vec docs;
  U32Vec freqs;
  docs.reserve(2048);
  freqs.reserve(2048);
  uint32_t cur = 0;
  for (int i = 0; i < 2048; ++i) {
    cur += 1 + (i % 4);  // ascending, compressible delta
    docs.push_back(cur);
    freqs.push_back(1 + (i % 3));
  }

  ByteSink auto_sink;
  ASSERT_TRUE(build_frq_window(docs, freqs, /*win_base=*/0, /*has_freq=*/true, -1, &auto_sink)
                  .ok());
  ByteSource probe(auto_sink.view());
  uint8_t win_mode = 0xFF;
  ASSERT_TRUE(probe.get_u8(&win_mode).ok());
  EXPECT_EQ(win_mode, static_cast<uint8_t>(snii::format::FrqWinMode::kZstd));

  U32Vec out_docs, out_freqs;
  ByteSource src(auto_sink.view());
  ASSERT_TRUE(read_frq_window(&src, /*win_base=*/0, &out_docs, &out_freqs).ok());
  EXPECT_EQ(out_docs, docs);
  EXPECT_EQ(out_freqs, freqs);
}

// Small window level<0 auto → raw (win_mode==kRaw, no comp_len).
TEST(FrqPod, SmallWindowUsesRaw) {
  U32Vec docs = {0, 1, 2};
  U32Vec freqs = {1, 1, 1};
  ByteSink sink;
  ASSERT_TRUE(build_frq_window(docs, freqs, /*win_base=*/0, /*has_freq=*/true, -1, &sink)
                  .ok());
  ByteSource src(sink.view());
  uint8_t win_mode = 0xFF;
  ASSERT_TRUE(src.get_u8(&win_mode).ok());
  EXPECT_EQ(win_mode, static_cast<uint8_t>(snii::format::FrqWinMode::kRaw));
}

// Explicit level=0 forces raw: large window is not compressed but still lossless.
TEST(FrqPod, ExplicitRawLargeWindowRoundTrip) {
  U32Vec docs;
  U32Vec freqs;
  uint32_t cur = 0;
  for (int i = 0; i < 600; ++i) {
    cur += 1 + (i % 7);
    docs.push_back(cur);
    freqs.push_back(1 + (i % 5));
  }
  ByteSink sink;
  ASSERT_TRUE(build_frq_window(docs, freqs, /*win_base=*/0, /*has_freq=*/true, /*level=*/0,
                               &sink)
                  .ok());
  ByteSource probe(sink.view());
  uint8_t win_mode = 0xFF;
  ASSERT_TRUE(probe.get_u8(&win_mode).ok());
  EXPECT_EQ(win_mode, static_cast<uint8_t>(snii::format::FrqWinMode::kRaw));

  U32Vec out_docs, out_freqs;
  ByteSource src(sink.view());
  ASSERT_TRUE(read_frq_window(&src, /*win_base=*/0, &out_docs, &out_freqs).ok());
  EXPECT_EQ(out_docs, docs);
  EXPECT_EQ(out_freqs, freqs);
}

// Explicit level>0 forces zstd, still lossless.
TEST(FrqPod, ExplicitZstdRoundTrip) {
  U32Vec docs;
  U32Vec freqs;
  for (uint32_t i = 0; i < 300; ++i) {
    docs.push_back(i * 2);
    freqs.push_back(2);
  }
  U32Vec out_docs, out_freqs;
  ASSERT_TRUE(RoundTrip(docs, freqs, /*win_base=*/0, /*has_freq=*/true, /*level=*/5, &out_docs,
                        &out_freqs)
                  .ok());
  EXPECT_EQ(out_docs, docs);
  EXPECT_EQ(out_freqs, freqs);
}

// Non-ascending docids must be rejected (InvalidArgument).
TEST(FrqPod, NonAscendingDocsRejected) {
  U32Vec docs = {0, 5, 3};  // 3 < 5
  U32Vec freqs = {1, 1, 1};
  ByteSink sink;
  Status s = build_frq_window(docs, freqs, /*win_base=*/0, /*has_freq=*/true, -1, &sink);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

// has_freq=true but freqs length does not match docs → InvalidArgument.
TEST(FrqPod, FreqLengthMismatchRejected) {
  U32Vec docs = {0, 1, 2};
  U32Vec freqs = {1, 2};  // one element short
  ByteSink sink;
  Status s = build_frq_window(docs, freqs, /*win_base=*/0, /*has_freq=*/true, -1, &sink);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

// first_docid < win_base (dd[0] would underflow) → InvalidArgument.
TEST(FrqPod, FirstDocBelowWinBaseRejected) {
  U32Vec docs = {100, 200};
  U32Vec freqs = {1, 1};
  ByteSink sink;
  Status s = build_frq_window(docs, freqs, /*win_base=*/500, /*has_freq=*/true, -1, &sink);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}

// CRC corruption is detectable: flipping the last byte causes read to return Corruption.
TEST(FrqPod, CrcCorruptionDetected) {
  U32Vec docs = {0, 1, 2, 3, 4, 5};
  U32Vec freqs = {1, 2, 3, 4, 5, 6};
  ByteSink sink;
  ASSERT_TRUE(build_frq_window(docs, freqs, /*win_base=*/0, /*has_freq=*/true, -1, &sink)
                  .ok());

  std::vector<uint8_t> bytes = sink.buffer();
  ASSERT_GT(bytes.size(), 1u);
  bytes.back() ^= 0xFF;

  Slice corrupt(bytes);
  ByteSource src(corrupt);
  U32Vec out_docs, out_freqs;
  Status s = read_frq_window(&src, /*win_base=*/0, &out_docs, &out_freqs);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
}

// Invalid win_mode is rejected.
TEST(FrqPod, InvalidWinModeRejected) {
  U32Vec docs = {0, 1};
  U32Vec freqs = {1, 1};
  ByteSink sink;
  ASSERT_TRUE(build_frq_window(docs, freqs, /*win_base=*/0, /*has_freq=*/true, -1, &sink)
                  .ok());
  std::vector<uint8_t> bytes = sink.buffer();
  bytes[0] = 0x7F;  // invalid win_mode

  Slice corrupt(bytes);
  ByteSource src(corrupt);
  U32Vec out_docs, out_freqs;
  Status s = read_frq_window(&src, /*win_base=*/0, &out_docs, &out_freqs);
  EXPECT_FALSE(s.ok());
}

// Truncated input should return an error instead of crashing.
TEST(FrqPod, TruncatedInputRejected) {
  U32Vec docs;
  U32Vec freqs;
  for (uint32_t i = 0; i < 50; ++i) {
    docs.push_back(i);
    freqs.push_back(1);
  }
  ByteSink sink;
  ASSERT_TRUE(build_frq_window(docs, freqs, /*win_base=*/0, /*has_freq=*/true, -1, &sink)
                  .ok());
  std::vector<uint8_t> bytes = sink.buffer();
  bytes.resize(bytes.size() / 2);

  Slice truncated(bytes);
  ByteSource src(truncated);
  U32Vec out_docs, out_freqs;
  Status s = read_frq_window(&src, /*win_base=*/0, &out_docs, &out_freqs);
  EXPECT_FALSE(s.ok());
}

// Empty window (0 docs) round-trip.
TEST(FrqPod, EmptyWindowRoundTrip) {
  U32Vec docs;
  U32Vec freqs;
  U32Vec out_docs, out_freqs;
  ASSERT_TRUE(RoundTrip(docs, freqs, /*win_base=*/0, /*has_freq=*/true, -1, &out_docs,
                        &out_freqs)
                  .ok());
  EXPECT_TRUE(out_docs.empty());
  EXPECT_TRUE(out_freqs.empty());
}

// Null argument guard.
TEST(FrqPod, NullArgsRejected) {
  U32Vec docs = {0, 1};
  U32Vec freqs = {1, 1};
  Status s = build_frq_window(docs, freqs, 0, true, -1, nullptr);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
}
