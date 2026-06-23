#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "snii/common/status.h"
#include "snii/reader/logical_index_reader.h"

// phrase_query -- MATCH_PHRASE: return the sorted docid set in which the terms
// occur consecutively (for some i, every term k appears at position pos+k in the
// same doc). It plans ALL needed .frq and .prx ranges for every term up front,
// fetches them in a single batched read (one serial round), then performs a
// positional merge:
//   1. intersect the per-term docid sets;
//   2. for each surviving doc, check that some position p exists with
//      term[0]@p, term[1]@p+1, ... term[n-1]@p+(n-1).
// An empty term list -> empty result. Any term absent -> empty result.
namespace snii::query {

Status phrase_query(const snii::reader::LogicalIndexReader& idx,
                    const std::vector<std::string>& terms,
                    std::vector<uint32_t>* docids);

// boolean_and (MATCH all-terms): sorted docid set of docs containing EVERY term,
// no positional constraint. Reuses the docid-only conjunction (min-df driver +
// covering-window reads for high-df terms) but fetches NO positions, so bytes scale
// with selectivity rather than df. Valid on docs-only indexes. Empty terms or any
// absent term -> empty result.
Status boolean_and(const snii::reader::LogicalIndexReader& idx,
                   const std::vector<std::string>& terms,
                   std::vector<uint32_t>* docids);

}  // namespace snii::query
