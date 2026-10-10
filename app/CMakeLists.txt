# 自在投影 app shell: wires pm_core (AirPlay), pm_miracast (Android 投放) and
# pm_android (Android wireless debugging) to pm_video and pm_audio, plus tray
# icon, settings, screenshots, recording and the pairing panel. See docs/app.md.
set(PM_APP_VERSION 0.7.9)
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
foreach(m miracast android share translate)
  if(NOT TARGET pm_${m} AND EXISTS ${CMAKE_SOURCE_DIR}/${m}/CMakeLists.txt)
    add_subdirectory(${CMAKE_SOURCE_DIR}/${m} ${CMAKE_BINARY_DIR}/${m})
  endif()
endforeach()

# ---- 拆檔 0.7.9: app 依資料夾分成靜態函式庫 (docs: launch/_work/audit/SPLIT_PLAN.md §4) ----
# 共用設定：所有 app 子函式庫都要繼承（漏掉 PM_SOURCE_DIR 會靜默改變行為）。
add_library(pm_app_config INTERFACE)
target_compile_definitions(pm_app_config INTERFACE UNICODE _UNICODE PM_APP_VERSION_STR="${PM_APP_VERSION}"
  PM_UPDATE_URL_STR="${PM_UPDATE_URL}" PM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  PM_VER_MAJOR=${PM_VER_MAJOR} PM_VER_MINOR=${PM_VER_MINOR} PM_VER_PATCH=${PM_VER_PATCH})
target_include_directories(pm_app_config INTERFACE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(pm_app_config INTERFACE pm_core pm_video pm_audio pm_recorder pm_miracast pm_android pm_share
  pm_translate)

# 既有面板與選單繪製（不依賴 main.cpp 拆出的任何函式）。
add_library(pm_app_ui STATIC popup_menu.cpp popup_menu.h pair_panel.cpp pair_panel.h about_panel.cpp about_panel.h
  ask_panel.cpp ask_panel.h share_panel.cpp share_panel.h share_picker.cpp share_picker.h update_panel.cpp update_panel.h
  settings_panel.cpp settings_panel.h tr_settings.cpp tr_settings.h volume_ui.cpp volume_ui.h volume_memory.h ui_anim.h)
target_link_libraries(pm_app_ui PUBLIC pm_app_config)
# 翻譯 ▸ 本機 AI 翻譯 / 線上翻譯 (tr_settings.cpp) need the optional engines of pm_translate.
if(PM_TRANSLATE_ENGINES)
  target_compile_definitions(pm_app_ui PRIVATE PM_APP_TR_ENGINES)
endif()

# 第 0 層：共用型別、狀態、原語。
add_library(pm_app_base STATIC core/base/app_common.h core/base/app_consts.h core/base/base.h core/base/log.h
  core/base/settings.h core/base/source_gate.h core/base/app_state.h
  core/base/util.cpp core/base/autostart.cpp core/base/source_gate.cpp core/base/app_state.cpp core/base/shell.cpp
  core/base/paths.cpp core/base/state_query.cpp core/base/titles.cpp core/base/live_toolbar.cpp)
target_link_libraries(pm_app_base PUBLIC pm_app_ui)

add_library(pm_app_help STATIC help/help.h help/shortcuts.cpp help/tutorial.cpp help/about.cpp)
add_library(pm_app_share STATIC share/share.h share/share_internal.h share/share_files.cpp share/share_qr.cpp
  share/share_push.cpp share/share_actions.cpp)
add_library(pm_app_translate STATIC translate/translate.h translate/magnifier.cpp translate/translate_actions.cpp
  translate/models.cpp translate/translator_host.cpp)
add_library(pm_app_capture STATIC capture/capture.h capture/screenshot.cpp capture/recording.cpp)
add_library(pm_app_connect STATIC connect/connect.h connect/connect_internal.h connect/airplay.cpp
  connect/airplay_events.cpp connect/sources.cpp connect/idle_hints.cpp connect/miracast.cpp connect/android.cpp
  connect/session.cpp)
add_library(pm_app_view STATIC view/view.h view/window_fit.cpp view/options.cpp view/picture.cpp view/close_flow.cpp
  view/language.cpp)
add_library(pm_app_update STATIC update/update.h update/update_check.cpp update/update_install.cpp
  update/local_updates.cpp update/updater.cpp update/updater.h)
add_library(pm_app_menus STATIC menus/menus.h menus/menu_items.cpp menus/menu_view_translate.cpp menus/menu_roots.cpp
  menus/menu_devshots.cpp)

target_link_libraries(pm_app_help      PUBLIC pm_app_base)
target_link_libraries(pm_app_share     PUBLIC pm_app_base)
target_link_libraries(pm_app_translate PUBLIC pm_app_base)
target_link_libraries(pm_app_capture   PUBLIC pm_app_share)
target_link_libraries(pm_app_connect   PUBLIC pm_app_capture pm_app_help)
target_link_libraries(pm_app_view      PUBLIC pm_app_connect pm_app_translate)
target_link_libraries(pm_app_update    PUBLIC pm_app_view)
target_link_libraries(pm_app_menus     PUBLIC pm_app_view)

# wWinMain 和資源檔留在 exe 自己的來源裡（.rc 放進靜態函式庫不會被連結）。
add_executable(PhoneMirror WIN32 core/app/main.cpp core/app/app_proc.cpp core/app/commands.cpp core/app/dev_harness.cpp
  core/app/core_app.h resource.h res/app.rc res/app.manifest
  ${CMAKE_SOURCE_DIR}/include/pm/i18n.h ${CMAKE_SOURCE_DIR}/include/pm/i18n_strings.inc)
# 連結順序：先列出全部 pm_app_*，再接拆檔前的原清單（同順序）。這樣 app 的程式碼在連到
# winhttp … shcore 之前就全部載入，外部符號的解析和拆檔前一樣（匯入表相同）；若只列
# pm_app_menus，registry 等 API 會改由後面的 windowsapp.lib 以 api-ms-win-* 匯入。
target_link_libraries(PhoneMirror PRIVATE pm_app_menus pm_app_update pm_app_view pm_app_connect pm_app_capture
  pm_app_share pm_app_help pm_app_translate pm_app_base pm_app_ui
  pm_core pm_video pm_audio pm_recorder pm_miracast pm_android pm_share pm_translate winhttp bcrypt version wintrust
  shell32 shlwapi ole32 advapi32 comctl32 mfplat mfuuid d2d1 dwrite windowscodecs dwmapi shcore)
# ---- 拆檔 0.7.9 (end) ----
set_target_properties(PhoneMirror PROPERTIES OUTPUT_NAME "自在投影")
target_compile_definitions(PhoneMirror PRIVATE UNICODE _UNICODE PM_APP_VERSION_STR="${PM_APP_VERSION}"
  PM_UPDATE_URL_STR="${PM_UPDATE_URL}" PM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  PM_VER_MAJOR=${PM_VER_MAJOR} PM_VER_MINOR=${PM_VER_MINOR} PM_VER_PATCH=${PM_VER_PATCH})

# Ship the VC++ runtime app-local (msvcp140 / msvcp140_1 / vcruntime140 / vcruntime140_1;
# msvcp140_1 for onnxruntime.dll)
# next to the exe, so the installer works on PCs without the VC++ redist.
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
include(InstallRequiredSystemLibraries)
set(PM_CRT_DLLS "")
foreach(lib ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS})
  get_filename_component(n "${lib}" NAME)
  string(TOLOWER "${n}" n)
  if(n MATCHES "^(msvcp140|msvcp140_1|vcruntime140|vcruntime140_1)\.dll$")
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
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:PhoneMirror>/licenses/onnxruntime
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/docs/licenses/onnxruntime/LICENSE.txt
          ${CMAKE_SOURCE_DIR}/docs/licenses/onnxruntime/ThirdPartyNotices.txt $<TARGET_FILE_DIR:PhoneMirror>/licenses/onnxruntime
  COMMENT "Copying the licence notices to licenses\\")

