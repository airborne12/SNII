#include "metric_gate.h"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace bench {

namespace {
// Inclusive epsilon for the kLeRatio boundary: an advantage that lands exactly
// on the cap (e.g. 2.0x against a 2.0x cap) must PASS, but a one-ulp regression
// must not be masked. 1e-9 is far below any real integer-metric ratio step.
constexpr double kRatioEpsilon = 1e-9;
}  // namespace

double gate_ratio(uint64_t clucene, uint64_t snii) {
  // Strict replica of bench/main.cpp ratio() (lines 243-246).
  if (snii == 0) {
    return clucene == 0 ? 1.0 : static_cast<double>(clucene);
  }
  return static_cast<double>(clucene) / static_cast<double>(snii);
}

uint64_t pick_metric(const snii::io::IoMetrics& m, const char* name) {
  const std::string n = name == nullptr ? std::string() : std::string(name);
  if (n == "serial_rounds") return m.serial_rounds;
  if (n == "range_gets") return m.range_gets;
  if (n == "total_request_bytes") return m.total_request_bytes;
  throw std::invalid_argument("unknown golden metric name: '" + n + "'");
}

MetricVerdict evaluate_metric(const snii::io::IoMetrics& clucene,
                              const snii::io::IoMetrics& snii,
                              const MetricThreshold& threshold) {
  const uint64_t cl = pick_metric(clucene, threshold.metric_name);
  const uint64_t sn = pick_metric(snii, threshold.metric_name);
  const double ratio = gate_ratio(cl, sn);

  MetricVerdict v;
  v.metric_name = threshold.metric_name;
  v.clucene = cl;
  v.snii = sn;
  v.ratio = ratio;

  char buf[256];
  switch (threshold.cmp) {
    case GateCmp::kLeRatio: {
      if (threshold.max_ratio_cl_over_snii <= 0.0) {
        // Parity-or-better sentinel: SNII must not fetch/round more than CLucene.
        v.pass = sn <= cl;
        std::snprintf(buf, sizeof(buf),
                      "parity-or-better: snii=%llu <= clucene=%llu",
                      static_cast<unsigned long long>(sn),
                      static_cast<unsigned long long>(cl));
      } else {
        // Advantage gate: gate_ratio(cl, sn) must be >= cap (inclusive).
        v.pass = ratio + kRatioEpsilon >= threshold.max_ratio_cl_over_snii;
        std::snprintf(buf, sizeof(buf),
                      "ratio(CL/SNII)=%.6f >= cap=%.6f",
                      ratio, threshold.max_ratio_cl_over_snii);
      }
      break;
    }
    case GateCmp::kLeAbsolute: {
      v.pass = sn <= threshold.max_absolute;
      std::snprintf(buf, sizeof(buf), "snii=%llu <= cap=%llu",
                    static_cast<unsigned long long>(sn),
                    static_cast<unsigned long long>(threshold.max_absolute));
      break;
    }
    case GateCmp::kEqual: {
      v.pass = sn == cl;
      std::snprintf(buf, sizeof(buf), "parity: snii=%llu == clucene=%llu",
                    static_cast<unsigned long long>(sn),
                    static_cast<unsigned long long>(cl));
      break;
    }
    default: {
      v.pass = false;
      std::snprintf(buf, sizeof(buf), "unknown GateCmp");
      break;
    }
  }
  v.reason = buf;
  return v;
}

}  // namespace bench
