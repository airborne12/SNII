#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/byte_source.h"
#include "snii/encoding/section_framer.h"
#include "snii/format/dict_block_directory.h"
#include "snii/format/format_constants.h"
#include "snii/format/per_index_meta.h"
#include "snii/format/sampled_term_index.h"
#include "snii/format/stats_block.h"

using namespace snii;
using namespace snii::format;

namespace {

// Produces the raw framed bytes of a SampledTermIndex from a list of first_terms.
std::vector<uint8_t> BuildSampled(const std::vector<std::string>& terms) {
  SampledTermIndexBuilder b;
  for (const auto& t : terms) {
    b.add_block_first_term(t);
  }
  ByteSink sink;
  b.finish(&sink);
  return sink.buffer();
}

// Produces the raw framed bytes of a DICT block directory from a list of refs.
std::vector<uint8_t> BuildDict(const std::vector<BlockRef>& refs) {
  DictBlockDirectoryBuilder b;
  for (const auto& r : refs) {
    b.add(r);
  }
  ByteSink sink;
  b.finish(&sink);
  return sink.buffer();
}

StatsBlock SampleStats() {
  StatsBlock sb;
  sb.doc_count = 1000;
  sb.indexed_doc_count = 950;
  sb.term_count = 4242;
  sb.sum_total_term_freq = 1234567;
  sb.null_count = 50;
  return sb;
}

SectionRefs SampleRefs() {
  SectionRefs r;
  r.dict_region = {4096, 65536};
  r.posting_region = {70000, 24576};  // single interleaved [prx][frq] region
  r.norms = {100000, 4096};
  r.null_bitmap = {0, 0};  // absent
  r.bsbf = {120000, 32768};
  return r;
}

void ExpectRegionEq(const RegionRef& a, const RegionRef& b) {
  EXPECT_EQ(a.offset, b.offset);
  EXPECT_EQ(a.length, b.length);
}

void ExpectRefsEq(const SectionRefs& a, const SectionRefs& b) {
  ExpectRegionEq(a.dict_region, b.dict_region);
  ExpectRegionEq(a.posting_region, b.posting_region);
  ExpectRegionEq(a.norms, b.norms);
  ExpectRegionEq(a.null_bitmap, b.null_bitmap);
  ExpectRegionEq(a.bsbf, b.bsbf);
}

void ExpectStatsEq(const StatsBlock& a, const StatsBlock& b) {
  EXPECT_EQ(a.doc_count, b.doc_count);
  EXPECT_EQ(a.indexed_doc_count, b.indexed_doc_count);
  EXPECT_EQ(a.term_count, b.term_count);
  EXPECT_EQ(a.sum_total_term_freq, b.sum_total_term_freq);
  EXPECT_EQ(a.null_count, b.null_count);
}

// Builds a full per-index meta block (SectionRefs carry the bsbf section ref).
std::vector<uint8_t> BuildMeta(uint64_t index_id, std::string suffix,
                               const std::vector<std::string>& sample_terms,
                               const std::vector<BlockRef>& dict_refs) {
  PerIndexMetaBuilder builder(index_id, std::move(suffix),
                              PerIndexMetaBuilder::kHasBsbf);
  builder.set_stats(SampleStats());
  builder.set_sampled_term_index(Slice(BuildSampled(sample_terms)));
  builder.set_dict_block_directory(Slice(BuildDict(dict_refs)));
  builder.set_section_refs(SampleRefs());
  ByteSink sink;
  builder.finish(&sink);
  return sink.buffer();
}

}  // namespace

