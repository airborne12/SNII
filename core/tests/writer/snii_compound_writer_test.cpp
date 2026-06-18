#include "snii/writer/snii_compound_writer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/encoding/byte_source.h"
#include "snii/format/bootstrap_header.h"
#include "snii/format/dict_block.h"
#include "snii/format/dict_block_directory.h"
#include "snii/format/dict_entry.h"
#include "snii/format/format_constants.h"
#include "snii/format/frq_pod.h"
#include "snii/format/frq_prelude.h"
#include "snii/format/per_index_meta.h"
#include "snii/format/prx_pod.h"
#include "snii/format/sampled_term_index.h"
#include "snii/format/tail_meta_region.h"
#include "snii/format/tail_pointer.h"
#include "snii/format/xfilter.h"
#include "snii/io/local_file.h"
#include "snii/io/metered_file_reader.h"
#include "snii/query/phrase_query.h"
#include "snii/query/term_query.h"
#include "snii/reader/logical_index_reader.h"
#include "snii/reader/snii_segment_reader.h"
#include "snii/writer/logical_index_writer.h"

using namespace snii;
using namespace snii::format;
using namespace snii::writer;

namespace {

// Temp file path helper (process-unique).
std::string TempPath() {
  static int counter = 0;
  return "/tmp/snii_cw_test_" + std::to_string(getpid()) + "_" +
         std::to_string(counter++) + ".idx";
}

// Reads the whole file into a buffer.
std::vector<uint8_t> ReadAll(const std::string& path) {
  io::LocalFileReader r;
  EXPECT_TRUE(r.open(path).ok());
  std::vector<uint8_t> out;
  EXPECT_TRUE(r.read_at(0, r.size(), &out).ok());
  return out;
}

// Builds a TermPostings with constant freq per doc and (optionally) positions.
TermPostings MakeTerm(const std::string& term, const std::vector<uint32_t>& docids,
                      bool with_positions) {
  TermPostings tp;
  tp.term = term;
  tp.docids = docids;
  tp.freqs.assign(docids.size(), 0);
  for (size_t i = 0; i < docids.size(); ++i) {
    tp.freqs[i] = static_cast<uint32_t>((i % 3) + 1);
  }
  if (with_positions) {
    tp.positions.resize(docids.size());
    for (size_t i = 0; i < docids.size(); ++i) {
      for (uint32_t k = 0; k < tp.freqs[i]; ++k) {
        tp.positions[i].push_back(k * 2);  // ascending positions
      }
    }
  }
  return tp;
}

// Builds a FrqRegionMeta for a window's dd region from its WindowMeta.
FrqRegionMeta DdMetaOf(const WindowMeta& m) {
  FrqRegionMeta r;
  r.zstd = m.dd_zstd;
  r.uncomp_len = m.dd_uncomp_len;
  r.disk_len = m.dd_disk_len;
  r.crc = m.crc_dd;
  return r;
}

// Index 0: ~30 docs. Vocab includes a HIGH-df term (df=600 > 512 -> windowed)
// and several LOW-df terms (-> slim/inline). Terms must be lexicographically
// sorted.
SniiIndexInput MakeIndex(uint64_t index_id, const std::string& suffix,
                         uint32_t doc_count) {
  SniiIndexInput in;
  in.index_id = index_id;
  in.index_suffix = suffix;
  in.config = IndexConfig::kDocsPositions;
  in.doc_count = doc_count;
  // Force one DICT block per term so the sampled-term index covers every term
  // (its max_term == the lexicographically largest term).
  in.target_dict_block_bytes = 1;

  // low-df "apple" in a handful of docs.
  in.terms.push_back(MakeTerm("apple", {0, 5, 12, 20}, true));
  // mid-low "banana".
  in.terms.push_back(MakeTerm("banana", {1, 2, 3, 4, 9, 15}, true));
  // HIGH-df "common": df=600 (>=512) -> windowed pod_ref.
  std::vector<uint32_t> common_docs;
  for (uint32_t d = 0; d < 600; ++d) common_docs.push_back(d);
  in.terms.push_back(MakeTerm("common", common_docs, true));
  // low-df "zebra".
  in.terms.push_back(MakeTerm("zebra", {7, 21, 29}, true));
  return in;
}

// Locate a term through the full reader walk and return its DictEntry.
Status LocateEntry(const std::vector<uint8_t>& file, const PerIndexMetaReader& meta,
                   const std::string& term, bool* found, DictEntry* out) {
  SampledTermIndexReader sti;
  SNII_RETURN_IF_ERROR(SampledTermIndexReader::open(meta.sampled_term_index_bytes(), &sti));
  DictBlockDirectoryReader dbd;
  SNII_RETURN_IF_ERROR(
      DictBlockDirectoryReader::open(meta.dict_block_directory_bytes(), &dbd));

  bool maybe = false;
  uint32_t ordinal = 0;
  SNII_RETURN_IF_ERROR(sti.locate(term, &maybe, &ordinal));
  if (!maybe) {
    *found = false;
    return Status::OK();
  }
  BlockRef ref{};
  SNII_RETURN_IF_ERROR(dbd.get(ordinal, &ref));
  Slice block(file.data() + ref.offset, ref.length);
  DictBlockReader br;
  SNII_RETURN_IF_ERROR(DictBlockReader::open(block, IndexTier::kT2, true, &br));
  return br.find_term(term, found, out);
}

}  // namespace

