#include "doris_english_analyzer.h"

namespace bench {

void doris_english_tokenize(std::string_view text, std::vector<std::string>* out) {
  out->clear();
  doris_english_for_each_token(
      text, [&](std::string_view t) { out->emplace_back(t); });
}

}  // namespace bench
