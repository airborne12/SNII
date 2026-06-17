#include "snii/io/batch_range_fetcher.h"

#include <algorithm>

namespace snii::io {

BatchRangeFetcher::BatchRangeFetcher(FileReader* reader, uint64_t coalesce_gap)
    : reader_(reader), coalesce_gap_(coalesce_gap) {}

size_t BatchRangeFetcher::add(uint64_t offset, size_t len) {
  reqs_.push_back(Req{offset, len});
  return reqs_.size() - 1;
}

void BatchRangeFetcher::clear() {
  reqs_.clear();
  phys_.clear();
}

Status BatchRangeFetcher::fetch() {
  phys_.clear();
  if (reqs_.empty()) return Status::OK();

  std::vector<size_t> order(reqs_.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(),
            [&](size_t a, size_t b) { return reqs_[a].offset < reqs_[b].offset; });

  // Sweep in offset order, merging requests into physical segments.
  std::vector<Range> segs;
  uint64_t cur_start = 0;
  uint64_t cur_end = 0;
  for (size_t k = 0; k < order.size(); ++k) {
    Req& r = reqs_[order[k]];
    const uint64_t r_end = r.offset + r.len;
    if (segs.empty() || r.offset > cur_end + coalesce_gap_) {
      segs.push_back(Range{r.offset, 0});  // length finalized below
      cur_start = r.offset;
      cur_end = r_end;
    } else {
      cur_end = std::max(cur_end, r_end);
    }
    r.phys_idx = segs.size() - 1;
    r.sub_offset = static_cast<size_t>(r.offset - cur_start);
    segs.back().len = static_cast<size_t>(cur_end - cur_start);
  }

  return reader_->read_batch(segs, &phys_);
}

Slice BatchRangeFetcher::get(size_t h) const {
  const Req& r = reqs_[h];
  const std::vector<uint8_t>& buf = phys_[r.phys_idx];
  return Slice(buf.data() + r.sub_offset, r.len);
}

}  // namespace snii::io
