#include "snii/writer/spimi_term_buffer.h"

#include <algorithm>
#include <utility>

namespace snii::writer {

SpimiTermBuffer::SpimiTermBuffer(bool has_positions) : has_positions_(has_positions) {}

size_t SpimiTermBuffer::unique_terms() const { return data_.size(); }

void SpimiTermBuffer::add_token(std::string_view term, uint32_t docid, uint32_t pos) {
  DocEntry& e = data_[std::string(term)][docid];
  ++e.freq;
  if (has_positions_) e.positions.push_back(pos);
  ++total_tokens_;
}

std::vector<TermPostings> SpimiTermBuffer::finalize_sorted() {
  std::vector<TermPostings> out;
  out.reserve(data_.size());
  for (auto& [term, docs] : data_) {
    TermPostings tp;
    tp.term = term;
    tp.docids.reserve(docs.size());
    tp.freqs.reserve(docs.size());
    if (has_positions_) tp.positions.reserve(docs.size());
    for (auto& [docid, entry] : docs) {  // std::map -> ascending docid
      tp.docids.push_back(docid);
      tp.freqs.push_back(entry.freq);
      if (has_positions_) tp.positions.push_back(std::move(entry.positions));
    }
    out.push_back(std::move(tp));
  }
  std::sort(out.begin(), out.end(),
            [](const TermPostings& a, const TermPostings& b) { return a.term < b.term; });
  return out;
}

}  // namespace snii::writer
