#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/crc32c.h"
#include "snii/format/dict_block.h"
#include "snii/format/dict_entry.h"
#include "snii/format/format_constants.h"

using namespace snii;          // NOLINT
using namespace snii::format;  // NOLINT

namespace {

// 构造一个 pod_ref slim 词典项。frq/prx off_delta 已相对 block base 计算。
DictEntry MakePodRef(std::string term, uint32_t df, uint64_t frq_off,
                     uint64_t prx_off = 0) {
  DictEntry e;
  e.term = std::move(term);
  e.kind = DictEntryKind::kPodRef;
  e.enc = DictEntryEnc::kSlim;
  e.df = df;
  e.ttf_delta = df * 2;  // 仅 tier>=T2 写出
  e.max_freq = 9;        // 仅 tier>=T2 写出
  e.frq_off_delta = frq_off;
  e.frq_len = 128;
  e.prx_off_delta = prx_off;  // 仅 positions 写出
  e.prx_len = 64;             // 仅 positions 写出
  return e;
}

DictEntry MakeInline(std::string term, uint32_t df) {
  DictEntry e;
  e.term = std::move(term);
  e.kind = DictEntryKind::kInline;
  e.enc = DictEntryEnc::kSlim;
  e.df = df;
  e.frq_bytes = {0x10, 0x20, 0x30};
  return e;
}

void ExpectCommon(const DictEntry& a, const DictEntry& b) {
  EXPECT_EQ(a.term, b.term);
  EXPECT_EQ(a.kind, b.kind);
  EXPECT_EQ(a.enc, b.enc);
  EXPECT_EQ(a.df, b.df);
}

// 用 builder 序列化一组 entries 到一个 block，返回字节缓冲。
std::vector<uint8_t> BuildBlock(const std::vector<DictEntry>& entries,
                                IndexTier tier, bool has_positions,
                                uint64_t frq_base, uint64_t prx_base,
                                uint32_t anchor_interval) {
  DictBlockBuilder builder(tier, has_positions, frq_base, prx_base,
                           anchor_interval);
  for (const auto& e : entries) builder.add_entry(e);
  ByteSink sink;
  builder.finish(&sink);
  return sink.buffer();
}

}  // namespace

// 空 block：n_entries=0，仍可 open，find_term 任何 target 均未命中。
TEST(DictBlock, EmptyBlock) {
  std::vector<uint8_t> bytes =
      BuildBlock({}, IndexTier::kT1, /*has_positions=*/false, 1000, 0, 16);
  DictBlockReader reader;
  ASSERT_TRUE(DictBlockReader::open(Slice(bytes), IndexTier::kT1,
                                    /*has_positions=*/false, &reader)
                  .ok());
  EXPECT_EQ(reader.n_entries(), 0u);
  EXPECT_EQ(reader.frq_base(), 1000u);

  bool found = true;
  DictEntry out;
  ASSERT_TRUE(reader.find_term("anything", &found, &out).ok());
  EXPECT_FALSE(found);
}

// 单 entry round-trip。
TEST(DictBlock, SingleEntryRoundTrip) {
  DictEntry e = MakePodRef("solo", 7, 0);
  std::vector<uint8_t> bytes =
      BuildBlock({e}, IndexTier::kT1, false, 4096, 0, 16);
  DictBlockReader reader;
  ASSERT_TRUE(
      DictBlockReader::open(Slice(bytes), IndexTier::kT1, false, &reader).ok());
  EXPECT_EQ(reader.n_entries(), 1u);
  EXPECT_EQ(reader.frq_base(), 4096u);

  bool found = false;
  DictEntry out;
  ASSERT_TRUE(reader.find_term("solo", &found, &out).ok());
  ASSERT_TRUE(found);
  ExpectCommon(e, out);
  EXPECT_EQ(out.frq_off_delta, e.frq_off_delta);
  EXPECT_EQ(out.frq_len, e.frq_len);
}

// 多 entry round-trip：全部 term 命中，字段保留。
TEST(DictBlock, MultiEntryRoundTrip) {
  std::vector<DictEntry> entries = {
      MakePodRef("alpha", 3, 0),  MakePodRef("beta", 5, 100),
      MakeInline("gamma", 2),     MakePodRef("delta", 9, 300),
      MakePodRef("epsilon", 11, 500)};
  // delta < gamma 字典序不对，重排为有序。
  entries = {MakePodRef("alpha", 3, 0), MakePodRef("beta", 5, 100),
             MakePodRef("delta", 9, 300), MakePodRef("epsilon", 11, 500),
             MakeInline("gamma", 2)};
  std::vector<uint8_t> bytes =
      BuildBlock(entries, IndexTier::kT2, true, 8192, 16384, 16);
  DictBlockReader reader;
  ASSERT_TRUE(
      DictBlockReader::open(Slice(bytes), IndexTier::kT2, true, &reader).ok());
  EXPECT_EQ(reader.n_entries(), entries.size());

  for (const auto& e : entries) {
    bool found = false;
    DictEntry out;
    ASSERT_TRUE(reader.find_term(e.term, &found, &out).ok()) << e.term;
    ASSERT_TRUE(found) << e.term;
    ExpectCommon(e, out);
  }
}