TEST(SniiCompoundWriter, ReadBackSelfValidation) {
  const std::string path = TempPath();
  {
    io::LocalFileWriter w;
    ASSERT_TRUE(w.open(path).ok());
    SniiCompoundWriter cw(&w);
    ASSERT_TRUE(cw.add_logical_index(MakeIndex(10, "title", 30)).ok());
    ASSERT_TRUE(cw.add_logical_index(MakeIndex(11, "body", 30)).ok());
    ASSERT_TRUE(cw.finish().ok());
  }

  std::vector<uint8_t> file = ReadAll(path);
  ASSERT_GT(file.size(), kBootstrapHeaderSize + tail_pointer_size());

  // --- bootstrap header ---
  BootstrapHeader bh;
  ASSERT_TRUE(
      decode_bootstrap_header(Slice(file.data(), kBootstrapHeaderSize), &bh).ok());
  EXPECT_EQ(bh.magic, kContainerMagic);
  EXPECT_EQ(bh.format_version, kFormatVersion);
  EXPECT_EQ(bh.tail_pointer_size, static_cast<uint8_t>(tail_pointer_size()));

  // --- tail pointer (last tail_pointer_size() bytes) ---
  Slice tail_bytes(file.data() + file.size() - tail_pointer_size(),
                   tail_pointer_size());
  TailPointer tp;
  ASSERT_TRUE(decode_tail_pointer(tail_bytes, &tp).ok());
  EXPECT_EQ(tp.hot_off, 0u);
  ASSERT_GT(tp.meta_region_length, 0u);
  ASSERT_LE(tp.meta_region_offset + tp.meta_region_length,
            file.size() - tail_pointer_size());

  // --- tail meta region ---
  Slice region(file.data() + tp.meta_region_offset, tp.meta_region_length);
  TailMetaRegionReader tmr;
  ASSERT_TRUE(TailMetaRegionReader::open(region, &tmr).ok());
  EXPECT_EQ(tmr.n_logical_indexes(), 2u);

  struct Expect {
    uint64_t id;
    std::string suffix;
  };
  std::vector<Expect> expects = {{10, "title"}, {11, "body"}};
  for (const auto& e : expects) {
    bool found = false;
    Slice meta_bytes;
    ASSERT_TRUE(tmr.find(e.id, e.suffix, &found, &meta_bytes).ok());
    ASSERT_TRUE(found) << "index " << e.id;

    PerIndexMetaReader meta;
    ASSERT_TRUE(PerIndexMetaReader::open(meta_bytes, &meta).ok());
    EXPECT_EQ(meta.index_id(), e.id);
    EXPECT_EQ(meta.index_suffix(), e.suffix);
    EXPECT_EQ(meta.stats().doc_count, 30u);
    EXPECT_EQ(meta.stats().term_count, 4u);

    const SectionRefs& refs = meta.section_refs();
    // dict_region / frq_pod / prx_pod must be within file bounds.
    ASSERT_GT(refs.dict_region.length, 0u);
    ASSERT_LE(refs.dict_region.offset + refs.dict_region.length, file.size());
    ASSERT_GT(refs.frq_pod.length, 0u);
    ASSERT_LE(refs.frq_pod.offset + refs.frq_pod.length, file.size());
    ASSERT_GT(refs.prx_pod.length, 0u);
    ASSERT_LE(refs.prx_pod.offset + refs.prx_pod.length, file.size());
    // norms absent for docs-positions (no scoring).
    EXPECT_EQ(refs.norms.offset, 0u);
    EXPECT_EQ(refs.norms.length, 0u);
    EXPECT_EQ(refs.null_bitmap.offset, 0u);
    EXPECT_EQ(refs.null_bitmap.length, 0u);

    // --- XFilter: present term true, absent term false ---
    ASSERT_TRUE(meta.has_xfilter());
    XFilterReader xf;
    ASSERT_TRUE(XFilterReader::open(meta.xfilter_bytes(), &xf).ok());
    EXPECT_TRUE(xf.maybe_contains("apple"));
    EXPECT_TRUE(xf.maybe_contains("common"));
    EXPECT_FALSE(xf.maybe_contains("nonexistent-term-xyzzy-12345"));

    // --- windowed high-df term "common": read its .frq window ---
    bool found_common = false;
    DictEntry common_entry;
    ASSERT_TRUE(LocateEntry(file, meta, "common", &found_common, &common_entry).ok());
    ASSERT_TRUE(found_common);
    EXPECT_EQ(common_entry.df, 600u);
    EXPECT_EQ(common_entry.kind, DictEntryKind::kPodRef);
    EXPECT_EQ(common_entry.enc, DictEntryEnc::kWindowed);

    // The DICT block carrying "common" supplies frq_base via DictBlockReader.
    // Recompute frq_base by re-locating the block.
    SampledTermIndexReader sti;
    ASSERT_TRUE(SampledTermIndexReader::open(meta.sampled_term_index_bytes(), &sti).ok());
    DictBlockDirectoryReader dbd;
    ASSERT_TRUE(
        DictBlockDirectoryReader::open(meta.dict_block_directory_bytes(), &dbd).ok());
    bool maybe = false;
    uint32_t ord = 0;
    ASSERT_TRUE(sti.locate("common", &maybe, &ord).ok());
    ASSERT_TRUE(maybe);
    BlockRef bref{};
    ASSERT_TRUE(dbd.get(ord, &bref).ok());
    Slice block(file.data() + bref.offset, bref.length);
    DictBlockReader br;
    ASSERT_TRUE(DictBlockReader::open(block, IndexTier::kT2, true, &br).ok());

    // Absolute .frq offset = frq_pod.offset + frq_base + frq_off_delta. The
    // windowed payload is [prelude][dd-block][freq-block]; parse the two-level
    // prelude, then decode every window's dd region from the dd-block (each with
    // its prelude win_base) to reconstruct the full posting (docs-only path).
    uint64_t frq_abs = refs.frq_pod.offset + br.frq_base() + common_entry.frq_off_delta;
    Slice prelude_bytes(file.data() + frq_abs, common_entry.prelude_len);
    FrqPreludeReader prelude;
    ASSERT_TRUE(FrqPreludeReader::open(prelude_bytes, &prelude).ok());
    const uint64_t dd_block_start = frq_abs + common_entry.prelude_len;
    std::vector<uint32_t> got_docs;
    uint32_t summed = 0;
    for (uint32_t w = 0; w < prelude.n_windows(); ++w) {
      WindowMeta m;
      ASSERT_TRUE(prelude.window(w, &m).ok());
      Slice dd(file.data() + dd_block_start + m.dd_off, m.dd_disk_len);
      std::vector<uint32_t> wdocs;
      ASSERT_TRUE(decode_dd_region(dd, DdMetaOf(m), m.win_base, &wdocs).ok());
      ASSERT_EQ(wdocs.size(), m.doc_count);
      summed += m.doc_count;
      got_docs.insert(got_docs.end(), wdocs.begin(), wdocs.end());
    }
    EXPECT_EQ(summed, 600u);  // window doc_counts sum to df.
    ASSERT_EQ(got_docs.size(), 600u);
    EXPECT_EQ(got_docs.front(), 0u);
    EXPECT_EQ(got_docs.back(), 599u);

    // --- inline low-df term "apple": decode its inline .frq bytes ---
    bool found_apple = false;
    DictEntry apple_entry;
    ASSERT_TRUE(LocateEntry(file, meta, "apple", &found_apple, &apple_entry).ok());
    ASSERT_TRUE(found_apple);
    EXPECT_EQ(apple_entry.df, 4u);
    EXPECT_EQ(apple_entry.kind, DictEntryKind::kInline);
    Slice apple_dd(apple_entry.frq_bytes.data(),
                   static_cast<size_t>(apple_entry.dd_meta.disk_len));
    std::vector<uint32_t> apple_docs;
    ASSERT_TRUE(decode_dd_region(apple_dd, apple_entry.dd_meta, 0, &apple_docs).ok());
    std::vector<uint32_t> expected_apple = {0, 5, 12, 20};
    EXPECT_EQ(apple_docs, expected_apple);

    // inline .prx bytes decode to per-doc positions.
    ByteSource psrc(Slice(apple_entry.prx_bytes));
    std::vector<std::vector<uint32_t>> apple_pos;
    ASSERT_TRUE(read_prx_window(&psrc, &apple_pos).ok());
    EXPECT_EQ(apple_pos.size(), 4u);

    // absent term -> not found through the dict walk.
    bool found_absent = true;
    DictEntry absent_entry;
    ASSERT_TRUE(
        LocateEntry(file, meta, "zzz-not-here", &found_absent, &absent_entry).ok());
    EXPECT_FALSE(found_absent);
  }

  std::remove(path.c_str());
}

