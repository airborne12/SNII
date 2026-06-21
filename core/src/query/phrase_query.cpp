#include "snii/query/phrase_query.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
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

// Decoded postings for one term, candidate-aligned, in CSR layout: docids[i]'s
// positions are pos_flat[pos_off[i] .. pos_off[i+1]). The flat layout replaces a
// per-doc std::vector<uint32_t> (which dominated phrase CPU via per-doc malloc);
// pos_flat/pos_off are flat uint32 buffers whose capacity is retained across decode
// calls (clear()), so the survivor positions are gathered with ~no allocation.
struct TermPostings {
  std::vector<uint32_t> docids;    // ascending
  std::vector<uint32_t> pos_flat;  // all docs' positions concatenated
  std::vector<uint32_t> pos_off;   // size docids.size()+1; pos_off[0]==0
};

// Intersects two ascending docid sets.
std::vector<uint32_t> IntersectSorted(const std::vector<uint32_t>& a,
                                      const std::vector<uint32_t>& b) {
  std::vector<uint32_t> out;
  out.reserve(std::min(a.size(), b.size()));
  std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                        std::back_inserter(out));
  return out;
}

// [begin,end) of the doc's positions in the CSR (docid must exist in post.docids).
std::pair<const uint32_t*, const uint32_t*> PositionsSpan(const TermPostings& post,
                                                          uint32_t docid) {
  const auto it = std::lower_bound(post.docids.begin(), post.docids.end(), docid);
  const size_t i = static_cast<size_t>(it - post.docids.begin());
  return {post.pos_flat.data() + post.pos_off[i],
          post.pos_flat.data() + post.pos_off[i + 1]};
}

// Checks term[0]@p, term[1]@p+1, ... consecutively in docid across ordered posts.
bool PhraseInDoc(const std::vector<const TermPostings*>& posts, uint32_t docid) {
  const auto first = PositionsSpan(*posts[0], docid);
  for (const uint32_t* pp = first.first; pp != first.second; ++pp) {
    const uint32_t start = *pp;
    bool ok = true;
    for (size_t t = 1; t < posts.size(); ++t) {
      const auto ps = PositionsSpan(*posts[t], docid);
      const uint32_t want = start + static_cast<uint32_t>(t);
      if (!std::binary_search(ps.first, ps.second, want)) {
        ok = false;
        break;
      }
    }
    if (ok) return true;
  }
  return false;
}

// Resolves the dd-region (docs-only) fetch length for a slim pod_ref: frq_docs_len
// (the dd region on-disk length) when valid, else the full window (defensive
// fallback). Validates frq_docs_len <= win_len (anti-DoS).
Status SlimFrqDocsLen(const DictEntry& entry, uint64_t win_len, uint64_t* out) {
  if (entry.frq_docs_len > win_len) {
    return Status::Corruption("phrase_query: slim frq_docs_len exceeds frq window");
  }
  *out = entry.frq_docs_len > 0 ? entry.frq_docs_len : win_len;
  return Status::OK();
}

// Returns the inline entry's dd region slice ([0, dd_meta.disk_len)).
Status InlineDdRegion(const DictEntry& entry, Slice* out) {
  if (entry.dd_meta.disk_len > entry.frq_bytes.size()) {
    return Status::Corruption("phrase_query: inline dd region exceeds frq bytes");
  }
  *out = Slice(entry.frq_bytes.data(), static_cast<size_t>(entry.dd_meta.disk_len));
  return Status::OK();
}

// Resolves every term, registering round-1 reads: windowed -> prelude bytes;
// slim pod_ref -> docs-only .frq prefix + full .prx; inline -> nothing.
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
      // Phrase needs docids + positions but NOT freqs: fetch only the .frq
      // docs-only prefix (frq_docs_len) when recorded; .prx is fetched in full.
      uint64_t frq_fetch = flen;
      SNII_RETURN_IF_ERROR(SlimFrqDocsLen(p.entry, flen, &frq_fetch));
      p.frq_handle = fetcher->add(foff, static_cast<size_t>(frq_fetch));
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

