#include "snii/query/phrase_query.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <unordered_map>
#include <utility>
#include <vector>

#include "snii/common/slice.h"
#include "snii/encoding/byte_source.h"
#include "snii/format/dict_entry.h"
#include "snii/format/frq_pod.h"
#include "snii/format/frq_prelude.h"
#include "snii/format/prx_pod.h"
#include "snii/io/batch_range_fetcher.h"
#include "snii/reader/windowed_posting.h"

// phrase_query implements MATCH_PHRASE with WINDOW (sub-block) SKIPPING for
// high-df windowed terms (design spec section 6.2):
//   1. Resolve every term; reject if any is absent.
//   2. Batch-read each windowed term's prelude + each slim/inline term's full
//      posting in one round; open the two-level prelude readers.
//   3. Pick the DRIVER = smallest-df term; materialize it fully -> the initial
//      candidate docid set.
//   4. For every other term in ascending-df order, narrow the candidate set:
//        - slim/inline: intersect with its (already decoded) full posting.
//        - windowed:    locate_window() the CURRENT candidates -> the SET of
//                       windows covering them; batch-fetch ONLY those windows'
//                       .frq (+.prx); decode; keep candidates present in some
//                       covering window. A high-df term thus reads O(candidates)
//                       windows instead of its whole O(df) posting.
//   5. Positional phrase check (term[0]@p, term[1]@p+1, ...) on the survivors.
// The result is identical to a full-read intersection; only the bytes read for
// high-df windowed terms shrink.
namespace snii::query {

using snii::format::DictEntry;
using snii::format::DictEntryEnc;
using snii::format::DictEntryKind;
using snii::format::FrqPreludeReader;
using snii::format::WindowMeta;
using snii::reader::LogicalIndexReader;

namespace {

// Resolved per-term lookup result plus its planned byte sources / prelude.
struct TermPlan {
  DictEntry entry;
  uint64_t frq_base = 0;
  uint64_t prx_base = 0;
  uint32_t df = 0;
  size_t order = 0;  // original term position (for the final phrase ordering)
  // slim pod_ref: batch handles for the full posting; inline: DictEntry bytes.
  size_t frq_handle = 0;
  size_t prx_handle = 0;
  size_t prelude_handle = 0;  // windowed: handle of the prelude bytes
  bool pod_ref = false;
  bool windowed = false;
  FrqPreludeReader prelude;  // valid for windowed terms after round 1
};

// Decoded postings for one term: docid -> aligned position list. For windowed
// terms this holds only the candidate docids that were located (a partial view).
struct TermPostings {
  std::vector<uint32_t> docids;                  // ascending
  std::vector<std::vector<uint32_t>> positions;  // positions[i] for docids[i]
};

// Decodes a full slim/inline posting (single window, win_base=0) from slices.
Status DecodeFullPosting(Slice frq_window, Slice prx_window, TermPostings* out) {
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

// Intersects two ascending docid sets, projecting the surviving docs of the
// driver's postings; positions are kept for the driver term only here (callers
// keep each term's own positions in its TermPostings).
std::vector<uint32_t> IntersectSorted(const std::vector<uint32_t>& a,
                                      const std::vector<uint32_t>& b) {
  std::vector<uint32_t> out;
  out.reserve(std::min(a.size(), b.size()));
  std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                        std::back_inserter(out));
  return out;
}

// Returns the position list aligned to docid (binary search; docid must exist).
const std::vector<uint32_t>& PositionsFor(const TermPostings& post, uint32_t docid) {
  const auto it = std::lower_bound(post.docids.begin(), post.docids.end(), docid);
  const size_t i = static_cast<size_t>(it - post.docids.begin());
  return post.positions[i];
}

// Checks term[0]@p, term[1]@p+1, ... consecutively in docid across ordered posts.
bool PhraseInDoc(const std::vector<const TermPostings*>& posts, uint32_t docid) {
  const std::vector<uint32_t>& first = PositionsFor(*posts[0], docid);
  for (uint32_t start : first) {
    bool ok = true;
    for (size_t t = 1; t < posts.size(); ++t) {
      const std::vector<uint32_t>& ps = PositionsFor(*posts[t], docid);
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

// Resolves every term, registering round-1 reads: windowed -> prelude bytes;
// slim pod_ref -> full posting ranges; inline -> nothing (bytes in the entry).
// *all_present=false (OK) as soon as any term is absent.
Status PlanTerms(const LogicalIndexReader& idx,
                 const std::vector<std::string>& terms,
                 snii::io::BatchRangeFetcher* fetcher,
                 std::vector<TermPlan>* plans, bool* all_present) {
  *all_present = true;
  plans->resize(terms.size());
  for (size_t i = 0; i < terms.size(); ++i) {
    TermPlan& p = (*plans)[i];
    p.order = i;
    bool found = false;
    SNII_RETURN_IF_ERROR(
        idx.lookup(terms[i], &found, &p.entry, &p.frq_base, &p.prx_base));
    if (!found) {
      *all_present = false;
      return Status::OK();
    }
    p.df = p.entry.df;
    p.pod_ref = (p.entry.kind == DictEntryKind::kPodRef);
    p.windowed = p.pod_ref && p.entry.enc == DictEntryEnc::kWindowed;
    if (p.windowed) {
      const uint64_t prelude_abs = idx.section_refs().frq_pod.offset + p.frq_base +
                                   p.entry.frq_off_delta;
      p.prelude_handle =
          fetcher->add(prelude_abs, static_cast<size_t>(p.entry.prelude_len));
    } else if (p.pod_ref) {
      uint64_t foff = 0, flen = 0, poff = 0, plen = 0;
      SNII_RETURN_IF_ERROR(idx.resolve_frq_window(p.entry, p.frq_base, &foff, &flen));
      SNII_RETURN_IF_ERROR(idx.resolve_prx_window(p.entry, p.prx_base, &poff, &plen));
      p.frq_handle = fetcher->add(foff, static_cast<size_t>(flen));
      p.prx_handle = fetcher->add(poff, static_cast<size_t>(plen));
    }
  }
  return Status::OK();
}

// Opens each windowed term's prelude from the round-1 batch result.
Status OpenPreludes(const snii::io::BatchRangeFetcher& fetcher,
                    std::vector<TermPlan>* plans) {
  for (TermPlan& p : *plans) {
    if (!p.windowed) continue;
    SNII_RETURN_IF_ERROR(
        FrqPreludeReader::open(fetcher.get(p.prelude_handle), &p.prelude));
    if (!p.prelude.has_prx()) {
      return Status::Corruption("phrase_query: windowed prelude has no positions");
    }
  }
  return Status::OK();
}

// Materializes a slim/inline term's full posting from round-1 results.
Status MaterializeFlat(const snii::io::BatchRangeFetcher& fetcher, const TermPlan& p,
                       TermPostings* out) {
  if (p.pod_ref) {
    return DecodeFullPosting(fetcher.get(p.frq_handle), fetcher.get(p.prx_handle), out);
  }
  return DecodeFullPosting(Slice(p.entry.frq_bytes), Slice(p.entry.prx_bytes), out);
}

// Fully materializes a windowed term (driver path): decode every window.
Status MaterializeWindowedFull(const LogicalIndexReader& idx, const TermPlan& p,
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

// Materializes any driver term (windowed full-decode or slim/inline flat decode).
Status MaterializeDriver(const LogicalIndexReader& idx,
                         const snii::io::BatchRangeFetcher& round1, const TermPlan& p,
                         TermPostings* out) {
  if (p.windowed) return MaterializeWindowedFull(idx, p, out);
  return MaterializeFlat(round1, p, out);
}

// Collects (deduplicated, ascending) the windows of a windowed term that cover
// the given candidate docids. Candidates beyond the term's last docid are simply
// not located (the term cannot match them).
Status SelectCoveringWindows(const FrqPreludeReader& prelude,
                             const std::vector<uint32_t>& candidates,
                             std::vector<uint32_t>* windows) {
  std::vector<uint32_t> sel;
  uint32_t last = UINT32_MAX;
  for (uint32_t d : candidates) {
    bool found = false;
    uint32_t w = 0;
    SNII_RETURN_IF_ERROR(prelude.locate_window(d, &found, &w));
    if (!found) continue;
    if (w != last) {  // candidates ascending => covering windows non-decreasing
      sel.push_back(w);
      last = w;
    }
  }
  *windows = std::move(sel);
  return Status::OK();
}

// Plans + fetches only the selected windows of a windowed term (round 2..N), then
// decodes them into a candidate-aligned posting. The covering windows are read
// instead of the full posting -- this is the byte-saving core.
Status MaterializeWindowedSkipped(const LogicalIndexReader& idx, const TermPlan& p,
                                  const std::vector<uint32_t>& windows,
                                  TermPostings* out) {
  snii::io::BatchRangeFetcher fetcher(idx.reader());
  std::vector<std::pair<size_t, size_t>> handles;  // (frq_handle, prx_handle)
  std::vector<WindowMeta> metas;
  handles.reserve(windows.size());
  metas.reserve(windows.size());
  for (uint32_t w : windows) {
    snii::reader::WindowAbsRange r;
    SNII_RETURN_IF_ERROR(snii::reader::windowed_window_range(
        idx, p.entry, p.frq_base, p.prx_base, p.prelude, w,
        /*want_positions=*/true, &r));
    WindowMeta m;
    SNII_RETURN_IF_ERROR(p.prelude.window(w, &m));
    const size_t fh = fetcher.add(r.frq_off, static_cast<size_t>(r.frq_len));
    const size_t ph = fetcher.add(r.prx_off, static_cast<size_t>(r.prx_len));
    handles.emplace_back(fh, ph);
    metas.push_back(m);
  }
  if (fetcher.pending() > 0) SNII_RETURN_IF_ERROR(fetcher.fetch());

  for (size_t k = 0; k < metas.size(); ++k) {
    std::vector<uint32_t> docids, freqs;
    std::vector<std::vector<uint32_t>> pos;
    SNII_RETURN_IF_ERROR(snii::reader::decode_window_slices(
        metas[k], fetcher.get(handles[k].first), fetcher.get(handles[k].second),
        /*want_positions=*/true, &docids, &freqs, &pos));
    for (size_t i = 0; i < docids.size(); ++i) {
      out->docids.push_back(docids[i]);
      out->positions.push_back(std::move(pos[i]));
    }
  }
  return Status::OK();
}

// Narrows candidates by a windowed term, reading only covering windows.
Status NarrowByWindowed(const LogicalIndexReader& idx, const TermPlan& p,
                        std::vector<uint32_t>* candidates, TermPostings* out) {
  std::vector<uint32_t> windows;
  SNII_RETURN_IF_ERROR(SelectCoveringWindows(p.prelude, *candidates, &windows));
  SNII_RETURN_IF_ERROR(MaterializeWindowedSkipped(idx, p, windows, out));
  *candidates = IntersectSorted(*candidates, out->docids);
  return Status::OK();
}

// Narrows candidates by a slim/inline term (its full posting was read in round 1).
Status NarrowByFlat(const snii::io::BatchRangeFetcher& round1, const TermPlan& p,
                    std::vector<uint32_t>* candidates, TermPostings* out) {
  SNII_RETURN_IF_ERROR(MaterializeFlat(round1, p, out));
  *candidates = IntersectSorted(*candidates, out->docids);
  return Status::OK();
}

// Returns plan indices ordered by ascending df (the driver -- smallest df -- is
// first; remaining terms shrink the candidate set fastest in this order).
std::vector<size_t> AscendingDfOrder(const std::vector<TermPlan>& plans) {
  std::vector<size_t> order(plans.size());
  for (size_t i = 0; i < plans.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(),
            [&](size_t a, size_t b) { return plans[a].df < plans[b].df; });
  return order;
}

// Lead-term-driven narrowing: fully materialize the driver, then narrow the
// candidate set by every other term (windowed terms read only covering windows).
Status NarrowCandidates(const LogicalIndexReader& idx,
                        const snii::io::BatchRangeFetcher& round1,
                        const std::vector<TermPlan>& plans,
                        std::vector<TermPostings>* posts,
                        std::vector<uint32_t>* candidates) {
  const std::vector<size_t> order = AscendingDfOrder(plans);
  for (size_t k = 0; k < order.size(); ++k) {
    const size_t ti = order[k];
    const TermPlan& p = plans[ti];
    TermPostings* out = &(*posts)[ti];
    if (k == 0) {
      SNII_RETURN_IF_ERROR(MaterializeDriver(idx, round1, p, out));
      *candidates = out->docids;
    } else if (p.windowed) {
      SNII_RETURN_IF_ERROR(NarrowByWindowed(idx, p, candidates, out));
    } else {
      SNII_RETURN_IF_ERROR(NarrowByFlat(round1, p, candidates, out));
    }
    if (candidates->empty()) break;
  }
  return Status::OK();
}

// Emits the candidates whose positions satisfy the consecutive-phrase predicate,
// using each term's postings ordered by its ORIGINAL phrase position.
void EmitPhraseMatches(const std::vector<TermPlan>& plans,
                       const std::vector<TermPostings>& posts,
                       const std::vector<uint32_t>& candidates,
                       std::vector<uint32_t>* docids) {
  std::vector<const TermPostings*> ordered(plans.size());
  for (size_t i = 0; i < plans.size(); ++i) ordered[plans[i].order] = &posts[i];
  for (uint32_t d : candidates) {
    if (PhraseInDoc(ordered, d)) docids->push_back(d);
  }
  std::sort(docids->begin(), docids->end());
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

  // Round 1: preludes (windowed) + full postings (slim/inline) batched together.
  snii::io::BatchRangeFetcher round1(idx.reader());
  std::vector<TermPlan> plans;
  bool all_present = false;
  SNII_RETURN_IF_ERROR(PlanTerms(idx, terms, &round1, &plans, &all_present));
  if (!all_present) return Status::OK();
  if (round1.pending() > 0) SNII_RETURN_IF_ERROR(round1.fetch());
  SNII_RETURN_IF_ERROR(OpenPreludes(round1, &plans));

  // Lead-term-driven window skipping: driver fully read, others narrow it.
  std::vector<TermPostings> posts(plans.size());
  std::vector<uint32_t> candidates;
  SNII_RETURN_IF_ERROR(NarrowCandidates(idx, round1, plans, &posts, &candidates));

  EmitPhraseMatches(plans, posts, candidates, docids);
  return Status::OK();
}

}  // namespace snii::query
