# SNII Phase 1 — 基座（脚手架 + L0 编码/序列化原语）实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 搭好 core/bench 隔离的工程骨架，交付 L0 编码层：Status 错误处理、Slice、varint、ByteSink/ByteSource 序列化游标、crc32c、SectionFramer 统一框定、zstd 包装、PFOR 整数块编解码——全部 round-trip 单测覆盖。

**Architecture:** 单一 `ByteSink`(append-only 写) / `ByteSource`(slice 读) 游标承载所有原语，上层 section 一律复用，杜绝重复手拼。`SectionFramer` 统一 `[type][len][payload][crc32c]` 框定与校验。codec 经显式接口暴露，不在调用点写分支长链。

**Tech Stack:** C++20、CMake ≥3.29、ldb_toolchain g++ 15.1、系统 libzstd、doris-thirdparty 预编译 gtest。

## Global Constraints

- 语言：C++20；编译器 `/mnt/disk1/jiangkai/workspace/bin/ldb_toolchain/bin/g++`（cmake 同目录）。
- 依赖单向铁律：`core` 绝不 `#include` 任何 bench/clucene 头；`core` 可单独 `cmake --build` 并跑全部单测。
- 函数默认 ≤50 行，硬上限 ≤80；if-else 链 → 多态/策略/dispatch；嵌套 ≤3 层；early-return。
- 全程 `Status` 返回，无静默失败；RAII，无裸 `new`；头文件禁 `using namespace`。
- DRY：公共逻辑下沉，零复制粘贴。文件单一职责，200–400 行典型，≤800。
- TDD：每模块先写失败测试（RED）→ 最小实现（GREEN）→ 重构（REFACTOR）；≥80% 覆盖。
- 命名空间统一 `snii`；公共头置于 `core/include/snii/`，实现置于 `core/src/`，测试置于 `core/tests/`。
- 字节序：所有多字节定长字段一律小端（little-endian）；varint 为 LEB128。

---

### Task 0: 工程脚手架与测试基座

**Files:**
- Create: `CMakeLists.txt`（顶层）
- Create: `core/CMakeLists.txt`
- Create: `core/tests/CMakeLists.txt`
- Create: `core/include/snii/version.h`
- Create: `core/tests/smoke_test.cpp`
- Create: `cmake/FindGTestPrebuilt.cmake`
- Create: `.gitignore`

**Interfaces:**
- Produces: `libsnii` 静态库 target；`snii_core_tests` 测试可执行；`SNII_VERSION_STRING` 宏。

- [ ] **Step 1: 定位预编译 gtest**

Run: `ls /mnt/disk1/jiangkai/workspace/src/doris-clean/thirdparty/installed/lib/libgtest*.a /mnt/disk1/jiangkai/workspace/src/doris-clean/thirdparty/installed/include/gtest/gtest.h`
Expected: 列出 `libgtest.a`（与 `libgtest_main.a`）与头文件。若缺失，改用其它 doris build（`doris-master`/`doris`）的 thirdparty installed。

- [ ] **Step 2: 写 `.gitignore`**

```gitignore
/build/
/build-*/
*.o
*.a
compile_commands.json
.cache/
# 凭证 / 本地配置，绝不入库
*.oss.env
.env
*.local.env
core/tests/testdata/tmp/
```

- [ ] **Step 3: 写 `core/include/snii/version.h`**

```cpp
#pragma once
#define SNII_VERSION_MAJOR 0
#define SNII_VERSION_MINOR 1
#define SNII_VERSION_STRING "0.1.0"
```

- [ ] **Step 4: 写 `cmake/FindGTestPrebuilt.cmake`**

```cmake
# 定位 doris-thirdparty 预编译 gtest，避免联网拉取。
set(_dt_candidates
  "/mnt/disk1/jiangkai/workspace/src/doris-clean/thirdparty/installed"
  "/mnt/disk1/jiangkai/workspace/src/doris-master/thirdparty/installed"
  "/mnt/disk1/jiangkai/workspace/src/doris/thirdparty/installed")
foreach(_dt ${_dt_candidates})
  if(EXISTS "${_dt}/lib/libgtest.a")
    set(GTEST_ROOT "${_dt}" CACHE PATH "gtest root")
    break()
  endif()
endforeach()
if(NOT GTEST_ROOT)
  message(FATAL_ERROR "prebuilt gtest not found; set GTEST_ROOT manually")
endif()
add_library(snii_gtest INTERFACE)
target_include_directories(snii_gtest INTERFACE "${GTEST_ROOT}/include")
target_link_libraries(snii_gtest INTERFACE
  "${GTEST_ROOT}/lib/libgtest.a" "${GTEST_ROOT}/lib/libgtest_main.a" pthread)
```

- [ ] **Step 5: 写顶层 `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.29)
project(snii LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wpedantic -Werror=return-type)
option(SNII_BUILD_TESTS "Build core unit tests" ON)
option(SNII_BUILD_BENCH "Build benchmark (depends on clucene)" OFF)
option(SNII_WITH_S3 "Build S3 backend (depends on aws-sdk-cpp)" OFF)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
add_subdirectory(core)
if(SNII_BUILD_BENCH)
  add_subdirectory(bench)
endif()
```

- [ ] **Step 6: 写 `core/CMakeLists.txt`**

```cmake
add_library(snii STATIC)
target_include_directories(snii PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_sources(snii PRIVATE
  # 源文件随任务追加
)
set_target_properties(snii PROPERTIES LINKER_LANGUAGE CXX)
# 占位空源，待 Task 1 起替换
target_sources(snii PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/placeholder.cpp)
if(SNII_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
```

并创建 `core/src/placeholder.cpp`：
```cpp
namespace snii { int snii_placeholder() { return 0; } }
```

- [ ] **Step 7: 写 `core/tests/CMakeLists.txt`**

