#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace snii::writer {

// One term's posting list: docids ascending, with parallel freqs and (when
// positions are enabled) per-doc position lists.
struct TermPostings {
  std::string term;
  std::vector<uint32_t> docids;
  std::vector<uint32_t> freqs;
  std::vector<std::vector<uint32_t>> positions;  // empty when positions disabled
};

// In-memory SPIMI (Single-Pass In-Memory Indexing) accumulator for one logical
// index. Records term occurrences and produces lexicographically sorted terms
// with ascending-docid posting lists. (Spill / k-way merge for out-of-core
// builds can wrap this later; the on-disk run format is identical.)
class SpimiTermBuffer {
 public:
  explicit SpimiTermBuffer(bool has_positions);

  // Records one token: `term` occurs in `docid` at `pos`. For a given (term,
  // docid), positions should be added in ascending order (caller's tokenizer order).
  void add_token(std::string_view term, uint32_t docid, uint32_t pos);

  size_t unique_terms() const;
  uint64_t total_tokens() const { return total_tokens_; }
  bool has_positions() const { return has_positions_; }

  // Returns terms sorted lexicographically; each term's docids are ascending.
  // May be called once (it drains/sorts internal state).
  std::vector<TermPostings> finalize_sorted();

 private:
  // term -> docid -> per-doc entry (freq always counted; positions if enabled).
  struct DocEntry {
    uint32_t freq = 0;
    std::vector<uint32_t> positions;
  };

  bool has_positions_;
  uint64_t total_tokens_ = 0;
  std::unordered_map<std::string, std::map<uint32_t, DocEntry>> data_;
};

}  // namespace snii::writer
