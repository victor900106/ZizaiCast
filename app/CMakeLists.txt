# 自在投影 app shell: wires pm_core (AirPlay), pm_miracast (Android 投放) and
# pm_android (Android wireless debugging) to pm_video and pm_audio, plus tray
# icon, settings, screenshots, recording and the pairing panel. See docs/app.md.
set(PM_APP_VERSION 0.6.1)
# Test builds only: -DPM_APP_VERSION_OVERRIDE=x.y.z (a fake newer app for the
# 本機更新 / 自動更新 end-to-end tests). Leave empty for real builds.
set(PM_APP_VERSION_OVERRIDE "" CACHE STRING "Test only: build the app as this version")
if(PM_APP_VERSION_OVERRIDE)
  set(PM_APP_VERSION ${PM_APP_VERSION_OVERRIDE})
endif()
string(REPLACE "." ";" PM_APP_VERSION_PARTS ${PM_APP_VERSION})
list(GET PM_APP_VERSION_PARTS 0 PM_VER_MAJOR)
list(GET PM_APP_VERSION_PARTS 1 PM_VER_MINOR)
list(GET PM_APP_VERSION_PARTS 2 PM_VER_PATCH)
# 自動更新 manifest URL (JSON {version, url, sha256, notes}); settings.ini
# update_url overrides it, empty disables the check. Published as a GitHub
# release asset (302 to the release-assets CDN; WinHTTP follows it).
set(PM_UPDATE_URL_DEFAULT "https://github.com/victor900106/ZizaiCast/releases/latest/download/update.json")
set(PM_UPDATE_URL "${PM_UPDATE_URL_DEFAULT}" CACHE STRING "自在投影 update manifest URL (empty = no update check)")
# Build trees that cached an earlier default (placeholder, pre-rename repo): move them on.
if(PM_UPDATE_URL STREQUAL "https://zizai-update.example.com/zizai/update.json" OR
   PM_UPDATE_URL STREQUAL "https://github.com/victor900106/zizai-touying/releases/latest/download/update.json")
  set(PM_UPDATE_URL "${PM_UPDATE_URL_DEFAULT}" CACHE STRING "自在投影 update manifest URL (empty = no update check)" FORCE)
endif()

# The source modules normally come from the top-level CMakeLists (before
# app); pull them in here if it does not list them.
foreach(m miracast android)
  if(NOT TARGET pm_${m} AND EXISTS ${CMAKE_SOURCE_DIR}/${m}/CMakeLists.txt)
    add_subdirectory(${CMAKE_SOURCE_DIR}/${m} ${CMAKE_BINARY_DIR}/${m})
  endif()
endforeach()

add_executable(PhoneMirror WIN32 main.cpp popup_menu.cpp popup_menu.h pair_panel.cpp pair_panel.h about_panel.cpp
  about_panel.h updater.cpp updater.h resource.h res/app.rc res/app.manifest
  ${CMAKE_SOURCE_DIR}/include/pm/i18n.h ${CMAKE_SOURCE_DIR}/include/pm/i18n_strings.inc)
target_link_libraries(PhoneMirror PRIVATE pm_core pm_video pm_audio pm_recorder pm_miracast pm_android winhttp bcrypt version
  shell32 shlwapi ole32 advapi32 comctl32 mfplat mfuuid d2d1 dwrite windowscodecs dwmapi shcore)
set_target_properties(PhoneMirror PROPERTIES OUTPUT_NAME "自在投影")
target_compile_definitions(PhoneMirror PRIVATE UNICODE _UNICODE PM_APP_VERSION_STR="${PM_APP_VERSION}"
  PM_UPDATE_URL_STR="${PM_UPDATE_URL}" PM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  PM_VER_MAJOR=${PM_VER_MAJOR} PM_VER_MINOR=${PM_VER_MINOR} PM_VER_PATCH=${PM_VER_PATCH})

# Ship the VC++ runtime app-local (msvcp140 / vcruntime140 / vcruntime140_1)
# next to the exe, so the installer works on PCs without the VC++ redist.
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
include(InstallRequiredSystemLibraries)
set(PM_CRT_DLLS "")
foreach(lib ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS})
  get_filename_component(n "${lib}" NAME)
  string(TOLOWER "${n}" n)
  if(n MATCHES "^(msvcp140|vcruntime140|vcruntime140_1)\.dll$")
    list(APPEND PM_CRT_DLLS "${lib}")
  endif()
endforeach()
if(PM_CRT_DLLS)
  add_custom_command(TARGET PhoneMirror POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PM_CRT_DLLS} $<TARGET_FILE_DIR:PhoneMirror>
    COMMENT "Copying VC++ runtime next to 自在投影.exe")
else()
  message(WARNING "VC++ runtime DLLs not found; the installer will need the VC++ redist")
endif()

# Android tools next to the exe (<bin>/android-tools: adb.exe + its DLLs,
# scrcpy-server, licences) — the installer ships that folder as 程式\android-tools.
set(PM_ANDROID_TP ${CMAKE_SOURCE_DIR}/android/third_party)
if(COMMAND pm_android_copy_tools AND EXISTS ${PM_ANDROID_TP}/platform-tools/adb.exe)
  pm_android_copy_tools(PhoneMirror)
  add_custom_command(TARGET PhoneMirror POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PM_ANDROID_TP}/scrcpy/LICENSE
            $<TARGET_FILE_DIR:PhoneMirror>/android-tools/LICENSE-scrcpy.txt
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PM_ANDROID_TP}/qrcodegen/LICENSE
            $<TARGET_FILE_DIR:PhoneMirror>/android-tools/LICENSE-qrcodegen.txt
    COMMENT "Android tool licences → android-tools")
else()
  message(WARNING "android tools (adb, scrcpy-server) missing: run android/third_party/fetch_tools.ps1")
endif()

# 使用教學 / User guide (both languages) next to the exe for dev builds (the
# installer puts them in 程式\ too, and the UI language's one at the top level).
foreach(guide 自在投影教學.html ZizaiCast-Guide.html)
  set(PM_TUTORIAL ${CMAKE_SOURCE_DIR}/docs/tutorial/${guide})
  if(EXISTS ${PM_TUTORIAL})
    add_custom_command(TARGET PhoneMirror POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PM_TUTORIAL} $<TARGET_FILE_DIR:PhoneMirror>
      COMMENT "Copying ${guide} next to 自在投影.exe")
  endif()
endforeach()

# Third-party licences (About → 授權資訊 / licenses folder): <bin>\licenses,
# like the installed 程式\licenses.
set(PM_LICENSE_FILES ${CMAKE_SOURCE_DIR}/docs/licenses/第三方授權.txt
  ${CMAKE_SOURCE_DIR}/docs/licenses/THIRD_PARTY_NOTICES.txt ${CMAKE_SOURCE_DIR}/docs/licenses/SOURCE.md)
add_custom_command(TARGET PhoneMirror POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:PhoneMirror>/licenses
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PM_LICENSE_FILES} $<TARGET_FILE_DIR:PhoneMirror>/licenses
  COMMENT "Copying the licence notices to licenses\\")
