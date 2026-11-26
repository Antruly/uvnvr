# Unit and integration tests.

add_executable(session_revocation_test
  session_revocation_test.cpp
  "${CMAKE_SOURCE_DIR}/src/auth/session_revocation.cpp"
)
target_include_directories(session_revocation_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(session_revocation_test PRIVATE ${UVP_UVCPP_TARGET})
if(UVNVR_LIBUVCPP_ROOT AND NOT WIN32)
  set_target_properties(session_revocation_test PROPERTIES
    BUILD_RPATH "${UVNVR_LIBUVCPP_ROOT}/lib")
endif()
add_test(NAME session_revocation COMMAND session_revocation_test)

add_executable(ai_plate_post_test
  ai_plate_post_test.cpp
  "${CMAKE_SOURCE_DIR}/src/ai/plate_post.cpp"
)
target_include_directories(ai_plate_post_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
add_test(NAME ai_plate_post COMMAND ai_plate_post_test)

add_executable(ai_snapshot_quota_test
  ai_snapshot_quota_test.cpp
  "${CMAKE_SOURCE_DIR}/src/ai/snapshot_quota.cpp"
)
target_include_directories(ai_snapshot_quota_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
add_test(NAME ai_snapshot_quota COMMAND ai_snapshot_quota_test)

if(NOT WIN32)
  # The fake camera uses POSIX sockets; the ONVIF code itself is portable.
  find_package(Threads REQUIRED)
  add_executable(onvif_ptz_test
    onvif_ptz_test.cpp
    "${CMAKE_SOURCE_DIR}/src/onvif.cpp"
  )
  target_include_directories(onvif_ptz_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(onvif_ptz_test PRIVATE OpenSSL::Crypto Threads::Threads
                                               ${UVP_UVCPP_TARGET})
  # onvif.cpp calls uv_interface_addresses() during ONVIF discovery.
  if(TARGET PkgConfig::LIBUV)
    target_link_libraries(onvif_ptz_test PRIVATE PkgConfig::LIBUV)
  else()
    find_library(UVNVR_TEST_UV_LIBRARY NAMES uv libuv)
    if(UVNVR_TEST_UV_LIBRARY)
      target_link_libraries(onvif_ptz_test PRIVATE "${UVNVR_TEST_UV_LIBRARY}")
    endif()
  endif()
  add_test(NAME onvif_ptz COMMAND onvif_ptz_test)
endif()

if(UVNVR_GM_CRYPTO_ENABLED)
  add_executable(gm_crypto_test gm_crypto_test.cpp)
  target_link_libraries(gm_crypto_test PRIVATE uvnvr_gm_crypto)
  add_test(NAME gm_crypto COMMAND gm_crypto_test)
endif()

if(UVNVR_SECURE_ENABLED)
  add_executable(secure_test secure_test.cpp)
  target_link_libraries(secure_test PRIVATE uvnvr_secure)
  add_test(NAME secure COMMAND secure_test)
endif()

if(UVNVR_GB28181_ENABLED)
  add_executable(sip_test sip_test.cpp)
  target_link_libraries(sip_test PRIVATE uvnvr_gb28181)
  if(UVNVR_LIBUVCPP_ROOT)
    set_property(TARGET sip_test APPEND PROPERTY BUILD_RPATH "${UVNVR_LIBUVCPP_ROOT}/lib")
  endif()
  add_test(NAME sip COMMAND sip_test)

  add_executable(gb28181_manscdp_test gb28181_manscdp_test.cpp)
  target_link_libraries(gb28181_manscdp_test PRIVATE uvnvr_gb28181)
  if(UVNVR_LIBUVCPP_ROOT)
    set_property(TARGET gb28181_manscdp_test APPEND PROPERTY
                 BUILD_RPATH "${UVNVR_LIBUVCPP_ROOT}/lib")
  endif()
  add_test(NAME gb28181_manscdp COMMAND gb28181_manscdp_test)

  add_executable(gb28181_ps_test gb28181_ps_test.cpp)
  target_link_libraries(gb28181_ps_test PRIVATE uvnvr_gb28181)
  add_test(NAME gb28181_ps COMMAND gb28181_ps_test)

  add_executable(gb28181_rtcp_test gb28181_rtcp_test.cpp)
  target_link_libraries(gb28181_rtcp_test PRIVATE uvnvr_gb28181)
  add_test(NAME gb28181_rtcp COMMAND gb28181_rtcp_test)

  add_executable(gb28181_media_session_test gb28181_media_session_test.cpp)
  target_link_libraries(gb28181_media_session_test PRIVATE uvnvr_gb28181)
  add_test(NAME gb28181_media_session COMMAND gb28181_media_session_test)

  add_executable(gb28181_record_playback_test gb28181_record_playback_test.cpp)
  target_link_libraries(gb28181_record_playback_test PRIVATE uvnvr_gb28181)
  add_test(NAME gb28181_record_playback COMMAND gb28181_record_playback_test)
endif()

if(UVNVR_GB35114_ENABLED)
  add_executable(gb35114_sign_test gb35114_sign_test.cpp)
  target_link_libraries(gb35114_sign_test PRIVATE uvnvr_gb35114)
  add_test(NAME gb35114_sign COMMAND gb35114_sign_test)

  add_executable(gb35114_vkek_test gb35114_vkek_test.cpp)
  target_link_libraries(gb35114_vkek_test PRIVATE uvnvr_gb35114)
  add_test(NAME gb35114_vkek COMMAND gb35114_vkek_test)

  add_executable(gb35114_test gb35114_test.cpp)
  target_link_libraries(gb35114_test PRIVATE uvnvr_gb35114)
  add_test(NAME gb35114 COMMAND gb35114_test)
endif()

if(UVNVR_GM_TLS_ENABLED AND NOT WIN32)
  add_executable(gm_tls_test gm_tls_test.cpp)
  target_link_libraries(gm_tls_test PRIVATE uvnvr_gm_tls)
  add_test(NAME gm_tls COMMAND gm_tls_test)
endif()
