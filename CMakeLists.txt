cmake_minimum_required(VERSION 3.24)

project(uvnvr VERSION 0.19.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(UVNVR_BUILD_TESTS "Build UvNVR unit tests" OFF)
option(UVNVR_EMBED_WEB "Embed the built frontend into the binary" OFF)
option(UVNVR_BUILD_SDK "Build the private-protocol C SDK" ON)
option(UVNVR_ENABLE_QUIC "Enable the QUIC listener for the private protocol" ON)
option(UVNVR_ENABLE_AI "Build the AI recognition pipeline (requires OpenCV)" ON)
option(UVNVR_ENABLE_HYPERLPR
       "Build the HyperLPR3 plate engine (requires OpenCV and MNN; falls back to the stub)"
       OFF)
option(UVNVR_BUILD_TOOLS "Build developer tools (hlpr_cli)" OFF)
set(UVNVR_MNN_ROOT "" CACHE PATH
    "MNN SDK root containing include/MNN and libMNN (built locally or installed)")
set(UVNVR_LIBUVCPP_ROOT "" CACHE PATH
    "Path to an unpacked libuvcpp 1.8.1 release archive (the archive route uses pkg-config)")

# libuvcpp is consumed in one of two ways:
#   * an unpacked release archive has lib/pkgconfig/uvcpp.pc and no CMake package;
#   * `cmake --install` (or a macOS source build) has lib/cmake/uvcpp.
# UVNVR_LIBUVCPP_ROOT pins the first route, which is what CI and the
# reproducible developer setup use. Without it, find_package() is tried first.
find_package(PkgConfig QUIET)
find_package(OpenSSL REQUIRED)

# AI recognition needs OpenCV core/imgproc/imgcodecs. It is optional: when the
# headers are missing the module is not compiled and the rest of the service
# builds and runs normally. Windows builds look in the local MinGW prefix used
# by the developer setup; CI has no OpenCV, so AI is compiled out there.
set(UVNVR_AI_ENABLED 0)
if(UVNVR_ENABLE_AI)
  if(WIN32)
    foreach(_uvnvr_opencv_hint
            "$ENV{HOME}/.local/opencv4-mingw"
            "/home/zwy/.local/opencv4-mingw")
      if(EXISTS "${_uvnvr_opencv_hint}")
        list(APPEND CMAKE_PREFIX_PATH "${_uvnvr_opencv_hint}")
      endif()
    endforeach()
  elseif(PkgConfig_FOUND)
    # A MinGW OpenCV installed under ~/.local can otherwise win the default
    # search (CMake scans <prefix>/<name>*). Pin the native package instead.
    pkg_check_modules(UVNVR_OPENCV_PC QUIET opencv4)
    if(UVNVR_OPENCV_PC_FOUND)
      set(_uvnvr_opencv_config "${UVNVR_OPENCV_PC_LIBDIR}/cmake/opencv4")
      if(EXISTS "${_uvnvr_opencv_config}/OpenCVConfig.cmake")
        set(OpenCV_DIR "${_uvnvr_opencv_config}" CACHE PATH "OpenCV config" FORCE)
      endif()
    endif()
  endif()
  find_package(OpenCV QUIET COMPONENTS core imgproc imgcodecs)
  if(OpenCV_FOUND)
    set(UVNVR_AI_ENABLED 1)
    message(STATUS "ai: OpenCV ${OpenCV_VERSION} (${OpenCV_DIR})")
  else()
    message(STATUS
      "ai: OpenCV not found; UVNVR_ENABLE_AI is forced OFF and the AI module will not be compiled")
  endif()
else()
  message(STATUS "ai: disabled by UVNVR_ENABLE_AI=OFF")
endif()

# HyperLPR3 is optional and must never make the build fail. When it is asked
# for but MNN (or OpenCV, or the platform) is unavailable, the plate engine
# silently stays on the stage-1 stub and the reason is printed here.
set(UVNVR_HYPERLPR_ENABLED 0)
if(UVNVR_ENABLE_HYPERLPR)
  if(NOT UVNVR_AI_ENABLED)
    message(STATUS
      "ai: HyperLPR3 requested but AI/OpenCV is unavailable; falling back to the stub plate engine")
  elseif(WIN32)
    message(STATUS
      "ai: HyperLPR3 requested on Windows but MNN is not wired up for MinGW yet; falling back to the stub plate engine")
  else()
    set(_uvnvr_mnn_roots)
    if(UVNVR_MNN_ROOT)
      list(APPEND _uvnvr_mnn_roots "${UVNVR_MNN_ROOT}")
    endif()
    if(DEFINED ENV{MNN_ROOT} AND NOT "$ENV{MNN_ROOT}" STREQUAL "")
      list(APPEND _uvnvr_mnn_roots "$ENV{MNN_ROOT}")
    endif()
    list(APPEND _uvnvr_mnn_roots
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mnn"
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/MNN")

    find_path(UVNVR_MNN_INCLUDE_DIR NAMES MNN/Interpreter.hpp
              PATHS ${_uvnvr_mnn_roots} PATH_SUFFIXES include NO_DEFAULT_PATH)
    if(NOT UVNVR_MNN_INCLUDE_DIR)
      find_path(UVNVR_MNN_INCLUDE_DIR NAMES MNN/Interpreter.hpp)
    endif()
    find_library(UVNVR_MNN_LIBRARY NAMES MNN
                 PATHS ${_uvnvr_mnn_roots} PATH_SUFFIXES build-linux lib build .
                 NO_DEFAULT_PATH)
    if(NOT UVNVR_MNN_LIBRARY)
      find_library(UVNVR_MNN_LIBRARY NAMES MNN)
    endif()

    if(UVNVR_MNN_INCLUDE_DIR AND UVNVR_MNN_LIBRARY)
      set(UVNVR_HYPERLPR_ENABLED 1)
      get_filename_component(UVNVR_MNN_LIBRARY_DIR "${UVNVR_MNN_LIBRARY}" DIRECTORY)
      get_filename_component(UVNVR_MNN_LIBRARY_NAME "${UVNVR_MNN_LIBRARY}" NAME)
      message(STATUS "ai: HyperLPR3 enabled (MNN ${UVNVR_MNN_LIBRARY})")
    else()
      if(NOT UVNVR_MNN_INCLUDE_DIR)
        set(_uvnvr_hyperlpr_reason
            "MNN headers (MNN/Interpreter.hpp) were not found; set UVNVR_MNN_ROOT")
      else()
        set(_uvnvr_hyperlpr_reason
            "libMNN was not found under ${_uvnvr_mnn_roots}; set UVNVR_MNN_ROOT")
      endif()
      message(STATUS
        "ai: HyperLPR3 requested but ${_uvnvr_hyperlpr_reason}; falling back to the stub plate engine")
    endif()
  endif()
else()
  message(STATUS
    "ai: HyperLPR3 disabled by UVNVR_ENABLE_HYPERLPR=OFF; the plate engine is the stage-1 stub")
endif()

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
  src/detect.cpp
  src/discovery.cpp
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
  src/service.cpp
  src/proto/proto_protocol.cpp
  src/proto/proto_server.cpp
  src/ai/plate_post.cpp
)

if(UVNVR_AI_ENABLED)
  target_sources(uvnvr PRIVATE
    src/ai/ai_engine.cpp
    src/ai/ai_service.cpp
  )
  target_include_directories(uvnvr PRIVATE ${OpenCV_INCLUDE_DIRS})
  target_link_libraries(uvnvr PRIVATE ${OpenCV_LIBS})
endif()

if(UVNVR_HYPERLPR_ENABLED)
  # Vendored HyperLPR3 3.0.1 (Apache-2.0), see third_party/hyperlpr3/README.md.
  file(GLOB_RECURSE UVNVR_HYPERLPR_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/hyperlpr3/cpp/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/hyperlpr3/cpp/c_api/*.cc")
  add_library(uvnvr_mnn INTERFACE)
  target_include_directories(uvnvr_mnn INTERFACE "${UVNVR_MNN_INCLUDE_DIR}")
  target_link_libraries(uvnvr_mnn INTERFACE "${UVNVR_MNN_LIBRARY}")

  add_library(uvnvr_hyperlpr3 STATIC ${UVNVR_HYPERLPR_SOURCES})
  target_compile_definitions(uvnvr_hyperlpr3 PRIVATE INFERENCE_HELPER_ENABLE_MNN=1)
  target_include_directories(uvnvr_hyperlpr3
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/hyperlpr3/cpp/c_api"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/hyperlpr3/cpp/src")
  target_link_libraries(uvnvr_hyperlpr3 PUBLIC ${OpenCV_LIBS} uvnvr_mnn)
  if(UNIX)
    target_link_libraries(uvnvr_hyperlpr3 PUBLIC pthread m ${CMAKE_DL_LIBS})
  endif()
  set_target_properties(uvnvr_hyperlpr3 PROPERTIES POSITION_INDEPENDENT_CODE ON)

  target_sources(uvnvr PRIVATE src/ai/engine_hyperlpr.cpp)
  target_link_libraries(uvnvr PRIVATE uvnvr_hyperlpr3)
  set_property(TARGET uvnvr APPEND PROPERTY BUILD_RPATH "${UVNVR_MNN_LIBRARY_DIR}")

  # Ship the MNN runtime next to the binary: the installed RPATH already
  # points at <prefix>/lib.
  if(UVNVR_MNN_LIBRARY_NAME MATCHES "\\.(so|dylib)(\\..*)?$" OR
     UVNVR_MNN_LIBRARY_NAME MATCHES "\\.dll$")
    install(FILES "${UVNVR_MNN_LIBRARY}" DESTINATION lib)
  endif()
endif()

target_compile_definitions(uvnvr PRIVATE
  UVNVR_ENABLE_AI=${UVNVR_AI_ENABLED}
  UVNVR_ENABLE_HYPERLPR=${UVNVR_HYPERLPR_ENABLED})

if(WIN32)
  # The icon resource is what Explorer and shortcuts display; SetupIconFile
  # only covers the installer itself. Generate the script so the icon path is
  # absolute, independent of the resource compiler's working directory.
  enable_language(RC)
  # Forward slashes: rc/windres treat backslashes in quoted strings as escapes.
  set(UVNVR_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/branding/uvnvr.ico")
  configure_file(resources/windows/uvnvr.rc.in
                 "${CMAKE_BINARY_DIR}/generated/uvnvr.rc" @ONLY)
  target_sources(uvnvr PRIVATE "${CMAKE_BINARY_DIR}/generated/uvnvr.rc")
endif()

target_include_directories(uvnvr PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_BINARY_DIR}/generated"
)

if(UVNVR_ENABLE_QUIC)
  target_compile_definitions(uvnvr PRIVATE UVNVR_ENABLE_QUIC=1)
else()
  target_compile_definitions(uvnvr PRIVATE UVNVR_ENABLE_QUIC=0)
endif()

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
  # wtsapi32/userenv let the service launch an interactive console instance
  # after it is unregistered (session 0 isolation).
  target_link_libraries(uvnvr PRIVATE ws2_32 wtsapi32 userenv advapi32 shell32)
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

if(UVNVR_BUILD_TOOLS)
  if(UVNVR_HYPERLPR_ENABLED)
    add_executable(hlpr_cli
      tools/hlpr_cli/main.cpp
      src/ai/engine_hyperlpr.cpp
      src/ai/plate_post.cpp
    )
    target_include_directories(hlpr_cli PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/ai"
      ${OpenCV_INCLUDE_DIRS})
    target_compile_definitions(hlpr_cli PRIVATE UVNVR_ENABLE_HYPERLPR=1)
    target_link_libraries(hlpr_cli PRIVATE uvnvr_hyperlpr3)
    set_target_properties(hlpr_cli PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    set_property(TARGET hlpr_cli APPEND PROPERTY BUILD_RPATH "${UVNVR_MNN_LIBRARY_DIR}")
  else()
    message(STATUS "tools: hlpr_cli skipped because HyperLPR3 is not enabled")
  endif()
endif()

if(UVNVR_BUILD_SDK)
  add_subdirectory(sdk)
endif()

if(UVNVR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