TEST(PerIndexMeta, RoundTrip) {
  std::vector<std::string> sample_terms = {"alpha", "kappa", "zeta"};
  std::vector<BlockRef> dict_refs = {
      {4096, 1024, 10, 0, 0x11111111u},
      {5120, 2048, 20, 1, 0x22222222u},
      {7168, 512, 5, 0, 0x33333333u},
  };
  auto bytes = BuildMeta(7, "title", sample_terms, dict_refs);

  PerIndexMetaReader reader;
  ASSERT_TRUE(PerIndexMetaReader::open(Slice(bytes), &reader).ok());

  EXPECT_EQ(reader.index_id(), 7u);
  EXPECT_EQ(reader.index_suffix(), "title");
  // The bsbf XFilter is a physical section ref, not embedded in the meta.
  EXPECT_TRUE(reader.has_bsbf());
  EXPECT_NE(reader.flags() & PerIndexMetaBuilder::kHasBsbf, 0u);

  ExpectStatsEq(reader.stats(), SampleStats());
  ExpectRefsEq(reader.section_refs(), SampleRefs());

  // The exposed sub-section byte Slices must be openable by their own readers.
  SampledTermIndexReader sti;
  ASSERT_TRUE(
      SampledTermIndexReader::open(reader.sampled_term_index_bytes(), &sti).ok());
  EXPECT_EQ(sti.n_blocks(), 3u);
  bool maybe = false;
  uint32_t ord = 0;
  ASSERT_TRUE(sti.locate("kappa", &maybe, &ord).ok());
  EXPECT_TRUE(maybe);
  EXPECT_EQ(ord, 1u);

  DictBlockDirectoryReader dbd;
  ASSERT_TRUE(
      DictBlockDirectoryReader::open(reader.dict_block_directory_bytes(), &dbd).ok());
  ASSERT_EQ(dbd.n_blocks(), 3u);
  BlockRef got{};
  ASSERT_TRUE(dbd.get(1, &got).ok());
  EXPECT_EQ(got.offset, 5120u);
  EXPECT_EQ(got.checksum, 0x22222222u);
}

TEST(PerIndexMeta, HasPositionsFlagRoundTrips) {
  // The kHasPositions header bit must survive a build/open round-trip and be read
  // back via the convenience accessor (capability lives in the flag, not a length).
  PerIndexMetaBuilder builder(
      9, "body", PerIndexMetaBuilder::kHasPositions | PerIndexMetaBuilder::kHasBsbf);
  builder.set_stats(SampleStats());
  builder.set_sampled_term_index(Slice(BuildSampled({"a"})));
  builder.set_dict_block_directory(Slice(BuildDict({{0, 1, 1, 0, 0}})));
  builder.set_section_refs(SampleRefs());
  ByteSink sink;
  ASSERT_TRUE(builder.finish(&sink).ok());

  PerIndexMetaReader reader;
  ASSERT_TRUE(PerIndexMetaReader::open(Slice(sink.buffer()), &reader).ok());
  EXPECT_TRUE(reader.has_positions());
  EXPECT_TRUE(reader.has_bsbf());
  EXPECT_NE(reader.flags() & PerIndexMetaBuilder::kHasPositions, 0u);

  // A docs-only meta (flag clear) reports has_positions() == false even though its
  // posting_region length is non-zero -- capability never comes from a region length.
  PerIndexMetaBuilder docs_only(10, "tag", 0u);
  docs_only.set_stats(SampleStats());
  docs_only.set_sampled_term_index(Slice(BuildSampled({"a"})));
  docs_only.set_dict_block_directory(Slice(BuildDict({{0, 1, 1, 0, 0}})));
  docs_only.set_section_refs(SampleRefs());  // posting_region.length > 0
  ByteSink docs_sink;
  ASSERT_TRUE(docs_only.finish(&docs_sink).ok());
  PerIndexMetaReader docs_reader;
  ASSERT_TRUE(PerIndexMetaReader::open(Slice(docs_sink.buffer()), &docs_reader).ok());
  EXPECT_FALSE(docs_reader.has_positions());
  EXPECT_GT(docs_reader.section_refs().posting_region.length, 0u);
}

TEST(PerIndexMeta, EmptySuffix) {
  auto bytes = BuildMeta(1, "", {"a"}, {{0, 1, 1, 0, 0}});
  PerIndexMetaReader reader;
  ASSERT_TRUE(PerIndexMetaReader::open(Slice(bytes), &reader).ok());
  EXPECT_EQ(reader.index_id(), 1u);
  EXPECT_TRUE(reader.index_suffix().empty());
}

TEST(PerIndexMeta, HeaderStartsWithMetaFormatVersion) {
  auto bytes = BuildMeta(7, "x", {"a"}, {{0, 1, 1, 0, 0}});
  ASSERT_GE(bytes.size(), 2u);
  // u16 meta_format_version, little-endian, is the first field.
  ByteSource src{Slice(bytes)};
  uint16_t ver = 0;
  ASSERT_TRUE(src.get_fixed16(&ver).ok());
  EXPECT_EQ(ver, kMetaFormatVersion);
}

