// VideoWindow::Impl: idle screen interaction: fullscreen, hit tests, mascot, hover, options, focus, keys, links.
// 拆檔 0.7.9：自 video_window.cpp（Impl 類別內）原樣搬出，改成類別外定義。
#include "video_window_impl.h"

namespace pm {

void VideoWindow::Impl::toggleFullscreen() {
    DWORD style = static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE));
    if (!fullscreen) {
        MONITORINFO mi{sizeof(mi)};
        if (!GetWindowPlacement(hwnd, &savedPlacement) ||
            !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        SetWindowLongW(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen = true;
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &savedPlacement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen = false;
    }
}

void VideoWindow::Impl::refitFullscreen(const char* why) {
    if (!fullscreen) return;
    MONITORINFO mi{sizeof(mi)};
    RECT wr{};
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi) || !GetWindowRect(hwnd, &wr) ||
        EqualRect(&wr, &mi.rcMonitor))
        return;
    log("fullscreen re-fitted to the monitor (%s): %ldx%ld at (%ld,%ld)", why, mi.rcMonitor.right - mi.rcMonitor.left,
        mi.rcMonitor.bottom - mi.rcMonitor.top, mi.rcMonitor.left, mi.rcMonitor.top);
    SetWindowPos(hwnd, nullptr, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
}

int VideoWindow::Impl::hitTest(LPARAM lp) {
    const POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
    std::lock_guard lk(m);
    for (size_t i = 0; i < optionRects.size(); ++i)
        if (PtInRect(&optionRects[i], pt)) return static_cast<int>(i);
    return -1;
}

bool VideoWindow::Impl::hitMascot(LPARAM lp) {
    const POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
    std::lock_guard lk(m);
    const RECT& r = mascotRect;
    if (r.right <= r.left || r.bottom <= r.top || !PtInRect(&r, pt) || !maskW || !maskH) return false;
    const UINT mx = std::min(maskW - 1, static_cast<UINT>((pt.x - r.left) * static_cast<LONGLONG>(maskW) / (r.right - r.left)));
    const UINT my = std::min(maskH - 1, static_cast<UINT>((pt.y - r.top) * static_cast<LONGLONG>(maskH) / (r.bottom - r.top)));
    return mascotMask[static_cast<size_t>(my) * maskW + mx] != 0;
}

void VideoWindow::Impl::setMascotHot(bool hot) {
    if (hot == mascotHotUi) return;
    mascotHotUi = hot;
    if (hot) SetCursor(LoadCursorW(nullptr, IDC_HAND));
    {
        std::lock_guard lk(m);
        mascotHoverReq = hot ? 1 : 0;
    }
    wake();
}

void VideoWindow::Impl::clickMascot() {
    {
        std::lock_guard lk(m);
        mascotClickReq = true;
    }
    wake();
}

void VideoWindow::Impl::setHover(int i) {
    if (i == hoverUi) return;
    hoverUi = i;
    if (i >= 0) SetCursor(LoadCursorW(nullptr, IDC_HAND));
    {
        std::lock_guard lk(m);
        hover = i;
        hoverChanged = true;
        dirty = true;
    }
    wake();
}

void VideoWindow::Impl::toggleOption(int i) {
    std::function<void(int, bool)> cb;
    int id = 0;
    bool checked = false;
    {
        std::lock_guard lk(m);
        if (i < 0 || static_cast<size_t>(i) >= options.size()) return;
        checked = options[i].checked = !options[i].checked;
        id = options[i].id;
        cb = onToggle;
        optionsChanged = true;
        dirty = true;
    }
    wake();
    if (cb) cb(id, checked);  // UI thread
}

void VideoWindow::Impl::sendMotion() {
    BOOL on = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) on = TRUE;
    const int reduced = motionOverride >= 0 ? motionOverride : (on ? 0 : 1);
    if (reduced == motionSent) return;
    motionSent = reduced;
    {
        std::lock_guard lk(m);
        motionReq = reduced;
    }
    wake();
}