# 翻譯 (pm_translate): the offline translation engine bergamot.dll (MPL-2.0,
# translate/tools/build_bergamot.sh) next to the exe; the installer ships it
# in 程式\. Models are downloaded on first use, never bundled.
if(PM_BERGAMOT_DLL AND EXISTS "${PM_BERGAMOT_DLL}")
  add_custom_command(TARGET PhoneMirror POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PM_BERGAMOT_DLL}" $<TARGET_FILE_DIR:PhoneMirror>
    COMMENT "Copying bergamot.dll next to 自在投影.exe" VERBATIM)
else()
  message(WARNING "bergamot.dll not found (${PM_BERGAMOT_DLL}): run translate/tools/build_bergamot.sh; 翻譯 will say the engine is missing")
endif()

# 翻譯 OCR (0.7.0): onnxruntime.dll (Microsoft ONNX Runtime 1.30.0, MIT, CPU;
# translate/tools/get_onnxruntime.sh) runs the PaddleOCR models; the installer
# ships it in 程式\. The OCR models are downloaded on first use, never bundled.
if(PM_ONNXRUNTIME_DLL AND EXISTS "${PM_ONNXRUNTIME_DLL}")
  add_custom_command(TARGET PhoneMirror POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PM_ONNXRUNTIME_DLL}" $<TARGET_FILE_DIR:PhoneMirror>
    COMMENT "Copying onnxruntime.dll next to 自在投影.exe" VERBATIM)
else()
  message(WARNING "onnxruntime.dll not found (${PM_ONNXRUNTIME_DLL}): run translate/tools/get_onnxruntime.sh; 翻譯 falls back to Windows OCR")
endif()

# 音量 / 靜音: toolbar speaker + slider, Ctrl+M / Ctrl+↑ / Ctrl+↓, menus (volume_ui.h): in pm_app_ui above.

# pm_volume_test: remembered phone volume (volume_memory.h), app mute and the
# slider's 0..100 % mapping; no network / device.
add_executable(pm_volume_test tools/pm_volume_test.cpp volume_memory.h)
target_link_libraries(pm_volume_test PRIVATE pm_media_api)

# pm_update_test: 自動更新 URL policy (https only), SHA-256 from the locked
# installer, launching it while locked, Authenticode; no network / UI.
add_executable(pm_update_test tools/pm_update_test.cpp update/updater.cpp update/updater.h)
target_link_libraries(pm_update_test PRIVATE winhttp bcrypt version wintrust)
target_compile_definitions(pm_update_test PRIVATE UNICODE _UNICODE)

# pm_panel_anim_test: the panels' open / close fades, hover / press and the
# QR reveal (ui_anim.h), off the desktop; frame PNGs + PASS / FAIL.
add_executable(pm_panel_anim_test tools/pm_panel_anim_test.cpp ui_anim.h popup_menu.cpp ask_panel.cpp pair_panel.cpp
  about_panel.cpp update_panel.cpp)
target_include_directories(pm_panel_anim_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(pm_panel_anim_test PRIVATE d2d1 dwrite windowscodecs dwmapi shcore comctl32 shlwapi ole32 shell32)
target_compile_definitions(pm_panel_anim_test PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(pm_panel_anim_test PRIVATE /utf-8)
