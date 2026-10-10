// 自在投影 app: capture（截圖、錄影） 對外宣告。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出
#pragma once

#include "core/base/app_state.h"
#include "core/base/base.h"

namespace pm_app {

// ---- capture/screenshot.cpp
fs::path stampedFile(const fs::path& dir, const wchar_t* ext);
void takeSnapshot();

// ---- capture/recording.cpp
unsigned long long freeBytes(const fs::path& dir);
void stopRecording(const std::wstring& toast = {});
void seedRecording(pm::Recorder* rec);
void startRecording();
void checkRecording();
void toggleRecording();

}  // namespace pm_app
