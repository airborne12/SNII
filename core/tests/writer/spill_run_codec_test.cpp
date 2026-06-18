#include "snii/writer/spill_run_codec.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

#include "snii/writer/spimi_term_buffer.h"

using snii::Status;
using snii::writer::MergeRuns;
using snii::writer::RunReader;
using snii::writer::RunWriter;
using snii::writer::TermPostings;

namespace {

std::string RunPath() {
  static int counter = 0;
  return "/tmp/snii_runcodec_" + std::to_string(getpid()) + "_" +
         std::to_string(counter++) + ".run";
}

// RAII temp file: removed on scope exit so the suite leaves no debris.
struct TempRun {
  std::string path = RunPath();
  ~TempRun() { std::remove(path.c_str()); }
};

// A run record is keyed by term-id; this pairs the id with the postings so the
// test can both write (by id) and assert (the resolved string round-trips).
struct IdTerm {
  uint32_t id;
  TermPostings tp;
};

TermPostings MakeTerm(std::vector<uint32_t> docids, std::vector<uint32_t> freqs,
                      std::vector<std::vector<uint32_t>> positions = {}) {
  TermPostings tp;
  tp.docids = std::move(docids);
  tp.freqs = std::move(freqs);
  tp.set_positions_per_doc(positions);  // flatten per-doc lists into positions_flat
  return tp;
}

// Writes a single run from `terms` (by id) and reads it back, asserting an exact
// round-trip of every field. The reader leaves current().term empty (runs store
// only the id), so the term-id is checked via current_id().
void RoundTrip(const std::vector<IdTerm>& terms, bool has_positions) {
  TempRun run;
  {
    RunWriter w;
    ASSERT_TRUE(w.open(run.path).ok());
    for (const auto& t : terms) ASSERT_TRUE(w.write_term(t.id, t.tp).ok());
    ASSERT_TRUE(w.close().ok());
  }
  RunReader r;
  ASSERT_TRUE(r.open(run.path, has_positions).ok());
  for (const auto& expect : terms) {
    ASSERT_FALSE(r.exhausted());
    EXPECT_EQ(r.current_id(), expect.id);
    const TermPostings& got = r.current();
    EXPECT_EQ(got.docids, expect.tp.docids);
    EXPECT_EQ(got.freqs, expect.tp.freqs);
    if (has_positions) {
      EXPECT_EQ(got.positions_flat, expect.tp.positions_flat);
    }
    ASSERT_TRUE(r.advance().ok());
  }
  EXPECT_TRUE(r.exhausted());
}

}  // namespace

// Empty run: open succeeds, immediately exhausted, merge yields nothing.
TEST(SpillRunCodec, EmptyRun) {
  TempRun run;
  RunWriter w;
  ASSERT_TRUE(w.open(run.path).ok());
  ASSERT_TRUE(w.close().ok());
  RunReader r;
  ASSERT_TRUE(r.open(run.path, /*has_positions=*/true).ok());
  EXPECT_TRUE(r.exhausted());
}

// Single doc, with positions: smallest non-trivial record round-trips.
TEST(SpillRunCodec, SingleDocWithPositions) {
  RoundTrip({{7, MakeTerm({7}, {3}, {{0, 4, 9}})}}, /*has_positions=*/true);
}

// Docs-only run (no positions): positions field is zero and decode skips it.
TEST(SpillRunCodec, NoPositions) {
  RoundTrip({{0, MakeTerm({0, 5, 99}, {1, 2, 1})},
             {1, MakeTerm({3}, {4})}},
            /*has_positions=*/false);
}

// Several terms with varied widths round-trip in ascending id order.
TEST(SpillRunCodec, MultiTermRoundTrip) {
  RoundTrip(
      {
          {0, MakeTerm({0, 1, 2}, {1, 1, 1}, {{0}, {1}, {2}})},
          {1, MakeTerm({10}, {2}, {{3, 8}})},
          {2, MakeTerm({4, 100}, {2, 1}, {{0, 1}, {7}})},
      },
      /*has_positions=*/true);
}

// K-way merge: a term-id present in EVERY run is concatenated in ascending run
// order; an id present in only ONE run passes through unchanged. The merged
// stream is ordered by each id's VOCAB STRING and the string is resolved onto
// the emitted TermPostings.
TEST(SpillRunCodec, MergeConcatenatesAcrossRuns) {
  // Vocab: id 0 -> "common", 1 -> "only0", 2 -> "zzz". Ordered by string:
  // "common" < "only0" < "zzz", which happens to match id order here.
  const std::vector<std::string> vocab = {"common", "only0", "zzz"};
  TempRun r0, r1, r2;
  // Each run covers a strictly later docid range for the shared id 0.
  {
    RunWriter w;
    ASSERT_TRUE(w.open(r0.path).ok());
    ASSERT_TRUE(w.write_term(0, MakeTerm({0, 1}, {1, 2}, {{0}, {1, 2}})).ok());
    ASSERT_TRUE(w.write_term(1, MakeTerm({3}, {1}, {{5}})).ok());
    ASSERT_TRUE(w.close().ok());
  }
  {
    RunWriter w;
    ASSERT_TRUE(w.open(r1.path).ok());
    ASSERT_TRUE(w.write_term(0, MakeTerm({5}, {1}, {{0}})).ok());
    ASSERT_TRUE(w.close().ok());
  }
  {
    RunWriter w;
    ASSERT_TRUE(w.open(r2.path).ok());
    ASSERT_TRUE(w.write_term(0, MakeTerm({8, 9}, {1, 1}, {{0}, {0}})).ok());
    ASSERT_TRUE(w.write_term(2, MakeTerm({2}, {1}, {{4}})).ok());
    ASSERT_TRUE(w.close().ok());
  }

  std::vector<TermPostings> merged;
  ASSERT_TRUE(MergeRuns({r0.path, r1.path, r2.path}, vocab, /*has_positions=*/true,
                        [&](TermPostings&& tp) { merged.push_back(std::move(tp)); })
                  .ok());

  ASSERT_EQ(merged.size(), 3u);
  EXPECT_EQ(merged[0].term, "common");
  EXPECT_EQ(merged[0].docids, (std::vector<uint32_t>{0, 1, 5, 8, 9}));
  EXPECT_EQ(merged[0].freqs, (std::vector<uint32_t>{1, 2, 1, 1, 1}));
  // Flat positions: doc0{0} doc1{1,2} doc5{0} doc8{0} doc9{0}.
  EXPECT_EQ(merged[0].positions_flat, (std::vector<uint32_t>{0, 1, 2, 0, 0, 0}));
  EXPECT_EQ(std::vector<uint32_t>(merged[0].doc_positions(1).begin(),
                                  merged[0].doc_positions(1).end()),
            (std::vector<uint32_t>{1, 2}));
  EXPECT_EQ(merged[1].term, "only0");
  EXPECT_EQ(merged[1].docids, (std::vector<uint32_t>{3}));
  EXPECT_EQ(merged[2].term, "zzz");
  EXPECT_EQ(merged[2].docids, (std::vector<uint32_t>{2}));
}

