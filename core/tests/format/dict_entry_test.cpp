#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/byte_source.h"
#include "snii/format/dict_entry.h"
#include "snii/format/format_constants.h"

using namespace snii;        // NOLINT
using namespace snii::format;  // NOLINT

namespace {

// 构造一个 pod_ref slim 词典项（tier 决定可选字段）。
DictEntry MakePodRefSlim(std::string term, uint32_t df) {
  DictEntry e;
  e.term = std::move(term);
  e.kind = DictEntryKind::kPodRef;
  e.enc = DictEntryEnc::kSlim;
  e.has_sb = false;
  e.df = df;
  e.ttf_delta = df * 3;       // 仅在 tier>=T2 写出
  e.max_freq = 7;             // 仅在 tier>=T2 写出
  e.frq_off_delta = 4096;
  e.frq_len = 333;
  e.prx_off_delta = 8192;     // 仅在 positions 时写出
  e.prx_len = 512;            // 仅在 positions 时写出
  return e;
}

DictEntry MakePodRefWindowed(std::string term, uint32_t df) {
  DictEntry e = MakePodRefSlim(std::move(term), df);
  e.enc = DictEntryEnc::kWindowed;
  e.has_sb = true;
  e.prelude_len = 64;         // 仅在 windowed 时写出
  return e;
}

DictEntry MakeInline(std::string term, uint32_t df) {
  DictEntry e;
  e.term = std::move(term);
  e.kind = DictEntryKind::kInline;
  e.enc = DictEntryEnc::kSlim;
  e.df = df;
  e.ttf_delta = df * 2;
  e.max_freq = 5;
  e.frq_bytes = {0x01, 0x02, 0x03, 0x04};
  e.prx_bytes = {0xAA, 0xBB};  // 仅在 positions 时写出
  return e;
}

// round-trip：编码再解码，按 tier 断言保留的字段一致。
DictEntry RoundTrip(const DictEntry& in, std::string_view prev, IndexTier tier) {
  ByteSink sink;
  EXPECT_TRUE(encode_dict_entry(in, prev, tier, &sink).ok());
  ByteSource src(sink.view());
  DictEntry out;
  Status s = decode_dict_entry(&src, prev, tier, &out);
  EXPECT_TRUE(s.ok()) << s.to_string();
  EXPECT_EQ(src.remaining(), 0u) << "decode 未消费整个 entry";
  return out;
}

void ExpectCommon(const DictEntry& a, const DictEntry& b) {
  EXPECT_EQ(a.term, b.term);
  EXPECT_EQ(a.kind, b.kind);
  EXPECT_EQ(a.enc, b.enc);
  EXPECT_EQ(a.has_sb, b.has_sb);
  EXPECT_EQ(a.df, b.df);
}

}  // namespace

TEST(DictEntry, PodRefSlimTier1RoundTrip) {
  DictEntry in = MakePodRefSlim("apple", 42);
  DictEntry out = RoundTrip(in, "", IndexTier::kT1);
  ExpectCommon(in, out);
  EXPECT_EQ(out.frq_off_delta, in.frq_off_delta);
  EXPECT_EQ(out.frq_len, in.frq_len);
  // tier1：不写 ttf/max_freq/prx，解码恢复为默认 0。
  EXPECT_EQ(out.ttf_delta, 0u);
  EXPECT_EQ(out.max_freq, 0u);
  EXPECT_EQ(out.prx_len, 0u);
}

TEST(DictEntry, PodRefSlimTier2RoundTrip) {
  DictEntry in = MakePodRefSlim("banana", 100);
  DictEntry out = RoundTrip(in, "", IndexTier::kT2);
  ExpectCommon(in, out);
  EXPECT_EQ(out.ttf_delta, in.ttf_delta);
  EXPECT_EQ(out.max_freq, in.max_freq);
  EXPECT_EQ(out.prx_off_delta, in.prx_off_delta);
  EXPECT_EQ(out.prx_len, in.prx_len);
}

