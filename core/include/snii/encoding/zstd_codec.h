#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "snii/common/slice.h"
#include "snii/common/status.h"

namespace snii {

// ZSTD 薄封装。用于 .prx 窗口等大块负载压缩。解压需调用方提供原长（来自块头）。
Status zstd_compress(Slice input, int level, std::vector<uint8_t>* out);
Status zstd_decompress(Slice input, size_t expected_uncomp_len, std::vector<uint8_t>* out);

}  // namespace snii
