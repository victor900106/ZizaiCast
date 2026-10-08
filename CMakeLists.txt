cmake_minimum_required(VERSION 3.25)

# Dependencies come from vcpkg (classic mode, triplet x64-windows):
#   cmake -B build -G "Visual Studio 18 2026" -A x64 ^
#     -DCMAKE_TOOLCHAIN_FILE=C:/Users/victo/vcpkg/scripts/buildsystems/vcpkg.cmake
project(PhoneMirror VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
  add_compile_options(/utf-8 /MP)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

add_library(pm_media_api INTERFACE)
target_include_directories(pm_media_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Each module owns its own directory and CMakeLists.txt.
foreach(module core video audio recorder miracast android app)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${module}/CMakeLists.txt)
    add_subdirectory(${module})
  endif()
endforeach()
