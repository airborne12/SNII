#include "snii/writer/spimi_term_buffer.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <numeric>
#include <string>
#include <utility>

#include "snii/writer/spill_run_codec.h"

#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace snii::writer {

namespace {

// Returns freed heap arenas to the OS (glibc only). The spill encode churns many
// small allocations whose freed chunks glibc retains in its arenas; trimming
// before the peak-RSS-defining merge phase recovers that retention. No-op (and
// harmless) on non-glibc libcs.
void TrimMalloc() {
#if defined(__GLIBC__)
  ::malloc_trim(0);
#endif
}

// Per-element overhead used by the live-byte estimate (4-byte docid/freq/pos
// each, plus a rough per-term node + key cost). The exact constant is irrelevant
// to correctness (output is byte-identical regardless of when we spill); it only
// governs WHEN a spill fires relative to the threshold.
constexpr size_t kBytesPerDocEntry = 4 + 4;  // one docid + one freq
constexpr size_t kBytesPerPosition = 4;
constexpr size_t kBytesPerTermNode = 64;     // per-term struct + slack

// Process-unique temp path for a spill run (pid + monotonic counter so parallel
// builds / multiple buffers never collide).
std::string MakeRunPath() {
  static std::atomic<uint64_t> counter{0};
  const uint64_t n = counter.fetch_add(1);
  return "/tmp/snii_spill_" + std::to_string(::getpid()) + "_" + std::to_string(n) +
         ".run";
}

}  // namespace

SpimiTermBuffer::SpimiTermBuffer(const std::vector<std::string>* vocab,
                                 bool has_positions, size_t spill_threshold_bytes)
    : vocab_(vocab),
      has_positions_(has_positions),
      spill_threshold_bytes_(spill_threshold_bytes) {
  // Borrowed-vocab mode: only the 4 B/id slot-index array is sized to the
  // vocabulary; the Term pool (slots_) grows with the LIVE touched count, so an
  // all-but-empty vocabulary costs ~4 B/id instead of ~80 B/id.
  slot_of_.assign(vocab_->size(), 0);
}

SpimiTermBuffer::SpimiTermBuffer(bool has_positions, size_t spill_threshold_bytes)
    : vocab_(&owned_vocab_),
      has_positions_(has_positions),
      spill_threshold_bytes_(spill_threshold_bytes) {
  // Owned-vocab mode: the vocabulary grows as strings are interned; terms_ /
  // present_ grow alongside it in add_token(string_view, ...).
}

SpimiTermBuffer::~SpimiTermBuffer() { cleanup_runs(); }

size_t SpimiTermBuffer::unique_terms() const { return live_term_count_; }

void SpimiTermBuffer::account_token(uint32_t term_id, bool new_term, bool new_doc) {
  if (new_term) live_bytes_ += kBytesPerTermNode + vocab()[term_id].size();
  if (new_doc) live_bytes_ += kBytesPerDocEntry;
  if (has_positions_) live_bytes_ += kBytesPerPosition;
}

// Returns the live Term for `term_id`, claiming a pool slot on first touch (1 ==
// new). Reuses a freed slot from free_slots_ when available; otherwise appends a
// fresh Term to slots_. slot_of_[term_id] holds (slot index + 1); 0 means empty.
SpimiTermBuffer::Term& SpimiTermBuffer::term_slot(uint32_t term_id, bool* new_term) {
  uint32_t enc = slot_of_[term_id];
  if (enc != 0) {
    *new_term = false;
    return slots_[enc - 1];
  }
  *new_term = true;
  uint32_t slot;
  if (!free_slots_.empty()) {
    slot = free_slots_.back();
    free_slots_.pop_back();
  } else {
    slot = static_cast<uint32_t>(slots_.size());
    slots_.emplace_back();
  }
  slot_of_[term_id] = slot + 1;
  return slots_[slot];
}

void SpimiTermBuffer::accumulate(uint32_t term_id, uint32_t docid, uint32_t pos) {
  bool new_term = false;
  Term& t = term_slot(term_id, &new_term);
  if (new_term) {
    touched_ids_.push_back(term_id);
    ++live_term_count_;
  }
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
    account_token(term_id, new_term, new_doc);
    if (live_bytes_ >= spill_threshold_bytes_ && spill_status_.ok()) {
      spill_status_ = spill_to_run();
    }
  }
}

void SpimiTermBuffer::add_token(uint32_t term_id, uint32_t docid, uint32_t pos) {
  // Hot path: a pooled slot lookup + a couple of pushes. No hashing, no string
  // construction per token. Reject (and latch) an out-of-range id.
  if (term_id >= slot_of_.size()) {
    if (spill_status_.ok()) {
      spill_status_ = Status::InvalidArgument("spimi: term_id out of vocab range");
    }
    return;
  }
  accumulate(term_id, docid, pos);
}

