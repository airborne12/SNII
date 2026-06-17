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

if(SNII_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
