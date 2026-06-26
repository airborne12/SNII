# Locate the doris-thirdparty prebuilt aws-sdk-cpp (static) to avoid fetching over
# the network. Provides the INTERFACE target: snii_aws (include dir + grouped libs).
# The static libs have circular dependencies, so they are wrapped in a single
# -Wl,--start-group ... -Wl,--end-group block at link time.
set(_dt_candidates
  "/mnt/disk1/jiangkai/workspace/src/doris-clean/thirdparty/installed"
  "/mnt/disk1/jiangkai/workspace/src/doris-master/thirdparty/installed"
  "/mnt/disk1/jiangkai/workspace/src/doris/thirdparty/installed")

set(AWSSDK_ROOT "" CACHE PATH "aws-sdk-cpp root")
if(NOT AWSSDK_ROOT)
  foreach(_dt ${_dt_candidates})
    if(EXISTS "${_dt}/lib/libaws-cpp-sdk-s3.a")
      set(AWSSDK_ROOT "${_dt}")
      break()
    endif()
  endforeach()
endif()

if(NOT AWSSDK_ROOT OR NOT EXISTS "${AWSSDK_ROOT}/lib/libaws-cpp-sdk-s3.a")
  set(AwsSdk_FOUND FALSE)
  if(AwsSdk_FIND_REQUIRED)
    message(FATAL_ERROR "prebuilt aws-sdk-cpp not found; set -DAWSSDK_ROOT=<path>")
  endif()
  return()
endif()

# Ordered list of static archives. Order matters for the linker, and the whole set
# is wrapped in a single group because of mutual references between the aws-c-* libs.
set(_aws_libs
  libaws-cpp-sdk-s3.a
  libaws-cpp-sdk-core.a
  libaws-crt-cpp.a
  libaws-c-auth.a
  libaws-c-http.a
  libaws-c-mqtt.a
  libaws-c-event-stream.a
  libaws-c-s3.a
  libaws-c-io.a
  libaws-c-compression.a
  libaws-c-cal.a
  libaws-c-sdkutils.a
  libaws-checksums.a
  libaws-c-common.a
  libs2n.a
  libcurl.a
  libssl.a
  libcrypto.a
  libzstd.a)

set(_aws_lib_paths "")
foreach(_lib ${_aws_libs})
  if(NOT EXISTS "${AWSSDK_ROOT}/lib/${_lib}")
    set(AwsSdk_FOUND FALSE)
    if(AwsSdk_FIND_REQUIRED)
      message(FATAL_ERROR "aws static lib missing: ${AWSSDK_ROOT}/lib/${_lib}")
    endif()
    return()
  endif()
  list(APPEND _aws_lib_paths "${AWSSDK_ROOT}/lib/${_lib}")
endforeach()

if(NOT TARGET snii_aws)
  add_library(snii_aws INTERFACE)
  target_include_directories(snii_aws SYSTEM INTERFACE "${AWSSDK_ROOT}/include")
  # Wrap the archives in a single link group to resolve circular references, then
  # add the platform system libraries the aws sdk depends on.
  target_link_libraries(snii_aws INTERFACE
    -Wl,--start-group
    ${_aws_lib_paths}
    -Wl,--end-group
    pthread dl m z rt resolv)
endif()

set(AwsSdk_FOUND TRUE)
message(STATUS "Found prebuilt aws-sdk-cpp at ${AWSSDK_ROOT}")
