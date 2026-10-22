# pm_remote: 遠端操控 -- a phone browser (same Wi-Fi, or Tailscale)
# sees and controls this PC. DXGI Desktop Duplication -> Media Foundation
# H.264 -> WebRTC (libdatachannel, H264RtpPacketizer) + DataChannel input,
# page + SDP exchange over a LAN-only token HTTP server.
# See launch/_work/remote/spike-report.md.
#
# Needs libdatachannel with media transport (vcpkg classic mode):
#   vcpkg install "libdatachannel[core,srtp]:x64-windows"
# Without it this module is skipped (the rest of the project still builds).
find_package(LibDataChannel CONFIG QUIET)
find_package(nlohmann_json CONFIG QUIET)
find_package(Opus CONFIG QUIET)
if(NOT LibDataChannel_FOUND OR NOT nlohmann_json_FOUND OR NOT Opus_FOUND)
  message(STATUS "pm_remote skipped: libdatachannel[srtp] / nlohmann-json / opus not found (vcpkg)")
  return()
endif()
if(NOT TARGET pm_share)
  message(STATUS "pm_remote skipped: needs pm_share (LAN interface choice, QR)")
  return()
endif()

# The phone page and its scripts, embedded as bytes (same helper as video/res).
#   web/trackpad.js  GPL-3.0-or-later, ported from MoonlightWeb (Bruno Martin)
#   web/vkbd.js      MPL-2.0, ported from noVNC (The noVNC authors)
set(PM_REMOTE_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(PM_REMOTE_WEB_HEADERS)
foreach(entry index.html:remote_page_html:kRemotePageHtml trackpad.js:remote_trackpad_js:kRemoteTrackpadJs
              vkbd.js:remote_vkbd_js:kRemoteVkbdJs p084.js:remote_p084_js:kRemoteP084Js icon.png:remote_icon_png:kRemoteIconPng
              manifest.webmanifest:remote_manifest:kRemoteManifest apple-touch-icon.png:remote_apple_icon_png:kRemoteAppleIconPng
              icon-192.png:remote_icon192_png:kRemoteIcon192Png icon-512.png:remote_icon512_png:kRemoteIcon512Png)
  string(REPLACE ":" ";" parts ${entry})
  list(GET parts 0 file)
  list(GET parts 1 header)
  list(GET parts 2 var)
  add_custom_command(
    OUTPUT ${PM_REMOTE_GEN}/${header}.h
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/web/${file}
            -DOUT=${PM_REMOTE_GEN}/${header}.h -DVAR=${var} -P ${CMAKE_SOURCE_DIR}/video/res/embed.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/${file} ${CMAKE_SOURCE_DIR}/video/res/embed.cmake
    COMMENT "Embedding remote/web/${file}"
    VERBATIM)
  list(APPEND PM_REMOTE_WEB_HEADERS ${PM_REMOTE_GEN}/${header}.h)
endforeach()

# 0.8.4 隱私黑屏: 投投 on the cover (the app's own layers, video/res/toutou: rim + cloud + face).
foreach(entry rim:remote_toutou_rim_png:kRemoteToutouRim cloud:remote_toutou_cloud_png:kRemoteToutouCloud
              face_happy:remote_toutou_face_png:kRemoteToutouFace)
  string(REPLACE ":" ";" parts ${entry})
  list(GET parts 0 file)
  list(GET parts 1 header)
  list(GET parts 2 var)
  add_custom_command(
    OUTPUT ${PM_REMOTE_GEN}/${header}.h
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/video/res/toutou/${file}.png
            -DOUT=${PM_REMOTE_GEN}/${header}.h -DVAR=${var} -P ${CMAKE_SOURCE_DIR}/video/res/embed.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/video/res/toutou/${file}.png ${CMAKE_SOURCE_DIR}/video/res/embed.cmake
    COMMENT "Embedding toutou/${file}.png (privacy cover)"
    VERBATIM)
  list(APPEND PM_REMOTE_WEB_HEADERS ${PM_REMOTE_GEN}/${header}.h)
endforeach()

add_library(pm_remote STATIC
  include/pm/remote.h
  src/util.h src/util.cpp src/gpu_util.h src/file_kind.h
  src/capture.h src/capture.cpp
  src/encoder.h src/encoder.cpp
  src/input.h src/input.cpp src/custom_keys.h src/custom_keys.cpp
  src/http_server.h src/http_server.cpp
  src/audio.h src/audio.cpp src/level.h src/synth_audio.h
  src/tunnel.h src/tunnel.cpp
  src/secure.cpp src/funnel.cpp
  src/auto_quality.h src/rtcp_stats.h src/rtcp_stats.cpp src/cursor_shape.h src/cursor_shape.cpp
  src/pc_files.h src/pc_files.cpp src/privacy_screen.h src/privacy_screen.cpp  # 0.8.4
  src/host.cpp
  web/index.html web/trackpad.js web/vkbd.js web/p084.js
  ${PM_REMOTE_WEB_HEADERS}
)
target_compile_features(pm_remote PUBLIC cxx_std_20)
target_include_directories(pm_remote PUBLIC include PRIVATE src ${PM_REMOTE_GEN})
target_link_libraries(pm_remote
  PRIVATE pm_share LibDataChannel::LibDataChannel nlohmann_json::nlohmann_json Opus::opus
          d3d11 dxgi d3dcompiler mfplat mfuuid mf strmiids ole32 ws2_32 iphlpapi bcrypt winmm pdh winhttp windowscodecs)

if(MSVC)
  target_compile_options(pm_remote PRIVATE /W4 /utf-8)
  target_compile_definitions(pm_remote PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

add_executable(pm_remote_spike tools/pm_remote_spike.cpp)
target_link_libraries(pm_remote_spike PRIVATE pm_remote pm_share ole32)
if(MSVC)
  target_compile_options(pm_remote_spike PRIVATE /W4 /utf-8)
  target_compile_definitions(pm_remote_spike PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

# 0.8.3.1: 常駐外網 Funnel start-up retry against a stubbed tailscale CLI (no process is run).
add_executable(pm_funnel_test tools/funnel_retry_test.cpp)
target_link_libraries(pm_funnel_test PRIVATE pm_remote)
target_include_directories(pm_funnel_test PRIVATE src)  # util.h: the log handler
if(MSVC)
  target_compile_options(pm_funnel_test PRIVATE /W4 /utf-8)
  target_compile_definitions(pm_funnel_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

# 0.8.3.5: 允許 once per 臨時帳號 / temporary-address login, not per connection (approval_memory.h): a model of
# the host's gate + checks that host.cpp / index.html do it that way. No network, no UI.
add_executable(pm_approval_test tools/approval_test.cpp src/approval_memory.h)
target_include_directories(pm_approval_test PRIVATE src include)  # 0.8.3.6: pm/remote.h (ApprovalPolicy)
target_compile_definitions(pm_approval_test PRIVATE PM_REMOTE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
  target_compile_options(pm_approval_test PRIVATE /W4 /utf-8)
endif()

# 0.8.3.2: the phone's sound level (AGC / noise gate / limiter) on synthetic PCM, offline:
# before (0.8.3.1) / after, through Opus and back. No audio device is opened.
add_executable(pm_level_test tools/level_test.cpp src/level.h src/synth_audio.h)
target_link_libraries(pm_level_test PRIVATE Opus::opus)
target_include_directories(pm_level_test PRIVATE src)
if(MSVC)
  target_compile_options(pm_level_test PRIVATE /W4 /utf-8)
  target_compile_definitions(pm_level_test PRIVATE NOMINMAX)
endif()

# 0.8.3.5: where a point of the picture lands (coords.h on single / multi-monitor layouts, the
# injector with its SendInput stub). No window, never a real mouse / key.
add_executable(pm_input_test tools/input_test.cpp src/coords.h)
target_link_libraries(pm_input_test PRIVATE pm_remote)
target_include_directories(pm_input_test PRIVATE src)
if(MSVC)
  target_compile_options(pm_input_test PRIVATE /W4 /utf-8)
  target_compile_definitions(pm_input_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

# 0.8.3.2: a FAKE tailscale CLI for the tests (an enable page and never exiting, silence, ...).
# Only ever started by pm_funnel_test / --dev runs that point TailscaleFunnel at it; it is not tailscale.
add_executable(pm_fake_tailscale tools/fake_tailscale.cpp)
if(MSVC)
  target_compile_options(pm_fake_tailscale PRIVATE /W4 /utf-8)
endif()
add_dependencies(pm_funnel_test pm_fake_tailscale)

# 0.8.3.5 files: the 傳檔 permission table (主帳號 / 臨時帳號 / 僅觀看) and the new-account defaults; pure logic.
add_executable(pm_fileperm_test tools/fileperm_test.cpp src/file_perm.h ${CMAKE_SOURCE_DIR}/app/guest_rights.h)
target_include_directories(pm_fileperm_test PRIVATE src ${CMAKE_SOURCE_DIR}/app)
if(MSVC)
  target_compile_options(pm_fileperm_test PRIVATE /W4 /utf-8)
endif()

# 0.8.4 自動畫質: the ladder (auto_quality.h) on a model link, offline. No network, no window, no sound.
add_executable(pm_autoq_test tools/autoq_test.cpp src/auto_quality.h)
target_include_directories(pm_autoq_test PRIVATE src)
if(MSVC)
  target_compile_options(pm_autoq_test PRIVATE /W4 /utf-8)
endif()

# 0.8.3.7: the stats line's GPU percentages from PDH's raw values (gpu_util.h): the owner's 36713195488442.6 %
# reproduced with 0.8.3.6's arithmetic, then 0..100 % or hidden; one live PDH sample of the test itself.
add_executable(pm_gpu_test tools/gpu_test.cpp src/gpu_util.h)
target_link_libraries(pm_gpu_test PRIVATE pm_remote)
target_include_directories(pm_gpu_test PRIVATE src)
if(MSVC)
  target_compile_options(pm_gpu_test PRIVATE /W4 /utf-8)
  target_compile_definitions(pm_gpu_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

# 0.8.3.7: a file from the phone: an image's extension follows its bytes (file_kind.h: HEIC / JPEG / PNG ...); pure logic.
add_executable(pm_filekind_test tools/filekind_test.cpp src/file_kind.h)
target_include_directories(pm_filekind_test PRIVATE src)
if(MSVC)
  target_compile_options(pm_filekind_test PRIVATE /W4 /utf-8)
endif()

# 0.8.4 隱私黑屏 / 電腦的檔案 (feat-084-privacy): the path rules (.. / links / junctions / device names,
# a sandbox under %TEMP%), Esc x3, and the cover against the capture (a 2x2 window at the primary
# monitor's top-left pixel for a moment: never a full-screen cover). The exe is also the cover's child.
add_executable(pm_privacy_test tools/privacy_test.cpp)
target_link_libraries(pm_privacy_test PRIVATE pm_remote d3d11 dxgi)
target_include_directories(pm_privacy_test PRIVATE src)
if(MSVC)
  target_compile_options(pm_privacy_test PRIVATE /W4 /utf-8)
  target_compile_definitions(pm_privacy_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()