```cmake
find_package(GTestPrebuilt REQUIRED)
add_executable(snii_core_tests
  smoke_test.cpp
)
target_link_libraries(snii_core_tests PRIVATE snii snii_gtest)
include(GoogleTest)
gtest_discover_tests(snii_core_tests)
```

- [ ] **Step 8: 写 `core/tests/smoke_test.cpp`**

```cpp
#include <gtest/gtest.h>
#include "snii/version.h"
TEST(Smoke, VersionDefined) {
  EXPECT_STREQ(SNII_VERSION_STRING, "0.1.0");
}
```

- [ ] **Step 9: 配置并编译运行**

Run:
```bash
cd /mnt/disk1/jiangkai/workspace/src/SNII
cmake -S . -B build -DCMAKE_C_COMPILER=/mnt/disk1/jiangkai/workspace/bin/ldb_toolchain/bin/gcc -DCMAKE_CXX_COMPILER=/mnt/disk1/jiangkai/workspace/bin/ldb_toolchain/bin/g++
cmake --build build -j
ctest --test-dir build --output-on-failure
```
Expected: 编译通过；`Smoke.VersionDefined` PASS。

- [ ] **Step 10: Commit**

```bash
git checkout -b feat/snii-phase1
git add CMakeLists.txt core cmake .gitignore docs
git commit -m "chore: scaffold core/bench-separated cmake project with gtest"
```

---

### Task 1: Status 错误处理

**Files:**
- Create: `core/include/snii/common/status.h`
- Create: `core/src/common/status.cpp`
- Test: `core/tests/common/status_test.cpp`

**Interfaces:**
- Produces:
  - `enum class StatusCode { kOk, kCorruption, kNotFound, kInvalidArgument, kIoError, kUnsupported, kInternal };`
  - `class Status { static Status OK(); static Status Corruption(std::string); ... bool ok() const; StatusCode code() const; const std::string& message() const; std::string to_string() const; };`
  - 宏 `SNII_RETURN_IF_ERROR(expr)`：对返回 `Status` 的表达式短路返回。

- [ ] **Step 1: 写失败测试 `core/tests/common/status_test.cpp`**

```cpp
#include <gtest/gtest.h>
#include "snii/common/status.h"
using snii::Status;
using snii::StatusCode;

TEST(Status, OkIsOk) {
  Status s = Status::OK();
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::kOk);
}
TEST(Status, CorruptionCarriesMessage) {
  Status s = Status::Corruption("bad crc");
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::kCorruption);
  EXPECT_NE(s.to_string().find("bad crc"), std::string::npos);
}
TEST(Status, ReturnIfErrorShortCircuits) {
  auto fn = []() -> Status {
    SNII_RETURN_IF_ERROR(Status::NotFound("x"));
    return Status::OK();
  };
  EXPECT_EQ(fn().code(), StatusCode::kNotFound);
}
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build -j && ctest --test-dir build -R Status` → 编译失败（头不存在）。

- [ ] **Step 3: 实现 `status.h`**

```cpp
#pragma once
#include <string>
#include <utility>
namespace snii {
enum class StatusCode {
  kOk, kCorruption, kNotFound, kInvalidArgument, kIoError, kUnsupported, kInternal
};
class Status {
 public:
  Status() : code_(StatusCode::kOk) {}
  static Status OK() { return Status(); }
  static Status Corruption(std::string m) { return Status(StatusCode::kCorruption, std::move(m)); }
  static Status NotFound(std::string m) { return Status(StatusCode::kNotFound, std::move(m)); }
  static Status InvalidArgument(std::string m) { return Status(StatusCode::kInvalidArgument, std::move(m)); }
  static Status IoError(std::string m) { return Status(StatusCode::kIoError, std::move(m)); }
  static Status Unsupported(std::string m) { return Status(StatusCode::kUnsupported, std::move(m)); }
  static Status Internal(std::string m) { return Status(StatusCode::kInternal, std::move(m)); }
  bool ok() const { return code_ == StatusCode::kOk; }
  StatusCode code() const { return code_; }
  const std::string& message() const { return message_; }
  std::string to_string() const;
 private:
  Status(StatusCode c, std::string m) : code_(c), message_(std::move(m)) {}
  StatusCode code_;
  std::string message_;
};
}  // namespace snii

#define SNII_RETURN_IF_ERROR(expr)                 \
  do {                                             \
    ::snii::Status _s = (expr);                    \
    if (!_s.ok()) return _s;                        \
  } while (0)
```

- [ ] **Step 4: 实现 `status.cpp`（to_string 用 dispatch 表，不写 switch 长链）**

```cpp
#include "snii/common/status.h"
#include <array>
namespace snii {
namespace {
constexpr std::array<const char*, 7> kNames = {
  "OK", "Corruption", "NotFound", "InvalidArgument", "IoError", "Unsupported", "Internal"};
}
std::string Status::to_string() const {
  std::string out = kNames[static_cast<size_t>(code_)];
  if (!message_.empty()) { out += ": "; out += message_; }
  return out;
}
}  // namespace snii
```

- [ ] **Step 5: 接入构建**：在 `core/CMakeLists.txt` 的 `target_sources(snii ...)` 增 `src/common/status.cpp`，移除 placeholder；`core/tests/CMakeLists.txt` 测试列表增 `common/status_test.cpp`。

- [ ] **Step 6: 运行确认通过**

Run: `cmake --build build -j && ctest --test-dir build -R Status --output-on-failure` → 全 PASS。

- [ ] **Step 7: Commit**

```bash
git add core docs && git commit -m "feat(core): add Status error type with SNII_RETURN_IF_ERROR"
```

---

### Task 2: Slice 只读字节视图

**Files:**
- Create: `core/include/snii/common/slice.h`
- Test: `core/tests/common/slice_test.cpp`

