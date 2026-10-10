// 自在投影 app: core/base/app_state.cpp — core/base（第 0 層：共用型別、狀態與原語）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"

namespace pm_app {

App g;
// The remembered phone volume (dB, -30..0).
float savedVolumeDb() { return g.volume ? g.volume->saved() : RememberVolumeAudioSink::load(g.volumeFile, -15.0f); }
pm::ui::VolumeControl g_volumeUi;  // 音量 / 靜音: toolbar, Ctrl+M / Ctrl+↑↓, menus (volume_ui.h)
// --dev test feeds: bumped by 中斷連線 so a fake phone stops like a dropped one.
std::atomic<int> g_testKick{0};

void saveSettings() { g.settings.save(g.settingsFile); }

// ---- 自動更新 ---------------------------------------------------------------------
const char* kAppVersion = PM_APP_VERSION_STR;

}  // namespace pm_app
