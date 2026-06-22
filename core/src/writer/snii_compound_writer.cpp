#include "snii/writer/snii_compound_writer.h"

#include <utility>

#include "snii/common/slice.h"
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/crc32c.h"
#include "snii/format/bootstrap_header.h"
#include "snii/format/per_index_meta.h"  // SectionRefs
#include "snii/format/tail_meta_region.h"
#include "snii/format/tail_pointer.h"

namespace snii::writer {

using snii::format::BootstrapHeader;
using snii::format::SectionRefs;
using snii::format::TailMetaRegionBuilder;
using snii::format::TailPointer;

SniiCompoundWriter::SniiCompoundWriter(snii::io::FileWriter* out) : out_(out) {}

Status SniiCompoundWriter::append(const std::vector<uint8_t>& bytes) {
  if (bytes.empty()) return Status::OK();
  SNII_RETURN_IF_ERROR(out_->append(Slice(bytes)));
  cursor_ += bytes.size();
  return Status::OK();
}

Status SniiCompoundWriter::add_logical_index(const SniiIndexInput& in) {
  if (out_ == nullptr) return Status::InvalidArgument("compound: null file writer");
  if (finished_) return Status::Internal("compound: add after finish");
  auto liw = std::make_unique<LogicalIndexWriter>(in);
  SNII_RETURN_IF_ERROR(liw->build());
  indexes_.push_back(std::move(liw));
  return Status::OK();
}

Status SniiCompoundWriter::write_bootstrap() {
  BootstrapHeader bh;
  bh.tail_pointer_size = static_cast<uint8_t>(snii::format::tail_pointer_size());
  ByteSink sink;
  SNII_RETURN_IF_ERROR(snii::format::encode_bootstrap_header(bh, &sink));
  return append(sink.buffer());
}

// Streams each index's posting region then dict_region (in add order) from its
// scratch temp files into the container, capturing the absolute offset/length of
// each into placements[i]. The posting region (the large append-only stream) is
// emitted BEFORE the compact DICT trailer per index. Only the peak RSS drops; the
// bytes within each region are byte-identical to the prior in-RAM path.
Status SniiCompoundWriter::write_index_sections(std::vector<Placement>* placements) {
  for (size_t i = 0; i < indexes_.size(); ++i) {
    Placement& p = (*placements)[i];
    const LogicalIndexWriter& w = *indexes_[i];

    p.post_off = cursor_;
    SNII_RETURN_IF_ERROR(w.stream_posting_region_into(out_));
    cursor_ += w.posting_region_size();
    p.post_len = cursor_ - p.post_off;

    p.dict_off = cursor_;
    SNII_RETURN_IF_ERROR(w.stream_dict_region_into(out_));
    cursor_ += w.dict_region_size();
    p.dict_len = cursor_ - p.dict_off;
  }
  return Status::OK();
}

// Writes each index's norms POD (in add order), after all dict/frq/prx regions.
Status SniiCompoundWriter::write_norms(std::vector<Placement>* placements) {
  for (size_t i = 0; i < indexes_.size(); ++i) {
    const LogicalIndexWriter& w = *indexes_[i];
    if (!w.has_norms() || w.norms_bytes().empty()) continue;
    Placement& p = (*placements)[i];
    p.norms_off = cursor_;
    SNII_RETURN_IF_ERROR(append(w.norms_bytes()));
    p.norms_len = cursor_ - p.norms_off;
  }
  // Block-split bloom XFilter sections (one physical section per index, after the
  // norms), so the reader can probe a single 32-byte block on demand at open.
  for (size_t i = 0; i < indexes_.size(); ++i) {
    const LogicalIndexWriter& w = *indexes_[i];
    if (!w.has_bsbf()) continue;
    Placement& p = (*placements)[i];
    p.bsbf_off = cursor_;
    SNII_RETURN_IF_ERROR(append(w.bsbf_bytes()));
    p.bsbf_len = cursor_ - p.bsbf_off;
  }
  return Status::OK();
}

Status SniiCompoundWriter::write_tail(const std::vector<Placement>& placements) {
  TailMetaRegionBuilder region;
  for (size_t i = 0; i < indexes_.size(); ++i) {
    const LogicalIndexWriter& w = *indexes_[i];
    const Placement& p = placements[i];

    SectionRefs refs;
    refs.dict_region = {p.dict_off, p.dict_len};
    refs.posting_region = {p.post_off, p.post_len};
    refs.norms = {p.norms_off, p.norms_len};
    refs.null_bitmap = {0, 0};
    refs.bsbf = {p.bsbf_off, p.bsbf_len};

    ByteSink meta;
    SNII_RETURN_IF_ERROR(w.finish_meta(refs, p.dict_off, &meta));
    region.add_index(w.index_id(), w.index_suffix(), meta.view());
  }

  ByteSink region_sink;
  region.finish(&region_sink);
  const uint64_t region_off = cursor_;
  SNII_RETURN_IF_ERROR(append(region_sink.buffer()));
  const uint64_t region_len = cursor_ - region_off;

  TailPointer tp;
  tp.meta_region_offset = region_off;
  tp.meta_region_length = region_len;
  tp.hot_off = 0;
  tp.meta_region_checksum = snii::crc32c(region_sink.view());
  // Reserved: the bootstrap header carries (and decode_bootstrap_header verifies) its
  // OWN internal crc32c, so a tail-pointer copy is redundant. Left 0 until a cross-
  // region check needs it; the tail pointer's own tail_checksum still covers this
  // field's bytes.
  tp.bootstrap_header_checksum = 0;
  ByteSink tail_sink;
  SNII_RETURN_IF_ERROR(snii::format::encode_tail_pointer(tp, &tail_sink));
  return append(tail_sink.buffer());
}

Status SniiCompoundWriter::finish() {
  if (out_ == nullptr) return Status::InvalidArgument("compound: null file writer");
  if (finished_) return Status::Internal("compound: finish called twice");
  finished_ = true;

  std::vector<Placement> placements(indexes_.size());
  SNII_RETURN_IF_ERROR(write_bootstrap());
  SNII_RETURN_IF_ERROR(write_index_sections(&placements));
  SNII_RETURN_IF_ERROR(write_norms(&placements));
  SNII_RETURN_IF_ERROR(write_tail(placements));
  return out_->finalize();
}

}  // namespace snii::writer
