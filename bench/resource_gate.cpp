#include "resource_gate.h"

#include <cinttypes>
#include <cstdio>

#include "bench_jsonl.h"  // json_escape (shared escaping convention)

namespace bench {

namespace {

// Inclusive-with-epsilon cap test: pass iff ratio <= cap + 1e-9 so an exact-cap
// match is a PASS (no off-by-one FAIL). Mirrors the kLeRatio epsilon convention
// of metric_gate.cpp.
bool le_cap(double ratio, double cap) { return ratio <= cap + 1e-9; }

// One axis: compute the CLucene-relative ratio (sn/cl) and its PASS flag.
//
//   cl == 0 && sn == 0 -> ratio 1.0, PASS (empty == empty is parity)
//   cl == 0 && sn  > 0 -> ratio = sn (div-by-zero guard), FAIL: a non-zero SNII
//                         cost can never be "<= cap x of nothing"
//   cl  > 0            -> ratio = sn / cl, PASS iff ratio <= cap (+eps)
struct AxisResult {
  double ratio;
  bool pass;
};

AxisResult gate_axis(double sn, double cl, double cap) {
  if (cl == 0.0) {
    if (sn == 0.0) return {1.0, true};  // empty == empty
    return {sn, false};                  // non-zero over a zero baseline -> FAIL
  }
  const double ratio = sn / cl;
  return {ratio, le_cap(ratio, cap)};
}

}  // namespace

ResourceVerdict evaluate_resources(const ResourceTrio& clucene,
                                   const ResourceTrio& snii, double cpu_cap,
                                   double rss_cap, double disk_cap) {
  ResourceVerdict v;

  const AxisResult cpu = gate_axis(snii.cpu_s, clucene.cpu_s, cpu_cap);
  const AxisResult rss = gate_axis(snii.peak_rss_mib, clucene.peak_rss_mib, rss_cap);
  const AxisResult disk = gate_axis(static_cast<double>(snii.index_bytes),
                                    static_cast<double>(clucene.index_bytes),
                                    disk_cap);

  v.cpu_ratio = cpu.ratio;
  v.rss_ratio = rss.ratio;
  v.disk_ratio = disk.ratio;
  v.cpu_pass = cpu.pass;
  v.rss_pass = rss.pass;
  v.disk_pass = disk.pass;
  v.overall_pass = cpu.pass && rss.pass && disk.pass;
  return v;
}

namespace {

void key_str(std::ostream& os, const char* key, const std::string& value) {
  os << '"' << key << "\":\"" << json_escape(value) << '"';
}
void key_u64(std::ostream& os, const char* key, uint64_t value) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%" PRIu64, value);
  os << '"' << key << "\":" << buf;
}
void key_bool(std::ostream& os, const char* key, bool value) {
  os << '"' << key << "\":" << (value ? "true" : "false");
}
// Doubles (cpu/rss/ratio/cap) with fixed %.6g: human-readable, never sci-notation
// for the magnitudes here, and distinct from the integer index_bytes a CI diff
// compares byte-for-byte.
void key_dbl(std::ostream& os, const char* key, double value) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.6g", value);
  os << '"' << key << "\":" << buf;
}

}  // namespace

void write_resource_jsonl(std::ostream& os, const ResourceRow& row) {
  os << '{';
  key_str(os, "metric_kind", std::string("resource"));
  os << ',';
  key_str(os, "surface", row.surface);
  os << ',';
  key_u64(os, "doc_count", static_cast<uint64_t>(row.doc_count));
  os << ',';
  key_u64(os, "seed", row.seed);
  os << ',';
  key_u64(os, "spill_mib", static_cast<uint64_t>(row.spill_mib));
  os << ',';
  key_str(os, "git_rev", row.git_rev);
  os << ',';
  // CLucene control trio.
  os << "\"clucene\":{";
  key_dbl(os, "cpu_s", row.clucene.cpu_s);
  os << ',';
  key_dbl(os, "peak_rss_mib", row.clucene.peak_rss_mib);
  os << ',';
  key_u64(os, "index_bytes", row.clucene.index_bytes);
  os << "},";
  // SNII engine trio.
  os << "\"snii\":{";
  key_dbl(os, "cpu_s", row.snii.cpu_s);
  os << ',';
  key_dbl(os, "peak_rss_mib", row.snii.peak_rss_mib);
  os << ',';
  key_u64(os, "index_bytes", row.snii.index_bytes);
  os << "},";
  // Caps (manifest thresholds; reproduction metadata).
  os << "\"caps\":{";
  key_dbl(os, "cpu", row.cpu_cap);
  os << ',';
  key_dbl(os, "rss", row.rss_cap);
  os << ',';
  key_dbl(os, "disk", row.disk_cap);
  os << "},";
  // Verdict: ratios + per-axis pass + overall.
  os << "\"verdict\":{";
  key_dbl(os, "cpu_ratio", row.verdict.cpu_ratio);
  os << ',';
  key_dbl(os, "rss_ratio", row.verdict.rss_ratio);
  os << ',';
  key_dbl(os, "disk_ratio", row.verdict.disk_ratio);
  os << ',';
  key_bool(os, "cpu_pass", row.verdict.cpu_pass);
  os << ',';
  key_bool(os, "rss_pass", row.verdict.rss_pass);
  os << ',';
  key_bool(os, "disk_pass", row.verdict.disk_pass);
  os << ',';
  key_bool(os, "overall_pass", row.verdict.overall_pass);
  os << '}';
  os << ',';
  key_bool(os, "overall_pass", row.verdict.overall_pass);
  os << "}\n";
}

}  // namespace bench