void VideoWindow::Impl::sendPress(int kind, int index) {
    if (kind < 0 && !pressSent) return;
    pressSent = kind >= 0;
    {
        std::lock_guard lk(m);
        pressReq = kind < 0 ? -1 : kind * 1000 + index;
    }
    wake();
}

void VideoWindow::Impl::sendFocus(int kind, int index) {
    {
        std::lock_guard lk(m);
        focusReq = kind < 0 ? -1 : kind * 1000 + index;
    }
    wake();
}

void VideoWindow::Impl::clearFocus() {
    if (focusUi < 0) return;
    focusUi = -1;
    sendFocus(-1, -1);
}

bool VideoWindow::Impl::idleKey(UINT msg, WPARAM wp) {
    // In reading order (0.7.8: the idle cards' buttons sit above the
    // check boxes): top to bottom, then left to right.
    struct Stop {
        int kind, index;
        RECT r;
    };
    std::vector<Stop> stops;
    {
        std::lock_guard lk(m);
        for (size_t i = 0; i < optionRects.size(); ++i)
            if (optionRects[i].right > optionRects[i].left)
                stops.push_back({video::Renderer::UiOption, static_cast<int>(i), optionRects[i]});
        for (size_t i = 0; i < linkRects.size(); ++i)
            if (linkRects[i].right > linkRects[i].left)
                stops.push_back({video::Renderer::UiAction, static_cast<int>(i), linkRects[i]});
    }
    std::stable_sort(stops.begin(), stops.end(), [](const Stop& a, const Stop& b) {
        const LONG ca = (a.r.top + a.r.bottom) / 2, cb = (b.r.top + b.r.bottom) / 2;
        // (the same row: centres within a few pixels)
        if (std::abs(ca - cb) > 4) return ca < cb;
        return a.r.left < b.r.left;
    });
    const int n = static_cast<int>(stops.size());
    if (msg == WM_KEYUP) {
        if ((wp == VK_SPACE || wp == VK_RETURN) && pressSent) {
            sendPress(-1, -1);
            return true;
        }
        return false;
    }
    if (n == 0) {
        clearFocus();
        return false;
    }
    if (focusUi >= n) focusUi = -1;
    switch (wp) {
    case VK_TAB: {
        if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0) return false;
        const bool back = GetKeyState(VK_SHIFT) < 0;
        focusUi = focusUi < 0 ? (back ? n - 1 : 0) : (focusUi + (back ? n - 1 : 1)) % n;
        break;
    }
    case VK_SPACE:
    case VK_RETURN:
        if (focusUi < 0) return false;
        sendPress(stops[focusUi].kind, stops[focusUi].index);
        if (stops[focusUi].kind == video::Renderer::UiOption) toggleOption(stops[focusUi].index);
        else clickLink(stops[focusUi].index);
        return true;
    case VK_ESCAPE:
        if (focusUi < 0) return false;
        clearFocus();
        return true;
    default:
        return false;
    }
    sendFocus(stops[focusUi].kind, stops[focusUi].index);
    return true;
}

void VideoWindow::Impl::markDirty() {
    {
        std::lock_guard lk(m);
        dirty = true;
    }
    wake();
}

void VideoWindow::Impl::requestAdapterCheck() {
    {
        std::lock_guard lk(m);
        adapterCheckReq = true;
    }
    wake();
}

int VideoWindow::Impl::hitLink(LPARAM lp) {
    const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    std::lock_guard lk(m);
    for (size_t i = 0; i < linkRects.size(); ++i)
        if (PtInRect(&linkRects[i], pt)) return static_cast<int>(i);
    return -1;
}

void VideoWindow::Impl::setLinkHot(int hot) {
    if (hot == linkHotUi) return;
    linkHotUi = hot;
    if (hot >= 0) SetCursor(LoadCursorW(nullptr, IDC_HAND));
    {
        std::lock_guard lk(m);
        linkHoverReq = hot;
    }
    wake();
}

void VideoWindow::Impl::clickLink(int i) {
    std::function<void()> cb;
    {
        std::lock_guard lk(m);
        if (i >= 0 && i < static_cast<int>(actionFns.size())) cb = actionFns[i];
    }
    if (cb) cb();  // UI thread
}

}  // namespace pm