// Retained position source for a NON-driver term after Phase A (docid narrowing):
// the FETCHED covering-window slices (windowed) or the flat dd/prx slices
// (slim/inline), so Phase B can decode positions ONLY for the final surviving
// candidates instead of for every doc the term touches. The referenced byte slices
// live in `round1` / the per-term fetchers kept alive in NarrowCandidates::owners.
struct PosSource {
  bool windowed = false;
  const DictEntry* entry = nullptr;  // points into the plan (alive for the query)
  // windowed: one entry per fetched covering window (slices reference a fetcher).
  std::vector<WindowMeta> metas;
  std::vector<Slice> dd_slices;
  std::vector<Slice> prx_slices;
  // slim/inline:
  Slice flat_dd;
  Slice flat_prx;
};

// Phase A (windowed term): fetch the given `windows` (dd + prx in one batch),
// decode DOCIDS ONLY (no positions -> no per-doc allocation) into *term_docids, and
// retain the fetched window slices in *src for the Phase-B positions pass. The
// fetcher is moved into *owners so the slices stay valid. For the driver `windows`
// is every window (its full docid set); for a narrowing term it is only the windows
// covering the current candidates. The fetch (dd + prx in one batch) matches the
// previous one-pass path, so the I/O metrics are unchanged -- only the position
// DECODE is deferred to the survivors.
Status DecodeWindowedDocids(
    const LogicalIndexReader& idx, const TermPlan& p,
    const std::vector<uint32_t>& windows,
    std::vector<std::unique_ptr<snii::io::BatchRangeFetcher>>* owners,
    PosSource* src, std::vector<uint32_t>* term_docids) {
  auto fetcher = std::make_unique<snii::io::BatchRangeFetcher>(
      idx.reader(), snii::reader::kSameTermCoalesceGap);
  std::vector<std::pair<size_t, size_t>> handles;  // (dd_handle, prx_handle)
  std::vector<WindowMeta> metas;
  handles.reserve(windows.size());
  metas.reserve(windows.size());
  for (uint32_t w : windows) {
    snii::reader::WindowAbsRange r;
    SNII_RETURN_IF_ERROR(snii::reader::windowed_window_range(
        idx, p.entry, p.frq_base, p.prx_base, p.prelude, w,
        /*want_positions=*/true, /*want_freq=*/false, &r));
    WindowMeta m;
    SNII_RETURN_IF_ERROR(p.prelude.window(w, &m));
    const size_t dh = fetcher->add(r.dd_off, static_cast<size_t>(r.dd_len));
    const size_t ph = fetcher->add(r.prx_off, static_cast<size_t>(r.prx_len));
    handles.emplace_back(dh, ph);
    metas.push_back(m);
  }
  if (fetcher->pending() > 0) SNII_RETURN_IF_ERROR(fetcher->fetch());

  src->windowed = true;
  for (size_t k = 0; k < metas.size(); ++k) {
    std::vector<uint32_t> docids, freqs;
    std::vector<std::vector<uint32_t>> pos;  // stays empty (want_positions=false)
    SNII_RETURN_IF_ERROR(snii::reader::decode_window_slices(
        metas[k], fetcher->get(handles[k].first), Slice(), Slice(),
        /*want_positions=*/false, /*want_freq=*/false, &docids, &freqs, &pos));
    for (uint32_t d : docids) term_docids->push_back(d);
    src->metas.push_back(metas[k]);
    src->dd_slices.push_back(fetcher->get(handles[k].first));
    src->prx_slices.push_back(fetcher->get(handles[k].second));
  }
  owners->push_back(std::move(fetcher));  // unique_ptr move keeps the buffer fixed
  return Status::OK();
}

