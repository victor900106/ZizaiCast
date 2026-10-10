// 自在投影 app: core/base 各 .cpp 的函式宣告（預設參數只寫在這裡）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出
#pragma once

#include "core/base/app_common.h"
#include "core/base/app_consts.h"

namespace pm_app {

// ---- core/base/util.cpp
std::string toUtf8(const std::wstring& w);
std::wstring toWide(const std::string& s);
fs::path knownFolder(REFKNOWNFOLDERID id);
fs::path dataDir(bool dev);
fs::path exePath();
pm::i18n::Lang resolveLanguage(int pref);
std::wstring moduleText(const std::wstring& s);
bool hevcDecoderAvailable();
long long nowMs();

// ---- core/base/autostart.cpp
std::wstring autostartCommand();
std::wstring readAutostart();
bool setAutostart(bool on);
void repairAutostart();

// ---- core/base/source_gate.cpp
void postText(HWND h, EventKind kind, std::wstring text);
void postSource(HWND h, SourceEvent kind, std::wstring text = {});
const wchar_t* sourceLabel(int s);
bool claimSource(int me, bool force);
void releaseSource(int s);

// ---- core/base/app_state.cpp
float savedVolumeDb();
void saveSettings();

// ---- core/base/shell.cpp
void showDeferred(const std::wstring& msg, int ms = 4000);
bool isFullscreen();
void keepOnScreen(HWND h);
void bringToFront();
void trayAdd();
void trayRetip();
void trayRemove();
void trayBalloon(const std::wstring& title, const std::wstring& text, bool update = false);
bool windowHidden();

// ---- core/base/paths.cpp
bool installedLayout(const fs::path& exeDir);
fs::path userDir(const wchar_t* leaf, REFKNOWNFOLDERID fallback);
fs::path screenshotDir();
fs::path recordingDir();
void openScreenshotDir();
void openRecordingDir();

// ---- core/base/state_query.cpp
bool recording();
bool pictureShowing();
bool viewAvailable();
pm::translate::Lang translateTarget();
bool translating();
std::wstring zoomText(float z);
bool offerLocal();
std::string offerVersion();
// A phone on screen (or connecting / held after a drop) or a recording: an
// automatic prompt must not cover the mirror.
int liveSourceNow();
std::wstring liveName(bool shortName = false);
std::wstring shortLabel(const wchar_t* s);
bool androidLive();
int effectiveQuality();

// ---- core/base/titles.cpp
std::wstring displayNameFor();
// While the phone has paused the stream: the title says whether a frozen picture is shown.
void refreshPausedTitle();
std::wstring currentTitle();
void refreshTitles();

// ---- core/base/live_toolbar.cpp
void refreshToolbar();

}  // namespace pm_app
