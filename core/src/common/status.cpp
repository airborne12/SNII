#include "snii/common/status.h"

#include <array>
#include <cstddef>

namespace snii {
namespace {

// 与 StatusCode 枚举同序的名称表，避免 to_string 写 switch 长链。
constexpr std::array<const char*, 7> kCodeNames = {
    "OK", "Corruption", "NotFound", "InvalidArgument", "IoError", "Unsupported", "Internal"};

}  // namespace

std::string Status::to_string() const {
  std::string out = kCodeNames[static_cast<std::size_t>(code_)];
  if (!message_.empty()) {
    out += ": ";
    out += message_;
  }
  return out;
}

}  // namespace snii