namespace {

// Oracle for the multi-super-block read-back test. "hot" is a very high-df term;
// at df=70000 it uses adaptive 1024-doc windows (df >= kAdaptiveWindowDfThreshold)
// -> ~69 windows -> >1 super-block at group_size=64. "spark" appears at position
// 1 immediately after "hot" in docs where d % 9 == 0, so the phrase "hot spark"
// holds exactly in those docs. "rare" is a tiny low-df term.
struct BigCorpus {
  uint32_t doc_count = 70000;
  std::vector<uint32_t> hot_docs;     // every doc
  std::vector<uint32_t> spark_docs;   // d % 9 == 0
  std::vector<uint32_t> rare_docs = {3, 17, 99, 1000, 19999};
  std::vector<uint32_t> phrase_oracle;  // docs where "hot spark" is consecutive
};

BigCorpus MakeBigCorpus() {
  BigCorpus c;
  for (uint32_t d = 0; d < c.doc_count; ++d) {
    c.hot_docs.push_back(d);
    if (d % 9 == 0) {
      c.spark_docs.push_back(d);
      c.phrase_oracle.push_back(d);  // "hot"@0 then "spark"@1 -> phrase hit
    }
  }
  return c;
}

// Builds a TermPostings with all freq=1 and a single position per doc.
TermPostings MakePosTerm(const std::string& term, const std::vector<uint32_t>& docs,
                         uint32_t pos) {
  TermPostings tp;
  tp.term = term;
  tp.docids = docs;
  tp.freqs.assign(docs.size(), 1);
  tp.positions.resize(docs.size());
  for (size_t i = 0; i < docs.size(); ++i) tp.positions[i].push_back(pos);
  return tp;
}

SniiIndexInput MakeBigIndex(const BigCorpus& c) {
  SniiIndexInput in;
  in.index_id = 1;
  in.index_suffix = "body";
  in.config = IndexConfig::kDocsPositions;
  in.doc_count = c.doc_count;
  // One DICT block per term so every term is a block first-term (covered by the
  // sampled-term index regardless of order), mirroring the other read tests.
  in.target_dict_block_bytes = 1;
  // Terms must be lexicographically sorted.
  in.terms.push_back(MakePosTerm("hot", c.hot_docs, /*pos=*/0));    // huge -> windowed
  in.terms.push_back(MakePosTerm("rare", c.rare_docs, /*pos=*/0));  // tiny -> inline
  in.terms.push_back(MakePosTerm("spark", c.spark_docs, /*pos=*/1));  // -> windowed
  return in;
}

}  // namespace

