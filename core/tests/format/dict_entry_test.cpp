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

// Construct a pod_ref slim dict entry (tier determines which optional fields are present).
DictEntry MakePodRefSlim(std::string term, uint32_t df) {
  DictEntry e;
  e.term = std::move(term);
  e.kind = DictEntryKind::kPodRef;
  e.enc = DictEntryEnc::kSlim;
  e.has_sb = false;
  e.df = df;
  e.ttf_delta = df * 3;       // written only when tier>=T2
  e.max_freq = 7;             // written only when tier>=T2
  e.frq_off_delta = 4096;
  e.frq_len = 333;
  e.frq_docs_len = 200;       // slim pod_ref: docs-only prefix (<= frq_len)
  e.prx_off_delta = 8192;     // written only when positions are stored
  e.prx_len = 512;            // written only when positions are stored
  return e;
}

DictEntry MakePodRefWindowed(std::string term, uint32_t df) {
  DictEntry e = MakePodRefSlim(std::move(term), df);
  e.enc = DictEntryEnc::kWindowed;
  e.has_sb = true;
  e.prelude_len = 64;         // written only for windowed encoding
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
  e.prx_bytes = {0xAA, 0xBB};  // written only when positions are stored
  return e;
}

// round-trip: encode then decode, assert that fields retained by the given tier match.
DictEntry RoundTrip(const DictEntry& in, std::string_view prev, IndexTier tier) {
  ByteSink sink;
  EXPECT_TRUE(encode_dict_entry(in, prev, tier, &sink).ok());
  ByteSource src(sink.view());
  DictEntry out;
  Status s = decode_dict_entry(&src, prev, tier, &out);
  EXPECT_TRUE(s.ok()) << s.to_string();
  EXPECT_EQ(src.remaining(), 0u) << "decode did not consume the entire entry";
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
  EXPECT_EQ(out.frq_docs_len, in.frq_docs_len);  // slim docs-only prefix preserved
  // tier1: ttf/max_freq/prx are not written; decode restores them to default 0.
  EXPECT_EQ(out.ttf_delta, 0u);
  EXPECT_EQ(out.max_freq, 0u);
  EXPECT_EQ(out.prx_len, 0u);
}

TEST(DictEntry, PodRefSlimTier2RoundTrip) {
  DictEntry in = MakePodRefSlim("banana", 100);
  DictEntry out = RoundTrip(in, "", IndexTier::kT2);
  ExpectCommon(in, out);
  EXPECT_EQ(out.frq_docs_len, in.frq_docs_len);
  EXPECT_EQ(out.ttf_delta, in.ttf_delta);
  EXPECT_EQ(out.max_freq, in.max_freq);
  EXPECT_EQ(out.prx_off_delta, in.prx_off_delta);
  EXPECT_EQ(out.prx_len, in.prx_len);
}

// A slim pod_ref whose frq_docs_len exceeds frq_len is rejected on decode.
TEST(DictEntry, PodRefSlimFrqDocsLenExceedsFrqLenRejected) {
  DictEntry in = MakePodRefSlim("kiwi", 50);
  in.frq_docs_len = in.frq_len + 1;  // impossible prefix
  ByteSink sink;
  ASSERT_TRUE(encode_dict_entry(in, "", IndexTier::kT2, &sink).ok());
  ByteSource src(sink.view());
  DictEntry out;
  Status s = decode_dict_entry(&src, "", IndexTier::kT2, &out);
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
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
  EXPECT_EQ(out.prx_len, 0u);  // tier1 has no prx
}

TEST(DictEntry, InlineTier1RoundTrip) {
  DictEntry in = MakeInline("fig", 3);
  DictEntry out = RoundTrip(in, "", IndexTier::kT1);
  ExpectCommon(in, out);
  EXPECT_EQ(out.frq_bytes, in.frq_bytes);
  EXPECT_TRUE(out.prx_bytes.empty());  // tier1 has no prx
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

// Front coding: consecutive encode/decode of sorted terms; suffix stores only the differing part.
TEST(DictEntry, PrefixCompressionSharedPrefix) {
  // "interest" and "interesting" share the prefix "interest".
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

// Encode/decode three sorted terms consecutively to verify the prefix chain.
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

// entry_len must allow a reader to skip the entire entry without parsing its internal fields.
TEST(DictEntry, EntryLenAllowsSkip) {
  DictEntry a = MakePodRefSlim("first", 9);
  DictEntry b = MakeInline("second", 4);
  ByteSink sink;
  ASSERT_TRUE(encode_dict_entry(a, "", IndexTier::kT2, &sink).ok());
  ASSERT_TRUE(encode_dict_entry(b, a.term, IndexTier::kT2, &sink).ok());

  ByteSource src(sink.view());
  // Skip the first entry using only entry_len.
  ASSERT_TRUE(skip_dict_entry(&src).ok());
  DictEntry out;
  ASSERT_TRUE(decode_dict_entry(&src, a.term, IndexTier::kT2, &out).ok());
  EXPECT_EQ(out.term, "second");
  EXPECT_TRUE(src.eof());
}

// Edge case: empty suffix (term is identical to prev).
TEST(DictEntry, EmptySuffixEqualsPrev) {
  DictEntry in = MakePodRefSlim("same", 1);
  DictEntry out = RoundTrip(in, "same", IndexTier::kT1);
  EXPECT_EQ(out.term, "same");
}

// Edge case: df = 0.
TEST(DictEntry, ZeroDf) {
  DictEntry in = MakePodRefSlim("zero", 0);
  DictEntry out = RoundTrip(in, "", IndexTier::kT2);
  EXPECT_EQ(out.df, 0u);
}

// Edge case: empty term (first entry, prev is empty, suffix is also empty).
TEST(DictEntry, EmptyTerm) {
  DictEntry in = MakePodRefSlim("", 1);
  DictEntry out = RoundTrip(in, "", IndexTier::kT1);
  EXPECT_EQ(out.term, "");
}

// Structural integrity: no CRC at the entry level (CRC is at the DICT block level), but entry_len and the actual body
// length must match; tampering with entry_len to make it smaller than the real body → decode must report Corruption.
TEST(DictEntry, EntryLenMismatchDetected) {
  DictEntry in = MakePodRefSlim("payload", 99);
  ByteSink sink;
  ASSERT_TRUE(encode_dict_entry(in, "", IndexTier::kT2, &sink).ok());
  std::vector<uint8_t> bytes = sink.buffer();
  ASSERT_GT(bytes[0], 1u);  // single-byte entry_len varint
  bytes[0] -= 1;            // claim the body is 1 byte shorter than it actually is

  Slice corrupt(bytes);
  ByteSource src(corrupt);
  DictEntry out;
  Status s = decode_dict_entry(&src, "", IndexTier::kT2, &out);
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
}

// prefix_len exceeding the length of prev must be rejected (guard against malformed input).
TEST(DictEntry, RejectPrefixLongerThanPrev) {
  DictEntry in = MakePodRefSlim("abcdef", 1);
  ByteSink sink;
  ASSERT_TRUE(encode_dict_entry(in, "abc", IndexTier::kT1, &sink).ok());
  // Decode with a shorter prev: prefix_len(=3) > prev.size()(=2) → Corruption.
  ByteSource src(sink.view());
  DictEntry out;
  Status s = decode_dict_entry(&src, "ab", IndexTier::kT1, &out);
  EXPECT_FALSE(s.ok());
}
