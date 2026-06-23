#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Deterministic synthetic corpus generator for the SNII-vs-CLucene benchmark.
//
// The corpus is reproducible from a seed (splitmix64 PRNG, never random_device)
// so both index builders and the in-memory oracle see exactly the same tokens.
// Term frequencies follow a Zipfian distribution over a fixed vocabulary, so a
// few terms have very high document frequency (which exercises SNII's windowed
// posting path) while most terms are low-df (inline postings).
namespace bench {

// Per-doc token sequence plus the vocabulary it was drawn from. Token strings
// are plain lowercase ASCII so they survive a WhitespaceAnalyzer unchanged and
// can be space-joined into a single CLucene field value.
struct Corpus {
  uint32_t doc_count = 0;
  std::vector<std::string> vocab;             // vocab[id] = term string
  std::vector<std::vector<uint32_t>> docs;    // docs[d] = ordered term-id list

  // Number of distinct documents that contain term-id `id`.
  uint32_t document_frequency(uint32_t id) const;

  // Returns the term string for a doc's k-th token.
  const std::string& token_text(uint32_t doc, uint32_t k) const {
    return vocab[docs[doc][k]];
  }
};

// Generates a deterministic corpus. `zipf_s` is the Zipf exponent (larger => more
// skew). `avg_doc_len` is the mean token count per document (each doc length is
// drawn around this mean). Token ids are sampled from a Zipfian distribution.
Corpus generate(uint32_t doc_count, uint32_t vocab_size, double zipf_s,
                uint32_t avg_doc_len, uint64_t seed);

// Picks a term-id whose document frequency is the highest in the corpus.
uint32_t highest_df_term(const Corpus& c);

// Picks a term-id whose document frequency is closest to the median of the
// non-zero-df terms (a "mid-df" term).
uint32_t mid_df_term(const Corpus& c);

// Picks a term-id with a low but non-zero document frequency (so the SNII inline
// posting path is exercised and the query still returns a non-empty set).
uint32_t low_df_term(const Corpus& c);

// Extracts a real `length`-token phrase that actually occurs in at least one
// document. The returned vector holds the term strings. If no such phrase exists
// (e.g. all docs are too short), returns an empty vector.
std::vector<std::string> extract_phrase(const Corpus& c, uint32_t length);

// --- df-bucket term selection (enriched scenario suite) ---
// All exclude the lexicographic-max (dictionary tail) term for safe reachability,
// consistent with highest/mid/low_df_term.

// Document frequency of every vocab term, in one pass.
std::vector<uint32_t> all_dfs(const Corpus& c);

// Lexicographically-smallest non-tail term whose df is in [lo_frac*N, hi_frac*N]
// (inclusive, N = doc_count). Returns 0 if the band is empty.
uint32_t term_in_df_bucket(const Corpus& c, double lo_frac, double hi_frac);

// Non-tail term whose df is closest to target_df (ties -> lex-smallest).
uint32_t term_at_df(const Corpus& c, uint32_t target_df);

// A non-tail term with df==1 (lex-smallest); falls back to low_df_term if none.
uint32_t df1_term(const Corpus& c);

// A vocab-shaped ([a-z0-9]) token guaranteed absent from the vocabulary (df=0).
std::string absent_token(const Corpus& c);

// A term pair (A in band [a_lo,a_hi], B in band [b_lo,b_hi]) that co-occurs in at
// least one document. first==second if no co-occurring B was found (caller checks).
std::pair<uint32_t, uint32_t> cooccurring_pair(const Corpus& c, double a_lo,
                                               double a_hi, double b_lo,
                                               double b_hi);

}  // namespace bench