TEST(DictEntry, PodRefSlimTier3RoundTrip) {
  DictEntry in = MakePodRefSlim("cherry", 500);
  DictEntry out = RoundTrip(in, "", IndexTier::kT3);
  ExpectCommon(in, out);
  EXPECT_EQ(out.ttf_delta, in.ttf_delta);
  EXPECT_EQ(out.max_freq, in.max_freq);
  EXPECT_EQ(out.prx_off_delta, in.prx_off_delta);
  EXPECT_EQ(out.prx_len, in.prx_len);
}

TEST(DictEntry, PodRefWindowedTier2RoundTrip) {
  DictEntry in = MakePodRefWindowed("durian", 2000);
  DictEntry out = RoundTrip(in, "", IndexTier::kT2);
  ExpectCommon(in, out);
  EXPECT_EQ(out.enc, DictEntryEnc::kWindowed);
  EXPECT_EQ(out.has_sb, true);
  EXPECT_EQ(out.prelude_len, in.prelude_len);
  EXPECT_EQ(out.prx_len, in.prx_len);
}

TEST(DictEntry, PodRefWindowedTier1RoundTrip) {
  DictEntry in = MakePodRefWindowed("elderberry", 1500);
  DictEntry out = RoundTrip(in, "", IndexTier::kT1);
  ExpectCommon(in, out);
  EXPECT_EQ(out.prelude_len, in.prelude_len);
  EXPECT_EQ(out.prx_len, 0u);  // tier1 无 prx
}

TEST(DictEntry, InlineTier1RoundTrip) {
  DictEntry in = MakeInline("fig", 3);
  DictEntry out = RoundTrip(in, "", IndexTier::kT1);
  ExpectCommon(in, out);
  EXPECT_EQ(out.frq_bytes, in.frq_bytes);
  EXPECT_TRUE(out.prx_bytes.empty());  // tier1 无 prx
}

TEST(DictEntry, InlineTier2RoundTrip) {
  DictEntry in = MakeInline("grape", 8);
  DictEntry out = RoundTrip(in, "", IndexTier::kT2);
  ExpectCommon(in, out);
  EXPECT_EQ(out.frq_bytes, in.frq_bytes);
  EXPECT_EQ(out.prx_bytes, in.prx_bytes);
  EXPECT_EQ(out.ttf_delta, in.ttf_delta);
  EXPECT_EQ(out.max_freq, in.max_freq);
}

TEST(DictEntry, InlineTier3RoundTrip) {
  DictEntry in = MakeInline("honeydew", 12);
  DictEntry out = RoundTrip(in, "", IndexTier::kT3);
  ExpectCommon(in, out);
  EXPECT_EQ(out.frq_bytes, in.frq_bytes);
  EXPECT_EQ(out.prx_bytes, in.prx_bytes);
}

// 前缀压缩：有序 term 连续编解码，suffix 只存差异部分。
TEST(DictEntry, PrefixCompressionSharedPrefix) {
  // "interest" 与 "interesting" 共享前缀 "interest"。
  DictEntry a = MakePodRefSlim("interest", 10);
  DictEntry b = MakePodRefSlim("interesting", 11);

  ByteSink sink;
  ASSERT_TRUE(encode_dict_entry(a, "", IndexTier::kT2, &sink).ok());
  size_t a_end = sink.size();
  ASSERT_TRUE(encode_dict_entry(b, a.term, IndexTier::kT2, &sink).ok());

  ByteSource src(sink.view());
  DictEntry oa;
  ASSERT_TRUE(decode_dict_entry(&src, "", IndexTier::kT2, &oa).ok());
  EXPECT_EQ(src.position(), a_end);
  EXPECT_EQ(oa.term, "interest");

  DictEntry ob;
  ASSERT_TRUE(decode_dict_entry(&src, oa.term, IndexTier::kT2, &ob).ok());
  EXPECT_EQ(ob.term, "interesting");
  EXPECT_TRUE(src.eof());
}

