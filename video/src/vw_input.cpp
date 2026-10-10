// VideoWindow::Impl: Android pointer and key forwarding.
// 拆檔 0.7.9：自 video_window.cpp（Impl 類別內）原樣搬出，改成類別外定義。
#include "video_window_impl.h"

namespace pm {

std::shared_ptr<std::function<void(const VideoWindow::Impl::PointerEvent&)>> VideoWindow::Impl::pointerHandler() {
    std::lock_guard lk(m);
    return pointerFn;
}

std::shared_ptr<std::function<void(unsigned, bool, wchar_t)>> VideoWindow::Impl::keyHandler() {
    std::lock_guard lk(m);
    return keyFn;
}

bool VideoWindow::Impl::mapPoint(POINT pt, float& x, float& y, bool clamp) {
    RECT r;
    int rot;
    bool mir;
    video::Renderer::View v;
    {
        std::lock_guard lk(m);
        r = picRect;
        rot = picRot;
        mir = picMirror;
        v = pubView;
    }
    if (r.right <= r.left || r.bottom <= r.top) return false;
    if (!clamp && (pt.x < r.left || pt.x >= r.right || pt.y < r.top || pt.y >= r.bottom)) return false;
    // Pixel centres: the first column maps to 0.5 / width, never 0.
    const float vx = std::clamp((pt.x + 0.5f - r.left) / (r.right - r.left), 0.f, 1.f);
    const float vy = std::clamp((pt.y + 0.5f - r.top) / (r.bottom - r.top), 0.f, 1.f);
    // Magnifier: viewport -> displayed picture.
    const float tx = v.cx + (vx - 0.5f) / v.zoom, ty = v.cy + (vy - 0.5f) / v.zoom;
    video::Renderer::screenToPicture(rot, mir, tx, ty, x, y);
    x = std::clamp(x, 0.f, 1.f);
    y = std::clamp(y, 0.f, 1.f);
    return true;
}

bool VideoWindow::Impl::overPicture() {
    if (!pointerHandler()) return false;
    POINT pt;
    float x, y;
    return GetCursorPos(&pt) && ScreenToClient(hwnd, &pt) && mapPoint(pt, x, y, false);
}

void VideoWindow::Impl::sendPointer(const std::shared_ptr<std::function<void(const PointerEvent&)>>& fn, PointerEvent::Kind kind,
                 float x, float y, int button) {
    PointerEvent e;
    e.kind = kind;
    e.x = lastPx = x;
    e.y = lastPy = y;
    e.button = button;
    if (fn && *fn) (*fn)(e);
}

void VideoWindow::Impl::cancelButtons() {
    if (!buttonsDown) return;
    auto fn = pointerHandler();
    const int mask = std::exchange(buttonsDown, 0);
    for (int b = 0; b < 3; ++b)
        if (mask & (1 << b)) sendPointer(fn, PointerEvent::Kind::Up, lastPx, lastPy, b);
    if (GetCapture() == hwnd) ReleaseCapture();
}

bool VideoWindow::Impl::pointerDown(int button, LPARAM lp) {
    auto fn = pointerHandler();
    float x, y;
    if (!fn || !mapPoint({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, x, y, false)) return false;
    if (!buttonsDown) SetCapture(hwnd);  // drags keep reporting outside the window
    buttonsDown |= 1 << button;
    hoverPending = false;
    sendPointer(fn, PointerEvent::Kind::Down, x, y, button);
    return true;
}

bool VideoWindow::Impl::pointerUp(int button, LPARAM lp) {
    if (!(buttonsDown & (1 << button))) return false;
    buttonsDown &= ~(1 << button);
    auto fn = pointerHandler();
    float x = lastPx, y = lastPy;
    mapPoint({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, x, y, true);
    sendPointer(fn, PointerEvent::Kind::Up, x, y, button);
    if (!buttonsDown && GetCapture() == hwnd) ReleaseCapture();  // WM_CAPTURECHANGED: nothing left to cancel
    return true;
}

bool VideoWindow::Impl::pointerMove(LPARAM lp) {
    auto fn = pointerHandler();
    if (!fn) {
        if (buttonsDown) {
            buttonsDown = 0;
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        return false;
    }
    const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    float x, y;
    if (buttonsDown) {
        if (!mapPoint(pt, x, y, true)) cancelButtons();  // the picture went away mid-drag
        else sendPointer(fn, PointerEvent::Kind::Move, x, y, lowestButton(buttonsDown));
        return true;
    }
    if (!mapPoint(pt, x, y, false)) {
        hoverPending = false;
        return false;
    }
    const double now = nowMs();
    if (now - lastHoverUi >= kHoverMinMs) {
        lastHoverUi = now;
        hoverPending = false;
        sendPointer(fn, PointerEvent::Kind::Move, x, y, -1);
    } else {
        hoverLp = lp;
        if (!hoverPending)
            SetTimer(hwnd, kHoverTimer, static_cast<UINT>(std::ceil(kHoverMinMs - (now - lastHoverUi))), nullptr);
        hoverPending = true;
    }
    return true;
}

bool VideoWindow::Impl::pointerWheel(UINT msg, WPARAM wp, LPARAM lp) {
    auto fn = pointerHandler();
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};  // screen coordinates
    float x, y;
    if (!fn || !ScreenToClient(hwnd, &pt) || !mapPoint(pt, x, y, false)) return false;
    PointerEvent e;
    e.kind = PointerEvent::Kind::Wheel;
    e.x = x;
    e.y = y;
    // Notches: +1 = one detent up (away from the user) / to the right.
    const float notches = GET_WHEEL_DELTA_WPARAM(wp) / static_cast<float>(WHEEL_DELTA);
    (msg == WM_MOUSEWHEEL ? e.wheelY : e.wheelX) = notches;
    (*fn)(e);
    return true;
}

bool VideoWindow::Impl::keyEvent(UINT msg, WPARAM wp) {
    auto fn = keyHandler();
    const unsigned vk = static_cast<unsigned>(wp) & 0xff;
    if (msg == WM_KEYUP) {
        if (!keysDown.test(vk)) return false;
        keysDown.reset(vk);
        if (fn && *fn) (*fn)(vk, false, 0);
        return true;
    }
    if (!fn) return false;
    switch (vk) {
    case VK_F11:
    case VK_F1:          // the app's 快速鍵一覽 (0.7.8)
    case VK_PROCESSKEY:  // IME composition: the result arrives as WM_CHAR
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
        return false;
    case VK_ESCAPE:
        if (fullscreen) return false;
        break;
    }
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
    if (alt) return false;  // incl. AltGr (Ctrl+Alt): its character still comes as WM_CHAR
    if (ctrl && vk != 'C' && vk != 'V' && vk != 'X' && vk != 'A' && vk != 'Z') return false;
    // The character TranslateMessage queued for this key (dead keys: none).
    wchar_t ch = 0;
    MSG cm;
    if (PeekMessageW(&cm, hwnd, WM_CHAR, WM_DEADCHAR, PM_REMOVE) && cm.message == WM_CHAR)
        ch = static_cast<wchar_t>(cm.wParam);
    if (ctrl || ch < 0x20 || ch == 0x7f) ch = 0;  // control characters: the vk says it
    keysDown.set(vk);
    (*fn)(vk, true, ch);
    return true;
}

bool VideoWindow::Impl::charEvent(WPARAM wp) {
    auto fn = keyHandler();
    const wchar_t ch = static_cast<wchar_t>(wp);
    if (!fn || ch < 0x20 || ch == 0x7f) return false;
    if (GetKeyState(VK_MENU) < 0 && GetKeyState(VK_CONTROL) >= 0) return false;  // Alt+numpad etc. stay
    (*fn)(0, true, ch);
    return true;
}

void VideoWindow::Impl::releaseKeys() {
    if (keysDown.none()) return;
    auto fn = keyHandler();
    for (unsigned vk = 0; vk < 256; ++vk)
        if (keysDown.test(vk) && fn && *fn) (*fn)(vk, false, 0);
    keysDown.reset();
}

}  // namespace pm
