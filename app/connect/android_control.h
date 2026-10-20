// 自在投影 app: Android 操控 (0.8.3) -- 剪貼簿同步, 操控時關掉手機螢幕,
// 連線期間不休眠, 用 USB 線連線, through the AndroidSource API of the
// droid-fix branch.
#pragma once

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "pm/android_source.h"

namespace pm_app {

// ---- connect/android_control.cpp
void androidControlCommand(UINT cmd);  // CmdAndClipSync / CmdAndScreenOff / CmdAndStayAwake / CmdAndUsb
// An Android phone started mirroring: hand it the three options.
void androidControlApply(const char* why);
// 連接手機 ▸: the 「Android 操控」 group (caption, three check items, USB).
void appendAndroidControlItems(std::vector<MenuItem>& v);
void androidControlShutdown();  // waits for a USB connect still running
// AndroidSource states while 用 USB 線連線 runs (looking, allow on the phone, none found ...):
// shown as toasts (the pairing panel is not open for it), until the phone is there or it ended.
void androidUsbState(pm::AndroidSource::State s, const std::wstring& detail);

}  // namespace pm_app
