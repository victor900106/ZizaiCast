// VideoWindow::Impl: 0.8.3.4 畫面組 -- when the live toolbar comes back (B1)
// and the clickable toast (B2).  UI thread.
#include "video_window_impl.h"

namespace pm {

// B1: near the toolbar = above its last bottom edge + 56 DIP (a band across
// the window's top); before it was ever shown, or after a resize, the top
// fifth of the window.
bool VideoWindow::Impl::nearToolbar(POINT pt) {
    RECT p{};
    {
        std::lock_guard lk(m);
        p = toolPill.right > toolPill.left ? toolPill : lastToolPill;
    }
    if (p.right > p.left) {
        const int dpi = hwnd ? static_cast<int>(GetDpiForWindow(hwnd)) : 96;
        return pt.y <= p.bottom + MulDiv(56, dpi ? dpi : 96, 96);
    }
    RECT c{};
    if (!hwnd || !GetClientRect(hwnd, &c)) return true;
    return pt.y <= c.bottom / 5;
}

void VideoWindow::Impl::setPointerIn(bool in) {
    if (in == pointerInUi) return;
    pointerInUi = in;
    {
        std::lock_guard lk(m);
        pointerInReq = in ? 1 : 0;
    }
    wake();
}

bool VideoWindow::Impl::onToast(POINT pt) {
    std::lock_guard lk(m);
    return toastFn && PtInRect(&toastRect, pt) != FALSE;
}

void VideoWindow::Impl::setToastHot(bool hot) {
    if (hot == toastHotUi) return;
    toastHotUi = hot;
    if (hot) SetCursor(LoadCursorW(nullptr, IDC_HAND));
    {
        std::lock_guard lk(m);
        toastHotReq = hot ? 1 : 0;
    }
    wake();
}

// Left press on the clickable toast: its own (never forwarded to the phone,
// no stroke in pen mode); the click happens on release over it.
bool VideoWindow::Impl::toastPress(LPARAM lp) {
    if (buttonsDown || !onToast({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) return false;
    pressedToast = true;
    return true;
}

bool VideoWindow::Impl::toastRelease(LPARAM lp) {
    if (!std::exchange(pressedToast, false)) return false;
    std::function<void()> fn;
    {
        std::lock_guard lk(m);
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (toastFn && PtInRect(&toastRect, pt)) fn = toastFn;
    }
    if (fn) {
        setToastHot(false);
        fn();  // UI thread
    }
    return true;
}

}  // namespace pm
