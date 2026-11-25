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
option(UVNVR_ENABLE_INSPIREFACE
       "Build the InspireFace face engine (requires OpenCV and the InspireFace SDK; falls back to the stub)"
       OFF)
option(UVNVR_BUILD_TOOLS "Build developer tools (face_cli)" OFF)
set(UVNVR_INSPIREFACE_ROOT "" CACHE PATH
    "InspireFace SDK root containing include/inspireface.h and lib/libInspireFace")
option(UVNVR_ENABLE_GM_CRYPTO "Build the SM2/SM3/SM4 and certificate library (requires OpenSSL)"
       ON)
# Audit hash chain, signed firmware packages and SM4 data-at-rest encryption.
# Off by default: normal deployments keep their current storage format.
option(UVNVR_ENABLE_SECURE "Build the audit-chain/firmware/storage-encryption module" OFF)
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

# InspireFace is optional and must never make the build fail. When it is asked
# for but the SDK (or OpenCV) is unavailable, the face engine stays on the
# stage-1 stub and the reason is printed here.
set(UVNVR_INSPIREFACE_ENABLED 0)
if(UVNVR_ENABLE_INSPIREFACE)
  if(NOT UVNVR_AI_ENABLED)
    message(STATUS
      "ai: InspireFace requested but AI/OpenCV is unavailable; falling back to the stub face engine")
  else()
    set(_uvnvr_inspireface_roots)
    if(UVNVR_INSPIREFACE_ROOT)
      list(APPEND _uvnvr_inspireface_roots "${UVNVR_INSPIREFACE_ROOT}")
    endif()
    foreach(_uvnvr_env INSPIREFACE_ROOT INSPIREFACE_SDK_ROOT)
      if(DEFINED ENV{${_uvnvr_env}} AND NOT "$ENV{${_uvnvr_env}}" STREQUAL "")
        list(APPEND _uvnvr_inspireface_roots "$ENV{${_uvnvr_env}}")
      endif()
    endforeach()

    find_path(UVNVR_INSPIREFACE_INCLUDE_DIR NAMES inspireface.h
              PATHS ${_uvnvr_inspireface_roots}
              PATH_SUFFIXES include InspireFace/include NO_DEFAULT_PATH)
    if(NOT UVNVR_INSPIREFACE_INCLUDE_DIR)
      find_path(UVNVR_INSPIREFACE_INCLUDE_DIR NAMES inspireface.h)
    endif()
    find_library(UVNVR_INSPIREFACE_LIBRARY
                 NAMES InspireFace libInspireFace inspireface
                 PATHS ${_uvnvr_inspireface_roots}
                 PATH_SUFFIXES lib InspireFace/lib NO_DEFAULT_PATH)
    if(NOT UVNVR_INSPIREFACE_LIBRARY)
      find_library(UVNVR_INSPIREFACE_LIBRARY NAMES InspireFace libInspireFace inspireface)
    endif()

    if(UVNVR_INSPIREFACE_INCLUDE_DIR AND UVNVR_INSPIREFACE_LIBRARY)
      set(UVNVR_INSPIREFACE_ENABLED 1)
      get_filename_component(UVNVR_INSPIREFACE_LIBRARY_DIR
                             "${UVNVR_INSPIREFACE_LIBRARY}" DIRECTORY)
      get_filename_component(UVNVR_INSPIREFACE_LIBRARY_NAME
                             "${UVNVR_INSPIREFACE_LIBRARY}" NAME)
      message(STATUS "ai: InspireFace enabled (${UVNVR_INSPIREFACE_LIBRARY})")
    else()
      if(NOT UVNVR_INSPIREFACE_INCLUDE_DIR)
        set(_uvnvr_inspireface_reason
            "inspireface.h was not found; set UVNVR_INSPIREFACE_ROOT to the SDK root")
      else()
        set(_uvnvr_inspireface_reason
            "libInspireFace was not found under ${_uvnvr_inspireface_roots}; set UVNVR_INSPIREFACE_ROOT")
      endif()
      message(STATUS
        "ai: InspireFace requested but ${_uvnvr_inspireface_reason}; falling back to the stub face engine")
    endif()
  endif()
else()
  message(STATUS
    "ai: InspireFace disabled by UVNVR_ENABLE_INSPIREFACE=OFF; the face engine is the stage-1 stub")
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
  src/auth/session_revocation.cpp
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
    src/ai/snapshot_quota.cpp
  )
  target_include_directories(uvnvr PRIVATE ${OpenCV_INCLUDE_DIRS})
  target_link_libraries(uvnvr PRIVATE ${OpenCV_LIBS})
endif()

