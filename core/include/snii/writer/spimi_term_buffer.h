#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "snii/common/status.h"

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
// with ascending-docid posting lists.
//
// TERM-ID ACCUMULATION (no per-token string work): tokens are accumulated by an
// INTEGER term-id, not by hashing/constructing a std::string per token. The
// caller supplies a VOCABULARY mapping term-id -> term string; the buffer keeps
// a DENSE std::vector<Term> indexed by term-id, so the hot add_token path is a
// vector index + a couple of pushes -- no hashing, no allocation per token. The
// vocabulary is resolved to strings only once per distinct term at finalize.
//
// Two construction modes:
//   * BORROWED vocab (the fast path): pass a non-null `vocab` that the caller
//     owns and keeps alive; add_token(term_id, ...) indexes straight into it.
//   * OWNED vocab (compatibility): pass a null `vocab`; the string-keyed
//     add_token(string_view, ...) interns each new term into an internal owned
//     vocabulary (assigning ids in first-seen order) and forwards to the id
//     path. Existing callers that feed strings keep working unchanged.
//
// SPILL / K-WAY MERGE (out-of-core, bounds input RAM): when a non-zero
// spill_threshold_bytes is set, the live in-memory size is tracked as tokens
// arrive; once it exceeds the threshold the buffer SORTS its current terms,
// writes a self-describing sorted RUN to a temp file, and CLEARS memory. Each
// run record is keyed by the TERM-ID (varint); the k-way merge orders runs by
// the id's VOCAB STRING so the merged stream stays lexicographic. Because
// tokens arrive in globally ascending docid order, a term that reappears in a
// later run only covers strictly-later docids, so concatenating its postings in
// run order during the final merge keeps docids ascending. for_each_term_sorted
// flushes the residual buffer as a final run, then k-way merges all runs
// materializing only ONE merged term at a time -> peak memory stays bounded by
// the threshold (plus the widest single term), NOT by total postings. With the
// default threshold 0 (unlimited) the path is exactly the in-memory behavior.
//
// Internal representation is FLAT per-term parallel arrays (docids/freqs and a
// single positions_flat vector), NOT a per-doc node-graph: this avoids the
// red-black-tree node overhead and the millions of tiny per-posting position
// vectors that dominated peak memory. Per-doc position counts are recoverable
// from freqs (positions are stored in document order).
//
// Duplicate vocab strings: the vocab is assumed to map each id to a DISTINCT
// string (a dense vocabulary). If two ids share a string they sort adjacently
// but are emitted as two separate terms; callers must not rely on coalescing.
class SpimiTermBuffer {
 public:
  // BORROWED-vocab constructor: `vocab` maps term-id -> term string and is
  // borrowed (NOT owned) -- the caller must keep it alive for the buffer's
  // lifetime. add_token(term_id, ...) accumulates by id with no string work.
  // spill_threshold_bytes == 0 means unlimited (pure in-memory, default); a
  // positive value caps the live buffer size, triggering a spill when crossed.
  explicit SpimiTermBuffer(const std::vector<std::string>* vocab, bool has_positions,
                           size_t spill_threshold_bytes = 0);

  // OWNED-vocab (compatibility) constructor: no external vocab. The string-keyed
  // add_token interns terms into an internal vocabulary on first occurrence.
  explicit SpimiTermBuffer(bool has_positions, size_t spill_threshold_bytes = 0);

  ~SpimiTermBuffer();

  SpimiTermBuffer(const SpimiTermBuffer&) = delete;
  SpimiTermBuffer& operator=(const SpimiTermBuffer&) = delete;

  // Records one token by TERM-ID: term `term_id` occurs in `docid` at `pos`.
  // `term_id` must be in [0, vocab_size). An out-of-range id latches an
  // InvalidArgument into status() and is ignored. For a given term, docids are
  // expected to arrive in non-decreasing order, and positions within a docid in
  // ascending order; out-of-order docids are tolerated and sorted at finalize.
  void add_token(uint32_t term_id, uint32_t docid, uint32_t pos);

  // Compatibility overload: records one token by TERM STRING. Only valid on an
  // OWNED-vocab buffer (constructed without an external vocab); interns `term`
  // into the internal vocabulary on first occurrence, then forwards by id.
  void add_token(std::string_view term, uint32_t docid, uint32_t pos);

  // Number of DISTINCT terms accumulated so far (touched ids still resident).
  size_t unique_terms() const;
  uint64_t total_tokens() const { return total_tokens_; }
  bool has_positions() const { return has_positions_; }

  // OK unless a spill / merge I/O or corruption error, or an out-of-range
  // term-id, occurred. The streaming for_each_term_sorted swallows such errors
  // (its callback signature has no return); callers MUST check this after
  // draining to detect a failed build.
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

  // The active vocabulary (term-id -> string): either the borrowed pointer or,
  // in owned mode, &owned_vocab_. Always non-null after construction.
  const std::vector<std::string>& vocab() const { return *vocab_; }

  // Accumulates one already-validated token into the per-id Term.
  void accumulate(uint32_t term_id, uint32_t docid, uint32_t pos);

  // Moves `t`'s flat arrays into a TermPostings (re-slicing positions_flat into
  // per-doc lists), sorting by docid first if `t.sorted` is false.
  TermPostings to_postings(std::string term, Term&& t) const;

  // Returns the touched term-ids sorted by their vocab string (lexicographic).
  std::vector<uint32_t> sorted_ids() const;
  // Streams the in-memory terms in sorted order, draining terms_ (the in-memory
  // single-pass path; used both by the no-spill case and to emit the final run).
  void drain_sorted(const std::function<void(TermPostings&&)>& fn);
  // Spills the current buffer to a fresh sorted run file and clears memory.
  Status spill_to_run();
  // Writes all current terms (sorted) to an already-open RunWriter, draining.
  Status drain_to_writer(class RunWriter* w);
  // Approximate live byte cost a token adds (per the threshold model).
  void account_token(uint32_t term_id, bool new_term, bool new_doc);
  // Final k-way merge over the spilled runs (+ the residual flushed as a run).
  void merge_runs(const std::function<void(TermPostings&&)>& fn);
  // Deletes every temp run file; called from the destructor (RAII cleanup).
  void cleanup_runs();
  // Frees a drained term's accumulator (id leaves the touched set).
  void release_term(uint32_t term_id);

  const std::vector<std::string>* vocab_;  // active vocab (borrowed or &owned_)
  std::vector<std::string> owned_vocab_;   // owned mode: interned term strings
  // Owned mode only: term string -> term-id, for interning on first occurrence.
  std::unordered_map<std::string, uint32_t> intern_;

  bool has_positions_;
  size_t spill_threshold_bytes_;  // 0 => unlimited (no spilling)
  size_t live_bytes_ = 0;         // tracked live cost of terms_ vs the threshold
  uint64_t total_tokens_ = 0;

  // Dense per-id accumulators, indexed by term-id (sized to vocab). present_[id]
  // is true while id has a live (non-drained) Term; touched_ids_ lists every id
  // currently present so finalize/spill iterate touched ids without scanning the
  // whole (possibly huge) vocabulary.
  std::vector<Term> terms_;
  std::vector<uint8_t> present_;
  std::vector<uint32_t> touched_ids_;
  size_t live_term_count_ = 0;  // present (non-drained) terms; == unique_terms()

  std::vector<std::string> run_paths_;  // spilled run temp files (deleted in dtor)
  Status spill_status_;                 // first spill / range error, at finalize
};

}  // namespace snii::writer
