# UvNVR private-protocol C SDK. Produces uvnvr_sdk.dll/.so, the import library
# (Windows) and the single public header.

enable_language(C)

add_library(uvnvr_sdk SHARED
  src/sdk.cpp
  src/proto_client.cpp
  ../src/proto/proto_protocol.cpp
)

target_include_directories(uvnvr_sdk
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)

set_target_properties(uvnvr_sdk PROPERTIES
  OUTPUT_NAME uvnvr_sdk
  VERSION ${PROJECT_VERSION}
  SOVERSION 0
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)
foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${uvp_config}" uvp_config_upper)
  set_target_properties(uvnvr_sdk PROPERTIES
    "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}"
    "LIBRARY_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}"
    "ARCHIVE_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
endforeach()
set_target_properties(uvnvr_sdk PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if(UVNVR_ENABLE_QUIC)
  target_compile_definitions(uvnvr_sdk PRIVATE UVNVR_ENABLE_QUIC=1)
else()
  target_compile_definitions(uvnvr_sdk PRIVATE UVNVR_ENABLE_QUIC=0)
endif()
target_compile_definitions(uvnvr_sdk PRIVATE
  UVNVR_SDK_BUILD=1
  UVNVR_SDK_VERSION="0.1.0"
)

# Do NOT link a second libuv here. libuvcpp's release archives embed their own
# libuv, and the SDK must not make direct libuv calls: a symbol resolved from
# the platform's libuv would operate on handles and a loop created by the
# libuv version embedded in libuvcpp. Those structs changed between libuv
# releases (uv_async_t's private fields among them), and a mismatch leaves the
# SDK's task pump unable to wake the loop. The SDK uses only libuvcpp's
# exported API; see Client::StartTaskPump().
target_link_libraries(uvnvr_sdk PRIVATE ${UVP_UVCPP_TARGET} OpenSSL::SSL OpenSSL::Crypto)

# libuvcpp's shared object carries a DT_NEEDED on libllhttp; ld needs to resolve
# it at link time even though the SDK never calls it directly.
find_library(UVNVR_LLHTTP_LIBRARY NAMES llhttp
             HINTS "${UVNVR_LIBUVCPP_ROOT}/lib")
if(UVNVR_LLHTTP_LIBRARY)
  if(MSVC)
    target_link_libraries(uvnvr_sdk PRIVATE "${UVNVR_LLHTTP_LIBRARY}")
  else()
    # libuvcpp references llhttp but does not carry it as DT_NEEDED in the
    # release archive. Force the dependency into libuvnvr_sdk.so so applications
    # only have to link/load this one library.
    target_link_libraries(uvnvr_sdk PRIVATE -Wl,--no-as-needed
                          "${UVNVR_LLHTTP_LIBRARY}" -Wl,--as-needed)
  endif()
endif()

if(WIN32)
  target_compile_definitions(uvnvr_sdk PRIVATE NOMINMAX)
  target_link_libraries(uvnvr_sdk PRIVATE ws2_32)
endif()

if(MSVC)
  target_compile_options(uvnvr_sdk PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(uvnvr_sdk PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(UVNVR_LIBUVCPP_ROOT)
  set_target_properties(uvnvr_sdk PROPERTIES BUILD_RPATH "${UVNVR_LIBUVCPP_ROOT}/lib")
  if(UVNVR_LLHTTP_LIBRARY)
    get_filename_component(_uvnvr_llhttp_dir "${UVNVR_LLHTTP_LIBRARY}" DIRECTORY)
    set_target_properties(uvnvr_sdk PROPERTIES
      BUILD_RPATH "${UVNVR_LIBUVCPP_ROOT}/lib;${_uvnvr_llhttp_dir}"
      INSTALL_RPATH "$ORIGIN")
  endif()
endif()

install(TARGETS uvnvr_sdk
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib)
install(FILES include/uvnvr_sdk.h DESTINATION include)

# Samples -------------------------------------------------------------------
foreach(sample basic live download)
  add_executable(uvnvr_sample_${sample} samples/sample_${sample}.c)
  target_link_libraries(uvnvr_sample_${sample} PRIVATE uvnvr_sdk)
  set_target_properties(uvnvr_sample_${sample} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  if(NOT MSVC)
    target_compile_options(uvnvr_sample_${sample} PRIVATE -Wall -Wextra)
  endif()
endforeach()

# Tests ---------------------------------------------------------------------
add_executable(uvnvr_sdk_test tests/test_sdk.c)
target_link_libraries(uvnvr_sdk_test PRIVATE uvnvr_sdk OpenSSL::Crypto)
set_target_properties(uvnvr_sdk_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(MSVC)
  # OpenSSL's EVP API is available through the linked Crypto library.
  target_compile_definitions(uvnvr_sdk_test PRIVATE NOMINMAX)
else()
  target_compile_options(uvnvr_sdk_test PRIVATE -Wall -Wextra)
endif()
