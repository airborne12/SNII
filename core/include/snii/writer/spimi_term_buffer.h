#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "snii/common/status.h"

namespace snii::writer {

// Heterogeneous (transparent) hash for a std::string-keyed map: lets add_token()
// look up by std::string_view without materializing a std::string. The two
// overloads MUST hash identically so a string and its view collide into the same
// bucket; std::hash<std::string_view> over the same bytes guarantees that.
struct TransparentStrHash {
  using is_transparent = void;
  size_t operator()(std::string_view sv) const noexcept {
    return std::hash<std::string_view>{}(sv);
  }
  size_t operator()(const std::string& s) const noexcept {
    return std::hash<std::string_view>{}(std::string_view(s));
  }
};

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
// with ascending-docid posting lists.
//
// SPILL / K-WAY MERGE (out-of-core, bounds input RAM): when a non-zero
// spill_threshold_bytes is set, the live in-memory size is tracked as tokens
// arrive; once it exceeds the threshold the buffer SORTS its current terms,
// writes a self-describing sorted RUN to a temp file, and CLEARS memory. Because
// tokens arrive in globally ascending docid order, a term that reappears in a
// later run only covers strictly-later docids, so concatenating its postings in
// run order during the final k-way merge keeps docids ascending. for_each_term_
// sorted flushes the residual buffer as a final run, then k-way merges all runs
// materializing only ONE merged term at a time -> peak memory stays bounded by
// the threshold (plus the widest single term), NOT by total postings. With the
// default threshold 0 (unlimited) the path is exactly the in-memory behavior.
//
// Internal representation is FLAT per-term parallel arrays (docids/freqs and a
// single positions_flat vector), NOT a per-doc node-graph: this avoids the
// red-black-tree node overhead and the millions of tiny per-posting position
// vectors that dominated peak memory. Per-doc position counts are recoverable
// from freqs (positions are stored in document order).
class SpimiTermBuffer {
 public:
  // spill_threshold_bytes == 0 means unlimited (pure in-memory, default). A
  // positive value caps the live buffer size; crossing it triggers a spill.
  explicit SpimiTermBuffer(bool has_positions, size_t spill_threshold_bytes = 0);

  ~SpimiTermBuffer();

  SpimiTermBuffer(const SpimiTermBuffer&) = delete;
  SpimiTermBuffer& operator=(const SpimiTermBuffer&) = delete;

  // Records one token: `term` occurs in `docid` at `pos`. For a given term,
  // docids are expected to arrive in non-decreasing order, and positions within
  // a docid in ascending order (caller's tokenizer order). Out-of-order docids
  // are tolerated and sorted once at finalize time.
  void add_token(std::string_view term, uint32_t docid, uint32_t pos);

  size_t unique_terms() const;
  uint64_t total_tokens() const { return total_tokens_; }
  bool has_positions() const { return has_positions_; }

  // OK unless a spill / merge I/O or corruption error occurred. The streaming
  // for_each_term_sorted swallows such errors (its callback signature has no
  // return); callers MUST check this after draining to detect a failed build.
  Status status() const { return spill_status_; }

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

  // Returns the sorted keys of the current in-memory data_ (lexicographic).
  std::vector<const std::string*> sorted_keys() const;
  // Streams the in-memory terms in sorted order, draining data_ (the legacy
  // single-pass path; used both by the no-spill case and to emit the final run).
  void drain_sorted(const std::function<void(TermPostings&&)>& fn);
  // Spills the current buffer to a fresh sorted run file and clears memory.
  Status spill_to_run();
  // Writes all current terms (sorted) to an already-open RunWriter, draining.
  Status drain_to_writer(class RunWriter* w);
  // Approximate live byte cost a token adds for `term` (per the threshold model).
  void account_token(const std::string& term, bool new_term, bool new_doc);
  // Final k-way merge over the spilled runs (+ the residual flushed as a run).
  void merge_runs(const std::function<void(TermPostings&&)>& fn);
  // Deletes every temp run file; called from the destructor (RAII cleanup).
  void cleanup_runs();

  bool has_positions_;
  size_t spill_threshold_bytes_;  // 0 => unlimited (no spilling)
  size_t live_bytes_ = 0;         // tracked live cost of data_ vs the threshold
  uint64_t total_tokens_ = 0;
  // Transparent map: keyed by std::string but probeable by std::string_view, so
  // the per-token hot path constructs a std::string only on first occurrence of
  // a term (the insert branch), not on every (hit) token.
  std::unordered_map<std::string, Term, TransparentStrHash, std::equal_to<>> data_;
  std::vector<std::string> run_paths_;  // spilled run temp files (deleted in dtor)
  Status spill_status_;                 // first spill error, surfaced at finalize
};

}  // namespace snii::writer
