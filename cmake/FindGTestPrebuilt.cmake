# Locate the doris-thirdparty prebuilt gtest/gmock to avoid fetching over the network.
# Provides the INTERFACE target: snii_gtest
set(_dt_candidates
  "/mnt/disk1/jiangkai/workspace/src/doris-clean/thirdparty/installed"
  "/mnt/disk1/jiangkai/workspace/src/doris-master/thirdparty/installed"
  "/mnt/disk1/jiangkai/workspace/src/doris/thirdparty/installed")

set(GTEST_ROOT "" CACHE PATH "gtest root")
if(NOT GTEST_ROOT)
  foreach(_dt ${_dt_candidates})
    if(EXISTS "${_dt}/lib/libgtest.a")
      set(GTEST_ROOT "${_dt}")
      break()
    endif()
  endforeach()
endif()

if(NOT GTEST_ROOT OR NOT EXISTS "${GTEST_ROOT}/lib/libgtest.a")
  set(GTestPrebuilt_FOUND FALSE)
  if(GTestPrebuilt_FIND_REQUIRED)
    message(FATAL_ERROR "prebuilt gtest not found; set -DGTEST_ROOT=<path>")
  endif()
  return()
endif()

if(NOT TARGET snii_gtest)
  add_library(snii_gtest INTERFACE)
  target_include_directories(snii_gtest INTERFACE "${GTEST_ROOT}/include")
  target_link_libraries(snii_gtest INTERFACE
    "${GTEST_ROOT}/lib/libgtest.a"
    "${GTEST_ROOT}/lib/libgtest_main.a"
    pthread)
endif()

set(GTestPrebuilt_FOUND TRUE)
message(STATUS "Found prebuilt gtest at ${GTEST_ROOT}")
