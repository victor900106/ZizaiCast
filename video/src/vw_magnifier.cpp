// VideoWindow::Impl: magnifier: view state, zoom / pan, viewport mapping, wheel and keys.
// 拆檔 0.7.9：自 video_window.cpp（Impl 類別內）原樣搬出，改成類別外定義。
#include "video_window_impl.h"

namespace pm {

void VideoWindow::Impl::applyView(video::Renderer::View v, bool user) {
    v = video::Renderer::clampView(v);
    std::shared_ptr<std::function<void(const VideoWindow::ViewState&)>> fn;
    VideoWindow::ViewState vs;
    {
        std::lock_guard lk(m);
        viewUi = v;
        viewChanged = true;
        fn = viewFn;
        vs = viewStateLocked();
    }
    wake();
    if (user && fn && *fn) (*fn)(vs);
}

VideoWindow::ViewState VideoWindow::Impl::viewStateLocked() const {
    VideoWindow::ViewState vs;
    vs.zoom = viewUi.zoom;
    vs.centerX = viewUi.cx;
    vs.centerY = viewUi.cy;
    vs.filter = static_cast<VideoWindow::Filter>(viewUi.filter);
    vs.frozen = frozenUi;
    return vs;
}

video::Renderer::View VideoWindow::Impl::currentView() {
    std::lock_guard lk(m);
    return viewUi;
}

void VideoWindow::Impl::zoomAtV(float zoom, float vx, float vy, bool user) {
    video::Renderer::View v = currentView();
    zoom = std::clamp(zoom, 1.f, video::Renderer::kMaxZoom);
    if (std::fabs(zoom - 1) < 0.06f) zoom = 1;
    // The picture point under (vx, vy) stays: c' = c + (v - .5) * (1/z - 1/z').
    v.cx += (vx - 0.5f) * (1 / v.zoom - 1 / zoom);
    v.cy += (vy - 0.5f) * (1 / v.zoom - 1 / zoom);
    v.zoom = zoom;
    applyView(v, user);
}

void VideoWindow::Impl::panV(float dx, float dy, bool user) {
    video::Renderer::View v = currentView();
    if (v.zoom <= 1.001f) return;
    v.cx += dx / v.zoom;
    v.cy += dy / v.zoom;
    applyView(v, user);
}

bool VideoWindow::Impl::viewportPoint(POINT pt, float& vx, float& vy, bool clamp) {
    RECT r;
    {
        std::lock_guard lk(m);
        r = picRect;
    }
    if (r.right <= r.left || r.bottom <= r.top) return false;
    if (!clamp && (pt.x < r.left || pt.x >= r.right || pt.y < r.top || pt.y >= r.bottom)) return false;
    vx = std::clamp((pt.x + 0.5f - r.left) / (r.right - r.left), 0.f, 1.f);
    vy = std::clamp((pt.y + 0.5f - r.top) / (r.bottom - r.top), 0.f, 1.f);
    return true;
}

void VideoWindow::Impl::viewportToContent(float vx, float vy, float& dx, float& dy) {
    video::Renderer::View v;
    bool mir;
    {
        std::lock_guard lk(m);
        v = pubView;
        mir = picMirror;
    }
    const float tx = v.cx + (vx - 0.5f) / v.zoom, ty = v.cy + (vy - 0.5f) / v.zoom;
    dx = std::clamp(mir ? 1 - tx : tx, 0.f, 1.f);
    dy = std::clamp(ty, 0.f, 1.f);
}

bool VideoWindow::Impl::magWheel(UINT msg, WPARAM wp, LPARAM lp) {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    float vx, vy;
    if (!ScreenToClient(hwnd, &pt) || !viewportPoint(pt, vx, vy, false)) return false;
    const float notches = GET_WHEEL_DELTA_WPARAM(wp) / static_cast<float>(WHEEL_DELTA);
    const bool ctrl = (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) != 0;
    if (ctrl && msg == WM_MOUSEWHEEL) {
        const float z = currentView().zoom * std::pow(1.25f, notches);
        zoomAtV(z, vx, vy, true);
        return true;
    }
    if (currentView().zoom <= 1.001f || pointerHandler()) return false;
    const bool side = msg == WM_MOUSEHWHEEL || (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) != 0;
    const float step = 0.15f * notches;
    if (msg == WM_MOUSEHWHEEL) panV(step, 0, true);
    else if (side) panV(-step, 0, true);
    else panV(0, -step, true);
    return true;
}

bool VideoWindow::Impl::panPress(LPARAM lp) {
    const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    float vx, vy;
    if (buttonsDown || currentView().zoom <= 1.001f || !viewportPoint(pt, vx, vy, false) || inToolPill(pt))
        return false;
    if (pointerHandler() && GetKeyState(VK_CONTROL) >= 0) return false;
    panning = true;
    panLast = pt;
    SetCapture(hwnd);
    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    return true;
}

void VideoWindow::Impl::panMove(LPARAM lp) {
    const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    RECT r;
    {
        std::lock_guard lk(m);
        r = picRect;
    }
    if (r.right > r.left && r.bottom > r.top && (pt.x != panLast.x || pt.y != panLast.y))
        panV(-static_cast<float>(pt.x - panLast.x) / (r.right - r.left),
             -static_cast<float>(pt.y - panLast.y) / (r.bottom - r.top), true);
    panLast = pt;
}

void VideoWindow::Impl::panEnd() {
    if (!panning) return;
    panning = false;
    if (GetCapture() == hwnd) ReleaseCapture();
}

bool VideoWindow::Impl::magKey(UINT msg, WPARAM wp) {
    const unsigned vk = static_cast<unsigned>(wp) & 0xff;
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    const bool alt = msg == WM_SYSKEYDOWN || GetKeyState(VK_MENU) < 0;
    if (ctrl && !alt && (vk == VK_OEM_PLUS || vk == VK_ADD)) {
        const auto v = currentView();
        zoomAtV(stepZoom(v.zoom, 1), 0.5f, 0.5f, true);
        return true;
    }
    if (ctrl && !alt && (vk == VK_OEM_MINUS || vk == VK_SUBTRACT)) {
        const auto v = currentView();
        zoomAtV(stepZoom(v.zoom, -1), 0.5f, 0.5f, true);
        return true;
    }
    if (ctrl && shift && !alt && (vk == '0' || vk == VK_NUMPAD0)) {
        auto v = currentView();
        v.zoom = 1;
        applyView(v, true);
        return true;
    }
    if (ctrl || currentView().zoom <= 1.001f) return false;
    float dx = 0, dy = 0;
    switch (vk) {
    case VK_LEFT: dx = -0.1f; break;
    case VK_RIGHT: dx = 0.1f; break;
    case VK_UP: dy = -0.1f; break;
    case VK_DOWN: dy = 0.1f; break;
    default: return false;
    }
    panV(dx, dy, true);
    return true;
}

}  // namespace pm
