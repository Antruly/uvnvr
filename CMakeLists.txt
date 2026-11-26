cmake_minimum_required(VERSION 3.24)

project(uvnvr VERSION 0.20.0 LANGUAGES CXX)

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
option(UVNVR_BUILD_TOOLS "Build developer tools (face_cli, hlpr_cli)" OFF)
set(UVNVR_INSPIREFACE_ROOT "" CACHE PATH
    "InspireFace SDK root containing include/inspireface.h and lib/libInspireFace")
option(UVNVR_ENABLE_GM_CRYPTO "Build the SM2/SM3/SM4 and certificate library (requires OpenSSL)"
       ON)
option(UVNVR_ENABLE_GB35114 "Build the GB 35114 B-level video encryption module" OFF)
# Audit hash chain, signed firmware packages and SM4 data-at-rest encryption.
# Off by default: normal deployments keep their current storage format.
option(UVNVR_ENABLE_SECURE "Build the audit-chain/firmware/storage-encryption module" OFF)
# Self-hosted GB/T 28181 stack: SIP registration/MESSAGE dispatch, MANSCDP
# codec, PS/RTP media and INVITE/BYE dialogs. Off by default: normal
# deployments do not join a GB/T 28181 platform and no existing
# preview/recording/ONVIF path changes when it is enabled.
option(UVNVR_ENABLE_GB28181 "Build the GB/T 28181 SIP, MANSCDP and PS/RTP modules" OFF)
# GB 35114 A-level/B-level video protection: SM3/SM2 frame signatures, VKEK
# negotiation, SM4 stream encryption and the optional GM/T 0024 TLS adapter.
# Off by default; requires the GM crypto library.
option(UVNVR_ENABLE_GB35114 "Build the GB 35114 video security modules" OFF)
# GM/T 0024 (TLCP) and RFC 8998 GM TLS 1.3 signalling adapter. OpenSSL 3 has
# the SM algorithms but not the GB/T 38636 cipher suites; this backend needs
# Tongsuo (Apache-2.0). Off by default so normal deployments are unchanged.
option(UVNVR_ENABLE_GM_TLS "Build the GM/T 0024 signalling adapter (requires Tongsuo)" OFF)
set(UVNVR_GM_TLS_ROOT "" CACHE PATH
    "Tongsuo installation root containing include/openssl/ntls.h and lib[/64]/libssl")
# Object-level channel ACL (GA/T 2155.2 ACC-02). Off by default: without it the
# service keeps the existing role-only authorization and every account sees the
# same catalog.
option(UVNVR_ENABLE_CHANNEL_ACL
       "Build the object-level channel/group authorization (deny-by-default for non-admin users)"
       OFF)
# SM3/SM2 integrity manifest for the startup configuration, plus warnings for
# unknown keys. Off by default: existing deployments keep loading their config
# without an integrity manifest.
option(UVNVR_ENABLE_CONFIG_INTEGRITY
       "Verify the startup configuration with an SM3/SM2 manifest" OFF)
option(UVNVR_ENABLE_HYPERLPR
       "Build the HyperLPR3 plate engine (requires OpenCV and MNN; falls back to the stub)"
       OFF)
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

# A static OpenSSL/Tongsuo build on MinGW needs the Winsock and Win32 helpers
# *after* libcrypto.a on the link line, so hang them off the imported targets
# instead of the executable.
if(WIN32 AND NOT MSVC)
  foreach(_uvnvr_ssl_target OpenSSL::SSL OpenSSL::Crypto)
    if(TARGET ${_uvnvr_ssl_target})
      set_property(TARGET ${_uvnvr_ssl_target} APPEND PROPERTY INTERFACE_LINK_LIBRARIES
        ws2_32 crypt32 advapi32 user32 gdi32 iphlpapi bcrypt)
    endif()
  endforeach()
endif()

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

