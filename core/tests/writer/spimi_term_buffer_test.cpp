#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

#include "snii/writer/spimi_term_buffer.h"

using snii::writer::SpimiTermBuffer;
using snii::writer::TermPostings;

// Tokens accumulate into sorted terms with ascending docids and per-doc positions.
TEST(SpimiTermBuffer, AccumulateAndSort) {
  SpimiTermBuffer buf(/*has_positions=*/true);
  // doc 0: "banana apple apple"
  buf.add_token("banana", 0, 0);
  buf.add_token("apple", 0, 1);
  buf.add_token("apple", 0, 2);
  // doc 1: "apple cherry"
  buf.add_token("apple", 1, 0);
  buf.add_token("cherry", 1, 1);

  EXPECT_EQ(buf.unique_terms(), 3u);
  EXPECT_EQ(buf.total_tokens(), 5u);

  std::vector<TermPostings> terms = buf.finalize_sorted();
  ASSERT_EQ(terms.size(), 3u);
  // Sorted lexicographically: apple, banana, cherry.
  EXPECT_EQ(terms[0].term, "apple");
  EXPECT_EQ(terms[1].term, "banana");
  EXPECT_EQ(terms[2].term, "cherry");

  // apple: docs 0 (freq 2, pos {1,2}) and 1 (freq 1, pos {0}).
  const TermPostings& apple = terms[0];
  ASSERT_EQ(apple.docids.size(), 2u);
  EXPECT_EQ(apple.docids[0], 0u);
  EXPECT_EQ(apple.freqs[0], 2u);
  ASSERT_EQ(apple.doc_positions(0).size(), 2u);
  EXPECT_EQ(apple.doc_positions(0)[0], 1u);
  EXPECT_EQ(apple.doc_positions(0)[1], 2u);
  EXPECT_EQ(apple.docids[1], 1u);
  EXPECT_EQ(apple.freqs[1], 1u);
}

// Without positions, freq is still counted but positions vectors stay empty.
TEST(SpimiTermBuffer, DocsOnlyNoPositions) {
  SpimiTermBuffer buf(/*has_positions=*/false);
  buf.add_token("x", 0, 0);
  buf.add_token("x", 0, 1);
  buf.add_token("x", 2, 0);

  std::vector<TermPostings> terms = buf.finalize_sorted();
  ASSERT_EQ(terms.size(), 1u);
  EXPECT_EQ(terms[0].term, "x");
  ASSERT_EQ(terms[0].docids.size(), 2u);
  EXPECT_EQ(terms[0].docids[0], 0u);
  EXPECT_EQ(terms[0].freqs[0], 2u);
  EXPECT_EQ(terms[0].docids[1], 2u);
  EXPECT_EQ(terms[0].freqs[1], 1u);
  EXPECT_TRUE(terms[0].positions_flat.empty());
}

TEST(SpimiTermBuffer, Empty) {
  SpimiTermBuffer buf(true);
  EXPECT_EQ(buf.unique_terms(), 0u);
  EXPECT_TRUE(buf.finalize_sorted().empty());
}

