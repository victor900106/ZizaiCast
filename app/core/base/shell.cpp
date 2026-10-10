// 自在投影 app: core/base/shell.cpp — core/base（第 0 層：共用型別、狀態與原語）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"

namespace pm_app {

// 0.7.8 UX (p9): a change held until the mirroring session ends (PIN, 畫面清晰度,
// 第二支手機連上時, 名稱): its toast, and above it a 「現在重新連線套用」 chip
// (CmdApplyNow; posted, the chip is inside its own click). 設定 ▸ offers the
// same row for as long as something is held (applyPendingNow).
void showDeferred(const std::wstring& msg, int ms) {
    g.window->showToast(msg, ms);
    g.shareChip.hide();  // same place over the window
    const HWND h = g.hwnd;
    std::vector<pm::ui::ShareChip::Action> acts;
    acts.push_back({0xE72C /* Refresh */, tr(S::ApplyNow), [h]() { PostMessageW(h, WM_PM_TOOL, CmdApplyNow, 0); }});
    g.applyChip.show(h, std::move(acts), 10000);
}

// ---- window helpers --------------------------------------------------------
bool isFullscreen() { return !(GetWindowLongW(g.hwnd, GWL_STYLE) & WS_CAPTION); }

// The window must be reachable: after a monitor was unplugged (or the
// resolution / scaling changed) a window kept hidden in the tray comes back at
// its old position, possibly off every screen. If its caption is not on the
// nearest monitor's work area, or less than a quarter of it is visible, it is
// moved (and shrunk if needed) onto that work area.
void keepOnScreen(HWND h) {
    if (g.testOffscreen || isFullscreen() || IsIconic(h) || IsZoomed(h)) return;  // fullscreen: video_window refits it
    RECT r{};
    MONITORINFO mi{sizeof(mi)};
    if (!GetWindowRect(h, &r) || !GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi)) return;
    const RECT& wa = mi.rcWork;
    RECT vis{};
    const long w = r.right - r.left, ht = r.bottom - r.top;
    const long seen = IntersectRect(&vis, &r, &wa) ? (vis.right - vis.left) * (vis.bottom - vis.top) : 0;
    const bool captionOn = r.top >= wa.top - 16 && r.top < wa.bottom - 24 && r.right > wa.left + 48 && r.left < wa.right - 48;
    if (captionOn && seen * 4 >= w * ht) return;
    const long nw = (std::min)(w, wa.right - wa.left), nh = (std::min)(ht, wa.bottom - wa.top);
    const long x = (std::max)(wa.left, (std::min)(r.left, wa.right - nw)), y = (std::max)(wa.top, (std::min)(r.top, wa.bottom - nh));
    g.log->write("info", "window was off-screen at (" + std::to_string(r.left) + "," + std::to_string(r.top) + "): moved to (" +
                             std::to_string(x) + "," + std::to_string(y) + ")");
    SetWindowPos(h, nullptr, x, y, nw, nh, SWP_NOZORDER | SWP_NOACTIVATE);
    // 0.7.9: landing on a monitor with another scale (e.g. 100 % -> 175 %)
    // resizes the window through WM_DPICHANGED, which can make it taller or
    // wider than that work area again: fit it once more, now at the new DPI.
    RECT r2{};
    MONITORINFO mi2{sizeof(mi2)};
    if (!GetWindowRect(h, &r2) || !GetMonitorInfoW(MonitorFromRect(&r2, MONITOR_DEFAULTTONEAREST), &mi2)) return;
    const RECT& wa2 = mi2.rcWork;
    if (r2.left >= wa2.left && r2.top >= wa2.top && r2.right <= wa2.right && r2.bottom <= wa2.bottom) return;
    const long w2 = (std::min)(r2.right - r2.left, wa2.right - wa2.left), h2 = (std::min)(r2.bottom - r2.top, wa2.bottom - wa2.top);
    const long x2 = (std::max)(wa2.left, (std::min)(r2.left, wa2.right - w2)), y2 = (std::max)(wa2.top, (std::min)(r2.top, wa2.bottom - h2));
    g.log->write("info", "window refit after the DPI change: (" + std::to_string(x2) + "," + std::to_string(y2) + ") " +
                             std::to_string(w2) + "x" + std::to_string(h2));
    SetWindowPos(h, nullptr, x2, y2, w2, h2, SWP_NOZORDER | SWP_NOACTIVATE);
}

void bringToFront() {
    HWND h = g.hwnd;
    if (g.testOffscreen) {  // --dev --test-offscreen: never take the foreground / z-order
        if (!IsWindowVisible(h)) ShowWindow(h, SW_SHOWNOACTIVATE);
        return;
    }
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    else if (!IsWindowVisible(h)) ShowWindow(h, SW_SHOW);
    keepOnScreen(h);
    // The foreground lock may refuse SetForegroundWindow from a background
    // process; a brief topmost flip still raises the window above others.
    if (!g.settings.topmost) {
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    SetForegroundWindow(h);
}

// ---- tray icon -------------------------------------------------------------
void trayAdd() {
    if (g.testOffscreen) {  // --dev --test-offscreen: nothing on the user's taskbar either
        g.trayOk = false;
        return;
    }
    g.nid = {};
    g.nid.cbSize = sizeof(g.nid);
    g.nid.hWnd = g.hwnd;
    g.nid.uID = 1;
    g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g.nid.uCallbackMessage = WM_PM_TRAY;
    g.nid.hIcon = g.iconSmall;
    wcscpy_s(g.nid.szTip, fmt(S::TrayTip, {tr(S::AppName)}).substr(0, 127).c_str());
    g.trayOk = Shell_NotifyIconW(NIM_ADD, &g.nid) != FALSE;
    g.nid.uVersion = NOTIFYICON_VERSION_4;
    if (g.trayOk) Shell_NotifyIconW(NIM_SETVERSION, &g.nid);
}

// Language switch / recording started or stopped: the tooltip in the
// current language; while recording it says so (0.7.9: the window may be
// minimized or in the tray, the REC badge is then out of sight).
void trayRetip() {
    if (!g.trayOk) return;
    g.nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcscpy_s(g.nid.szTip, fmt(g.recordingFile.empty() ? S::TrayTip : S::TrayTipRec, {tr(S::AppName)}).substr(0, 127).c_str());
    Shell_NotifyIconW(NIM_MODIFY, &g.nid);
    g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
}

void trayRemove() {
    if (!g.trayOk) return;
    Shell_NotifyIconW(NIM_DELETE, &g.nid);
    g.trayOk = false;
}

// `update`: the 「有新版本」 balloon (a click opens the dialog, WM_PM_TRAY).
void trayBalloon(const std::wstring& title, const std::wstring& text, bool update) {
    g.balloonUpdate = update;
    if (g.log)
        g.log->write("info", std::string(g.trayOk ? "tray balloon: " : "tray balloon (no tray icon): ") + toUtf8(title) +
                                 " | " + toUtf8(text));
    if (!g.trayOk) return;
    NOTIFYICONDATAW n = g.nid;
    n.uFlags = NIF_INFO;
    wcscpy_s(n.szInfoTitle, title.substr(0, 63).c_str());
    wcscpy_s(n.szInfo, text.substr(0, 255).c_str());
    n.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    n.hBalloonIcon = g.iconBig;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

bool windowHidden() { return !IsWindowVisible(g.hwnd) || IsIconic(g.hwnd); }

}  // namespace pm_app
