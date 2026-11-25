# Unit and integration tests.

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

if(UVNVR_GM_TLS_ENABLED AND NOT WIN32)
  add_executable(gm_tls_test gm_tls_test.cpp)
  target_link_libraries(gm_tls_test PRIVATE uvnvr_gm_tls)
  add_test(NAME gm_tls COMMAND gm_tls_test)
endif()
