# libsnii -- S3-native inverted index core library. Zero bench / zero clucene deps.
# Sources are picked up via GLOB_RECURSE + CONFIGURE_DEPENDS: adding a new module only
# requires adding files under src/, with no edits to this list (so parallel module
# development does not conflict on a shared CMakeLists). git still tracks everything.
file(GLOB_RECURSE SNII_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(snii STATIC ${SNII_SOURCES})
target_include_directories(snii PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(snii PROPERTIES LINKER_LANGUAGE CXX)

# libzstd (for compressing large payloads such as .prx). Prefer the doris-thirdparty
# install dir (a non-implicit path, avoiding CMake stripping /usr/include from -I, and
# matching the Doris build).
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

# CRoaring (for the null-bitmap POD), from the doris-thirdparty install dir.
find_path(ROARING_INCLUDE_DIR roaring/roaring.hh PATHS ${_dt_installed_candidates}
  PATH_SUFFIXES include NO_DEFAULT_PATH REQUIRED)
find_library(ROARING_LIB NAMES roaring libroaring.a PATHS ${_dt_installed_candidates}
  PATH_SUFFIXES lib NO_DEFAULT_PATH REQUIRED)
target_include_directories(snii PUBLIC ${ROARING_INCLUDE_DIR})
target_link_libraries(snii PUBLIC ${ROARING_LIB})

# xxHash header (used header-only via XXH_INLINE_ALL by the block-split bloom filter).
find_path(XXHASH_INCLUDE_DIR xxhash.h PATHS ${_dt_installed_candidates}
  PATH_SUFFIXES include NO_DEFAULT_PATH REQUIRED)
target_include_directories(snii PUBLIC ${XXHASH_INCLUDE_DIR})

# S3 / OSS backend (optional). When OFF, s3_object_store.cpp is still picked up by
# the GLOB above but compiles to an empty TU (its body is #ifdef SNII_WITH_S3),
# so core links NO aws and stays aws-free by default. When ON, link aws-sdk-cpp
# and define the macro PUBLIC so the header body and tests are enabled too.
if(SNII_WITH_S3)
  find_package(AwsSdk REQUIRED)
  target_compile_definitions(snii PUBLIC SNII_WITH_S3)
  target_link_libraries(snii PUBLIC snii_aws)
endif()

if(SNII_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