TEST(PerIndexMeta, HeaderCrcCorruptionDetected) {
  auto bytes = BuildMeta(7, "title", {"a", "b"}, {{0, 1, 1, 0, 0}});
  ASSERT_GE(bytes.size(), 3u);
  // Flip a byte inside the header (index_id varint region after the u16 version).
  bytes[3] ^= 0xFF;
  PerIndexMetaReader reader;
  EXPECT_EQ(PerIndexMetaReader::open(Slice(bytes), &reader).code(),
            StatusCode::kCorruption);
}

TEST(PerIndexMeta, SubSectionCrcCorruptionDetected) {
  std::vector<std::string> sample_terms = {"alpha", "kappa"};
  std::vector<BlockRef> dict_refs = {{4096, 1024, 10, 0, 0x11111111u}};
  auto bytes = BuildMeta(7, "title", sample_terms, dict_refs);
  // Corrupt a byte deep in the block (well past the header, inside a framed
  // sub-section payload). The framer CRC of that sub-section must catch it.
  ASSERT_GT(bytes.size(), 40u);
  bytes[bytes.size() - 10] ^= 0xFF;
  PerIndexMetaReader reader;
  EXPECT_EQ(PerIndexMetaReader::open(Slice(bytes), &reader).code(),
            StatusCode::kCorruption);
}

TEST(PerIndexMeta, UnknownOptionalSectionSkipped) {
  // Build a normal meta block, then splice an unknown-type framed section in
  // front of the SectionRefs by rebuilding manually: header + stats + sampled +
  // dict + unknown + section_refs. The reader must skip the unknown section and
  // still expose all the known ones.
  PerIndexMetaBuilder builder(7, "title", 0);
  builder.set_stats(SampleStats());
  builder.set_sampled_term_index(Slice(BuildSampled({"a", "b"})));
  builder.set_dict_block_directory(Slice(BuildDict({{0, 1, 1, 0, 0}})));
  builder.set_section_refs(SampleRefs());

  // Inject an extra framed section with an unrecognized type id (200).
  const uint8_t junk[] = {0xDE, 0xAD, 0xBE, 0xEF};
  ByteSink extra;
  SectionFramer::write(extra, 200, Slice(junk, sizeof(junk)));
  builder.add_raw_section(extra.view());

  ByteSink sink;
  builder.finish(&sink);
  auto bytes = sink.buffer();

  PerIndexMetaReader reader;
  ASSERT_TRUE(PerIndexMetaReader::open(Slice(bytes), &reader).ok());
  ExpectStatsEq(reader.stats(), SampleStats());
  ExpectRefsEq(reader.section_refs(), SampleRefs());
  SampledTermIndexReader sti;
  ASSERT_TRUE(
      SampledTermIndexReader::open(reader.sampled_term_index_bytes(), &sti).ok());
  EXPECT_EQ(sti.n_blocks(), 2u);
  DictBlockDirectoryReader dbd;
  ASSERT_TRUE(
      DictBlockDirectoryReader::open(reader.dict_block_directory_bytes(), &dbd).ok());
  EXPECT_EQ(dbd.n_blocks(), 1u);
}

TEST(PerIndexMeta, TruncatedHeaderRejected) {
  auto bytes = BuildMeta(7, "title", {"a"}, {{0, 1, 1, 0, 0}});
  // Keep only one byte: cannot even read the u16 version.
  std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + 1);
  PerIndexMetaReader reader;
  EXPECT_FALSE(PerIndexMetaReader::open(Slice(truncated), &reader).ok());
}

TEST(PerIndexMeta, NullSinkRejectedByFinish) {
  PerIndexMetaBuilder builder(1, "x", 0);
  builder.set_stats(SampleStats());
  builder.set_sampled_term_index(Slice(BuildSampled({"a"})));
  builder.set_dict_block_directory(Slice(BuildDict({{0, 1, 1, 0, 0}})));
  builder.set_section_refs(SampleRefs());
  EXPECT_EQ(builder.finish(nullptr).code(), StatusCode::kInvalidArgument);
}

TEST(PerIndexMeta, OpenNullReaderRejected) {
  auto bytes = BuildMeta(1, "x", {"a"}, {{0, 1, 1, 0, 0}});
  EXPECT_EQ(PerIndexMetaReader::open(Slice(bytes), nullptr).code(),
            StatusCode::kInvalidArgument);
}