void SpimiTermBuffer::add_token(std::string_view term, uint32_t docid, uint32_t pos) {
  // Compatibility path: intern the term into the owned vocabulary on first
  // occurrence, then accumulate by its id. Only valid in owned-vocab mode.
  auto it = intern_.find(std::string(term));
  uint32_t term_id;
  if (it == intern_.end()) {
    term_id = static_cast<uint32_t>(owned_vocab_.size());
    owned_vocab_.emplace_back(term);
    intern_.emplace(owned_vocab_.back(), term_id);
    slot_of_.push_back(0);  // vocab grows: new id starts with no live slot
  } else {
    term_id = it->second;
  }
  accumulate(term_id, docid, pos);
}

namespace {

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
    // Flat positions move straight through -- no per-doc vector-of-vectors built.
    tp.positions_flat = std::move(t.positions_flat);
  }
  return tp;
}

std::vector<uint32_t> SpimiTermBuffer::sorted_ids() const {
  std::vector<uint32_t> ids = touched_ids_;
  const std::vector<std::string>& v = vocab();
  std::sort(ids.begin(), ids.end(),
            [&](uint32_t a, uint32_t b) { return v[a] < v[b]; });
  return ids;
}

void SpimiTermBuffer::release_term(uint32_t term_id) {
  const uint32_t enc = slot_of_[term_id];
  if (enc == 0) return;  // not live (defensive)
  const uint32_t slot = enc - 1;
  slots_[slot] = Term();  // free this term's arrays; the empty Term slot is reusable
  free_slots_.push_back(slot);
  slot_of_[term_id] = 0;
  --live_term_count_;
}

void SpimiTermBuffer::drain_sorted(const std::function<void(TermPostings&&)>& fn) {
  const std::vector<std::string>& v = vocab();
  for (uint32_t id : sorted_ids()) {
    Term term = std::move(slots_[slot_of_[id] - 1]);
    release_term(id);  // release this term's slot before building the next
    TermPostings tp = to_postings(v[id], std::move(term));
    fn(std::move(tp));
  }
  touched_ids_.clear();
  live_bytes_ = 0;
}

Status SpimiTermBuffer::drain_to_writer(RunWriter* w) {
  Status st = Status::OK();
  const std::vector<std::string>& v = vocab();
  // Spill writes by term-id (no string IO). Iterate touched ids in vocab-string
  // order so each run is sorted; the k-way merge re-orders runs by the same key.
  for (uint32_t id : sorted_ids()) {
    Term term = std::move(slots_[slot_of_[id] - 1]);
    release_term(id);
    TermPostings tp = to_postings(v[id], std::move(term));
    if (st.ok()) st = w->write_term(id, tp);
  }
  touched_ids_.clear();
  live_bytes_ = 0;
  return st;
}

Status SpimiTermBuffer::spill_to_run() {
  const std::string path = MakeRunPath();
  RunWriter w;
  SNII_RETURN_IF_ERROR(w.open(path));
  run_paths_.push_back(path);  // tracked for cleanup even if a later step fails
  SNII_RETURN_IF_ERROR(drain_to_writer(&w));
  // drain emptied touched_ids_ and freed each term's arrays; terms_/present_ keep
  // their (vocab-sized) capacity so the next fill reuses the dense slots with no
  // re-allocation. present_ is already all-zero after release_term per id.
  return w.close();
}

void SpimiTermBuffer::merge_runs(const std::function<void(TermPostings&&)>& fn) {
  // Flush whatever is still resident as one final sorted run so the k-way merge
  // sees a uniform set of run files (and never holds two term sources at once).
  if (!touched_ids_.empty()) {
    Status s = spill_to_run();
    if (!s.ok() && spill_status_.ok()) spill_status_ = s;
  }
  if (!spill_status_.ok()) return;  // a spill failed earlier; emit nothing
  // All terms are now spilled; the merge reads runs and never touches the
  // accumulators. Free the pool + the vocab-sized slot index so the merge phase
  // holds none of the input-side arrays resident -- keeps spill-mode peak RSS
  // down. malloc_trim(0) returns the freed glibc arenas to the OS so the peak RSS
  // measurement reflects the merge transient, not retained input-phase chunks.
  std::vector<Term>().swap(slots_);
  std::vector<uint32_t>().swap(free_slots_);
  std::vector<uint32_t>().swap(slot_of_);
  TrimMalloc();
  Status s = MergeRuns(run_paths_, vocab(), has_positions_, fn);
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
  out.reserve(touched_ids_.size());
  for_each_term_sorted([&out](TermPostings&& tp) { out.push_back(std::move(tp)); });
  return out;
}

void SpimiTermBuffer::cleanup_runs() {
  for (const std::string& p : run_paths_) std::remove(p.c_str());
  run_paths_.clear();
}

}  // namespace snii::writer
