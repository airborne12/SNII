#include "snii/format/frq_prelude.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <iterator>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/crc32c.h"

using snii::ByteSink;
using snii::crc32c;
using snii::Slice;
using snii::Status;
using snii::StatusCode;
using snii::format::build_frq_prelude;
using snii::format::FrqPreludeColumns;
using snii::format::FrqPreludeReader;
using snii::format::WindowMeta;

namespace {

// Builds N windows that tile a docid space in fixed strides, with deterministic
// per-window metadata. doc_count is `stride`; absolute last docid of window w is
// (w+1)*stride - 1 so windows are contiguous and strictly ascending.
FrqPreludeColumns MakeColumns(uint32_t n, uint32_t group_size, uint32_t stride,
                              bool has_prx) {
  FrqPreludeColumns cols;
  cols.has_freq = true;
  cols.has_prx = has_prx;
  cols.group_size = group_size;
  uint64_t frq_running = 0;
  uint64_t prx_running = 0;
  for (uint32_t w = 0; w < n; ++w) {
    WindowMeta m;
    m.last_docid = (w + 1) * stride - 1;
    m.doc_count = stride;
    m.frq_off = frq_running;
    m.frq_len = 10 + w;  // arbitrary but unique-ish
    m.frq_docs_len = 6 + (w % 4);  // docs-only prefix, always <= frq_len
    frq_running += m.frq_len;
    if (has_prx) {
      m.prx_off = prx_running;
      m.prx_len = 7 + (w % 5);
      prx_running += m.prx_len;
    }
    m.max_freq = w * 3 + 1;
    m.max_norm = static_cast<uint8_t>((w * 13 + 3) & 0xFF);
    m.win_crc = 0xCAFE0000u + w;
    cols.windows.push_back(m);
  }
  return cols;
}

// Round-trips columns through build + open, asserting every window's absolute
// fields match (and win_base chains correctly).
void ExpectRoundTrip(const FrqPreludeColumns& cols) {
  ByteSink sink;
  ASSERT_TRUE(build_frq_prelude(cols, &sink).ok());

  FrqPreludeReader reader;
  ASSERT_TRUE(FrqPreludeReader::open(sink.view(), &reader).ok());

  ASSERT_EQ(reader.n_windows(), cols.windows.size());
  EXPECT_EQ(reader.has_freq(), cols.has_freq);
  EXPECT_EQ(reader.has_prx(), cols.has_prx);

  uint64_t expect_win_base = 0;
  for (uint32_t w = 0; w < reader.n_windows(); ++w) {
    WindowMeta got;
    ASSERT_TRUE(reader.window(w, &got).ok()) << "window w=" << w;
    const WindowMeta& exp = cols.windows[w];
    EXPECT_EQ(got.last_docid, exp.last_docid) << "ldd w=" << w;
    EXPECT_EQ(got.win_base, expect_win_base) << "win_base w=" << w;
    EXPECT_EQ(got.doc_count, exp.doc_count) << "doc_count w=" << w;
    EXPECT_EQ(got.frq_off, exp.frq_off) << "frq_off w=" << w;
    EXPECT_EQ(got.frq_len, exp.frq_len) << "frq_len w=" << w;
    EXPECT_EQ(got.frq_docs_len, exp.frq_docs_len) << "frq_docs_len w=" << w;
    EXPECT_EQ(got.max_freq, exp.max_freq) << "max_freq w=" << w;
    EXPECT_EQ(got.max_norm, exp.max_norm) << "max_norm w=" << w;
    EXPECT_EQ(got.win_crc, exp.win_crc) << "win_crc w=" << w;
    if (cols.has_prx) {
      EXPECT_EQ(got.prx_off, exp.prx_off) << "prx_off w=" << w;
      EXPECT_EQ(got.prx_len, exp.prx_len) << "prx_len w=" << w;
    }
    expect_win_base = exp.last_docid;
  }
}

}  // namespace

// Many windows across multiple super-blocks, no prx.
TEST(FrqPrelude, RoundTripManyWindowsNoPrx) {
  ExpectRoundTrip(MakeColumns(/*n=*/20, /*group_size=*/4, /*stride=*/256, false));
}

// Many windows across multiple super-blocks, with prx.
TEST(FrqPrelude, RoundTripManyWindowsWithPrx) {
  ExpectRoundTrip(MakeColumns(/*n=*/17, /*group_size=*/4, /*stride=*/256, true));
}

