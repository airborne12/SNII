#include "corpus_gen.h"

#include <algorithm>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace {

// Builds a Corpus with controlled document frequencies. vocab[i] gets df[i] by
// placing term i in the first df[i] documents. doc_count = max(df).
bench::Corpus make_corpus(const std::vector<std::string>& vocab,
                          const std::vector<uint32_t>& df) {
  bench::Corpus c;
  c.vocab = vocab;
  uint32_t n = 0;
  for (uint32_t v : df) n = std::max(n, v);
  c.doc_count = n;
  c.docs.assign(n, {});
  for (uint32_t i = 0; i < vocab.size(); ++i) {
    for (uint32_t d = 0; d < df[i]; ++d) c.docs[d].push_back(i);
  }
  return c;
}

uint32_t df_of(const bench::Corpus& c, uint32_t id) {
  return c.document_frequency(id);
}

TEST(CorpusDfBuckets, AllDfsMatchesConstruction) {
  bench::Corpus c = make_corpus({"a", "b", "c", "d"}, {10, 6, 3, 1});
  std::vector<uint32_t> df = bench::all_dfs(c);
  ASSERT_EQ(df.size(), 4u);
  EXPECT_EQ(df[0], 10u);
  EXPECT_EQ(df[1], 6u);
  EXPECT_EQ(df[2], 3u);
  EXPECT_EQ(df[3], 1u);
}

TEST(CorpusDfBuckets, TermInDfBucketReturnsTermInBand) {
  // N=100; bands: very-high [0.5,1.0], mid [0.1,0.5), low (0,0.1).
  bench::Corpus c = make_corpus({"hi", "mid", "lo", "rare"}, {90, 30, 5, 1});
  const uint32_t vh = bench::term_in_df_bucket(c, 0.5, 1.0);
  EXPECT_GE(df_of(c, vh), 50u);
  const uint32_t md = bench::term_in_df_bucket(c, 0.1, 0.5);
  EXPECT_GE(df_of(c, md), 10u);
  EXPECT_LT(df_of(c, md), 50u);
}

TEST(CorpusDfBuckets, TermAtDfPicksNearest) {
  bench::Corpus c = make_corpus({"a", "b", "c", "d"}, {1000, 520, 480, 50});
  // Nearest to 512 is 520 (id 1) over 480 (id 2): |520-512|=8 < |480-512|=32.
  EXPECT_EQ(bench::term_at_df(c, 512), 1u);
  // Nearest to 490 is 480 (id 2).
  EXPECT_EQ(bench::term_at_df(c, 490), 2u);
}

TEST(CorpusDfBuckets, Df1TermHasDfOne) {
  // df==1 term ("rare1") is NOT the lexicographic-max ("zcommon"), so the
  // tail-exclusion (shared with highest/mid/low_df_term) does not hide it.
  bench::Corpus c = make_corpus({"a", "b", "rare1", "zcommon"}, {10, 6, 1, 8});
  const uint32_t id = bench::df1_term(c);
  EXPECT_EQ(df_of(c, id), 1u);
}

TEST(CorpusDfBuckets, AbsentTokenNotInVocab) {
  bench::Corpus c = make_corpus({"alpha", "beta", "gamma"}, {5, 3, 1});
  const std::string tok = bench::absent_token(c);
  EXPECT_FALSE(tok.empty());
  EXPECT_EQ(std::find(c.vocab.begin(), c.vocab.end(), tok), c.vocab.end());
}

TEST(CorpusDfBuckets, CooccurringPairSharesADoc) {
  // N=10. hi (df10) in band A [0.5,1.0]; rare (df1) in band B [0,0.2]. "rare"
  // is placed only in doc 3 -> co-occurs with hi there. "zzcommon" is the
  // lexicographic tail (excluded), so it is not chosen as either endpoint.
  bench::Corpus c = make_corpus({"hi", "mid", "rare", "zzcommon"}, {10, 4, 0, 8});
  c.docs[3].push_back(2);  // "rare" (id 2) into doc 3
  const auto pr =
      bench::cooccurring_pair(c, /*a_lo=*/0.5, 1.0, /*b_lo=*/0.0, 0.2);
  ASSERT_NE(pr.first, pr.second);
  bool together = false;
  for (const auto& doc : c.docs) {
    const bool a = std::find(doc.begin(), doc.end(), pr.first) != doc.end();
    const bool b = std::find(doc.begin(), doc.end(), pr.second) != doc.end();
    if (a && b) { together = true; break; }
  }
  EXPECT_TRUE(together);
}

}  // namespace
