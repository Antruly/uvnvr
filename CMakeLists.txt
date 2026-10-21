cmake_minimum_required(VERSION 3.24)

project(uvnvr VERSION 0.17.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(UVNVR_BUILD_TESTS "Build UvNVR unit tests" OFF)
option(UVNVR_EMBED_WEB "Embed the built frontend into the binary" OFF)
set(UVNVR_LIBUVCPP_ROOT "" CACHE PATH
    "Path to an unpacked libuvcpp 1.8.1 release archive (the archive route uses pkg-config)")

# libuvcpp is consumed in one of two ways:
#   * an unpacked release archive has lib/pkgconfig/uvcpp.pc and no CMake package;
#   * `cmake --install` (or a macOS source build) has lib/cmake/uvcpp.
# UVNVR_LIBUVCPP_ROOT pins the first route, which is what CI and the
# reproducible developer setup use. Without it, find_package() is tried first.
find_package(PkgConfig QUIET)
find_package(OpenSSL REQUIRED)

if(UVNVR_LIBUVCPP_ROOT)
  if(MSVC)
    find_path(UVNVR_UVCPP_INCLUDE_DIR uvcpp.h
              PATHS "${UVNVR_LIBUVCPP_ROOT}/include" NO_DEFAULT_PATH)
    find_library(UVNVR_UVCPP_LIBRARY NAMES uvcpp
                 PATHS "${UVNVR_LIBUVCPP_ROOT}/lib" NO_DEFAULT_PATH)
    if(NOT UVNVR_UVCPP_INCLUDE_DIR OR NOT UVNVR_UVCPP_LIBRARY)
      message(FATAL_ERROR "cannot find uvcpp headers/library under ${UVNVR_LIBUVCPP_ROOT}")
    endif()
    add_library(uvcpp_archive INTERFACE)
    target_include_directories(uvcpp_archive INTERFACE "${UVNVR_UVCPP_INCLUDE_DIR}")
    target_link_libraries(uvcpp_archive INTERFACE "${UVNVR_UVCPP_LIBRARY}")
    set(UVP_UVCPP_TARGET uvcpp_archive)
    set(UVP_UVCPP_ROUTE "MSVC archive (${UVNVR_LIBUVCPP_ROOT})")
  else()
    if(NOT EXISTS "${UVNVR_LIBUVCPP_ROOT}/lib/pkgconfig/uvcpp.pc")
      message(FATAL_ERROR
        "UVNVR_LIBUVCPP_ROOT=${UVNVR_LIBUVCPP_ROOT} does not contain lib/pkgconfig/uvcpp.pc")
    endif()
    if(WIN32)
      set(_uvnvr_pkg_sep ";")
    else()
      set(_uvnvr_pkg_sep ":")
    endif()
    set(ENV{PKG_CONFIG_PATH}
        "${UVNVR_LIBUVCPP_ROOT}/lib/pkgconfig${_uvnvr_pkg_sep}$ENV{PKG_CONFIG_PATH}")
    if(NOT PkgConfig_FOUND)
      message(FATAL_ERROR "pkg-config is required for the libuvcpp release-archive route")
    endif()
    pkg_check_modules(UVCPP REQUIRED IMPORTED_TARGET "uvcpp>=1.8.1")
    set(UVP_UVCPP_TARGET PkgConfig::UVCPP)
    set(UVP_UVCPP_ROUTE "pkg-config (${UVCPP_VERSION})")
  endif()
else()
  find_package(uvcpp 1.8.1 CONFIG QUIET)
  if(uvcpp_FOUND)
    set(UVP_UVCPP_TARGET uvcpp::uvcpp)
    set(UVP_UVCPP_ROUTE "CMake package (${uvcpp_DIR})")
  else()
    if(NOT PkgConfig_FOUND)
      message(FATAL_ERROR
        "libuvcpp >= 1.8.1 was not found. Install libuvcpp, or set "
        "UVNVR_LIBUVCPP_ROOT to an unpacked release archive.")
    endif()
    pkg_check_modules(UVCPP REQUIRED IMPORTED_TARGET "uvcpp>=1.8.1")
    set(UVP_UVCPP_TARGET PkgConfig::UVCPP)
    set(UVP_UVCPP_ROUTE "pkg-config (${UVCPP_VERSION})")
  endif()
endif()

message(STATUS "uvp: libuvcpp via ${UVP_UVCPP_ROUTE}")

# Version metadata, including the git SHA when the checkout has one.
set(UVNVR_GIT_SHA "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE UVNVR_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(NOT UVNVR_GIT_SHA)
    set(UVNVR_GIT_SHA "unknown")
  endif()
endif()
configure_file(cmake/version.h.in
               "${CMAKE_BINARY_DIR}/generated/uvnvr/version.h" @ONLY)

# Homebrew prefixes are added to CMAKE_PREFIX_PATH in CI; adding their lib
# directories lets imported targets that spell -luv/-lnghttp2 resolve.
foreach(_uvnvr_prefix IN LISTS CMAKE_PREFIX_PATH)
  if(IS_DIRECTORY "${_uvnvr_prefix}/lib")
    link_directories("${_uvnvr_prefix}/lib")
  endif()
  if(IS_DIRECTORY "${_uvnvr_prefix}/include")
    include_directories("${_uvnvr_prefix}/include")
  endif()
endforeach()

add_executable(uvnvr
  src/main.cpp
  src/app.cpp
  src/brand_http.cpp
  src/config.cpp
  src/exporter.cpp
  src/media.cpp
  src/onvif.cpp
  src/onvif_events.cpp
  src/openapi.cpp
  src/recording.cpp
  src/playback.cpp
  src/storage_cache.cpp
  src/rtsp.cpp
  src/ntp.cpp
  src/platform.cpp
)

target_include_directories(uvnvr PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_BINARY_DIR}/generated"
)

# The manifest names the executable, and uvp resolves it as build/<name>.
set_target_properties(uvnvr PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
foreach(uvp_config ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${uvp_config}" uvp_config_upper)
  set_target_properties(uvnvr PROPERTIES
    "RUNTIME_OUTPUT_DIRECTORY_${uvp_config_upper}" "${CMAKE_BINARY_DIR}")
endforeach()

# Recording uses libuv's fs requests directly (io_uring on Linux when the
# kernel supports it). libuvcpp links libuv privately, so link it explicitly.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(LIBUV QUIET IMPORTED_TARGET libuv)
endif()
if(TARGET PkgConfig::LIBUV)
  target_link_libraries(uvnvr PRIVATE ${UVP_UVCPP_TARGET} PkgConfig::LIBUV OpenSSL::Crypto
                                     OpenSSL::SSL)
else()
  # MSVC builds get libuv through the libuvcpp archive; only add an explicit
  # library when one is available.
  find_library(UV_LIBRARY NAMES uv libuv)
  if(UV_LIBRARY)
    target_link_libraries(uvnvr PRIVATE ${UVP_UVCPP_TARGET} ${UV_LIBRARY} OpenSSL::Crypto
                                       OpenSSL::SSL)
  else()
    # The libuvcpp archive may provide libuv transitively on some platforms.
    target_link_libraries(uvnvr PRIVATE ${UVP_UVCPP_TARGET} OpenSSL::Crypto OpenSSL::SSL)
  endif()
endif()
if(WIN32)
  # uvcpp's Windows headers pull in <windows.h>; keep its min/max macros from
  # breaking std::min/std::max in this project (MSVC sees C2589 otherwise).
  target_compile_definitions(uvnvr PRIVATE NOMINMAX)
  target_link_libraries(uvnvr PRIVATE ws2_32)
endif()

# The release archive is a shared library outside the normal loader path.
if(UVNVR_LIBUVCPP_ROOT)
  set_target_properties(uvnvr PROPERTIES BUILD_RPATH "${UVNVR_LIBUVCPP_ROOT}/lib")
endif()
if(APPLE)
  set_target_properties(uvnvr PROPERTIES INSTALL_RPATH "@loader_path/../lib")
elseif(NOT WIN32)
  set_target_properties(uvnvr PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
endif()

if(MSVC)
  target_compile_options(uvnvr PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(uvnvr PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS uvnvr RUNTIME DESTINATION bin)
install(DIRECTORY frontend/dist/ DESTINATION share/uvnvr/frontend/dist OPTIONAL)

if(UVNVR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