// Phase A (slim/inline term): decode DOCIDS ONLY from the round-1 dd region into
// *term_docids, and retain the dd/prx slices in *src for Phase B.
Status DecodeFlatDocids(const snii::io::BatchRangeFetcher& round1, const TermPlan& p,
                        PosSource* src, std::vector<uint32_t>* term_docids) {
  src->windowed = false;
  src->entry = &p.entry;
  if (p.pod_ref) {
    src->flat_dd = round1.get(p.frq_handle);
    src->flat_prx = round1.get(p.prx_handle);
  } else {
    SNII_RETURN_IF_ERROR(InlineDdRegion(p.entry, &src->flat_dd));
    src->flat_prx = Slice(p.entry.prx_bytes);
  }
  SNII_RETURN_IF_ERROR(snii::format::decode_dd_region(
      src->flat_dd, p.entry.dd_meta, /*win_base=*/0, term_docids));
  return Status::OK();
}

// All windows of a windowed term in ascending order (the driver reads its full
// docid set; a narrowing term reads only SelectCoveringWindows).
std::vector<uint32_t> AllWindows(const FrqPreludeReader& prelude) {
  std::vector<uint32_t> ws(prelude.n_windows());
  for (uint32_t i = 0; i < prelude.n_windows(); ++i) ws[i] = i;
  return ws;
}

// Appends doc-source-index `si`'s positions (CSR span [soff[si],soff[si+1]) of
// sflat) as candidate `ci`'s positions in the output CSR.
inline void AppendSpan(const std::vector<uint32_t>& sflat,
                       const std::vector<uint32_t>& soff, size_t si, size_t ci,
                       TermPostings* out) {
  out->pos_off[ci] = static_cast<uint32_t>(out->pos_flat.size());
  for (uint32_t j = soff[si]; j < soff[si + 1]; ++j) out->pos_flat.push_back(sflat[j]);
  out->pos_off[ci + 1] = static_cast<uint32_t>(out->pos_flat.size());
}

// Phase B (windowed): decode positions for the FINAL candidates only, from the
// retained covering windows, into the output CSR. Each window's positions are
// decoded into a FLAT buffer (read_prx_window_csr -- no per-doc allocation) and
// only the surviving candidates' spans are copied out. Windows with no surviving
// candidate are skipped entirely (the dominant decode saving for the lazily-read
// driver). Windows + candidates are ascending, so one forward merge suffices.
Status MaterializeCandidatePositionsWindowed(const PosSource& src,
                                             const std::vector<uint32_t>& candidates,
                                             TermPostings* out) {
  out->docids = candidates;
  out->pos_flat.clear();
  out->pos_off.assign(candidates.size() + 1, 0);
  size_t ci = 0;
  for (size_t k = 0; k < src.metas.size() && ci < candidates.size(); ++k) {
    if (candidates[ci] > src.metas[k].last_docid) continue;  // no survivor here
    std::vector<uint32_t> wdocids, wfreqs;
    std::vector<std::vector<uint32_t>> wpos_unused;  // stays empty
    SNII_RETURN_IF_ERROR(snii::reader::decode_window_slices(
        src.metas[k], src.dd_slices[k], Slice(), Slice(),
        /*want_positions=*/false, /*want_freq=*/false, &wdocids, &wfreqs,
        &wpos_unused));
    std::vector<uint32_t> wflat, woff;
    ByteSource psrc(src.prx_slices[k]);
    SNII_RETURN_IF_ERROR(snii::format::read_prx_window_csr(&psrc, &wflat, &woff));
    if (woff.size() != wdocids.size() + 1) {
      return Status::Corruption("phrase_query: window prx/dd doc-count mismatch");
    }
    size_t di = 0;
    while (di < wdocids.size() && ci < candidates.size()) {
      if (wdocids[di] < candidates[ci]) {
        ++di;
      } else if (wdocids[di] == candidates[ci]) {
        AppendSpan(wflat, woff, di, ci, out);
        ++di;
        ++ci;
      } else {
        out->pos_off[ci + 1] = out->pos_off[ci];  // unreachable for valid input
        ++ci;
      }
    }
  }
  for (; ci < candidates.size(); ++ci) out->pos_off[ci + 1] = out->pos_off[ci];
  return Status::OK();
}