# HyperLPR3 is optional and must never make the build fail. When it is asked
# for but MNN (or OpenCV, or the platform) is unavailable, the plate engine
# silently stays on the stage-1 stub and the reason is printed here.
set(UVNVR_HYPERLPR_ENABLED 0)
if(UVNVR_ENABLE_HYPERLPR)
  if(NOT UVNVR_AI_ENABLED)
    message(STATUS
      "ai: HyperLPR3 requested but AI/OpenCV is unavailable; falling back to the stub plate engine")
  elseif(MSVC)
    message(STATUS
      "ai: HyperLPR3 needs a MinGW toolchain on Windows; falling back to the stub plate engine")
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
    # MNN keeps the library in a build directory; accept the layout used by
    # the Linux and the MinGW builds so one UVNVR_MNN_ROOT works for both.
    find_library(UVNVR_MNN_LIBRARY NAMES MNN
                 PATHS ${_uvnvr_mnn_roots}
                 PATH_SUFFIXES build-linux build-mingw build lib .
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
    src/ai/ai_rules.cpp
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
  UVNVR_ENABLE_INSPIREFACE=${UVNVR_INSPIREFACE_ENABLED}
  UVNVR_ENABLE_HYPERLPR=${UVNVR_HYPERLPR_ENABLED})

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

