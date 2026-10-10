// 自在投影 app: core/app（視窗程序、命令分派、--dev 測試台） 對外宣告。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出
#pragma once

#include "core/base/app_state.h"
#include "core/base/base.h"

namespace pm_app {

// ---- --dev test harness (no iPhone needed) -----------------------------------
// --test-feed FILE.h264 [--test-seconds N] [--test-takeover S]: a fake phone
// "連線測試機" connects 1 s after start, its Annex-B H.264 file is looped at
// 30 fps into the same sinks as the core's, and after N s (default 0 = never)
// the video stops and a disconnect is posted (-> the 連線中斷 hold). With
// --test-takeover S, at S s a takeover by 「第二支 iPhone」 is posted.
// Registered message PhoneMirror.DevCommand (wParam = menu command id) runs a
// menu command, for scripted screenshots. Both only with --dev.
// --test-source android: the H.264 file comes from a fake Android phone
// 「Galaxy S24」 through the Android path (arbitration, gates, input handlers
// that log taps / keys) without adb. --test-source miracast: a fake Miracast
// cast 「Galaxy S24」 of generated BGRA pictures (no file needed).
// --test-delay S: connect after S s instead of 1 s.
// --test-start-fail: the connection never gets going (Android: start-up error
// after 連線中; Miracast: pending, then back to listening; iPhone: a PIN that
// is cancelled on the phone). --test-no-picture: connects, never sends a
// picture (the 30 s 連線中 watchdog). PM_DEV_INSTANCE=x: own data folder.
struct TestFeed {
    std::wstring file;
    int seconds = 0;
    int stallAt = 0;  // --test-stall S: stop sending at S s without a reset (phone left the Wi-Fi)
    int takeoverAt = 0;
    int source = SrcAirPlay;
    int delayMs = 1000;
    bool startFail = false;  // --test-start-fail: android start-up fails after 連線中 (no picture)
    bool noPicture = false;  // --test-no-picture: connects, then never sends a picture (連線中 watchdog)
};


// ---- core/app/commands.cpp
void runCommand(UINT cmd);
void runMenu(const std::vector<MenuItem>& items, POINT pt, bool keyboard, bool tray);
void showTrayMenu(int x, int y);
void showContextMenu(int x, int y);

// ---- core/app/app_proc.cpp
std::string endSessionWhy(LPARAM lp);
LRESULT CALLBACK appProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
LRESULT appProcInner(HWND h, UINT msg, WPARAM wp, LPARAM lp);

// ---- core/app/dev_harness.cpp
void runTestMiracast(TestFeed tf, pm::VideoWindow* win, HWND h, Log* log, std::atomic<bool>* stop);
std::vector<std::pair<size_t, size_t>> splitH264(const std::string& d);
void runTestFeed(TestFeed tf, pm::VideoSink* sink, HWND h, Log* log, std::atomic<bool>* stop);

// ---- core/app/main.cpp
HICON loadAppIcon(int cx);
void activateRunningInstance(bool background, UINT showMsg, Log& log);

}  // namespace pm_app
