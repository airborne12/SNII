#pragma once

#include <cstdint>
#include <set>
#include <string>
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

}  // namespace bench
