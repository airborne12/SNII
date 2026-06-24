// Unit tests for the declarative threshold manifest (bench::thresholds_for).
//
// BENCH.2: pure data + lookup, no I/O, no build, no CLucene. Every benchmark
// item (scenario_id x scale x surface) resolves to a documented set of
// MetricThreshold + a docid-equality requirement. These tests assert:
//   - scale bucketing (150K vs 5M) at the 1M doc_count boundary;
//   - PHRASE advantage tightens with scale (5M request_bytes cap >= ratio of
//     the 150K cap -- the "advantage grows with scale" invariant);
//   - high-df TERM serial_rounds gate is parity-or-better (snii <= clucene);
//   - honest-envelope scenarios (AND-DENSE, all-stopword) gate parity ONLY,
//     never a byte advantage;
//   - absent-term / reversed-phrase scenarios gate docid-equality ONLY (empty
//     metric set -- a 0/0 ratio is meaningless);
//   - an unknown scenario id falls back to a permissive parity gate that still
//     requires docid equality (a new scenario never auto-fails nor silently wins).
//
// Threshold numbers are sourced from docs/benchmark-results.md (the measured
// 5M cost-model + real-OSS results) and the SNII status memo.

#include "bench_manifest.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "metric_gate.h"

namespace {

// Find the threshold for a named golden metric in an ItemThresholds, or nullptr.
const bench::MetricThreshold* find_metric(const bench::ItemThresholds& it,
                                          const std::string& name) {
  for (const auto& m : it.metrics) {
    if (name == m.metric_name) return &m;
  }
  return nullptr;
}

constexpr bench::Surface kLocal = bench::Surface::kLocalCostModel;

// --- scale bucketing -------------------------------------------------------

TEST(Manifest, ScaleBucketBoundary) {
  // doc_count < 1'000'000 is the small (150K) bucket; >= 1'000'000 is large.
  EXPECT_EQ(bench::Scale::kSmall150K, bench::scale_of(999999u));
  EXPECT_EQ(bench::Scale::kLarge5M, bench::scale_of(1000000u));
}

TEST(Manifest, ScaleBucketExtremes) {
  EXPECT_EQ(bench::Scale::kSmall150K, bench::scale_of(0u));
  EXPECT_EQ(bench::Scale::kSmall150K, bench::scale_of(150000u));
  EXPECT_EQ(bench::Scale::kLarge5M, bench::scale_of(5000000u));
}

// --- PHRASE advantage grows with scale -------------------------------------

TEST(Manifest, Phrase5MTighterThan150K) {
  // Same scenario id, two scales: the 5M request_bytes advantage cap must be
  // at least as strong as the 150K one (advantage grows with scale).
  auto small = bench::thresholds_for("PHRASE-5", bench::Scale::kSmall150K, kLocal);
  auto large = bench::thresholds_for("PHRASE-5", bench::Scale::kLarge5M, kLocal);
  const auto* sb = find_metric(small, "total_request_bytes");
  const auto* lb = find_metric(large, "total_request_bytes");
  ASSERT_NE(nullptr, sb);
  ASSERT_NE(nullptr, lb);
  EXPECT_EQ(bench::GateCmp::kLeRatio, sb->cmp);
  EXPECT_EQ(bench::GateCmp::kLeRatio, lb->cmp);
  // Larger scale demands an equal-or-greater advantage ratio.
  EXPECT_GE(lb->max_ratio_cl_over_snii, sb->max_ratio_cl_over_snii);
}

TEST(Manifest, Phrase5MSerialRoundsAbsoluteCap) {
  // The 5M 5-term phrase head serial_rounds is documented at SNII=8 (vs CL 16);
  // the manifest must carry an absolute serial_rounds cap for it.
  auto large = bench::thresholds_for("PHRASE-5", bench::Scale::kLarge5M, kLocal);
  const auto* sr = find_metric(large, "serial_rounds");
  ASSERT_NE(nullptr, sr);
  EXPECT_EQ(bench::GateCmp::kLeAbsolute, sr->cmp);
  EXPECT_GE(sr->max_absolute, 8u);  // measured SNII value; cap must admit it
}

// --- TERM high-df: parity-or-better serial_rounds --------------------------

TEST(Manifest, TermHighDfSerialRoundsParityOrBetter) {
  auto it = bench::thresholds_for("TERM-high", bench::Scale::kLarge5M, kLocal);
  const auto* sr = find_metric(it, "serial_rounds");
  ASSERT_NE(nullptr, sr);
  // Parity-or-better: a kLeRatio gate with cap <= 1.0 means snii <= clucene.
  EXPECT_EQ(bench::GateCmp::kLeRatio, sr->cmp);
  EXPECT_LE(sr->max_ratio_cl_over_snii, 1.0);
  EXPECT_TRUE(it.require_docid_equality);
}

TEST(Manifest, TermHighDfRequestBytesAdvantage) {
  // 5M high-df term request_bytes is documented at 2.1x (2.82MB -> 1.37MB).
  auto it = bench::thresholds_for("TERM-high", bench::Scale::kLarge5M, kLocal);
  const auto* rb = find_metric(it, "total_request_bytes");
  ASSERT_NE(nullptr, rb);
  EXPECT_EQ(bench::GateCmp::kLeRatio, rb->cmp);
  EXPECT_GT(rb->max_ratio_cl_over_snii, 1.0);  // a real advantage, not parity
}

// --- honest-envelope scenarios: parity ONLY --------------------------------

TEST(Manifest, HonestEnvelopeIsParityGate) {
  // AND-DENSE: SNII's known-worst AND (two high-df terms). The manifest must
  // gate serial_rounds at parity-or-better and must NOT claim a byte advantage.
  auto it = bench::thresholds_for("AND-DENSE", bench::Scale::kLarge5M, kLocal);
  const auto* sr = find_metric(it, "serial_rounds");
  ASSERT_NE(nullptr, sr);
  EXPECT_EQ(bench::GateCmp::kLeRatio, sr->cmp);
  EXPECT_LE(sr->max_ratio_cl_over_snii, 1.0);  // parity-or-better only
  // No byte-advantage gate: either absent, or present only as parity-or-better.
  const auto* rb = find_metric(it, "total_request_bytes");
  if (rb != nullptr) {
    EXPECT_EQ(bench::GateCmp::kLeRatio, rb->cmp);
    EXPECT_LE(rb->max_ratio_cl_over_snii, 1.0);
  }
  EXPECT_TRUE(it.require_docid_equality);
}

// --- absent-term / reversed-phrase: docid-equality ONLY --------------------

TEST(Manifest, AbsentTermOnlyDocidEqualityNoMetricGate) {
  // TERM-absent (df=0): both engines fetch ~zero bytes, a ratio is meaningless.
  auto it = bench::thresholds_for("TERM-absent", bench::Scale::kLarge5M, kLocal);
  EXPECT_TRUE(it.metrics.empty());
  EXPECT_TRUE(it.require_docid_equality);
}

TEST(Manifest, ReversedPhraseOnlyDocidEquality) {
  auto it =
      bench::thresholds_for("PHRASE-REVERSED", bench::Scale::kLarge5M, kLocal);
  EXPECT_TRUE(it.metrics.empty());
  EXPECT_TRUE(it.require_docid_equality);
}

// --- unknown id: permissive parity fallback --------------------------------

TEST(Manifest, UnknownIdFallsBackToParity) {
  auto it = bench::thresholds_for("NEVER-SEEN", bench::Scale::kLarge5M, kLocal);
  // A new scenario must never auto-fail nor silently win: parity-or-better on
  // the golden metrics it does gate, and docid equality required.
  EXPECT_TRUE(it.require_docid_equality);
  for (const auto& m : it.metrics) {
    if (m.cmp == bench::GateCmp::kLeRatio) {
      EXPECT_LE(m.max_ratio_cl_over_snii, 1.0)
          << "unknown-id fallback must not assert a hard advantage";
    }
  }
}

TEST(Manifest, EmptyIdStringFallsBackWithoutThrow) {
  bench::ItemThresholds it;
  ASSERT_NO_THROW(
      it = bench::thresholds_for("", bench::Scale::kLarge5M, kLocal));
  EXPECT_TRUE(it.require_docid_equality);
}

// --- surface distinction ---------------------------------------------------

TEST(Manifest, OssSurfaceDistinctFromLocalNoCrash) {
  // The real-OSS surface tolerates wall-clock noise: it must resolve (possibly
  // looser) thresholds without crashing.
  bench::ItemThresholds it;
  ASSERT_NO_THROW(it = bench::thresholds_for("PHRASE-5", bench::Scale::kLarge5M,
                                             bench::Surface::kRealOss));
  // Whatever it returns, docid equality remains a hard correctness gate.
  EXPECT_TRUE(it.require_docid_equality);
}

}  // namespace
