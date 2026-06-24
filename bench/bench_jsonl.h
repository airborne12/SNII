#pragma once

// BENCH.3 -- JSONL result emitter.
//
// One reproducible, machine-readable record per benchmark item: the two engines'
// raw golden metrics (the SAME snii::io::IoMetrics yardstick), the docid-equality
// correctness gate, every per-metric MetricVerdict (BENCH.1), and the reproduction
// metadata (corpus seed, scale=doc_count, git rev, surface) needed to replay the
// row. Pure serializer: no filesystem, no CLucene, no corpus, no build -- it takes
// a fully populated BenchRow and writes exactly one line of valid JSON.
//
// The emitted JSONL is the CI-gating artifact: a pipeline diffs it across commits,
// and the manifest gate (BENCH.4) reads it back. Metrics are written as integer
// literals (never scientific notation) so a 5M-scale 139377956-byte fetch shows up
// as the exact integer a CI diff can compare.

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#include "metric_gate.h"
#include "snii/io/metered_file_reader.h"

namespace bench {

// One benchmark item's full result: enough to gate it and to replay it.
struct BenchRow {
  std::string scenario_id;        // e.g. "PHRASE-5", "TERM-HIGH-DF"
  std::string surface;            // "local" (cost-model) or "oss" (real OSS)
  uint32_t doc_count = 0;         // corpus scale (drives 150K/5M threshold bucket)
  uint64_t seed = 0;              // corpus_gen seed (reproducibility)
  std::string git_rev;            // git revision the row was produced at
  size_t hits = 0;                // result-set size (correctness cross-check)
  bool docids_match = false;      // the standing docid-equality correctness gate
  snii::io::IoMetrics clucene;    // CLucene control metrics
  snii::io::IoMetrics snii;       // SNII engine metrics
  std::vector<MetricVerdict> verdicts;  // per golden-metric PASS/FAIL (may be empty)
  bool overall_pass = false;      // docids_match AND every verdict.pass
};

// JSON string escaping: backslash and double-quote are backslash-escaped; the
// control characters JSON forbids bare (\n, \r, \t, and other < 0x20) are emitted
// as their JSON escapes so a stray newline can never split a JSONL record.
std::string json_escape(const std::string& s);

// Write exactly one JSON object followed by exactly one trailing '\n'. All metric
// values are integer literals (PRIu64 / %zu), never floating point, so large
// uint64 byte counts keep full integer fidelity in a CI diff. Empty verdicts emit
// a legal empty array "[]" (not a missing key, not a trailing comma).
void write_jsonl(std::ostream& os, const BenchRow& row);

}  // namespace bench
