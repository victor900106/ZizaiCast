# pm_audio: AirPlay audio decode (FFmpeg libavcodec AAC / Apple ALAC) + WASAPI
# playback.
#
# AAC (ct=8 ELD, ct=4 LC) uses FFmpeg's native "aac" decoder, LGPL-only build:
#   vcpkg install "ffmpeg[core,avcodec]:x64-windows" --overlay-ports=tools/vcpkg-overlay
# (the overlay port builds only the native aac decoder/parser + avutil)
# (no gpl/nonfree features; the player logs avcodec_license() once). Shipped
# DLLs: avcodec-*.dll + avutil-*.dll. fdk-aac (GPL-incompatible license) is NOT
# linked into pm_audio; only the unshipped pm_audio_test uses its *encoder*
# (PM_TEST_FDK) to synthesize AAC-ELD/LC test vectors.
find_package(FFMPEG REQUIRED)

# Apple ALAC codec (vcpkg port `alac`: static libalac.lib, headers in
# include/alac, no CMake config). Optional: without it ct=2 is rejected with a
# log line.
find_path(PM_ALAC_INCLUDE_DIR NAMES alac/ALACDecoder.h)
if(PM_ALAC_INCLUDE_DIR AND NOT TARGET pm_alac)
  get_filename_component(_pm_alac_root "${PM_ALAC_INCLUDE_DIR}" DIRECTORY)
  find_library(PM_ALAC_LIBRARY_RELEASE NAMES libalac PATHS "${_pm_alac_root}/lib" NO_DEFAULT_PATH)
  find_library(PM_ALAC_LIBRARY_DEBUG NAMES libalac PATHS "${_pm_alac_root}/debug/lib" NO_DEFAULT_PATH)
  if(PM_ALAC_LIBRARY_RELEASE)
    if(NOT PM_ALAC_LIBRARY_DEBUG)
      set(PM_ALAC_LIBRARY_DEBUG "${PM_ALAC_LIBRARY_RELEASE}")
    endif()
    add_library(pm_alac STATIC IMPORTED)
    set_target_properties(pm_alac PROPERTIES
      IMPORTED_CONFIGURATIONS "RELEASE;DEBUG"
      IMPORTED_LOCATION_RELEASE "${PM_ALAC_LIBRARY_RELEASE}"
      IMPORTED_LOCATION_DEBUG "${PM_ALAC_LIBRARY_DEBUG}"
      MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
      MAP_IMPORTED_CONFIG_MINSIZEREL Release
      INTERFACE_INCLUDE_DIRECTORIES "${PM_ALAC_INCLUDE_DIR}/alac"
      INTERFACE_COMPILE_DEFINITIONS "PM_HAVE_ALAC=1;TARGET_OS_WIN32=1")
  endif()
endif()

add_library(pm_audio STATIC
  include/pm/audio_player.h
  src/audio_decoder.h
  src/audio_decoder.cpp
  src/audio_player.cpp
)
target_compile_features(pm_audio PUBLIC cxx_std_20)
target_include_directories(pm_audio PUBLIC include PRIVATE src)
if(TARGET pm_media_api)
  target_link_libraries(pm_audio PUBLIC pm_media_api)
endif()
target_include_directories(pm_audio PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(pm_audio PRIVATE ${FFMPEG_LIBRARIES} ole32 avrt)
target_compile_definitions(pm_audio PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(TARGET pm_alac)
  message(STATUS "pm_audio: ALAC enabled (${PM_ALAC_LIBRARY_RELEASE})")
  target_sources(pm_audio PRIVATE src/alac_decoder.cpp)
  target_link_libraries(pm_audio PRIVATE pm_alac)
else()
  message(STATUS "pm_audio: vcpkg port 'alac' not found, ALAC (ct=2) disabled")
endif()

add_executable(pm_audio_test tools/pm_audio_test.cpp)
target_include_directories(pm_audio_test PRIVATE src ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(pm_audio_test PRIVATE pm_audio ${FFMPEG_LIBRARIES} winmm ole32)
# Test-only: fdk-aac's encoder synthesizes the AAC-ELD/LC vectors and its
# decoder is the reference for --bench. pm_audio_test is never shipped.
option(PM_TEST_FDK "pm_audio_test: use fdk-aac (encoder + reference decoder); test target only" ON)
if(PM_TEST_FDK)
  find_package(fdk-aac CONFIG QUIET)
  if(TARGET FDK-AAC::fdk-aac)
    target_link_libraries(pm_audio_test PRIVATE FDK-AAC::fdk-aac)
    target_compile_definitions(pm_audio_test PRIVATE PM_TEST_FDK=1)
  else()
    message(STATUS "pm_audio_test: fdk-aac not found, AAC test vectors disabled (ALAC only)")
  endif()
endif()
if(TARGET pm_alac)
  # The test also uses Apple's ALAC *encoder* to produce ct=2 frames.
  target_link_libraries(pm_audio_test PRIVATE pm_alac)
endif()