// 前缀压缩跨锚点：anchor_interval 较小时，跨锚点的 term 仍能正确恢复。
TEST(DictBlock, PrefixCompressionAcrossAnchors) {
  std::vector<DictEntry> entries;
  // 21 个共享长前缀的有序 term，anchor_interval=4 → 多个锚点。
  std::vector<std::string> terms = {
      "interest",    "interested",  "interesting", "interestingly",
      "interests",   "internal",    "internally",  "international",
      "internet",    "internets",   "interplay",   "interpose",
      "interpret",   "interpreted", "interval",    "intervene",
      "interview",   "interviewed", "intestine",   "intimate",
      "intricate"};
  uint64_t off = 0;
  for (const auto& t : terms) {
    entries.push_back(MakePodRef(t, 4, off));
    off += 50;
  }
  std::vector<uint8_t> bytes =
      BuildBlock(entries, IndexTier::kT1, false, 0, 0, /*anchor_interval=*/4);
  DictBlockReader reader;
  ASSERT_TRUE(
      DictBlockReader::open(Slice(bytes), IndexTier::kT1, false, &reader).ok());
  EXPECT_EQ(reader.n_entries(), terms.size());

  for (size_t i = 0; i < terms.size(); ++i) {
    bool found = false;
    DictEntry out;
    ASSERT_TRUE(reader.find_term(terms[i], &found, &out).ok()) << terms[i];
    ASSERT_TRUE(found) << terms[i];
    EXPECT_EQ(out.term, terms[i]);
    EXPECT_EQ(out.frq_off_delta, static_cast<uint64_t>(i * 50));
  }
}

// find_term 边界：小于首 term、大于末 term、恰为锚点 term。
TEST(DictBlock, FindTermBoundaries) {
  std::vector<std::string> terms;
  for (int i = 0; i < 40; ++i) {
    char buf[8];
    snprintf(buf, sizeof(buf), "t%02d", i);
    terms.emplace_back(buf);
  }
  std::vector<DictEntry> entries;
  for (size_t i = 0; i < terms.size(); ++i) {
    entries.push_back(MakePodRef(terms[i], 1, i * 10));
  }
  std::vector<uint8_t> bytes =
      BuildBlock(entries, IndexTier::kT1, false, 0, 0, 8);
  DictBlockReader reader;
  ASSERT_TRUE(
      DictBlockReader::open(Slice(bytes), IndexTier::kT1, false, &reader).ok());

  // 小于首 term。
  {
    bool found = true;
    DictEntry out;
    ASSERT_TRUE(reader.find_term("\x01", &found, &out).ok());
    EXPECT_FALSE(found);
  }
  // 大于末 term。
  {
    bool found = true;
    DictEntry out;
    ASSERT_TRUE(reader.find_term("zzzz", &found, &out).ok());
    EXPECT_FALSE(found);
  }
  // 命中恰为锚点位置的 term（anchor_interval=8 → t08, t16, t24 ...）。
  for (const char* t : {"t00", "t08", "t16", "t24", "t32"}) {
    bool found = false;
    DictEntry out;
    ASSERT_TRUE(reader.find_term(t, &found, &out).ok()) << t;
    ASSERT_TRUE(found) << t;
    EXPECT_EQ(out.term, t);
  }
  // 命中非锚点位置的 term。
  for (const char* t : {"t03", "t11", "t27"}) {
    bool found = false;
    DictEntry out;
    ASSERT_TRUE(reader.find_term(t, &found, &out).ok()) << t;
    ASSERT_TRUE(found) << t;
    EXPECT_EQ(out.term, t);
  }
  // 在 term 范围内但不存在的 gap。
  {
    bool found = true;
    DictEntry out;
    ASSERT_TRUE(reader.find_term("t05x", &found, &out).ok());
    EXPECT_FALSE(found);
  }
}

