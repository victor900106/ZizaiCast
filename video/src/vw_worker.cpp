// VideoWindow::Impl: render / decode worker thread: UI -> worker state hand-over (take), batches, loop, present stats.
// 拆檔 0.7.9：自 video_window.cpp（Impl 類別內）原樣搬出，改成類別外定義。
#include "video_window_impl.h"

namespace pm {

void VideoWindow::Impl::tapPicture(const video::Renderer::Picture* pic, uint64_t pts) {
    // Keep a ring slot free (several pictures in one decode batch); the
    // usual delivery happens after Present (off the latency path).
    if (ren.tapPending() >= video::Renderer::kTapRing - 1) deliverTaps(false);
    if (!ren.tapSubmit(pic, pts)) deviceFailed();
}

void VideoWindow::Impl::deliverTaps(bool all) {
    std::lock_guard lk(tapCallM);
    std::shared_ptr<VideoWindow::FrameTap> fn = tapFn;  // survives setFrameTap() from inside the tap
    if (!fn || !*fn) {
        ren.tapReset();
        return;
    }
    ren.tapDeliver(
        [&](const uint8_t* d, int w, int h, int stride, uint64_t pts) {
            const double cost = ren.tapCostMs(true);
            {
                std::lock_guard sl(sm);
                st.framesTapped++;
                if (tapMs.size() < kMaxStatSamples) tapMs.push_back(static_cast<float>(cost));
            }
            (*fn)(d, w, h, stride, pts);
        },
        all);
}

void VideoWindow::Impl::workerMain(std::promise<bool>& ready) {
    SetThreadDescription(GetCurrentThread(), L"pm_video");
    workerTid = GetCurrentThreadId();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    bool ok = initGraphics();
    if (ok) {
        const auto& art = ren.mascotArt();
        std::lock_guard lk(m);
        mascotMask = art.mask();
        maskW = art.maskW();
        maskH = art.maskH();
    }
    ready.set_value(ok);
    if (ok) loop();
    {
        std::lock_guard lk(m);
        stop = true;  // also when the loop ended on device loss
        for (auto& r : snapReqs) SetEvent(r->done);
        snapReqs.clear();
    }
    dec.close();
    ren.shutdown();
    dxgiMgr.Reset();
    dev.Reset();
    MFShutdown();
    CoUninitialize();
}

bool VideoWindow::Impl::take(std::deque<AccessUnit>& out, double waitMs) {
    if (waitMs != 0) {
        bool pending;
        {
            std::lock_guard lk(m);
            pending = pendingLocked();
        }
        if (!pending) {
            setStage(StWait);
            if (waitMs < 0) {
                WaitForSingleObject(wakeEvent, INFINITE);
            } else {
                LARGE_INTEGER due;
                due.QuadPart = -static_cast<LONGLONG>(waitMs * 10000);  // relative, 100 ns
                SetWaitableTimer(frameTimer, &due, 0, nullptr, nullptr, FALSE);
                HANDLE hs[2] = {wakeEvent, frameTimer};
                WaitForMultipleObjects(2, hs, FALSE, INFINITE);
                CancelWaitableTimer(frameTimer);
            }
        }
    }
    bool reset = false, codecCh = false, resize = false, paint = false, poke = false, optsCh = false,
         hoverCh = false, syncCh = false, adapterCh = false;
    int pause = -1, hov = -1, vis = -1, test = -1, theme = -1, dim = -1, frame = -1, rec = -1, mHover = -1;
    int motion = -1, pressR = -2, focusR = -2;
    bool flashR = false;
    LPARAM testLp = 0;
    bool xform = false, mClick = false, tapCh = false;
    UINT w = 0, h = 0, dpi = 0;
    VideoCodec c;
    std::optional<std::wstring> connecting, pin, toast;
    double toastHold = 0;
    std::optional<std::vector<video::Renderer::ToolItem>> tools;
    bool toolAct = false, toolLeave = false, toolInside = false;
    std::optional<std::pair<double, std::wstring>> toolReveal;
    int toolHover = -2;
    std::optional<std::vector<video::Renderer::Action>> actions;
    std::optional<std::vector<std::wstring>> hints;
    std::optional<std::vector<video::Renderer::Card>> cards;
    int linkHover = -2;
    std::vector<video::Renderer::Option> opts;
    std::vector<std::shared_ptr<SnapshotRequest>> snaps;
    std::optional<video::Renderer::View> view;
    int frozenR = -1, originalR = -1;
    std::optional<std::vector<video::Renderer::TextBox>> boxes;
    std::optional<std::vector<std::wstring>> onlineMarks;
    std::optional<std::wstring> busy;
    int ovHot = -2, ovSel = -2, ovMode = -1, ovDark = -1;
    bool ovReveal = false;
    float ovScroll = 0;
    bool selCh = false, selOn = false;
    float selR[4] = {};
    {
        std::lock_guard lk(m);
        if (stop) return false;
        if (std::exchange(viewChanged, false)) view = viewUi;
        frozenR = std::exchange(frozenReq, -1);
        originalR = std::exchange(originalReq, -1);
        boxes = std::exchange(boxesReq, std::nullopt);
        onlineMarks = std::exchange(onlineReq, std::nullopt);
        busy = std::exchange(busyReq, std::nullopt);
        ovHot = std::exchange(ovHotReq, -2);
        ovMode = std::exchange(ovModeReq, -1);
        ovDark = std::exchange(ovDarkReq, -1);
        ovSel = std::exchange(ovSelReq, -2);
        ovReveal = std::exchange(ovRevealReq, false);
        ovScroll = std::exchange(ovScrollReq, 0.f);
        if ((selCh = std::exchange(selChanged, false))) {
            selOn = selActiveReq;
            std::copy(std::begin(selReq), std::end(selReq), selR);
        }
        out.swap(queue);
        reset = std::exchange(resetPending, false);
        pause = std::exchange(pausePending, -1);
        codecCh = std::exchange(codecChanged, false);
        resize = std::exchange(resizePending, false);
        paint = std::exchange(dirty, false);
        poke = std::exchange(pokeReq, false);
        dpi = std::exchange(dpiReq, 0u);
        vis = std::exchange(visibleReq, -1);
        connecting = std::exchange(connectingReq, std::nullopt);
        pin = std::exchange(pinReq, std::nullopt);
        toast = std::exchange(toastReq, std::nullopt);
        toastHold = toastHoldReq;
        tools = std::exchange(toolbarReq, std::nullopt);
        toolAct = std::exchange(toolActivityReq, false);
        toolReveal = std::exchange(toolRevealReq, std::nullopt);
        toolLeave = std::exchange(toolLeaveReq, false);
        toolHover = std::exchange(toolHoverReq, -2);
        toolInside = toolInsideReq;
        snaps = std::exchange(snapReqs, {});
        syncCh = std::exchange(syncChanged, false);
        adapterCh = std::exchange(adapterCheckReq, false);
        test = std::exchange(testReq, -1);
        testLp = testArg;
        theme = std::exchange(themeReq, -1);
        motion = std::exchange(motionReq, -1);
        pressR = std::exchange(pressReq, -2);
        focusR = std::exchange(focusReq, -2);
        flashR = std::exchange(flashReq, false);
        dim = std::exchange(dimReq, -1);
        frame = std::exchange(frameReq, -1);
        rec = std::exchange(recReq, -1);
        mHover = std::exchange(mascotHoverReq, -1);
        xform = std::exchange(xformChanged, false);
        mClick = std::exchange(mascotClickReq, false);
        tapCh = std::exchange(tapChanged, false);
        hints = std::exchange(hintsReq, std::nullopt);
        cards = std::exchange(cardsReq, std::nullopt);
        actions = std::exchange(actionsReq, std::nullopt);
        linkHover = std::exchange(linkHoverReq, -2);
        if ((optsCh = std::exchange(optionsChanged, false)))
            for (auto& o : options) opts.push_back({o.label, o.checked});
        if ((hoverCh = std::exchange(hoverChanged, false))) hov = hover;
        w = pendW;
        h = pendH;
        c = codec;
    }
    if (test >= 0) {
        static const char* const kTests[] = {"simulated device removal (test)", "switch to power-saving GPU (test)",
                                             "switch to WARP (test)", "back to automatic GPU choice (test)"};
        if (test >= 1 && test <= 3) forceAdapter = test == 3 ? 0 : test;
        if (test == 0 && testLp == 1) {  // removal seen inside the decoder callback (next picture)
            faultLoseInDecode = true;
            log("fault injection: device removal at the next decoded picture");
        } else if (test <= 3) {
            loseDevice(kTests[test], DXGI_ERROR_DEVICE_REMOVED);
        }
        else injectFault(test, testLp);
    }
    if (adapterCh) checkAdapter("display / monitor change");
    if (dpi) {
        ren.setDpi(dpi);
        paint = true;
    }
    if (resize) {
        if (!ren.resize(w, h)) loseDevice("ResizeBuffers", dev ? dev->GetDeviceRemovedReason() : E_FAIL);
        ren.poke();
        paint = true;
    }
    if (vis >= 0) {
        ren.setVisible(vis == 1);
        if (vis == 1) ren.poke();
        paint = true;
    }
    if ((hiddenSample || bgraHidden) && !ren.minimized() && ren.visible()) showHidden();
    if (tapCh && !tapOn.load()) ren.tapReset();
    if ((codecCh || reset) && tapOn.load()) deliverTaps(true);  // the last pictures of the stream
    if (theme >= 0) {
        ren.setTheme(theme);
        paint = true;
    }
    if (motion >= 0) {
        ren.setReducedMotion(motion == 1);
        paint = true;
    }
    if (pressR != -2) {
        ren.setPressed(pressR < 0 ? -1 : pressR / 1000, pressR < 0 ? -1 : pressR % 1000);
        paint = true;
    }
    if (flashR) {
        ren.flash();
        paint = true;
    }
    if (focusR != -2) {
        ren.setFocus(focusR < 0 ? -1 : focusR / 1000, focusR < 0 ? -1 : focusR % 1000);
        paint = true;
    }
    if (dim >= 0) {
        ren.setDimmed(dim == 1);
        paint = true;
    }
    if (xform) {
        ren.setTransform(rotation.load(), mirrored.load());
        paint = true;
    }
    if (frame >= 0) {
        ren.setDeviceFrame(frame == 1);
        paint = true;
    }
    if (rec >= 0) {
        ren.setRecording(rec == 1);
        paint = true;
    }
    if (mHover >= 0) {
        ren.setMascotHover(mHover == 1);
        paint = true;
    }
    if (mClick) {
        ren.mascotClicked();
        paint = true;
    }
    if (codecCh || reset) {
        if (codecCh || decCodec != c) {
            dec.close();
            decCodec = c;
            // eagerly: MFT creation costs ~100 ms, keep it off the first frame
            if (!deviceLost) openDecoder();
        } else {
            dec.flush();
        }
        waitKey = true;
        gop.clear();
        gopBytes = 0;
        gopTruncated = false;
        dropHeld();
        ren.trimPool();
        // New stream: watchdog episodes end; a deferred GPU switch can happen now.
        starvedSince = pendingSince = waitStuckSince = keyWaitSince = -1;
        starvedFeeds = pendingPictures = decRecoveries = 0;
        refsLost = false;
        refsDropped = false;
        dg.gopAUs = 0;
        dg.gopTrunc = false;
        if (adapterSwitchPending) {
            adapterSwitchPending = false;
            loseDevice(adapterSwitchWhy.c_str(), S_OK);
        }
    }
    if (syncCh) {
        const bool on = syncEnabled.load();
        if (on != syncActive) {
            syncActive = on;
            if (!on && !held.empty()) {  // show the newest held picture right away
                showHeld(held.back(), kNoTime);
                held.pop_back();
                dropHeld();
                ren.trimPool();
            }
        }
        // A new latency takes effect through syncTick() / syncWaitMs().
    }
    if (pause >= 0) {
        ren.setPaused(pause == 1);
        paint = true;
    }
    if (reset) {
        if (ren.frozen()) applyFrozen(false, "stream reset");
        ren.reset();
        pictureTIn = -1;
        newPicture = false;
        paint = true;
        dropBgra();
    }
    takeBgra();  // after the reset: "reset, then a new source" shows it
    if (hints) {
        ren.setHints(std::move(*hints));
        paint = true;
    }
    if (cards) {
        ren.setCards(std::move(*cards));
        paint = true;
    }
    if (actions) {
        ren.setActions(std::move(*actions));
        paint = true;
    }
    if (linkHover != -2) {
        ren.setActionHover(linkHover);
        paint = true;
    }
    if (connecting) ren.setConnecting(*connecting);  // after reset: "reset, then connect" keeps connecting
    if (optsCh) ren.setOptions(std::move(opts));
    if (hoverCh) ren.setHover(hov);
    if (pin) ren.setPin(*pin);
    if (toast) ren.showToast(*toast, toastHold);
    if (tools) {
        ren.setToolbar(std::move(*tools));
        paint = true;
    }
    if (toolHover != -2) {
        ren.setToolbarHover(toolHover, toolInside);
        paint = true;
    }
    if (toolLeave) {
        ren.toolbarLeave();
        paint = true;
    }
    if (toolAct) {
        if (ren.toolbarActivity()) paint = true;
    }
    if (toolReveal) {
        ren.revealToolbar(toolReveal->first, std::move(toolReveal->second));
        paint = true;
    }
    if (poke) {
        const bool wasIdle = ren.nextFrameInMs() < 0;
        ren.poke();
        if (wasIdle && ren.nextFrameInMs() >= 0) paint = true;  // (reduced motion: stays a still picture)
    }
    if (view) {
        ren.setView(*view);
        paint = true;
    }
    if (frozenR >= 0) {
        applyFrozen(frozenR == 1, "request");
        paint = true;
    }
    if (const int lv = liveOvReq.exchange(-1); lv >= 0) {  // 即時翻譯 (live_overlay.cpp)
        ren.setOverlayLive(lv == 1);
        paint = true;
    }
    if (boxes) {
        ren.setTextOverlay(std::move(*boxes));
        ren.liveBoxesSet();
        paint = true;
    }
    if (onlineMarks) {
        ren.setTextOverlayOnline(*onlineMarks);
        paint = true;
    }
    if (originalR >= 0) {
        ren.setTextOverlayOriginal(originalR == 1);
        paint = true;
    }
    if (ovMode >= 0 || ovDark >= 0) {
        if (ovMode >= 0) ren.setOverlayMode(ovMode);
        if (ovDark >= 0) ren.setOverlayDark(ovDark == 1);
        paint = true;
    }
    if (ovHot != -2) {
        ren.setOverlayHot(ovHot);
        paint = true;
    }
    if (ovSel != -2) {
        ren.setOverlaySelected(ovSel, ovReveal);
        paint = true;
    }
    if (ovScroll != 0) {
        ren.scrollOverlayList(ovScroll);
        paint = true;
    }
    if (busy) {
        ren.setBusy(*busy);
        paint = true;
    }
    if (selCh) {
        ren.setSelection(selOn, selR[0], selR[1], selR[2], selR[3]);
        paint = true;
    }
    if (!snaps.empty()) showHidden();  // the newest picture, even while minimized
    for (auto& snap : snaps) {  // every waiting caller gets its picture (none waits out the 3 s)
        snap->ok = snap->ui     ? ren.renderCapture(snap->pixels, snap->w, snap->h)
                   : snap->grab ? ren.grab(snap->pixels, snap->w, snap->h, false)
                                : ren.snapshot(snap->pixels, snap->w, snap->h, snap->framed);
        SetEvent(snap->done);
    }
    if (paint) needPresent = true;
    return true;
}

void VideoWindow::Impl::publishOptionRects() {
    const auto& r = ren.optionRects();
    std::lock_guard lk(m);
    mascotRect = ren.mascotRect();
    linkRects = ren.actionRects();
    toolRects = ren.toolbarRects();
    toolPill = ren.toolbarPill();
    picRect = ren.pictureRect();
    picRot = ren.rotation();
    picMirror = ren.mirrored();
    pubView = ren.view();
    ovHits = ren.overlayHits();
    if (r.size() == optionRects.size() &&
        std::equal(r.begin(), r.end(), optionRects.begin(), [](const RECT& a, const RECT& b) {
            return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
        }))
        return;
    optionRects = r;
}

void VideoWindow::Impl::decodeBatch(std::deque<AccessUnit>& batch) {
    if (refsDropped.exchange(false)) refsLost = true;
    for (auto& au : batch) decodeOne(au);
    batch.clear();
    if (!deviceLost) checkDevice();
    syncTick();
    setStage(StOther);
}

void VideoWindow::Impl::loop() {
    std::deque<AccessUnit> batch;
    while (true) {
        if (deviceLost && !recoverDevice()) {
            // Window and UI stay responsive; AUs keep the GOP for the re-feed.
            if (!take(batch, std::max(1.0, nextRecoverAt - nowMs()))) return;
            decodeBatch(batch);
            continue;
        }
        // Event-driven: sleep until something happens, unless an animation
        // is running or a held picture becomes due.  While DXGI reports
        // occlusion a new picture is presented every kOccludedPresentMs
        // (decoding goes on at full rate; the first S_OK restores it).
        double waitMs = needPresent ? 0 : newPicture ? occludedWaitMs() : -1;
        if (waitMs != 0) waitMs = earliest(waitMs, earliest(ren.nextFrameInMs(), syncWaitMs()));
        waitMs = earliest(waitMs, ren.tapFlushInMs());
        if (!take(batch, waitMs)) return;
        decodeBatch(batch);
        if (deviceLost) continue;
        watchdogDecode();
        watchdogPresent();
        if (deviceLost) continue;
        if (const double f = ren.tapFlushInMs(); f >= 0 && f < 1) {  // stream went quiet
            setStage(StTap);
            deliverTaps(true);
        }
        const double next = ren.nextFrameInMs();
        const bool animDue = next >= 0 && next < 1.0;
        const bool pictureDue = newPicture && occludedWaitMs() == 0;
        if (!pictureDue && !needPresent && !animDue) continue;

        // Wait until the swap chain can take a frame, then pick up anything
        // that arrived meanwhile so we present the freshest picture.  Not
        // while occluded: the waitable may not be signalled then.
        if (HANDLE wh = ren.frameWaitable(); wh && !(wd && ren.occluded())) {
            setStage(StFrameWait);
            if (WaitForSingleObjectEx(wh, 100, FALSE) == WAIT_TIMEOUT) {
                dg.waitTimeouts.fetch_add(1, std::memory_order_relaxed);
                if (waitStuckSince < 0) waitStuckSince = nowMs();
            } else {
                waitStuckSince = -1;
            }
        }
        if (!take(batch, 0)) return;
        decodeBatch(batch);
        if (deviceLost) continue;

        double tIn = newPicture ? pictureTIn : -1;
        const double target = newPicture ? pictureTarget : kNoTime;
        bool presentedPicture = newPicture && !ren.idle();
        newPicture = needPresent = false;
        setStage(StPresent);
        if (!ren.render()) {
            loseDevice("Present", dev ? dev->GetDeviceRemovedReason() : E_FAIL);
            continue;
        }
        setStage(StOther);
        notePresent(presentedPicture);
        publishOptionRects();
        if (ren.tapPending()) {  // previous pictures: their GPU work is long done
            setStage(StTap);
            deliverTaps(false);
            setStage(StOther);
        }
        if (presentedPicture && ren.lastPresented()) {
            if (reportPictureBack) {
                reportPictureBack = false;
                log("picture back on screen %.0f ms after the device loss", nowMs() - lostAt);
            }
            const double t = nowMs();
            std::lock_guard sl(sm);
            st.framesPresented++;
            if (tIn >= 0 && e2eMs.size() < kMaxStatSamples) e2eMs.push_back(static_cast<float>(t - tIn));
            if (target != kNoTime && syncErrMs.size() < kMaxStatSamples)
                syncErrMs.push_back(static_cast<float>(t - target));
        }
    }
}

void VideoWindow::Impl::notePresent(bool picture) {
    const double now = nowMs();
    const bool did = ren.lastPresented();
    const HRESULT hr = ren.lastPresentHr();
    dg.lastPresentHr.store(hr, std::memory_order_relaxed);
    if (did) (hr == DXGI_STATUS_OCCLUDED ? dg.presentOccl : dg.presentOk).fetch_add(1, std::memory_order_relaxed);
    else if (FAILED(hr)) dg.presentErr.fetch_add(1, std::memory_order_relaxed);
    const bool occ = ren.occluded();
    if (occ != wasOccluded) {
        wasOccluded = occ;
        dg.occluded.store(occ, std::memory_order_relaxed);
        if (occ) {
            occludedSince = now;
            occludedPresents = 0;
            log("Present: DXGI_STATUS_OCCLUDED (window covered or off-screen): pictures at %.0f fps until visible",
                wd ? 1000 / kOccludedPresentMs : 60.0);
        } else {
            log("Present: S_OK again after %.1f s occluded (%lld pictures presented meanwhile)",
                (now - occludedSince) / 1000, occludedPresents);
        }
    }
    if (!picture || !did) return;
    dg.presented.fetch_add(1, std::memory_order_relaxed);
    lastPicturePresentAt = now;
    if (occ) ++occludedPresents;
    pendingSince = -1;
    pendingPictures = 0;
    if (reportPresentBack) {
        reportPresentBack = false;
        video::wdlog("picture presented again %.0f ms after the swap chain re-creation", now - swapRecoverAt);
    }
}

}  // namespace pm
