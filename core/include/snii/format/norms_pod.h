#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/encoding/byte_sink.h"

namespace snii::format {

// norms POD：per logical index / field 保存每 doc 1 字节的 encoded doc length，
// 供 BM25 长度归一化（SniiStatsProvider::encoded_norm）按 docid 读取。
//
// on-disk 布局（整体经 SectionFramer 框定，故另带 type+len+crc32c 外壳）：
//   framer payload = [varint64 doc_count][bytes encoded_norm[doc_count]]
//   framer 外壳     = [u8 type][varint64 payload_len][payload][fixed32 crc32c]
// encoded_norm 的编码（length -> 1B）不在本模块范围，这里只做原始字节的存取。
class NormsPodWriter {
 public:
  // 追加下一个 docid 的 encoded_norm（docid 隐式为追加顺序，从 0 递增）。
  void add(uint8_t encoded_norm) { norms_.push_back(encoded_norm); }

  // 已累积的 doc 数（即当前下一个 docid）。
  size_t count() const { return norms_.size(); }

  // 把 [doc_count][bytes] 经 SectionFramer 框定后写入 sink（追加，不清空 sink）。
  void finish(ByteSink* sink) const;

 private:
  std::vector<uint8_t> norms_;
};

// 只读视图：open 时校验 framer crc 与 doc_count/payload 长度自洽，
// 之后 encoded_norm(docid) 为 O(1) 直接索引（零拷贝，借用底层缓冲）。
class NormsPodReader {
 public:
  NormsPodReader() = default;

  // 解析整段（含 framer 外壳）。crc 不匹配 / 截断 / 长度不符返回 Corruption。
  // 成功后 *out 借用 framer_payload 指向的内存，调用方须保证其生命周期。
  static Status open(Slice framed, NormsPodReader* out);

  uint32_t doc_count() const { return doc_count_; }

  // 前置条件（硬契约）：docid < doc_count()。语义同 std::vector::operator[]——
  // 调用方负责保证（docid 来自 SNII 内部解码的受信 postings）。debug 下断言；
  // Release(NDEBUG) 下不做检查。需校验不可信 docid 时用 try_encoded_norm。
  uint8_t encoded_norm(uint32_t docid) const {
    assert(docid < doc_count_);
    return norms_[docid];
  }

  // 受检访问：docid 越界返回 InvalidArgument，不读越界内存。
  Status try_encoded_norm(uint32_t docid, uint8_t* out) const {
    if (docid >= doc_count_) return Status::InvalidArgument("norms: docid out of range");
    *out = norms_[docid];
    return Status::OK();
  }

 private:
  const uint8_t* norms_ = nullptr;
  uint32_t doc_count_ = 0;
};

}  // namespace snii::format
