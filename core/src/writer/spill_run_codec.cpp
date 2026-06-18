#include "snii/writer/spill_run_codec.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <queue>
#include <utility>

#include "snii/encoding/varint.h"

namespace snii::writer {

namespace {

// Flush staging once it grows past this. A LARGE write buffer (4 MiB) collapses
// the per-flush write() syscall count by ~64x: at 64 KiB the 5M build issued
// ~8800 write()s to ext4 (~9s of syscall overhead) for ~553 MiB of runs, versus
// a raw dd of the same bytes taking ~1.2s. Runs are PRIVATE temp files, so the
// on-disk index is unaffected; the only cost is a slightly larger transient
// RunWriter staging buffer (4 MiB, bounded, freed at close()).
constexpr size_t kWriteFlushBytes = 1u << 22;  // 4 MiB
// RunReader reads this much per disk fill; the window slides so a single record
// never needs the whole run in RAM (only the current term's encoded span). KEEP
// this small (64 KiB): a large read chunk x many open runs would inflate the
// merge-phase peak RSS at low spill thresholds (each reader holds a window).
constexpr size_t kReadChunkBytes = 1u << 16;  // 64 KiB

void AppendVarint(std::vector<uint8_t>* buf, uint64_t v) {
  uint8_t tmp[10];
  const size_t n = encode_varint64(v, tmp);
  buf->insert(buf->end(), tmp, tmp + n);
}

// Appends a block of `count` uint32 values as RAW little-endian fixed-width bytes
// (memcpy from contiguous source). Runs are private temp files; the on-disk index
// is unaffected. Raw blocks make encode/decode ~10x cheaper than per-value varint
// for the freqs/positions streams (which compress poorly as varints anyway), at
// the cost of a modestly larger temp run. Empty source is a no-op.
void AppendRawU32(std::vector<uint8_t>* buf, const uint32_t* src, size_t count) {
  if (count == 0) return;
  const auto* bytes = reinterpret_cast<const uint8_t*>(src);
  buf->insert(buf->end(), bytes, bytes + count * sizeof(uint32_t));
}

// Writes the full byte range [data, data+len) to fd, looping over short writes.
Status WriteAll(int fd, const uint8_t* data, size_t len) {
  size_t off = 0;
  while (off < len) {
    const ssize_t n = ::write(fd, data + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return Status::IoError(std::string("run write failed: ") + std::strerror(errno));
    }
    off += static_cast<size_t>(n);
  }
  return Status::OK();
}

}  // namespace

// ---------------------------------------------------------------------------
// RunWriter
// ---------------------------------------------------------------------------

RunWriter::~RunWriter() {
  if (fd_ >= 0) ::close(fd_);
}

Status RunWriter::open(const std::string& path) {
  fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd_ < 0) {
    return Status::IoError("run open(" + path + "): " + std::strerror(errno));
  }
  buf_.clear();
  return Status::OK();
}

Status RunWriter::flush() {
  if (buf_.empty()) return Status::OK();
  SNII_RETURN_IF_ERROR(WriteAll(fd_, buf_.data(), buf_.size()));
  buf_.clear();
  return Status::OK();
}

Status RunWriter::write_term(uint32_t term_id, const TermPostings& tp) {
  AppendVarint(&buf_, term_id);
  AppendVarint(&buf_, tp.docids.size());
  // Docids are a RAW fixed-width u32 block (bulk memcpy), NOT per-value VInt.
  // Per-value varint over ~60M docids cost ~1.5s of encode CPU on the spill feed
  // side; raw is a single memcpy and the decode side becomes a memcpy too. Runs
  // are PRIVATE temp files written then read back from page cache, so the modestly
  // larger run (no delta packing) costs ~0 extra real I/O. Absolute docids are
  // stored (the merge concatenates per-term across runs and re-deltas at encode).
  AppendRawU32(&buf_, tp.docids.data(), tp.docids.size());
  // Freqs + positions are RAW fixed-width u32 blocks (bulk memcpy). The decoder
  // reads them back the same way; n_pos == positions_flat.size() is recoverable
  // from sum(freqs), but is written explicitly so a reader can size the block.
  AppendRawU32(&buf_, tp.freqs.data(), tp.freqs.size());
  const uint64_t n_pos = tp.positions_flat.size();
  AppendVarint(&buf_, n_pos);
  AppendRawU32(&buf_, tp.positions_flat.data(), tp.positions_flat.size());
  if (buf_.size() >= kWriteFlushBytes) SNII_RETURN_IF_ERROR(flush());
  return Status::OK();
}

