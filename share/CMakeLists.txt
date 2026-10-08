# pm_share: 傳到手機 over the LAN (QR code → a page on the phone). A tiny
# token-protected HTTP server serving only the files the user shared, for 10
# minutes. QR codes via nayuki qrcodegen (pm_qrcodegen, vendored in android/).
# See docs/share.md.
if(NOT TARGET pm_qrcodegen AND EXISTS ${CMAKE_SOURCE_DIR}/android/third_party/qrcodegen/qrcodegen.cpp)
  add_library(pm_qrcodegen STATIC ${CMAKE_SOURCE_DIR}/android/third_party/qrcodegen/qrcodegen.cpp)
  target_include_directories(pm_qrcodegen PUBLIC ${CMAKE_SOURCE_DIR}/android/third_party/qrcodegen)
endif()

add_library(pm_share STATIC
  include/pm/share_server.h
  src/share_server.cpp
  src/page.h src/page.cpp
  src/lan.cpp
)
target_compile_features(pm_share PUBLIC cxx_std_20)
target_include_directories(pm_share PUBLIC include PRIVATE src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(pm_share PRIVATE pm_qrcodegen ws2_32 iphlpapi bcrypt)
target_compile_definitions(pm_share PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

# Offline tests: HTTP server on 127.0.0.1 (token, 404, ranges, expiry, stop),
# QR decoded back by OpenCV, and AndroidSource::pushToGallery against a fake
# adb.exe (fake_adb.cpp). `pm_share_test --serve N [--en]` serves sample
# files on 127.0.0.1 for N seconds (page screenshots).
add_executable(pm_share_fake_adb tools/fake_adb.cpp)
target_compile_definitions(pm_share_fake_adb PRIVATE UNICODE _UNICODE)
add_executable(pm_share_test tools/pm_share_test.cpp)
target_include_directories(pm_share_test PRIVATE src)
target_link_libraries(pm_share_test PRIVATE pm_share ws2_32)
if(TARGET pm_android)
  target_link_libraries(pm_share_test PRIVATE pm_android)
  target_compile_definitions(pm_share_test PRIVATE PM_SHARE_TEST_ANDROID=1)
endif()
add_dependencies(pm_share_test pm_share_fake_adb)
target_compile_definitions(pm_share_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
  "PM_VERIFY_QR=L\"${CMAKE_SOURCE_DIR}/android/tools/verify_qr.py\"")
