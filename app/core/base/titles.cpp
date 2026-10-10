// 自在投影 app: core/base/titles.cpp — core/base（第 0 層：共用型別、狀態與原語）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"

namespace pm_app {

// ---- 語言 / Language ----
// Display name advertised to phones: --name, else settings display_name, else
// 自在投影 / Zizai Cast (+ " (測試)" / " (Test)" for --dev) in the UI language.
std::wstring displayNameFor() {
    if (!g.nameArg.empty()) return g.nameArg;
    if (!g.settings.displayName.empty()) return g.settings.displayName;
    return tr(g.dev && !g.demoBranding ? S::AppNameDev : S::AppName);
}

void refreshPausedTitle() {
    if (!g.hwnd || g.videoState != StatePaused) return;
    const std::wstring t = currentTitle();
    wchar_t cur[512] = {};
    GetWindowTextW(g.hwnd, cur, 512);
    if (t != cur) SetWindowTextW(g.hwnd, t.c_str());
}

// Window title for the current state, in the current language.
std::wstring currentTitle() {
    const std::wstring app = tr(S::AppName);
    if (g.videoState == StateLost) return fmt(S::TitleLost, {app});
    if (g.videoState == StatePaused)  // a frozen (translated) picture stays up: not "screen off"
        return g.window && g.window->viewState().frozen ? fmt(S::TitlePausedFrozen, {app}) : g.pausedTitle;
    if (g.videoState == StateMirroring && liveSourceNow() != SrcNone)
        return g.peerName.empty() ? fmt(S::TitleMirroring, {app}) : fmt(S::TitleMirroringName, {app, g.peerName});
    if (liveSourceNow() != SrcNone && !g.peerName.empty()) return fmt(S::TitleConnecting, {app, g.peerName});
    return g.idleTitle;
}

void refreshTitles() {
    g.idleTitle = fmt(S::TitleIdle, {tr(S::AppName), g.name});
    g.pausedTitle = fmt(S::TitlePaused, {tr(S::AppName)});
}

}  // namespace pm_app
