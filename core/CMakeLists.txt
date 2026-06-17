# libsnii —— S3-native 倒排索引核心库。零 bench / 零 clucene 依赖。
# 源文件清单按任务显式追加（不使用 glob，保证可审计、可控）。
set(SNII_SOURCES
  src/common/status.cpp
  src/encoding/varint.cpp
  src/encoding/byte_sink.cpp
  src/encoding/byte_source.cpp
  src/encoding/crc32c.cpp
  src/encoding/section_framer.cpp
  src/encoding/zstd_codec.cpp
  src/encoding/pfor.cpp
)

add_library(snii STATIC ${SNII_SOURCES})
target_include_directories(snii PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(snii PROPERTIES LINKER_LANGUAGE CXX)

# libzstd（.prx 等大块压缩）。优先用 doris-thirdparty 安装目录（非隐式路径，避免
# CMake 把 /usr/include 从 -I 剥离的问题，且与 Doris 构建一致）。
set(_dt_installed_candidates
  "/mnt/disk1/jiangkai/workspace/src/doris-clean/thirdparty/installed"
  "/mnt/disk1/jiangkai/workspace/src/doris-master/thirdparty/installed"
  "/mnt/disk1/jiangkai/workspace/src/doris/thirdparty/installed")
find_path(ZSTD_INCLUDE_DIR zstd.h PATHS ${_dt_installed_candidates} PATH_SUFFIXES include
  NO_DEFAULT_PATH REQUIRED)
find_library(ZSTD_LIB NAMES zstd libzstd.a PATHS ${_dt_installed_candidates} PATH_SUFFIXES lib
  NO_DEFAULT_PATH REQUIRED)
target_include_directories(snii PUBLIC ${ZSTD_INCLUDE_DIR})
target_link_libraries(snii PUBLIC ${ZSTD_LIB})

if(SNII_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
