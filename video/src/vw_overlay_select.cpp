// VideoWindow::Impl: text overlay markers and region selection.
// 拆檔 0.7.9：自 video_window.cpp（Impl 類別內）原樣搬出，改成類別外定義。
#include "video_window_impl.h"

namespace pm {

int VideoWindow::Impl::ovHit(POINT pt, bool* marker) {
    std::lock_guard lk(m);
    const auto& h = ovHits;
    if (PtInRect(&h.zoomBtn, pt)) return kOvZoom;
    for (const auto& [r, i] : h.rows)
        if (PtInRect(&r, pt)) {
            if (marker) *marker = false;
            return i;
        }
    if (PtInRect(&h.panel, pt)) return kOvPanel;
    for (const auto& [r, i] : h.markers)
        if (PtInRect(&r, pt)) {
            if (marker) *marker = true;
            return i;
        }
    return kOvNone;
}

bool VideoWindow::Impl::hitIsMarker(POINT pt) {
    bool marker = false;
    ovHit(pt, &marker);
    return marker;
}

void VideoWindow::Impl::ovSetHot(int i) {
    if (i == ovHotUi) return;
    ovHotUi = i;
    {
        std::lock_guard lk(m);
        ovHotReq = i;
    }
    wake();
}

void VideoWindow::Impl::ovClick(int hit, bool marker) {
    if (hit == kOvZoom) {  // 放大這一塊: the listed blocks fill the view
        float z[4];
        bool mir;
        {
            std::lock_guard lk(m);
            std::copy(std::begin(ovHits.zoomTo), std::end(ovHits.zoomTo), z);
            mir = picMirror;
        }
        if (z[2] <= z[0] || z[3] <= z[1]) return;
        video::Renderer::View v = currentView();
        const float cx = (z[0] + z[2]) / 2;
        v.cx = mir ? 1 - cx : cx;
        v.cy = (z[1] + z[3]) / 2;
        v.zoom = std::min(0.9f / (z[2] - z[0]), 0.9f / (z[3] - z[1]));
        applyView(v, true);
        return;
    }
    if (hit < 0) return;
    {
        std::lock_guard lk(m);
        ovSelUi = (ovSelUi == hit && !marker) ? -1 : hit;  // a row clicked again: deselected
        ovSelReq = ovSelUi;
        ovRevealReq = marker;
    }
    wake();
}

void VideoWindow::Impl::sendSelection() {
    {
        std::lock_guard lk(m);
        selChanged = true;
        selActiveReq = selecting;
        std::copy(std::begin(selV), std::end(selV), selReq);
    }
    wake();
}

void VideoWindow::Impl::startSelect(std::function<void(bool, float, float, float, float)> done) {
    if (selecting) endSelect(false);
    cancelButtons();
    panEnd();
    selecting = true;
    selDragging = false;
    selDone = std::move(done);
    std::fill(std::begin(selV), std::end(selV), -1.f);
    sendSelection();
    POINT pt;
    if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (PtInRect(&rc, pt)) SetCursor(LoadCursorW(nullptr, IDC_CROSS));
    }
}

void VideoWindow::Impl::endSelect(bool ok) {
    if (!selecting) return;
    selecting = false;
    const bool dragged = selDragging;
    selDragging = false;
    if (GetCapture() == hwnd) ReleaseCapture();
    auto done = std::move(selDone);
    selDone = nullptr;
    float d[4] = {0, 0, 0, 0};
    if (ok && dragged) {
        viewportToContent(std::min(selV[0], selV[2]), std::min(selV[1], selV[3]), d[0], d[1]);
        viewportToContent(std::max(selV[0], selV[2]), std::max(selV[1], selV[3]), d[2], d[3]);
        if (d[0] > d[2]) std::swap(d[0], d[2]);  // mirrored
        RECT r;
        {
            std::lock_guard lk(m);
            r = picRect;
        }
        // A click or a sliver (< 8 x 8 px on screen) is not a selection.
        const float wPx = std::fabs(selV[2] - selV[0]) * (r.right - r.left);
        const float hPx = std::fabs(selV[3] - selV[1]) * (r.bottom - r.top);
        ok = wPx >= 8 && hPx >= 8;
    } else {
        ok = false;
    }
    std::fill(std::begin(selV), std::end(selV), -1.f);
    sendSelection();
    if (done) done(ok, d[0], d[1], d[2], d[3]);
}

bool VideoWindow::Impl::selectMouse(UINT msg, LPARAM lp) {
    if (!selecting) return false;
    const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    float vx, vy;
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (!viewportPoint(pt, vx, vy, false)) {
            endSelect(false);
            return true;
        }
        selDragging = true;
        selV[0] = selV[2] = vx;
        selV[1] = selV[3] = vy;
        SetCapture(hwnd);
        sendSelection();
        return true;
    case WM_MOUSEMOVE:
        if (selDragging && viewportPoint(pt, vx, vy, true)) {
            selV[2] = vx;
            selV[3] = vy;
            sendSelection();
        }
        return true;
    case WM_LBUTTONUP:
        if (selDragging) {
            if (viewportPoint(pt, vx, vy, true)) {
                selV[2] = vx;
                selV[3] = vy;
            }
            endSelect(true);
        }
        return true;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
        endSelect(false);
        return true;
    }
    return false;
}

}  // namespace pm
