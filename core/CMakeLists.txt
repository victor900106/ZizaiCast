# PhoneMirror AirPlay protocol core: MSVC port of UxPlay's lib/ (GPL-3.0).
# Upstream: https://github.com/FDH2/UxPlay  (copied from _ref/uxplay @ 3dbf7ce, 2026-10-05)
# Local changes are marked with "PM:" comments.

find_package(OpenSSL REQUIRED)
find_package(PThreads4W REQUIRED)
find_package(unofficial-libplist CONFIG QUIET)
if(NOT TARGET unofficial::libplist::libplist)
  # Fallback: plain header/library lookup (vcpkg puts both on the default search paths).
  find_path(PLIST_INCLUDE_DIR plist/plist.h REQUIRED)
  find_library(PLIST_LIBRARY NAMES plist-2.0 libplist-2.0 plist REQUIRED)
  add_library(pm_plist UNKNOWN IMPORTED)
  set_target_properties(pm_plist PROPERTIES
    IMPORTED_LOCATION ${PLIST_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES ${PLIST_INCLUDE_DIR})
  set(PM_PLIST_TARGET pm_plist)
else()
  set(PM_PLIST_TARGET unofficial::libplist::libplist)
endif()

set(UX ${CMAKE_CURRENT_SOURCE_DIR}/src)

# --- third-party pieces bundled by UxPlay -----------------------------------
add_library(pm_llhttp STATIC ${UX}/llhttp/api.c ${UX}/llhttp/http.c ${UX}/llhttp/llhttp.c)
target_include_directories(pm_llhttp PUBLIC ${UX}/llhttp)

file(GLOB PM_PLAYFAIR_SRC ${UX}/playfair/*.c)
add_library(pm_playfair STATIC ${PM_PLAYFAIR_SRC})
target_include_directories(pm_playfair PUBLIC ${UX}/playfair)

# --- UxPlay lib/ (raop, rtp, ntp, pairing, fairplay, internal mdnsd) ----------
file(GLOB PM_AIRPLAY_SRC ${UX}/*.c)
add_library(pm_core STATIC
  ${PM_AIRPLAY_SRC}
  ${UX}/mdnsd/mdnsd.c
  ${UX}/mdnsd/dnssd_mdnsd.c
  ${UX}/pm_win/pm_win.c
  src/airplay_server.cpp
  include/pm/airplay_server.h)

target_include_directories(pm_core
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${UX} ${UX}/pm_win)
target_compile_definitions(pm_core PRIVATE
  WIN32                 # PM: UxPlay tests WIN32 (MinGW defines it, MSVC only defines _WIN32)
  NOHOLD                # same as upstream lib/CMakeLists.txt: new client may take over
  PLIST_210 PLIST_230   # vcpkg libplist is >= 2.3
  HAVE_STRUCT_TIMESPEC  # PM: UCRT already defines struct timespec (pthreads4w)
  _WINSOCK_DEPRECATED_NO_WARNINGS
  PUBLIC OPENSSL_API_COMPAT=0x10101000L)
if(MSVC)
  # C sources: silence the noisiest upstream warnings (int truncation/sign) only.
  target_compile_options(pm_core PRIVATE
    /FIpm_posix.h "$<$<COMPILE_LANGUAGE:C>:/wd4244;/wd4267;/wd4018;/wd4996;/wd4146>")
  target_compile_options(pm_llhttp PRIVATE /wd4244 /wd4267)
  target_compile_options(pm_playfair PRIVATE /wd4244 /wd4267 /wd4146)
endif()
target_link_libraries(pm_core
  PUBLIC pm_media_api
  PRIVATE pm_llhttp pm_playfair ${PM_PLIST_TARGET} OpenSSL::Crypto PThreads4W::PThreads4W
          ws2_32 iphlpapi)

add_subdirectory(${CMAKE_SOURCE_DIR}/tools/pm_probe ${CMAKE_BINARY_DIR}/tools/pm_probe)

# PM: offline jitter-buffer simulation (pause -> play gap patterns)
add_executable(pm_buffer_test tests/pm_buffer_test.c)
target_include_directories(pm_buffer_test PRIVATE ${UX} ${UX}/pm_win)
target_compile_definitions(pm_buffer_test PRIVATE WIN32 HAVE_STRUCT_TIMESPEC)
if(MSVC)
  target_compile_options(pm_buffer_test PRIVATE /FIpm_posix.h /wd4244 /wd4267)
endif()
target_link_libraries(pm_buffer_test PRIVATE pm_core winmm)

# PM: offline NTP clock-mapping check (fake iPhone timing server on 127.0.0.1)
add_executable(pm_ntp_test tests/pm_ntp_test.c)
target_include_directories(pm_ntp_test PRIVATE ${UX} ${UX}/pm_win)
target_compile_definitions(pm_ntp_test PRIVATE WIN32 HAVE_STRUCT_TIMESPEC)
if(MSVC)
  target_compile_options(pm_ntp_test PRIVATE /FIpm_posix.h /wd4244 /wd4267)
endif()
target_link_libraries(pm_ntp_test PRIVATE pm_core ws2_32)

# PM: offline replay of the NTP re-latch storm after a pause/resume (pure logic)
add_executable(pm_clock_test tests/pm_clock_test.cpp)
target_include_directories(pm_clock_test PRIVATE ${UX})

# PM: offline robustness checks (httpd, stream threads, pairing); loopback-only sockets
add_executable(pm_robust_test tests/pm_robust_test.c)
target_include_directories(pm_robust_test PRIVATE ${UX} ${UX}/pm_win)
target_compile_definitions(pm_robust_test PRIVATE WIN32 HAVE_STRUCT_TIMESPEC)
if(MSVC)
  target_compile_options(pm_robust_test PRIVATE /FIpm_posix.h /wd4244 /wd4267)
endif()
target_link_libraries(pm_robust_test PRIVATE pm_core ws2_32 ${PM_PLIST_TARGET})

# PM: INFO request-log limiter/formatter (pure, no sockets)
add_executable(pm_reqlog_test tests/pm_reqlog_test.c)
target_include_directories(pm_reqlog_test PRIVATE ${UX})
