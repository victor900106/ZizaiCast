# Standalone build of the recorder module only:
#   cmake -S recorder/standalone -B build-rec -G "Visual Studio 18 2026" -A x64
cmake_minimum_required(VERSION 3.25)
project(PhoneMirrorRecorder LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if(MSVC)
  add_compile_options(/utf-8 /MP)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

get_filename_component(PM_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
add_subdirectory(${PM_ROOT}/recorder ${CMAKE_BINARY_DIR}/recorder)
