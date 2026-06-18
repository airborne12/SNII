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

// Flush staging once it grows past this; keeps RunWriter RSS tiny and bounded.
constexpr size_t kWriteFlushBytes = 1u << 16;  // 64 KiB
// RunReader reads this much per disk fill; the window slides so a single record
// never needs the whole run in RAM (only the current term's encoded span).
constexpr size_t kReadChunkBytes = 1u << 16;  // 64 KiB

void AppendVarint(std::vector<uint8_t>* buf, uint64_t v) {
  uint8_t tmp[10];
  const size_t n = encode_varint64(v, tmp);
  buf->insert(buf->end(), tmp, tmp + n);
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

Status RunWriter::write_term(const TermPostings& tp) {
  AppendVarint(&buf_, tp.term.size());
  buf_.insert(buf_.end(), tp.term.begin(), tp.term.end());
  AppendVarint(&buf_, tp.docids.size());
  uint32_t prev = 0;
  for (size_t i = 0; i < tp.docids.size(); ++i) {
    AppendVarint(&buf_, static_cast<uint64_t>(tp.docids[i]) - prev);
    prev = tp.docids[i];
  }
  for (uint32_t f : tp.freqs) AppendVarint(&buf_, f);
  uint64_t n_pos = 0;
  for (const auto& pl : tp.positions) n_pos += pl.size();
  AppendVarint(&buf_, n_pos);
  for (const auto& pl : tp.positions) {
    for (uint32_t p : pl) AppendVarint(&buf_, p);
  }
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

Status RunReader::read_raw(size_t n, const uint8_t** p) {
  SNII_RETURN_IF_ERROR(ensure(n));
  *p = window_.data() + pos_;
  pos_ += n;
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

Status RunReader::advance() {
  // End-of-run detection: at a record boundary, if no bytes remain we are done.
  if (available() == 0) {
    SNII_RETURN_IF_ERROR(fill());
    if (available() == 0 && eof_) {
      exhausted_ = true;
      return Status::OK();
    }
  }
  uint64_t term_len = 0;
  SNII_RETURN_IF_ERROR(read_varint(&term_len));
  const uint8_t* tp = nullptr;
  SNII_RETURN_IF_ERROR(read_raw(static_cast<size_t>(term_len), &tp));
  current_.term.assign(reinterpret_cast<const char*>(tp), static_cast<size_t>(term_len));

  uint64_t n_docs = 0;
  SNII_RETURN_IF_ERROR(read_varint(&n_docs));
  current_.docids.resize(static_cast<size_t>(n_docs));
  current_.freqs.resize(static_cast<size_t>(n_docs));
  uint32_t acc = 0;
  for (size_t i = 0; i < n_docs; ++i) {
    uint64_t d = 0;
    SNII_RETURN_IF_ERROR(read_varint(&d));
    acc = static_cast<uint32_t>(acc + d);  // delta-decode (wraps validated by writer)
    current_.docids[i] = acc;
  }
  for (size_t i = 0; i < n_docs; ++i) {
    uint64_t f = 0;
    SNII_RETURN_IF_ERROR(read_varint(&f));
    if (f > UINT32_MAX) return Status::Corruption("run freq exceeds uint32");
    current_.freqs[i] = static_cast<uint32_t>(f);
  }
  uint64_t n_pos = 0;
  SNII_RETURN_IF_ERROR(read_varint(&n_pos));
  current_.positions.clear();
  if (has_positions_) {
    current_.positions.resize(static_cast<size_t>(n_docs));
    for (size_t i = 0; i < n_docs; ++i) {
      current_.positions[i].resize(current_.freqs[i]);
      for (uint32_t k = 0; k < current_.freqs[i]; ++k) {
        uint64_t p = 0;
        SNII_RETURN_IF_ERROR(read_varint(&p));
        current_.positions[i][k] = static_cast<uint32_t>(p);
      }
    }
  } else {
    for (uint64_t i = 0; i < n_pos; ++i) {
      uint64_t skip = 0;
      SNII_RETURN_IF_ERROR(read_varint(&skip));  // tolerate stray positions
    }
  }
  return Status::OK();
}

// ---------------------------------------------------------------------------
// K-way merge
// ---------------------------------------------------------------------------

namespace {

// Min-heap entry: orders by the run's current term, tie-broken by run index so
// equal terms are gathered run-order (keeping concatenated docids ascending).
struct HeapItem {
  std::string term;
  size_t run;
};
struct HeapGreater {
  bool operator()(const HeapItem& a, const HeapItem& b) const {
    if (a.term != b.term) return a.term > b.term;
    return a.run > b.run;
  }
};

// Appends src's postings onto dst (run order). Later runs only cover docids
// >= dst's last, so docids stay ascending. COALESCE the boundary doc: if a spill
// fell BETWEEN two tokens of the same doc, that doc ends one run and begins the
// next with the SAME docid -- merge them (sum freqs, append positions) so the
// merged term has exactly one entry per docid (matching the in-memory build).
void Concat(TermPostings* dst, const TermPostings& src, bool has_positions) {
  if (src.docids.empty()) return;
  size_t start = 0;
  if (!dst->docids.empty() && dst->docids.back() == src.docids.front()) {
    dst->freqs.back() += src.freqs.front();
    if (has_positions) {
      auto& tail = dst->positions.back();
      const auto& head = src.positions.front();
      tail.insert(tail.end(), head.begin(), head.end());
    }
    start = 1;  // boundary doc folded in; append the rest
  }
  dst->docids.insert(dst->docids.end(), src.docids.begin() + start, src.docids.end());
  dst->freqs.insert(dst->freqs.end(), src.freqs.begin() + start, src.freqs.end());
  if (has_positions) {
    dst->positions.insert(dst->positions.end(), src.positions.begin() + start,
                          src.positions.end());
  }
}

}  // namespace

Status MergeRuns(const std::vector<std::string>& run_paths, bool has_positions,
                 const std::function<void(TermPostings&&)>& fn) {
  std::vector<std::unique_ptr<RunReader>> readers;
  readers.reserve(run_paths.size());
  std::priority_queue<HeapItem, std::vector<HeapItem>, HeapGreater> heap;
  for (size_t i = 0; i < run_paths.size(); ++i) {
    auto r = std::make_unique<RunReader>();
    SNII_RETURN_IF_ERROR(r->open(run_paths[i], has_positions));
    if (!r->exhausted()) heap.push({r->current().term, i});
    readers.push_back(std::move(r));
  }

  while (!heap.empty()) {
    TermPostings merged;
    merged.term = heap.top().term;
    // Drain every run whose head equals this term, in ascending run order.
    while (!heap.empty() && heap.top().term == merged.term) {
      const size_t ri = heap.top().run;
      heap.pop();
      RunReader* r = readers[ri].get();
      Concat(&merged, r->current(), has_positions);
      SNII_RETURN_IF_ERROR(r->advance());
      if (!r->exhausted()) heap.push({r->current().term, ri});
    }
    fn(std::move(merged));
  }
  return Status::OK();
}

}  // namespace snii::writer
