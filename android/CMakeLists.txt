# pm_android: Android phones over Wi-Fi (adb + scrcpy-server v5.0), QR pairing.
# Third-party binaries are NOT in git: third_party/fetch_tools.ps1 downloads
# adb (platform-tools r37.0.1) and scrcpy-server v5.0 with pinned SHA-256.
option(PM_ANDROID_FETCH_TOOLS "Download adb + scrcpy-server at configure time if missing" ON)

set(PM_ANDROID_TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
if(PM_ANDROID_FETCH_TOOLS AND (NOT EXISTS ${PM_ANDROID_TP}/platform-tools/adb.exe OR NOT EXISTS ${PM_ANDROID_TP}/scrcpy/scrcpy-server))
  message(STATUS "pm_android: fetching adb + scrcpy-server")
  execute_process(
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${PM_ANDROID_TP}/fetch_tools.ps1
    RESULT_VARIABLE _pm_fetch_rc)
  if(NOT _pm_fetch_rc EQUAL 0)
    message(WARNING "pm_android: fetch_tools.ps1 failed (${_pm_fetch_rc}); run it manually")
  endif()
endif()

add_library(pm_qrcodegen STATIC third_party/qrcodegen/qrcodegen.cpp third_party/qrcodegen/qrcodegen.hpp)
target_include_directories(pm_qrcodegen PUBLIC third_party/qrcodegen)

add_library(pm_android STATIC
  include/pm/android_source.h
  src/adb.h src/adb.cpp
  src/mdns.h src/mdns.cpp
  src/pairing_qr.h src/pairing_qr.cpp
  src/scrcpy_proto.h src/scrcpy_proto.cpp
  src/scrcpy_session.h src/scrcpy_session.cpp
  src/android_source.cpp
)
target_compile_features(pm_android PUBLIC cxx_std_20)
# Only the VideoWindow::PointerEvent declaration is used (header only).
target_include_directories(pm_android PUBLIC include ${CMAKE_CURRENT_SOURCE_DIR}/../video/include PRIVATE src)
if(TARGET pm_media_api)
  target_link_libraries(pm_android PUBLIC pm_media_api)
endif()
target_link_libraries(pm_android PRIVATE pm_qrcodegen ws2_32 iphlpapi bcrypt)
target_compile_definitions(pm_android PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

# Copies adb + scrcpy-server next to an executable: <bin>/android-tools/.
set(PM_ANDROID_TOOL_FILES
  ${PM_ANDROID_TP}/platform-tools/adb.exe
  ${PM_ANDROID_TP}/platform-tools/AdbWinApi.dll
  ${PM_ANDROID_TP}/platform-tools/AdbWinUsbApi.dll
  ${PM_ANDROID_TP}/platform-tools/NOTICE.txt
  ${PM_ANDROID_TP}/scrcpy/scrcpy-server
)
set(PM_ANDROID_TOOL_FILES ${PM_ANDROID_TOOL_FILES} CACHE INTERNAL "")
function(pm_android_copy_tools target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${target}>/android-tools
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PM_ANDROID_TOOL_FILES} $<TARGET_FILE_DIR:${target}>/android-tools/
    COMMENT "android-tools → $<TARGET_FILE_DIR:${target}>/android-tools")
endfunction()

# Offline tests (no phone needed).
add_executable(pm_android_test tools/pm_android_test.cpp)
target_include_directories(pm_android_test PRIVATE src)
target_link_libraries(pm_android_test PRIVATE pm_android pm_qrcodegen ws2_32)
target_compile_definitions(pm_android_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
  "PM_ANDROID_TOOLS_DIR=L\"${CMAKE_CURRENT_SOURCE_DIR}/tools\"")
# adb stand-in for the AndroidSource state tests (no sockets).
add_executable(pm_android_fake_adb tools/pm_android_fake_adb.cpp)
add_dependencies(pm_android_test pm_android_fake_adb)
# Keys end to end through a real (off-desktop) VideoWindow.
if(TARGET pm_video)
  target_link_libraries(pm_android_test PRIVATE pm_video)
  target_compile_definitions(pm_android_test PRIVATE PM_ANDROID_TEST_VIDEO=1)
endif()
if(EXISTS ${PM_ANDROID_TP}/platform-tools/adb.exe)
  pm_android_copy_tools(pm_android_test)
endif()

# Owner test with a real phone: QR in the console, mirror in a VideoWindow.
if(TARGET pm_video AND TARGET pm_audio)
  add_executable(pm_android_demo tools/pm_android_demo.cpp)
  target_link_libraries(pm_android_demo PRIVATE pm_android pm_video pm_audio)
  file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/../video/src/video_window.cpp _pm_ptr REGEX "VideoWindow::setPointerHandler")
  if(_pm_ptr)
    target_compile_definitions(pm_android_demo PRIVATE PM_HAVE_POINTER_HANDLER=1)
  endif()
  if(EXISTS ${PM_ANDROID_TP}/platform-tools/adb.exe)
    pm_android_copy_tools(pm_android_demo)
  endif()
endif()
