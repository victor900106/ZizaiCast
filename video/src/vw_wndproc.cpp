// VideoWindow::Impl: window procedure.
// 拆檔 0.7.9：自 video_window.cpp（Impl 類別內）原樣搬出，改成類別外定義。
#include "video_window_impl.h"

namespace pm {

LRESULT VideoWindow::Impl::wndProc(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kCancelSelectMsg && kCancelSelectMsg) {
        endSelect(false);
        return 0;
    }
    static const UINT runOnUiMsg = RegisterWindowMessageW(L"PhoneMirror.Video.RunOnUi");
    if (msg == runOnUiMsg && runOnUiMsg && lp) {
        std::unique_ptr<std::function<void()>> fn(reinterpret_cast<std::function<void()>*>(lp));
        (*fn)();
        return 0;
    }
    if (msg == kTestMsg && kTestMsg && wp == 10) {
        // Test hook: centre of idle action lp (0 = first; the help link
        // when it is the only one) in client px, -1 if not shown.
        std::lock_guard lk(m);
        const size_t i = static_cast<size_t>(lp);
        if (i >= linkRects.size() || linkRects[i].right <= linkRects[i].left) return -1;
        const RECT& r = linkRects[i];
        return MAKELRESULT((r.left + r.right) / 2, (r.top + r.bottom) / 2);
    }
    if (msg == kTestMsg && kTestMsg && wp == 13) {
        // Test hook: a point on the mascot's cloud (its middle) in client
        // px, -1 if it cannot be clicked right now.
        std::lock_guard lk(m);
        const RECT& r = mascotRect;
        if (r.right <= r.left || r.bottom <= r.top) return -1;
        return MAKELRESULT(r.left + (r.right - r.left) * 43 / 100, r.top + (r.bottom - r.top) * 55 / 100);
    }
    if (msg == kTestMsg && kTestMsg && wp == 12) {
        // Test hook: lp = 1: posted mouse messages only (scripted
        // screenshots without moving the real cursor): no leave tracking.
        synthMouse = lp != 0;
        return 0;
    }
    if (msg == kTestMsg && kTestMsg && wp >= 14 && wp <= 16) {
        // Test hook: centre (client px) of translation list row lp (14),
        // marker lp (15) or the 放大這一塊 button (16); -1 if not shown.
        std::lock_guard lk(m);
        const RECT* r = nullptr;
        if (wp == 16) r = &ovHits.zoomBtn;
        else
            for (const auto& [rr, i] : wp == 14 ? ovHits.rows : ovHits.markers)
                if (i == static_cast<int>(lp)) r = &rr;
        if (!r || r->right <= r->left) return -1;
        return MAKELRESULT((r->left + r->right) / 2, (r->top + r->bottom) / 2);
    }
    if (msg == kTestMsg && kTestMsg && wp == 18) {
        // Test hook: centre of idle check box row lp in client px, -1 if not shown.
        std::lock_guard lk(m);
        const size_t i = static_cast<size_t>(lp);
        if (i >= optionRects.size() || optionRects[i].right <= optionRects[i].left) return -1;
        const RECT& r = optionRects[i];
        return MAKELRESULT((r.left + r.right) / 2, (r.top + r.bottom) / 2);
    }
    if (msg == kTestMsg && kTestMsg && wp == 17) {
        // Test hook: lp = 1 animations off (as with Windows 「顯示動畫」
        // off), 0 on, 2 follow the Windows setting again.
        motionOverride = lp == 2 ? -1 : lp ? 1 : 0;
        sendMotion();
        return 0;
    }
    if (msg == kTestMsg && kTestMsg && wp == 11) {
        // Test hook: centre of live-toolbar button lp in client px, -1
        // if the toolbar is not up.
        std::lock_guard lk(m);
        const size_t i = static_cast<size_t>(lp);
        if (i >= toolRects.size()) return -1;
        const RECT& r = toolRects[i];
        return MAKELRESULT((r.left + r.right) / 2, (r.top + r.bottom) / 2);
    }
    if (msg == kTestMsg && kTestMsg) {
        // Test hook (pm_video_test --device-loss-at / --gpu-switch-at):
        // 0 simulated device removal, 1 power-saving GPU, 2 WARP, 3 automatic.
        {
            std::lock_guard lk(m);
            testReq = static_cast<int>(wp);
            testArg = lp;
        }
        wake();
        return 0;
    }
    switch (msg) {
    case WM_SIZE: {
        {
            std::lock_guard lk(m);
            pendW = LOWORD(lp);
            pendH = HIWORD(lp);
            resizePending = true;
        }
        wake();
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        markDirty();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        if (!trackingMouse && !synthMouse) {
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
            trackingMouse = TrackMouseEvent(&t) != FALSE;
        }
        if (selecting) {
            selectMouse(msg, lp);
            return 0;
        }
        if (panning) {
            panMove(lp);
            return 0;
        }
        if (slidingTool >= 0) {  // dragging a toolbar slider: the toolbar stays, nothing else
            toolActivity();
            setToolHot(slidingTool, true);
            slideTool(GET_X_LPARAM(lp), false);
            return 0;
        }
        // Live toolbar: shown by any movement; on it, nothing goes to the phone.
        toolActivity();
        const POINT mpt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const bool onTool = !buttonsDown && inToolPill(mpt);
        setToolHot(onTool ? hitTool(mpt) : -1, onTool);
        if (onTool) hoverPending = false;
        // Translation list / markers: theirs, nothing goes to the phone.
        ovOnUi = onTool || buttonsDown ? kOvNone : ovHit(mpt);
        ovSetHot(ovOnUi >= 0 ? ovOnUi : -1);
        if (ovOnUi != kOvNone) {
            hoverPending = false;
            setHover(-1);
            setLinkHot(-1);
            return 0;
        }
        const bool remote = onTool || pointerMove(lp);
        const int opt = remote ? -1 : hitTest(lp);
        setHover(opt);
        const int link = opt < 0 && !remote ? hitLink(lp) : -1;
        setLinkHot(link);
        setMascotHot(opt < 0 && link < 0 && !remote && hitMascot(lp));
        // Activity restarts the idle animation (at most once a second).
        const double now = nowMs();
        if (now - lastPokeUi > 1000) {
            lastPokeUi = now;
            {
                std::lock_guard lk(m);
                pokeReq = true;
            }
            wake();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        trackingMouse = false;
        if (slidingTool >= 0) return 0;  // a slider drag (captured) goes on outside
        ovSetHot(-1);
        ovOnUi = kOvNone;
        setToolHot(-1, false);
        toolLeave();
        setHover(-1);
        setLinkHot(-1);
        setMascotHot(false);
        sendPress(-1, -1);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && (selecting || panning)) {
            SetCursor(LoadCursorW(nullptr, selecting ? IDC_CROSS : IDC_SIZEALL));
            return TRUE;
        }
        if (LOWORD(lp) == HTCLIENT && (hoverUi >= 0 || mascotHotUi || linkHotUi >= 0 || toolHotUi >= 0)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        if (LOWORD(lp) == HTCLIENT && ovOnUi != kOvNone) {
            SetCursor(LoadCursorW(nullptr, ovOnUi == kOvPanel ? IDC_ARROW : IDC_HAND));
            return TRUE;
        }
        if (LOWORD(lp) == HTCLIENT && toolInsideUi) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        if (LOWORD(lp) == HTCLIENT && touchCursor && overPicture()) {
            SetCursor(touchCursor);
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        if (selectMouse(msg, lp)) return 0;
        if (toolPress(lp)) return 0;
        if (!buttonsDown && (ovPress = ovHit({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) != kOvNone) return 0;
        if (panPress(lp)) return 0;
        if (pointerDown(0, lp)) return 0;
        pressedUi = hitTest(lp);
        pressedLink = pressedUi < 0 ? hitLink(lp) : -1;
        pressedMascot = pressedUi < 0 && pressedLink < 0 && hitMascot(lp);
        clearFocus();  // the ring is for the keyboard
        if (pressedUi >= 0) sendPress(video::Renderer::UiOption, pressedUi);
        else if (pressedLink >= 0) sendPress(video::Renderer::UiAction, pressedLink);
        return 0;
    case WM_LBUTTONUP: {
        if (selectMouse(msg, lp)) return 0;
        if (slidingTool >= 0) {  // toolbar slider released
            slideTool(GET_X_LPARAM(lp), true);
            return 0;
        }
        if (ovPress != kOvNone) {  // pressed on the list / a marker: a click if released on the same thing
            const int p = std::exchange(ovPress, kOvNone);
            bool marker = false;
            if (ovHit({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, &marker) == p) ovClick(p, marker);
            return 0;
        }
        if (panning) {
            panEnd();
            return 0;
        }
        sendPress(-1, -1);
        if (pressedTool != -2) {  // pressed on the toolbar: a click if released on the same button
            const int i = std::exchange(pressedTool, -2);
            if (i >= 0 && hitTool({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}) == i) clickTool(i);
            return 0;
        }
        if (pointerUp(0, lp)) return 0;
        const int i = hitTest(lp);
        if (i >= 0 && i == pressedUi) toggleOption(i);
        else if (i < 0 && pressedLink >= 0 && hitLink(lp) == pressedLink) clickLink(pressedLink);
        else if (i < 0 && pressedMascot && hitMascot(lp)) clickMascot();
        pressedUi = -1;
        pressedLink = -1;
        pressedMascot = false;
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        if (selectMouse(msg, lp)) return 0;
        if (toolPress(lp)) return 0;  // second click of a fast double click on the toolbar
        if (!buttonsDown && (ovPress = ovHit({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) != kOvNone) return 0;
        if (panPress(lp)) return 0;   // magnified: a fast second drag, not fullscreen
        // On the picture with a pointer handler: a press for the phone.
        if (pointerDown(0, lp)) return 0;
        // Second click of a fast double click on a check box, the help
        // link or the mascot: a click, not fullscreen.
        pressedUi = hitTest(lp);
        pressedLink = pressedUi < 0 ? hitLink(lp) : -1;
        pressedMascot = pressedUi < 0 && pressedLink < 0 && hitMascot(lp);
        if (pressedUi >= 0) sendPress(video::Renderer::UiOption, pressedUi);
        else if (pressedLink >= 0) sendPress(video::Renderer::UiAction, pressedLink);
        if (pressedUi < 0 && pressedLink < 0 && !pressedMascot) toggleFullscreen();
        return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        if (selectMouse(WM_RBUTTONDOWN, lp)) return 0;
        // Shift+right click keeps the window's context menu on the picture;
        // so does a right click on the toolbar (never sent to the phone).
        if (!buttonsDown && inToolPill({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) break;
        if (!buttonsDown && ovHit({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}) != kOvNone) break;  // the window's menu
        if (GetKeyState(VK_SHIFT) >= 0 && pointerDown(1, lp)) return 0;
        break;
    case WM_RBUTTONUP:
        if (selectMouse(msg, lp)) return 0;
        if (pointerUp(1, lp)) return 0;  // no WM_CONTEXTMENU for a click that went to the phone
        break;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
        if (selectMouse(WM_MBUTTONDOWN, lp)) return 0;
        if (!buttonsDown && inToolPill({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) return 0;
        if (pointerDown(2, lp)) return 0;
        break;
    case WM_MBUTTONUP:
        if (pointerUp(2, lp)) return 0;
        break;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        POINT wpt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (ScreenToClient(hwnd, &wpt) && inToolPill(wpt)) {  // the toolbar's; a slider (volume) steps
            if (msg == WM_MOUSEWHEEL && slidingTool < 0) toolWheel(wpt, GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        }
        if (selecting) return 0;
        if (const int oh = ovHit(wpt); oh != kOvNone && !hitIsMarker(wpt)) {  // scrolls the translation list
            {
                std::lock_guard lk(m);
                ovScrollReq -= GET_WHEEL_DELTA_WPARAM(wp) / static_cast<float>(WHEEL_DELTA) * 60.f;
            }
            wake();
            return 0;
        }
        if (magWheel(msg, wp, lp)) return 0;
        if (pointerWheel(msg, wp, lp)) return 0;
        break;
    }
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lp) != hwnd) {
            if (slidingTool >= 0) {  // slider drag cut short: keep where it got to
                POINT cp{};
                GetCursorPos(&cp);
                ScreenToClient(hwnd, &cp);
                slideTool(cp.x, true);
            }
            cancelButtons();
            panning = false;
            if (selecting && selDragging) endSelect(false);
        }
        break;
    case WM_TIMER:
        if (wp == kHoverTimer) {
            KillTimer(hwnd, kHoverTimer);
            if (std::exchange(hoverPending, false) && !buttonsDown) {
                lastHoverUi = -1e9;
                pointerMove(hoverLp);
            }
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (selecting && wp == VK_ESCAPE) {
            endSelect(false);
            return 0;
        }
        if (idleKey(msg, wp)) return 0;
        if (keyEvent(msg, wp)) return 0;
        if (magKey(msg, wp)) return 0;
        if (wp == VK_F11 || (wp == VK_ESCAPE && fullscreen)) toggleFullscreen();
        return 0;
    case WM_KEYUP:
        if (idleKey(msg, wp)) return 0;
        if (keyEvent(msg, wp)) return 0;
        break;
    case WM_CHAR:
        if (charEvent(wp)) return 0;
        break;
    case WM_SYSKEYDOWN:
        if (magKey(msg, wp)) return 0;  // Alt+arrows pan while magnified
        break;
    case WM_KILLFOCUS:
        releaseKeys();
        endSelect(false);
        panEnd();
        break;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        const int dpi = hwnd ? static_cast<int>(GetDpiForWindow(hwnd)) : 96;
        mmi->ptMinTrackSize = {MulDiv(320, dpi, 96), MulDiv(240, dpi, 96)};
        return 0;
    }
    case WM_DPICHANGED: {
        {
            std::lock_guard lk(m);
            dpiReq = HIWORD(wp);
        }
        wake();
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        if (!fullscreen)
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        else
            refitFullscreen("DPI change");  // the suggested rect is not the monitor
        return 0;
    }
    case WM_SETTINGCHANGE:  // e.g. 「顯示動畫」 (SPI_SETCLIENTAREAANIMATION) switched
        sendMotion();
        break;
    case WM_DISPLAYCHANGE:  // display mode / GPU / monitor topology changed
        requestAdapterCheck();
        refitFullscreen("display change");
        break;
    case WM_WINDOWPOSCHANGED: {
        // Moved to a monitor on another GPU?
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (mon != lastMonitor) {
            const bool first = lastMonitor == nullptr;
            lastMonitor = mon;
            if (!first) requestAdapterCheck();
            if (!first) refitFullscreen("moved to another monitor");  // e.g. its monitor was unplugged
        }
        break;  // DefWindowProc sends WM_SIZE / WM_MOVE
    }
    case WM_SHOWWINDOW: {
        {
            std::lock_guard lk(m);
            visibleReq = wp ? 1 : 0;
        }
        wake();
        break;
    }
    case WM_CLOSE:
        if (stopping) return 0;
        stopping = true;
        stopWorker();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
        hwnd = nullptr;
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK VideoWindow::Impl::staticWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, msg, wp, lp);
    LRESULT r = self->wndProc(msg, wp, lp);
    if (msg == WM_NCDESTROY) SetWindowLongPtrW(h, GWLP_USERDATA, 0);
    return r;
}

}  // namespace pm
