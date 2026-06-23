#include "snii/query/phrase_query.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
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

// Intersects two ascending docid sets.
std::vector<uint32_t> IntersectSorted(const std::vector<uint32_t>& a,
                                      const std::vector<uint32_t>& b) {
  std::vector<uint32_t> out;
  out.reserve(std::min(a.size(), b.size()));
  std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                        std::back_inserter(out));
  return out;
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
                 std::vector<TermPlan>* plans, bool* all_present,
                 bool need_positions) {
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
      // Docids (always): only the .frq docs-only prefix (frq_docs_len) when
      // recorded. Positions (.prx) only when needed (phrase); a docid-only
      // conjunction (boolean AND) or a docs-only index skips the prx range.
      uint64_t frq_fetch = flen;
      SNII_RETURN_IF_ERROR(SlimFrqDocsLen(p.entry, flen, &frq_fetch));
      p.frq_handle = fetcher->add(foff, static_cast<size_t>(frq_fetch));
      if (need_positions) {
        SNII_RETURN_IF_ERROR(idx.resolve_prx_window(p.entry, p.prx_base, &poff, &plen));
        p.prx_handle = fetcher->add(poff, static_cast<size_t>(plen));
      }
    }
  }
  return Status::OK();
}

// Opens each windowed term's prelude from the round-1 batch result. When
// need_positions, the index must carry positions (phrase); a docid-only
// conjunction (boolean AND) or a docs-only index does not require prx.
Status OpenPreludes(const snii::io::BatchRangeFetcher& fetcher,
                    std::vector<TermPlan>* plans, bool need_positions) {
  for (TermPlan& p : *plans) {
    if (!p.windowed) continue;
    SNII_RETURN_IF_ERROR(
        FrqPreludeReader::open(fetcher.get(p.prelude_handle), &p.prelude));
    if (need_positions && !p.prelude.has_prx()) {
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

// One decoded chunk of a term's posting: a windowed term's covering window, or a
// slim/inline term's single posting. `docids` is decoded in the conjunction phase
// (and reused by the streaming cursor -- the dd region is decoded exactly once);
// `prx` is the on-disk positions bytes, decoded lazily by the cursor (once per
// chunk) during phrase verification.
struct PosChunk {
  std::vector<uint32_t> docids;  // ascending, absolute
  Slice prx;                      // .prx window bytes (reference fetcher/round1/entry)
};

// A term's retained posting as an ordered list of chunks (windowed: covering
// windows in docid order; slim/inline: one). The referenced prx bytes live in
// `round1` / the per-term fetchers kept alive in phrase_query::owners for the
// whole query, so the cursor can decode positions during verification.
struct PosSource {
  std::vector<PosChunk> chunks;
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
    const std::vector<uint32_t>& windows, bool need_positions,
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
        /*want_positions=*/need_positions, /*want_freq=*/false, &r));
    WindowMeta m;
    SNII_RETURN_IF_ERROR(p.prelude.window(w, &m));
    const size_t dh = fetcher->add(r.dd_off, static_cast<size_t>(r.dd_len));
    const size_t ph = need_positions
                          ? fetcher->add(r.prx_off, static_cast<size_t>(r.prx_len))
                          : 0;
    handles.emplace_back(dh, ph);
    metas.push_back(m);
  }
  if (fetcher->pending() > 0) SNII_RETURN_IF_ERROR(fetcher->fetch());

  for (size_t k = 0; k < metas.size(); ++k) {
    PosChunk chunk;
    std::vector<uint32_t> freqs;
    std::vector<std::vector<uint32_t>> pos;  // stays empty (want_positions=false)
    SNII_RETURN_IF_ERROR(snii::reader::decode_window_slices(
        metas[k], fetcher->get(handles[k].first), Slice(), Slice(),
        /*want_positions=*/false, /*want_freq=*/false, &chunk.docids, &freqs, &pos));
    for (uint32_t d : chunk.docids) term_docids->push_back(d);
    if (need_positions) chunk.prx = fetcher->get(handles[k].second);
    src->chunks.push_back(std::move(chunk));
  }
  owners->push_back(std::move(fetcher));  // unique_ptr move keeps the buffer fixed
  return Status::OK();
}

