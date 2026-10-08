# pm_recorder: MP4 recorder (Media Foundation Sink Writer: H.264 + AAC-LC).
add_library(pm_recorder STATIC
  include/pm/recorder.h
  src/recorder.cpp
)
target_compile_features(pm_recorder PUBLIC cxx_std_20)
target_include_directories(pm_recorder PUBLIC include)
target_link_libraries(pm_recorder PRIVATE mfplat mfreadwrite mfuuid ole32)
target_compile_definitions(pm_recorder PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(pm_recorder_test tools/pm_recorder_test.cpp)
target_link_libraries(pm_recorder_test PRIVATE pm_recorder winmm pdh mfplat mfreadwrite mfuuid ole32)
target_compile_definitions(pm_recorder_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