**Interfaces:**
- Produces: `class Slice { const uint8_t* data(); size_t size(); bool empty(); uint8_t operator[](size_t); Slice subslice(size_t off, size_t n); };` 构造自 `(const uint8_t*, size_t)` 与 `std::string_view`/`std::vector<uint8_t>`。

- [ ] **Step 1: 写失败测试**

```cpp
#include <gtest/gtest.h>
#include "snii/common/slice.h"
using snii::Slice;
TEST(Slice, BasicAccess) {
  const uint8_t buf[] = {1,2,3,4};
  Slice s(buf, 4);
  EXPECT_EQ(s.size(), 4u);
  EXPECT_EQ(s[2], 3u);
  Slice sub = s.subslice(1, 2);
  EXPECT_EQ(sub.size(), 2u);
  EXPECT_EQ(sub[0], 2u);
}
TEST(Slice, EmptyDefault) {
  Slice s;
  EXPECT_TRUE(s.empty());
}
```

- [ ] **Step 2: 运行确认失败** → 头不存在。

- [ ] **Step 3: 实现 `slice.h`**

```cpp
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string_view>
#include <cassert>
namespace snii {
class Slice {
 public:
  Slice() = default;
  Slice(const uint8_t* d, size_t n) : data_(d), size_(n) {}
  explicit Slice(const std::vector<uint8_t>& v) : data_(v.data()), size_(v.size()) {}
  explicit Slice(std::string_view sv)
      : data_(reinterpret_cast<const uint8_t*>(sv.data())), size_(sv.size()) {}
  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  uint8_t operator[](size_t i) const { assert(i < size_); return data_[i]; }
  Slice subslice(size_t off, size_t n) const {
    assert(off + n <= size_);
    return Slice(data_ + off, n);
  }
 private:
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
};
}  // namespace snii
```

- [ ] **Step 4: 接入构建**（仅头，测试列表增 `common/slice_test.cpp`）。
- [ ] **Step 5: 运行确认通过**。
- [ ] **Step 6: Commit**：`git commit -m "feat(core): add Slice read-only byte view"`

---

### Task 3: varint (LEB128) 编解码自由函数

**Files:**
- Create: `core/include/snii/encoding/varint.h`
- Create: `core/src/encoding/varint.cpp`
- Test: `core/tests/encoding/varint_test.cpp`

**Interfaces:**
- Produces:
  - `size_t encode_varint32(uint32_t v, uint8_t* out);` / `encode_varint64`（out 需 ≥10 字节，返回写入字节数）
  - `Status decode_varint32(const uint8_t* p, const uint8_t* end, uint32_t* v, const uint8_t** next);` / `decode_varint64`
  - `uint64_t zigzag_encode(int64_t);` / `int64_t zigzag_decode(uint64_t);`
  - `size_t varint_len(uint64_t);`

- [ ] **Step 1: 写失败测试**

```cpp
#include <gtest/gtest.h>
#include "snii/encoding/varint.h"
using namespace snii;
TEST(Varint, RoundTrip32) {
  for (uint32_t v : {0u, 1u, 127u, 128u, 300u, 16384u, 0xFFFFFFFFu}) {
    uint8_t buf[10]; size_t n = encode_varint32(v, buf);
    EXPECT_EQ(n, varint_len(v));
    uint32_t out; const uint8_t* next;
    ASSERT_TRUE(decode_varint32(buf, buf + n, &out, &next).ok());
    EXPECT_EQ(out, v);
    EXPECT_EQ(next, buf + n);
  }
}
TEST(Varint, TruncatedFails) {
  uint8_t buf[1] = {0x80};  // 续位但无后续
  uint32_t out; const uint8_t* next;
  EXPECT_FALSE(decode_varint32(buf, buf + 1, &out, &next).ok());
}
TEST(Varint, ZigzagRoundTrip) {
  for (int64_t v : {0LL, -1LL, 1LL, -1000LL, 1000LL, INT64_MIN, INT64_MAX})
    EXPECT_EQ(zigzag_decode(zigzag_encode(v)), v);
}
```

- [ ] **Step 2: 运行确认失败**。
- [ ] **Step 3: 实现 `varint.h` + `varint.cpp`**

`varint.h`:
```cpp
#pragma once
#include <cstdint>
#include <cstddef>
#include "snii/common/status.h"
namespace snii {
size_t varint_len(uint64_t v);
size_t encode_varint32(uint32_t v, uint8_t* out);
size_t encode_varint64(uint64_t v, uint8_t* out);
Status decode_varint32(const uint8_t* p, const uint8_t* end, uint32_t* v, const uint8_t** next);
Status decode_varint64(const uint8_t* p, const uint8_t* end, uint64_t* v, const uint8_t** next);
inline uint64_t zigzag_encode(int64_t v) { return (static_cast<uint64_t>(v) << 1) ^ (v >> 63); }
inline int64_t zigzag_decode(uint64_t v) { return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1); }
}  // namespace snii
```

`varint.cpp`（decode 用统一 helper，32 位复用 64 位实现，DRY）:
```cpp
#include "snii/encoding/varint.h"
namespace snii {
size_t varint_len(uint64_t v) {
  size_t n = 1;
  while (v >= 0x80) { v >>= 7; ++n; }
  return n;
}
size_t encode_varint64(uint64_t v, uint8_t* out) {
  size_t i = 0;
  while (v >= 0x80) { out[i++] = static_cast<uint8_t>(v) | 0x80; v >>= 7; }
  out[i++] = static_cast<uint8_t>(v);
  return i;
}
size_t encode_varint32(uint32_t v, uint8_t* out) { return encode_varint64(v, out); }
Status decode_varint64(const uint8_t* p, const uint8_t* end, uint64_t* v, const uint8_t** next) {
  uint64_t result = 0; int shift = 0;
  while (p < end) {
    uint8_t b = *p++;
    result |= static_cast<uint64_t>(b & 0x7F) << shift;
    if ((b & 0x80) == 0) { *v = result; *next = p; return Status::OK(); }
    shift += 7;
    if (shift >= 64) return Status::Corruption("varint64 overflow");
  }
  return Status::Corruption("varint truncated");
}
Status decode_varint32(const uint8_t* p, const uint8_t* end, uint32_t* v, const uint8_t** next) {
  uint64_t tmp; SNII_RETURN_IF_ERROR(decode_varint64(p, end, &tmp, next));
  if (tmp > 0xFFFFFFFFu) return Status::Corruption("varint32 overflow");
  *v = static_cast<uint32_t>(tmp);
  return Status::OK();
}
}  // namespace snii
```

