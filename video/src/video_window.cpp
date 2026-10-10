#include "pm/video_window.h"

#include "video_window_impl.h"

namespace pm {

namespace detail {
const UINT kTestMsg = RegisterWindowMessageW(L"PhoneMirror.Video.Test");
const UINT kCancelSelectMsg = RegisterWindowMessageW(L"PhoneMirror.Video.CancelSelect");
}  // namespace detail

VideoWindow::VideoWindow() : impl_(std::make_unique<Impl>()) {}

VideoWindow::~VideoWindow() {
    impl_->stopping = true;
    impl_->stopWorker();
    if (impl_->hwnd) DestroyWindow(impl_->hwnd);
    if (impl_->touchCursor) DestroyCursor(impl_->touchCursor);
}

bool VideoWindow::create(const wchar_t* title, int clientWidth, int clientHeight) {
    if (impl_->hwnd) return true;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    if (!GetClassInfoExW(inst, kClassName, &wc)) {
        wc = {sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &Impl::staticWndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) {
            log("RegisterClassEx failed (%lu)", GetLastError());
            return false;
        }
    }
    RECT rc{0, 0, clientWidth, clientHeight};
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
    // Test hook (PM_VIDEO_OFFSCREEN=1, e.g. pm_video_test --mascot-tour):
    // far off the desktop, no taskbar button, never activated, so scripted
    // screenshots (saveWindowShot) can run next to someone using the PC.
    wchar_t off[4] = {};
    const bool offscreen = GetEnvironmentVariableW(L"PM_VIDEO_OFFSCREEN", off, 4) && off[0] == L'1';
    CreateWindowExW(offscreen ? WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE : 0, kClassName, title ? title : L"PhoneMirror",
                    WS_OVERLAPPEDWINDOW, offscreen ? -12000 : CW_USEDEFAULT, offscreen ? 200 : CW_USEDEFAULT,
                    rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, impl_.get());
    if (!impl_->hwnd) {
        log("CreateWindowEx failed (%lu)", GetLastError());
        return false;
    }
    // The requested client size is in DIPs: scale it for the window's monitor.
    if (UINT dpi = GetDpiForWindow(impl_->hwnd); dpi && dpi != 96) {
        RECT sr{0, 0, MulDiv(clientWidth, dpi, 96), MulDiv(clientHeight, dpi, 96)};
        AdjustWindowRectExForDpi(&sr, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        SetWindowPos(impl_->hwnd, nullptr, 0, 0, sr.right - sr.left, sr.bottom - sr.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // On the work area: 540x960 DIPs at 150 % is 1440 px tall, more than a
    // 1080p screen. Shrink it (same shape) to fit, then move it fully inside.
    MONITORINFO mi{sizeof(mi)};
    RECT wr{}, cr{};
    if (!offscreen && GetWindowRect(impl_->hwnd, &wr) && GetClientRect(impl_->hwnd, &cr) && cr.right > 0 &&
        cr.bottom > 0 && GetMonitorInfoW(MonitorFromWindow(impl_->hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        const RECT& wa = mi.rcWork;
        const int fw = (wr.right - wr.left) - cr.right, fh = (wr.bottom - wr.top) - cr.bottom;  // frame + caption
        const double k = (std::min)({1.0, static_cast<double>((wa.right - wa.left) - fw) / cr.right,
                                     static_cast<double>((wa.bottom - wa.top) - fh) / cr.bottom});
        const int w = static_cast<int>(cr.right * k) + fw, h = static_cast<int>(cr.bottom * k) + fh;
        const int x = (std::max)(static_cast<int>(wa.left), (std::min)(static_cast<int>(wr.left), static_cast<int>(wa.right) - w));
        const int y = (std::max)(static_cast<int>(wa.top), (std::min)(static_cast<int>(wr.top), static_cast<int>(wa.bottom) - h));
        if (k < 1.0 || x != wr.left || y != wr.top) {
            log("first window %ldx%ld at (%ld,%ld) does not fit the work area %ldx%ld: %dx%d at (%d,%d)", wr.right - wr.left,
                wr.bottom - wr.top, wr.left, wr.top, wa.right - wa.left, wa.bottom - wa.top, w, h, x, y);
            SetWindowPos(impl_->hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    if (!impl_->touchCursor) impl_->touchCursor = createTouchCursor();
    std::promise<bool> ready;
    auto fut = ready.get_future();
    impl_->worker = std::thread([this, p = std::move(ready)]() mutable { impl_->workerMain(p); });
    if (!fut.get()) {
        impl_->worker.join();
        DestroyWindow(impl_->hwnd);
        return false;
    }
    impl_->startMonitor();
    impl_->sendMotion();
    if (!impl_->wd) log("watchdog off (PM_VIDEO_WATCHDOG=0): 0.6.1 behaviour");
    // PM_VIDEO_START_HIDDEN=1 (the app's --background start): never shown
    // here, so it does not flash up before the caller hides it.
    wchar_t hid[4] = {};
    if (!offscreen && GetEnvironmentVariableW(L"PM_VIDEO_START_HIDDEN", hid, 4) && hid[0] == L'1') {
        {
            std::lock_guard lk(impl_->m);
            impl_->visibleReq = 0;  // no WM_SHOWWINDOW will say so: the renderer must not animate it
        }
        impl_->wake();
        return true;
    }
    ShowWindow(impl_->hwnd, offscreen ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
    UpdateWindow(impl_->hwnd);
    return true;
}

int VideoWindow::runMessageLoop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void VideoWindow::setLogHandler(std::function<void(const char* line)> fn) {
    auto p = fn ? std::make_shared<std::function<void(const char*)>>(std::move(fn)) : nullptr;
    std::lock_guard lk(video::g_logM);
    video::g_logFn = std::move(p);
}

void VideoWindow::close() {
    if (HWND h = impl_->hwnd) PostMessageW(h, WM_CLOSE, 0, 0);
}

HWND__* VideoWindow::hwnd() const { return impl_->hwnd; }

VideoWindow::Stats VideoWindow::stats() const {
    Impl& I = *impl_;
    Stats s;
    std::lock_guard cl(I.statCopyM);
    // Under sm (the render thread takes it per picture): only the samples
    // added since the last call.
    auto take = [](const std::vector<float>& from, Impl::StatCopy& c) {
        if (from.size() < c.v.size()) c = {};  // (append-only: never)
        for (size_t i = c.v.size(); i < from.size(); ++i) c.sum += from[i];
        c.v.insert(c.v.end(), from.begin() + static_cast<std::ptrdiff_t>(c.v.size()), from.end());
    };
    {
        std::lock_guard sl(I.sm);
        s = I.st;
        take(I.tapMs, I.tapCopy);
        take(I.decMs, I.decCopy);
        take(I.e2eMs, I.e2eCopy);
        take(I.syncErrMs, I.syncCopy);
    }
    const double now = nowMs();
    if (now - I.statP95At >= 1000) {
        I.statP95At = now;
        double unused;
        percentile(I.decCopy.v, unused, I.decCopy.p95);
        percentile(I.e2eCopy.v, unused, I.e2eCopy.p95);
        percentile(I.tapCopy.v, unused, I.tapCopy.p95);
        std::vector<float> a(I.syncCopy.v);
        for (float& x : a) x = std::fabs(x);
        percentile(std::move(a), unused, I.syncCopy.p95);
    }
    auto avg = [](const Impl::StatCopy& c) { return c.v.empty() ? 0.0 : c.sum / static_cast<double>(c.v.size()); };
    s.decodeAvgMs = avg(I.decCopy), s.decodeP95Ms = I.decCopy.p95;
    s.e2eAvgMs = avg(I.e2eCopy), s.e2eP95Ms = I.e2eCopy.p95;
    s.tapAvgMs = avg(I.tapCopy), s.tapP95Ms = I.tapCopy.p95;
    s.syncPresented = static_cast<long long>(I.syncCopy.v.size());
    s.syncErrAvgMs = avg(I.syncCopy), s.syncErrAbsP95Ms = I.syncCopy.p95;
    s.presentsTotal = I.dg.presentOk.load() + I.dg.presentOccl.load();
    return s;
}

void VideoWindow::setSyncMode(bool enabled, int audioLatencyMs) {
    const bool was = impl_->syncEnabled.exchange(enabled);
    const int oldLat = impl_->syncLatencyMs.exchange(std::clamp(audioLatencyMs, 0, 5000));
    if (was != enabled) log("A/V sync %s (audio latency %d ms)", enabled ? "on" : "off", audioLatencyMs);
    if (was == enabled && oldLat == audioLatencyMs) return;
    {
        std::lock_guard lk(impl_->m);
        impl_->syncChanged = true;
    }
    impl_->wake();
}

void VideoWindow::onCodec(VideoCodec codec) {
    {
        std::lock_guard lk(impl_->m);
        // Repeated announcement of the same codec: keep the running decoder.
        if (impl_->codecAnnounced && codec == impl_->codec) return;
        impl_->codecAnnounced = true;
        impl_->queue.clear();
        impl_->codec = codec;
        impl_->codecChanged = true;
    }
    impl_->wake();
}

void VideoWindow::onFrame(const uint8_t* annexB, size_t len, uint64_t ntpLocalNs) {
    if (!annexB || !len) return;
    VideoCodec c;
    {
        std::lock_guard lk(impl_->m);
        c = impl_->codec;
    }
    AccessUnit au;
    au.tIn = nowMs();
    if (impl_->tapOn.load(std::memory_order_relaxed)) au.pts = impl_->tapPts(ntpLocalNs);
    if (ntpLocalNs && impl_->syncEnabled.load(std::memory_order_relaxed)) {
        // ntpLocalNs is on pm::AirPlayServer::localTimeNs()'s clock: accept
        // QPC ns or Unix-epoch wall-clock ns (whichever "now" it is close
        // to) and convert to our QPC milliseconds right away, so later
        // wall-clock adjustments cannot move pictures already queued.
        const int64_t t = static_cast<int64_t>(ntpLocalNs);
        const int64_t dq = t - qpcNs(), du = t - utcNs();
        const bool qpc = std::llabs(dq) <= std::llabs(du);
        const int64_t d = qpc ? dq : du;
        if (std::llabs(d) < 30'000'000'000LL) {  // within 30 s of now: plausible
            au.due = au.tIn + static_cast<double>(d) / 1e6;
            const int clk = qpc ? 1 : 2;
            if (impl_->ntpClock.exchange(clk) != clk)
                log("A/V sync: ntpLocalNs is on the %s clock (lead %+.1f ms)", qpc ? "QPC" : "UTC wall", d / 1e6);
        } else if (impl_->ntpClock.exchange(-1) != -1) {
            log("A/V sync: ntpLocalNs %llu is on neither clock (off by %.1f s); showing ASAP",
                static_cast<unsigned long long>(ntpLocalNs), d / 1e9);
        }
    }
    au.data.assign(annexB, annexB + len);
    annexb::forEachNal(annexB, len, [&](const annexb::Nal& n) {
        int t = annexb::nalType(c, n.data);
        if (annexb::isIrap(c, t)) au.irap = au.keep = true;
        if (annexb::isParamSet(c, t)) au.keep = true;
    });
    impl_->enqueue(std::move(au));
}

void VideoWindow::onSourceSize(int width, int height) { log("source size %dx%d", width, height); }

void VideoWindow::onPaused(bool paused) {
    log(paused ? "stream paused by phone" : "stream resumed");
    {
        std::lock_guard lk(impl_->m);
        impl_->pausePending = paused ? 1 : 0;
    }
    impl_->wake();
}

void VideoWindow::onReset() {
    {
        std::lock_guard lk(impl_->bgraM);  // an unshown BGRA picture belongs to the ended source
        impl_->bgraNew = false;
        impl_->bgraBuf = {};
    }
    {
        std::lock_guard lk(impl_->m);
        impl_->queue.clear();
        impl_->resetPending = true;
        impl_->frozenUi = false;  // the renderer drops the frozen picture and the overlay
        impl_->frozenReq = -1;
        impl_->boxesReq.reset();
        impl_->busyReq.reset();
    }
    impl_->wake();
    if (HWND h = impl_->hwnd.load(); h && kCancelSelectMsg) PostMessageW(h, kCancelSelectMsg, 0, 0);
}

// ---- Android sources ----

void VideoWindow::submitBgraFrame(const uint8_t* bgra, int width, int height, int stride, uint64_t ptsNs) {
    if (!bgra || width <= 0 || height <= 0 || width > 16384 || height > 16384 || stride < width * 4) return;
    const double tIn = nowMs();
    const uint64_t pts = impl_->tapOn.load(std::memory_order_relaxed) ? impl_->tapPts(ptsNs) : 0;
    const size_t row = static_cast<size_t>(width) * 4;
    bool replaced;
    {
        // Newest picture only: an unshown one is replaced (counted as dropped).
        std::lock_guard lk(impl_->bgraM);
        replaced = impl_->bgraNew.load();
        auto& buf = impl_->bgraBuf;
        buf.resize(row * height);
        if (static_cast<size_t>(stride) == row) {
            std::memcpy(buf.data(), bgra, row * height);
        } else {
            for (int y = 0; y < height; ++y)
                std::memcpy(buf.data() + row * y, bgra + static_cast<size_t>(stride) * y, row);
        }
        impl_->bgraW = width;
        impl_->bgraH = height;
        impl_->bgraTIn = tIn;
        impl_->bgraPts = pts;
        impl_->bgraNew = true;
    }
    impl_->wake();
    std::lock_guard sl(impl_->sm);
    impl_->st.framesIn++;
    if (replaced) impl_->st.framesDropped++;
}

}  // namespace pm