// Feeds the same token stream into two buffers and asserts the streaming
// for_each_term_sorted produces the byte-identical postings finalize_sorted
// returns (same flat-array refactor must not change observable output).
TEST(SpimiTermBuffer, StreamingMatchesMaterialized) {
  auto feed = [](SpimiTermBuffer& b) {
    b.add_token("banana", 0, 0);
    b.add_token("apple", 0, 1);
    b.add_token("apple", 0, 2);
    b.add_token("apple", 1, 0);
    b.add_token("cherry", 1, 1);
    b.add_token("apple", 5, 3);
    b.add_token("apple", 5, 7);
    b.add_token("banana", 9, 0);
  };
  SpimiTermBuffer mat(/*has_positions=*/true);
  SpimiTermBuffer strm(/*has_positions=*/true);
  feed(mat);
  feed(strm);

  std::vector<TermPostings> material = mat.finalize_sorted();
  std::vector<TermPostings> streamed;
  EXPECT_TRUE(strm.for_each_term_sorted([&](TermPostings&& tp) { streamed.push_back(std::move(tp)); }).ok());

  ASSERT_EQ(material.size(), streamed.size());
  for (size_t i = 0; i < material.size(); ++i) {
    EXPECT_EQ(material[i].term, streamed[i].term);
    EXPECT_EQ(material[i].docids, streamed[i].docids);
    EXPECT_EQ(material[i].freqs, streamed[i].freqs);
    EXPECT_EQ(material[i].positions_flat, streamed[i].positions_flat);
  }
  // apple: docs {0(pos 1,2), 1(pos 0), 5(pos 3,7)} -> positions re-sliced by freq.
  ASSERT_EQ(streamed[0].term, "apple");
  ASSERT_EQ(streamed[0].docids.size(), 3u);
  EXPECT_EQ(streamed[0].freqs, (std::vector<uint32_t>{2u, 1u, 2u}));
  EXPECT_EQ(std::vector<uint32_t>(streamed[0].doc_positions(2).begin(),
                                  streamed[0].doc_positions(2).end()),
            (std::vector<uint32_t>{3u, 7u}));
}

// Out-of-order docid GROUPS (each doc's tokens stay contiguous, but the docids
// are not non-decreasing) are tolerated and reordered once at finalize, with
// each doc carrying its own positions (defensive fallback path, e.g. a merge of
// pre-sorted runs). Tokens for a single docid are always contiguous.
TEST(SpimiTermBuffer, OutOfOrderDocidsSortedAtFinalize) {
  SpimiTermBuffer buf(/*has_positions=*/true);
  buf.add_token("t", 5, 50);  // doc 5 group (contiguous)
  buf.add_token("t", 5, 51);
  buf.add_token("t", 1, 10);  // doc 1 group, arrives after doc 5
  buf.add_token("t", 1, 11);
  buf.add_token("t", 3, 30);  // doc 3 group

  std::vector<TermPostings> terms = buf.finalize_sorted();
  ASSERT_EQ(terms.size(), 1u);
  const TermPostings& t = terms[0];
  EXPECT_EQ(t.docids, (std::vector<uint32_t>{1u, 3u, 5u}));
  EXPECT_EQ(t.freqs, (std::vector<uint32_t>{2u, 1u, 2u}));
  EXPECT_EQ(t.positions_flat, (std::vector<uint32_t>{10u, 11u, 30u, 50u, 51u}));
}

// BORROWED-vocab id path: feeding raw term-ids (no per-token string work)
// produces the SAME lexicographically sorted postings as the string path. The
// vocab order (apple < banana < cherry) drives the emitted order, NOT the id
// order (banana=0, apple=1, cherry=2).
TEST(SpimiTermBuffer, TermIdPathMatchesStringPath) {
  const std::vector<std::string> vocab = {"banana", "apple", "cherry"};
  SpimiTermBuffer buf(&vocab, /*has_positions=*/true);
  // doc 0: "banana apple apple", doc 1: "apple cherry" -- by id.
  buf.add_token(0, 0, 0);  // banana
  buf.add_token(1, 0, 1);  // apple
  buf.add_token(1, 0, 2);  // apple
  buf.add_token(1, 1, 0);  // apple
  buf.add_token(2, 1, 1);  // cherry

  EXPECT_EQ(buf.unique_terms(), 3u);
  EXPECT_EQ(buf.total_tokens(), 5u);
  EXPECT_TRUE(buf.status().ok());

  std::vector<TermPostings> terms = buf.finalize_sorted();
  ASSERT_EQ(terms.size(), 3u);
  EXPECT_EQ(terms[0].term, "apple");
  EXPECT_EQ(terms[1].term, "banana");
  EXPECT_EQ(terms[2].term, "cherry");
  const TermPostings& apple = terms[0];
  ASSERT_EQ(apple.docids.size(), 2u);
  EXPECT_EQ(apple.freqs[0], 2u);
  EXPECT_EQ(std::vector<uint32_t>(apple.doc_positions(0).begin(),
                                 apple.doc_positions(0).end()),
            (std::vector<uint32_t>{1u, 2u}));
  EXPECT_EQ(apple.docids[1], 1u);
  EXPECT_EQ(apple.freqs[1], 1u);
}