- [ ] **Step 4: 接入构建** `src/encoding/varint.cpp` + 测试 `encoding/varint_test.cpp`。
- [ ] **Step 5: 运行确认通过**。
- [ ] **Step 6: Commit**：`git commit -m "feat(core): add LEB128 varint + zigzag codec"`

---

### Task 4: ByteSink（append-only 写游标）

**Files:**
- Create: `core/include/snii/encoding/byte_sink.h`
- Create: `core/src/encoding/byte_sink.cpp`
- Test: `core/tests/encoding/byte_sink_test.cpp`

**Interfaces:**
- Produces: `class ByteSink { void put_u8(uint8_t); void put_fixed32(uint32_t); void put_fixed64(uint64_t); void put_varint32(uint32_t); void put_varint64(uint64_t); void put_zigzag(int64_t); void put_bytes(Slice); size_t size() const; const std::vector<uint8_t>& buffer() const; Slice view() const; };`（小端 fixed）。

- [ ] **Step 1: 写失败测试**

```cpp
#include <gtest/gtest.h>
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/varint.h"
using namespace snii;
TEST(ByteSink, Fixed32LittleEndian) {
  ByteSink s; s.put_fixed32(0x04030201u);
  ASSERT_EQ(s.size(), 4u);
  const auto& b = s.buffer();
  EXPECT_EQ(b[0], 0x01); EXPECT_EQ(b[3], 0x04);
}
TEST(ByteSink, VarintThenBytes) {
  ByteSink s; s.put_varint32(300); 
  const uint8_t payload[] = {0xAA, 0xBB};
  s.put_bytes(Slice(payload, 2));
  EXPECT_EQ(s.size(), varint_len(300) + 2);
}
```

- [ ] **Step 2: 运行确认失败**。
- [ ] **Step 3: 实现 `byte_sink.h` + `.cpp`**

`byte_sink.h`:
```cpp
#pragma once
#include <cstdint>
#include <vector>
#include "snii/common/slice.h"
namespace snii {
class ByteSink {
 public:
  void put_u8(uint8_t v) { buf_.push_back(v); }
  void put_fixed32(uint32_t v);
  void put_fixed64(uint64_t v);
  void put_varint32(uint32_t v);
  void put_varint64(uint64_t v);
  void put_zigzag(int64_t v);
  void put_bytes(Slice s);
  size_t size() const { return buf_.size(); }
  const std::vector<uint8_t>& buffer() const { return buf_; }
  Slice view() const { return Slice(buf_); }
 private:
  std::vector<uint8_t> buf_;
};
}  // namespace snii
```

`byte_sink.cpp`:
```cpp
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/varint.h"
namespace snii {
void ByteSink::put_fixed32(uint32_t v) {
  for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void ByteSink::put_fixed64(uint64_t v) {
  for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void ByteSink::put_varint32(uint32_t v) {
  uint8_t tmp[5]; size_t n = encode_varint32(v, tmp);
  buf_.insert(buf_.end(), tmp, tmp + n);
}
void ByteSink::put_varint64(uint64_t v) {
  uint8_t tmp[10]; size_t n = encode_varint64(v, tmp);
  buf_.insert(buf_.end(), tmp, tmp + n);
}
void ByteSink::put_zigzag(int64_t v) { put_varint64(zigzag_encode(v)); }
void ByteSink::put_bytes(Slice s) { buf_.insert(buf_.end(), s.data(), s.data() + s.size()); }
}  // namespace snii
```

- [ ] **Step 4: 接入构建** + **Step 5: 运行确认通过** + **Step 6: Commit** `git commit -m "feat(core): add ByteSink append-only write cursor"`

---

### Task 5: ByteSource（slice 读游标）

**Files:**
- Create: `core/include/snii/encoding/byte_source.h`
- Create: `core/src/encoding/byte_source.cpp`
- Test: `core/tests/encoding/byte_source_test.cpp`

**Interfaces:**
- Produces: `class ByteSource { explicit ByteSource(Slice); Status get_u8(uint8_t*); Status get_fixed32(uint32_t*); Status get_fixed64(uint64_t*); Status get_varint32(uint32_t*); Status get_varint64(uint64_t*); Status get_zigzag(int64_t*); Status get_bytes(size_t n, Slice* out); size_t remaining() const; size_t position() const; bool eof() const; };`

- [ ] **Step 1: 写失败测试（含越界）**

```cpp
#include <gtest/gtest.h>
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/byte_source.h"
using namespace snii;
TEST(ByteSource, RoundTripWithSink) {
  ByteSink s; s.put_fixed32(0xDEADBEEF); s.put_varint64(123456789); s.put_zigzag(-42);
  ByteSource src(s.view());
  uint32_t a; uint64_t b; int64_t c;
  ASSERT_TRUE(src.get_fixed32(&a).ok()); EXPECT_EQ(a, 0xDEADBEEFu);
  ASSERT_TRUE(src.get_varint64(&b).ok()); EXPECT_EQ(b, 123456789u);
  ASSERT_TRUE(src.get_zigzag(&c).ok()); EXPECT_EQ(c, -42);
  EXPECT_TRUE(src.eof());
}
TEST(ByteSource, OverrunFails) {
  uint8_t one[1] = {0x01};
  ByteSource src(Slice(one, 1));
  uint32_t a;
  EXPECT_FALSE(src.get_fixed32(&a).ok());
}
```

