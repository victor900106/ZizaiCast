// pm::VideoWindow public API: magnifier, filters, freeze, text overlay, region selection, snapshots, grabPicture.
// 拆檔 0.7.9：自 video_window.cpp 原樣搬出。
#include "pm/video_window.h"

#include "video_window_impl.h"

namespace pm {

void VideoWindow::setZoom(float zoom) {
    auto v = impl_->currentView();
    v.zoom = zoom;
    impl_->applyView(v, false);
}

void VideoWindow::zoomAt(float zoom, float vx, float vy) { impl_->zoomAtV(zoom, vx, vy, false); }

void VideoWindow::zoomStep(int steps) {
    impl_->zoomAtV(Impl::stepZoom(impl_->currentView().zoom, steps), 0.5f, 0.5f, false);
}

void VideoWindow::panBy(float dx, float dy) { impl_->panV(dx, dy, false); }

void VideoWindow::resetMagnifier() {
    auto v = impl_->currentView();
    v.zoom = 1;
    v.cx = v.cy = 0.5f;
    impl_->applyView(v, false);
}

void VideoWindow::setFilter(Filter f) {
    auto v = impl_->currentView();
    v.filter = static_cast<int>(f);
    impl_->applyView(v, false);
}

void VideoWindow::setFrozen(bool frozen) {
    {
        std::lock_guard lk(impl_->m);
        if (impl_->frozenUi == frozen && impl_->frozenReq < 0) return;
        impl_->frozenUi = frozen;
        impl_->frozenReq = frozen ? 1 : 0;
    }
    impl_->wake();
}

VideoWindow::ViewState VideoWindow::viewState() const {
    std::lock_guard lk(impl_->m);
    return impl_->viewStateLocked();
}

void VideoWindow::setViewHandler(std::function<void(const ViewState&)> fn) {
    auto p = fn ? std::make_shared<std::function<void(const ViewState&)>>(std::move(fn)) : nullptr;
    std::lock_guard lk(impl_->m);
    impl_->viewFn = std::move(p);
}

void VideoWindow::setTextOverlay(std::vector<TextBox> boxes) {
    std::vector<video::Renderer::TextBox> out;
    out.reserve(boxes.size());
    for (auto& b : boxes) {
        out.push_back({b.x0, b.y0, b.x1, b.y1, std::move(b.text), std::move(b.original), std::max(1, b.lines), b.bg, b.fg,
                       b.colors});
        out.back().online = b.online;
    }
    {
        std::lock_guard lk(impl_->m);
        impl_->boxesReq = std::move(out);
        impl_->onlineReq.reset();  // (marks of the previous boxes)
        impl_->ovSelUi = -1;
    }
    impl_->wake();
}

void VideoWindow::setTextOverlayOnline(std::vector<std::wstring> originals) {
    {
        std::lock_guard lk(impl_->m);
        impl_->onlineReq = std::move(originals);
    }
    impl_->wake();
}

void VideoWindow::setTextOverlayStyle(int mode, bool dark) {
    {
        std::lock_guard lk(impl_->m);
        impl_->ovModeReq = std::clamp(mode, 0, 2);
        impl_->ovDarkReq = dark ? 1 : 0;
    }
    impl_->wake();
}

VideoWindow::TextOverlayInfo VideoWindow::textOverlayInfo() const {
    std::lock_guard lk(impl_->m);
    const auto& h = impl_->ovHits;
    TextOverlayInfo i;
    i.inPlace = h.inPlace;
    i.listed = h.listed;
    i.notFitting = h.notFitting;
    i.listAll = h.listAll;
    i.zoomButton = h.zoomBtn.right > h.zoomBtn.left;
    i.overlaps = h.overlaps;
    i.tooClose = h.tooClose;
    i.truncated = h.truncated;
    i.kinsoku = h.kinsoku;
    i.shortLast = h.shortLast;
    i.markerClashes = h.markerClashes;
    i.fontSizes = h.fontSizes;
    i.minFontPx = h.minFont;
    i.old070Cards = h.old070Cards;
    i.old070Overlaps = h.old070Overlaps;
    i.old070Cut = h.old070Cut;
    i.old070ShortLast = h.old070ShortLast;
    return i;
}

void VideoWindow::setTextOverlayOriginal(bool showOriginal) {
    {
        std::lock_guard lk(impl_->m);
        impl_->originalReq = showOriginal ? 1 : 0;
    }
    impl_->wake();
}

void VideoWindow::setOverlayBusy(const std::wstring& label) {
    {
        std::lock_guard lk(impl_->m);
        impl_->busyReq = label;
    }
    impl_->wake();
}

namespace {
// Runs fn on the window's UI thread (synchronously when already on it).
void onUiThread(HWND h, std::function<void()> fn) {
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

void VideoWindow::beginRegionSelect(std::function<void(bool ok, float x0, float y0, float x1, float y1)> done) {
    Impl* impl = impl_.get();
    onUiThread(impl->hwnd.load(), [impl, done = std::move(done)]() mutable { impl->startSelect(std::move(done)); });
}

void VideoWindow::post(std::function<void()> fn) {
    if (fn) onUiThread(impl_->hwnd.load(), std::move(fn));
}

void VideoWindow::cancelRegionSelect() {
    Impl* impl = impl_.get();
    onUiThread(impl->hwnd.load(), [impl] { impl->endSelect(false); });
}

bool VideoWindow::saveSnapshotFramed(const std::wstring& pngPath) { return impl_->snapshot(pngPath, true); }

bool VideoWindow::saveSnapshot(const std::wstring& pngPath) { return impl_->snapshot(pngPath, false); }

bool VideoWindow::saveWindowShot(const std::wstring& pngPath) { return impl_->snapshot(pngPath, false, true); }

bool VideoWindow::Impl::snapshot(const std::wstring& pngPath, bool framed, bool ui) {
    auto req = runSnapshot(framed, ui, false);
    if (!req) return false;  // nothing shown yet
    return video::writePng(pngPath, req->pixels.data(), req->w, req->h, framed);
}

bool VideoWindow::grabPicture(std::vector<uint8_t>& bgra, int& width, int& height) {
    width = height = 0;
    auto req = impl_->runSnapshot(false, false, true);
    if (!req) return false;
    bgra = std::move(req->pixels);
    for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 255;
    width = static_cast<int>(req->w);
    height = static_cast<int>(req->h);
    return true;
}

std::shared_ptr<SnapshotRequest> VideoWindow::Impl::runSnapshot(bool framed, bool ui, bool grab) {
    if (!worker.joinable()) return nullptr;
    auto req = std::make_shared<SnapshotRequest>();
    req->framed = framed;
    req->ui = ui;
    req->grab = grab;
    {
        std::lock_guard lk(m);
        if (stop) return nullptr;
        snapReqs.push_back(req);
    }
    wake();
    // The worker converts the picture on the GPU (a few ms).  Wait without
    // dispatching input, but keep serving cross-thread sent messages so a
    // worker that needs the window thread cannot deadlock against us.
    const ULONGLONG deadline = GetTickCount64() + 3000;
    while (true) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        DWORD r = MsgWaitForMultipleObjects(1, &req->done, FALSE, static_cast<DWORD>(deadline - now), QS_SENDMESSAGE);
        if (r == WAIT_OBJECT_0) break;
        if (r == WAIT_OBJECT_0 + 1) {
            MSG msg;
            PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
            continue;
        }
        break;
    }
    if (WaitForSingleObject(req->done, 0) != WAIT_OBJECT_0) {
        std::lock_guard lk(m);
        snapReqs.erase(std::remove(snapReqs.begin(), snapReqs.end(), req), snapReqs.end());
        log("snapshot timed out");
        return nullptr;
    }
    return req->ok ? req : nullptr;
}

}  // namespace pm

namespace pm {
void VideoWindow::setTextOverlayLive(bool on) {
    impl_->liveOvReq = on ? 1 : 0;
    impl_->wake();
}
}  // namespace pm
