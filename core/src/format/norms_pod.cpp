#include "snii/format/norms_pod.h"

#include <limits>

#include "snii/common/slice.h"
#include "snii/encoding/byte_source.h"
#include "snii/encoding/section_framer.h"
#include "snii/format/format_constants.h"

namespace snii::format {

void NormsPodWriter::finish(ByteSink* sink) const {
  // 先拼内层 payload：[varint64 doc_count][raw norm bytes]。
  ByteSink payload;
  payload.put_varint64(norms_.size());
  payload.put_bytes(Slice(norms_));
  // 外层统一交给 framer 加 type+len+crc32c，杜绝手拼校验。
  SectionFramer::write(*sink, static_cast<uint8_t>(SectionType::kStatsBlock),
                       payload.view());
}

Status NormsPodReader::open(Slice framed, NormsPodReader* out) {
  // framer 负责 crc 校验、截断检出与 payload 切片。
  ByteSource src(framed);
  FramedSection sec;
  SNII_RETURN_IF_ERROR(SectionFramer::read(src, &sec));

  // 解析内层 payload：[varint64 doc_count][bytes]。
  ByteSource payload(sec.payload);
  uint64_t doc_count = 0;
  SNII_RETURN_IF_ERROR(payload.get_varint64(&doc_count));
  if (doc_count > std::numeric_limits<uint32_t>::max()) {
    return Status::Corruption("norms POD doc_count overflows uint32");
  }
  // doc_count 必须与剩余字节数严格相等（每 doc 恰 1B）。
  if (payload.remaining() != doc_count) {
    return Status::Corruption("norms POD length mismatch");
  }

  Slice bytes;
  SNII_RETURN_IF_ERROR(payload.get_bytes(static_cast<size_t>(doc_count), &bytes));
  out->doc_count_ = static_cast<uint32_t>(doc_count);
  out->norms_ = bytes.data();
  return Status::OK();
}

}  // namespace snii::format
