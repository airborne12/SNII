#include "snii/writer/snii_compound_writer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

#include "snii/common/status.h"
#include "snii/format/format_constants.h"
#include "snii/io/local_file.h"
#include "snii/io/metered_file_reader.h"
#include "snii/query/phrase_query.h"
#include "snii/query/term_query.h"
#include "snii/reader/logical_index_reader.h"
#include "snii/reader/snii_segment_reader.h"
#include "snii/writer/logical_index_writer.h"
#include "snii/writer/spimi_term_buffer.h"

using namespace snii;
using namespace snii::format;
using namespace snii::writer;

namespace {

std::string TempPath() {
  static int counter = 0;
  return "/tmp/snii_spill_test_" + std::to_string(getpid()) + "_" +
         std::to_string(counter++) + ".idx";
}

std::vector<uint8_t> ReadAll(const std::string& path) {
  io::LocalFileReader r;
  EXPECT_TRUE(r.open(path).ok());
  std::vector<uint8_t> out;
  EXPECT_TRUE(r.read_at(0, r.size(), &out).ok());
  return out;
}

// Deterministic (term, doc, pos) stream with globally ascending docids. Mixes
// high-df ("alpha", every doc), mid-df, multi-token docs, and a term whose docs
// straddle arbitrary spill boundaries -- so the spill path exercises a term in
// one run, a term in every run, and a spill boundary mid-term.
void Feed(SpimiTermBuffer* buf, uint32_t doc_count) {
  for (uint32_t d = 0; d < doc_count; ++d) {
    buf->add_token("alpha", d, 0);                  // every doc (spans all runs)
    buf->add_token("alpha", d, 7);                  // freq 2 in every doc
    if (d % 2 == 0) buf->add_token("beta", d, 1);
    if (d % 3 == 0) buf->add_token("gamma", d, 2);
    if (d % 5 == 0) {
      buf->add_token("delta", d, 3);
      buf->add_token("delta", d, 9);
    }
    if (d == 1) buf->add_token("singleton", d, 4);  // only one doc, one run
  }
}

SniiIndexInput BaseInput(uint32_t doc_count) {
  SniiIndexInput in;
  in.index_id = 1;
  in.index_suffix = "body";
  in.config = IndexConfig::kDocsPositions;
  in.doc_count = doc_count;
  in.target_dict_block_bytes = 512;  // force several DICT blocks
  return in;
}

std::vector<uint8_t> WriteContainer(const SniiIndexInput& in) {
  const std::string path = TempPath();
  io::LocalFileWriter writer;
  EXPECT_TRUE(writer.open(path).ok());
  SniiCompoundWriter compound(&writer);
  EXPECT_TRUE(compound.add_logical_index(in).ok());
  EXPECT_TRUE(compound.finish().ok());
  std::vector<uint8_t> bytes = ReadAll(path);
  std::remove(path.c_str());
  return bytes;
}

// Builds a container by STREAMING a freshly-fed buffer at the given spill
// threshold (0 == unlimited), returning the container bytes.
std::vector<uint8_t> BuildStreamed(uint32_t docs, size_t spill_bytes) {
  SpimiTermBuffer buf(/*has_positions=*/true, spill_bytes);
  Feed(&buf, docs);
  SniiIndexInput in = BaseInput(docs);
  in.term_source = &buf;
  return WriteContainer(in);
}

// Opens a container from bytes and runs a couple of queries for a sanity match.
struct OpenedIndex {
  std::string path;
  io::LocalFileReader local;
  std::unique_ptr<io::MeteredFileReader> metered;
  reader::SniiSegmentReader segment;
  reader::LogicalIndexReader index;

  ~OpenedIndex() {
    if (!path.empty()) std::remove(path.c_str());
  }
};

void OpenFromBytes(const std::vector<uint8_t>& bytes, OpenedIndex* out) {
  out->path = TempPath();
  io::LocalFileWriter w;
  ASSERT_TRUE(w.open(out->path).ok());
  ASSERT_TRUE(w.append(Slice(bytes)).ok());
  ASSERT_TRUE(w.finalize().ok());
  ASSERT_TRUE(out->local.open(out->path).ok());
  out->metered = std::make_unique<io::MeteredFileReader>(&out->local);
  ASSERT_TRUE(reader::SniiSegmentReader::open(out->metered.get(), &out->segment).ok());
  ASSERT_TRUE(out->segment.open_index(1, "body", &out->index).ok());
}

std::vector<uint32_t> TermDocs(OpenedIndex* idx, const std::string& term) {
  std::vector<uint32_t> docs;
  const Status s = query::term_query(idx->index, term, &docs);
  EXPECT_TRUE(s.ok()) << "term=" << term << " err=" << s.to_string();
  return docs;
}

}  // namespace