// Phase B (slim/inline): decode the full posting's docids + positions (positions
// into a FLAT CSR -- no per-doc allocation), then copy only the final candidates'
// spans into the output CSR.
Status MaterializeCandidatePositionsFlat(const PosSource& src,
                                         const std::vector<uint32_t>& candidates,
                                         TermPostings* out) {
  std::vector<uint32_t> fdocids;
  SNII_RETURN_IF_ERROR(snii::format::decode_dd_region(
      src.flat_dd, src.entry->dd_meta, /*win_base=*/0, &fdocids));
  std::vector<uint32_t> fflat, foff;
  ByteSource psrc(src.flat_prx);
  SNII_RETURN_IF_ERROR(snii::format::read_prx_window_csr(&psrc, &fflat, &foff));
  if (foff.size() != fdocids.size() + 1) {
    return Status::Corruption("phrase_query: slim prx/dd doc-count mismatch");
  }
  out->docids = candidates;
  out->pos_flat.clear();
  out->pos_off.assign(candidates.size() + 1, 0);
  size_t di = 0, ci = 0;
  while (di < fdocids.size() && ci < candidates.size()) {
    if (fdocids[di] < candidates[ci]) {
      ++di;
    } else if (fdocids[di] == candidates[ci]) {
      AppendSpan(fflat, foff, di, ci, out);
      ++di;
      ++ci;
    } else {
      out->pos_off[ci + 1] = out->pos_off[ci];  // unreachable for valid input
      ++ci;
    }
  }
  for (; ci < candidates.size(); ++ci) out->pos_off[ci + 1] = out->pos_off[ci];
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

// Lead-term-driven narrowing in TWO phases. EVERY term (the driver included) is
// read DOCID-ONLY in Phase A; positions are decoded in Phase B ONLY for the final
// surviving candidates -- so no term, not even the driver, allocates a position
// vector for a doc that the phrase conjunction rejects. The fetch pattern matches
// the previous one-pass path (dd + prx batched per term), so the I/O metrics are
// unchanged; only position-decode CPU + allocation move to the survivor set.
Status NarrowCandidates(const LogicalIndexReader& idx,
                        const snii::io::BatchRangeFetcher& round1,
                        const std::vector<TermPlan>& plans,
                        std::vector<TermPostings>* posts,
                        std::vector<uint32_t>* candidates) {
  const std::vector<size_t> order = AscendingDfOrder(plans);
  std::vector<PosSource> srcs(plans.size());
  std::vector<std::unique_ptr<snii::io::BatchRangeFetcher>> owners;

  // Phase A: docid-only. Driver (k==0) reads its full docid set; each later term
  // reads only the windows / posting covering the running candidate set.
  for (size_t k = 0; k < order.size(); ++k) {
    const size_t ti = order[k];
    const TermPlan& p = plans[ti];
    std::vector<uint32_t> term_docids;
    if (p.windowed) {
      std::vector<uint32_t> windows;
      if (k == 0) {
        windows = AllWindows(p.prelude);
      } else {
        SNII_RETURN_IF_ERROR(
            SelectCoveringWindows(p.prelude, *candidates, &windows));
      }
      SNII_RETURN_IF_ERROR(
          DecodeWindowedDocids(idx, p, windows, &owners, &srcs[ti], &term_docids));
    } else {
      SNII_RETURN_IF_ERROR(DecodeFlatDocids(round1, p, &srcs[ti], &term_docids));
    }
    if (k == 0) {
      *candidates = std::move(term_docids);
    } else {
      *candidates = IntersectSorted(*candidates, term_docids);
    }
    if (candidates->empty()) return Status::OK();  // no survivors -> no positions
  }

  // Phase B: positions for the final candidates of EVERY term.
  for (size_t k = 0; k < order.size(); ++k) {
    const size_t ti = order[k];
    if (srcs[ti].windowed) {
      SNII_RETURN_IF_ERROR(
          MaterializeCandidatePositionsWindowed(srcs[ti], *candidates, &(*posts)[ti]));
    } else {
      SNII_RETURN_IF_ERROR(
          MaterializeCandidatePositionsFlat(srcs[ti], *candidates, &(*posts)[ti]));
    }
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
