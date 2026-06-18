#include "snii/query/scoring_query.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <queue>
#include <unordered_map>
#include <vector>

#include "snii/common/slice.h"
#include "snii/encoding/byte_source.h"
#include "snii/format/dict_entry.h"
#include "snii/format/format_constants.h"
#include "snii/format/frq_pod.h"
#include "snii/format/frq_prelude.h"
#include "snii/io/batch_range_fetcher.h"

namespace snii::query {

using snii::format::DictEntry;
using snii::format::DictEntryEnc;
using snii::format::DictEntryKind;
using snii::format::FrqPreludeReader;
using snii::reader::LogicalIndexReader;

namespace {

// One scored posting for one term in one doc.
struct TermPosting {
  uint32_t docid = 0;
  double score = 0.0;
};

// One window's block-max upper bound and the docid range it covers. block_max is
// true when max_score came from the frq_prelude columns (vs the exact-score
// fallback); both are valid upper bounds, so it is informational only.
struct WindowBound {
  uint32_t first_docid = 0;  // inclusive
  uint32_t last_docid = 0;   // inclusive
  double max_score = 0.0;    // block-max upper bound for any doc in this window
  bool block_max = false;
};

// All scored postings of one query term plus its block-max metadata.
struct TermCursor {
  std::vector<TermPosting> postings;  // ascending docid, exact per-doc scores
  std::vector<WindowBound> windows;   // ascending, covering all postings
  size_t pos = 0;                     // DAAT cursor into postings
};

uint32_t CurrentDoc(const TermCursor& c) {
  return c.pos < c.postings.size() ? c.postings[c.pos].docid
                                   : std::numeric_limits<uint32_t>::max();
}

// Reads one .frq window's bytes (prelude stripped) for a pod_ref/inline entry.
Status FetchWindowBytes(const LogicalIndexReader& idx, const DictEntry& entry,
                        uint64_t frq_base, std::vector<uint8_t>* window_owned,
                        Slice* window) {
  if (entry.kind == DictEntryKind::kInline) {
    *window = Slice(entry.frq_bytes);
    return Status::OK();
  }
  uint64_t win_abs = 0;
  uint64_t win_len = 0;
  SNII_RETURN_IF_ERROR(idx.resolve_frq_window(entry, frq_base, &win_abs, &win_len));
  snii::io::BatchRangeFetcher fetcher(idx.reader());
  const size_t h = fetcher.add(win_abs, static_cast<size_t>(win_len));
  SNII_RETURN_IF_ERROR(fetcher.fetch());
  Slice got = fetcher.get(h);
  window_owned->assign(got.data(), got.data() + got.size());
  *window = Slice(*window_owned);
  return Status::OK();
}

// Reads a windowed entry's frq_prelude (block-max columns live here).
Status FetchPrelude(const LogicalIndexReader& idx, const DictEntry& entry,
                    uint64_t frq_base, FrqPreludeReader* out) {
  const auto& frq = idx.section_refs().frq_pod;
  const uint64_t prelude_abs = frq.offset + frq_base + entry.frq_off_delta;
  snii::io::BatchRangeFetcher fetcher(idx.reader());
  const size_t h =
      fetcher.add(prelude_abs, static_cast<size_t>(entry.prelude_len));
  SNII_RETURN_IF_ERROR(fetcher.fetch());
  return FrqPreludeReader::open(fetcher.get(h), out);
}

// Builds per-window block-max bounds from a windowed entry's prelude. Falls back
// to block_max=false windows when the prelude is unusable.
void BuildWindowBounds(const FrqPreludeReader& prelude, const ScorerContext& ctx,
                       double avgdl, const Bm25Params& params,
                       const std::vector<TermPosting>& postings,
                       std::vector<WindowBound>* windows) {
  const uint32_t n = prelude.n_windows();
  size_t idx = 0;
  for (uint32_t w = 0; w < n; ++w) {
    const uint32_t len = prelude.frq_window_len(w);
    if (len == 0 || idx >= postings.size()) continue;
    const size_t end = std::min(postings.size(), idx + len);
    WindowBound wb;
    wb.first_docid = postings[idx].docid;
    wb.last_docid = postings[end - 1].docid;
    wb.max_score = ctx.max_score(prelude.max_freq(w), prelude.max_norm(w), avgdl,
                                 params);
    wb.block_max = true;
    windows->push_back(wb);
    idx = end;
  }
}

// Fallback single window covering all postings, bounded by the exact max score
// (always a valid upper bound, so pruning stays correct).
void SingleWindowFallback(const std::vector<TermPosting>& postings,
                          std::vector<WindowBound>* windows) {
  if (postings.empty()) return;
  WindowBound wb;
  wb.first_docid = postings.front().docid;
  wb.last_docid = postings.back().docid;
  wb.block_max = false;
  for (const auto& p : postings) wb.max_score = std::max(wb.max_score, p.score);
  windows->push_back(wb);
}

// Decodes (docid, freq) postings and computes exact per-doc BM25 scores.
Status ScorePostings(const snii::stats::SniiStatsProvider& stats,
                     const ScorerContext& ctx, const Bm25Params& params,
                     Slice window, std::vector<TermPosting>* out) {
  ByteSource src(window);
  std::vector<uint32_t> docids;
  std::vector<uint32_t> freqs;
  SNII_RETURN_IF_ERROR(
      snii::format::read_frq_window(&src, /*win_base=*/0, &docids, &freqs));
  const double avgdl = stats.avgdl();
  out->reserve(docids.size());
  for (size_t i = 0; i < docids.size(); ++i) {
    uint8_t norm = 0;
    SNII_RETURN_IF_ERROR(stats.encoded_norm(docids[i], &norm));
    const uint32_t tf = i < freqs.size() ? freqs[i] : 1;
    out->push_back({docids[i], ctx.score(tf, norm, avgdl, params)});
  }
  return Status::OK();
}

// Builds the cursor for one term: postings with exact scores + window bounds.
Status BuildCursor(const LogicalIndexReader& idx,
                   const snii::stats::SniiStatsProvider& stats,
                   const std::string& term, const Bm25Params& params,
                   bool* found, TermCursor* cursor) {
  DictEntry entry;
  uint64_t frq_base = 0;
  uint64_t prx_base = 0;
  SNII_RETURN_IF_ERROR(idx.lookup(term, found, &entry, &frq_base, &prx_base));
  if (!*found) return Status::OK();

  const ScorerContext ctx =
      ScorerContext::make(stats.indexed_doc_count(), entry.df);

  std::vector<uint8_t> owned;
  Slice window;
  SNII_RETURN_IF_ERROR(FetchWindowBytes(idx, entry, frq_base, &owned, &window));
  SNII_RETURN_IF_ERROR(
      ScorePostings(stats, ctx, params, window, &cursor->postings));

  const bool windowed = entry.kind == DictEntryKind::kPodRef &&
                        entry.enc == DictEntryEnc::kWindowed &&
                        entry.prelude_len > 0;
  if (windowed) {
    FrqPreludeReader prelude;
    if (FetchPrelude(idx, entry, frq_base, &prelude).ok()) {
      BuildWindowBounds(prelude, ctx, stats.avgdl(), params, cursor->postings,
                        &cursor->windows);
    }
  }
  if (cursor->windows.empty()) {
    SingleWindowFallback(cursor->postings, &cursor->windows);
  }
  return Status::OK();
}

// Block-max upper bound for a term at a given docid: the max_score of the window
// covering docid (windows are ascending and contiguous). Beyond the last window
// the bound is 0 (the term cannot contribute).
double TermBoundAt(const TermCursor& c, uint32_t docid) {
  // Windows are ascending and contiguous; the first window whose last_docid is
  // >= docid covers it. Its block-max is a valid upper bound for any contained
  // doc, so it also bounds gaps between windows.
  for (const auto& w : c.windows) {
    if (docid <= w.last_docid) return w.max_score;
  }
  return 0.0;
}

// Min-heap keyed on score (smallest at top) maintaining the top-K.
struct TopK {
  explicit TopK(uint32_t k) : k_(k) {}
  void offer(uint32_t docid, double score) {
    if (heap_.size() < k_) {
      heap_.push({score, docid});
      return;
    }
    if (heap_.empty()) return;
    const Entry& worst = heap_.top();  // lowest score; ties: largest docid
    const bool better = score > worst.first ||
                        (score == worst.first && docid < worst.second);
    if (better) {
      heap_.pop();
      heap_.push({score, docid});
    }
  }
  double threshold() const {
    return heap_.size() < k_ ? -1.0 : heap_.top().first;
  }

