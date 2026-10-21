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
              vkbd.js:remote_vkbd_js:kRemoteVkbdJs icon.png:remote_icon_png:kRemoteIconPng
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

add_library(pm_remote STATIC
  include/pm/remote.h
  src/util.h src/util.cpp
  src/capture.h src/capture.cpp
  src/encoder.h src/encoder.cpp
  src/input.h src/input.cpp
  src/http_server.h src/http_server.cpp
  src/audio.h src/audio.cpp
  src/tunnel.h src/tunnel.cpp
  src/secure.cpp src/funnel.cpp
  src/host.cpp
  web/index.html web/trackpad.js web/vkbd.js
  ${PM_REMOTE_WEB_HEADERS}
)
target_compile_features(pm_remote PUBLIC cxx_std_20)
target_include_directories(pm_remote PUBLIC include PRIVATE src ${PM_REMOTE_GEN})
target_link_libraries(pm_remote
  PRIVATE pm_share LibDataChannel::LibDataChannel nlohmann_json::nlohmann_json Opus::opus
          d3d11 dxgi d3dcompiler mfplat mfuuid mf strmiids ole32 ws2_32 iphlpapi bcrypt winmm pdh winhttp)

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