// A term-id never touched is simply skipped (no empty term emitted); an empty
// vocab yields no terms and stays valid.
TEST(SpimiTermBuffer, UntouchedIdSkippedAndEmptyVocab) {
  const std::vector<std::string> vocab = {"a", "b", "c", "d"};
  SpimiTermBuffer buf(&vocab, /*has_positions=*/false);
  buf.add_token(0, 0, 0);  // a
  buf.add_token(2, 1, 0);  // c -- ids 1 (b) and 3 (d) never touched
  std::vector<TermPostings> terms = buf.finalize_sorted();
  ASSERT_EQ(terms.size(), 2u);
  EXPECT_EQ(terms[0].term, "a");
  EXPECT_EQ(terms[1].term, "c");

  const std::vector<std::string> empty;
  SpimiTermBuffer empty_buf(&empty, /*has_positions=*/false);
  EXPECT_EQ(empty_buf.unique_terms(), 0u);
  EXPECT_TRUE(empty_buf.finalize_sorted().empty());
  EXPECT_TRUE(empty_buf.status().ok());
}

// An out-of-range term-id is rejected: the token is ignored and an
// InvalidArgument is latched into status().
TEST(SpimiTermBuffer, OutOfRangeTermIdRejected) {
  const std::vector<std::string> vocab = {"x", "y"};
  SpimiTermBuffer buf(&vocab, /*has_positions=*/true);
  buf.add_token(0, 0, 0);  // valid
  buf.add_token(5, 0, 1);  // out of range -> ignored + latched
  EXPECT_FALSE(buf.status().ok());
  EXPECT_EQ(buf.unique_terms(), 1u);
  EXPECT_EQ(buf.total_tokens(), 1u);  // the rejected token was not counted
}

// The borrowed-vocab id path is byte-identical across a spill: a tiny threshold
// (many spills + k-way merge over term-id runs) must match the unlimited build.
TEST(SpimiTermBuffer, TermIdSpillMatchesUnlimited) {
  const std::vector<std::string> vocab = {"alpha", "beta", "gamma", "delta"};
  auto feed = [&](SpimiTermBuffer& b) {
    for (uint32_t d = 0; d < 300; ++d) {
      b.add_token(0, d, 0);                 // alpha: every doc
      b.add_token(0, d, 9);                 // freq 2
      if (d % 2 == 0) b.add_token(1, d, 1);  // beta
      if (d % 3 == 0) b.add_token(2, d, 2);  // gamma
      if (d % 5 == 0) b.add_token(3, d, 3);  // delta
    }
  };
  SpimiTermBuffer un(&vocab, /*has_positions=*/true, /*spill=*/0);
  SpimiTermBuffer sp(&vocab, /*has_positions=*/true, /*spill=*/256);
  feed(un);
  feed(sp);
  const std::vector<TermPostings> a = un.finalize_sorted();
  const std::vector<TermPostings> b = sp.finalize_sorted();
  ASSERT_EQ(a.size(), b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i].term, b[i].term);
    EXPECT_EQ(a[i].docids, b[i].docids);
    EXPECT_EQ(a[i].freqs, b[i].freqs);
    EXPECT_EQ(a[i].positions_flat, b[i].positions_flat);
  }
  EXPECT_TRUE(un.status().ok());
  EXPECT_TRUE(sp.status().ok());
}

