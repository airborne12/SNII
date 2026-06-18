#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace snii::writer {

// One term's posting list: docids ascending, with parallel freqs and (when
// positions are enabled) per-doc position lists.
struct TermPostings {
  std::string term;
  std::vector<uint32_t> docids;
  std::vector<uint32_t> freqs;
  std::vector<std::vector<uint32_t>> positions;  // empty when positions disabled
};

// In-memory SPIMI (Single-Pass In-Memory Indexing) accumulator for one logical
// index. Records term occurrences and produces lexicographically sorted terms
// with ascending-docid posting lists. (Spill / k-way merge for out-of-core
// builds can wrap this later; the on-disk run format is identical.)
//
// Internal representation is FLAT per-term parallel arrays (docids/freqs and a
// single positions_flat vector), NOT a per-doc node-graph: this avoids the
// red-black-tree node overhead and the millions of tiny per-posting position
// vectors that dominated peak memory. Per-doc position counts are recoverable
// from freqs (positions are stored in document order).
class SpimiTermBuffer {
 public:
  explicit SpimiTermBuffer(bool has_positions);

  // Records one token: `term` occurs in `docid` at `pos`. For a given term,
  // docids are expected to arrive in non-decreasing order, and positions within
  // a docid in ascending order (caller's tokenizer order). Out-of-order docids
  // are tolerated and sorted once at finalize time.
  void add_token(std::string_view term, uint32_t docid, uint32_t pos);

  size_t unique_terms() const;
  uint64_t total_tokens() const { return total_tokens_; }
  bool has_positions() const { return has_positions_; }

  // Materializes all terms sorted lexicographically; each term's docids are
  // ascending. Convenience wrapper around for_each_term_sorted that keeps the
  // whole result alive at once. Prefer for_each_term_sorted for low peak memory.
  // May be called once (it drains internal state).
  std::vector<TermPostings> finalize_sorted();

  // Streams terms to `fn` in lexicographic order, building ONE transient
  // TermPostings at a time and freeing that term's accumulated arrays before
  // moving to the next. This keeps at most a single term's postings duplicated,
  // avoiding the input+output coexistence peak. May be called once (it drains
  // internal state).
  void for_each_term_sorted(const std::function<void(TermPostings&&)>& fn);

 private:
  // Flat per-term accumulator. docids[i]/freqs[i] are parallel; positions_flat
  // holds all positions for this term in document order, partitioned by the
  // per-doc freqs (doc i owns the next freqs[i] entries). No per-posting heap
  // vectors, no ordering tree.
  struct Term {
    std::vector<uint32_t> docids;
    std::vector<uint32_t> freqs;
    std::vector<uint32_t> positions_flat;  // empty when positions disabled
    bool sorted = true;  // false if a docid ever arrived out of ascending order
  };

  // Moves `t`'s flat arrays into a TermPostings (re-slicing positions_flat into
  // per-doc lists), sorting by docid first if `t.sorted` is false.
  TermPostings to_postings(std::string term, Term&& t) const;

  bool has_positions_;
  uint64_t total_tokens_ = 0;
  std::unordered_map<std::string, Term> data_;
};

}  // namespace snii::writer