// Phase A (slim/inline term): decode DOCIDS ONLY from the round-1 dd region into
// *term_docids, and retain the dd/prx slices in *src for Phase B (prx only when
// need_positions; a docid-only conjunction / docs-only index leaves it empty).
Status DecodeFlatDocids(const snii::io::BatchRangeFetcher& round1, const TermPlan& p,
                        bool need_positions, PosSource* src,
                        std::vector<uint32_t>* term_docids) {
  PosChunk chunk;
  Slice dd;
  if (p.pod_ref) {
    dd = round1.get(p.frq_handle);
    if (need_positions) chunk.prx = round1.get(p.prx_handle);
  } else {
    SNII_RETURN_IF_ERROR(InlineDdRegion(p.entry, &dd));
    if (need_positions) chunk.prx = Slice(p.entry.prx_bytes);
  }
  SNII_RETURN_IF_ERROR(snii::format::decode_dd_region(
      dd, p.entry.dd_meta, /*win_base=*/0, &chunk.docids));
  *term_docids = chunk.docids;
  src->chunks.push_back(std::move(chunk));
  return Status::OK();
}

// All windows of a windowed term in ascending order (the driver reads its full
// docid set; a narrowing term reads only SelectCoveringWindows).
std::vector<uint32_t> AllWindows(const FrqPreludeReader& prelude) {
  std::vector<uint32_t> ws(prelude.n_windows());
  for (uint32_t i = 0; i < prelude.n_windows(); ++i) ws[i] = i;
  return ws;
}

// Streaming position cursor over one term's retained chunks. It advances ONLY
// forward (callers seek ascending candidate docids), decodes each chunk's docids
// once (reused from the conjunction phase) and each chunk's positions at most once
// (lazily, into a flat CSR whose capacity is retained across chunks). No per-doc
// allocation, no per-candidate docid binary search: positions are addressed by the
// doc's local index within its chunk. This is the read-side dual of the windowed
// posting layout -- the S3-native batch fetch already pulled every needed chunk
// into memory; the cursor is pure in-memory column iteration.
class PostingCursor {
 public:
  void init(const PosSource* src) {
    src_ = src;
    ci_ = 0;
    li_ = 0;
    decoded_pos_chunk_ = kNoChunk;
  }

  // Positions the cursor at `target` (guaranteed present: candidates are the
  // intersection of exactly these chunks' docids). Monotonic forward advance.
  void seek(uint32_t target) {
    while (ci_ < src_->chunks.size() &&
           (src_->chunks[ci_].docids.empty() ||
            src_->chunks[ci_].docids.back() < target)) {
      ++ci_;
      li_ = 0;
    }
    if (ci_ >= src_->chunks.size()) return;  // exhausted (not expected for a candidate)
    const std::vector<uint32_t>& d = src_->chunks[ci_].docids;
    while (li_ < d.size() && d[li_] < target) ++li_;
  }

  // [begin,end) of the current doc's positions, decoding the current chunk's .prx
  // exactly once (cached). Must follow a seek that landed on a real doc.
  Status positions(std::pair<const uint32_t*, const uint32_t*>* out) {
    if (ci_ >= src_->chunks.size() || li_ >= src_->chunks[ci_].docids.size()) {
      return Status::Corruption("phrase_query: cursor positions out of range");
    }
    if (decoded_pos_chunk_ != ci_) {
      ByteSource ps(src_->chunks[ci_].prx);
      SNII_RETURN_IF_ERROR(
          snii::format::read_prx_window_csr(&ps, &pflat_, &poff_));
      if (poff_.size() != src_->chunks[ci_].docids.size() + 1) {
        return Status::Corruption("phrase_query: prx/dd doc-count mismatch");
      }
      decoded_pos_chunk_ = ci_;
    }
    *out = {pflat_.data() + poff_[li_], pflat_.data() + poff_[li_ + 1]};
    return Status::OK();
  }

