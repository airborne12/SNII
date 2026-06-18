#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "snii/common/status.h"
#include "snii/query/bm25_scorer.h"
#include "snii/reader/logical_index_reader.h"
#include "snii/stats/snii_stats_provider.h"

// scoring_query -- top-K BM25 scored retrieval over one logical index for one or
// more query terms. Two entry points produce IDENTICAL rankings:
//   - scoring_query_exhaustive(): scores every candidate document (the baseline
//     correctness oracle).
//   - scoring_query_wand(): a block-max / WAND-style optimization that uses the
//     per-window max_freq / max_norm columns from the frq_prelude to bound each
//     window's best possible score and SKIP windows that cannot enter the
//     current top-K. A window without block-max stats (slim/inline entries or a
//     missing prelude) is never pruned, so the result still equals the
//     exhaustive ranking.
//
// Results are sorted by score descending; ties are broken by ascending docid so
// the ordering is deterministic and the two paths compare equal.
namespace snii::query {

// One scored hit.
struct ScoredDoc {
  uint32_t docid = 0;
  double score = 0.0;
};

// Exhaustive baseline: score every doc that contains any query term, return the
// top-k by score. params controls k1/b. Unknown terms are skipped.
Status scoring_query_exhaustive(const snii::reader::LogicalIndexReader& idx,
                                const snii::stats::SniiStatsProvider& stats,
                                const std::vector<std::string>& terms, uint32_t k,
                                const Bm25Params& params,
                                std::vector<ScoredDoc>* out);

// WAND-style block-max pruning. MUST return the same top-k as the exhaustive
// path. Windows whose block-max upper bound cannot beat the current k-th score
// are skipped; windows lacking block-max stats are scored fully.
Status scoring_query_wand(const snii::reader::LogicalIndexReader& idx,
                          const snii::stats::SniiStatsProvider& stats,
                          const std::vector<std::string>& terms, uint32_t k,
                          const Bm25Params& params, std::vector<ScoredDoc>* out);

}  // namespace snii::query
