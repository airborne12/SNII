#include "snii/query/phrase_query.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

#include "snii/common/slice.h"
#include "snii/encoding/byte_source.h"
#include "snii/format/dict_entry.h"
#include "snii/format/frq_pod.h"
#include "snii/format/prx_pod.h"
#include "snii/io/batch_range_fetcher.h"
#include "snii/reader/windowed_posting.h"

namespace snii::query {

using snii::format::DictEntry;
using snii::format::DictEntryEnc;
using snii::format::DictEntryKind;
using snii::reader::LogicalIndexReader;

namespace {

// Resolved per-term lookup result plus the planned (inline-or-batched) byte
// sources for its .frq and .prx windows.
struct TermPlan {
  DictEntry entry;
  uint64_t frq_base = 0;
  uint64_t prx_base = 0;
  // For slim pod_ref terms: batch handles into the shared fetcher. For inline
  // terms the bytes are taken from the DictEntry directly (handles unused).
  size_t frq_handle = 0;
  size_t prx_handle = 0;
  bool pod_ref = false;
  bool windowed = false;  // multi-window: decoded via the two-level prelude
};

// Decoded postings for one term: docid -> position list (the window's per-doc
// position lists, aligned by index with the frq window's docids).
struct TermPostings {
  std::vector<uint32_t> docids;
  std::vector<std::vector<uint32_t>> positions;  // positions[i] for docids[i]
};

// Computes the absolute .frq window byte range (prelude stripped) for a pod_ref,
// validated against the POD section.
Status FrqWindowRange(const LogicalIndexReader& idx, const TermPlan& p,
                      uint64_t* off, uint64_t* len) {
  return idx.resolve_frq_window(p.entry, p.frq_base, off, len);
}

// Computes the absolute .prx window byte range for a pod_ref, validated.
Status PrxWindowRange(const LogicalIndexReader& idx, const TermPlan& p,
                      uint64_t* off, uint64_t* len) {
  return idx.resolve_prx_window(p.entry, p.prx_base, off, len);
}

// Decodes a term's frq docids and prx positions from the given byte slices.
Status DecodePostings(Slice frq_window, Slice prx_window, TermPostings* out) {
  ByteSource fsrc(frq_window);
  SNII_RETURN_IF_ERROR(
      snii::format::read_frq_window_docs(&fsrc, /*win_base=*/0, &out->docids));
  ByteSource psrc(prx_window);
  SNII_RETURN_IF_ERROR(snii::format::read_prx_window(&psrc, &out->positions));
  if (out->positions.size() != out->docids.size()) {
    return Status::Corruption("phrase_query: prx/frq doc-count mismatch");
  }
  return Status::OK();
}

// Intersects the docid sets of all per-term postings (all are sorted ascending).
std::vector<uint32_t> IntersectDocids(const std::vector<TermPostings>& posts) {
  std::vector<uint32_t> result = posts[0].docids;
  for (size_t t = 1; t < posts.size(); ++t) {
    std::vector<uint32_t> next;
    std::set_intersection(result.begin(), result.end(),
                          posts[t].docids.begin(), posts[t].docids.end(),
                          std::back_inserter(next));
    result.swap(next);
    if (result.empty()) break;
  }
  return result;
}

// For a doc present in every term, returns its position list for term t.
const std::vector<uint32_t>& PositionsFor(const TermPostings& post,
                                          uint32_t docid) {
  // docids are sorted; binary search to find the aligned positions index.
  const auto it =
      std::lower_bound(post.docids.begin(), post.docids.end(), docid);
  const size_t i = static_cast<size_t>(it - post.docids.begin());
  return post.positions[i];
}

// Checks whether term[0]@p, term[1]@p+1, ... occur consecutively in docid.
bool PhraseInDoc(const std::vector<TermPostings>& posts, uint32_t docid) {
  const std::vector<uint32_t>& first = PositionsFor(posts[0], docid);
  for (uint32_t start : first) {
    bool ok = true;
    for (size_t t = 1; t < posts.size(); ++t) {
      const std::vector<uint32_t>& ps = PositionsFor(posts[t], docid);
      const uint32_t want = start + static_cast<uint32_t>(t);
      if (!std::binary_search(ps.begin(), ps.end(), want)) {
        ok = false;
        break;
      }
    }
    if (ok) return true;
  }
  return false;
}

// Resolves every term and registers its pod_ref ranges with the fetcher.
// *all_present=false (with OK) as soon as any term is absent.
Status PlanTerms(const LogicalIndexReader& idx,
                 const std::vector<std::string>& terms,
                 snii::io::BatchRangeFetcher* fetcher,
                 std::vector<TermPlan>* plans, bool* all_present) {
  *all_present = true;
  plans->resize(terms.size());
  for (size_t i = 0; i < terms.size(); ++i) {
    TermPlan& p = (*plans)[i];
    bool found = false;
    SNII_RETURN_IF_ERROR(
        idx.lookup(terms[i], &found, &p.entry, &p.frq_base, &p.prx_base));
    if (!found) {
      *all_present = false;
      return Status::OK();
    }
    p.pod_ref = (p.entry.kind == DictEntryKind::kPodRef);
    p.windowed = p.pod_ref && p.entry.enc == DictEntryEnc::kWindowed;
    // Windowed terms are decoded later through their two-level prelude (their
    // own batched sub-range fetch); only slim pod_ref terms join this batch.
    if (p.pod_ref && !p.windowed) {
      uint64_t foff = 0, flen = 0, poff = 0, plen = 0;
      SNII_RETURN_IF_ERROR(FrqWindowRange(idx, p, &foff, &flen));
      SNII_RETURN_IF_ERROR(PrxWindowRange(idx, p, &poff, &plen));
      p.frq_handle = fetcher->add(foff, static_cast<size_t>(flen));
      p.prx_handle = fetcher->add(poff, static_cast<size_t>(plen));
    }
  }
  return Status::OK();
}

// Decodes a windowed term's full posting (docids + positions) via its prelude.
Status MaterializeWindowed(const LogicalIndexReader& idx, const TermPlan& p,
                           TermPostings* out) {
  snii::reader::DecodedPosting posting;
  SNII_RETURN_IF_ERROR(snii::reader::read_windowed_posting(
      idx, p.entry, p.frq_base, p.prx_base, /*want_positions=*/true, &posting));
  out->docids = std::move(posting.docids);
  out->positions = std::move(posting.positions);
  if (out->positions.size() != out->docids.size()) {
    return Status::Corruption("phrase_query: windowed prx/frq doc-count mismatch");
  }
  return Status::OK();
}

// Materializes per-term postings from inline bytes, batched slim fetch results,
// or (for windowed terms) the two-level prelude decode.
Status MaterializePostings(const LogicalIndexReader& idx,
                           const snii::io::BatchRangeFetcher& fetcher,
                           const std::vector<TermPlan>& plans,
                           std::vector<TermPostings>* posts) {
  posts->resize(plans.size());
  for (size_t i = 0; i < plans.size(); ++i) {
    const TermPlan& p = plans[i];
    if (p.windowed) {
      SNII_RETURN_IF_ERROR(MaterializeWindowed(idx, p, &(*posts)[i]));
    } else if (p.pod_ref) {
      SNII_RETURN_IF_ERROR(DecodePostings(fetcher.get(p.frq_handle),
                                          fetcher.get(p.prx_handle),
                                          &(*posts)[i]));
    } else {
      SNII_RETURN_IF_ERROR(DecodePostings(Slice(p.entry.frq_bytes),
                                          Slice(p.entry.prx_bytes),
                                          &(*posts)[i]));
    }
  }
  return Status::OK();
}

}  // namespace

Status phrase_query(const LogicalIndexReader& idx,
                    const std::vector<std::string>& terms,
                    std::vector<uint32_t>* docids) {
  if (docids == nullptr) return Status::InvalidArgument("phrase_query: null out");
  docids->clear();
  if (terms.empty()) return Status::OK();
  if (!idx.has_positions()) {
    return Status::Unsupported("phrase_query: index has no positions");
  }

  // 1. Resolve all terms and plan their .frq/.prx ranges.
  snii::io::BatchRangeFetcher fetcher(idx.reader());
  std::vector<TermPlan> plans;
  bool all_present = false;
  SNII_RETURN_IF_ERROR(PlanTerms(idx, terms, &fetcher, &plans, &all_present));
  if (!all_present) return Status::OK();

  // 2. One batched read for all pod_ref windows (one serial round).
  if (fetcher.pending() > 0) SNII_RETURN_IF_ERROR(fetcher.fetch());

  // 3. Decode every term's postings (windowed terms tile their windows here).
  std::vector<TermPostings> posts;
  SNII_RETURN_IF_ERROR(MaterializePostings(idx, fetcher, plans, &posts));

  // 4. Positional merge: intersect docids, then verify consecutive positions.
  std::vector<uint32_t> candidates = IntersectDocids(posts);
  for (uint32_t d : candidates) {
    if (PhraseInDoc(posts, d)) docids->push_back(d);
  }
  return Status::OK();
}

}  // namespace snii::query