// PHASE A read-back self-validation: a high-df term spanning MANY windows across
// MULTIPLE super-blocks. Asserts (a) sum of window doc_counts == df, (b) the
// windows tile the posting in order, (c) locate_window resolves the covering
// window, and (d) term_query / phrase_query agree with the oracle.
TEST(SniiCompoundWriter, MultiSuperBlockReadBack) {
  const BigCorpus c = MakeBigCorpus();
  const std::string path = TempPath();
  {
    io::LocalFileWriter w;
    ASSERT_TRUE(w.open(path).ok());
    SniiCompoundWriter cw(&w);
    ASSERT_TRUE(cw.add_logical_index(MakeBigIndex(c)).ok());
    ASSERT_TRUE(cw.finish().ok());
  }

  io::LocalFileReader local;
  ASSERT_TRUE(local.open(path).ok());
  io::MeteredFileReader metered(&local);
  reader::SniiSegmentReader seg;
  ASSERT_TRUE(reader::SniiSegmentReader::open(&metered, &seg).ok());
  reader::LogicalIndexReader idx;
  ASSERT_TRUE(seg.open_index(1, "body", &idx).ok());

  // Resolve "hot" to its DictEntry + the block's frq_base.
  bool found = false;
  DictEntry hot;
  uint64_t frq_base = 0, prx_base = 0;
  ASSERT_TRUE(idx.lookup("hot", &found, &hot, &frq_base, &prx_base).ok());
  ASSERT_TRUE(found);
  EXPECT_EQ(hot.df, c.doc_count);
  EXPECT_EQ(hot.enc, DictEntryEnc::kWindowed);
  EXPECT_TRUE(hot.has_sb);

  // Fetch + parse the two-level prelude.
  const uint64_t prelude_abs =
      idx.section_refs().frq_pod.offset + frq_base + hot.frq_off_delta;
  std::vector<uint8_t> prelude_bytes;
  ASSERT_TRUE(local.read_at(prelude_abs, hot.prelude_len, &prelude_bytes).ok());
  FrqPreludeReader prelude;
  ASSERT_TRUE(FrqPreludeReader::open(Slice(prelude_bytes), &prelude).ok());
  EXPECT_GT(prelude.n_super_blocks(), 1u) << "expected >1 super-block";

  // (a)+(b): decode every window's dd region from the dd-block; doc_counts sum to
  // df and the concatenated docids equal the full ascending posting [0, doc_count).
  const uint64_t dd_block_start = prelude_abs + hot.prelude_len;
  std::vector<uint32_t> tiled;
  uint64_t summed = 0;
  uint64_t expect_win_base = 0;
  for (uint32_t w = 0; w < prelude.n_windows(); ++w) {
    WindowMeta m;
    ASSERT_TRUE(prelude.window(w, &m).ok());
    EXPECT_EQ(m.win_base, expect_win_base) << "win_base w=" << w;
    std::vector<uint8_t> ddbytes;
    ASSERT_TRUE(local.read_at(dd_block_start + m.dd_off, m.dd_disk_len, &ddbytes).ok());
    std::vector<uint32_t> wdocs;
    ASSERT_TRUE(decode_dd_region(Slice(ddbytes), DdMetaOf(m), m.win_base, &wdocs).ok());
    ASSERT_EQ(wdocs.size(), m.doc_count) << "w=" << w;
    summed += m.doc_count;
    expect_win_base = m.last_docid;
    tiled.insert(tiled.end(), wdocs.begin(), wdocs.end());
  }
  EXPECT_EQ(summed, c.doc_count);
  ASSERT_EQ(tiled.size(), c.doc_count);
  EXPECT_EQ(tiled, c.hot_docs);

  // (c): locate_window returns the window actually containing the docid.
  const uint32_t probes[] = {0, 255, 256, 257, 5000, 16383, 16384, 19999};
  for (uint32_t docid : probes) {
    bool lfound = false;
    uint32_t w = 0;
    ASSERT_TRUE(prelude.locate_window(docid, &lfound, &w).ok());
    ASSERT_TRUE(lfound) << "docid=" << docid;
    WindowMeta m;
    ASSERT_TRUE(prelude.window(w, &m).ok());
    EXPECT_GE(static_cast<uint64_t>(docid), m.win_base + (w == 0 ? 0 : 1))
        << "docid=" << docid;
    EXPECT_LE(docid, m.last_docid) << "docid=" << docid;
  }
  // past-end -> not found.
  {
    bool lfound = true;
    uint32_t w = 0;
    ASSERT_TRUE(prelude.locate_window(c.doc_count, &lfound, &w).ok());
    EXPECT_FALSE(lfound);
  }

  // (d): term_query / phrase_query agree with the oracle (full-read path).
  std::vector<uint32_t> hot_docs;
  ASSERT_TRUE(query::term_query(idx, "hot", &hot_docs).ok());
  EXPECT_EQ(hot_docs, c.hot_docs);

  std::vector<uint32_t> spark_docs;
  ASSERT_TRUE(query::term_query(idx, "spark", &spark_docs).ok());
  EXPECT_EQ(spark_docs, c.spark_docs);

  std::vector<uint32_t> rare_docs;
  ASSERT_TRUE(query::term_query(idx, "rare", &rare_docs).ok());
  EXPECT_EQ(rare_docs, c.rare_docs);

  std::vector<uint32_t> phrase_docs;
  ASSERT_TRUE(query::phrase_query(idx, {"hot", "spark"}, &phrase_docs).ok());
  EXPECT_EQ(phrase_docs, c.phrase_oracle);

  // reversed phrase must be absent ("spark"@1 never precedes "hot"@0).
  std::vector<uint32_t> reversed;
  ASSERT_TRUE(query::phrase_query(idx, {"spark", "hot"}, &reversed).ok());
  EXPECT_TRUE(reversed.empty());

  std::remove(path.c_str());
}
