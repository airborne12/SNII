#include "snii/writer/spill_run_codec.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

#include "snii/encoding/varint.h"
#include "snii/writer/spimi_term_buffer.h"

using snii::Status;
using snii::StatusCode;
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
    // Positions are LAZY: the count is known after advance(), the bytes only after
    // materialize_positions().
    EXPECT_EQ(r.current_pos_count(), expect.tp.positions_flat.size());
    ASSERT_TRUE(r.materialize_positions().ok());
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

// DoS prevention: a corrupt/truncated run whose n_docs length varint decodes to an
// absurd value must yield Status::Corruption (bounded by the run's file size), NOT an
// uncaught std::bad_alloc from read_raw_u32's resize(). No docid data follows the
// huge count, so without the file-size bound this would resize() to ~4e9 u32s.
TEST(SpillRunCodec, CorruptDocCountIsCorruptionNotBadAlloc) {
  TempRun run;
  {
    std::FILE* f = std::fopen(run.path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    uint8_t buf[16];
    size_t n = 0;
    n += snii::encode_varint64(0, buf + n);              // term_id = 0
    n += snii::encode_varint64(0xFFFFFFFFull, buf + n);  // n_docs ~= 4e9, no data follows
    ASSERT_EQ(std::fwrite(buf, 1, n, f), n);
    std::fclose(f);
  }
  RunReader r;
  const Status s = r.open(run.path, /*has_positions=*/false);  // open() -> advance()
  EXPECT_EQ(s.code(), StatusCode::kCorruption) << s.message();
}

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

// Lazy positions: stream_positions yields the SAME bytes as the materialized
// block, even when pulled in awkward (non-block-aligned) chunk sizes that straddle
// the reader's internal 64 KiB window boundaries.
TEST(SpillRunCodec, StreamPositionsMatchesMaterialized) {
  TempRun run;
  // One wide term: 5000 docs, freq 3 each -> 15000 flat positions spanning several
  // internal read windows.
  std::vector<uint32_t> docids, freqs, flat;
  for (uint32_t d = 0; d < 5000; ++d) {
    docids.push_back(d);
    freqs.push_back(3);
    flat.push_back(d * 7 + 0);
    flat.push_back(d * 7 + 1);
    flat.push_back(d * 7 + 2);
  }
  TermPostings tp;
  tp.docids = docids;
  tp.freqs = freqs;
  tp.positions_flat = flat;
  {
    RunWriter w;
    ASSERT_TRUE(w.open(run.path).ok());
    ASSERT_TRUE(w.write_term(0, tp).ok());
    ASSERT_TRUE(w.close().ok());
  }
  RunReader r;
  ASSERT_TRUE(r.open(run.path, /*has_positions=*/true).ok());
  ASSERT_EQ(r.current_pos_count(), flat.size());
  ASSERT_EQ(r.positions_remaining(), flat.size());
  // Pull in odd chunks (7, 1000, 7, 1000, ...) until drained.
  std::vector<uint32_t> got;
  std::vector<size_t> chunks = {7, 1000, 7, 1000};
  size_t ci = 0;
  while (r.positions_remaining() > 0) {
    size_t want = std::min<size_t>(chunks[ci % chunks.size()],
                                   static_cast<size_t>(r.positions_remaining()));
    ++ci;
    std::vector<uint32_t> buf(want);
    ASSERT_TRUE(r.stream_positions(buf.data(), want).ok());
    got.insert(got.end(), buf.begin(), buf.end());
  }
  EXPECT_EQ(got, flat);
  EXPECT_TRUE(r.positions_drained());
  ASSERT_TRUE(r.advance().ok());
  EXPECT_TRUE(r.exhausted());
}

// advance() after a PARTIALLY-streamed term skips the unread positions and lands
// on the next record correctly.
TEST(SpillRunCodec, PartialStreamThenAdvanceSkipsRemainder) {
  TempRun run;
  {
    RunWriter w;
    ASSERT_TRUE(w.open(run.path).ok());
    ASSERT_TRUE(w.write_term(0, MakeTerm({0, 1, 2}, {2, 2, 2},
                                         {{10, 11}, {20, 21}, {30, 31}}))
                    .ok());
    ASSERT_TRUE(w.write_term(1, MakeTerm({9}, {1}, {{99}})).ok());
    ASSERT_TRUE(w.close().ok());
  }
  RunReader r;
  ASSERT_TRUE(r.open(run.path, /*has_positions=*/true).ok());
  ASSERT_EQ(r.current_id(), 0u);
  // Pull only the first two positions, then advance -- the remaining 4 are skipped.
  std::vector<uint32_t> buf(2);
  ASSERT_TRUE(r.stream_positions(buf.data(), 2).ok());
  EXPECT_EQ(buf, (std::vector<uint32_t>{10, 11}));
  ASSERT_TRUE(r.advance().ok());
  ASSERT_FALSE(r.exhausted());
  EXPECT_EQ(r.current_id(), 1u);
  ASSERT_TRUE(r.materialize_positions().ok());
  EXPECT_EQ(r.current().positions_flat, (std::vector<uint32_t>{99}));
}

// Drains a streamed merge term's pos_pump into a flat buffer (mirrors the windowed
// writer's synchronous consumption). Returns the merged term with positions
// realized so tests can compare against the materialized path.
TermPostings DrainStreamed(TermPostings&& tp) {
  if (tp.pos_pump) {
    tp.positions_flat.resize(static_cast<size_t>(tp.pos_total));
    if (tp.pos_total != 0) tp.pos_pump(tp.positions_flat.data(),
                                       static_cast<size_t>(tp.pos_total));
    tp.pos_pump = nullptr;
  }
  return std::move(tp);
}

// WIDE-TERM STREAMING == MATERIALIZED (byte-identity proof at the postings level):
// a term with df >= kSlimDfThreshold split across several runs (with a boundary doc
// straddling a spill) must yield IDENTICAL docids/freqs/positions whether the merge
// streams positions via pos_pump (allow_stream=true) or materializes them
// (allow_stream=false). Pulling the pump in document order reproduces the exact
// coalesced positions_flat.
TEST(SpillRunCodec, MergeWideTermStreamsIdenticalToMaterialized) {
  const std::vector<std::string> vocab = {"wide"};
  // Build a wide term (df ~ 2000) sharded across 3 runs, with the LAST doc of each
  // run continuing as the FIRST doc of the next (boundary-doc coalesce).
  TempRun r0, r1, r2;
  auto shard = [&](TempRun& run, uint32_t lo, uint32_t hi, uint32_t carry_first) {
    TermPostings tp;
    for (uint32_t d = lo; d < hi; ++d) {
      tp.docids.push_back(d);
      // Boundary docs (lo when it's a carry) get freq 1 here; otherwise freq 2.
      const uint32_t fc = 2;
      tp.freqs.push_back(fc);
      for (uint32_t k = 0; k < fc; ++k) tp.positions_flat.push_back(d * 13 + k);
    }
    (void)carry_first;
    RunWriter w;
    ASSERT_TRUE(w.open(run.path).ok());
    ASSERT_TRUE(w.write_term(0, tp).ok());
    ASSERT_TRUE(w.close().ok());
  };
  // Ranges chosen so doc 700 ends r0 AND begins r1 (boundary), doc 1400 likewise.
  // Encode the boundary by repeating that docid at the seam with extra positions.
  {
    TermPostings a;
    for (uint32_t d = 0; d <= 700; ++d) {
      a.docids.push_back(d);
      a.freqs.push_back(2);
      a.positions_flat.push_back(d * 13);
      a.positions_flat.push_back(d * 13 + 1);
    }
    RunWriter w;
    ASSERT_TRUE(w.open(r0.path).ok());
    ASSERT_TRUE(w.write_term(0, a).ok());
    ASSERT_TRUE(w.close().ok());
  }
  {
    TermPostings b;
    // doc 700 continues here (boundary): extra positions for it, then 701..1400.
    b.docids.push_back(700);
    b.freqs.push_back(1);
    b.positions_flat.push_back(700 * 13 + 2);
    for (uint32_t d = 701; d <= 1400; ++d) {
      b.docids.push_back(d);
      b.freqs.push_back(2);
      b.positions_flat.push_back(d * 13);
      b.positions_flat.push_back(d * 13 + 1);
    }
    RunWriter w;
    ASSERT_TRUE(w.open(r1.path).ok());
    ASSERT_TRUE(w.write_term(0, b).ok());
    ASSERT_TRUE(w.close().ok());
  }
  {
    TermPostings c;
    c.docids.push_back(1400);
    c.freqs.push_back(1);
    c.positions_flat.push_back(1400 * 13 + 2);
    for (uint32_t d = 1401; d <= 2100; ++d) {
      c.docids.push_back(d);
      c.freqs.push_back(2);
      c.positions_flat.push_back(d * 13);
      c.positions_flat.push_back(d * 13 + 1);
    }
    RunWriter w;
    ASSERT_TRUE(w.open(r2.path).ok());
    ASSERT_TRUE(w.write_term(0, c).ok());
    ASSERT_TRUE(w.close().ok());
  }
  (void)shard;

  const std::vector<std::string> paths = {r0.path, r1.path, r2.path};
  TermPostings materialized, streamed;
  ASSERT_TRUE(MergeRuns(paths, vocab, /*has_positions=*/true,
                        [&](TermPostings&& tp) { materialized = std::move(tp); },
                        /*allow_stream_positions=*/false)
                  .ok());
  ASSERT_TRUE(MergeRuns(paths, vocab, /*has_positions=*/true,
                        [&](TermPostings&& tp) { streamed = DrainStreamed(std::move(tp)); },
                        /*allow_stream_positions=*/true)
                  .ok());

  // The materialized path filled positions_flat; the streamed path must too (after
  // draining the pump) -- identical docids, freqs, and positions.
  EXPECT_GE(materialized.docids.size(), 512u);  // wide enough to take the stream path
  EXPECT_EQ(materialized.docids, streamed.docids);
  EXPECT_EQ(materialized.freqs, streamed.freqs);
  EXPECT_EQ(materialized.positions_flat, streamed.positions_flat);
  // Boundary doc 700 coalesced: freq 2 (r0) + 1 (r1) = 3, positions in run order.
  const auto it = std::find(materialized.docids.begin(), materialized.docids.end(), 700u);
  ASSERT_NE(it, materialized.docids.end());
  const size_t bi = static_cast<size_t>(it - materialized.docids.begin());
  EXPECT_EQ(materialized.freqs[bi], 3u);
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