Status RunWriter::close() {
  if (fd_ < 0) return Status::OK();
  SNII_RETURN_IF_ERROR(flush());
  const int fd = fd_;
  fd_ = -1;
  if (::close(fd) != 0) {
    return Status::IoError(std::string("run close: ") + std::strerror(errno));
  }
  return Status::OK();
}

// ---------------------------------------------------------------------------
// RunReader
// ---------------------------------------------------------------------------

RunReader::~RunReader() {
  if (fd_ >= 0) ::close(fd_);
}

Status RunReader::open(const std::string& path, bool has_positions) {
  fd_ = ::open(path.c_str(), O_RDONLY);
  if (fd_ < 0) {
    return Status::IoError("run reopen(" + path + "): " + std::strerror(errno));
  }
  has_positions_ = has_positions;
  exhausted_ = false;
  eof_ = false;
  pos_ = 0;
  window_.clear();
  return advance();
}

// Slides consumed bytes out of the window, then appends one disk chunk.
Status RunReader::fill() {
  if (pos_ > 0) {
    window_.erase(window_.begin(), window_.begin() + pos_);
    pos_ = 0;
  }
  if (eof_) return Status::OK();
  const size_t base = window_.size();
  window_.resize(base + kReadChunkBytes);
  ssize_t n;
  do {
    n = ::read(fd_, window_.data() + base, kReadChunkBytes);
  } while (n < 0 && errno == EINTR);
  if (n < 0) return Status::IoError(std::string("run read: ") + std::strerror(errno));
  window_.resize(base + static_cast<size_t>(n));
  if (n == 0) eof_ = true;
  return Status::OK();
}

// Buffered bytes available to the decoder right now (from pos_ to window end).
// fill() may slide the window (erasing consumed bytes), so callers must compare
// THIS quantity -- not window_.size() -- to decide whether more data arrived.
size_t RunReader::available() const { return window_.size() - pos_; }

Status RunReader::ensure(size_t n) {
  while (available() < n) {
    const size_t had = available();
    SNII_RETURN_IF_ERROR(fill());
    if (available() == had && eof_) {
      return Status::Corruption("run truncated: needed more bytes than available");
    }
  }
  return Status::OK();
}

// Streamed varint: decode from the current window; if it straddles the buffered
// boundary, top up from disk and retry. A varint is at most 10 bytes, so this
// loops at most a couple of times. Bounds-safe: decode_varint64 never reads past
// `end`, and a partial varint at true eof is reported as corruption.
Status RunReader::read_varint(uint64_t* v) {
  while (true) {
    const uint8_t* p = window_.data() + pos_;
    const uint8_t* end = window_.data() + window_.size();
    const uint8_t* next = nullptr;
    Status s = decode_varint64(p, end, v, &next);
    if (s.ok()) {
      pos_ += static_cast<size_t>(next - p);
      return Status::OK();
    }
    if (eof_) return Status::Corruption("run truncated: incomplete varint");
    const size_t had = available();
    SNII_RETURN_IF_ERROR(fill());
    if (available() == had && eof_) {
      return Status::Corruption("run truncated: incomplete varint at eof");
    }
  }
}

// Bulk-decodes `count` raw little-endian u32s into `out`, topping up the window
// from disk as needed. Copies whatever is buffered each pass (the window may hold
// only part of a large block), so a high-df term's freqs/positions stream through
// in 64 KiB chunks without ever needing the whole block resident at once.
Status RunReader::read_raw_u32(size_t count, std::vector<uint32_t>* out) {
  out->resize(count);
  if (count == 0) return Status::OK();
  auto* dst = reinterpret_cast<uint8_t*>(out->data());
  size_t need = count * sizeof(uint32_t);
  size_t written = 0;
  while (need > 0) {
    if (available() == 0) {
      const size_t had = available();
      SNII_RETURN_IF_ERROR(fill());
      if (available() == had && eof_) {
        return Status::Corruption("run truncated: needed more raw bytes than available");
      }
    }
    const size_t take = std::min(need, available());
    std::memcpy(dst + written, window_.data() + pos_, take);
    pos_ += take;
    written += take;
    need -= take;
  }
  return Status::OK();
}

