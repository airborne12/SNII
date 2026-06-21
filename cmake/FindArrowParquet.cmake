# Locates the prebuilt Apache Arrow + Parquet static libraries (a separate
# install tree from CLucene) and exposes the INTERFACE target: snii_arrow.
#
# The shipped Arrow CMake config package re-runs find_dependency for lz4/zstd/...
# against the prefix and fails, so we link the static archives directly. Arrow
# here is built against the standalone compression/crypto archives in the same
# lib64 (NOT a self-contained bundled archive), so those are linked explicitly.
# All archives are wrapped in a single --start-group/--end-group so inter-archive
# ordering is irrelevant.
set(_arrow_root "/mnt/disk1/jiangkai/workspace/install/installed-master")
set(_arrow_lib "${_arrow_root}/lib64")
set(_arrow_inc "${_arrow_root}/include")

set(ArrowParquet_FOUND TRUE)
set(_arrow_archives
  "${_arrow_lib}/libparquet.a"
  "${_arrow_lib}/libarrow.a"
  "${_arrow_lib}/libarrow_bundled_dependencies.a"
  "${_arrow_lib}/libthrift.a"
  "${_arrow_lib}/libsnappy.a"
  "${_arrow_lib}/liblz4.a"
  "${_arrow_lib}/libzstd.a"
  "${_arrow_lib}/libbrotlienc.a"
  "${_arrow_lib}/libbrotlidec.a"
  "${_arrow_lib}/libbrotlicommon.a"
  "${_arrow_lib}/libre2.a"
  "${_arrow_lib}/libz.a"
  "${_arrow_lib}/libbz2.a"
  "${_arrow_lib}/liblzma.a"
  "${_arrow_lib}/libcrc32c.a"
  "${_arrow_lib}/libabsl_crc32c.a"
  "${_arrow_lib}/libglog.a"
  "${_arrow_lib}/libgflags.a"
  "${_arrow_lib}/libssl.a"
  "${_arrow_lib}/libcrypto.a")

foreach(_a ${_arrow_archives})
  if(NOT EXISTS "${_a}")
    set(ArrowParquet_FOUND FALSE)
    if(ArrowParquet_FIND_REQUIRED)
      message(FATAL_ERROR "Arrow/Parquet archive not found: ${_a}")
    endif()
    return()
  endif()
endforeach()
if(NOT EXISTS "${_arrow_inc}/arrow/api.h" OR NOT EXISTS "${_arrow_inc}/parquet/arrow/reader.h")
  set(ArrowParquet_FOUND FALSE)
  if(ArrowParquet_FIND_REQUIRED)
    message(FATAL_ERROR "Arrow/Parquet headers not found under ${_arrow_inc}")
  endif()
  return()
endif()

if(NOT TARGET snii_arrow)
  add_library(snii_arrow INTERFACE)
  target_include_directories(snii_arrow INTERFACE "${_arrow_inc}")
  target_link_libraries(snii_arrow INTERFACE
    -Wl,--start-group ${_arrow_archives} -Wl,--end-group
    pthread dl m rt)
endif()

message(STATUS "Found prebuilt Arrow/Parquet at ${_arrow_root}")
