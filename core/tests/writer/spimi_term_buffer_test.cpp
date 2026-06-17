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
