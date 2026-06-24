#pragma once

// BENCH.1 -- pure golden-metric verdict core.
//
// Zero I/O, zero CLucene, zero SNII-reader dependency: this header declares the
// gate that turns two snii::io::IoMetrics samples (the CLucene control vs the
// SNII engine, measured through the SAME MeteredFileReader yardstick) plus a
// declared threshold into a structured PASS/FAIL verdict. Every later benchmark
// item asserts its three golden metrics (serial_rounds, range_gets,
// total_request_bytes) through evaluate_metric().

#include <cstdint>
#include <string>

#include "snii/io/metered_file_reader.h"

namespace bench {

// How a metric is gated against its threshold.
enum class GateCmp {
  // SNII must be <= max_ratio_cl_over_snii x better-or-equal than CLucene, i.e.
  // the advantage ratio gate_ratio(CLucene, SNII) must be >= the cap. A cap
  // <= 0 degrades to "parity-or-better" (snii <= clucene): the honest
  // break-even gate for SNII's known-worst scenarios (dense AND, all-stopword).
  kLeRatio,
  // The SNII metric itself must be <= max_absolute (an absolute cap, e.g. the
  // 5M-phrase head serial_rounds cap). CLucene is recorded but not gated.
  kLeAbsolute,
  // The metric must be exactly equal on both engines (a parity invariant, e.g.
  // range_gets when both engines must coalesce to the same GET count).
  kEqual,
};

// A declared threshold for one named golden metric.
struct MetricThreshold {
  const char* metric_name;          // one of the three golden metric names
  GateCmp cmp;                       // comparison operator
  double max_ratio_cl_over_snii;     // for kLeRatio: required advantage cap
  uint64_t max_absolute;             // for kLeAbsolute: absolute cap on SNII
};

// The structured outcome of gating one metric.
struct MetricVerdict {
  std::string metric_name;
  uint64_t clucene;  // CLucene control value
  uint64_t snii;     // SNII engine value
  double ratio;      // gate_ratio(clucene, snii) (CL/SNII advantage)
  bool pass;
  std::string reason;  // human-readable explanation of the verdict
};

// CLucene-over-SNII advantage ratio. Byte-for-byte replica of bench/main.cpp
// ratio(): when snii == 0 the result is 1.0 if clucene is also 0 (absent term,
// no false regression) else (double)clucene (div-by-zero guard); otherwise the
// plain quotient.
double gate_ratio(uint64_t clucene, uint64_t snii);

// Resolve a golden-metric value by name. Throws std::invalid_argument on any
// name that is not one of {serial_rounds, range_gets, total_request_bytes}, so
// a manifest typo fails loud instead of silently gating the wrong metric.
uint64_t pick_metric(const snii::io::IoMetrics& m, const char* name);

// Evaluate one threshold against the two engines' metrics. Pure: no side
// effects, no I/O. Throws std::invalid_argument (via pick_metric) on an unknown
// metric name.
MetricVerdict evaluate_metric(const snii::io::IoMetrics& clucene,
                              const snii::io::IoMetrics& snii,
                              const MetricThreshold& threshold);

}  // namespace bench
