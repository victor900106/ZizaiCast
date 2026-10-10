// VideoWindow::Impl: D3D11 device: adapter choice, creation, loss / recovery, GOP re-feed, decoder opening.
// 拆檔 0.7.9：自 video_window.cpp（Impl 類別內）原樣搬出，改成類別外定義。
#include "video_window_impl.h"

namespace pm {

ComPtr<IDXGIAdapter1> VideoWindow::Impl::chooseAdapter(const char*& why) {
    ComPtr<IDXGIFactory1> f1;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f1)))) return {};
    auto software = [](IDXGIAdapter1* a) {
        DXGI_ADAPTER_DESC1 d{};
        return FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    };
    auto hasOutputs = [](IDXGIAdapter1* a) {
        ComPtr<IDXGIOutput> o;
        return SUCCEEDED(a->EnumOutputs(0, &o));
    };
    ComPtr<IDXGIAdapter1> pref;
    ComPtr<IDXGIFactory6> f6;
    if (SUCCEEDED(f1.As(&f6))) {
        const auto p = forceAdapter == 1 ? DXGI_GPU_PREFERENCE_MINIMUM_POWER : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; SUCCEEDED(f6->EnumAdapterByGpuPreference(i, p, IID_PPV_ARGS(&a))); ++i, a.Reset())
            if (!software(a.Get())) {
                pref = a;
                break;
            }
        why = forceAdapter == 1 ? "power-saving GPU (test)" : "high-performance GPU";
    } else {
        f1->EnumAdapters1(0, &pref);
        why = "default adapter";
    }
    if (forceAdapter == 1) return pref;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    ComPtr<IDXGIAdapter1> a, monAdapter;
    for (UINT i = 0; !monAdapter && SUCCEEDED(f1->EnumAdapters1(i, &a)); ++i, a.Reset()) {
        ComPtr<IDXGIOutput> o;
        for (UINT j = 0; SUCCEEDED(a->EnumOutputs(j, &o)); ++j, o.Reset()) {
            DXGI_OUTPUT_DESC od{};
            if (SUCCEEDED(o->GetDesc(&od)) && od.Monitor == mon) {
                monAdapter = a;
                break;
            }
        }
    }
    if (pref && monAdapter && !software(monAdapter.Get())) {
        DXGI_ADAPTER_DESC1 dp{}, dm{};
        pref->GetDesc1(&dp);
        monAdapter->GetDesc1(&dm);
        const bool same = dp.AdapterLuid.LowPart == dm.AdapterLuid.LowPart &&
                          dp.AdapterLuid.HighPart == dm.AdapterLuid.HighPart;
        if (!same && hasOutputs(pref.Get())) {
            why = "GPU driving the window's monitor";
            return monAdapter;
        }
    }
    return pref;
}

bool VideoWindow::Impl::createDevice() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                  D3D_FEATURE_LEVEL_10_0};
    const char* why = "default adapter";
    ComPtr<IDXGIAdapter1> adapter;
    if (forceAdapter != 2) adapter = chooseAdapter(why);
    auto create = [&](UINT f) {
        dev.Reset();
        if (forceAdapter == 2)
            return D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, f, levels,
                                     static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &dev, nullptr,
                                     nullptr);
        return D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                 nullptr, f, levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &dev,
                                 nullptr, nullptr);
    };
    HRESULT hr = create(flags);
    if (FAILED(hr)) {
        log("no D3D11 video device (hr=0x%08lx), trying without video support", hr);
        flags &= ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        hr = create(flags);
    }
    if (FAILED(hr) && forceAdapter != 2) {
        log("no hardware D3D11 device, using WARP");
        dev.Reset();
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels,
                               static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &dev, nullptr, nullptr);
    }
    if (FAILED(hr)) {
        log("D3D11CreateDevice failed hr=0x%08lx", hr);
        dev.Reset();
        return false;
    }
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> used;
    DXGI_ADAPTER_DESC ad{};
    if (SUCCEEDED(dev.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&used)) && SUCCEEDED(used->GetDesc(&ad))) {
        char name[128];
        WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name), nullptr, nullptr);
        log("D3D11 adapter: %s (%s)", name, forceAdapter == 2 ? "WARP (test)" : why);
        adapterLuid = ad.AdapterLuid;
        std::lock_guard sl(sm);
        st.adapter = ad.Description;
    }
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);
    dxgiMgr.Reset();
    if (flags & D3D11_CREATE_DEVICE_VIDEO_SUPPORT) {
        if (SUCCEEDED(MFCreateDXGIDeviceManager(&resetToken, &dxgiMgr)))
            dxgiMgr->ResetDevice(dev.Get(), resetToken);
    }
    return true;
}

