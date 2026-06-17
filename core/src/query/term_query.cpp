#include "snii/query/term_query.h"

#include <vector>

#include "snii/common/slice.h"
#include "snii/encoding/byte_source.h"
#include "snii/format/dict_entry.h"
#include "snii/format/format_constants.h"
#include "snii/format/frq_pod.h"
#include "snii/io/batch_range_fetcher.h"

namespace snii::query {

using snii::format::DictEntry;
using snii::format::DictEntryKind;
using snii::reader::LogicalIndexReader;

namespace {

// Decodes the docids of one .frq window from raw window bytes (prelude already
// stripped). win_base is 0 for SNII single-window slim/windowed entries (the
// writer builds each window with win_base=0).
Status DecodeFrqDocs(Slice window, std::vector<uint32_t>* docids) {
  ByteSource src(window);
  return snii::format::read_frq_window_docs(&src, /*win_base=*/0, docids);
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

  // Inline entry: the frq window bytes live inside the DictEntry.
  if (entry.kind == DictEntryKind::kInline) {
    return DecodeFrqDocs(Slice(entry.frq_bytes), docids);
  }

  // pod_ref entry: abs frq offset = frq_pod.offset + frq_base + frq_off_delta.
  // The windowed payload is [prelude][window]; skip prelude_len to reach the
  // window. Slim pod_ref has no prelude (prelude_len == 0).
  const uint64_t frq_abs =
      idx.section_refs().frq_pod.offset + frq_base + entry.frq_off_delta;
  const uint64_t win_abs = frq_abs + entry.prelude_len;
  const uint64_t win_len = entry.frq_len - entry.prelude_len;

  snii::io::BatchRangeFetcher fetcher(idx.reader());
  const size_t h = fetcher.add(win_abs, static_cast<size_t>(win_len));
  SNII_RETURN_IF_ERROR(fetcher.fetch());
  return DecodeFrqDocs(fetcher.get(h), docids);
}

}  // namespace snii::query
