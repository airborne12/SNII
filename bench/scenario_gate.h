#pragma once

// BENCH.4 -- wire the gate into the synthetic-corpus scenario suite.
//
// This is the join point that turns the existing "print-only" df-calibrated
// scenario catalog (resolve_scenarios() in main.cpp) into a reproducible,
// threshold-gated benchmark SUITE. For one resolved scenario it produces a fully
// populated BenchRow (BENCH.3): it resolves the corpus scale from doc_count
// (scale_of, BENCH.2), looks up the declared per-scenario thresholds
// (thresholds_for, BENCH.2), evaluates every golden metric (evaluate_metric,
// BENCH.1), and computes
//
//     overall_pass = docids_match AND (every manifest metric verdict passes)
//
// so a docid-equality correctness failure overrides ANY metric advantage. The
// scenario mode collects one row per scenario, optionally emits JSONL, and exits
// non-zero iff any row fails (all_passed).
//
// Pure: no I/O, no CLucene, no corpus, no build. It takes already-measured
// IoMetrics (the SAME MeteredFileReader yardstick on both engines) plus the
// docid-equality result, and returns the gated row. Unit-testable on literal
// IoMetrics with zero filesystem / network.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bench_jsonl.h"
#include "bench_manifest.h"
#include "snii/io/metered_file_reader.h"

namespace bench {

// Build the gated BenchRow for one scenario. `surface` is "local" (cost model)
// or "oss" (real OSS); it selects the Surface manifest bucket. The scale bucket
// is derived from `doc_count` via scale_of(). Each declared metric threshold is
// evaluated through evaluate_metric() and appended to row.verdicts; the row's
// overall_pass is docids_match AND every verdict.pass. When docids_match is
// false the row fails regardless of how good the metrics look (the correctness
// gate dominates). A scenario whose manifest declares no metric gates (e.g. an
// absent df=0 term) passes on docid equality alone with an empty verdict list.
BenchRow build_row(const std::string& id, const char* surface,
                   uint32_t doc_count, uint64_t seed, const std::string& git_rev,
                   size_t hits, bool docids_match,
                   const snii::io::IoMetrics& clucene,
                   const snii::io::IoMetrics& snii);

// True iff every row's overall_pass is true. Vacuously true for an empty set
// (a corpus too small to resolve any scenario must not crash the gate).
bool all_passed(const std::vector<BenchRow>& rows);

}  // namespace bench
