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
// A RUN is a self-describing file holding a lexicographically sorted sequence of
// terms, each followed by its postings, in this exact wire layout (all integers
// LEB128 varints, no fixed framing -- the file is produced and consumed by this
// module only, but decode still validates every length against the file size):
//
//   run := record*                       (terms strictly ascending within a run)
//   record :=
//     VInt term_len, term_bytes[term_len]
//     VInt n_docs
//     VInt docid_delta * n_docs          (first delta is docid-0, then gaps; all
//                                          docids ascending so deltas are >= 1
//                                          after the first, which may be 0)
//     VInt freq * n_docs                 (each >= 1)
//     VInt n_pos                         (== sum(freqs) when has_positions, else 0)
//     VInt position * n_pos              (document-order, partitioned by freqs)
//
// Decode is fully STREAMED: a RunReader reads a small fixed buffer at a time and
// materializes only the CURRENT term's postings, never the whole run. The k-way
// merge keeps one heap slot per run (each holding only its current term string +
// that term's postings), so peak memory is bounded by the widest single term
// summed across the runs that contain it -- not by total postings.

// Writes a sorted sequence of terms to one run file. Terms must be handed to
// write_term in strictly ascending order (the spill caller sorts before
// spilling). RAII: the file is flushed and closed on close(); the partial file
// is left for the owning SpimiTermBuffer to delete on its temp-path list.
class RunWriter {
 public:
  RunWriter() = default;
  ~RunWriter();

  RunWriter(const RunWriter&) = delete;
  RunWriter& operator=(const RunWriter&) = delete;

  // Opens `path` for writing (truncating). Returns IoError on failure.
  Status open(const std::string& path);

  // Appends one term's postings. `positions` must be empty iff !has_positions.
  // Caller guarantees ascending docids and parallel array lengths.
  Status write_term(const TermPostings& tp);

  // Flushes the buffer and closes the file. Safe to call once; idempotent.
  Status close();

 private:
  Status flush();

  int fd_ = -1;
  std::vector<uint8_t> buf_;  // staging buffer; flushed in fixed-size chunks
};

// Streamed reader over one run file. After open() the first term is loaded;
// current() exposes it; advance() loads the next (or marks exhausted). Only the
// current term's postings live in memory at a time.
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

  // Loads the next record into current(); sets exhausted() at end of file.
  Status advance();

 private:
  size_t available() const;              // buffered bytes from pos_ to window end
  Status fill();                         // tops up the decode window from disk
  Status ensure(size_t n);               // guarantees >= n buffered bytes (or eof)
  Status read_varint(uint64_t* v);       // bounds-checked streamed varint
  Status read_raw(size_t n, const uint8_t** p);  // contiguous n bytes in window

  int fd_ = -1;
  bool has_positions_ = false;
  bool exhausted_ = false;
  std::vector<uint8_t> window_;  // sliding decode window
  size_t pos_ = 0;               // consumed offset within window_
  bool eof_ = false;             // no more bytes on disk
  TermPostings current_;
};

// K-way merges the given run files into a single lexicographically sorted term
// stream, invoking `fn` once per distinct term with its postings concatenated
// across all runs that contain it (in run order -> docids stay ascending). Only
// one merged term is materialized at a time. Returns IoError/Corruption on bad
// run data. has_positions must match how the runs were written.
Status MergeRuns(const std::vector<std::string>& run_paths, bool has_positions,
                 const std::function<void(TermPostings&&)>& fn);

}  // namespace snii::writer
