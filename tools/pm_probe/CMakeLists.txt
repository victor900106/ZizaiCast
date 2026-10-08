# pm_probe: console harness for pm_core (hooked from core/CMakeLists.txt).
add_executable(pm_probe main.cpp)
target_link_libraries(pm_probe PRIVATE pm_core dbghelp)
