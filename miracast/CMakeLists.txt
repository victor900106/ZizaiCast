# pm_miracast: Android (Miracast) receiver on Windows.Media.Miracast +
# MediaPlayer frame server -> pm::VideoWindow::submitBgraFrame.
# C++/WinRT headers come from the Windows SDK (10.0.19041+); link windowsapp.
# See docs/miracast.md.

add_library(pm_miracast STATIC
  include/pm/miracast_receiver.h
  src/frame_pump.h
  src/frame_pump.cpp
  src/miracast_receiver.cpp
  src/wireless_display.h
  src/wireless_display.cpp
)
target_include_directories(pm_miracast PUBLIC include PRIVATE src)
target_link_libraries(pm_miracast PUBLIC pm_video PRIVATE windowsapp d3d11 dxgi ole32)
target_compile_features(pm_miracast PUBLIC cxx_std_20)
if(MSVC)
  target_compile_options(pm_miracast PRIVATE /W4 /utf-8 /EHsc /bigobj)
  target_compile_definitions(pm_miracast PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

add_executable(pm_miracast_test tools/pm_miracast_test.cpp)
target_include_directories(pm_miracast_test PRIVATE src)
target_link_libraries(pm_miracast_test PRIVATE pm_miracast pm_video windowsapp)
if(MSVC)
  target_compile_options(pm_miracast_test PRIVATE /W4 /utf-8 /EHsc /bigobj)
  target_compile_definitions(pm_miracast_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()
