#include "snii/query/term_query.h"

#include <utility>
#include <vector>

#include "snii/common/slice.h"
#include "snii/encoding/byte_source.h"
#include "snii/format/dict_entry.h"
#include "snii/format/format_constants.h"
#include "snii/format/frq_pod.h"
#include "snii/io/batch_range_fetcher.h"
#include "snii/reader/windowed_posting.h"

namespace snii::query {

using snii::format::DictEntry;
using snii::format::DictEntryEnc;
using snii::format::DictEntryKind;
using snii::reader::LogicalIndexReader;

namespace {

// Decodes the docids of one slim .frq window from raw window bytes. The writer
// builds slim/inline windows with win_base=0.
Status DecodeSlimDocs(Slice window, std::vector<uint32_t>* docids) {
  ByteSource src(window);
  return snii::format::read_frq_window_docs(&src, /*win_base=*/0, docids);
}

// Decodes a windowed term's full docid sequence by tiling all its windows
// through the two-level prelude. Docid-only: each window fetches ONLY its
// docs-only prefix (want_freq=false), so the freq region never crosses the wire.
Status DecodeWindowedDocs(const LogicalIndexReader& idx, const DictEntry& entry,
                          uint64_t frq_base, uint64_t prx_base,
                          std::vector<uint32_t>* docids) {
  snii::reader::DecodedPosting posting;
  SNII_RETURN_IF_ERROR(snii::reader::read_windowed_posting(
      idx, entry, frq_base, prx_base, /*want_positions=*/false,
      /*want_freq=*/false, &posting));
  *docids = std::move(posting.docids);
  return Status::OK();
}

// Resolves the docs-only fetch length for a slim pod_ref window: the recorded
// docs-only prefix (frq_docs_len) when present and valid, else the whole window
// (defensive fallback for malformed entries). Validates frq_docs_len <= win_len.
Status SlimDocsFetchLen(const DictEntry& entry, uint64_t win_len, uint64_t* out) {
  if (entry.frq_docs_len > win_len) {
    return Status::Corruption("term_query: slim frq_docs_len exceeds frq window");
  }
  *out = entry.frq_docs_len > 0 ? entry.frq_docs_len : win_len;
  return Status::OK();
}

}  // namespace

Status term_query(const LogicalIndexReader& idx, std::string_view term,
                  std::vector<uint32_t>* docids) {
  if (docids == nullptr) return Status::InvalidArgument("term_query: null out");
  docids->clear();

  bool found = false;
  DictEntry entry;
  uint64_t frq_base = 0;
  uint64_t prx_base = 0;
  SNII_RETURN_IF_ERROR(idx.lookup(term, &found, &entry, &frq_base, &prx_base));
  if (!found) return Status::OK();

  // Inline entry: the single frq window bytes live inside the DictEntry.
  if (entry.kind == DictEntryKind::kInline) {
    return DecodeSlimDocs(Slice(entry.frq_bytes), docids);
  }

  // Windowed pod_ref: tile every window via the two-level prelude.
  if (entry.enc == DictEntryEnc::kWindowed) {
    return DecodeWindowedDocs(idx, entry, frq_base, prx_base, docids);
  }

  // Slim pod_ref: one .frq window after the (absent) prelude. Docid-only: fetch
  // ONLY the docs-only prefix (frq_docs_len) instead of the full window.
  uint64_t win_abs = 0;
  uint64_t win_len = 0;
  SNII_RETURN_IF_ERROR(idx.resolve_frq_window(entry, frq_base, &win_abs, &win_len));
  uint64_t docs_len = 0;
  SNII_RETURN_IF_ERROR(SlimDocsFetchLen(entry, win_len, &docs_len));
  snii::io::BatchRangeFetcher fetcher(idx.reader());
  const size_t h = fetcher.add(win_abs, static_cast<size_t>(docs_len));
  SNII_RETURN_IF_ERROR(fetcher.fetch());
  return DecodeSlimDocs(fetcher.get(h), docids);
}

}  // namespace snii::query
