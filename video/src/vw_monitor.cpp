// VideoWindow::Impl: 5 s statistics monitor thread and stopping the worker.
// 拆檔 0.7.9：自 video_window.cpp（Impl 類別內）原樣搬出，改成類別外定義。
#include "video_window_impl.h"

namespace pm {

VideoWindow::Impl::Counts VideoWindow::Impl::counts() const {
    auto l = [](const std::atomic<long long>& a) { return a.load(std::memory_order_relaxed); };
    return {l(dg.auIn),   l(dg.auDropped),  l(dg.auSkipped), l(dg.auFed),      l(dg.mftOut),     l(dg.decoded),
            l(dg.presented), l(dg.presentOk), l(dg.presentOccl), l(dg.presentErr), l(dg.waitTimeouts),
            l(dg.auBytes)};
}

const char* VideoWindow::Impl::stageName(int s) {
    static const char* const k[] = {"wait", "decode", "frame wait", "Present", "recovery", "frame tap", "other"};
    return s >= 0 && s < 7 ? k[s] : "?";
}

void VideoWindow::Impl::monitorMain() {
    SetThreadDescription(GetCurrentThread(), L"pm_video_monitor");
    Counts prev = counts();
    double last = nowMs(), stuckLoggedAt = -1e300;
    bool summarised = false;
    while (!monStop.load()) {
        double now = nowMs();
        const double au = dg.lastAuAt.load();
        if (au < 0 || now - au > 5000) {
            if (summarised) {  // the stream went quiet: one last line for the partial interval
                summary(prev, last, now);
                summarised = false;
            }
            monIdle = true;
            const double au2 = dg.lastAuAt.load();
            if (au2 < 0 || nowMs() - au2 > 5000) WaitForSingleObject(monEvent, INFINITE);
            monIdle = false;
            prev = counts();
            last = nowMs();
            continue;
        }
        WaitForSingleObject(monEvent, 1000);
        now = nowMs();
        const int stg = dg.stage.load();
        const double stuck = now - dg.stageAt.load();
        if (stg != StWait && stuck > 3000 && now - stuckLoggedAt > 5000) {
            stuckLoggedAt = now;
            size_t q;
            {
                std::lock_guard lk(m);
                q = queue.size();
            }
            video::wdlog("render thread busy in %s for %.1f s (%zu AUs queued)", stageName(stg), stuck / 1000, q);
        }
        if (now - last >= 5000) {
            summary(prev, last, now);
            summarised = true;
        }
    }
}

void VideoWindow::Impl::summary(Counts& prev, double& last, double now) {
    const Counts c = counts();
    size_t q;
    {
        std::lock_guard lk(m);
        q = queue.size();
    }
    const double idr = dg.lastIdrAt.load();
    // avg AU size: an unchanging phone screen arrives as ~1-2 KB P-frames
    // (the picture really is still on the phone, not stuck here).
    // FROZEN: the window shows the frozen copy (凍結 / translation).
    const long long nAu = c.in - prev.in;
    const double frz = dg.frozenSince.load(), pic = dg.lastPictureAt.load();
    char tail[96] = "";
    if (frz >= 0)
        std::snprintf(tail, sizeof tail, " | FROZEN for %.1f s (newest live picture %.1f s old)", (now - frz) / 1000,
                      pic >= 0 ? (now - pic) / 1000 : -1.0);
    log("%.0fs: AU in %lld (avg %.1f KB) drop %lld skip %lld | fed %lld > MFT %lld > pictures %lld (%s) | presented "
        "%lld (ok %lld, occluded %lld, failed %lld, last 0x%08lx; frame-wait timeouts %lld) | queue %zu | last IDR "
        "%.1f s ago, GOP %d%s%s",
        (now - last) / 1000, nAu, nAu > 0 ? (c.bytes - prev.bytes) / 1024.0 / nAu : 0.0, c.drop - prev.drop,
        c.skip - prev.skip, c.fed - prev.fed, c.mft - prev.mft, c.dec - prev.dec, dg.hw.load() ? "hw" : "sw",
        c.pres - prev.pres, c.ok - prev.ok, c.occl - prev.occl, c.err - prev.err,
        static_cast<unsigned long>(dg.lastPresentHr.load()), c.wto - prev.wto, q,
        idr >= 0 ? (now - idr) / 1000 : -1.0, dg.gopAUs.load(), dg.gopTrunc.load() ? " (truncated)" : "", tail);
    prev = c;
    last = now;
}

void VideoWindow::Impl::startMonitor() {
    if (!monitor.joinable()) monitor = std::thread([this] { monitorMain(); });
}

void VideoWindow::Impl::stopMonitor() {
    if (!monitor.joinable()) return;
    monStop = true;
    SetEvent(monEvent);
    monitor.join();
}

void VideoWindow::Impl::stopWorker() {
    stopMonitor();
    if (!worker.joinable()) return;
    {
        std::lock_guard lk(m);
        stop = true;
    }
    wake();
    // Keep pumping messages: DXGI may need the window thread while the
    // worker tears the swap chain down.
    HANDLE th = worker.native_handle();
    while (MsgWaitForMultipleObjects(1, &th, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(msg.wParam));
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    worker.join();
}

}  // namespace pm
