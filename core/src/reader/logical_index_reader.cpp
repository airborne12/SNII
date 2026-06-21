#include "snii/reader/logical_index_reader.h"

#include <vector>

#include "snii/encoding/zstd_codec.h"
#include "snii/format/dict_block.h"
#include "snii/format/dict_block_directory.h"

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

  // 4. One range read of the DICT block, then anchor search. A zstd-compressed
  //    block is fetched compressed (fewer S3 bytes) and decompressed in RAM
  //    before parsing; the block-level crc32c still validates the inflated bytes.
  std::vector<uint8_t> block_bytes;
  SNII_RETURN_IF_ERROR(reader_->read_at(ref.offset, ref.length, &block_bytes));
  std::vector<uint8_t> inflated;
  Slice block_slice(block_bytes);
  if (ref.flags & snii::format::block_ref_flags::kZstd) {
    // Anti-DoS: uncomp_len comes from on-disk directory bytes and is passed to
    // zstd_decompress, which resizes the output to it BEFORE the block crc can
    // validate the inflated bytes. A crafted ref (uncomp_len = 1<<48) would force
    // a multi-TB allocation. Reject an absurd or zero size up front, mirroring the
    // uncomp_len caps already enforced on the .frq / .prx compressed regions.
    constexpr uint64_t kMaxDictBlockUncompBytes = 256ull * 1024 * 1024;
    if (ref.uncomp_len == 0 || ref.uncomp_len > kMaxDictBlockUncompBytes) {
      return Status::Corruption("dict block: zstd uncomp_len out of range");
    }
    SNII_RETURN_IF_ERROR(snii::zstd_decompress(
        Slice(block_bytes), static_cast<size_t>(ref.uncomp_len), &inflated));
    block_slice = Slice(inflated);
  }
  DictBlockReader br;
  SNII_RETURN_IF_ERROR(
      DictBlockReader::open(block_slice, tier_, has_positions_, &br));

  bool hit = false;
  SNII_RETURN_IF_ERROR(br.find_term(term, &hit, entry));
  if (!hit) return Status::OK();

  *found = true;
  *frq_base = br.frq_base();
  *prx_base = br.prx_base();
  return Status::OK();
}

Status LogicalIndexReader::prefix_terms(std::string_view prefix,
                                        std::vector<PrefixHit>* out) const {
  out->clear();
  if (reader_ == nullptr) return Status::InvalidArgument("logical_index: not opened");

  // Seek the start block: the SampledTermIndex block whose first term <= prefix
  // (terms with `prefix` are >= prefix, so they begin in that block or later). If
  // the prefix sorts before every sample (or is empty), start at block 0.
  uint32_t start = 0;
  if (!prefix.empty()) {
    bool maybe = false;
    uint32_t ordinal = 0;
    SNII_RETURN_IF_ERROR(sti_.locate(prefix, &maybe, &ordinal));
    if (maybe) start = ordinal;
  }

  for (uint32_t ord = start; ord < dbd_.n_blocks(); ++ord) {
    BlockRef ref{};
    SNII_RETURN_IF_ERROR(dbd_.get(ord, &ref));
    std::vector<uint8_t> block_bytes;
    SNII_RETURN_IF_ERROR(reader_->read_at(ref.offset, ref.length, &block_bytes));
    std::vector<uint8_t> inflated;
    Slice block_slice(block_bytes);
    if (ref.flags & snii::format::block_ref_flags::kZstd) {
      constexpr uint64_t kMaxDictBlockUncompBytes = 256ull * 1024 * 1024;
      if (ref.uncomp_len == 0 || ref.uncomp_len > kMaxDictBlockUncompBytes) {
        return Status::Corruption("dict block: zstd uncomp_len out of range");
      }
      SNII_RETURN_IF_ERROR(snii::zstd_decompress(
          Slice(block_bytes), static_cast<size_t>(ref.uncomp_len), &inflated));
      block_slice = Slice(inflated);
    }
    DictBlockReader br;
    SNII_RETURN_IF_ERROR(
        DictBlockReader::open(block_slice, tier_, has_positions_, &br));
    std::vector<DictEntry> entries;
    SNII_RETURN_IF_ERROR(br.decode_all(&entries));

    for (DictEntry& e : entries) {
      const std::string_view t(e.term);
      if (t < prefix) continue;  // not yet at the prefix range
      const bool has_prefix =
          t.size() >= prefix.size() && t.compare(0, prefix.size(), prefix) == 0;
      if (!has_prefix) return Status::OK();  // past the prefix range; sorted -> done
      PrefixHit hit;
      hit.term = e.term;
      hit.entry = std::move(e);
      hit.frq_base = br.frq_base();
      hit.prx_base = br.prx_base();
      out->push_back(std::move(hit));
    }
  }
  return Status::OK();
}

namespace {

// Validates a pod_ref window locator against its POD section and returns the
// absolute window range (after the prelude). Rejects corrupt locators rather
// than letting size_t underflow / uint64 overflow reach read_at.
Status resolve_window(const snii::format::RegionRef& section, uint64_t base,
                      uint64_t off_delta, uint64_t total_len, uint64_t prelude_len,
                      uint64_t* abs_off, uint64_t* len) {
  if (prelude_len > total_len) {
    return Status::Corruption("logical_index: prelude_len exceeds window len");
  }
  const uint64_t in_pod = base + off_delta;
  if (in_pod < base) return Status::Corruption("logical_index: locator overflow");
  if (in_pod > section.length || total_len > section.length - in_pod) {
    return Status::Corruption("logical_index: window past POD section");
  }
  *abs_off = section.offset + in_pod + prelude_len;
  *len = total_len - prelude_len;
  return Status::OK();
}

}  // namespace

Status LogicalIndexReader::resolve_frq_window(const snii::format::DictEntry& entry,
                                              uint64_t frq_base, uint64_t* abs_off,
                                              uint64_t* len) const {
  return resolve_window(section_refs().frq_pod, frq_base, entry.frq_off_delta,
                        entry.frq_len, entry.prelude_len, abs_off, len);
}

Status LogicalIndexReader::resolve_prx_window(const snii::format::DictEntry& entry,
                                              uint64_t prx_base, uint64_t* abs_off,
                                              uint64_t* len) const {
  // .prx windows carry no prelude (prelude_len = 0).
  return resolve_window(section_refs().prx_pod, prx_base, entry.prx_off_delta,
                        entry.prx_len, 0, abs_off, len);
}

}  // namespace snii::reader