bool VideoWindow::Impl::initGraphics() { return createDevice() && ren.init(hwnd, dev.Get()); }

void VideoWindow::Impl::loseDevice(const char* why, HRESULT reason) {
    if (deviceLost) return;
    log("D3D11 device lost: %s (reason=0x%08lx); re-creating device, swap chain and decoder", why, reason);
    dropHeld();
    dec.close();
    ren.releaseDevice();
    dxgiMgr.Reset();
    dev.Reset();
    deviceLost = true;
    faultRemoved = false;
    lostAt = nowMs();
    nextRecoverAt = 0;
    recoverAttempts = 0;
    newPicture = false;
    needPresent = true;
}

void VideoWindow::Impl::applyFrozen(bool on, const char* why) {
    const bool was = ren.frozen();
    ren.setFrozen(on);
    if (was == on) return;
    const double now = nowMs();
    const double since = dg.frozenSince.exchange(on ? now : -1.0);
    if (on) {
        freezeAuIn = dg.auIn.load();
        freezeDecoded = dg.decoded.load();
        log("picture frozen (%s)", why);
        return;
    }
    const double pic = dg.lastPictureAt.load(), au = dg.lastAuAt.load(), idr = dg.lastIdrAt.load();
    log("picture unfrozen (%s) after %.1f s: live picture again (newest decoded %.1f s ago; while frozen %lld AUs "
        "in, %lld pictures decoded; last AU %.1f s ago, last IDR %.1f s ago%s)",
        why, since >= 0 ? (now - since) / 1000 : -1.0, pic >= 0 ? (now - pic) / 1000 : -1.0,
        dg.auIn.load() - freezeAuIn, dg.decoded.load() - freezeDecoded, au >= 0 ? (now - au) / 1000 : -1.0,
        idr >= 0 ? (now - idr) / 1000 : -1.0, refsLost ? "; references lost, wrong until the next IDR" : "");
}

void VideoWindow::Impl::freezeDropped() {
    dg.frozenSince.store(-1.0);
    log("picture unfrozen (device lost; the frozen picture could not be kept), text overlay cleared");
    std::shared_ptr<std::function<void(const VideoWindow::ViewState&)>> fn;
    VideoWindow::ViewState vs;
    {
        std::lock_guard lk(m);
        if (frozenReq < 0) frozenUi = false;  // a newer request still applies
        boxesReq.reset();                     // boxes for the frozen picture
        fn = viewFn;
        vs = viewStateLocked();
    }
    static const UINT runOnUiMsg = RegisterWindowMessageW(L"PhoneMirror.Video.RunOnUi");
    if (HWND h = hwnd.load(); h && fn && *fn && runOnUiMsg) {
        auto* call = new std::function<void()>([fn, vs] { (*fn)(vs); });
        if (!PostMessageW(h, runOnUiMsg, 0, reinterpret_cast<LPARAM>(call))) delete call;
    }
}

size_t VideoWindow::Impl::refeedGop() {
    const size_t n = gopTruncated ? std::min<size_t>(gop.size(), 1) : gop.size();
    if (gopTruncated && n) {
        refsLost = true;
        log("GOP since the last IDR exceeds the cache (%zu AUs / %zu MB): only the IDR is re-fed; pictures "
            "until the next IDR lack references",
            gopMaxAUs(), gopMaxBytes() >> 20);
    }
    for (size_t i = 0; i < n && !deviceLost; ++i) {  // lost again: recovery re-feeds
        skipOutput = i + 1 < n;
        feed(gop[i].data.data(), gop[i].data.size(), -1, gop[i].due);
    }
    skipOutput = false;
    return n;
}

bool VideoWindow::Impl::checkDevice() {
    if (deviceLost) return true;
    if (!dev) return false;
    const HRESULT r = faultRemoved ? DXGI_ERROR_DEVICE_REMOVED : dev->GetDeviceRemovedReason();
    if (SUCCEEDED(r)) return false;
    loseDevice(faultRemoved ? "device removed (test: seen inside the decoder callback)" : "device removed", r);
    return true;
}

void VideoWindow::Impl::deviceFailed() {
    if (inFeed) lostInDecode = true;
    else checkDevice();
}

