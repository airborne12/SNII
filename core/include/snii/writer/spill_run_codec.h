#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "snii/common/status.h"
#include "snii/writer/spimi_term_buffer.h"

namespace snii::writer {

// On-disk SPIMI "run" codec for the spill / k-way-merge out-of-core build path.
//
// A RUN is a self-describing file holding a sequence of terms keyed by TERM-ID,
// each followed by its postings, in this exact wire layout. The file is produced
// and consumed by THIS module only (a private temp file -- the on-disk INDEX is
// unaffected), so the format is chosen for cheap I/O: docids, freqs and positions
// are ALL RAW fixed-width little-endian u32 BLOCKS (bulk memcpy on both ends,
// ~10x cheaper than per-value varint -- which cost ~1.5s of encode CPU over the
// 5M build's ~60M docids and compressed those streams poorly anyway). Decode
// still validates every length against the file size.
//
//   run := record*                       (term-ids ordered by vocab string,
//                                          strictly ascending within a run)
//   record :=
//     VInt term_id                       (index into the shared vocabulary; the
//                                          string is NOT stored -- smaller runs,
//                                          no per-record string IO)
//     VInt n_docs
//     u32  docid   * n_docs              (RAW LE block, memcpy; ABSOLUTE ascending
//                                          docids -- the merge concatenates across
//                                          runs and re-deltas at index encode time)
//     u32  freq    * n_docs              (RAW LE block, memcpy; each >= 1)
//     VInt n_pos                         (== sum(freqs) when has_positions, else 0)
//     u32  position * n_pos              (RAW LE block, document-order, partitioned
//                                          by freqs)
//
// Decode is fully STREAMED: a RunReader reads a small fixed buffer at a time and
// materializes only the CURRENT term's postings, never the whole run. The k-way
// merge keeps one heap slot per run (each holding only its current term-id +
// that term's postings), so peak memory is bounded by the widest single term
// summed across the runs that contain it -- not by total postings. The merge
// orders runs by the term-id's VOCAB STRING (resolved via the shared vocabulary)
// so the merged stream is lexicographic.

// Writes a sorted sequence of terms (by id) to one run file. Term-ids must be
// handed to write_term in vocab-string ascending order (the spill caller sorts
// before spilling). RAII: the file is flushed and closed on close(); the partial
// file is left for the owning SpimiTermBuffer to delete on its temp-path list.
class RunWriter {
 public:
  RunWriter() = default;
  ~RunWriter();

  RunWriter(const RunWriter&) = delete;
  RunWriter& operator=(const RunWriter&) = delete;

  // Opens `path` for writing (truncating). Returns IoError on failure.
  Status open(const std::string& path);

  // Appends one term's postings under `term_id`. `tp.positions_flat` must be empty
  // iff !has_positions (and otherwise hold sum(freqs) entries in doc order).
  // Caller guarantees ascending docids and parallel docids/freqs lengths.
  Status write_term(uint32_t term_id, const TermPostings& tp);

  // Flushes the buffer and closes the file. Safe to call once; idempotent.
  Status close();

 private:
  Status flush();

  int fd_ = -1;
  std::vector<uint8_t> buf_;  // staging buffer; flushed in fixed-size chunks
};

// Streamed reader over one run file. After open() the first term is loaded;
// current()/current_id() expose it; advance() loads the next (or marks
// exhausted). Only the current term's postings live in memory at a time. The
// current record's `term` string is left EMPTY -- runs store only the id; the
// owner resolves the string via the shared vocabulary.
class RunReader {
 public:
  RunReader() = default;
  ~RunReader();

  RunReader(const RunReader&) = delete;
  RunReader& operator=(const RunReader&) = delete;

  // Opens `path`, loading the first record (if any). has_positions must match
  // the writer's setting so n_pos is interpreted consistently.
  Status open(const std::string& path, bool has_positions);

  bool exhausted() const { return exhausted_; }
  const TermPostings& current() const { return current_; }
  uint32_t current_id() const { return current_id_; }

  // Loads the next record into current(); sets exhausted() at end of file.
  Status advance();

 private:
  size_t available() const;              // buffered bytes from pos_ to window end
  Status fill();                         // tops up the decode window from disk
  Status ensure(size_t n);               // guarantees >= n buffered bytes (or eof)
  Status read_varint(uint64_t* v);       // bounds-checked streamed varint
  // Bulk-reads `count` RAW little-endian u32s from the window into `out` (resized
  // to count). Bounds-checked against the run's true length (Corruption on EOF).
  Status read_raw_u32(size_t count, std::vector<uint32_t>* out);

  int fd_ = -1;
  bool has_positions_ = false;
  bool exhausted_ = false;
  std::vector<uint8_t> window_;  // sliding decode window
  size_t pos_ = 0;               // consumed offset within window_
  bool eof_ = false;             // no more bytes on disk
  uint32_t current_id_ = 0;      // current record's term-id
  TermPostings current_;
};

// K-way merges the given run files into a single term stream ordered by the
// term-id's VOCAB STRING (lexicographic), invoking `fn` once per distinct
// term-id with its postings concatenated across all runs that contain it (in
// run order -> docids stay ascending) and its `term` resolved from `vocab`.
// Only one merged term is materialized at a time. Returns IoError/Corruption on
// bad run data. has_positions must match how the runs were written. `vocab`
// maps term-id -> string and is borrowed.
Status MergeRuns(const std::vector<std::string>& run_paths,
                 const std::vector<std::string>& vocab, bool has_positions,
                 const std::function<void(TermPostings&&)>& fn);

}  // namespace snii::writer
