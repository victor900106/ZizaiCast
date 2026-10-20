// VideoWindow::Impl: 簡報畫筆 (0.8.3) input + API, 拖檔到視窗 hint.
// The strokes themselves: ink_layer.*; drawn by renderer_ink.cpp.
#include "video_window_impl.h"

namespace pm {

namespace {
// Runs fn on the window's thread (at once when already on it).
void runOnUi(HWND h, std::function<void()> fn) {
    if (!h) return;
    if (GetWindowThreadProcessId(h, nullptr) == GetCurrentThreadId()) {
        fn();
        return;
    }
    static const UINT msg = RegisterWindowMessageW(L"PhoneMirror.Video.RunOnUi");
    auto* p = new std::function<void()>(std::move(fn));
    if (!PostMessageW(h, msg, 0, reinterpret_cast<LPARAM>(p))) delete p;
}
}  // namespace

bool VideoWindow::Impl::inkPoint(POINT pt, bool clamp, float& ux, float& uy, float* aspect, float* pxPerU) {
    float x, y;
    if (!mapPoint(pt, x, y, clamp)) return false;
    const int sw = srcW.load(std::memory_order_relaxed), sh = srcH.load(std::memory_order_relaxed);
    if (sw <= 0 || sh <= 0) return false;
    const float a = static_cast<float>(sw) / sh;
    float ax, ay;
    video::InkLayer::axes(a, ax, ay);
    ux = x * ax;
    uy = y * ay;
    if (aspect) *aspect = a;
    if (pxPerU) {
        RECT r;
        int rot;
        float zoom;
        {
            std::lock_guard lk(m);
            r = picRect;
            rot = picRot;
            zoom = pubView.zoom;
        }
        // The picture's displayed width in U units: ax, or ay when turned a quarter.
        *pxPerU = (r.right - r.left) * std::max(1.f, zoom) / ((rot & 1) ? ay : ax);
    }
    return true;
}

bool VideoWindow::Impl::inkPress(LPARAM lp) {
    if (!inkOn.load() || inking || GetKeyState(VK_CONTROL) < 0) return false;  // Ctrl+drag: pan (magnifier)
    const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    float ux, uy, a, ppu = 0;
    if (!inkPoint(pt, false, ux, uy, &a, &ppu) || ppu <= 0) return false;
    const float s = static_cast<float>(GetDpiForWindow(hwnd)) / 96.f;
    video::InkLayer::Style st;
    st.tool = static_cast<int>(inkStyle.tool);
    st.rgb = inkStyle.color & 0xFFFFFF;
    float wDip = std::clamp(inkStyle.widthDip, 1.f, 64.f);
    if (inkStyle.tool == InkTool::Highlighter) wDip = std::max(16.f, wDip * 3.2f);  // a marker is broad
    st.width = wDip * s / ppu;
    ink.begin(st, ux, uy, a);
    inking = true;
    inkLast = pt;
    SetCapture(hwnd);
    markDirty();
    return true;
}

void VideoWindow::Impl::inkMove(LPARAM lp) {
    if (!inking) return;
    const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (std::abs(pt.x - inkLast.x) + std::abs(pt.y - inkLast.y) < 2) return;  // sub-pixel jitter: no new point
    float ux, uy;
    if (!inkPoint(pt, true, ux, uy)) return;
    inkLast = pt;
    ink.add(ux, uy);
    markDirty();
}

void VideoWindow::Impl::inkEnd(bool commit) {
    if (!inking) return;
    inking = false;  // first: ReleaseCapture sends WM_CAPTURECHANGED
    if (commit) ink.end();
    else ink.cancel();
    if (GetCapture() == hwnd) ReleaseCapture();
    inkChanged();
}

void VideoWindow::Impl::inkChanged() {
    markDirty();
    std::shared_ptr<std::function<void(int)>> fn;
    {
        std::lock_guard lk(m);
        fn = inkFn;
    }
    if (fn && *fn) (*fn)(ink.count());
}

void VideoWindow::Impl::inkDroppedByPicture() {
    runOnUi(hwnd, [this] {
        if (inking) {  // the stroke in progress went with the others
            inking = false;
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        log("ink: the picture changed shape, strokes dropped");
        inkChanged();
    });
}

bool VideoWindow::Impl::overInkPicture() {
    if (!inkOn.load()) return false;
    POINT pt;
    float x, y;
    return GetCursorPos(&pt) && ScreenToClient(hwnd, &pt) && mapPoint(pt, x, y, false);
}

// ---- public API ----

void VideoWindow::setInkMode(bool on, const std::wstring& badge) {
    Impl* d = impl_.get();
    runOnUi(d->hwnd.load(), [d, on, badge] {
        if (on && !d->inkOn.load()) {
            d->cancelButtons();  // a press for the phone ends here (its Up goes to the phone)
            d->hoverPending = false;
        }
        if (!on) d->inkEnd(true);
        d->inkOn.store(on);
        {
            std::lock_guard lk(d->m);
            d->inkBadgeReq = on ? badge : std::wstring();
            d->dirty = true;
        }
        d->wake();
        POINT pt;  // the cursor over the picture changes at once (pen / ring)
        if (GetCursorPos(&pt) && WindowFromPoint(pt) == d->hwnd.load())
            PostMessageW(d->hwnd.load(), WM_SETCURSOR, reinterpret_cast<WPARAM>(d->hwnd.load()), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    });
}

bool VideoWindow::inkMode() const { return impl_->inkOn.load(); }

void VideoWindow::setInkStyle(const InkStyle& style) {
    Impl* d = impl_.get();
    runOnUi(d->hwnd.load(), [d, style] { d->inkStyle = style; });
}

bool VideoWindow::inkUndo() {
    Impl* d = impl_.get();
    if (d->inking) {  // undo while drawing: the stroke in progress goes
        d->inkEnd(false);
        return true;
    }
    const bool ok = d->ink.undo();
    d->markDirty();
    return ok;
}

void VideoWindow::inkClear() {
    Impl* d = impl_.get();
    if (d->inking) {
        d->inking = false;
        if (GetCapture() == d->hwnd.load()) ReleaseCapture();
    }
    d->ink.clear();
    d->markDirty();
}

int VideoWindow::inkCount() const { return impl_->ink.count(); }

void VideoWindow::setInkCapture(bool inSnapshots, bool inRecordings) { impl_->ink.setCapture(inSnapshots, inRecordings); }

void VideoWindow::setInkHandler(std::function<void(int strokes)> fn) {
    std::lock_guard lk(impl_->m);
    impl_->inkFn = fn ? std::make_shared<std::function<void(int)>>(std::move(fn)) : nullptr;
}

void VideoWindow::setDropHint(const std::wstring& title, const std::wstring& sub) {
    {
        std::lock_guard lk(impl_->m);
        impl_->dropHintReq = std::make_pair(title, title.empty() ? std::wstring() : sub);
        impl_->dirty = true;
    }
    impl_->wake();
}

}  // namespace pm
