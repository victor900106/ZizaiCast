# Standalone build of the audio module only (independent of the other
# modules' CMakeLists):
#   cmake -S audio/standalone -B build-audio -G "Visual Studio 18 2026" -A x64 ^
#     -DCMAKE_TOOLCHAIN_FILE=C:/Users/victo/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake_minimum_required(VERSION 3.25)
project(PhoneMirrorAudio LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if(MSVC)
  add_compile_options(/utf-8 /MP)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

get_filename_component(PM_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
add_library(pm_media_api INTERFACE)
target_include_directories(pm_media_api INTERFACE ${PM_ROOT}/include)

add_subdirectory(${PM_ROOT}/audio ${CMAKE_BINARY_DIR}/audio)
