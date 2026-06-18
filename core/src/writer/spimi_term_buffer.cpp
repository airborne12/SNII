#include "snii/writer/spimi_term_buffer.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <numeric>
#include <string>
#include <utility>

#include "snii/writer/spill_run_codec.h"

namespace snii::writer {

namespace {

// Per-element overhead used by the live-byte estimate (4-byte docid/freq/pos
// each, plus a rough per-term map-node + key cost). The exact constant is
// irrelevant to correctness (output is byte-identical regardless of when we
// spill); it only governs WHEN a spill fires relative to the threshold.
constexpr size_t kBytesPerDocEntry = 4 + 4;  // one docid + one freq
constexpr size_t kBytesPerPosition = 4;
constexpr size_t kBytesPerTermNode = 64;     // map node + small Term struct

// Process-unique temp path for a spill run (pid + monotonic counter so parallel
// builds / multiple buffers never collide).
std::string MakeRunPath() {
  static std::atomic<uint64_t> counter{0};
  const uint64_t n = counter.fetch_add(1);
  return "/tmp/snii_spill_" + std::to_string(::getpid()) + "_" + std::to_string(n) +
         ".run";
}

}  // namespace

SpimiTermBuffer::SpimiTermBuffer(bool has_positions, size_t spill_threshold_bytes)
    : has_positions_(has_positions), spill_threshold_bytes_(spill_threshold_bytes) {}

SpimiTermBuffer::~SpimiTermBuffer() { cleanup_runs(); }

size_t SpimiTermBuffer::unique_terms() const { return data_.size(); }

void SpimiTermBuffer::account_token(const std::string& term, bool new_term,
                                    bool new_doc) {
  if (new_term) live_bytes_ += kBytesPerTermNode + term.size();
  if (new_doc) live_bytes_ += kBytesPerDocEntry;
  if (has_positions_) live_bytes_ += kBytesPerPosition;
}

void SpimiTermBuffer::add_token(std::string_view term, uint32_t docid, uint32_t pos) {
  // Probe by string_view (transparent hash/equal): a std::string is heap-built
  // only when the term is new, not on every (hit) token. This removes one
  // allocation + copy per token on the dominant repeated-term path.
  auto it = data_.find(term);
  bool inserted = false;
  if (it == data_.end()) {
    it = data_.emplace(std::string(term), Term{}).first;
    inserted = true;
  }
  Term& t = it->second;
  const bool new_doc = t.docids.empty() || t.docids.back() != docid;
  if (new_doc) {
    if (!t.docids.empty() && docid < t.docids.back()) t.sorted = false;
    t.docids.push_back(docid);
    t.freqs.push_back(0);
  }
  ++t.freqs.back();
  if (has_positions_) t.positions_flat.push_back(pos);
  ++total_tokens_;

  if (spill_threshold_bytes_ != 0) {
    account_token(it->first, inserted, new_doc);
    if (live_bytes_ >= spill_threshold_bytes_ && spill_status_.ok()) {
      spill_status_ = spill_to_run();
    }
  }
}

namespace {

// Re-slices a term's flat positions (document order, partitioned by freqs) into
// per-doc position lists. positions_flat[run] maps to doc i for run lengths freqs[i].
std::vector<std::vector<uint32_t>> SlicePositions(const std::vector<uint32_t>& freqs,
                                                  std::vector<uint32_t>&& flat) {
  std::vector<std::vector<uint32_t>> out(freqs.size());
  size_t off = 0;
  for (size_t i = 0; i < freqs.size(); ++i) {
    const size_t n = freqs[i];
    out[i].assign(flat.begin() + off, flat.begin() + off + n);
    off += n;
  }
  return out;
}

// Reorders a term's flat arrays into ascending-docid order. Only invoked for the
// rare term that received out-of-order docids; the common path stays untouched.
void SortByDocid(std::vector<uint32_t>* docids, std::vector<uint32_t>* freqs,
                 std::vector<uint32_t>* positions_flat, bool has_positions) {
  const size_t n = docids->size();
  std::vector<size_t> order(n);
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(),
            [&](size_t a, size_t b) { return (*docids)[a] < (*docids)[b]; });

