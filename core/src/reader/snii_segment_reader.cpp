#include "snii/reader/snii_segment_reader.h"

#include <vector>

#include "snii/format/bootstrap_header.h"
#include "snii/format/format_constants.h"
#include "snii/format/per_index_meta.h"
#include "snii/format/stats_block.h"
#include "snii/format/tail_pointer.h"

namespace snii::reader {

using snii::format::BootstrapHeader;
using snii::format::IndexTier;
using snii::format::PerIndexMetaReader;
using snii::format::StatsBlock;
using snii::format::TailMetaRegionReader;
using snii::format::TailPointer;

namespace {

// Reads the bootstrap header from the front of the file and validates it.
Status ReadBootstrap(snii::io::FileReader* reader, BootstrapHeader* bh) {
  std::vector<uint8_t> buf;
  SNII_RETURN_IF_ERROR(
      reader->read_at(0, snii::format::kBootstrapHeaderSize, &buf));
  return snii::format::decode_bootstrap_header(Slice(buf), bh);
}

// Reads the fixed tail pointer (last tail_pointer_size() bytes) of the file.
Status ReadTailPointer(snii::io::FileReader* reader, TailPointer* tp) {
  const size_t tp_size = snii::format::tail_pointer_size();
  const uint64_t total = reader->size();
  if (total < tp_size) {
    return Status::Corruption("segment: file smaller than tail pointer");
  }
  std::vector<uint8_t> buf;
  SNII_RETURN_IF_ERROR(reader->read_at(total - tp_size, tp_size, &buf));
  return snii::format::decode_tail_pointer(Slice(buf), tp);
}

}  // namespace

Status SniiSegmentReader::open(snii::io::FileReader* reader,
                               SniiSegmentReader* out) {
  if (reader == nullptr) return Status::InvalidArgument("segment: null reader");
  if (out == nullptr) return Status::InvalidArgument("segment: null out");

  BootstrapHeader bh;
  SNII_RETURN_IF_ERROR(ReadBootstrap(reader, &bh));

  TailPointer tp;
  SNII_RETURN_IF_ERROR(ReadTailPointer(reader, &tp));
  if (tp.meta_region_length == 0) {
    return Status::Corruption("segment: empty tail meta region");
  }

  out->reader_ = reader;
  SNII_RETURN_IF_ERROR(reader->read_at(tp.meta_region_offset,
                                       tp.meta_region_length,
                                       &out->meta_region_));
  return TailMetaRegionReader::open(Slice(out->meta_region_),
                                    &out->region_reader_);
}

Status SniiSegmentReader::open_index(uint64_t index_id, std::string_view suffix,
                                     LogicalIndexReader* out) const {
  if (out == nullptr) return Status::InvalidArgument("segment: null index out");
  if (reader_ == nullptr) return Status::InvalidArgument("segment: not opened");

  bool found = false;
  Slice meta_bytes;
  SNII_RETURN_IF_ERROR(
      region_reader_.find(index_id, suffix, &found, &meta_bytes));
  if (!found) return Status::NotFound("segment: logical index not found");

  // Determine tier / positions capability from the per-index meta. v1 stores
  // positions whenever the index is a docs-positions(+scoring) index. The .prx
  // POD is only populated by windowed (high-df) terms, so for a scoring index
  // whose terms are all slim (positions inlined in the DictEntry) prx_pod can be
  // empty while every DICT block still carries the positions flag. A present
  // norms POD implies the scoring config, which always has positions; treat that
  // as positions-capable so block decoding stays consistent with the writer.
  PerIndexMetaReader meta;
  SNII_RETURN_IF_ERROR(PerIndexMetaReader::open(meta_bytes, &meta));
  const bool has_norms = meta.section_refs().norms.length > 0;
  const bool has_positions =
      meta.section_refs().prx_pod.length > 0 || has_norms;
  const IndexTier tier = has_norms
                             ? IndexTier::kT3
                             : (has_positions ? IndexTier::kT2 : IndexTier::kT1);

  return LogicalIndexReader::open(reader_, tier, has_positions, meta_bytes, out);
}

}  // namespace snii::reader
