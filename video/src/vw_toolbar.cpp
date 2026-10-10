// VideoWindow::Impl: live toolbar interaction (hit test, hover, clicks, sliders, wheel).
// 拆檔 0.7.9：自 video_window.cpp（Impl 類別內）原樣搬出，改成類別外定義。
#include "video_window_impl.h"

namespace pm {

int VideoWindow::Impl::hitTool(POINT pt) {
    std::lock_guard lk(m);
    for (size_t i = 0; i < toolRects.size(); ++i)
        if (PtInRect(&toolRects[i], pt)) return static_cast<int>(i);
    return -1;
}

bool VideoWindow::Impl::inToolPill(POINT pt) {
    std::lock_guard lk(m);
    return PtInRect(&toolPill, pt) != FALSE;
}

void VideoWindow::Impl::setToolHot(int hot, bool inside) {
    if (hot == toolHotUi && inside == toolInsideUi) return;
    toolHotUi = hot;
    toolInsideUi = inside;
    if (hot >= 0) SetCursor(LoadCursorW(nullptr, IDC_HAND));
    {
        std::lock_guard lk(m);
        toolHoverReq = hot;
        toolInsideReq = inside;
    }
    wake();
}

void VideoWindow::Impl::toolActivity() {
    const double now = nowMs();
    if (now - lastToolPokeUi < 200) return;
    lastToolPokeUi = now;
    {
        std::lock_guard lk(m);
        if (toolIds.empty()) return;
        toolActivityReq = true;
    }
    wake();
}

void VideoWindow::Impl::toolLeave() {
    lastToolPokeUi = -1e9;
    {
        std::lock_guard lk(m);
        toolLeaveReq = true;
    }
    wake();
}

void VideoWindow::Impl::clickTool(int i) {
    std::function<void(int)> cb;
    int id = 0;
    {
        std::lock_guard lk(m);
        if (i < 0 || i >= static_cast<int>(toolIds.size())) return;
        id = toolIds[i];
        cb = toolFn;
    }
    if (cb) cb(id);  // UI thread
}

bool VideoWindow::Impl::toolPress(LPARAM lp) {
    const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (buttonsDown || !inToolPill(pt)) return false;
    pressedTool = hitTool(pt);
    if (pressedTool >= 0 && toolIsSlider(pressedTool)) {  // a slider: drag (capture) from here
        slidingTool = pressedTool;
        pressedTool = -2;
        SetCapture(hwnd);
        slideTool(pt.x, false);
    } else if (pressedTool >= 0) {
        sendPress(video::Renderer::UiTool, pressedTool);
    }
    return true;
}

bool VideoWindow::Impl::toolIsSlider(int i) {
    std::lock_guard lk(m);
    return i >= 0 && i < static_cast<int>(toolItemsUi.size()) && toolItemsUi[i].slider >= 0;
}

void VideoWindow::Impl::slideTool(int x, bool done) {
    const int i = slidingTool;
    std::function<void(int, float, bool)> cb;
    int id = 0;
    float v = 0;
    bool ok = true;
    {
        std::lock_guard lk(m);
        if (i < 0 || i >= static_cast<int>(toolItemsUi.size()) || i >= static_cast<int>(toolIds.size()) ||
            toolItemsUi[i].slider < 0) {  // the toolbar changed under the drag
            ok = false;
        } else {
            v = toolItemsUi[i].slider;
        }
    }
    if (!ok) {
        slidingTool = -1;
        if (GetCapture() == hwnd) ReleaseCapture();
        return;
    }
    {
        std::lock_guard lk(m);
        if (i < static_cast<int>(toolRects.size()) && toolRects[i].right > toolRects[i].left) {
            const RECT& r = toolRects[i];
            const float inset = (r.bottom - r.top) * video::Renderer::kToolSliderInset;
            const float span = std::max(1.f, (r.right - r.left) - 2 * inset);
            v = std::clamp((x - (r.left + inset)) / span, 0.f, 1.f);
        }
        if (v != toolItemsUi[i].slider) {
            toolItemsUi[i].slider = v;
            toolbarReq = toolItemsUi;
            dirty = true;
        }
        id = toolIds[i];
        cb = toolSlideFn;
    }
    wake();
    if (done) {
        slidingTool = -1;
        if (GetCapture() == hwnd) ReleaseCapture();
    }
    if (cb) cb(id, v, done);  // UI thread
}

bool VideoWindow::Impl::toolWheel(POINT pt, int delta) {
    const int hit = hitTool(pt);
    std::function<void(int, int)> cb;
    int id = 0;
    {
        std::lock_guard lk(m);
        const int n = static_cast<int>(std::min(toolItemsUi.size(), toolIds.size()));
        int i = -1;
        if (hit >= 0 && hit < n && toolItemsUi[hit].slider >= 0) i = hit;
        else if (hit >= 0 && hit + 1 < n && toolItemsUi[hit + 1].slider >= 0) i = hit + 1;
        if (i < 0) return false;
        id = toolIds[i];
        cb = toolWheelFn;
    }
    toolWheelAcc += delta;
    const int notches = toolWheelAcc / WHEEL_DELTA;
    toolWheelAcc -= notches * WHEEL_DELTA;
    if (notches && cb) cb(id, notches);  // UI thread
    return true;
}

}  // namespace pm