- [ ] **Step 2: 运行确认失败**。
- [ ] **Step 3: 实现**

`byte_source.h`:
```cpp
#pragma once
#include "snii/common/slice.h"
#include "snii/common/status.h"
namespace snii {
class ByteSource {
 public:
  explicit ByteSource(Slice s) : s_(s) {}
  Status get_u8(uint8_t* v);
  Status get_fixed32(uint32_t* v);
  Status get_fixed64(uint64_t* v);
  Status get_varint32(uint32_t* v);
  Status get_varint64(uint64_t* v);
  Status get_zigzag(int64_t* v);
  Status get_bytes(size_t n, Slice* out);
  size_t remaining() const { return s_.size() - pos_; }
  size_t position() const { return pos_; }
  bool eof() const { return pos_ == s_.size(); }
 private:
  Slice s_;
  size_t pos_ = 0;
};
}  // namespace snii
```

`byte_source.cpp`:
```cpp
#include "snii/encoding/byte_source.h"
#include "snii/encoding/varint.h"
namespace snii {
Status ByteSource::get_u8(uint8_t* v) {
  if (remaining() < 1) return Status::Corruption("get_u8 overrun");
  *v = s_[pos_++]; return Status::OK();
}
Status ByteSource::get_fixed32(uint32_t* v) {
  if (remaining() < 4) return Status::Corruption("get_fixed32 overrun");
  uint32_t r = 0;
  for (int i = 0; i < 4; ++i) r |= static_cast<uint32_t>(s_[pos_ + i]) << (8 * i);
  pos_ += 4; *v = r; return Status::OK();
}
Status ByteSource::get_fixed64(uint64_t* v) {
  if (remaining() < 8) return Status::Corruption("get_fixed64 overrun");
  uint64_t r = 0;
  for (int i = 0; i < 8; ++i) r |= static_cast<uint64_t>(s_[pos_ + i]) << (8 * i);
  pos_ += 8; *v = r; return Status::OK();
}
Status ByteSource::get_varint64(uint64_t* v) {
  const uint8_t* p = s_.data() + pos_;
  const uint8_t* next = nullptr;
  SNII_RETURN_IF_ERROR(decode_varint64(p, s_.data() + s_.size(), v, &next));
  pos_ = static_cast<size_t>(next - s_.data());
  return Status::OK();
}
Status ByteSource::get_varint32(uint32_t* v) {
  uint64_t tmp; SNII_RETURN_IF_ERROR(get_varint64(&tmp));
  if (tmp > 0xFFFFFFFFu) return Status::Corruption("varint32 overflow");
  *v = static_cast<uint32_t>(tmp); return Status::OK();
}
Status ByteSource::get_zigzag(int64_t* v) {
  uint64_t tmp; SNII_RETURN_IF_ERROR(get_varint64(&tmp)); *v = zigzag_decode(tmp); return Status::OK();
}
Status ByteSource::get_bytes(size_t n, Slice* out) {
  if (remaining() < n) return Status::Corruption("get_bytes overrun");
  *out = s_.subslice(pos_, n); pos_ += n; return Status::OK();
}
}  // namespace snii
```

- [ ] **Step 4–6: 接入构建 / 运行通过 / Commit** `git commit -m "feat(core): add ByteSource read cursor with overrun checks"`

---

### Task 6: crc32c

**Files:**
- Create: `core/include/snii/encoding/crc32c.h`
- Create: `core/src/encoding/crc32c.cpp`
- Test: `core/tests/encoding/crc32c_test.cpp`

**Interfaces:**
- Produces: `uint32_t crc32c(Slice data);` 与 `uint32_t crc32c_extend(uint32_t crc, Slice data);`（Castagnoli 多项式 0x1EDC6F41，反射实现）。

- [ ] **Step 1: 写失败测试（用已知向量）**

```cpp
#include <gtest/gtest.h>
#include "snii/encoding/crc32c.h"
using namespace snii;
TEST(Crc32c, KnownVectors) {
  // RFC 3720 / iSCSI 测试向量：32 字节 0x00 → 0x8a9136aa
  std::vector<uint8_t> zeros(32, 0x00);
  EXPECT_EQ(crc32c(Slice(zeros)), 0x8a9136aau);
  std::vector<uint8_t> ff(32, 0xff);
  EXPECT_EQ(crc32c(Slice(ff)), 0x62a8ab43u);
}
TEST(Crc32c, ExtendEqualsContiguous) {
  std::vector<uint8_t> v{1,2,3,4,5,6,7,8};
  uint32_t whole = crc32c(Slice(v));
  uint32_t part = crc32c(Slice(v.data(), 4));
  part = crc32c_extend(part, Slice(v.data()+4, 4));
  EXPECT_EQ(whole, part);
}
```

- [ ] **Step 2: 运行确认失败**。
- [ ] **Step 3: 实现**（表驱动软件实现，保证可移植 + 确定性；硬件加速留后续优化）

`crc32c.h`:
```cpp
#pragma once
#include <cstdint>
#include "snii/common/slice.h"
namespace snii {
uint32_t crc32c_extend(uint32_t crc, Slice data);
inline uint32_t crc32c(Slice data) { return crc32c_extend(0, data); }
}  // namespace snii
```

`crc32c.cpp`:
```cpp
#include "snii/encoding/crc32c.h"
#include <array>
namespace snii {
namespace {
constexpr uint32_t kPoly = 0x82F63B78u;  // 反射后的 Castagnoli
std::array<uint32_t, 256> make_table() {
  std::array<uint32_t, 256> t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (kPoly ^ (c >> 1)) : (c >> 1);
    t[i] = c;
  }
  return t;
}
const std::array<uint32_t, 256> kTable = make_table();
}  // namespace
uint32_t crc32c_extend(uint32_t crc, Slice data) {
  crc = ~crc;
  for (size_t i = 0; i < data.size(); ++i)
    crc = kTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}
}  // namespace snii
```

