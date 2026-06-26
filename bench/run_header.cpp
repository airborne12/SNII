#include "run_header.h"

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>

#include "bench_jsonl.h"   // json_escape (BENCH.3) -- shared string escaper
#include "bench_manifest.h"

namespace bench {

namespace {

// FNV-1a 64-bit over a string. Used to fingerprint the manifest canonical form.
uint64_t fnv1a(const std::string& s) {
  uint64_t h = 1469598103934665603ull;  // FNV offset basis
  for (unsigned char c : s) {
    h ^= static_cast<uint64_t>(c);
    h *= 1099511628211ull;  // FNV prime
  }
  return h;
}

// Render a 64-bit value as fixed-width lowercase hex (stable, no locale deps).
std::string hex16(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return std::string(buf);
}

const char* cmp_name(GateCmp c) {
  switch (c) {
    case GateCmp::kLeRatio:
      return "le_ratio";
    case GateCmp::kLeAbsolute:
      return "le_absolute";
    case GateCmp::kEqual:
      return "equal";
  }
  return "?";
}

// Append one item's declared thresholds to a canonical (order-stable) string.
void append_item(std::ostringstream& os, const char* id, const char* scale_tag,
                 const char* surface_tag, const ItemThresholds& it) {
  os << id << '|' << scale_tag << '|' << surface_tag
     << "|doceq=" << (it.require_docid_equality ? 1 : 0);
  for (const MetricThreshold& m : it.metrics) {
    os << "|m=" << (m.metric_name ? m.metric_name : "") << ',' << cmp_name(m.cmp)
       << ',' << m.max_ratio_cl_over_snii << ',' << m.max_absolute;
  }
  os << '\n';
}

}  // namespace

void write_run_header(std::ostream& os, const RunHeader& h) {
  os << '{';
  os << "\"record\":\"" << json_escape(h.record) << "\",";
  os << "\"git_rev\":\"" << json_escape(h.git_rev) << "\",";
  os << "\"seed\":" << h.seed << ',';

  os << "\"scales\":[";
  for (size_t i = 0; i < h.scales.size(); ++i) {
    if (i) os << ',';
    os << h.scales[i];
  }
  os << "],";

  os << "\"surfaces\":[";
  for (size_t i = 0; i < h.surfaces.size(); ++i) {
    if (i) os << ',';
    os << '"' << json_escape(h.surfaces[i]) << '"';
  }
  os << "],";

  os << "\"harness_version\":\"" << json_escape(h.harness_version) << "\",";
  os << "\"manifest_hash\":\"" << json_escape(h.manifest_hash) << '"';
  os << "}\n";
}

std::string manifest_hash() {
  // The full declared catalog: every scenario id the manifest gives an explicit
  // threshold to (any id not listed resolves to the same permissive fallback, so
  // listing the explicit ones fully captures the gate surface). Order is fixed so
  // the canonical form -- and thus the hash -- is deterministic.
  static const char* const kIds[] = {
      "TERM-absent",   "PHRASE-REVERSED", "AND-DENSE",   "PHRASE-5",
      "PHRASE-2",      "PHRASE-3",        "PHRASE-8",    "TERM-very-high",
      "TERM-high",     "TERM-mid",        "TERM-low",    "TERM-df1",
      "OR-MIXED-DF",   "AND-HIGH-LOW",    "MATCH-ALL",   "__fallback__",
  };
  struct ScaleTag {
    Scale scale;
    const char* tag;
  };
  static const ScaleTag kScales[] = {{Scale::kSmall150K, "150K"},
                                     {Scale::kLarge5M, "5M"}};
  struct SurfaceTag {
    Surface surface;
    const char* tag;
  };
  static const SurfaceTag kSurfaces[] = {{Surface::kLocalCostModel, "local"},
                                         {Surface::kRealOss, "oss"}};

  std::ostringstream canon;
  canon << "snii-bench-manifest/v1\n";
  for (const char* id : kIds) {
    for (const ScaleTag& s : kScales) {
      for (const SurfaceTag& sf : kSurfaces) {
        const ItemThresholds it = thresholds_for(id, s.scale, sf.surface);
        append_item(canon, id, s.tag, sf.tag, it);
      }
    }
  }
  return hex16(fnv1a(canon.str()));
}

}  // namespace bench