// for_each_term_sorted drains the buffer term-by-term: after each callback the
// consumed term is gone, so at most one term's arrays remain materialized.
TEST(SpimiTermBuffer, StreamingDrainsAndShrinks) {
  SpimiTermBuffer buf(/*has_positions=*/false);
  for (uint32_t d = 0; d < 100; ++d) {
    buf.add_token("a", d, 0);
    buf.add_token("b", d, 0);
    buf.add_token("c", d, 0);
  }
  EXPECT_EQ(buf.unique_terms(), 3u);
  std::vector<size_t> remaining_after_each;
  size_t seen = 0;
  EXPECT_TRUE(buf.for_each_term_sorted([&](TermPostings&& tp) {
    ++seen;
    EXPECT_EQ(tp.docids.size(), 100u);
    remaining_after_each.push_back(buf.unique_terms());
  }).ok());
  EXPECT_EQ(seen, 3u);
  // After consuming each of the 3 terms, the live count drops 2,1,0.
  EXPECT_EQ(remaining_after_each, (std::vector<size_t>{2u, 1u, 0u}));
  EXPECT_EQ(buf.unique_terms(), 0u);
}

// A REVISITED docid (the out-of-order defensive path actually re-touches a doc:
// feed 5,1,5) MUST coalesce into ONE entry per docid -- summed freq, positions
// concatenated in document order -- matching the k-way merge path and the writer's
// strictly-ascending precondition. Without coalescing this yielded docids
// {1,5,5} (duplicate, unsorted) that the writer later rejects.
TEST(SpimiTermBuffer, RevisitedDocidCoalescesWithPositions) {
  SpimiTermBuffer buf(/*has_positions=*/true);
  buf.add_token("t", 5, 50);  // doc 5, first visit
  buf.add_token("t", 5, 51);
  buf.add_token("t", 1, 10);  // doc 1
  buf.add_token("t", 5, 52);  // doc 5 REVISITED (a fresh doc-group, same docid)

  std::vector<TermPostings> terms = buf.finalize_sorted();
  ASSERT_EQ(terms.size(), 1u);
  const TermPostings& t = terms[0];
  // Exactly one entry per docid, strictly ascending.
  EXPECT_EQ(t.docids, (std::vector<uint32_t>{1u, 5u}));
  // doc 5 freq = 2 (first visit) + 1 (revisit) = 3; doc 1 freq = 1.
  EXPECT_EQ(t.freqs, (std::vector<uint32_t>{1u, 3u}));
  // Positions in document order: doc 1 {10}, then doc 5's two visits in arrival
  // order {50,51} then {52}.
  EXPECT_EQ(t.positions_flat, (std::vector<uint32_t>{10u, 50u, 51u, 52u}));
  // doc_positions slices stay consistent with the merged freqs.
  EXPECT_EQ(std::vector<uint32_t>(t.doc_positions(1).begin(), t.doc_positions(1).end()),
            (std::vector<uint32_t>{50u, 51u, 52u}));
  EXPECT_TRUE(buf.status().ok());
}

// Same revisit, positions disabled: freqs still sum and docids stay unique.
TEST(SpimiTermBuffer, RevisitedDocidCoalescesNoPositions) {
  SpimiTermBuffer buf(/*has_positions=*/false);
  buf.add_token("t", 5, 0);
  buf.add_token("t", 1, 0);
  buf.add_token("t", 5, 0);  // revisit doc 5

  std::vector<TermPostings> terms = buf.finalize_sorted();
  ASSERT_EQ(terms.size(), 1u);
  EXPECT_EQ(terms[0].docids, (std::vector<uint32_t>{1u, 5u}));
  EXPECT_EQ(terms[0].freqs, (std::vector<uint32_t>{1u, 2u}));
  EXPECT_TRUE(terms[0].positions_flat.empty());
}

