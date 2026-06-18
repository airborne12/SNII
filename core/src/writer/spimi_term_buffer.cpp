#include "snii/writer/spimi_term_buffer.h"

#include <algorithm>
#include <numeric>
#include <utility>

namespace snii::writer {

SpimiTermBuffer::SpimiTermBuffer(bool has_positions) : has_positions_(has_positions) {}

size_t SpimiTermBuffer::unique_terms() const { return data_.size(); }

void SpimiTermBuffer::add_token(std::string_view term, uint32_t docid, uint32_t pos) {
  Term& t = data_[std::string(term)];
  if (t.docids.empty() || t.docids.back() != docid) {
    if (!t.docids.empty() && docid < t.docids.back()) t.sorted = false;
    t.docids.push_back(docid);
    t.freqs.push_back(0);
  }
  ++t.freqs.back();
  if (has_positions_) t.positions_flat.push_back(pos);
  ++total_tokens_;
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

void SpimiTermBuffer::for_each_term_sorted(const std::function<void(TermPostings&&)>& fn) {
  std::vector<const std::string*> keys;
  keys.reserve(data_.size());
  for (const auto& [term, _] : data_) keys.push_back(&term);
  std::sort(keys.begin(), keys.end(),
            [](const std::string* a, const std::string* b) { return *a < *b; });

  for (const std::string* key : keys) {
    auto it = data_.find(*key);
    Term term = std::move(it->second);
    TermPostings tp = to_postings(*key, std::move(term));
    data_.erase(it);  // release this term's arrays before building the next
    fn(std::move(tp));
  }
}

std::vector<TermPostings> SpimiTermBuffer::finalize_sorted() {
  std::vector<TermPostings> out;
  out.reserve(data_.size());
  for_each_term_sorted([&out](TermPostings&& tp) { out.push_back(std::move(tp)); });
  return out;
}

}  // namespace snii::writer
