cmake_minimum_required(VERSION 3.25)

# Dependencies come from vcpkg (classic mode, triplet x64-windows):
#   cmake -B build -G "Visual Studio 18 2026" -A x64 ^
#     -DCMAKE_TOOLCHAIN_FILE=%USERPROFILE%/vcpkg/scripts/buildsystems/vcpkg.cmake
project(PhoneMirror VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
  add_compile_options(/utf-8 /MP)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  # Size: function-level linking + dead code / identical COMDAT folding in
  # Release. Deliberately no /GL + /LTCG (dumpbin/editbin cannot read /GL
  # objects; the release checks use dumpbin). The UI targets are switched to
  # /O1 (minimise size) at the end of this file; the real-time paths
  # (core, video, audio, miracast, recorder, translate) keep /O2.
  add_compile_options($<$<CONFIG:Release,MinSizeRel>:/Gy> $<$<CONFIG:Release,MinSizeRel>:/Gw>)
  add_link_options($<$<CONFIG:Release,MinSizeRel>:/OPT:REF> $<$<CONFIG:Release,MinSizeRel>:/OPT:ICF>)
endif()

add_library(pm_media_api INTERFACE)
target_include_directories(pm_media_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Each module owns its own directory and CMakeLists.txt.
foreach(module core video audio recorder miracast android translate share app)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${module}/CMakeLists.txt)
    add_subdirectory(${module})
  endif()
endforeach()

# Size: /O1 for the UI / glue code (window, panels, menus, Android + share
# helpers). Appended after CMake's /O2, so it wins for these targets only.
if(MSVC)
  foreach(t PhoneMirror pm_app_ui pm_app_base pm_app_help pm_app_share pm_app_translate pm_app_capture
            pm_app_connect pm_app_view pm_app_update pm_app_menus pm_android pm_share pm_qrcodegen)
    if(TARGET ${t})
      target_compile_options(${t} PRIVATE $<$<CONFIG:Release,MinSizeRel>:/O1>)
    endif()
  endforeach()
endif()