  std::vector<uint32_t> pos_off;
  if (has_positions) {
    pos_off.resize(n);
    uint32_t running = 0;
    for (size_t i = 0; i < n; ++i) {
      pos_off[i] = running;
      running += (*freqs)[i];
    }
  }
  std::vector<uint32_t> nd, nf, np;
  nd.reserve(n);
  nf.reserve(n);
  if (has_positions) np.reserve(positions_flat->size());
  for (size_t k : order) {
    nd.push_back((*docids)[k]);
    nf.push_back((*freqs)[k]);
    if (has_positions) {
      np.insert(np.end(), positions_flat->begin() + pos_off[k],
                positions_flat->begin() + pos_off[k] + (*freqs)[k]);
    }
  }
  *docids = std::move(nd);
  *freqs = std::move(nf);
  if (has_positions) *positions_flat = std::move(np);
}

}  // namespace

TermPostings SpimiTermBuffer::to_postings(std::string term, Term&& t) const {
  if (!t.sorted) {
    SortByDocid(&t.docids, &t.freqs, &t.positions_flat, has_positions_);
  }
  TermPostings tp;
  tp.term = std::move(term);
  tp.docids = std::move(t.docids);
  tp.freqs = std::move(t.freqs);
  if (has_positions_) {
    tp.positions = SlicePositions(tp.freqs, std::move(t.positions_flat));
  }
  return tp;
}

std::vector<const std::string*> SpimiTermBuffer::sorted_keys() const {
  std::vector<const std::string*> keys;
  keys.reserve(data_.size());
  for (const auto& [term, _] : data_) keys.push_back(&term);
  std::sort(keys.begin(), keys.end(),
            [](const std::string* a, const std::string* b) { return *a < *b; });
  return keys;
}

void SpimiTermBuffer::drain_sorted(const std::function<void(TermPostings&&)>& fn) {
  for (const std::string* key : sorted_keys()) {
    auto it = data_.find(*key);
    Term term = std::move(it->second);
    TermPostings tp = to_postings(*key, std::move(term));
    data_.erase(it);  // release this term's arrays before building the next
    fn(std::move(tp));
  }
  live_bytes_ = 0;
}

Status SpimiTermBuffer::drain_to_writer(RunWriter* w) {
  Status st = Status::OK();
  drain_sorted([&](TermPostings&& tp) {
    if (st.ok()) st = w->write_term(tp);
  });
  return st;
}

Status SpimiTermBuffer::spill_to_run() {
  const std::string path = MakeRunPath();
  RunWriter w;
  SNII_RETURN_IF_ERROR(w.open(path));
  run_paths_.push_back(path);  // tracked for cleanup even if a later step fails
  SNII_RETURN_IF_ERROR(drain_to_writer(&w));
  // drain_sorted erases every entry but leaves the bucket array at its grown
  // capacity; swap in a fresh map so that capacity is returned to the allocator
  // and the next fill restarts from a small table (keeps peak RSS bounded).
  decltype(data_)().swap(data_);
  return w.close();
}

void SpimiTermBuffer::merge_runs(const std::function<void(TermPostings&&)>& fn) {
  // Flush whatever is still resident as one final sorted run so the k-way merge
  // sees a uniform set of run files (and never holds two term sources at once).
  if (!data_.empty()) {
    Status s = spill_to_run();
    if (!s.ok() && spill_status_.ok()) spill_status_ = s;
  }
  if (!spill_status_.ok()) return;  // a spill failed earlier; emit nothing
  Status s = MergeRuns(run_paths_, has_positions_, fn);
  if (!s.ok() && spill_status_.ok()) spill_status_ = s;
}

void SpimiTermBuffer::for_each_term_sorted(const std::function<void(TermPostings&&)>& fn) {
  if (run_paths_.empty() && spill_status_.ok()) {
    drain_sorted(fn);  // pure in-memory path: byte-for-byte the legacy behavior
    return;
  }
  merge_runs(fn);
}

std::vector<TermPostings> SpimiTermBuffer::finalize_sorted() {
  std::vector<TermPostings> out;
  out.reserve(data_.size());
  for_each_term_sorted([&out](TermPostings&& tp) { out.push_back(std::move(tp)); });
  return out;
}

void SpimiTermBuffer::cleanup_runs() {
  for (const std::string& p : run_paths_) std::remove(p.c_str());
  run_paths_.clear();
}

}  // namespace snii::writer
