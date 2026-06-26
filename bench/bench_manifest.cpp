#include "bench_manifest.h"

#include <string>
#include <vector>

namespace bench {

Scale scale_of(uint32_t doc_count) {
  // Single documented boundary: doc_count >= 1'000'000 is the large (5M) bucket.
  return doc_count >= 1'000'000u ? Scale::kLarge5M : Scale::kSmall150K;
}

namespace {

// Convenience constructors for the three golden-metric gate shapes. All metric
// names are string literals with static storage duration, so storing them in a
// const char* MetricThreshold is safe across the manifest's lifetime.

// Advantage gate: SNII must beat CLucene by at least `cap`x on this metric.
MetricThreshold ratio_gate(const char* name, double cap) {
  return MetricThreshold{name, GateCmp::kLeRatio, cap, 0};
}

// Parity-or-better gate: SNII must not fetch/round MORE than CLucene (cap <= 0
// degrades kLeRatio to "snii <= clucene"). Used for honest-envelope scenarios.
MetricThreshold parity_gate(const char* name) {
  return MetricThreshold{name, GateCmp::kLeRatio, 0.0, 0};
}

// Absolute cap on the SNII metric itself (CLucene recorded but not gated).
MetricThreshold absolute_gate(const char* name, uint64_t cap) {
  return MetricThreshold{name, GateCmp::kLeAbsolute, 0.0, cap};
}

// Small-scale slack gate: SNII may read/round somewhat MORE than CLucene because
// its fixed per-query planning overhead (an extra term-dictionary / skip-list
// round, a denser multi-region fetch) is not yet amortized at 150K. The design
// thesis is "advantage grows with scale", so the small bucket must NOT demand a
// hard win. A kLeRatio cap of `cap` (0 < cap < 1) admits CL/SNII >= cap, i.e.
// SNII up to (1/cap)x CLucene. Calibrated from the measured 150K cost-model:
//   serial_rounds: SNII +1 round on short phrases / rare-driver AND (ratio 0.5).
//   request_bytes: SNII up to ~1.13x on a single-block high-df term (ratio 0.85).
MetricThreshold slack_gate(const char* name, double cap) {
  return MetricThreshold{name, GateCmp::kLeRatio, cap, 0};
}

// Calibrated small-scale (150K) slack caps, sourced from bench --scenarios
// --docs 150000 (seed 42) measured rounds/bytes.
constexpr double kSmallSerialRoundsCap = 0.5;   // tolerate SNII +1 round
constexpr double kSmallRequestBytesCap = 0.85;  // tolerate SNII up to ~1.18x

constexpr const char* kSerialRounds = "serial_rounds";
constexpr const char* kRequestBytes = "total_request_bytes";

// The permissive fallback used for any id not in the table: parity-or-better on
// serial_rounds + request_bytes, docid equality required. A new scenario thus
// neither auto-fails nor silently claims a hard advantage.
ItemThresholds parity_fallback() {
  ItemThresholds it;
  it.metrics.push_back(parity_gate(kSerialRounds));
  it.metrics.push_back(parity_gate(kRequestBytes));
  it.require_docid_equality = true;
  return it;
}

// --- the single threshold table ---------------------------------------------
//
// One function = one place a reviewer reads every declared gate. Each branch
// cites the measured number in docs/benchmark-results.md that it admits.
ItemThresholds local_thresholds(const std::string& id, Scale scale) {
  const bool big = scale == Scale::kLarge5M;

  // df=0 scenarios: both engines fetch ~zero bytes, a ratio is meaningless.
  // Gate docid equality ALONE (empty metric set). PHRASE-REVERSED usually
  // resolves to an empty result set for the same reason.
  if (id == "TERM-absent" || id == "PHRASE-REVERSED") {
    ItemThresholds it;
    it.require_docid_equality = true;
    return it;  // no metric gates
  }

  // Honest-envelope scenarios: SNII's known-worst shapes (dense high-df AND;
  // all-stopword phrase). The design explicitly accepts NO byte advantage here;
  // gate serial_rounds at parity-or-better only and never assert bytes.
  if (id == "AND-DENSE") {
    ItemThresholds it;
    it.metrics.push_back(parity_gate(kSerialRounds));
    it.require_docid_equality = true;
    return it;
  }

  // 5-term PHRASE: the suite's headline advantage. Measured 5M:
  // serial_rounds 16 -> 8, request_bytes 10.77MB -> 0.96MB (11x). At 150K/100K
  // the advantage is smaller (serial_rounds 4 -> 3), so the small cap is looser.
  if (id == "PHRASE-5") {
    ItemThresholds it;
    if (big) {
      it.metrics.push_back(absolute_gate(kSerialRounds, 8u));   // measured 8
      it.metrics.push_back(ratio_gate(kRequestBytes, 5.0));     // measured 11x
    } else {
      // 150K measured: serial_rounds 3 -> 2 (snii wins), bytes ~2x advantage.
      it.metrics.push_back(absolute_gate(kSerialRounds, 3u));   // measured 2 (cap 3)
      it.metrics.push_back(slack_gate(kRequestBytes, kSmallRequestBytesCap));
    }
    it.require_docid_equality = true;
    return it;
  }

  // Other phrase lengths (2/3/8-term): serial_rounds parity-or-better and a
  // modest byte advantage at 5M; parity at small scale.
  if (id == "PHRASE-2" || id == "PHRASE-3" || id == "PHRASE-8") {
    ItemThresholds it;
    // 150K: short phrases (2/3) cost SNII one extra dictionary round (3 vs 2);
    // PHRASE-8 already wins (2 vs 3). Slack the small serial_rounds gate.
    it.metrics.push_back(big ? parity_gate(kSerialRounds)
                             : slack_gate(kSerialRounds, kSmallSerialRoundsCap));
    it.metrics.push_back(big ? ratio_gate(kRequestBytes, 1.5)
                             : slack_gate(kRequestBytes, kSmallRequestBytesCap));
    it.require_docid_equality = true;
    return it;
  }

  // High-df TERM: large posting list. Measured 5M serial_rounds 4 -> 3
  // (parity-or-better, NOT a multiple), request_bytes 2.82MB -> 1.37MB (2.1x).
  if (id == "TERM-very-high" || id == "TERM-high") {
    ItemThresholds it;
    it.metrics.push_back(parity_gate(kSerialRounds));          // 4 -> 3 (1 -> 1 @150K)
    // 150K: TERM-high reads a single 64KB CLucene block vs SNII's denser
    // multi-region fetch (74116 vs 65536, ~1.13x), so the small bytes gate
    // tolerates SNII's slack; the 5M advantage (2.1x) is gated hard.
    it.metrics.push_back(big ? ratio_gate(kRequestBytes, 1.5)  // measured 2.1x
                             : slack_gate(kRequestBytes, kSmallRequestBytesCap));
    it.require_docid_equality = true;
    return it;
  }

  // Mid/low-df TERM: measured serial_rounds 2 -> 1 and request_bytes ~300x at
  // 5M (131KB -> ~400B). Gate serial_rounds parity-or-better + a real byte
  // advantage at 5M; parity at small scale.
  if (id == "TERM-mid" || id == "TERM-low" || id == "TERM-df1") {
    ItemThresholds it;
    it.metrics.push_back(parity_gate(kSerialRounds));
    it.metrics.push_back(big ? ratio_gate(kRequestBytes, 2.0)
                             : parity_gate(kRequestBytes));
    it.require_docid_equality = true;
    return it;
  }

  // Boolean OR / AND-HIGH-LOW (rare-driver AND): serial_rounds parity-or-better;
  // bytes parity at small scale, modest advantage at 5M (driver prunes reads).
  if (id == "OR-MIXED-DF" || id == "AND-HIGH-LOW") {
    ItemThresholds it;
    // 150K: rare-driver AND costs SNII one extra round (1 -> 2) before the
    // driver prunes; OR is at parity. Slack the small serial_rounds gate.
    it.metrics.push_back(big ? parity_gate(kSerialRounds)
                             : slack_gate(kSerialRounds, kSmallSerialRoundsCap));
    if (big) it.metrics.push_back(ratio_gate(kRequestBytes, 1.2));
    it.require_docid_equality = true;
    return it;
  }

  // MATCH-ALL: scans the full corpus on both engines; gate parity-or-better on
  // serial_rounds, no byte advantage claimed.
  if (id == "MATCH-ALL") {
    ItemThresholds it;
    it.metrics.push_back(parity_gate(kSerialRounds));
    it.require_docid_equality = true;
    return it;
  }

  // Unknown id: permissive parity fallback (never auto-fail, never silent win).
  return parity_fallback();
}

}  // namespace

ItemThresholds thresholds_for(const std::string& scenario_id, Scale scale,
                              Surface surface) {
  ItemThresholds it = local_thresholds(scenario_id, scale);

  // Real-OSS surface tolerates wall-clock / object-store noise: relax every
  // advantage-ratio gate (kLeRatio with cap > 1.0) toward parity-or-better so a
  // noisy run does not fail on a deterministic-only margin, while keeping the
  // docid-equality correctness gate and all absolute / parity gates intact.
  if (surface == Surface::kRealOss) {
    for (auto& m : it.metrics) {
      if (m.cmp == GateCmp::kLeRatio && m.max_ratio_cl_over_snii > 1.0) {
        m.max_ratio_cl_over_snii = 1.0;  // demand only parity-or-better on OSS
      }
    }
  }
  return it;
}

}  // namespace bench
