#include "snii/reader/logical_index_reader.h"

#include <vector>

#include "snii/format/dict_block.h"

namespace snii::reader {

using snii::format::BlockRef;
using snii::format::DictBlockDirectoryReader;
using snii::format::DictBlockReader;
using snii::format::DictEntry;
using snii::format::IndexTier;
using snii::format::PerIndexMetaReader;
using snii::format::SampledTermIndexReader;
using snii::format::XFilterReader;

Status LogicalIndexReader::open(snii::io::FileReader* file_reader,
                                IndexTier tier, bool has_positions,
                                Slice meta_block, LogicalIndexReader* out) {
  if (file_reader == nullptr) {
    return Status::InvalidArgument("logical_index: null file reader");
  }
  if (out == nullptr) return Status::InvalidArgument("logical_index: null out");

  out->reader_ = file_reader;
  out->tier_ = tier;
  out->has_positions_ = has_positions;

  SNII_RETURN_IF_ERROR(PerIndexMetaReader::open(meta_block, &out->meta_));
  SNII_RETURN_IF_ERROR(SampledTermIndexReader::open(
      out->meta_.sampled_term_index_bytes(), &out->sti_));
  SNII_RETURN_IF_ERROR(DictBlockDirectoryReader::open(
      out->meta_.dict_block_directory_bytes(), &out->dbd_));
  if (out->meta_.has_xfilter()) {
    SNII_RETURN_IF_ERROR(
        XFilterReader::open(out->meta_.xfilter_bytes(), &out->xfilter_));
  }
  return Status::OK();
}

Status LogicalIndexReader::lookup(std::string_view term, bool* found,
                                  DictEntry* entry, uint64_t* frq_base,
                                  uint64_t* prx_base) const {
  *found = false;
  if (reader_ == nullptr) return Status::InvalidArgument("logical_index: not opened");

  // 1. XFilter fast rejection.
  if (meta_.has_xfilter() && !xfilter_.maybe_contains(term)) {
    return Status::OK();
  }

  // 2. SampledTermIndex -> candidate block ordinal.
  bool maybe = false;
  uint32_t ordinal = 0;
  SNII_RETURN_IF_ERROR(sti_.locate(term, &maybe, &ordinal));
  if (!maybe) return Status::OK();

  // 3. DICT block directory -> block range.
  BlockRef ref{};
  SNII_RETURN_IF_ERROR(dbd_.get(ordinal, &ref));

  // 4. One range read of the DICT block, then anchor search.
  std::vector<uint8_t> block_bytes;
  SNII_RETURN_IF_ERROR(reader_->read_at(ref.offset, ref.length, &block_bytes));
  DictBlockReader br;
  SNII_RETURN_IF_ERROR(
      DictBlockReader::open(Slice(block_bytes), tier_, has_positions_, &br));

  bool hit = false;
  SNII_RETURN_IF_ERROR(br.find_term(term, &hit, entry));
  if (!hit) return Status::OK();

  *found = true;
  *frq_base = br.frq_base();
  *prx_base = br.prx_base();
  return Status::OK();
}

}  // namespace snii::reader
