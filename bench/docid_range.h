#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace bench {

inline uint32_t require_uint32_doc_count(uint64_t n, const char* owner) {
  if (n > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error(std::string(owner) +
                             ": doc_count exceeds uint32_t docid range");
  }
  return static_cast<uint32_t>(n);
}

}  // namespace bench
