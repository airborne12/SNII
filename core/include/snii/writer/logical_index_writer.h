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
#include "snii/io/file_writer.h"
#include "snii/writer/spimi_term_buffer.h"
#include "snii/writer/temp_section_file.h"

// LogicalIndexWriter -- builds the per-logical-index section bytes (interleaved
// posting region + DICT block region) and the meta sub-sections (SampledTermIndex,
// DICT block directory, StatsBlock, XFilter) for ONE logical index. It owns the
// in-memory section bytes and the metadata needed by the container orchestrator
// (SniiCompoundWriter) to resolve absolute offsets and emit the per-index meta
// block.
//
// This module deliberately produces ONLY relative bytes/structures: it has no
// knowledge of the absolute file position where the sections will land. The
// orchestrator stitches the absolute offsets in afterward (append-only, no
// seek-back). See snii_compound_writer.h for the precise offset contract.
//
// POSTING REGION (single interleaved sink): the former separate .frq POD and .prx
// POD are merged into ONE posting region. For each pod_ref term, in term order, the
// writer appends its prx span FIRST then its frq span, contiguously:
//   posting region = concat over pod_ref terms of [prx span][frq span].
// The prx span is empty when !has_prx (docs-only / keyword tier). INLINE terms
// append NOTHING to the posting region.
//
// Per-term encoding policy (v1):
//   df >= kSlimDfThreshold (512): WINDOWED pod_ref. The term's [prx windows] are
//     appended to the posting region first, then its [prelude][dd-block][freq-block]
//     frq span. The DictEntry records frq/prx off_delta+len relative to
//     frq_base/prx_base (see below).
//   df < kSlimDfThreshold: SLIM. The postings are encoded as a single .frq
//     window (and .prx window). If the encoded .frq bytes are small
//     (<= kDefaultInlineThreshold), they are stored INLINE inside the DictEntry
//     (kind=inline); otherwise the term's [prx][frq] spans are appended to the
//     posting region as a slim pod_ref (kind=pod_ref, enc=slim, no prelude).
//
// frq_base / prx_base convention (DOCUMENTED CONTRACT):
//   For each DICT block, frq_base == prx_base == the running byte offset into THIS
//   index's posting region at the moment the block opens (the posting-region size
//   when the block's first POD-backed entry is appended). A windowed/slim pod_ref
//   entry then sets frq_off_delta = (offset of its frq span within the posting
//   region) - frq_base, so the reader computes the absolute file offset as
//     section_refs.posting_region.offset + frq_base + frq_off_delta.
//   prx_base / prx_off_delta follow the identical rule against the SAME region.
//   Because [prx][frq] are written contiguously per term, a writer-side property
//   holds when has_prx: frq_off_delta == prx_off_delta + prx_len. The reader does
//   NOT rely on it -- each delta is resolved independently.
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
  // Lexicographically sorted terms with ascending-docid postings. Used when
  // `term_source` is null (callers that already hold a materialized vector,
  // e.g. unit tests). The writer reads but does not retain these.
  std::vector<TermPostings> terms;
  // Optional streaming term source. When non-null, the writer DRAINS it via
  // SpimiTermBuffer::for_each_term_sorted so that only one term's postings is
  // materialized at a time (avoiding the full TermPostings vector and its
  // second-copy peak). `terms` is ignored when this is set. The buffer is
  // consumed (emptied) by build(); the caller must keep it alive until build()
  // returns and must not reuse it afterwards.
  SpimiTermBuffer* term_source = nullptr;
  // Target DICT block size in bytes; a block is cut once its estimate reaches
  // this. 0 uses kDefaultTargetDictBlockBytes. Smaller values yield more blocks
  // (and a finer-grained sampled-term index).
  uint32_t target_dict_block_bytes = 0;
};

// Builds and holds the section bytes + meta sub-sections for one logical index.
class LogicalIndexWriter {
 public:
  explicit LogicalIndexWriter(const SniiIndexInput& in);

  // Builds DICT blocks, interleaved posting region, sampled-term index, dict
  // directory, stats and xfilter. Must be called once before the accessors below.
  // Returns InvalidArgument on inconsistent input (e.g. norms/doc_count
  // mismatch when scoring is enabled, or non-ascending docids).
  Status build();

  // Section byte lengths (relative; orchestrator decides their absolute offsets).
  // The DICT region and the interleaved posting region are streamed to scratch
  // temp files during build() rather than buffered in RAM, so only their lengths
  // are exposed here; their bytes are emitted via stream_*_into below. norms stays
  // in RAM (1 byte/doc, small) and is returned directly.
  uint64_t dict_region_size() const { return dict_file_.size(); }
  uint64_t posting_region_size() const { return post_file_.size(); }
  const std::vector<uint8_t>& norms_bytes() const { return norms_section_; }
  // Block-split bloom XFilter blob ([28B header][bitset]); empty when no terms.
  const std::vector<uint8_t>& bsbf_bytes() const { return bsbf_bytes_; }
  bool has_bsbf() const { return !bsbf_bytes_.empty(); }