# Configuration integrity: an SM3 digest (optionally SM2-signed) manifest is
# checked before the service uses the config file, and unknown keys are
# reported. It depends on the GM crypto library. Like the secure module it is
# isolated behind its own switch so a default build behaves exactly as before.
set(UVNVR_CONFIG_INTEGRITY_ENABLED 0)
if(UVNVR_ENABLE_CONFIG_INTEGRITY)
  if(NOT UVNVR_GM_CRYPTO_ENABLED)
    message(STATUS
      "config_integrity: requested but gm_crypto is unavailable; UVNVR_ENABLE_CONFIG_INTEGRITY is forced OFF")
  else()
    set(UVNVR_CONFIG_INTEGRITY_ENABLED 1)
    add_library(uvnvr_config_integrity STATIC
      src/secure/config_integrity.cpp
    )
    target_include_directories(uvnvr_config_integrity PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(uvnvr_config_integrity PUBLIC uvnvr_gm_crypto OpenSSL::Crypto)
    target_compile_definitions(uvnvr_config_integrity PUBLIC UVNVR_ENABLE_CONFIG_INTEGRITY=1)
    if(MSVC)
      target_compile_options(uvnvr_config_integrity PRIVATE /W4 /permissive- /utf-8)
    else()
      target_compile_options(uvnvr_config_integrity PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    target_link_libraries(uvnvr PRIVATE uvnvr_config_integrity)
    message(STATUS "config_integrity: enabled")
  endif()
else()
  message(STATUS "config_integrity: disabled by UVNVR_ENABLE_CONFIG_INTEGRITY=OFF")
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

# GB/T 28181 stack: SIP registration/Digest/MESSAGE dispatch, MANSCDP codec,
# MPEG-2 PS encapsulation (Appendix C), RTP/RTCP and the INVITE/BYE media
# dialog. The SIP stack is self-hosted (no GPL/LGPL eXosip2). The library is
# not linked into the server yet, so enabling it cannot change the existing
# preview/recording/ONVIF paths.
set(UVNVR_GB28181_ENABLED 0)
if(UVNVR_ENABLE_GB28181)
  set(UVNVR_GB28181_ENABLED 1)
  add_library(uvnvr_gb28181 STATIC
    src/gb28181/sip.cpp
    src/gb28181/manscdp.cpp
    src/gb28181/time.cpp
    src/gb28181/ps.cpp
    src/gb28181/rtp.cpp
    src/gb28181/rtcp.cpp
    src/gb28181/media_session.cpp
    src/gb28181/ts_ps.cpp
    src/gb28181/playback.cpp
    src/gb28181/service.cpp
  )
  target_include_directories(uvnvr_gb28181 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(uvnvr_gb28181 PUBLIC OpenSSL::Crypto ${UVP_UVCPP_TARGET})
  target_compile_definitions(uvnvr_gb28181 PUBLIC UVNVR_ENABLE_GB28181=1)
  if(MSVC)
    target_compile_options(uvnvr_gb28181 PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(uvnvr_gb28181 PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  if(WIN32)
    # sip.cpp/rtp.cpp use Winsock directly; consumers of the static library
    # need this as well, so it is PUBLIC.
    target_link_libraries(uvnvr_gb28181 PUBLIC ws2_32)
  endif()
  # App wires the service only when UVNVR_ENABLE_GB28181 is on; with the
  # default (OFF) the executable and its behaviour are unchanged.
  target_link_libraries(uvnvr PRIVATE uvnvr_gb28181)

  add_executable(sip_reg_cli tools/sip_reg_cli/main.cpp)
  target_link_libraries(sip_reg_cli PRIVATE uvnvr_gb28181)
  set_target_properties(sip_reg_cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  if(UVNVR_LIBUVCPP_ROOT)
    set_property(TARGET sip_reg_cli APPEND PROPERTY BUILD_RPATH "${UVNVR_LIBUVCPP_ROOT}/lib")
  endif()

  add_executable(gb28181_ps_cli tools/gb28181_ps_cli/main.cpp)
  target_link_libraries(gb28181_ps_cli PRIVATE uvnvr_gb28181)
  set_target_properties(gb28181_ps_cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

  add_executable(gb28181_session_cli tools/gb28181_session_cli/main.cpp)
  target_link_libraries(gb28181_session_cli PRIVATE uvnvr_gb28181)
  set_target_properties(gb28181_session_cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  if(UVNVR_LIBUVCPP_ROOT)
    set_property(TARGET gb28181_session_cli APPEND PROPERTY BUILD_RPATH
                 "${UVNVR_LIBUVCPP_ROOT}/lib")
  endif()
  message(STATUS "gb28181: enabled (SIP, MANSCDP, PS/RTP and media dialogs)")
else()
  message(STATUS "gb28181: disabled by UVNVR_ENABLE_GB28181=OFF")
endif()

# GB 35114 module: SM3 access-unit digests and SM2 device signatures (A level),
# SM2/SM3 signature negotiation and SM2-encrypted VKEK distribution with
# rotation/replay protection, plus SM4-CTR encryption of the H.264 elementary
# stream (B level). It is a standalone library and is not linked into the
# server, so enabling it does not change the existing
# preview/recording/RTSP/ONVIF/private-protocol paths.
set(UVNVR_GB35114_ENABLED 0)
if(UVNVR_ENABLE_GB35114)
  if(NOT UVNVR_GM_CRYPTO_ENABLED)
    message(STATUS
      "gb35114: requested but gm_crypto is unavailable; UVNVR_ENABLE_GB35114 is forced OFF")
  else()
    set(UVNVR_GB35114_ENABLED 1)
    add_library(uvnvr_gb35114 STATIC
      src/gb35114/sign.cpp
      src/gb35114/vkek.cpp
      src/gb35114/stream_keys.cpp
      src/gb35114/es_crypto.cpp
    )
    target_include_directories(uvnvr_gb35114 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(uvnvr_gb35114 PUBLIC uvnvr_gm_crypto OpenSSL::Crypto)
    target_compile_definitions(uvnvr_gb35114 PUBLIC UVNVR_ENABLE_GB35114=1)
    if(MSVC)
      target_compile_options(uvnvr_gb35114 PRIVATE /W4 /permissive- /utf-8)
    else()
      target_compile_options(uvnvr_gb35114 PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_executable(gb35114_verify tools/gb35114_verify/main.cpp)
    target_link_libraries(gb35114_verify PRIVATE uvnvr_gb35114)
    set_target_properties(gb35114_verify PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    install(TARGETS gb35114_verify RUNTIME DESTINATION bin)

    add_executable(gb35114_cli tools/gb35114_cli/main.cpp)
    target_link_libraries(gb35114_cli PRIVATE uvnvr_gb35114)
    set_target_properties(gb35114_cli PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    install(TARGETS gb35114_cli RUNTIME DESTINATION bin)

    # A-level -> B-level interop harness. It needs the GB/T 28181 RTP framing
    # for the media path and uses POSIX UDP sockets, so it is built on
    # non-Windows hosts when both compliance modules are enabled.
    if(UVNVR_GB28181_ENABLED AND NOT WIN32)
      add_executable(gb35114_interop tools/gb35114_interop/main.cpp)
      target_link_libraries(gb35114_interop PRIVATE uvnvr_gb35114 uvnvr_gb28181)
      set_target_properties(gb35114_interop PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
      if(UVNVR_LIBUVCPP_ROOT)
        set_property(TARGET gb35114_interop APPEND PROPERTY BUILD_RPATH
                     "${UVNVR_LIBUVCPP_ROOT}/lib")
      endif()
    endif()

    message(STATUS
      "gb35114: enabled (A-level signing + VKEK negotiation + B-level SM4, gb35114_verify/gb35114_cli)")
  endif()
else()
  message(STATUS "gb35114: disabled by UVNVR_ENABLE_GB35114=OFF")
endif()

# GM/T 0024 signalling adapter. This is intentionally a standalone library and
# CLI probe; it is not linked into the uvnvr server, so the existing RTSP,
# ONVIF, private-protocol and HTTPS paths keep using the system OpenSSL. A
# Tongsuo build is found explicitly by UVNVR_GM_TLS_ROOT (or TONGSUO_ROOT) to
# avoid silently linking the system OpenSSL, which has no NTLS cipher suites.
set(UVNVR_GM_TLS_ENABLED 0)
if(UVNVR_ENABLE_GM_TLS)
  set(_uvnvr_gm_tls_roots)
  if(UVNVR_GM_TLS_ROOT)
    list(APPEND _uvnvr_gm_tls_roots "${UVNVR_GM_TLS_ROOT}")
  endif()
  foreach(_uvnvr_gm_tls_env TONGSUO_ROOT TONGSUO_INSTALL_ROOT)
    if(DEFINED ENV{${_uvnvr_gm_tls_env}} AND NOT "$ENV{${_uvnvr_gm_tls_env}}" STREQUAL "")
      list(APPEND _uvnvr_gm_tls_roots "$ENV{${_uvnvr_gm_tls_env}}")
    endif()
  endforeach()

  find_path(UVNVR_GM_TLS_INCLUDE_DIR NAMES openssl/ntls.h
            PATHS ${_uvnvr_gm_tls_roots} PATH_SUFFIXES include NO_DEFAULT_PATH)
  if(UVNVR_GM_TLS_INCLUDE_DIR)
    get_filename_component(_uvnvr_gm_tls_prefix "${UVNVR_GM_TLS_INCLUDE_DIR}" DIRECTORY)
    find_library(UVNVR_GM_TLS_SSL_LIBRARY NAMES ssl libssl
                 PATHS "${_uvnvr_gm_tls_prefix}" PATH_SUFFIXES lib lib64 NO_DEFAULT_PATH)
    find_library(UVNVR_GM_TLS_CRYPTO_LIBRARY NAMES crypto libcrypto
                 PATHS "${_uvnvr_gm_tls_prefix}" PATH_SUFFIXES lib lib64 NO_DEFAULT_PATH)
  endif()

  if(UVNVR_GM_TLS_INCLUDE_DIR AND UVNVR_GM_TLS_SSL_LIBRARY AND UVNVR_GM_TLS_CRYPTO_LIBRARY)
    # Confirm the headers really are Tongsuo (not stock OpenSSL, which lacks
    # the GM/T 0024 cipher suites and the NTLS methods).
    file(READ "${UVNVR_GM_TLS_INCLUDE_DIR}/openssl/opensslv.h" _uvnvr_gm_tls_version_header)
    file(READ "${UVNVR_GM_TLS_INCLUDE_DIR}/openssl/opensslconf.h" _uvnvr_gm_tls_conf_header)
    set(_uvnvr_gm_tls_config_header "")
    if(EXISTS "${UVNVR_GM_TLS_INCLUDE_DIR}/openssl/configuration.h")
      file(READ "${UVNVR_GM_TLS_INCLUDE_DIR}/openssl/configuration.h"
           _uvnvr_gm_tls_config_header)
    endif()
    if(NOT _uvnvr_gm_tls_version_header MATCHES "TONGSUO_VERSION_MAJOR")
      message(STATUS
        "gm_tls: ${UVNVR_GM_TLS_INCLUDE_DIR} is not a Tongsuo installation; UVNVR_ENABLE_GM_TLS is forced OFF")
    elseif(_uvnvr_gm_tls_conf_header MATCHES "OPENSSL_NO_NTLS" OR
           _uvnvr_gm_tls_config_header MATCHES "define OPENSSL_NO_NTLS")
      message(STATUS
        "gm_tls: Tongsuo was built without NTLS; UVNVR_ENABLE_GM_TLS is forced OFF")
    else()
      find_package(Threads REQUIRED)
      set(UVNVR_GM_TLS_ENABLED 1)
      add_library(uvnvr_gm_tls STATIC src/gb35114/tls_gm.cpp)
      target_include_directories(uvnvr_gm_tls
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src" "${UVNVR_GM_TLS_INCLUDE_DIR}")
      target_link_libraries(uvnvr_gm_tls
        PUBLIC "${UVNVR_GM_TLS_SSL_LIBRARY}"
               "${UVNVR_GM_TLS_CRYPTO_LIBRARY}"
               Threads::Threads
               ${CMAKE_DL_LIBS})
      if(WIN32)
        target_link_libraries(uvnvr_gm_tls PUBLIC ws2_32 crypt32 bcrypt)
      endif()
      target_compile_definitions(uvnvr_gm_tls PUBLIC UVNVR_ENABLE_GM_TLS=1)
      if(MSVC)
        target_compile_options(uvnvr_gm_tls PRIVATE /W4 /permissive- /utf-8)
      else()
        target_compile_options(uvnvr_gm_tls PRIVATE -Wall -Wextra -Wpedantic)
      endif()

      add_executable(gm_tls_cli tools/gm_tls_cli/main.cpp)
      target_link_libraries(gm_tls_cli PRIVATE uvnvr_gm_tls)
      set_target_properties(gm_tls_cli PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
      message(STATUS
        "gm_tls: enabled (Tongsuo ${UVNVR_GM_TLS_SSL_LIBRARY}, headers ${UVNVR_GM_TLS_INCLUDE_DIR})")
    endif()
  else()
    message(STATUS
      "gm_tls: Tongsuo not found; set UVNVR_GM_TLS_ROOT to its install prefix. UVNVR_ENABLE_GM_TLS is forced OFF")
  endif()
else()
  message(STATUS "gm_tls: disabled by UVNVR_ENABLE_GM_TLS=OFF")
endif()

# Object-level channel ACL. The static library holds the pure decision logic so
# it can be unit tested without libuvcpp; the tables and enforcement hooks live
# in the server under the same compile definition. When the option is off no
# ACL code, table or route is compiled, so existing deployments are unchanged.
set(UVNVR_CHANNEL_ACL_ENABLED 0)
if(UVNVR_ENABLE_CHANNEL_ACL)
  set(UVNVR_CHANNEL_ACL_ENABLED 1)
  add_library(uvnvr_channel_acl STATIC src/secure/channel_acl.cpp)
  target_include_directories(uvnvr_channel_acl PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(uvnvr_channel_acl PUBLIC UVNVR_ENABLE_CHANNEL_ACL=1)
  if(MSVC)
    target_compile_options(uvnvr_channel_acl PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(uvnvr_channel_acl PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  target_link_libraries(uvnvr PRIVATE uvnvr_channel_acl)
  message(STATUS "channel_acl: enabled")
else()
  message(STATUS "channel_acl: disabled by UVNVR_ENABLE_CHANNEL_ACL=OFF")
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
  # A static Tongsuo/OpenSSL build also pulls in the Winsock/crypto helpers.
  target_link_libraries(uvnvr PRIVATE ws2_32 wtsapi32 userenv advapi32 shell32
                                         crypt32 bcrypt iphlpapi user32 gdi32)
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
  # app.cpp is large enough that MSVC runs out of section entries on arm64.
  target_compile_options(uvnvr PRIVATE /W4 /permissive- /utf-8 /bigobj)
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