if(UVNVR_INSPIREFACE_ENABLED)
  target_sources(uvnvr PRIVATE src/ai/engine_inspireface.cpp)
  target_include_directories(uvnvr PRIVATE "${UVNVR_INSPIREFACE_INCLUDE_DIR}")
  target_link_libraries(uvnvr PRIVATE "${UVNVR_INSPIREFACE_LIBRARY}")
  set_property(TARGET uvnvr APPEND PROPERTY BUILD_RPATH "${UVNVR_INSPIREFACE_LIBRARY_DIR}")
  if(UVNVR_INSPIREFACE_LIBRARY_NAME MATCHES "\\.(so|dylib)(\\..*)?$" OR
     UVNVR_INSPIREFACE_LIBRARY_NAME MATCHES "\\.dll$")
    install(FILES "${UVNVR_INSPIREFACE_LIBRARY}" DESTINATION lib)
  endif()
endif()

target_compile_definitions(uvnvr PRIVATE
  UVNVR_ENABLE_AI=${UVNVR_AI_ENABLED}
  UVNVR_ENABLE_INSPIREFACE=${UVNVR_INSPIREFACE_ENABLED})

# GM crypto library: SM2/SM3/SM4 primitives plus device certificate handling.
# It is a standalone library, not linked into the server, so no existing
# authentication path changes. OpenSSL 3 provides SM2/SM3/SM4; when it is
# unavailable the module is skipped and the rest of the build is unaffected.
set(UVNVR_GM_CRYPTO_ENABLED 0)
if(UVNVR_ENABLE_GM_CRYPTO)
  find_package(OpenSSL 3.0 QUIET COMPONENTS Crypto)
  if(OpenSSL_FOUND AND TARGET OpenSSL::Crypto)
    set(UVNVR_GM_CRYPTO_ENABLED 1)
    add_library(uvnvr_gm_crypto STATIC
      src/crypto/sm.cpp
      src/crypto/cert.cpp
    )
    target_include_directories(uvnvr_gm_crypto PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(uvnvr_gm_crypto PUBLIC OpenSSL::Crypto)
    target_compile_definitions(uvnvr_gm_crypto PUBLIC UVNVR_ENABLE_GM_CRYPTO=1)
    if(MSVC)
      target_compile_options(uvnvr_gm_crypto PRIVATE /W4 /permissive- /utf-8)
    else()
      target_compile_options(uvnvr_gm_crypto PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    message(STATUS "gm_crypto: enabled")
  else()
    message(STATUS
      "gm_crypto: OpenSSL 3 Crypto not found; UVNVR_ENABLE_GM_CRYPTO is forced OFF")
  endif()
else()
  message(STATUS "gm_crypto: disabled by UVNVR_ENABLE_GM_CRYPTO=OFF")
endif()

# Secure module: audit hash chain, firmware signature verification and
# SM4 data-at-rest encryption. It depends on the GM crypto library; asking for
# it without crypto is a configuration error rather than a silent downgrade.
set(UVNVR_SECURE_ENABLED 0)
if(UVNVR_ENABLE_SECURE)
  if(NOT UVNVR_GM_CRYPTO_ENABLED)
    message(STATUS
      "secure: requested but gm_crypto is unavailable; UVNVR_ENABLE_SECURE is forced OFF")
  else()
    set(UVNVR_SECURE_ENABLED 1)
    add_library(uvnvr_secure STATIC
      src/secure/audit_chain.cpp
      src/secure/audit_store.cpp
      src/secure/firmware.cpp
      src/secure/storage_crypto.cpp
    )
    target_include_directories(uvnvr_secure PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(uvnvr_secure PUBLIC uvnvr_gm_crypto OpenSSL::Crypto ${UVP_UVCPP_TARGET})
    target_compile_definitions(uvnvr_secure PUBLIC UVNVR_ENABLE_SECURE=1)
    if(MSVC)
      target_compile_options(uvnvr_secure PRIVATE /W4 /permissive- /utf-8)
    else()
      target_compile_options(uvnvr_secure PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    target_link_libraries(uvnvr PRIVATE uvnvr_secure)
    message(STATUS "secure: enabled")
  endif()
else()
  message(STATUS "secure: disabled by UVNVR_ENABLE_SECURE=OFF")
endif()

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
  if(UVNVR_INSPIREFACE_ENABLED)
    # Standalone smoke/benchmark tool for the same face engine the AI service
    # uses, so a model pack can be validated before starting the service.
    add_executable(face_cli
      tools/face_cli/main.cpp
      src/ai/engine_inspireface.cpp
    )
    target_include_directories(face_cli PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/ai"
      "${UVNVR_INSPIREFACE_INCLUDE_DIR}"
      ${OpenCV_INCLUDE_DIRS})
    target_compile_definitions(face_cli PRIVATE UVNVR_ENABLE_INSPIREFACE=1)
    target_link_libraries(face_cli PRIVATE "${UVNVR_INSPIREFACE_LIBRARY}" ${OpenCV_LIBS})
    set_target_properties(face_cli PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    set_property(TARGET face_cli APPEND PROPERTY BUILD_RPATH
                 "${UVNVR_INSPIREFACE_LIBRARY_DIR}")
  else()
    message(STATUS "tools: face_cli skipped because InspireFace is not enabled")
  endif()
endif()

if(UVNVR_BUILD_SDK)
  add_subdirectory(sdk)
endif()

if(UVNVR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
