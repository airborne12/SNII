#include "snii/format/sampled_term_index.h"

#include <algorithm>

#include "snii/encoding/byte_source.h"
#include "snii/encoding/section_framer.h"

namespace snii::format {

namespace {

// term 与 prev 的最长公共前缀长度（与 dict_entry 一致的前缀压缩原语）。
uint32_t common_prefix_len(std::string_view term, std::string_view prev) {
  uint32_t n = 0;
  const uint32_t lim = static_cast<uint32_t>(std::min(term.size(), prev.size()));
  while (n < lim && term[n] == prev[n]) ++n;
  return n;
}

// 写一个 prefix/suffix 压缩的 term key（prefix_len + suffix_len + suffix）。
void write_term_key(std::string_view term, std::string_view prev, ByteSink* sink) {
  const uint32_t prefix = common_prefix_len(term, prev);
  const std::string_view suffix = term.substr(prefix);
  sink->put_varint32(prefix);
  sink->put_varint32(static_cast<uint32_t>(suffix.size()));
  sink->put_bytes(Slice(suffix));
}

// 读一个 prefix/suffix 压缩的 term key，由 prev + suffix 重建到 out。
Status read_term_key(ByteSource* src, std::string_view prev, std::string* out) {
  uint32_t prefix = 0;
  uint32_t suffix_len = 0;
  SNII_RETURN_IF_ERROR(src->get_varint32(&prefix));
  SNII_RETURN_IF_ERROR(src->get_varint32(&suffix_len));
  if (prefix > prev.size()) {
    return Status::Corruption("sampled_term_index: prefix_len 超过 prev_term 长度");
  }
  Slice suffix;
  SNII_RETURN_IF_ERROR(src->get_bytes(suffix_len, &suffix));
  out->assign(prev.substr(0, prefix));
  out->append(reinterpret_cast<const char*>(suffix.data()), suffix.size());
  return Status::OK();
}

}  // namespace

void SampledTermIndexBuilder::add_block_first_term(std::string_view first_term) {
  first_terms_.emplace_back(first_term);
}

void SampledTermIndexBuilder::finish(ByteSink* sink) {
  ByteSink payload;
  payload.put_varint32(static_cast<uint32_t>(first_terms_.size()));
  // min_term / max_term 仅在非空时写出（== 首/尾 sample_term）。
  if (!first_terms_.empty()) {
    write_term_key(first_terms_.front(), std::string_view{}, &payload);
    write_term_key(first_terms_.back(), std::string_view{}, &payload);
    std::string_view prev{};
    for (const auto& t : first_terms_) {
      write_term_key(t, prev, &payload);
      prev = t;
    }
  }
  SectionFramer::write(*sink, static_cast<uint8_t>(SectionType::kSampledTermIndex),
                       payload.view());
}

namespace {

// 从 payload 解析 n_blocks、min/max（未直接使用，仅消费校验对齐）与全部 sample_terms。
Status parse_payload(Slice payload, std::vector<std::string>* terms) {
  ByteSource src(payload);
  uint32_t n_blocks = 0;
  SNII_RETURN_IF_ERROR(src.get_varint32(&n_blocks));
  if (n_blocks == 0) {
    if (!src.eof()) {
      return Status::Corruption("sampled_term_index: 空索引含多余字节");
    }
    terms->clear();
    return Status::OK();
  }

  // min_term / max_term（不直接驱动二分，但需消费并校验结构对齐）。
  std::string min_term;
  std::string max_term;
  SNII_RETURN_IF_ERROR(read_term_key(&src, std::string_view{}, &min_term));
  SNII_RETURN_IF_ERROR(read_term_key(&src, std::string_view{}, &max_term));

  std::vector<std::string> out;
  out.reserve(n_blocks);
  std::string prev;
  for (uint32_t i = 0; i < n_blocks; ++i) {
    std::string term;
    SNII_RETURN_IF_ERROR(read_term_key(&src, prev, &term));
    prev = term;
    out.push_back(std::move(term));
  }
  if (!src.eof()) {
    return Status::Corruption("sampled_term_index: payload 含多余字节");
  }
  if (out.front() != min_term || out.back() != max_term) {
    return Status::Corruption("sampled_term_index: min/max 与 sample_terms 不一致");
  }
  *terms = std::move(out);
  return Status::OK();
}

}  // namespace

Status SampledTermIndexReader::open(Slice section, SampledTermIndexReader* out) {
  if (out == nullptr) {
    return Status::InvalidArgument("sampled_term_index: out 为空");
  }
  ByteSource src(section);
  FramedSection sec;
  SNII_RETURN_IF_ERROR(SectionFramer::read(src, &sec));
  if (sec.type != static_cast<uint8_t>(SectionType::kSampledTermIndex)) {
    return Status::InvalidArgument("sampled_term_index: 非 kSampledTermIndex section");
  }
  *out = SampledTermIndexReader{};
  return parse_payload(sec.payload, &out->sample_terms_);
}

Status SampledTermIndexReader::locate(std::string_view target, bool* maybe_present,
                                      uint32_t* block_ordinal) const {
  if (maybe_present == nullptr || block_ordinal == nullptr) {
    return Status::InvalidArgument("sampled_term_index: 输出指针为空");
  }
  *maybe_present = false;
  *block_ordinal = 0;
  if (sample_terms_.empty()) {
    return Status::OK();  // 空索引：恒越界。
  }
  // target < min_term → 越界。
  if (target < std::string_view(sample_terms_.front())) {
    return Status::OK();
  }
  // target > max_term → 越界。
  if (target > std::string_view(sample_terms_.back())) {
    return Status::OK();
  }
  // 最后一个 sample_term <= target：upper_bound 后回退一位。
  auto it = std::upper_bound(
      sample_terms_.begin(), sample_terms_.end(), target,
      [](std::string_view t, const std::string& s) { return t < std::string_view(s); });
  const auto idx = (it - sample_terms_.begin()) - 1;  // it 必 > begin（已排除 < min）。
  *maybe_present = true;
  *block_ordinal = static_cast<uint32_t>(idx);
  return Status::OK();
}

}  // namespace snii::format
