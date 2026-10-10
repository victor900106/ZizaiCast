// VideoWindow::Impl: watchdog (0.6.2): fault injection, stalled decoder / present recovery.
// 拆檔 0.7.9：自 video_window.cpp（Impl 類別內）原樣搬出，改成類別外定義。
#include "video_window_impl.h"

namespace pm {

void VideoWindow::Impl::injectFault(int code, LPARAM arg) {
    switch (code) {
    case 20:  // the decoder stops returning pictures (+ arg more HW instances)
        faultWedgeHw = static_cast<int>(arg);
        if (dec.isOpen()) dec.setFaultDropOutput(true);
        log("fault injection: decoder output swallowed (this instance%s)",
            arg ? " and the next hardware instance(s)" : "");
        break;
    case 21:  // Present reports DXGI_STATUS_OCCLUDED for arg ms
        ren.faultOcclude(video::clockMs() + static_cast<double>(arg));
        log("fault injection: Present reports DXGI_STATUS_OCCLUDED for %ld ms", static_cast<long>(arg));
        break;
    case 22:  // the swap chain stops presenting (until re-created)
        ren.faultSwallowPresents();
        log("fault injection: Present calls swallowed (stuck swap chain)");
        break;
    case 23:  // the render thread blocks for arg ms (a stalled Present / GPU)
        log("fault injection: render thread blocked for %ld ms", static_cast<long>(arg));
        setStage(StPresent);
        Sleep(static_cast<DWORD>(arg));
        setStage(StOther);
        break;
    case 24: {  // the next arg non-IDR AUs are lost before decoding
        std::lock_guard lk(m);
        dropNextRefs = static_cast<size_t>(arg);
        log("fault injection: dropping the next %ld non-IDR AU(s)", static_cast<long>(arg));
        break;
    }
    case 26:  // display change after which the power-saving GPU is preferred
        forceAdapter = 1;
        checkAdapter("display change (test: power-saving GPU preferred)");
        break;
    default:
        break;
    }
}

void VideoWindow::Impl::watchdogDecode() {
    if (!wd || deviceLost || !dec.isOpen()) return;
    const double now = nowMs();
    if (starvedSince < 0 || starvedFeeds < kWdMinFeeds || now - starvedSince < kWdStallMs) return;
    if (decRecoveries > 0 && now - decRecoverAt < (decRecoveries >= 2 ? kWdBackoffMs : kWdStallMs)) return;
    const auto c = dec.counters();
    const double idrAgo = dg.lastIdrAt.load() >= 0 ? (now - dg.lastIdrAt.load()) / 1000 : -1;
    if (refsLost && decRecoveries > 0) {
        if (decRecoveries == 1) {
            video::wdlog("still no picture after %d AUs: references lost (last IDR %.0f s ago); keeping the last "
                         "picture until the next IDR",
                         starvedFeeds, idrAgo);
            ++decRecoveries;
        }
        decRecoverAt = now;
        return;
    }
    while (!decFailTimes.empty() && now - decFailTimes.front() > 60000) decFailTimes.pop_front();
    const bool toSw = dec.hardware() && !decFailTimes.empty() && !refsLost;
    video::wdlog("no picture decoded for %.1f s although %d AUs were fed (%s decoder '%s': in %lld, out %lld, "
                 "errors %lld, last hr=0x%08lx; last IDR %.1f s ago, GOP cache %zu AUs%s%s): %s",
                 (now - starvedSince) / 1000, starvedFeeds, dec.hardware() ? "hardware" : "software",
                 dec.name().c_str(), c.inputs, c.outputs, c.errors, static_cast<unsigned long>(c.lastError), idrAgo,
                 gop.size(), gopTruncated ? " truncated" : "", refsLost ? ", references lost" : "",
                 gop.empty() ? "restarting the decoder; no key frame kept: waiting for the next IDR"
                 : toSw      ? "2nd failure within a minute: software decoder, re-feeding from the last IDR"
                             : "restarting the decoder, re-feeding from the last IDR");
    decFailTimes.push_back(now);
    ++decRecoveries;
    decRecoverAt = now;
    reportDecodeBack = true;
    {
        std::lock_guard sl(sm);
        st.watchdogRecoveries++;
    }
    if (toSw) {
        hwBroken = true;
        swSince = now;
    }
    setStage(StRecover);
    dec.close();
    starvedSince = -1;
    starvedFeeds = 0;
    if (!openDecoder()) return;
    if (gop.empty()) {
        waitKey = true;
        return;
    }
    const size_t n = refeedGop();
    video::wdlog("decoder re-opened (%s) and %zu AU(s) re-fed in %.0f ms", dec.hardware() ? "hardware" : "software",
                 n, nowMs() - now);
    setStage(StOther);
}

void VideoWindow::Impl::watchdogPresent() {
    if (!wd || deviceLost) return;
    const double now = nowMs();
    if (ren.minimized() || !ren.visible() || ren.idle() || ren.paused()) {  // nothing to show: not a stall
        pendingSince = waitStuckSince = -1;
        pendingPictures = 0;
        return;
    }
    const bool stuckPresent = pendingSince >= 0 && pendingPictures >= 2 && now - pendingSince > kWdStallMs;
    // Frames presented but the swap chain never signals that it can take
    // the next one: only a new swap chain, at most every 30 s (a locked
    // session might look like this too; the 100 ms wait bounds the cost).
    const bool stuckWait = !stuckPresent && waitStuckSince >= 0 && now - waitStuckSince > kWdStallMs &&
                           streamLive() && now - swapRecoverAt > 30000;
    if (!stuckPresent && !stuckWait) return;
    const bool again = stuckPresent && now - swapRecoverAt < 10000;
    video::wdlog("%s for %.1f s (%d picture(s) waiting; last Present hr=0x%08lx%s, %lld frame-wait timeouts): %s",
                 stuckPresent ? "pictures decoded but none presented" : "swap chain releases no frames",
                 (now - (stuckPresent ? pendingSince : waitStuckSince)) / 1000, pendingPictures,
                 static_cast<unsigned long>(ren.lastPresentHr()), ren.lastPresented() ? "" : ", not presented",
                 dg.waitTimeouts.load(), again ? "re-creating the device" : "re-creating the swap chain");
    swapRecoverAt = now;
    pendingSince = waitStuckSince = -1;
    pendingPictures = 0;
    {
        std::lock_guard sl(sm);
        st.watchdogRecoveries++;
    }
    setStage(StRecover);
    if (again || !ren.recreateSwapChain()) {
        loseDevice("watchdog: presenting stuck", S_OK);
        return;
    }
    reportPresentBack = true;
    newPicture = needPresent = true;  // the newest picture, now
    setStage(StOther);
}

}  // namespace pm