// The coalesced out-of-order output satisfies the writer's strictly-ascending
// docid precondition: docids are unique AND strictly increasing (the exact check
// LogicalIndexWriter::validate_term enforces). This is the contract the fix
// restores -- previously a revisited docid produced a non-ascending list.
TEST(SpimiTermBuffer, RevisitedDocidProducesStrictlyAscending) {
  SpimiTermBuffer buf(/*has_positions=*/true);
  // A messy revisit pattern: 9,3,9,3,1.
  buf.add_token("w", 9, 0);
  buf.add_token("w", 3, 0);
  buf.add_token("w", 9, 1);
  buf.add_token("w", 3, 1);
  buf.add_token("w", 1, 0);

  std::vector<TermPostings> terms = buf.finalize_sorted();
  ASSERT_EQ(terms.size(), 1u);
  const TermPostings& t = terms[0];
  ASSERT_FALSE(t.docids.empty());
  for (size_t i = 1; i < t.docids.size(); ++i) {
    EXPECT_LT(t.docids[i - 1], t.docids[i]) << "docids must be strictly ascending";
  }
  EXPECT_EQ(t.docids, (std::vector<uint32_t>{1u, 3u, 9u}));
  EXPECT_EQ(t.freqs, (std::vector<uint32_t>{1u, 2u, 2u}));
  // Total positions equals total tokens (sum of freqs) -- nothing dropped.
  uint64_t total_freq = 0;
  for (uint32_t f : t.freqs) total_freq += f;
  EXPECT_EQ(t.positions_flat.size(), total_freq);
}

// Hardening: add_token(string_view) on a BORROWED-vocab buffer is rejected (it
// would otherwise grow the owned vocab out of step with the borrowed one and
// corrupt the build). The token is ignored and an error is latched.
TEST(SpimiTermBuffer, AddTokenStringViewRejectedInBorrowedMode) {
  const std::vector<std::string> vocab = {"a", "b"};
  SpimiTermBuffer buf(&vocab, /*has_positions=*/false);
  buf.add_token(0, 0, 0);                 // valid id-path token
  buf.add_token(std::string_view("a"), 1, 0);  // illegal on a borrowed-vocab buffer
  EXPECT_FALSE(buf.status().ok());
  // The string-view token was ignored: only the one id-path token counts.
  EXPECT_EQ(buf.total_tokens(), 1u);
  EXPECT_EQ(buf.unique_terms(), 1u);
}

// A spill's open() I/O failure surfaces as a LATCHED error in status() (the
// streaming for_each_term_sorted swallows the failure, so callers must check
// status()). We force open() to fail deterministically by EXHAUSTING every free
// file descriptor below the soft limit, so the spill's ::open returns EMFILE.
TEST(SpimiTermBuffer, SpillOpenIoFailureLatched) {
  // Tiny threshold so the very first token triggers a spill_to_run().
  SpimiTermBuffer buf(/*has_positions=*/false, /*spill_threshold_bytes=*/1);

  // Cap the soft limit low so we can exhaust the fd table cheaply, then hold it.
  struct rlimit saved {};
  ASSERT_EQ(getrlimit(RLIMIT_NOFILE, &saved), 0);
  struct rlimit tight = saved;
  tight.rlim_cur = 64;  // small, but >= the few gtest/std fds already open
  if (tight.rlim_cur > saved.rlim_max) tight.rlim_cur = saved.rlim_max;
  ASSERT_EQ(setrlimit(RLIMIT_NOFILE, &tight), 0);

  // Open /dev/null until the table is full: every free fd below the limit is now
  // taken, so the next ::open (the spill's) cannot get one -> EMFILE.
  std::vector<int> hogs;
  for (;;) {
    int fd = ::open("/dev/null", O_RDONLY);
    if (fd < 0) break;  // table exhausted
    hogs.push_back(fd);
  }
  ASSERT_FALSE(hogs.empty());

  buf.add_token("z", 0, 0);  // triggers a spill whose RunWriter::open must fail

  // Release the hog fds and restore the limit before asserting (so gtest I/O works).
  for (int fd : hogs) ::close(fd);
  ASSERT_EQ(setrlimit(RLIMIT_NOFILE, &saved), 0);

  EXPECT_FALSE(buf.status().ok()) << "spill open() failure must latch an error";
}

