#include "oss_gate.h"

#include <algorithm>

namespace bench {

double median_ms(std::vector<double> samples) {
  if (samples.empty()) return 0.0;
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

namespace {

// CLucene-over-SNII wall ratio with the same div-by-zero guard as main.cpp
// ratio(): when the SNII median is 0 the ratio is 1.0 if the CLucene median is
// also 0 (no signal either side -> no false win), else the CLucene median itself.
double wall_ratio(double cl_median, double sn_median) {
  if (sn_median == 0.0) return cl_median == 0.0 ? 1.0 : cl_median;
  return cl_median / sn_median;
}

}  // namespace

OssVerdict evaluate_oss(const std::vector<double>& cl_wall,
                        const std::vector<double>& sn_wall,
                        const snii::io::IoMetrics& cl,
                        const snii::io::IoMetrics& sn, bool docids_match,
                        double min_wall_ratio) {
  OssVerdict v;
  const double cl_med = median_ms(cl_wall);
  const double sn_med = median_ms(sn_wall);

  v.min_wall_ratio = min_wall_ratio;
  v.wall_ratio_cl_over_snii = wall_ratio(cl_med, sn_med);
  // Inclusive-with-epsilon floor test: an exact-floor match is a PASS (no
  // off-by-one FAIL), mirroring the kLeRatio epsilon of metric_gate.cpp.
  v.wall_pass = v.wall_ratio_cl_over_snii >= min_wall_ratio - 1e-9;
  // Structural axis: never noise-relaxed. SNII must not issue MORE serial rounds
  // than CLucene (parity-or-better).
  v.rounds_pass = sn.serial_rounds <= cl.serial_rounds;
  // Correctness gate: vetoes the verdict regardless of latency.
  v.docids_pass = docids_match;
  v.overall_pass = v.wall_pass && v.rounds_pass && v.docids_pass;
  return v;
}

}  // namespace bench
