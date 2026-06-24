#include "scenario_gate.h"

#include <string>

#include "bench_manifest.h"
#include "metric_gate.h"

namespace bench {

namespace {

// Map the JSONL surface label to its manifest Surface bucket. Anything that is
// not the explicit "oss" real-object-store label is treated as the deterministic
// local cost model (the conservative default: the strict, non-noise-tolerant
// thresholds).
Surface surface_of(const char* surface) {
  const std::string s = surface == nullptr ? std::string() : std::string(surface);
  return s == "oss" ? Surface::kRealOss : Surface::kLocalCostModel;
}

}  // namespace

BenchRow build_row(const std::string& id, const char* surface,
                   uint32_t doc_count, uint64_t seed, const std::string& git_rev,
                   size_t hits, bool docids_match,
                   const snii::io::IoMetrics& clucene,
                   const snii::io::IoMetrics& snii) {
  BenchRow row;
  row.scenario_id = id;
  row.surface = surface == nullptr ? std::string() : std::string(surface);
  row.doc_count = doc_count;
  row.seed = seed;
  row.git_rev = git_rev;
  row.hits = hits;
  row.docids_match = docids_match;
  row.clucene = clucene;
  row.snii = snii;

  const Scale scale = scale_of(doc_count);
  const ItemThresholds thresholds = thresholds_for(id, scale, surface_of(surface));

  // The correctness gate dominates: even a perfect set of metric verdicts cannot
  // rescue a row whose docid set disagrees with the CLucene oracle.
  bool overall = docids_match;
  for (const MetricThreshold& t : thresholds.metrics) {
    const MetricVerdict v = evaluate_metric(clucene, snii, t);
    overall = overall && v.pass;
    row.verdicts.push_back(v);
  }
  row.overall_pass = overall;
  return row;
}

bool all_passed(const std::vector<BenchRow>& rows) {
  for (const BenchRow& r : rows) {
    if (!r.overall_pass) return false;
  }
  return true;  // vacuously true for an empty set
}

}  // namespace bench