 private:
  static constexpr size_t kNoChunk = static_cast<size_t>(-1);
  const PosSource* src_ = nullptr;
  size_t ci_ = 0;                       // current chunk
  size_t li_ = 0;                       // current local doc index within the chunk
  size_t decoded_pos_chunk_ = kNoChunk;  // which chunk pflat_/poff_ currently hold
  std::vector<uint32_t> pflat_;         // current chunk's flat positions (reused)
  std::vector<uint32_t> poff_;          // current chunk's per-doc offsets (reused)
};

// Returns plan indices ordered by ascending df (the driver -- smallest df -- is
// first; remaining terms shrink the candidate set fastest in this order).
std::vector<size_t> AscendingDfOrder(const std::vector<TermPlan>& plans) {
  std::vector<size_t> order(plans.size());
  for (size_t i = 0; i < plans.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(),
            [&](size_t a, size_t b) { return plans[a].df < plans[b].df; });
  return order;
}

// Lead-term-driven DOCID-ONLY conjunction (S3-native fetch unchanged): the driver
// (smallest df) reads its full docid set; each later term reads only the windows /
// posting covering the running candidate set, decoding ONLY docids. Fills `srcs`
// (each term's retained chunks, for the streaming position cursor) and `candidates`
// (the docs containing every term). The per-term fetchers are moved into `owners`,
// which the caller keeps alive through phrase verification (the chunks' prx slices
// reference them). No positions are decoded here.
Status BuildConjunction(
    const LogicalIndexReader& idx, const snii::io::BatchRangeFetcher& round1,
    const std::vector<TermPlan>& plans, bool need_positions,
    std::vector<std::unique_ptr<snii::io::BatchRangeFetcher>>* owners,
    std::vector<PosSource>* srcs, std::vector<uint32_t>* candidates) {
  const std::vector<size_t> order = AscendingDfOrder(plans);
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
      SNII_RETURN_IF_ERROR(DecodeWindowedDocids(
          idx, p, windows, need_positions, owners, &(*srcs)[ti], &term_docids));
    } else {
      SNII_RETURN_IF_ERROR(
          DecodeFlatDocids(round1, p, need_positions, &(*srcs)[ti], &term_docids));
    }
    if (k == 0) {
      *candidates = std::move(term_docids);
    } else {
      *candidates = IntersectSorted(*candidates, term_docids);
    }
    if (candidates->empty()) return Status::OK();  // empty conjunction
  }
  return Status::OK();
}