// crc 损坏检出：篡改任一字节，open 必须报 Corruption。
TEST(DictBlock, CrcCorruptionDetected) {
  std::vector<DictEntry> entries = {MakePodRef("aaa", 1, 0),
                                    MakePodRef("bbb", 2, 50),
                                    MakePodRef("ccc", 3, 100)};
  std::vector<uint8_t> bytes =
      BuildBlock(entries, IndexTier::kT2, true, 100, 200, 16);
  // 篡改 entries 区中部一个字节。
  bytes[bytes.size() / 2] ^= 0xFF;
  DictBlockReader reader;
  Status s = DictBlockReader::open(Slice(bytes), IndexTier::kT2, true, &reader);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
}

// 截断 block（短于 crc footer）应报 Corruption，而非越界崩溃。
TEST(DictBlock, TruncatedBlockDetected) {
  std::vector<DictEntry> entries = {MakePodRef("xxx", 1, 0)};
  std::vector<uint8_t> bytes =
      BuildBlock(entries, IndexTier::kT1, false, 0, 0, 16);
  bytes.resize(2);  // 只剩 header 开头几字节。
  DictBlockReader reader;
  Status s =
      DictBlockReader::open(Slice(bytes), IndexTier::kT1, false, &reader);
  EXPECT_FALSE(s.ok());
}

// positions 模式：prx_base 与 prx 字段正确保留。
TEST(DictBlock, PositionsPrxRoundTrip) {
  std::vector<DictEntry> entries = {MakePodRef("phrase", 5, 0, 0),
                                    MakePodRef("query", 6, 200, 80)};
  std::vector<uint8_t> bytes =
      BuildBlock(entries, IndexTier::kT2, true, 7000, 9000, 16);
  DictBlockReader reader;
  ASSERT_TRUE(
      DictBlockReader::open(Slice(bytes), IndexTier::kT2, true, &reader).ok());
  EXPECT_EQ(reader.prx_base(), 9000u);

  bool found = false;
  DictEntry out;
  ASSERT_TRUE(reader.find_term("query", &found, &out).ok());
  ASSERT_TRUE(found);
  EXPECT_EQ(out.prx_off_delta, 80u);
  EXPECT_EQ(out.prx_len, 64u);
  EXPECT_EQ(out.ttf_delta, 12u);
}

// estimated_bytes 单调递增：每次 add_entry 后估算值增长，且 finish 后实际字节
// 不超过估算（估算为上界，供切块决策）。
TEST(DictBlock, EstimatedBytesMonotonic) {
  DictBlockBuilder builder(IndexTier::kT1, false, 0, 0, 16);
  size_t prev = builder.estimated_bytes();
  std::vector<std::string> terms = {"aa", "bb", "cc", "dd"};
  for (const auto& t : terms) {
    builder.add_entry(MakePodRef(t, 1, 0));
    size_t now = builder.estimated_bytes();
    EXPECT_GE(now, prev);
    prev = now;
  }
  ByteSink sink;
  builder.finish(&sink);
  EXPECT_LE(sink.size(), builder.estimated_bytes());
}

// Security regression: anchor offsets must be strictly increasing with the first
// anchor at the entries start. A tampered anchor table (offsets swapped) whose crc
// is re-stamped must be rejected by open(); otherwise scan_from_anchor would
// underflow seg_end-seg_begin and read out of bounds (found via ASAN/UBSAN review).
TEST(DictBlock, RejectsNonMonotonicAnchorOffsets) {
  std::vector<DictEntry> entries = {MakePodRef("aaa", 1, 0), MakePodRef("bbb", 2, 10),
                                    MakePodRef("ccc", 3, 20)};
  // anchor_interval = 1 -> every entry is an anchor (3 anchors).
  std::vector<uint8_t> bytes = BuildBlock(entries, IndexTier::kT1, false, 100, 0, 1);
  const size_t n = bytes.size();
  ASSERT_GT(n, 20u);
  // Tail layout: [...][anchor_offsets: 3 * u32][n_anchors u32][crc32c u32].
  const size_t off1 = n - 4 /*crc*/ - 4 /*n_anchors*/ - 4 /*offset[2]*/ - 4 /*offset[1]*/;
  const size_t off2 = off1 + 4;
  for (int k = 0; k < 4; ++k) {
    uint8_t tmp = bytes[off1 + k];
    bytes[off1 + k] = bytes[off2 + k];
    bytes[off2 + k] = tmp;
  }
  // Re-stamp crc32c over the covered region [0, n-4) so corruption is structural.
  uint32_t crc = crc32c(Slice(bytes.data(), n - 4));
  for (int k = 0; k < 4; ++k) bytes[n - 4 + k] = static_cast<uint8_t>(crc >> (8 * k));

  DictBlockReader reader;
  Status s = DictBlockReader::open(Slice(bytes), IndexTier::kT1, false, &reader);
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
}