// Single window: N=1 collapses to a single super-block.
TEST(FrqPrelude, SingleWindow) {
  FrqPreludeColumns cols = MakeColumns(/*n=*/1, /*group_size=*/64, /*stride=*/300, true);
  ByteSink sink;
  ASSERT_TRUE(build_frq_prelude(cols, &sink).ok());
  FrqPreludeReader reader;
  ASSERT_TRUE(FrqPreludeReader::open(sink.view(), &reader).ok());
  EXPECT_EQ(reader.n_windows(), 1u);
  EXPECT_EQ(reader.n_super_blocks(), 1u);
  WindowMeta m;
  ASSERT_TRUE(reader.window(0, &m).ok());
  EXPECT_EQ(m.win_base, 0u);
  EXPECT_EQ(m.last_docid, 299u);
}

// N exactly == group_size -> exactly one super-block.
TEST(FrqPrelude, ExactlyOneSuperBlock) {
  FrqPreludeColumns cols = MakeColumns(/*n=*/4, /*group_size=*/4, /*stride=*/256, true);
  ByteSink sink;
  ASSERT_TRUE(build_frq_prelude(cols, &sink).ok());
  FrqPreludeReader reader;
  ASSERT_TRUE(FrqPreludeReader::open(sink.view(), &reader).ok());
  EXPECT_EQ(reader.n_super_blocks(), 1u);
}

// Large N exercises many super-blocks and varint streams.
TEST(FrqPrelude, LargeN) {
  ExpectRoundTrip(MakeColumns(/*n=*/1000, /*group_size=*/64, /*stride=*/256, true));
}

// locate_window finds the covering window at boundaries, inside, and past-end.
TEST(FrqPrelude, LocateWindowBoundaries) {
  // 10 windows, stride 100 -> window w covers docids [w*100, w*100+99].
  FrqPreludeColumns cols = MakeColumns(/*n=*/10, /*group_size=*/3, /*stride=*/100, true);
  ByteSink sink;
  ASSERT_TRUE(build_frq_prelude(cols, &sink).ok());
  FrqPreludeReader reader;
  ASSERT_TRUE(FrqPreludeReader::open(sink.view(), &reader).ok());

  struct Case {
    uint32_t docid;
    bool found;
    uint32_t w;
  };
  const Case cases[] = {
      {0, true, 0},      // first docid of window 0
      {99, true, 0},     // last docid of window 0
      {100, true, 1},    // first docid of window 1 (boundary)
      {250, true, 2},    // inside window 2
      {299, true, 2},    // last docid of window 2
      {300, true, 3},    // boundary into window 3
      {999, true, 9},    // last docid overall (window 9 covers [900,999])
      {1000, false, 0},  // past the term's last docid
      {5000, false, 0},  // far past end
  };
  for (const auto& c : cases) {
    bool found = false;
    uint32_t w = 999;
    ASSERT_TRUE(reader.locate_window(c.docid, &found, &w).ok()) << "docid=" << c.docid;
    EXPECT_EQ(found, c.found) << "docid=" << c.docid;
    if (c.found) { EXPECT_EQ(w, c.w) << "docid=" << c.docid; }
  }
}

// locate_window must agree with a linear scan over windows for every docid in a
// gappy docid layout (windows not contiguous: gaps between windows).
TEST(FrqPrelude, LocateWindowAgainstLinearScan) {
  FrqPreludeColumns cols;
  cols.has_prx = true;
  cols.group_size = 4;
  // Non-contiguous absolute last docids with gaps: 50, 130, 131, 400, 900.
  const uint32_t lasts[] = {50, 130, 131, 400, 900};
  uint64_t frq_running = 0, prx_running = 0;
  for (uint32_t v : lasts) {
    WindowMeta m;
    m.last_docid = v;
    m.doc_count = 5;
    m.frq_off = frq_running;
    m.frq_len = 12;
    frq_running += 12;
    m.prx_off = prx_running;
    m.prx_len = 6;
    prx_running += 6;
    m.win_crc = v;
    cols.windows.push_back(m);
  }
  ByteSink sink;
  ASSERT_TRUE(build_frq_prelude(cols, &sink).ok());
  FrqPreludeReader reader;
  ASSERT_TRUE(FrqPreludeReader::open(sink.view(), &reader).ok());

  for (uint32_t docid = 0; docid <= 950; ++docid) {
    // Linear oracle: first window whose absolute last_docid >= docid.
    bool exp_found = false;
    uint32_t exp_w = 0;
    for (uint32_t w = 0; w < std::size(lasts); ++w) {
      if (docid <= lasts[w]) {
        exp_found = true;
        exp_w = w;
        break;
      }
    }
    bool found = false;
    uint32_t w = 999;
    ASSERT_TRUE(reader.locate_window(docid, &found, &w).ok());
    EXPECT_EQ(found, exp_found) << "docid=" << docid;
    if (exp_found) { EXPECT_EQ(w, exp_w) << "docid=" << docid; }
  }
}