Status RunReader::advance() {
  // End-of-run detection: at a record boundary, if no bytes remain we are done.
  if (available() == 0) {
    SNII_RETURN_IF_ERROR(fill());
    if (available() == 0 && eof_) {
      exhausted_ = true;
      return Status::OK();
    }
  }
  uint64_t term_id = 0;
  SNII_RETURN_IF_ERROR(read_varint(&term_id));
  if (term_id > UINT32_MAX) return Status::Corruption("run term_id exceeds uint32");
  current_id_ = static_cast<uint32_t>(term_id);
  current_.term.clear();  // runs store only the id; owner resolves the string

  uint64_t n_docs = 0;
  SNII_RETURN_IF_ERROR(read_varint(&n_docs));
  // Docids: RAW absolute u32 block (bulk read), matching the writer's AppendRawU32.
  SNII_RETURN_IF_ERROR(read_raw_u32(static_cast<size_t>(n_docs), &current_.docids));
  // Freqs: RAW u32 block (bulk read), matching the writer's AppendRawU32.
  SNII_RETURN_IF_ERROR(read_raw_u32(static_cast<size_t>(n_docs), &current_.freqs));
  uint64_t n_pos = 0;
  SNII_RETURN_IF_ERROR(read_varint(&n_pos));
  current_.positions_flat.clear();
  if (has_positions_) {
    // Positions: RAW u32 block, flat document-order (partitioned by freqs). No
    // per-doc vector-of-vectors is built -- the widest term's positions land in a
    // single contiguous buffer.
    SNII_RETURN_IF_ERROR(read_raw_u32(static_cast<size_t>(n_pos), &current_.positions_flat));
  } else if (n_pos != 0) {
    // No-positions runs should carry n_pos == 0; tolerate (skip) a stray block.
    std::vector<uint32_t> skip;
    SNII_RETURN_IF_ERROR(read_raw_u32(static_cast<size_t>(n_pos), &skip));
  }
  return Status::OK();
}

// ---------------------------------------------------------------------------
// K-way merge
// ---------------------------------------------------------------------------

namespace {

// Min-heap entry: orders by the run's current term-id's VOCAB STRING, tie-broken
// by run index so equal terms are gathered run-order (keeping concatenated
// docids ascending). The comparator resolves id -> string via the shared vocab,
// so the merged stream is lexicographic (the dictionary order the writer needs).
struct HeapItem {
  uint32_t term_id;
  size_t run;
};
struct HeapGreater {
  const std::vector<std::string>* vocab;
  bool operator()(const HeapItem& a, const HeapItem& b) const {
    const std::string& sa = (*vocab)[a.term_id];
    const std::string& sb = (*vocab)[b.term_id];
    if (sa != sb) return sa > sb;
    return a.run > b.run;
  }
};

// Appends src's postings onto dst (run order). Later runs only cover docids
// >= dst's last, so docids stay ascending. COALESCE the boundary doc: if a spill
// fell BETWEEN two tokens of the same doc, that doc ends one run and begins the
// next with the SAME docid -- merge them (sum freqs, splice positions) so the
// merged term has exactly one entry per docid (matching the in-memory build).
//
// Positions are FLAT: doc order, partitioned by freqs. Because both dst and src
// already store doc-ordered flat positions, the common (no-boundary-overlap) case
// is a single bulk append. The boundary-overlap case must INSERT src's first
// doc's positions right after dst's last doc's positions so flat order stays
// consistent with the merged (coalesced) freqs.
void Concat(TermPostings* dst, const TermPostings& src, bool has_positions) {
  if (src.docids.empty()) return;
  size_t start = 0;
  size_t src_pos_start = 0;  // flat offset of src positions to append after splice
  if (!dst->docids.empty() && dst->docids.back() == src.docids.front()) {
    const uint32_t head_fc = src.freqs.front();
    if (has_positions && head_fc != 0) {
      // Splice src's first-doc positions in right after dst's last-doc positions.
      // dst's last doc owns dst->freqs.back() entries at the tail of positions_flat
      // BEFORE we bump that freq, so insert at end() (last doc is the tail run).
      auto& flat = dst->positions_flat;
      flat.insert(flat.end(), src.positions_flat.begin(),
                  src.positions_flat.begin() + head_fc);
    }
    dst->freqs.back() += head_fc;
    src_pos_start = head_fc;
    start = 1;  // boundary doc folded in; append the rest
  }
  dst->docids.insert(dst->docids.end(), src.docids.begin() + start, src.docids.end());
  dst->freqs.insert(dst->freqs.end(), src.freqs.begin() + start, src.freqs.end());
  if (has_positions) {
    dst->positions_flat.insert(dst->positions_flat.end(),
                               src.positions_flat.begin() + src_pos_start,
                               src.positions_flat.end());
  }
}

}  // namespace

