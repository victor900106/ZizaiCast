// 自在投影 app: core/base/source_gate.cpp — core/base（第 0 層：共用型別、狀態與原語）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"

namespace pm_app {

void postText(HWND h, EventKind kind, std::wstring text) {
    if (!h) return;
    auto* p = new std::wstring(std::move(text));
    if (!PostMessageW(h, WM_PM_EVENT, kind, reinterpret_cast<LPARAM>(p))) delete p;
}

void postSource(HWND h, SourceEvent kind, std::wstring text) {
    if (!h) return;
    auto* p = new std::wstring(std::move(text));
    if (!PostMessageW(h, WM_PM_SRCEV, kind, reinterpret_cast<LPARAM>(p))) delete p;
}
std::atomic<int> g_active{SrcNone};
std::atomic<bool> g_takeoverNew{true};       // Settings::takeoverKeep == false
std::atomic<HWND> g_uiHwnd{nullptr};
pm::MiracastReceiver* g_miracast = nullptr;  // set in wWinMain, lives until exit
pm::AndroidSource* g_android = nullptr;

const wchar_t* sourceLabel(int s) {
    return s == SrcAirPlay ? L"AirPlay" : s == SrcMiracast ? L"Miracast" : s == SrcAndroid ? L"Android" : L"";
}

// Any thread. Takes the window for `me`: free → yes; held by another source
// → only if `force`. Stops a Miracast / Android owner right here (before the
// newcomer's first picture) and tells the UI thread.
bool claimSource(int me, bool force) {
    int cur = g_active.load();
    for (;;) {
        if (cur == me) return true;
        if (cur != SrcNone && !force) return false;
        if (g_active.compare_exchange_weak(cur, me)) break;
    }
    if (cur == SrcMiracast && g_miracast) g_miracast->disconnect();  // ends its cast (window->onReset)
    if (cur == SrcAndroid && g_android) g_android->stop();           // its resets are gated off now
    if (HWND h = g_uiHwnd.load()) PostMessageW(h, WM_PM_SOURCE, static_cast<WPARAM>(me), static_cast<LPARAM>(cur));
    return true;
}

// Gives the window back if `s` still owns it.
void releaseSource(int s) {
    int cur = s;
    g_active.compare_exchange_strong(cur, SrcNone);
}

}  // namespace pm_app