// CORE GUARANTEE: building the SAME corpus with a tiny spill threshold (forcing
// many spills + a final k-way merge) yields a container that is BYTE-FOR-BYTE
// identical to the unlimited in-memory build.
TEST(SpimiSpillWriter, SpilledMatchesUnlimitedBytes) {
  constexpr uint32_t kDocs = 400;
  const std::vector<uint8_t> unlimited = BuildStreamed(kDocs, /*spill=*/0);
  // ~4 KiB threshold forces dozens of spills across this corpus.
  const std::vector<uint8_t> spilled = BuildStreamed(kDocs, /*spill=*/4096);

  ASSERT_EQ(unlimited.size(), spilled.size());
  EXPECT_EQ(unlimited, spilled);
}

// Identical bytes regardless of threshold size: a mid threshold (one or two
// spills) must also match the unlimited build exactly.
TEST(SpimiSpillWriter, MidThresholdAlsoIdentical) {
  constexpr uint32_t kDocs = 400;
  const std::vector<uint8_t> unlimited = BuildStreamed(kDocs, /*spill=*/0);
  const std::vector<uint8_t> mid = BuildStreamed(kDocs, /*spill=*/32 * 1024);
  EXPECT_EQ(unlimited, mid);
}

// An extremely small threshold (spill almost every token) still produces the
// identical container -- stresses many single-term runs and the merge.
TEST(SpimiSpillWriter, ExtremeSpillIdentical) {
  constexpr uint32_t kDocs = 200;
  const std::vector<uint8_t> unlimited = BuildStreamed(kDocs, /*spill=*/0);
  const std::vector<uint8_t> tiny = BuildStreamed(kDocs, /*spill=*/1);
  EXPECT_EQ(unlimited, tiny);
}

// No-positions config with spilling still matches the in-memory build.
TEST(SpimiSpillWriter, NoPositionsSpillIdentical) {
  constexpr uint32_t kDocs = 300;
  auto build = [&](size_t spill) {
    SpimiTermBuffer buf(/*has_positions=*/false, spill);
    for (uint32_t d = 0; d < kDocs; ++d) {
      buf.add_token("alpha", d, 0);
      if (d % 2 == 0) buf.add_token("beta", d, 0);
      if (d % 4 == 0) buf.add_token("gamma", d, 0);
    }
    SniiIndexInput in = BaseInput(kDocs);
    in.config = IndexConfig::kDocsOnly;  // docids only (no positions section)
    in.term_source = &buf;
    return WriteContainer(in);
  };
  EXPECT_EQ(build(0), build(2048));
}

// Queries against the spilled and unlimited containers return identical results.
// Uses >= kSlimDfThreshold docs so the high-df term is windowed (populating the
// .prx POD) -- the segment reader infers has_positions from a non-empty prx POD.
TEST(SpimiSpillWriter, QueriesMatchAcrossSpill) {
  constexpr uint32_t kDocs = 700;
  OpenedIndex un, sp;
  OpenFromBytes(BuildStreamed(kDocs, 0), &un);
  OpenFromBytes(BuildStreamed(kDocs, 4096), &sp);

  for (const char* term : {"alpha", "beta", "gamma", "delta", "singleton", "absent"}) {
    EXPECT_EQ(TermDocs(&un, term), TermDocs(&sp, term)) << "term=" << term;
  }
  std::vector<uint32_t> un_phrase, sp_phrase;
  ASSERT_TRUE(query::phrase_query(un.index, {"alpha", "alpha"}, &un_phrase).ok());
  ASSERT_TRUE(query::phrase_query(sp.index, {"alpha", "alpha"}, &sp_phrase).ok());
  EXPECT_EQ(un_phrase, sp_phrase);
}

// Single-doc corpus with spilling: an edge corpus must not crash or diverge.
TEST(SpimiSpillWriter, SingleDocCorpus) {
  SpimiTermBuffer un(/*has_positions=*/true, 0);
  SpimiTermBuffer sp(/*has_positions=*/true, 1);
  for (auto* b : {&un, &sp}) {
    b->add_token("only", 0, 0);
    b->add_token("only", 0, 1);
    b->add_token("word", 0, 2);
  }
  SniiIndexInput un_in = BaseInput(1);
  un_in.term_source = &un;
  SniiIndexInput sp_in = BaseInput(1);
  sp_in.term_source = &sp;
  EXPECT_EQ(WriteContainer(un_in), WriteContainer(sp_in));
}

// finalize_sorted (the materialized accessor) also reflects spilled runs and is
// byte-identical to the in-memory result at the postings level.
TEST(SpimiSpillWriter, FinalizeSortedMatchesAcrossSpill) {
  constexpr uint32_t kDocs = 150;
  SpimiTermBuffer un(/*has_positions=*/true, 0);
  SpimiTermBuffer sp(/*has_positions=*/true, 256);
  Feed(&un, kDocs);
  Feed(&sp, kDocs);
  const std::vector<TermPostings> a = un.finalize_sorted();
  const std::vector<TermPostings> b = sp.finalize_sorted();
  ASSERT_EQ(a.size(), b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i].term, b[i].term);
    EXPECT_EQ(a[i].docids, b[i].docids);
    EXPECT_EQ(a[i].freqs, b[i].freqs);
    EXPECT_EQ(a[i].positions, b[i].positions);
  }
  EXPECT_TRUE(un.status().ok());
  EXPECT_TRUE(sp.status().ok());
}
