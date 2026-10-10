// VideoWindow::Impl: decode path: AU queue, A/V sync hold, decoded / BGRA pictures, feeding the decoder.
// 拆檔 0.7.9：自 video_window.cpp（Impl 類別內）原樣搬出，改成類別外定義。
#include "video_window_impl.h"

namespace pm {

void VideoWindow::Impl::enqueue(AccessUnit&& au) {
    long long dropped = 0;
    const double now = nowMs();
    dg.auIn.fetch_add(1, std::memory_order_relaxed);
    dg.auBytes.fetch_add(static_cast<long long>(au.data.size()), std::memory_order_relaxed);
    dg.lastAuAt.store(now, std::memory_order_relaxed);
    if (au.irap) dg.lastIdrAt.store(now, std::memory_order_relaxed);
    bool faultDrop = false;
    {
        std::lock_guard lk(m);
        if (dropNextRefs > 0 && !au.keep) {  // fault 24: a lost reference AU
            --dropNextRefs;
            dropped = 1;
            faultDrop = true;
            refsDropped = true;
        } else if (queue.size() >= kMaxQueuedAUs) {
            // Prefer skipping straight to the newest key frame (clean).
            auto it = std::find_if(queue.rbegin(), queue.rend(), [](const AccessUnit& a) { return a.irap; });
            size_t bytes = 0;
            if (!au.irap && (it == queue.rend() || it.base() - 1 == queue.begin()) && wd)
                for (const auto& a : queue) bytes += a.data.size();
            if (au.irap) {
                dropped = static_cast<long long>(queue.size());
                queue.clear();
            } else if (it != queue.rend() && it.base() - 1 != queue.begin()) {
                auto keyPos = it.base() - 1;
                dropped = keyPos - queue.begin();
                queue.erase(queue.begin(), keyPos);
            } else if (!wd || queue.size() >= kHardMaxQueuedAUs || bytes >= kHardMaxQueuedBytes) {
                // Otherwise drop the oldest droppable frame (reference loss
                // until the next IDR; the HEVC decoder shows nothing until then).
                auto victim = std::find_if(queue.begin(), queue.end(), [](const AccessUnit& a) { return !a.keep; });
                if (victim != queue.end()) {
                    queue.erase(victim);
                    dropped = 1;
                    refsDropped = true;
                    if (!warnedRefDrop) {
                        warnedRefDrop = true;
                        queueFullSince = now;
                        log("render thread %zu AUs behind: dropping non-IDR access units (picture broken until "
                            "the next IDR)",
                            queue.size() + 1);
                    }
                }
            }
        } else if (warnedRefDrop && queue.size() < kMaxQueuedAUs / 2) {
            warnedRefDrop = false;  // caught up: the next backlog logs again
            log("render thread caught up (AU dropping lasted %.1f s)", (now - queueFullSince) / 1000);
        }
        if (!faultDrop) queue.push_back(std::move(au));
    }
    if (monIdle.load(std::memory_order_relaxed)) SetEvent(monEvent);  // stream (re)started: summaries
    if (dropped) dg.auDropped.fetch_add(dropped, std::memory_order_relaxed);
    wake();
    std::lock_guard sl(sm);
    st.framesIn++;
    st.framesDropped += dropped;
}

void VideoWindow::Impl::dropHeld() {
    hiddenSample.Reset();
    for (auto& h : held) ren.recycle(std::move(h.pic));
    held.clear();
}

void VideoWindow::Impl::showHeld(Held& h, double target) {
    ren.show(std::move(h.pic));
    pictureTIn = h.tIn;
    pictureTarget = target;
    newPicture = true;
}

void VideoWindow::Impl::syncTick() {
    if (held.empty()) return;
    const double now = nowMs(), lat = syncLatencyMs.load();
    size_t n = 0;
    while (n < held.size() && heldTarget(held[n], lat) <= now + kSyncSlackMs) ++n;
    if (!n) return;
    for (size_t i = 0; i + 1 < n; ++i) ren.recycle(std::move(held[i].pic));
    showHeld(held[n - 1], heldTarget(held[n - 1], lat));
    held.erase(held.begin(), held.begin() + n);
}

void VideoWindow::Impl::pictureDecoded(double t, bool live) {
    dg.decoded.fetch_add(1, std::memory_order_relaxed);
    if (pendingSince < 0) pendingSince = t;
    ++pendingPictures;
    if (!live) return;
    dg.lastPictureAt.store(t, std::memory_order_relaxed);
    if (reportDecodeBack) {
        reportDecodeBack = false;
        video::wdlog("pictures decoded again %.0f ms after the decoder restart", t - decRecoverAt);
    }
    starvedSince = -1;
    starvedFeeds = 0;
    if (!refsLost) decRecoveries = 0;  // a stray picture without references ends nothing: the IDR does
}

void VideoWindow::Impl::onDecoded(IMFSample* sample, const video::VideoFormat& fmt) {
    LONGLONG ts = 0;
    sample->GetSampleTime(&ts);
    int64_t s = ts / 166667;
    int slot = static_cast<int>(s % kRing);
    double due = ringDue[slot];
    const double tIn = ringIn[slot];
    const uint64_t pts = ringPts[slot];
    if (skipOutput) return;
    if (lostInDecode) return;  // the device is gone: recovery re-feeds the GOP
    bgraLive = false;  // a decoded stream took over
    srcW.store(fmt.visibleWidth(), std::memory_order_relaxed);
    srcH.store(fmt.visibleHeight(), std::memory_order_relaxed);
    // Frame tap: only pictures of newly received AUs (not re-fed ones).
    const bool tap = tIn >= 0 && tapOn.load(std::memory_order_relaxed);
    // A picture without a timestamp is shown ASAP, but never ahead of
    // pictures still waiting: it queues one frame behind them.
    if (syncActive && due == kNoTime && !held.empty()) {
        const size_t k = held.size();
        const double step = k >= 2 ? std::clamp(held[k - 1].due - held[k - 2].due, 0.0, 50.0) : 1000.0 / 60;
        due = held.back().due + step;
    }
    const bool faultLose = std::exchange(faultLoseInDecode, false);
    if (faultLose) {
        log("fault injection: device removal seen while uploading a decoded picture (inside the decoder callback)");
        faultRemoved = true;
    }
    if (syncActive && due != kNoTime) {
        Held h;
        if (faultLose || !ren.hold(sample, fmt, h.pic)) {  // waits for GPU completion
            deviceFailed();
            return;
        }
        h.due = due;
        h.tIn = tIn;
        h.decoded = nowMs();
        if (tap) tapPicture(&h.pic, pts);
        if (lostInDecode) {  // the tap found the device gone: nothing is held on a lost device
            ren.recycle(std::move(h.pic));
            return;
        }
        if (held.size() >= kSyncMaxHeld) {  // more than the cap: show the oldest now
            showHeld(held.front(), kNoTime);
            held.pop_front();
        }
        held.push_back(std::move(h));
    } else {
        dropHeld();  // older than this one
        if (!tap && !faultLose && (ren.minimized() || !ren.visible())) {  // nobody sees it: no GPU copy
            hiddenSample = sample;
            hiddenFmt = fmt;
            hiddenTIn = tIn;
        } else if (faultLose || !ren.upload(sample, fmt)) {  // waits for GPU completion
            deviceFailed();
            return;
        }
        pictureTIn = tIn;
        pictureTarget = kNoTime;
        newPicture = true;
        if (tap) tapPicture(nullptr, pts);
    }
    double t = nowMs();
    pictureDecoded(t, tIn >= 0);
    ComPtr<IMFMediaBuffer> buf;
    ComPtr<IMFDXGIBuffer> dx;
    bool hwOut = SUCCEEDED(sample->GetBufferByIndex(0, &buf)) && SUCCEEDED(buf.As(&dx));
    std::lock_guard sl(sm);
    st.framesDecoded++;
    st.hardwareDecode = hwOut;
    st.width = fmt.visibleWidth();
    st.height = fmt.visibleHeight();
    if (decMs.size() < kMaxStatSamples && s <= seq) decMs.push_back(static_cast<float>(t - ringSubmit[slot]));
}

void VideoWindow::Impl::takeBgra() {
    if (!bgraNew.load()) return;
    double tIn;
    uint64_t pts;
    {
        std::lock_guard lk(bgraM);
        if (!bgraNew.exchange(false)) return;
        std::swap(bgraBuf, bgraCur);  // keeps both allocations alive: no per-frame malloc
        bgraCurW = bgraW;
        bgraCurH = bgraH;
        tIn = bgraTIn;
        pts = bgraPts;
    }
    bgraLive = true;
    if (deviceLost) return;  // re-uploaded by recoverDevice()
    showBgra(tIn, pts);
}

void VideoWindow::Impl::showBgra(double tIn, uint64_t pts) {
    if (bgraCur.empty()) return;
    dropHeld();
    bgraHidden = (ren.minimized() || !ren.visible()) && !(tIn >= 0 && tapOn.load(std::memory_order_relaxed));
    if (!bgraHidden && !ren.uploadBgra(bgraCur.data(), bgraCurW, bgraCurH, bgraCurW * 4)) {
        checkDevice();
        return;
    }
    srcW.store(bgraCurW, std::memory_order_relaxed);
    srcH.store(bgraCurH, std::memory_order_relaxed);
    pictureTIn = tIn;
    pictureTarget = kNoTime;
    newPicture = true;
    if (tIn >= 0 && tapOn.load(std::memory_order_relaxed)) tapPicture(nullptr, pts);
    pictureDecoded(nowMs(), true);
    std::lock_guard sl(sm);
    st.framesDecoded++;
    st.hardwareDecode = false;
    st.width = bgraCurW;
    st.height = bgraCurH;
    if (tIn >= 0 && decMs.size() < kMaxStatSamples) decMs.push_back(static_cast<float>(nowMs() - tIn));
}

void VideoWindow::Impl::showHidden() {
    if (deviceLost) return;
    if (hiddenSample) {
        ComPtr<IMFSample> s = std::move(hiddenSample);
        if (ren.upload(s.Get(), hiddenFmt)) {
            pictureTIn = hiddenTIn;
            pictureTarget = kNoTime;
            newPicture = true;
        }
    }
    if (bgraHidden && !bgraCur.empty()) {
        bgraHidden = false;
        if (ren.uploadBgra(bgraCur.data(), bgraCurW, bgraCurH, bgraCurW * 4)) newPicture = true;
    }
}

void VideoWindow::Impl::dropBgra() {
    bgraCur = {};
    bgraHidden = false;
    bgraLive = false;
}

HRESULT VideoWindow::Impl::feed(const uint8_t* p, size_t n, double tIn, double due, uint64_t pts) {
    ++seq;
    int slot = static_cast<int>(seq % kRing);
    ringIn[slot] = tIn;
    ringDue[slot] = due;
    ringPts[slot] = pts;
    ringSubmit[slot] = nowMs();
    const long long out0 = dec.counters().outputs;
    inFeed = true;
    const HRESULT hr =
        dec.decode(p, n, seq * 166667, [this](IMFSample* s, const video::VideoFormat& f) { onDecoded(s, f); });
    inFeed = false;
    dg.mftOut.fetch_add(dec.counters().outputs - out0, std::memory_order_relaxed);
    if (FAILED(hr)) dg.lastDecodeHr.store(hr, std::memory_order_relaxed);
    // A device failure inside the callback: now the decoder may close.
    if (std::exchange(lostInDecode, false)) checkDevice();
    return hr;
}

void VideoWindow::Impl::remember(AccessUnit& au) {
    if (au.irap) {
        gop.clear();
        gopBytes = 0;
        gopTruncated = false;
    }
    if (!gopTruncated && (gop.size() >= gopMaxAUs() || gopBytes + au.data.size() > gopMaxBytes())) {
        gopTruncated = true;
        gop.resize(1);  // keep just the key frame
        gopBytes = gop[0].data.size();
    }
    if (!gopTruncated) {
        gopBytes += au.data.size();
        gop.push_back(std::move(au));
    }
    dg.gopAUs.store(static_cast<int>(gop.size()), std::memory_order_relaxed);
    dg.gopTrunc.store(gopTruncated, std::memory_order_relaxed);
}

void VideoWindow::Impl::decodeOne(AccessUnit& au) {
    if (decoderMissing) return;
    if (waitKey) {
        if (!au.irap) {  // cannot start mid-GOP
            dg.auSkipped.fetch_add(1, std::memory_order_relaxed);
            if (keyWaitSince < 0) keyWaitSince = nowMs();
            return;
        }
        waitKey = false;
        keyWaitSince = -1;
    }
    if (au.irap) {
        refsLost = false;  // a clean start: nothing before it is needed
        if (adapterSwitchPending && !deviceLost) {
            adapterSwitchPending = false;
            log("key frame: switching GPU now (%s)", adapterSwitchWhy.c_str());
            loseDevice(adapterSwitchWhy.c_str(), S_OK);
        }
        // After a watchdog switch to software decoding: DXVA again at a
        // clean point (software HEVC cannot keep up with 1440p60).
        if (swSince >= 0 && nowMs() - swSince > kHwRetryMs && !deviceLost) {
            swSince = -1;
            hwBroken = false;
            dec.close();
            video::wdlog("key frame: trying hardware decoding again");
        }
    }
    if (deviceLost) {  // keep the GOP for the re-feed after recovery
        remember(au);
        return;
    }
    if (!dec.isOpen() && !openDecoder()) return;

    setStage(StDecode);
    HRESULT hr = feed(au.data.data(), au.data.size(), au.tIn, au.due, au.pts);
    if (SUCCEEDED(hr)) {
        errorStreak = 0;
        dg.auFed.fetch_add(1, std::memory_order_relaxed);
        if (starvedSince < 0) starvedSince = nowMs();  // reset by the next decoded picture
        ++starvedFeeds;
    } else if (checkDevice()) {
        // decoder failed because the device went away: recovery re-feeds
    } else {
        log("decode error hr=0x%08lx (%s)", hr, dec.hardware() ? "hw" : "sw");
        if (dec.hardware()) {
            log("falling back to software decoding");
            hwBroken = true;
            dec.close();
            if (openDecoder()) {
                const size_t n = au.irap ? 0 : gopTruncated ? std::min<size_t>(gop.size(), 1) : gop.size();
                for (size_t i = 0; i < n && !deviceLost; ++i) feed(gop[i].data.data(), gop[i].data.size(), -1, kNoTime);
                if (!deviceLost) feed(au.data.data(), au.data.size(), au.tIn, au.due, au.pts);
            }
        } else if (++errorStreak > 30) {
            log("too many decode errors, restarting decoder");
            dec.close();
            errorStreak = 0;
            if (wd && !gop.empty() && openDecoder()) {
                remember(au);
                refeedGop();  // iOS: the next IDR may be minutes away
                return;
            }
            waitKey = true;
        }
    }
    remember(au);
}

}  // namespace pm
