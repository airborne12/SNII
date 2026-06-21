#include "parallel_tokenizer.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "corpus_gen.h"
#include "doris_english_analyzer.h"
#include "gtest/gtest.h"

namespace {

// Single-threaded reference: first-occurrence interning over documents in order,
// the exact semantics tokenize_corpus must reproduce regardless of thread count.
bench::Corpus serial_oracle(const std::vector<std::string>& bodies) {
  bench::Corpus c;
  std::vector<std::string> toks;
  std::unordered_map<std::string, uint32_t> vocab_index;
  for (const auto& body : bodies) {
    bench::doris_english_tokenize(body, &toks);
    std::vector<uint32_t> doc;
    doc.reserve(toks.size());
    for (const auto& t : toks) {
      auto it = vocab_index.find(t);
      uint32_t id;
      if (it == vocab_index.end()) {
        id = static_cast<uint32_t>(c.vocab.size());
        c.vocab.push_back(t);
        vocab_index.emplace(t, id);
      } else {
        id = it->second;
      }
      doc.push_back(id);
    }
    c.docs.push_back(std::move(doc));
  }
  c.doc_count = static_cast<uint32_t>(c.docs.size());
  return c;
}

void expect_corpus_eq(const bench::Corpus& a, const bench::Corpus& b) {
  EXPECT_EQ(a.doc_count, b.doc_count);
  EXPECT_EQ(a.vocab, b.vocab);
  ASSERT_EQ(a.docs.size(), b.docs.size());
  for (size_t d = 0; d < a.docs.size(); ++d) {
    EXPECT_EQ(a.docs[d], b.docs[d]) << "doc " << d;
  }
}

const std::vector<std::string>& sample_bodies() {
  static const std::vector<std::string> bodies = {
      "Failed to place order",
      R"({"code":13,"details":"failed to charge card: Visa cache full"})",
      "Checkout",
      "",  // empty document is kept so docids stay aligned
      "failed to convert shipping cost to currency",
      "PLACE order PLACE order place",  // repeats map to one id, kept positionally
      "::::",                            // separators only -> zero tokens
      "trace info warn error fatal trace",
  };
  return bodies;
}

TEST(ParallelTokenizer, SingleThreadMatchesSerialOracle) {
  expect_corpus_eq(bench::tokenize_corpus(sample_bodies(), 1),
                   serial_oracle(sample_bodies()));
}

TEST(ParallelTokenizer, FourThreadsMatchSerialOracle) {
  expect_corpus_eq(bench::tokenize_corpus(sample_bodies(), 4),
                   serial_oracle(sample_bodies()));
}

TEST(ParallelTokenizer, SixteenThreadsMatchSerialOracle) {
  expect_corpus_eq(bench::tokenize_corpus(sample_bodies(), 16),
                   serial_oracle(sample_bodies()));
}

TEST(ParallelTokenizer, ThreadsExceedingDocCountIsSafe) {
  // More threads than documents must not crash or change the result.
  std::vector<std::string> bodies = {"alpha beta", "beta gamma"};
  expect_corpus_eq(bench::tokenize_corpus(bodies, 64), serial_oracle(bodies));
}

TEST(ParallelTokenizer, EmptyInputYieldsEmptyCorpus) {
  bench::Corpus c = bench::tokenize_corpus({}, 8);
  EXPECT_EQ(c.doc_count, 0u);
  EXPECT_TRUE(c.vocab.empty());
  EXPECT_TRUE(c.docs.empty());
}

TEST(ParallelTokenizer, ZeroThreadsTreatedAsOne) {
  expect_corpus_eq(bench::tokenize_corpus(sample_bodies(), 0),
                   serial_oracle(sample_bodies()));
}

TEST(ParallelTokenizer, VocabIdOrderIsFirstOccurrenceAcrossShards) {
  // With a doc boundary that lands inside a multi-thread split, a term first
  // seen in an earlier document must keep the lower global id even if a later
  // thread also interns it locally first.
  std::vector<std::string> bodies;
  for (int i = 0; i < 1000; ++i) {
    bodies.push_back(i == 0 ? "zzz first marker" : "common word common");
  }
  bench::Corpus par = bench::tokenize_corpus(bodies, 8);
  bench::Corpus ser = serial_oracle(bodies);
  expect_corpus_eq(par, ser);
  // "zzz" appears only in doc 0 -> must be a valid low id; "common" is shared.
  EXPECT_EQ(par.vocab[0], "zzz");
}

}  // namespace
