#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"
#include "snii/format/dict_block.h"
#include "snii/format/dict_block_directory.h"
#include "snii/format/dict_entry.h"
#include "snii/format/format_constants.h"
#include "snii/format/per_index_meta.h"
#include "snii/format/sampled_term_index.h"
#include "snii/format/stats_block.h"
#include "snii/writer/spimi_term_buffer.h"

// LogicalIndexWriter -- builds the per-logical-index section bytes (DICT block
// region, .frq POD, .prx POD) and the meta sub-sections (SampledTermIndex, DICT
// block directory, StatsBlock, XFilter) for ONE logical index. It owns the
// in-memory section bytes and the metadata needed by the container orchestrator
// (SniiCompoundWriter) to resolve absolute offsets and emit the per-index meta
// block.
//
// This module deliberately produces ONLY relative bytes/structures: it has no
// knowledge of the absolute file position where the sections will land. The
// orchestrator stitches the absolute offsets in afterward (append-only, no
// seek-back). See snii_compound_writer.h for the precise offset contract.
//
// Per-term encoding policy (v1):
//   df >= kSlimDfThreshold (512): WINDOWED pod_ref. A frq_prelude (one window)
//     plus one .frq window is appended to the .frq POD; one .prx window to the
//     .prx POD (when tier>=T2). The DictEntry records frq/prx off_delta+len
//     relative to frq_base/prx_base (see below).
//   df < kSlimDfThreshold: SLIM. The postings are encoded as a single .frq
//     window (and .prx window). If the encoded .frq bytes are small
//     (<= kDefaultInlineThreshold), they are stored INLINE inside the DictEntry
//     (kind=inline); otherwise they are appended to the .frq POD as a slim
//     pod_ref (kind=pod_ref, enc=slim, no prelude).
//
// frq_base / prx_base convention (DOCUMENTED CONTRACT):
//   For each DICT block, frq_base = the running byte offset into THIS index's
//   .frq POD at the moment the block's first POD-backed entry is appended (i.e.
//   the POD size when the block opens). A windowed/slim pod_ref entry then sets
//   frq_off_delta = (offset of its frq bytes within the .frq POD) - frq_base, so
//   the reader computes the absolute file offset as
//     section_refs.frq_pod.offset + frq_base + frq_off_delta.
//   prx_base / prx_off_delta follow the identical rule against the .prx POD.
//   Inline entries carry no off_delta (bytes live in the entry).
namespace snii::writer {

// Inputs describing one logical index to be written.
struct SniiIndexInput {
  uint64_t index_id = 0;
  std::string index_suffix;
  snii::format::IndexConfig config = snii::format::IndexConfig::kDocsPositions;
  uint32_t doc_count = 0;
  // Per-doc 1-byte encoded norm (length doc_count); only consumed when the
  // config has scoring. May be empty otherwise.
  std::vector<uint8_t> encoded_norms;
  // Lexicographically sorted terms with ascending-docid postings.
  std::vector<TermPostings> terms;
  // Target DICT block size in bytes; a block is cut once its estimate reaches
  // this. 0 uses kDefaultTargetDictBlockBytes. Smaller values yield more blocks
  // (and a finer-grained sampled-term index).
  uint32_t target_dict_block_bytes = 0;
};

// Builds and holds the section bytes + meta sub-sections for one logical index.
class LogicalIndexWriter {
 public:
  explicit LogicalIndexWriter(const SniiIndexInput& in);

  // Builds DICT blocks, .frq/.prx PODs, sampled-term index, dict directory,
  // stats and xfilter. Must be called once before the accessors below.
  // Returns InvalidArgument on inconsistent input (e.g. norms/doc_count
  // mismatch when scoring is enabled, or non-ascending docids).
  Status build();

  // Section byte blobs (relative; orchestrator decides their absolute offsets).
  const std::vector<uint8_t>& dict_region_bytes() const { return dict_region_; }
  const std::vector<uint8_t>& frq_pod_bytes() const { return frq_pod_; }
  const std::vector<uint8_t>& prx_pod_bytes() const { return prx_pod_; }
  const std::vector<uint8_t>& norms_bytes() const { return norms_section_; }

  bool has_prx() const { return has_prx_; }
  bool has_norms() const { return has_norms_; }
  snii::format::IndexTier tier() const { return tier_; }
  uint64_t index_id() const { return index_id_; }
  const std::string& index_suffix() const { return index_suffix_; }

  // Builds the per-index meta block bytes given the resolved ABSOLUTE section
  // refs (filled by the orchestrator), appending them to out. The DICT block
  // directory entries are rebased to absolute offsets using dict_region_offset.
  Status finish_meta(const snii::format::SectionRefs& abs_refs,
                     uint64_t dict_region_offset, ByteSink* out) const;

 private:
  // One DICT block's accumulated bytes + its directory metadata (offset filled
  // by the orchestrator at finish_meta time).
  struct BlockOut {
    std::vector<uint8_t> bytes;
    uint32_t n_entries = 0;
    uint32_t checksum = 0;
    std::string first_term;
  };

  Status validate() const;
  // Iterates terms, splits DICT blocks by target size, fills PODs + blocks_.
  Status build_blocks();
  // Builds one DictEntry (inline or pod_ref), growing the PODs as needed.
  Status build_entry(const TermPostings& tp, uint64_t frq_base, uint64_t prx_base,
                     snii::format::DictEntry* e);
  // Builds a windowed (df >= kSlimDfThreshold) entry: multi-window + two-level
  // prelude appended to the .frq/.prx PODs.
  Status build_windowed_entry(const TermPostings& tp, uint64_t frq_base,
                              uint64_t prx_base, snii::format::DictEntry* e);
  // Builds a slim (df < kSlimDfThreshold) entry: single window, inline or
  // pod_ref, no prelude.
  Status build_slim_entry(const TermPostings& tp, uint64_t frq_base,
                          uint64_t prx_base, snii::format::DictEntry* e);
  // Serializes the current open block into a finished BlockOut.
  void flush_block(snii::format::DictBlockBuilder* block, std::string first_term);

  uint64_t index_id_;
  std::string index_suffix_;
  snii::format::IndexTier tier_;
  bool has_prx_;
  bool has_freq_;  // tier >= T2: a freq region is encoded per window
  bool has_norms_;
  uint32_t doc_count_;
  const std::vector<TermPostings>& terms_;
  const std::vector<uint8_t>& encoded_norms_;

  uint32_t target_dict_block_bytes_;
  std::vector<uint8_t> dict_region_;
  std::vector<uint8_t> frq_pod_;
  std::vector<uint8_t> prx_pod_;
  std::vector<uint8_t> norms_section_;

  std::vector<BlockOut> blocks_;
  std::vector<std::string> sample_first_terms_;
  std::vector<std::string> all_terms_;
  snii::format::StatsBlock stats_;
  std::vector<uint8_t> xfilter_bytes_;
};

}  // namespace snii::writer
