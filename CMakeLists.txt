cmake_minimum_required(VERSION 3.24)

project(uvnvr VERSION 0.1.0 LANGUAGES CXX)

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

if(UVNVR_LIBUVCPP_ROOT)
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

add_executable(uvnvr
  src/main.cpp
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

target_link_libraries(uvnvr PRIVATE ${UVP_UVCPP_TARGET})

# The release archive is a shared library outside the normal loader path.
if(UVNVR_LIBUVCPP_ROOT)
  set_target_properties(uvnvr PROPERTIES
    BUILD_RPATH "${UVNVR_LIBUVCPP_ROOT}/lib"
    INSTALL_RPATH "$ORIGIN/../lib")
endif()

if(MSVC)
  target_compile_options(uvnvr PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(uvnvr PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS uvnvr RUNTIME DESTINATION bin)

if(UVNVR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