// BOUNDARY COALESCE with FLAT positions: a spill that falls BETWEEN two tokens of
// the SAME doc leaves that doc ending one run and beginning the next with the same
// docid. The merge must fold them into ONE doc whose positions concatenate (run
// order) into the correct flat layout -- the trickiest flat-positions merge path.
TEST(SpillRunCodec, MergeCoalescesBoundaryDocPositionsFlat) {
  const std::vector<std::string> vocab = {"alpha"};
  TempRun r0, r1;
  {
    RunWriter w;
    ASSERT_TRUE(w.open(r0.path).ok());
    // doc 0 (pos 0,7), doc 1 first half (pos 1) -- doc 1 continues in r1.
    ASSERT_TRUE(w.write_term(0, MakeTerm({0, 1}, {2, 1}, {{0, 7}, {1}})).ok());
    ASSERT_TRUE(w.close().ok());
  }
  {
    RunWriter w;
    ASSERT_TRUE(w.open(r1.path).ok());
    // doc 1 second half (pos 4,9), then doc 2 (pos 3).
    ASSERT_TRUE(w.write_term(0, MakeTerm({1, 2}, {2, 1}, {{4, 9}, {3}})).ok());
    ASSERT_TRUE(w.close().ok());
  }
  std::vector<TermPostings> merged;
  ASSERT_TRUE(MergeRuns({r0.path, r1.path}, vocab, /*has_positions=*/true,
                        [&](TermPostings&& tp) { merged.push_back(std::move(tp)); })
                  .ok());
  ASSERT_EQ(merged.size(), 1u);
  EXPECT_EQ(merged[0].docids, (std::vector<uint32_t>{0, 1, 2}));
  // doc 1 coalesced: freq 1 + 2 = 3, positions 1,4,9 (run order).
  EXPECT_EQ(merged[0].freqs, (std::vector<uint32_t>{2, 3, 1}));
  // Flat: doc0{0,7} doc1{1,4,9} doc2{3}.
  EXPECT_EQ(merged[0].positions_flat, (std::vector<uint32_t>{0, 7, 1, 4, 9, 3}));
  EXPECT_EQ(std::vector<uint32_t>(merged[0].doc_positions(1).begin(),
                                  merged[0].doc_positions(1).end()),
            (std::vector<uint32_t>{1, 4, 9}));
}

// The merge order follows the VOCAB STRING, not the numeric id: ids whose
// strings sort in the opposite order are emitted lexicographically.
TEST(SpillRunCodec, MergeOrdersByVocabStringNotId) {
  // id 0 -> "zebra", id 1 -> "apple": string order is apple(1) < zebra(0).
  const std::vector<std::string> vocab = {"zebra", "apple"};
  TempRun r0;
  {
    RunWriter w;
    ASSERT_TRUE(w.open(r0.path).ok());
    // Written in run order by string: apple(1) before zebra(0).
    ASSERT_TRUE(w.write_term(1, MakeTerm({2}, {1})).ok());
    ASSERT_TRUE(w.write_term(0, MakeTerm({5}, {1})).ok());
    ASSERT_TRUE(w.close().ok());
  }
  std::vector<std::string> order;
  ASSERT_TRUE(MergeRuns({r0.path}, vocab, /*has_positions=*/false,
                        [&](TermPostings&& tp) { order.push_back(tp.term); })
                  .ok());
  EXPECT_EQ(order, (std::vector<std::string>{"apple", "zebra"}));
}

// A truncated run file is rejected by decode (anti-corruption on bytes we read).
TEST(SpillRunCodec, TruncatedRunIsCorruption) {
  TempRun run;
  {
    RunWriter w;
    ASSERT_TRUE(w.open(run.path).ok());
    ASSERT_TRUE(
        w.write_term(0, MakeTerm({0, 1, 2}, {1, 1, 1}, {{0}, {0}, {0}})).ok());
    ASSERT_TRUE(w.write_term(1, MakeTerm({4}, {1}, {{0}})).ok());
    ASSERT_TRUE(w.close().ok());
  }
  // Chop the file so the second record promises more bytes than remain.
  ASSERT_EQ(::truncate(run.path.c_str(), 4), 0);
  RunReader r;
  Status s = r.open(run.path, /*has_positions=*/true);
  while (s.ok() && !r.exhausted()) s = r.advance();
  EXPECT_FALSE(s.ok());
}