bool VideoWindow::Impl::recoverDevice() {
    const double now = nowMs();
    if (now < nextRecoverAt) return false;
    if (!createDevice() || !ren.attachDevice(dev.Get())) {
        ren.releaseDevice();
        dxgiMgr.Reset();
        dev.Reset();
        ++recoverAttempts;
        nextRecoverAt = now + std::min(2000.0, 100.0 * recoverAttempts);  // a driver update takes seconds
        if (recoverAttempts == 1 || recoverAttempts % 20 == 0)
            log("device re-creation failed (attempt %d), retrying", recoverAttempts);
        return false;
    }
    const double tDevice = nowMs();
    deviceLost = false;
    if (ren.takeFrozenDropped()) freezeDropped();
    hwBroken = false;  // new device: give DXVA another chance
    // Decoder on the new device; re-feed everything since the last key
    // frame so the next picture is complete.
    dec.close();
    size_t refed = 0;
    double tDecoder = 0;
    setStage(StRecover);
    if (!gop.empty() && openDecoder()) {
        tDecoder = nowMs();
        refed = refeedGop();
    } else {
        waitKey = true;
        if (!decoderMissing && !dec.isOpen()) openDecoder();
    }
    if (deviceLost) {  // lost again during the re-feed: start over
        log("device lost again during the re-feed (attempt %d)", recoverAttempts + 1);
        return false;
    }
    if (bgraLive) showBgra(-1, 0);  // the BGRA source's last picture (no tap: already delivered)
    const double tEnd = nowMs();
    log("device re-created in %.0f ms (attempt %d): device+swap chain %.0f ms, decoder %.0f ms, %zu AU(s) re-fed in %.0f ms",
        tEnd - lostAt, recoverAttempts + 1, tDevice - now, tDecoder ? tDecoder - tDevice : 0.0, refed,
        tDecoder ? tEnd - tDecoder : 0.0);
    {
        std::lock_guard sl(sm);
        st.deviceRecoveries++;
    }
    reportPictureBack = !ren.idle();
    ren.poke();
    needPresent = true;
    starvedSince = pendingSince = waitStuckSince = -1;
    starvedFeeds = pendingPictures = 0;
    return true;
}

void VideoWindow::Impl::checkAdapter(const char* trigger) {
    if (deviceLost || forceAdapter == 2) return;
    const char* why = "";
    ComPtr<IDXGIAdapter1> a = chooseAdapter(why);
    DXGI_ADAPTER_DESC1 d{};
    if (!a || FAILED(a->GetDesc1(&d))) return;
    char name[128];
    WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), nullptr, nullptr);
    if (sameLuid(d.AdapterLuid, adapterLuid)) {
        log("adapter check (%s): keeping %s", trigger, name);
        if (adapterSwitchPending) log("adapter check: pending GPU switch cancelled");
        adapterSwitchPending = false;
        return;
    }
    // Switching re-creates the decoder and re-feeds the GOP cache.  If the
    // cache no longer holds everything since the last IDR, that would
    // lose the references (an iOS stream would freeze until its next
    // IDR): wait for one instead.
    if (wd && streamLive() && !waitKey && gopTruncated) {
        log("adapter check (%s): %s preferred (%s); switching at the next key frame", trigger, name, why);
        adapterSwitchPending = true;
        adapterSwitchWhy = why;
        return;
    }
    log("adapter check (%s): switching to %s (%s)", trigger, name, why);
    loseDevice(why, S_OK);
}

bool VideoWindow::Impl::openDecoder() {
    decoderMissing = false;
    // PM_VIDEO_FORCE_SW=1 disables DXVA (diagnostics / testing the fallback).
    static const bool forceSw = [] {
        char v[8]{};
        return GetEnvironmentVariableA("PM_VIDEO_FORCE_SW", v, sizeof(v)) > 0 && v[0] == '1';
    }();
    dec.setConcealMissingRefs(wd);
    bool ok = (dxgiMgr && !hwBroken && !forceSw && dec.open(decCodec, dxgiMgr.Get())) || dec.open(decCodec, nullptr);
    if (!ok) {
        decoderMissing = true;
        return false;
    }
    dg.hw.store(dec.hardware(), std::memory_order_relaxed);
    if (faultWedgeHw > 0 && dec.hardware()) {
        --faultWedgeHw;
        dec.setFaultDropOutput(true);
        log("fault injection: this hardware decoder instance swallows its output");
    }
    return true;
}

}  // namespace pm