// Single streaming pass over the candidates: for each (ascending) candidate,
// advance every term's cursor to it, gather each term's positions IN PHRASE ORDER,
// and test the consecutive-phrase predicate (term[0]@p, term[1]@p+1, ...) with
// term-level short-circuit. Cursors decode each chunk's docids/positions exactly
// once and address positions by local index -- no per-candidate docid binary
// search, no full-candidate position materialization. Candidates are ascending so
// the emitted docids are already sorted.
Status EmitPhraseStreaming(const std::vector<TermPlan>& plans,
                           std::vector<PosSource>& srcs,
                           const std::vector<uint32_t>& candidates,
                           std::vector<uint32_t>* docids) {
  const size_t n = plans.size();
  std::vector<PostingCursor> cur(n);
  for (size_t i = 0; i < n; ++i) cur[i].init(&srcs[i]);
  // ordered[phrase_pos] = the cursor of the term at that phrase position.
  std::vector<PostingCursor*> ordered(n);
  for (size_t i = 0; i < n; ++i) ordered[plans[i].order] = &cur[i];

  std::vector<std::pair<const uint32_t*, const uint32_t*>> span(n);
  for (uint32_t d : candidates) {
    for (size_t i = 0; i < n; ++i) cur[i].seek(d);
    for (size_t pp = 0; pp < n; ++pp) {
      SNII_RETURN_IF_ERROR(ordered[pp]->positions(&span[pp]));
    }
    bool match = false;
    for (const uint32_t* p = span[0].first; p != span[0].second; ++p) {
      const uint32_t start = *p;
      bool ok = true;
      for (size_t t = 1; t < n; ++t) {
        const uint32_t want = start + static_cast<uint32_t>(t);
        if (!std::binary_search(span[t].first, span[t].second, want)) {
          ok = false;
          break;
        }
      }
      if (ok) {
        match = true;
        break;
      }
    }
    if (match) docids->push_back(d);
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

  // Round 1: preludes (windowed) + full postings (slim/inline) batched together.
  snii::io::BatchRangeFetcher round1(idx.reader());
  std::vector<TermPlan> plans;
  bool all_present = false;
  SNII_RETURN_IF_ERROR(
      PlanTerms(idx, terms, &round1, &plans, &all_present, /*need_positions=*/true));
  if (!all_present) return Status::OK();
  if (round1.pending() > 0) SNII_RETURN_IF_ERROR(round1.fetch());
  SNII_RETURN_IF_ERROR(OpenPreludes(round1, &plans, /*need_positions=*/true));

  // Phase 1: DOCID-ONLY conjunction (S3-native batch fetch). `owners` holds the
  // per-term fetchers; it MUST outlive the verification pass below because the
  // chunks' prx slices reference its buffers.
  std::vector<PosSource> srcs(plans.size());
  std::vector<std::unique_ptr<snii::io::BatchRangeFetcher>> owners;
  std::vector<uint32_t> candidates;
  SNII_RETURN_IF_ERROR(BuildConjunction(idx, round1, plans, /*need_positions=*/true,
                                        &owners, &srcs, &candidates));
  if (candidates.empty()) return Status::OK();

  // Phase 2: single streaming pass -- positional verify via per-term cursors.
  SNII_RETURN_IF_ERROR(EmitPhraseStreaming(plans, srcs, candidates, docids));
  return Status::OK();
}

// boolean AND (MATCH all-terms): the sorted docid set of docs containing EVERY
// term, with NO positional constraint. Reuses the docid-only conjunction (driver =
// min-df term, high-df terms read only the windows covering the running
// candidates) but fetches NO positions -- so the high-df term's bytes scale with
// selectivity, not df. Works on docs-only indexes (no .prx required). Empty term
// list or any absent term -> empty result.
Status boolean_and(const LogicalIndexReader& idx,
                   const std::vector<std::string>& terms,
                   std::vector<uint32_t>* docids) {
  if (docids == nullptr) return Status::InvalidArgument("boolean_and: null out");
  docids->clear();
  if (terms.empty()) return Status::OK();

  snii::io::BatchRangeFetcher round1(idx.reader());
  std::vector<TermPlan> plans;
  bool all_present = false;
  SNII_RETURN_IF_ERROR(PlanTerms(idx, terms, &round1, &plans, &all_present,
                                 /*need_positions=*/false));
  if (!all_present) return Status::OK();
  if (round1.pending() > 0) SNII_RETURN_IF_ERROR(round1.fetch());
  SNII_RETURN_IF_ERROR(OpenPreludes(round1, &plans, /*need_positions=*/false));

  std::vector<PosSource> srcs(plans.size());
  std::vector<std::unique_ptr<snii::io::BatchRangeFetcher>> owners;
  SNII_RETURN_IF_ERROR(BuildConjunction(idx, round1, plans, /*need_positions=*/false,
                                        &owners, &srcs, docids));
  return Status::OK();
}

}  // namespace snii::query
