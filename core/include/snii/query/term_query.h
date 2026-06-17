#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "snii/common/status.h"
#include "snii/reader/logical_index_reader.h"

// term_query -- the simplest SNII query: return the sorted docid set that
// contains term. It runs the term lookup on the logical index, then issues a
// single batched .frq range read (one serial round) to decode the postings.
// Absent term -> empty result (OK status).
namespace snii::query {

Status term_query(const snii::reader::LogicalIndexReader& idx,
                  std::string_view term, std::vector<uint32_t>* docids);

}  // namespace snii::query
