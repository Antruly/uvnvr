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

if(UVNVR_GB35114_ENABLED)
  add_executable(gb35114_vkek_test gb35114_vkek_test.cpp)
  target_link_libraries(gb35114_vkek_test PRIVATE uvnvr_gb35114)
  add_test(NAME gb35114_vkek COMMAND gb35114_vkek_test)
endif()
