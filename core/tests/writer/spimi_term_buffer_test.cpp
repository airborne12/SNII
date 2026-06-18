#include <gtest/gtest.h>

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
  ASSERT_EQ(apple.positions[0].size(), 2u);
  EXPECT_EQ(apple.positions[0][0], 1u);
  EXPECT_EQ(apple.positions[0][1], 2u);
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
  EXPECT_TRUE(terms[0].positions.empty());
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
  strm.for_each_term_sorted([&](TermPostings&& tp) { streamed.push_back(std::move(tp)); });

  ASSERT_EQ(material.size(), streamed.size());
  for (size_t i = 0; i < material.size(); ++i) {
    EXPECT_EQ(material[i].term, streamed[i].term);
    EXPECT_EQ(material[i].docids, streamed[i].docids);
    EXPECT_EQ(material[i].freqs, streamed[i].freqs);
    EXPECT_EQ(material[i].positions, streamed[i].positions);
  }
  // apple: docs {0(pos 1,2), 1(pos 0), 5(pos 3,7)} -> positions re-sliced by freq.
  ASSERT_EQ(streamed[0].term, "apple");
  ASSERT_EQ(streamed[0].docids.size(), 3u);
  EXPECT_EQ(streamed[0].freqs, (std::vector<uint32_t>{2u, 1u, 2u}));
  EXPECT_EQ(streamed[0].positions[2], (std::vector<uint32_t>{3u, 7u}));
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
  EXPECT_EQ(t.positions[0], (std::vector<uint32_t>{10u, 11u}));
  EXPECT_EQ(t.positions[1], (std::vector<uint32_t>{30u}));
  EXPECT_EQ(t.positions[2], (std::vector<uint32_t>{50u, 51u}));
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
  buf.for_each_term_sorted([&](TermPostings&& tp) {
    ++seen;
    EXPECT_EQ(tp.docids.size(), 100u);
    remaining_after_each.push_back(buf.unique_terms());
  });
  EXPECT_EQ(seen, 3u);
  // After consuming each of the 3 terms, the live count drops 2,1,0.
  EXPECT_EQ(remaining_after_each, (std::vector<size_t>{2u, 1u, 0u}));
  EXPECT_EQ(buf.unique_terms(), 0u);
}
