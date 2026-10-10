// 自在投影 app: view/window_fit.cpp — view（視窗動作、檢視選項）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "help/help.h"
#include "share/share.h"
#include "capture/capture.h"
#include "translate/translate.h"
#include "connect/connect.h"
#include "view/view.h"

namespace pm_app {

void applyTopmost() {
    SetWindowPos(g.hwnd, g.settings.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Shape the picture wants (after 畫面 rotation and with the iPhone frame), from
// the video window. False before the first picture.
bool desiredAspect(int& w, int& h) {
    w = h = 0;
    g.window->desiredClientAspect(w, h);
    return w > 0 && h > 0;
}

// Moves/sizes the window to a client area of cw x ch around its centre,
// kept on the monitor's work area.
void setClientSizeCentred(HWND h, int cw, int ch) {
    const LONG style = GetWindowLongW(h, GWL_STYLE);
    RECT win{};
    GetWindowRect(h, &win);
    RECT want{0, 0, cw, ch};
    AdjustWindowRectExForDpi(&want, style, FALSE, GetWindowLongW(h, GWL_EXSTYLE), GetDpiForWindow(h));
    const int w = want.right - want.left, hgt = want.bottom - want.top;
    int x = (win.left + win.right) / 2 - w / 2, y = (win.top + win.bottom) / 2 - hgt / 2;
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
        const RECT& wa = mi.rcWork;
        x = (std::max)(static_cast<int>(wa.left), (std::min)(x, static_cast<int>(wa.right) - w));
        y = (std::max)(static_cast<int>(wa.top), (std::min)(y, static_cast<int>(wa.bottom) - hgt));
    }
    SetWindowPos(h, nullptr, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
}

bool windowResizable(HWND h) {
    return (GetWindowLongW(h, GWL_STYLE) & WS_CAPTION) && !IsZoomed(h) && !IsIconic(h);
}

// When the picture turns (the phone rotates, or 畫面 rotation), turn the
// window too: swap the client width/height around the window centre.
// Skipped while fullscreen (no caption) or maximized.
void followOrientation(HWND h) {
    int aw = 0, ah = 0;
    if (!desiredAspect(aw, ah)) return;
    const int orientation = aw > ah ? 2 : 1;
    if (orientation == g.lastOrientation) return;
    const bool first = g.lastOrientation == 0;
    g.lastOrientation = orientation;
    if (!windowResizable(h)) return;
    RECT client{};
    GetClientRect(h, &client);
    const int cw = client.right, ch = client.bottom;
    if (cw <= 0 || ch <= 0 || ((cw > ch) == (orientation == 2))) return;  // already matches
    if (first && orientation == 1) return;  // default window is already portrait
    setClientSizeCentred(h, ch, cw);
}

// After a 畫面 change by the user (rotate / flip / reset / iPhone frame): fit
// the window to the picture exactly, keeping its long side, so there are no
// bars around it.
void fitWindowToPicture(HWND h) {
    int aw = 0, ah = 0;
    if (!desiredAspect(aw, ah)) return;
    g.lastOrientation = aw > ah ? 2 : 1;
    if (!windowResizable(h)) return;
    RECT client{};
    GetClientRect(h, &client);
    const int longSide = (std::max)(client.right, client.bottom);
    if (longSide <= 0) return;
    int cw, ch;
    if (aw >= ah) {
        cw = longSide;
        ch = static_cast<int>(std::lround(static_cast<double>(longSide) * ah / aw));
    } else {
        ch = longSide;
        cw = static_cast<int>(std::lround(static_cast<double>(longSide) * aw / ah));
    }
    // Shrink (keeping the shape) if that would not fit on the work area.
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
        const int maxW = (mi.rcWork.right - mi.rcWork.left) * 95 / 100;
        const int maxH = (mi.rcWork.bottom - mi.rcWork.top) * 90 / 100;
        const double k = (std::min)({1.0, static_cast<double>(maxW) / cw, static_cast<double>(maxH) / ch});
        cw = static_cast<int>(cw * k);
        ch = static_cast<int>(ch * k);
    }
    if (std::abs(cw - client.right) <= 1 && std::abs(ch - client.bottom) <= 1) return;
    setClientSizeCentred(h, cw, ch);
}

}  // namespace pm_app