// 三个有序 term 连续编解码，验证前缀链路。
TEST(DictEntry, PrefixCompressionChain) {
  std::vector<std::string> terms = {"app", "apple", "application"};
  ByteSink sink;
  std::string prev;
  for (const auto& t : terms) {
    DictEntry e = MakePodRefSlim(t, 5);
    ASSERT_TRUE(encode_dict_entry(e, prev, IndexTier::kT1, &sink).ok());
    prev = t;
  }
  ByteSource src(sink.view());
  prev.clear();
  for (const auto& t : terms) {
    DictEntry out;
    ASSERT_TRUE(decode_dict_entry(&src, prev, IndexTier::kT1, &out).ok());
    EXPECT_EQ(out.term, t);
    prev = out.term;
  }
  EXPECT_TRUE(src.eof());
}

// entry_len 必须允许 reader 在不解析内部字段的情况下跳过整个 entry。
TEST(DictEntry, EntryLenAllowsSkip) {
  DictEntry a = MakePodRefSlim("first", 9);
  DictEntry b = MakeInline("second", 4);
  ByteSink sink;
  ASSERT_TRUE(encode_dict_entry(a, "", IndexTier::kT2, &sink).ok());
  ASSERT_TRUE(encode_dict_entry(b, a.term, IndexTier::kT2, &sink).ok());

  ByteSource src(sink.view());
  // 仅靠 entry_len 跳过第一个 entry。
  ASSERT_TRUE(skip_dict_entry(&src).ok());
  DictEntry out;
  ASSERT_TRUE(decode_dict_entry(&src, a.term, IndexTier::kT2, &out).ok());
  EXPECT_EQ(out.term, "second");
  EXPECT_TRUE(src.eof());
}

// 边界：空 suffix（term 完全等于 prev）。
TEST(DictEntry, EmptySuffixEqualsPrev) {
  DictEntry in = MakePodRefSlim("same", 1);
  DictEntry out = RoundTrip(in, "same", IndexTier::kT1);
  EXPECT_EQ(out.term, "same");
}

// 边界：df = 0。
TEST(DictEntry, ZeroDf) {
  DictEntry in = MakePodRefSlim("zero", 0);
  DictEntry out = RoundTrip(in, "", IndexTier::kT2);
  EXPECT_EQ(out.df, 0u);
}

// 边界：空 term（首个 entry，prev 为空，suffix 也为空）。
TEST(DictEntry, EmptyTerm) {
  DictEntry in = MakePodRefSlim("", 1);
  DictEntry out = RoundTrip(in, "", IndexTier::kT1);
  EXPECT_EQ(out.term, "");
}

// 结构完整性：entry 级无 crc（crc 在 DICT block 级），但 entry_len 与 body 实际
// 长度必须一致；篡改 entry_len 使其小于真实 body → decode 必须报 Corruption。
TEST(DictEntry, EntryLenMismatchDetected) {
  DictEntry in = MakePodRefSlim("payload", 99);
  ByteSink sink;
  ASSERT_TRUE(encode_dict_entry(in, "", IndexTier::kT2, &sink).ok());
  std::vector<uint8_t> bytes = sink.buffer();
  ASSERT_GT(bytes[0], 1u);  // 单字节 entry_len varint
  bytes[0] -= 1;            // 声称 body 比实际少 1 字节

  Slice corrupt(bytes);
  ByteSource src(corrupt);
  DictEntry out;
  Status s = decode_dict_entry(&src, "", IndexTier::kT2, &out);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
}

// prefix_len 超过 prev 长度应被拒绝（防御非法输入）。
TEST(DictEntry, RejectPrefixLongerThanPrev) {
  DictEntry in = MakePodRefSlim("abcdef", 1);
  ByteSink sink;
  ASSERT_TRUE(encode_dict_entry(in, "abc", IndexTier::kT1, &sink).ok());
  // 用一个更短的 prev 解码：prefix_len(=3) > prev.size()(=2) → Corruption。
  ByteSource src(sink.view());
  DictEntry out;
  Status s = decode_dict_entry(&src, "ab", IndexTier::kT1, &out);
  EXPECT_FALSE(s.ok());
}
