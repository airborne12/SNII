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

// Decodes the docids of one slim/inline .frq window's dd region (win_base=0). The
// dd_region slice is exactly the entry's dd region on-disk bytes; entry.dd_meta
// drives the decode.
Status DecodeSlimDocs(const DictEntry& entry, Slice dd_region,
                      std::vector<uint32_t>* docids) {
  return snii::format::decode_dd_region(dd_region, entry.dd_meta, /*win_base=*/0, docids);
}

// Decodes a windowed term's full docid sequence by reading the contiguous
// [prelude][dd-block] prefix (want_freq=false), so the freq-block never crosses
// the wire.
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

// Resolves the dd-region (docs-only) fetch length for a slim pod_ref: frq_docs_len
// (the dd region on-disk length). Validates it <= the full window. A defensive
// fallback to the full window covers malformed (frq_docs_len==0) entries.
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

  // Inline entry: the single frq window bytes ([dd][freq]) live in the DictEntry.
  // The dd region is the docs-only prefix [0, dd_meta.disk_len).
  if (entry.kind == DictEntryKind::kInline) {
    if (entry.dd_meta.disk_len > entry.frq_bytes.size()) {
      return Status::Corruption("term_query: inline dd region exceeds frq bytes");
    }
    return DecodeSlimDocs(entry, Slice(entry.frq_bytes.data(),
                                       static_cast<size_t>(entry.dd_meta.disk_len)),
                          docids);
  }

  // Windowed pod_ref: read the contiguous [prelude][dd-block] prefix.
  if (entry.enc == DictEntryEnc::kWindowed) {
    return DecodeWindowedDocs(idx, entry, frq_base, prx_base, docids);
  }

  // Slim pod_ref: [dd_region][freq_region] after the (absent) prelude. Docid-only:
  // fetch ONLY the dd region (frq_docs_len) instead of the full window.
  uint64_t win_abs = 0;
  uint64_t win_len = 0;
  SNII_RETURN_IF_ERROR(idx.resolve_frq_window(entry, frq_base, &win_abs, &win_len));
  uint64_t docs_len = 0;
  SNII_RETURN_IF_ERROR(SlimDocsFetchLen(entry, win_len, &docs_len));
  snii::io::BatchRangeFetcher fetcher(idx.reader());
  const size_t h = fetcher.add(win_abs, static_cast<size_t>(docs_len));
  SNII_RETURN_IF_ERROR(fetcher.fetch());
  return DecodeSlimDocs(entry, fetcher.get(h), docids);
}

}  // namespace snii::query