Status MergeRuns(const std::vector<std::string>& run_paths,
                 const std::vector<std::string>& vocab, bool has_positions,
                 const std::function<void(TermPostings&&)>& fn) {
  std::vector<std::unique_ptr<RunReader>> readers;
  readers.reserve(run_paths.size());
  std::priority_queue<HeapItem, std::vector<HeapItem>, HeapGreater> heap(
      HeapGreater{&vocab});
  for (size_t i = 0; i < run_paths.size(); ++i) {
    auto r = std::make_unique<RunReader>();
    SNII_RETURN_IF_ERROR(r->open(run_paths[i], has_positions));
    if (!r->exhausted()) {
      if (r->current_id() >= vocab.size()) {
        return Status::Corruption("run term_id out of vocab range");
      }
      heap.push({r->current_id(), i});
    }
    readers.push_back(std::move(r));
  }

  std::vector<size_t> matching;  // run indices contributing the current term
  while (!heap.empty()) {
    const uint32_t id = heap.top().term_id;
    TermPostings merged;
    merged.term = vocab[id];  // resolve the id -> dictionary string once
    // Gather every run whose head id maps to the same string (the heap's run
    // tie-break keeps them in run order, so concatenated docids stay ascending).
    // Equal strings imply equal ids for a dense vocab; compare by string so a
    // duplicate string still groups correctly. The matching runs' current slices
    // are already loaded in their readers (they were read to seed the heap), so
    // summing their sizes here costs nothing extra in RAM.
    matching.clear();
    uint64_t total_docs = 0, total_pos = 0;
    while (!heap.empty() && vocab[heap.top().term_id] == merged.term) {
      const size_t ri = heap.top().run;
      heap.pop();
      const TermPostings& cur = readers[ri]->current();
      total_docs += cur.docids.size();
      total_pos += cur.positions_flat.size();
      matching.push_back(ri);
    }
    // Reserve EXACTLY the summed sizes (an upper bound -- boundary-doc coalescing
    // only shrinks the final size). This eliminates std::vector's geometric
    // over-allocation, which left ~32 MiB of dead capacity on the widest term (df
    // in the millions split across spills) -- a dominant merge-phase peak-RSS
    // overhang at 5M. The reserved-but-unwritten pages are not faulted in, so the
    // empty reservation itself does not raise RSS; only the actual data does.
    merged.docids.reserve(static_cast<size_t>(total_docs));
    merged.freqs.reserve(static_cast<size_t>(total_docs));
    if (has_positions) merged.positions_flat.reserve(static_cast<size_t>(total_pos));
    for (size_t ri : matching) {
      RunReader* r = readers[ri].get();
      Concat(&merged, r->current(), has_positions);
      SNII_RETURN_IF_ERROR(r->advance());  // frees this run's slice, loads next term
      if (!r->exhausted()) {
        if (r->current_id() >= vocab.size()) {
          return Status::Corruption("run term_id out of vocab range");
        }
        heap.push({r->current_id(), ri});
      }
    }
    fn(std::move(merged));
  }
  return Status::OK();
}

}  // namespace snii::writer