- [ ] **Step 4–6: 接入构建 / 运行通过 / Commit** `git commit -m "feat(core): add crc32c (Castagnoli) with known-vector tests"`

---

### Task 7: SectionFramer（统一 section 框定 + 校验）

**Files:**
- Create: `core/include/snii/encoding/section_framer.h`
- Create: `core/src/encoding/section_framer.cpp`
- Test: `core/tests/encoding/section_framer_test.cpp`

**Interfaces:**
- Produces:
  - `void SectionFramer::write(ByteSink& sink, uint8_t section_type, Slice payload);` 帧格式：`[u8 type][varint64 payload_len][payload bytes][fixed32 crc32c(type+len_bytes+payload)]`
  - `struct FramedSection { uint8_t type; Slice payload; };`
  - `Status SectionFramer::read(ByteSource& src, FramedSection* out);`（校验 crc，失败返回 Corruption；用于跳过 unknown optional section：调用方按 type dispatch，未知 optional 直接忽略 payload）

- [ ] **Step 1: 写失败测试（round-trip + 损坏检测 + 跳过未知）**

```cpp
#include <gtest/gtest.h>
#include "snii/encoding/section_framer.h"
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/byte_source.h"
using namespace snii;
TEST(SectionFramer, RoundTrip) {
  ByteSink sink;
  const uint8_t p[] = {9,8,7};
  SectionFramer::write(sink, 0x42, Slice(p, 3));
  ByteSource src(sink.view());
  FramedSection sec;
  ASSERT_TRUE(SectionFramer::read(src, &sec).ok());
  EXPECT_EQ(sec.type, 0x42);
  ASSERT_EQ(sec.payload.size(), 3u);
  EXPECT_EQ(sec.payload[0], 9u);
  EXPECT_TRUE(src.eof());
}
TEST(SectionFramer, DetectsCorruption) {
  ByteSink sink; const uint8_t p[] = {1,2,3,4};
  SectionFramer::write(sink, 1, Slice(p, 4));
  auto bytes = sink.buffer();
  bytes[3] ^= 0xFF;  // 翻转 payload 一个字节
  ByteSource src(Slice(bytes));
  FramedSection sec;
  EXPECT_EQ(SectionFramer::read(src, &sec).code(), StatusCode::kCorruption);
}
TEST(SectionFramer, SkipMultiple) {
  ByteSink sink; const uint8_t a[]={1}; const uint8_t b[]={2,2};
  SectionFramer::write(sink, 10, Slice(a,1));
  SectionFramer::write(sink, 11, Slice(b,2));
  ByteSource src(sink.view());
  FramedSection s1, s2;
  ASSERT_TRUE(SectionFramer::read(src, &s1).ok());
  ASSERT_TRUE(SectionFramer::read(src, &s2).ok());
  EXPECT_EQ(s1.type, 10); EXPECT_EQ(s2.type, 11);
}
```

- [ ] **Step 2: 运行确认失败**。
- [ ] **Step 3: 实现**

`section_framer.h`:
```cpp
#pragma once
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/byte_source.h"
#include "snii/common/slice.h"
#include "snii/common/status.h"
namespace snii {
struct FramedSection { uint8_t type = 0; Slice payload; };
class SectionFramer {
 public:
  static void write(ByteSink& sink, uint8_t section_type, Slice payload);
  static Status read(ByteSource& src, FramedSection* out);
};
}  // namespace snii
```

`section_framer.cpp`（crc 覆盖 type+len+payload，用一个临时 sink 复用编码逻辑，DRY）:
```cpp
#include "snii/encoding/section_framer.h"
#include "snii/encoding/crc32c.h"
namespace snii {
void SectionFramer::write(ByteSink& sink, uint8_t section_type, Slice payload) {
  ByteSink framed;          // type + len + payload，统一计算 crc
  framed.put_u8(section_type);
  framed.put_varint64(payload.size());
  framed.put_bytes(payload);
  uint32_t crc = crc32c(framed.view());
  sink.put_bytes(framed.view());
  sink.put_fixed32(crc);
}
Status SectionFramer::read(ByteSource& src, FramedSection* out) {
  size_t start = src.position();
  uint8_t type; SNII_RETURN_IF_ERROR(src.get_u8(&type));
  uint64_t len; SNII_RETURN_IF_ERROR(src.get_varint64(&len));
  Slice payload; SNII_RETURN_IF_ERROR(src.get_bytes(static_cast<size_t>(len), &payload));
  size_t framed_len = src.position() - start;
  uint32_t stored; SNII_RETURN_IF_ERROR(src.get_fixed32(&stored));
  // crc 覆盖 [start, start+framed_len)
  Slice framed = Slice(payload.data() - (framed_len - payload.size()), framed_len);
  if (crc32c(framed) != stored) return Status::Corruption("section crc mismatch");
  out->type = type; out->payload = payload;
  return Status::OK();
}
}  // namespace snii
```
> 注：`read` 中 framed 切片起点由 payload 指针回推，避免持有原始 buffer 引用；实现期若指针回推不便，改为在 ByteSource 暴露 `slice_from(start, len)` helper（择一，保持单一来源）。

- [ ] **Step 4–6: 接入构建 / 运行通过 / Commit** `git commit -m "feat(core): add SectionFramer with crc-verified framing"`

---

### Task 8: zstd 编解码包装

**Files:**
- Create: `core/include/snii/encoding/zstd_codec.h`
- Create: `core/src/encoding/zstd_codec.cpp`
- Test: `core/tests/encoding/zstd_codec_test.cpp`
- Modify: `core/CMakeLists.txt`（链接 `libzstd`）

