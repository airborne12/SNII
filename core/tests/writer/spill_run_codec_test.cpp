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

TermPostings MakeTerm(const std::string& term, std::vector<uint32_t> docids,
                      std::vector<uint32_t> freqs,
                      std::vector<std::vector<uint32_t>> positions = {}) {
  TermPostings tp;
  tp.term = term;
  tp.docids = std::move(docids);
  tp.freqs = std::move(freqs);
  tp.positions = std::move(positions);
  return tp;
}

// Writes a single run from `terms` and reads it back, asserting an exact
// round-trip of every field.
void RoundTrip(const std::vector<TermPostings>& terms, bool has_positions) {
  TempRun run;
  {
    RunWriter w;
    ASSERT_TRUE(w.open(run.path).ok());
    for (const auto& t : terms) ASSERT_TRUE(w.write_term(t).ok());
    ASSERT_TRUE(w.close().ok());
  }
  RunReader r;
  ASSERT_TRUE(r.open(run.path, has_positions).ok());
  for (const auto& expect : terms) {
    ASSERT_FALSE(r.exhausted());
    const TermPostings& got = r.current();
    EXPECT_EQ(got.term, expect.term);
    EXPECT_EQ(got.docids, expect.docids);
    EXPECT_EQ(got.freqs, expect.freqs);
    if (has_positions) {
      EXPECT_EQ(got.positions, expect.positions);
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
  RoundTrip({MakeTerm("solo", {7}, {3}, {{0, 4, 9}})}, /*has_positions=*/true);
}

// Docs-only run (no positions): positions field is zero and decode skips it.
TEST(SpillRunCodec, NoPositions) {
  RoundTrip({MakeTerm("a", {0, 5, 99}, {1, 2, 1}),
             MakeTerm("b", {3}, {4})},
            /*has_positions=*/false);
}

// Several terms with varied widths round-trip in ascending term order.
TEST(SpillRunCodec, MultiTermRoundTrip) {
  RoundTrip(
      {
          MakeTerm("alpha", {0, 1, 2}, {1, 1, 1}, {{0}, {1}, {2}}),
          MakeTerm("beta", {10}, {2}, {{3, 8}}),
          MakeTerm("gamma", {4, 100}, {2, 1}, {{0, 1}, {7}}),
      },
      /*has_positions=*/true);
}

// K-way merge: a term present in EVERY run is concatenated in ascending run
// order; a term present in only ONE run passes through unchanged.
TEST(SpillRunCodec, MergeConcatenatesAcrossRuns) {
  TempRun r0, r1, r2;
  // Each run covers a strictly later docid range for the shared term "common".
  {
    RunWriter w;
    ASSERT_TRUE(w.open(r0.path).ok());
    ASSERT_TRUE(w.write_term(MakeTerm("common", {0, 1}, {1, 2}, {{0}, {1, 2}})).ok());
    ASSERT_TRUE(w.write_term(MakeTerm("only0", {3}, {1}, {{5}})).ok());
    ASSERT_TRUE(w.close().ok());
  }
  {
    RunWriter w;
    ASSERT_TRUE(w.open(r1.path).ok());
    ASSERT_TRUE(w.write_term(MakeTerm("common", {5}, {1}, {{0}})).ok());
    ASSERT_TRUE(w.close().ok());
  }
  {
    RunWriter w;
    ASSERT_TRUE(w.open(r2.path).ok());
    ASSERT_TRUE(w.write_term(MakeTerm("common", {8, 9}, {1, 1}, {{0}, {0}})).ok());
    ASSERT_TRUE(w.write_term(MakeTerm("zzz", {2}, {1}, {{4}})).ok());
    ASSERT_TRUE(w.close().ok());
  }

  std::vector<TermPostings> merged;
  ASSERT_TRUE(MergeRuns({r0.path, r1.path, r2.path}, /*has_positions=*/true,
                        [&](TermPostings&& tp) { merged.push_back(std::move(tp)); })
                  .ok());

  ASSERT_EQ(merged.size(), 3u);
  EXPECT_EQ(merged[0].term, "common");
  EXPECT_EQ(merged[0].docids, (std::vector<uint32_t>{0, 1, 5, 8, 9}));
  EXPECT_EQ(merged[0].freqs, (std::vector<uint32_t>{1, 2, 1, 1, 1}));
  EXPECT_EQ(merged[0].positions[1], (std::vector<uint32_t>{1, 2}));
  EXPECT_EQ(merged[1].term, "only0");
  EXPECT_EQ(merged[1].docids, (std::vector<uint32_t>{3}));
  EXPECT_EQ(merged[2].term, "zzz");
  EXPECT_EQ(merged[2].docids, (std::vector<uint32_t>{2}));
}

// A truncated run file is rejected by decode (anti-corruption on bytes we read).
TEST(SpillRunCodec, TruncatedRunIsCorruption) {
  TempRun run;
  {
    RunWriter w;
    ASSERT_TRUE(w.open(run.path).ok());
    ASSERT_TRUE(w.write_term(MakeTerm("term", {0, 1, 2}, {1, 1, 1}, {{0}, {0}, {0}})).ok());
    ASSERT_TRUE(w.close().ok());
  }
  // Chop the file to its first byte (a term_len that promises more than exists).
  ASSERT_EQ(::truncate(run.path.c_str(), 1), 0);
  RunReader r;
  const Status s = r.open(run.path, /*has_positions=*/true);
  EXPECT_FALSE(s.ok());
}
