# Standalone build of the video module only (useful while other modules are
# in flux):  cmake -S video/standalone -B build-video -G "Visual Studio 18 2026" -A x64
cmake_minimum_required(VERSION 3.25)
project(pm_video_standalone LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_subdirectory(.. video)