**Interfaces:**
- Produces:
  - `Status zstd_compress(Slice input, int level, std::vector<uint8_t>* out);`
  - `Status zstd_decompress(Slice input, size_t expected_uncomp_len, std::vector<uint8_t>* out);`

- [ ] **Step 1: 写失败测试**

```cpp
#include <gtest/gtest.h>
#include "snii/encoding/zstd_codec.h"
using namespace snii;
TEST(Zstd, RoundTrip) {
  std::vector<uint8_t> in;
  for (int i = 0; i < 10000; ++i) in.push_back(static_cast<uint8_t>(i % 7));
  std::vector<uint8_t> comp, decomp;
  ASSERT_TRUE(zstd_compress(Slice(in), 3, &comp).ok());
  EXPECT_LT(comp.size(), in.size());
  ASSERT_TRUE(zstd_decompress(Slice(comp), in.size(), &decomp).ok());
  EXPECT_EQ(decomp, in);
}
TEST(Zstd, WrongLenFails) {
  std::vector<uint8_t> in(100, 7), comp, decomp;
  ASSERT_TRUE(zstd_compress(Slice(in), 3, &comp).ok());
  EXPECT_FALSE(zstd_decompress(Slice(comp), 99, &decomp).ok());
}
```

- [ ] **Step 2: 运行确认失败**。
- [ ] **Step 3: 实现 + 链接**

`core/CMakeLists.txt`：
```cmake
find_library(ZSTD_LIB zstd REQUIRED)
target_link_libraries(snii PUBLIC ${ZSTD_LIB})
```

`zstd_codec.h`:
```cpp
#pragma once
#include <vector>
#include "snii/common/slice.h"
#include "snii/common/status.h"
namespace snii {
Status zstd_compress(Slice input, int level, std::vector<uint8_t>* out);
Status zstd_decompress(Slice input, size_t expected_uncomp_len, std::vector<uint8_t>* out);
}  // namespace snii
```

`zstd_codec.cpp`:
```cpp
#include "snii/encoding/zstd_codec.h"
#include <zstd.h>
namespace snii {
Status zstd_compress(Slice input, int level, std::vector<uint8_t>* out) {
  size_t bound = ZSTD_compressBound(input.size());
  out->resize(bound);
  size_t n = ZSTD_compress(out->data(), bound, input.data(), input.size(), level);
  if (ZSTD_isError(n)) return Status::Internal(std::string("zstd compress: ") + ZSTD_getErrorName(n));
  out->resize(n);
  return Status::OK();
}
Status zstd_decompress(Slice input, size_t expected_uncomp_len, std::vector<uint8_t>* out) {
  out->resize(expected_uncomp_len);
  size_t n = ZSTD_decompress(out->data(), expected_uncomp_len, input.data(), input.size());
  if (ZSTD_isError(n)) return Status::Corruption(std::string("zstd decompress: ") + ZSTD_getErrorName(n));
  if (n != expected_uncomp_len) return Status::Corruption("zstd decompressed length mismatch");
  return Status::OK();
}
}  // namespace snii
```

- [ ] **Step 4–6: 接入构建 / 运行通过 / Commit** `git commit -m "feat(core): add zstd codec wrapper"`

---

### Task 9: PFOR 整数块编解码

**Files:**
- Create: `core/include/snii/encoding/pfor.h`
- Create: `core/src/encoding/pfor.cpp`
- Test: `core/tests/encoding/pfor_test.cpp`

**Interfaces:**
- Produces:
  - `void pfor_encode(const uint32_t* values, size_t n, ByteSink* out);` —— FOR：写 `[u8 bit_width][varint base 省略, 调用方已做 delta][bit-packed n 个 (value) using bit_width][异常表]`。本期实现 **bit-packing + 异常**（高于 bit_width 的值进异常表，存 (index, full_value)）。
  - `Status pfor_decode(ByteSource* src, size_t n, uint32_t* out);`

> 设计：encode 选取覆盖 ~90% 值的 bit_width（最小化总字节）；超出者作为异常按 `(varint index_delta, varint value)` 存于尾部。块大小由调用方控制（典型 ≤256）。delta/zigzag 由上层（frq window）负责，PFOR 只处理无符号整数数组。

- [ ] **Step 1: 写失败测试（round-trip：常规 + 含异常 + 全相等）**

```cpp
#include <gtest/gtest.h>
#include "snii/encoding/pfor.h"
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/byte_source.h"
using namespace snii;
static void roundtrip(const std::vector<uint32_t>& v) {
  ByteSink sink; pfor_encode(v.data(), v.size(), &sink);
  ByteSource src(sink.view());
  std::vector<uint32_t> out(v.size());
  ASSERT_TRUE(pfor_decode(&src, v.size(), out.data()).ok());
  EXPECT_EQ(out, v);
}
TEST(Pfor, Uniform)   { roundtrip(std::vector<uint32_t>(200, 5)); }
TEST(Pfor, Ramp)      { std::vector<uint32_t> v; for (uint32_t i=0;i<256;++i) v.push_back(i); roundtrip(v); }
TEST(Pfor, WithOutliers) {
  std::vector<uint32_t> v(128, 3); v[10]=1000000; v[77]=999; roundtrip(v);
}
TEST(Pfor, AllZero)   { roundtrip(std::vector<uint32_t>(64, 0)); }
```

- [ ] **Step 2: 运行确认失败**。
- [ ] **Step 3: 实现**（bit-pack helper 单独抽出，避免主函数 if-else 堆叠）

`pfor.h`:
```cpp
#pragma once
#include <cstdint>
#include <cstddef>
#include "snii/encoding/byte_sink.h"
#include "snii/encoding/byte_source.h"
#include "snii/common/status.h"
namespace snii {
void pfor_encode(const uint32_t* values, size_t n, ByteSink* out);
Status pfor_decode(ByteSource* src, size_t n, uint32_t* out);
}  // namespace snii
```