// Double-drain safety: a SECOND drain (finalize_sorted after for_each_term_sorted,
// or a second finalize_sorted) must NOT silently re-emit or emit a wrong stream;
// it returns empty / no callbacks AND latches an error.
TEST(SpimiTermBuffer, DoubleDrainIsRejected) {
  SpimiTermBuffer buf(/*has_positions=*/true);
  buf.add_token("a", 0, 0);
  buf.add_token("b", 0, 0);

  std::vector<TermPostings> first = buf.finalize_sorted();
  ASSERT_EQ(first.size(), 2u);
  EXPECT_TRUE(buf.status().ok());

  // Second finalize_sorted: empty result + latched error.
  std::vector<TermPostings> second = buf.finalize_sorted();
  EXPECT_TRUE(second.empty());
  EXPECT_FALSE(buf.status().ok());

  // for_each_term_sorted after a drain also emits nothing; now returns an error.
  size_t seen = 0;
  EXPECT_FALSE(buf.for_each_term_sorted([&](TermPostings&&) { ++seen; }).ok());
  EXPECT_EQ(seen, 0u);
}

// Double-drain via for_each_term_sorted first, then finalize_sorted: same guard.
TEST(SpimiTermBuffer, DoubleDrainStreamingThenMaterialized) {
  SpimiTermBuffer buf(/*has_positions=*/false);
  buf.add_token("a", 0, 0);

  size_t seen = 0;
  EXPECT_TRUE(buf.for_each_term_sorted([&](TermPostings&&) { ++seen; }).ok());
  EXPECT_EQ(seen, 1u);
  EXPECT_TRUE(buf.status().ok());

  std::vector<TermPostings> again = buf.finalize_sorted();
  EXPECT_TRUE(again.empty());
  EXPECT_FALSE(buf.status().ok());
}

// BYTE-IDENTICAL guard for normal ascending input: the streaming and materialized
// drains over an ASCENDING feed (the common, valid path -- NOT the out-of-order
// path the coalescing fix touches) must produce identical docids/freqs/positions.
// This asserts the fix did not perturb the normal path's produced postings.
TEST(SpimiTermBuffer, AscendingInputByteIdenticalAcrossDrains) {
  auto feed = [](SpimiTermBuffer& b) {
    for (uint32_t d = 0; d < 50; ++d) {
      b.add_token("apple", d, d * 2);
      b.add_token("apple", d, d * 2 + 1);  // freq 2 per doc
      if (d % 2 == 0) b.add_token("banana", d, d);
      if (d % 3 == 0) b.add_token("cherry", d, d + 100);
    }
  };
  SpimiTermBuffer mat(/*has_positions=*/true);
  SpimiTermBuffer strm(/*has_positions=*/true);
  feed(mat);
  feed(strm);

  std::vector<TermPostings> material = mat.finalize_sorted();
  std::vector<TermPostings> streamed;
  EXPECT_TRUE(strm.for_each_term_sorted([&](TermPostings&& tp) { streamed.push_back(std::move(tp)); }).ok());

  ASSERT_EQ(material.size(), streamed.size());
  for (size_t i = 0; i < material.size(); ++i) {
    EXPECT_EQ(material[i].term, streamed[i].term);
    EXPECT_EQ(material[i].docids, streamed[i].docids);
    EXPECT_EQ(material[i].freqs, streamed[i].freqs);
    EXPECT_EQ(material[i].positions_flat, streamed[i].positions_flat);
  }
  // Spot-check apple stayed exactly one entry per ascending docid (no coalescing
  // path was taken for this valid feed).
  ASSERT_EQ(material[0].term, "apple");
  EXPECT_EQ(material[0].docids.size(), 50u);
  for (uint32_t f : material[0].freqs) EXPECT_EQ(f, 2u);
  EXPECT_TRUE(mat.status().ok());
  EXPECT_TRUE(strm.status().ok());
}