// Builder rejects a null sink.
TEST(FrqPrelude, BuildNullSinkRejected) {
  FrqPreludeColumns cols = MakeColumns(3, 4, 100, false);
  EXPECT_EQ(build_frq_prelude(cols, nullptr).code(), StatusCode::kInvalidArgument);
}

// Builder rejects group_size == 0.
TEST(FrqPrelude, BuildZeroGroupSizeRejected) {
  FrqPreludeColumns cols = MakeColumns(3, 4, 100, false);
  cols.group_size = 0;
  ByteSink sink;
  EXPECT_EQ(build_frq_prelude(cols, &sink).code(), StatusCode::kInvalidArgument);
}

// Builder rejects non-monotonic last_docid ordering across windows.
TEST(FrqPrelude, BuildNonMonotonicRejected) {
  FrqPreludeColumns cols = MakeColumns(3, 4, 100, false);
  cols.windows[2].last_docid = cols.windows[1].last_docid - 1;  // goes backwards
  ByteSink sink;
  EXPECT_EQ(build_frq_prelude(cols, &sink).code(), StatusCode::kInvalidArgument);
}

// A window row whose frq_docs_len exceeds frq_len is rejected on open (anti-DoS:
// the docs-only prefix can never be longer than the full window).
TEST(FrqPrelude, FrqDocsLenExceedsFrqLenRejected) {
  FrqPreludeColumns cols = MakeColumns(/*n=*/5, /*group_size=*/4, /*stride=*/256, false);
  cols.windows[2].frq_docs_len = cols.windows[2].frq_len + 100;  // impossible prefix
  ByteSink sink;
  ASSERT_TRUE(build_frq_prelude(cols, &sink).ok());  // builder does not validate
  FrqPreludeReader reader;
  Status s = FrqPreludeReader::open(sink.view(), &reader);
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
}

// CRC corruption inside the header / super_block_dir is detected by open().
TEST(FrqPrelude, CrcCorruptionDetected) {
  FrqPreludeColumns cols = MakeColumns(8, 4, 256, true);
  ByteSink sink;
  ASSERT_TRUE(build_frq_prelude(cols, &sink).ok());
  std::vector<uint8_t> bytes = sink.buffer();
  ASSERT_GE(bytes.size(), 4u);
  bytes[1] ^= 0xFF;  // flip a flags/header byte
  FrqPreludeReader reader;
  Status s = FrqPreludeReader::open(Slice(bytes), &reader);
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
}

// Truncated buffer (chopping into a window block) is rejected.
TEST(FrqPrelude, TruncatedRejected) {
  FrqPreludeColumns cols = MakeColumns(8, 4, 256, true);
  ByteSink sink;
  ASSERT_TRUE(build_frq_prelude(cols, &sink).ok());
  std::vector<uint8_t> bytes = sink.buffer();
  bytes.resize(bytes.size() - 5);  // drop tail bytes of the last window block
  FrqPreludeReader reader;
  Status s = FrqPreludeReader::open(Slice(bytes), &reader);
  EXPECT_FALSE(s.ok());
}

// Out-of-range window() returns InvalidArgument (no OOB read).
TEST(FrqPrelude, WindowOutOfRangeRejected) {
  FrqPreludeColumns cols = MakeColumns(3, 4, 100, true);
  ByteSink sink;
  ASSERT_TRUE(build_frq_prelude(cols, &sink).ok());
  FrqPreludeReader reader;
  ASSERT_TRUE(FrqPreludeReader::open(sink.view(), &reader).ok());
  WindowMeta m;
  EXPECT_EQ(reader.window(99, &m).code(), StatusCode::kInvalidArgument);
}

// Anti-DoS: a crc-valid header declaring an absurd window count N must be
// rejected before any reserve(N). We craft header + super_block_dir bytes by
// hand with a matching crc, so verify_crc passes but the count is rejected.
TEST(FrqPrelude, RejectsOversizedWindowCount) {
  ByteSink covered;
  covered.put_u8(snii::format::frq_prelude_flags::kHasFreq);
  covered.put_varint64(0xFFFFFFFFull);  // N: absurd window count
  covered.put_varint64(64);             // G
  covered.put_varint64(1);              // n_super (bogus but small)
  covered.put_varint64(0);              // sbdir_len = 0
  ByteSink frame;
  frame.put_bytes(covered.view());
  frame.put_fixed32(crc32c(covered.view()));
  FrqPreludeReader reader;
  Status s = FrqPreludeReader::open(frame.view(), &reader);
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
}
