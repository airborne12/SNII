#pragma once

// BENCH.2 -- declarative golden-metric threshold manifest.
//
// Pure data + lookup: no I/O, no build, no CLucene, no SNII-reader. Given a
// (scenario_id, scale, surface) triple this returns the declared set of
// MetricThreshold (BENCH.1) plus whether docid-equality is a hard correctness
// gate for that item. This is the single place a reviewer reads every declared
// pass/fail threshold of the SNII-vs-CLucene suite.
//
// The numbers are sourced from docs/benchmark-results.md (the measured 5M
// cost-model and real-OSS results) and the SNII status memo. The manifest
// encodes the design intent "SNII's advantage grows with scale": the 5M caps
// are equal-or-tighter than the 150K caps, honest-envelope scenarios (dense AND,
// all-stopword) gate parity-or-better only, and df=0 scenarios gate docid
// equality alone (a 0/0 byte ratio carries no signal).

#include <cstdint>
#include <string>
#include <vector>

#include "metric_gate.h"

namespace bench {

// Corpus-size bucket. The single documented boundary is 1'000'000 docs:
// anything below is the small (150K-class) bucket, anything at or above is the
// large (5M-class) bucket. Smaller corpora get looser caps because SNII's
// batch-range advantage over CLucene's cursor reads grows with data size.
enum class Scale {
  kSmall150K,
  kLarge5M,
};

// Measurement surface. The local cost model is deterministic (exact request
// bytes / serial rounds via MeteredFileReader); the real-OSS surface carries
// wall-clock noise and therefore tolerates looser thresholds.
enum class Surface {
  kLocalCostModel,
  kRealOss,
};

// Bucket a doc_count into its Scale. The boundary is inclusive at 1'000'000:
// scale_of(999999) == kSmall150K, scale_of(1000000) == kLarge5M.
Scale scale_of(uint32_t doc_count);

// The declared thresholds for one benchmark item. `metrics` is the (possibly
// empty) set of golden-metric gates; `require_docid_equality` is the hard
// correctness gate that overrides any metric verdict (SNII must return the same
// docid set as the CLucene oracle).
struct ItemThresholds {
  std::vector<MetricThreshold> metrics;
  bool require_docid_equality = true;
};

// Resolve the declared thresholds for a (scenario_id, scale, surface) item.
// Known ids resolve to their documented caps (per-scale tightening); unknown ids
// fall back to a permissive parity gate that still requires docid equality, so a
// newly added scenario neither auto-fails nor silently wins. Never throws.
ItemThresholds thresholds_for(const std::string& scenario_id, Scale scale,
                              Surface surface);

}  // namespace bench