  // Streams each section's bytes into the append-only container writer using a
  // fixed copy buffer (no whole-section reload). Append order/content is
  // unchanged, so the produced container is byte-identical to the in-RAM path.
  Status stream_dict_region_into(snii::io::FileWriter* out) const {
    return dict_file_.stream_into(out);
  }
  Status stream_posting_region_into(snii::io::FileWriter* out) const {
    return post_file_.stream_into(out);
  }

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
  // One DICT block's directory record. The block's serialized bytes are streamed
  // to the dict scratch file as soon as the block is cut (not retained); only this
  // compact summary (offset within the dict region + length + entry count +
  // checksum) is kept to build the DICT block directory at finish_meta time. The
  // absolute file offset is computed as dict_region_offset + rel_offset.
  struct BlockRecord {
    uint64_t rel_offset = 0;  // byte offset of this block within the dict region
    uint64_t length = 0;      // ON-DISK block length (compressed when flags&kZstd)
    uint32_t n_entries = 0;
    uint32_t checksum = 0;    // crc32c of the UNCOMPRESSED block bytes
    uint8_t flags = 0;        // block_ref_flags::* (kZstd when block is compressed)
    uint64_t uncomp_len = 0;  // uncompressed block length (when flags&kZstd)
    std::string first_term;
  };

  // Validates one term's shape (parallel lengths, strictly ascending docids).
  Status validate_term(const TermPostings& tp) const;
  // Iterates terms (from the streaming source or the materialized vector),
  // splitting DICT blocks by target size and filling PODs + blocks_.
  Status build_blocks();
  // Per-term driver shared by both the streaming and materialized paths:
  // validates the term, opens a block if needed, builds its DictEntry, and cuts
  // the block once it reaches the target size. Mutates the running block state.
  struct BlockState;
  // `tp` is taken by mutable reference: the encode FREES the term's large flat
  // arrays (docids/freqs/positions_flat) as soon as they are consumed, so the
  // widest term's source does not co-exist with its encoded output at peak RSS.
  Status process_term(TermPostings& tp, BlockState* st);
  // Builds one DictEntry (inline or pod_ref), growing the posting region as needed.
  Status build_entry(TermPostings& tp, uint64_t frq_base, uint64_t prx_base,
                     snii::format::DictEntry* e);
  // Builds a windowed (df >= kSlimDfThreshold) entry: multi-window + two-level
  // prelude. The term's [prx span][frq span] is appended to the posting region.
  Status build_windowed_entry(TermPostings& tp, uint64_t frq_base,
                              uint64_t prx_base, snii::format::DictEntry* e);
  // Builds a slim (df < kSlimDfThreshold) entry: single window, inline or
  // pod_ref, no prelude.
  Status build_slim_entry(TermPostings& tp, uint64_t frq_base,
                          uint64_t prx_base, snii::format::DictEntry* e);
  // Serializes the current open block, streams its bytes into the dict scratch
  // file, and records a compact directory entry (no block bytes retained).
  Status flush_block(snii::format::DictBlockBuilder* block, std::string first_term);

  uint64_t index_id_;
  std::string index_suffix_;
  snii::format::IndexTier tier_;
  bool has_prx_;
  bool has_freq_;  // tier >= T2: a freq region is encoded per window
  bool has_norms_;
  uint32_t doc_count_;
  const std::vector<TermPostings>& terms_;  // materialized fallback (may be empty)
  SpimiTermBuffer* term_source_;            // streaming source (null => use terms_)
  uint64_t term_count_ = 0;                 // distinct terms actually consumed
  const std::vector<uint8_t>& encoded_norms_;

  uint32_t target_dict_block_bytes_;
  // Large per-term-accumulated sections streamed to scratch temp files instead of
  // being buffered in RAM until finish(). build() seals them; the orchestrator
  // streams them into the container. RAII-cleaned on every path. post_file_ holds
  // the interleaved [prx][frq] posting region (was two separate frq/prx temps);
  // it is ALWAYS opened, even when !has_prx (it then holds frq spans only).
  TempSectionFile dict_file_;
  TempSectionFile post_file_;
  std::vector<uint8_t> norms_section_;

  std::vector<BlockRecord> blocks_;
  std::vector<std::string> sample_first_terms_;
  // One 8-byte XXH64 (seed 0) filter key per term, collected during the build pass
  // so the whole-vocabulary string copy is never retained.
  std::vector<uint64_t> term_hashes_;
  snii::format::StatsBlock stats_;
  std::vector<uint8_t> bsbf_bytes_;  // serialized block-split bloom XFilter section
};

}  // namespace snii::writer
