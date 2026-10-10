// 自在投影 app: help（說明、教學、快速鍵一覽） 對外宣告。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出
#pragma once

#include "core/base/app_state.h"
#include "core/base/base.h"

namespace pm_app {

// ---- help/shortcuts.cpp
std::vector<pm::ui::SettingsPanel::Item> shortcutItems();
void openShortcuts();
void openDownloadPage();

// ---- help/tutorial.cpp
const wchar_t* guideName();
fs::path tutorialFile();
fs::path licensesDir();
void openTutorial(const wchar_t* anchor);

// ---- help/about.cpp
void openAbout();

}  // namespace pm_app
