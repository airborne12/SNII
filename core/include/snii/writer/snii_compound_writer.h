#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "snii/common/status.h"
#include "snii/io/file_writer.h"
#include "snii/writer/logical_index_writer.h"

// SniiCompoundWriter -- orchestrates a single-segment SNII container for one or
// more logical indexes, written front-to-back through an append-only
// io::FileWriter (no seek-back). It resolves all back-references by writing the
// tail meta region and the fixed tail pointer LAST.
//
// CONTAINER LAYOUT PRODUCED (this is the on-disk contract the reader matches):
//   [bootstrap_header]                          (kBootstrapHeaderSize bytes)
//   for each logical index, in add order:
//     [DICT blocks region]   concatenated DICT blocks, split by
//                            target_dict_block_bytes
//     [.frq POD]             concatenated frq prelude+windows / slim windows
//     [.prx POD]             concatenated prx windows (tier>=T2 only)
//   for each logical index, in add order:
//     [norms POD]            NormsPodWriter::finish (scoring only; else absent)
//   [tail_meta_region]       one per_index_meta block per index + directory
//   [tail_pointer]           encode_tail_pointer at EOF
//
// OFFSET CONVENTIONS (ABSOLUTE file offsets unless stated otherwise):
//   - SectionRefs in each per_index_meta record ABSOLUTE file offset+length of
//     that index's dict_region, frq_pod, prx_pod, norms. Absent regions are
//     (0,0) (e.g. .prx and norms for a docs-positions index carry .prx but not
//     norms; null_bitmap is always (0,0) in v1).
//   - DictBlockDirectory entries record each DICT block's ABSOLUTE file offset +
//     length.
//   - A windowed/slim pod_ref entry's absolute .frq offset =
//       section_refs.frq_pod.offset + frq_base + frq_off_delta
//     where frq_base is the .frq-POD-relative running offset captured at the
//     block's open (see logical_index_writer.h). prx follows the identical rule.
//   - tail_pointer.meta_region_offset/length point at the tail_meta_region;
//     hot_off = 0 (no hot region in v1).
namespace snii::writer {

class SniiCompoundWriter {
 public:
  explicit SniiCompoundWriter(snii::io::FileWriter* out);

  // Buffers one logical index: builds its section bytes and meta sub-sections.
  // The actual file writing happens in finish() (single front-to-back pass).
  Status add_logical_index(const SniiIndexInput& in);

  // Writes bootstrap header + all index sections + norms + tail meta region +
  // tail pointer, then finalizes the underlying writer. May be called once.
  Status finish();

 private:
  // Absolute placement of one index's sections, resolved during finish().
  struct Placement {
    uint64_t dict_off = 0;
    uint64_t dict_len = 0;
    uint64_t frq_off = 0;
    uint64_t frq_len = 0;
    uint64_t prx_off = 0;
    uint64_t prx_len = 0;
    uint64_t norms_off = 0;
    uint64_t norms_len = 0;
    uint64_t bsbf_off = 0;
    uint64_t bsbf_len = 0;
  };

  Status write_bootstrap();
  Status write_index_sections(std::vector<Placement>* placements);
  Status write_norms(std::vector<Placement>* placements);
  Status write_tail(const std::vector<Placement>& placements);
  Status append(const std::vector<uint8_t>& bytes);

  snii::io::FileWriter* out_;
  std::vector<std::unique_ptr<LogicalIndexWriter>> indexes_;
  uint64_t cursor_ = 0;  // running absolute write offset
  bool finished_ = false;
};

}  // namespace snii::writer