`pfor.cpp`（要点；实现期补全 bit-pack/unpack helper，函数各自 ≤50 行）:
```cpp
#include "snii/encoding/pfor.h"
#include "snii/encoding/varint.h"
#include <algorithm>
#include <vector>
namespace snii {
namespace {
uint8_t bits_for(uint32_t v) { uint8_t b = 0; while (v) { ++b; v >>= 1; } return b; }
// 选取使总字节最小的 bit_width：对每个候选 w 估算 packed + 异常成本，取最小。
uint8_t choose_width(const uint32_t* v, size_t n) {
  uint8_t maxw = 0;
  for (size_t i = 0; i < n; ++i) maxw = std::max(maxw, bits_for(v[i]));
  uint8_t best = maxw; size_t best_cost = SIZE_MAX;
  for (uint8_t w = 0; w <= maxw; ++w) {
    size_t exc = 0;
    for (size_t i = 0; i < n; ++i) if (bits_for(v[i]) > w) ++exc;
    size_t cost = (static_cast<size_t>(w) * n + 7) / 8 + exc * 6;  // 估算
    if (cost < best_cost) { best_cost = cost; best = w; }
  }
  return best;
}
void bitpack(const uint32_t* v, size_t n, uint8_t w, ByteSink* out) {
  uint64_t acc = 0; int filled = 0;
  for (size_t i = 0; i < n; ++i) {
    uint32_t masked = (w == 32) ? v[i] : (v[i] & ((1u << w) - 1));
    acc |= static_cast<uint64_t>(masked) << filled; filled += w;
    while (filled >= 8) { out->put_u8(static_cast<uint8_t>(acc)); acc >>= 8; filled -= 8; }
  }
  if (filled > 0) out->put_u8(static_cast<uint8_t>(acc));
}
Status bitunpack(ByteSource* src, size_t n, uint8_t w, uint32_t* out) {
  uint64_t acc = 0; int filled = 0;
  for (size_t i = 0; i < n; ++i) {
    while (filled < w) {
      uint8_t b; SNII_RETURN_IF_ERROR(src->get_u8(&b));
      acc |= static_cast<uint64_t>(b) << filled; filled += 8;
    }
    out[i] = (w == 0) ? 0u : static_cast<uint32_t>(acc & ((w==32)?0xFFFFFFFFull:((1ull<<w)-1)));
    acc >>= w; filled -= w;
  }
  return Status::OK();
}
}  // namespace
void pfor_encode(const uint32_t* values, size_t n, ByteSink* out) {
  uint8_t w = choose_width(values, n);
  std::vector<std::pair<uint32_t,uint32_t>> exc;  // (index, value)
  for (size_t i = 0; i < n; ++i) if (bits_for(values[i]) > w) exc.emplace_back(i, values[i]);
  out->put_u8(w);
  out->put_varint32(static_cast<uint32_t>(exc.size()));
  // 低位 packed（异常位置写 0 占位，真值在异常表）
  std::vector<uint32_t> low(values, values + n);
  for (auto& e : exc) low[e.first] = 0;
  bitpack(low.data(), n, w, out);
  uint32_t prev = 0;
  for (auto& e : exc) { out->put_varint32(e.first - prev); out->put_varint32(e.second); prev = e.first; }
}
Status pfor_decode(ByteSource* src, size_t n, uint32_t* out) {
  uint8_t w; SNII_RETURN_IF_ERROR(src->get_u8(&w));
  uint32_t nexc; SNII_RETURN_IF_ERROR(src->get_varint32(&nexc));
  SNII_RETURN_IF_ERROR(bitunpack(src, n, w, out));
  uint32_t idx = 0;
  for (uint32_t i = 0; i < nexc; ++i) {
    uint32_t d, val; SNII_RETURN_IF_ERROR(src->get_varint32(&d)); SNII_RETURN_IF_ERROR(src->get_varint32(&val));
    idx += d; if (idx >= n) return Status::Corruption("pfor exception index oob"); out[idx] = val;
  }
  return Status::OK();
}
}  // namespace snii
```

- [ ] **Step 4–6: 接入构建 / 运行通过 / Commit** `git commit -m "feat(core): add PFOR integer block codec"`

---

## Phase 1 完成判据

- [ ] `cmake -S . -B build && cmake --build build -j && ctest --test-dir build` 全绿。
- [ ] core 在 `SNII_BUILD_BENCH=OFF` 下可独立编译，零 bench/clucene 依赖。
- [ ] 所有 L0 原语具备 round-trip + 损坏检测测试。
- [ ] 无函数 >80 行；无 if-else 长链；零重复编码逻辑。
- [ ] 过一次 code-reviewer agent，CRITICAL/HIGH 清零。

## 后续 Phase（各自 JIT 出独立计划，spec §12 路线图）

- Phase 2 — L1 I/O：FileReader/Writer、LocalFile、MeteredFileReader(S3 成本模型)、BatchRangeFetcher。
- Phase 3 — L2 词典族：dict_entry / dict_block / sampled_term_index / dict_block_directory / xfilter。
- Phase 4 — L2 倒排族：frq_pod / frq_prelude / prx_pod / norms_pod / null_bitmap / stats_block。
- Phase 5 — L2 容器/meta：bootstrap_header / per_index_meta / logical_index_directory / tail_meta_region / tail_pointer。
- Phase 6 — L3 写：spimi_term_buffer / logical_index_writer / snii_compound_writer。
- Phase 7 — L3 读/查询：segment_reader / logical_index_reader / term_lookup / decoders / term·phrase·bm25。
- Phase 8 — S3 后端：S3FileReader/Writer（aws-sdk），真实 OSS 打通。
- Phase 9 — 基准：corpus_gen / clucene·snii adapter / runner / 双轨四指标报告。
