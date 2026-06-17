# libsnii —— S3-native 倒排索引核心库。零 bench / 零 clucene 依赖。
# 源文件清单按任务显式追加（不使用 glob，保证可审计、可控）。
set(SNII_SOURCES
  src/common/status.cpp
  src/encoding/varint.cpp
  src/encoding/byte_sink.cpp
  src/encoding/byte_source.cpp
)

add_library(snii STATIC ${SNII_SOURCES})
target_include_directories(snii PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(snii PROPERTIES LINKER_LANGUAGE CXX)

if(SNII_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
