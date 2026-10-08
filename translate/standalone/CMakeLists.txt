# Standalone build of video + translate (useful while other modules are in flux):
#   cmake -S translate/standalone -B build-translate -G "Visual Studio 18 2026" -A x64
cmake_minimum_required(VERSION 3.25)
project(pm_translate_standalone LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_library(pm_media_api INTERFACE)
target_include_directories(pm_media_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
add_subdirectory(../../video video)
add_subdirectory(.. translate)
