#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Doris `parser=english` tokenization semantics, implemented ONCE and shared by
// the whole bench so both the SNII and CLucene paths index byte-identical tokens
// (the single source of truth that makes the cross-engine consistency check
// meaningful). A token is a maximal run of ASCII [a-z0-9]; uppercase letters are
// folded to lowercase and every other byte is a separator. A run longer than
// kDorisEnglishMaxTokenLen keeps only its first kDorisEnglishMaxTokenLen bytes
// (the rest of the run is dropped, the run is NOT split into two tokens), which
// defends the docid/vocab space against pathological log blobs.
namespace bench {

constexpr std::size_t kDorisEnglishMaxTokenLen = 64;

// Maps one raw byte to its normalized form: lowercase ASCII letters, digits
// unchanged, everything else a separator (-1). Shared with the streaming corpus
// loader so both paths apply the exact same rule.
inline int doris_english_normalize(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
  if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return c;
  return -1;  // separator
}

// Invokes `emit(std::string_view)` once per token, in order. The string_view is
// backed by a scratch buffer that is reused across tokens, so callers must copy
// (not retain) the view past the callback. No heap allocation per token beyond
// the single growing scratch string.
template <class Emit>
inline void doris_english_for_each_token(std::string_view text, Emit&& emit) {
  std::string scratch;
  scratch.reserve(kDorisEnglishMaxTokenLen);
  for (std::size_t i = 0; i < text.size(); ++i) {
    const int nc = doris_english_normalize(static_cast<unsigned char>(text[i]));
    if (nc < 0) {
      if (!scratch.empty()) {
        emit(std::string_view(scratch));
        scratch.clear();
      }
    } else if (scratch.size() < kDorisEnglishMaxTokenLen) {
      scratch.push_back(static_cast<char>(nc));
    }
    // else: inside an over-long run past the cap -- drop until next separator.
  }
  if (!scratch.empty()) emit(std::string_view(scratch));
}

// Collects all tokens of `text` into `*out` (cleared first). Convenience form
// for tests and callers that want a materialized vector.
void doris_english_tokenize(std::string_view text, std::vector<std::string>* out);

}  // namespace bench
