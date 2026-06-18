#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/format/dict_block_directory.h"
#include "snii/format/dict_entry.h"
#include "snii/format/format_constants.h"
#include "snii/format/per_index_meta.h"
#include "snii/format/sampled_term_index.h"
#include "snii/format/stats_block.h"
#include "snii/format/xfilter.h"
#include "snii/io/file_reader.h"

// LogicalIndexReader -- read-side counterpart of LogicalIndexWriter for one
// logical index. It owns the resident per-index meta sub-readers (XFilter,
// SampledTermIndex, DICT block directory, StatsBlock, SectionRefs) parsed from
// the per-index meta block, and resolves a query term to its DictEntry through
// the documented lookup flow:
//   XFilter (reject absent) -> SampledTermIndex (candidate block ordinal) ->
//   DICT block directory (block range) -> one range read of the DICT block ->
//   DictBlockReader::find_term.
//
// lookup() also returns the block's frq_base/prx_base (captured by the
// DictBlockReader) so callers can resolve a pod_ref entry's absolute .frq/.prx
// offsets via the writer's contract:
//   abs_frq = section_refs().frq_pod.offset + frq_base + entry.frq_off_delta
//   abs_prx = section_refs().prx_pod.offset + prx_base + entry.prx_off_delta
//
// The meta block bytes must outlive this reader (they are owned by the parent
// SniiSegmentReader's resident meta region).
namespace snii::reader {

class LogicalIndexReader {
 public:
  LogicalIndexReader() = default;

  // Parses the per-index meta block and binds the reader to file_reader.
  // file_reader / meta_block must outlive this reader.
  static Status open(snii::io::FileReader* file_reader,
                     snii::format::IndexTier tier, bool has_positions,
                     Slice meta_block, LogicalIndexReader* out);

  // Resolves term to a DictEntry. *found=false when the term is absent (XFilter
  // rejection, out-of-range sample, or DICT-block miss). On a hit, *entry is
  // filled and *frq_base / *prx_base carry the candidate block's bases.
  Status lookup(std::string_view term, bool* found, snii::format::DictEntry* entry,
                uint64_t* frq_base, uint64_t* prx_base) const;

  // Resolves a pod_ref entry's absolute .frq / .prx window byte range, validating
  // the locator against the section length (defends against corrupt entries:
  // prelude_len > frq_len underflow, or off_delta+len past the POD). *abs_off is
  // the absolute file offset of the window (after prelude); *len its byte length.
  Status resolve_frq_window(const snii::format::DictEntry& entry, uint64_t frq_base,
                            uint64_t* abs_off, uint64_t* len) const;
  Status resolve_prx_window(const snii::format::DictEntry& entry, uint64_t prx_base,
                            uint64_t* abs_off, uint64_t* len) const;

  const snii::format::SectionRefs& section_refs() const {
    return meta_.section_refs();
  }
  const snii::format::StatsBlock& stats() const { return meta_.stats(); }
  snii::format::IndexTier tier() const { return tier_; }
  bool has_positions() const { return has_positions_; }
  snii::io::FileReader* reader() const { return reader_; }

 private:
  snii::io::FileReader* reader_ = nullptr;
  snii::format::IndexTier tier_ = snii::format::IndexTier::kT1;
  bool has_positions_ = false;
  snii::format::PerIndexMetaReader meta_;
  snii::format::SampledTermIndexReader sti_;
  snii::format::DictBlockDirectoryReader dbd_;
  snii::format::XFilterReader xfilter_;
};

}  // namespace snii::reader