  using Entry = std::pair<double, uint32_t>;
  struct Cmp {
    bool operator()(const Entry& a, const Entry& b) const {
      if (a.first != b.first) return a.first > b.first;  // min-score at top
      return a.second < b.second;  // for ties, largest docid at top (evictable)
    }
  };
  uint32_t k_;
  std::priority_queue<Entry, std::vector<Entry>, Cmp> heap_;
};

void DrainSorted(TopK* topk, std::vector<ScoredDoc>* out) {
  std::vector<ScoredDoc> all;
  while (!topk->heap_.empty()) {
    all.push_back({topk->heap_.top().second, topk->heap_.top().first});
    topk->heap_.pop();
  }
  std::sort(all.begin(), all.end(), [](const ScoredDoc& a, const ScoredDoc& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.docid < b.docid;
  });
  *out = std::move(all);
}

Status BuildCursors(const LogicalIndexReader& idx,
                    const snii::stats::SniiStatsProvider& stats,
                    const std::vector<std::string>& terms,
                    const Bm25Params& params, std::vector<TermCursor>* cursors) {
  for (const auto& term : terms) {
    bool found = false;
    TermCursor c;
    SNII_RETURN_IF_ERROR(BuildCursor(idx, stats, term, params, &found, &c));
    if (found && !c.postings.empty()) cursors->push_back(std::move(c));
  }
  return Status::OK();
}

}  // namespace

Status scoring_query_exhaustive(const LogicalIndexReader& idx,
                                const snii::stats::SniiStatsProvider& stats,
                                const std::vector<std::string>& terms, uint32_t k,
                                const Bm25Params& params,
                                std::vector<ScoredDoc>* out) {
  if (out == nullptr) return Status::InvalidArgument("scoring_query: null out");
  out->clear();
  if (k == 0) return Status::OK();

  std::vector<TermCursor> cursors;
  SNII_RETURN_IF_ERROR(BuildCursors(idx, stats, terms, params, &cursors));

  std::unordered_map<uint32_t, double> scores;
  for (const auto& c : cursors)
    for (const auto& p : c.postings) scores[p.docid] += p.score;

  std::vector<ScoredDoc> all;
  all.reserve(scores.size());
  for (const auto& [docid, score] : scores) all.push_back({docid, score});
  std::sort(all.begin(), all.end(), [](const ScoredDoc& a, const ScoredDoc& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.docid < b.docid;
  });
  if (all.size() > k) all.resize(k);
  *out = std::move(all);
  return Status::OK();
}

Status scoring_query_wand(const LogicalIndexReader& idx,
                          const snii::stats::SniiStatsProvider& stats,
                          const std::vector<std::string>& terms, uint32_t k,
                          const Bm25Params& params, std::vector<ScoredDoc>* out) {
  if (out == nullptr) return Status::InvalidArgument("scoring_query: null out");
  out->clear();
  if (k == 0) return Status::OK();

  std::vector<TermCursor> cursors;
  SNII_RETURN_IF_ERROR(BuildCursors(idx, stats, terms, params, &cursors));

  TopK topk(k);
  // Document-at-a-time WAND with block-max bounds.
  while (true) {
    // Sort cursors by current docid (ascending; exhausted cursors sink).
    std::sort(cursors.begin(), cursors.end(),
              [](const TermCursor& a, const TermCursor& b) {
                return CurrentDoc(a) < CurrentDoc(b);
              });
    if (cursors.empty() || CurrentDoc(cursors.front()) ==
                               std::numeric_limits<uint32_t>::max()) {
      break;
    }

    const double theta = topk.threshold();
    // Accumulate block-max upper bounds in docid order to find the pivot term.
    double bound = 0.0;
    size_t pivot = 0;
    bool found_pivot = false;
    for (size_t i = 0; i < cursors.size(); ++i) {
      const uint32_t d = CurrentDoc(cursors[i]);
      if (d == std::numeric_limits<uint32_t>::max()) break;
      bound += TermBoundAt(cursors[i], d);
      // Use >= (not >) so a doc whose upper bound only TIES the K-th threshold is
      // still explored and exact-scored: under the (score desc, docid asc) total
      // order a tie can still evict the current K-th entry (smaller docid wins),
      // exactly as the exhaustive path would. Strict > would wrongly prune ties.
      if (bound >= theta) {
        pivot = i;
        found_pivot = true;
        break;
      }
    }
    if (!found_pivot) break;  // no doc can beat the threshold anymore.

    const uint32_t pivot_doc = CurrentDoc(cursors[pivot]);
    if (CurrentDoc(cursors.front()) == pivot_doc) {
      // All cursors at the pivot doc are aligned: score it exactly.
      double doc_score = 0.0;
      for (auto& c : cursors) {
        if (CurrentDoc(c) == pivot_doc) {
          doc_score += c.postings[c.pos].score;
          ++c.pos;
        }
      }
      topk.offer(pivot_doc, doc_score);
    } else {
      // Advance a lagging cursor toward pivot_doc (skip docs it cannot win on).
      for (auto& c : cursors) {
        if (CurrentDoc(c) < pivot_doc) {
          while (c.pos < c.postings.size() &&
                 c.postings[c.pos].docid < pivot_doc) {
            ++c.pos;
          }
          break;
        }
      }
    }
  }
  DrainSorted(&topk, out);
  return Status::OK();
}

}  // namespace snii::query
