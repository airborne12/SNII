# Locates the doris-thirdparty prebuilt CLucene (Doris's customized fork, char*/UTF-8
# API) and its transitive deps, and exposes the INTERFACE target: snii_clucene.
# Link order matters (static archives): clucene-core -> shared -> contribs -> libic
# (TurboPFor: p4nd1dec256v32 etc.) -> lz4/zstd/roaring/icu.
set(_doris_clean "/mnt/disk1/jiangkai/workspace/src/doris-clean")
set(_clucene_src "/mnt/disk1/jiangkai/workspace/src/doris-thirdparty/src")
set(_clucene_bin "${_doris_clean}/be/build_Release/bin")
set(_clucene_cfg "${_doris_clean}/be/build_Release/clucene/src/shared")
set(_dt_installed "${_doris_clean}/thirdparty/installed/lib")

set(_required_libs
  "${_clucene_bin}/libclucene-core-static.a"
  "${_clucene_bin}/libclucene-shared-static.a"
  "${_clucene_bin}/libclucene-contribs-lib.a"
  "${_clucene_bin}/libic.a"
  "${_dt_installed}/liblz4.a"
  "${_dt_installed}/libzstd.a"
  "${_dt_installed}/libroaring.a"
  "${_dt_installed}/libicuuc.a"
  "${_dt_installed}/libicudata.a")

set(CLucene_FOUND TRUE)
foreach(_lib ${_required_libs})
  if(NOT EXISTS "${_lib}")
    set(CLucene_FOUND FALSE)
    if(CLucene_FIND_REQUIRED)
      message(FATAL_ERROR "CLucene dependency not found: ${_lib}")
    endif()
    return()
  endif()
endforeach()

if(NOT EXISTS "${_clucene_src}/core/CLucene.h" OR NOT EXISTS "${_clucene_cfg}/CLucene/clucene-config.h")
  set(CLucene_FOUND FALSE)
  if(CLucene_FIND_REQUIRED)
    message(FATAL_ERROR "CLucene headers/config not found under ${_clucene_src} / ${_clucene_cfg}")
  endif()
  return()
endif()

if(NOT TARGET snii_clucene)
  add_library(snii_clucene INTERFACE)
  target_include_directories(snii_clucene SYSTEM INTERFACE
    "${_clucene_src}/core" "${_clucene_src}/shared" "${_clucene_cfg}")
  target_link_libraries(snii_clucene INTERFACE ${_required_libs} pthread dl m)
endif()

message(STATUS "Found prebuilt CLucene (Doris fork) at ${_clucene_bin}")
