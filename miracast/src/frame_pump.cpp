#include "frame_pump.h"

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <mutex>
#include <thread>

#include "pm/video_window.h"

namespace pm {

namespace wmp = winrt::Windows::Media::Playback;
namespace wmc = winrt::Windows::Media::Core;
namespace wgd = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {

uint64_t nowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

template <class T>
struct Com {  // minimal COM smart pointer (no WRL / ATL dependency)
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T** put() {
        reset();
        return &p;
    }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// Content rectangle of a BGRA picture whose borders may be black bars.
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool operator==(const Rect&) const = default;
};

inline bool lit(const uint8_t* px) { return px[0] > 24 || px[1] > 24 || px[2] > 24; }

// Samples 17 lines across the picture; returns false for an all-black frame.
bool detectContent(const uint8_t* data, int w, int h, int stride, Rect& out) {
    constexpr int kSamples = 17;
    auto colLit = [&](int x) {
        for (int i = 1; i <= kSamples; ++i) {
            const int y = h * i / (kSamples + 1);
            if (lit(data + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4)) return true;
        }
        return false;
    };
    auto rowLit = [&](int y) {
        const uint8_t* row = data + static_cast<size_t>(y) * stride;
        for (int i = 1; i <= kSamples; ++i) {
            const int x = w * i / (kSamples + 1);
            if (lit(row + static_cast<size_t>(x) * 4)) return true;
        }
        return false;
    };
    int l = 0, r = w - 1, t = 0, b = h - 1;
    while (l < w && !colLit(l)) ++l;
    if (l >= w) return false;
    while (r > l && !colLit(r)) --r;
    while (t < h && !rowLit(t)) ++t;
    if (t >= h) return false;
    while (b > t && !rowLit(b)) --b;
    out = {l, t, r - l + 1, b - t + 1};
    return true;
}

}  // namespace

struct FramePump::Impl : std::enable_shared_from_this<Impl> {
    VideoWindow* window = nullptr;
    Log log;

    // D3D (context calls from the MF thread and the reader thread are
    // serialised by ID3D11Multithread).
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> ctx;
    Com<ID3D11Multithread> mt;

    struct Slot {
        Com<ID3D11Texture2D> staging;
        Com<ID3D11Query> done;
        enum class State { Free, Pending, Reading } state = State::Free;
        uint64_t seq = 0;
        uint64_t submittedNs = 0;
        uint64_t ptsNs = 0;
    };
    static constexpr int kSlots = 3;

    std::mutex m;  // guards everything below (not held during D3D waits / submit)
    std::condition_variable cv;
    std::array<Slot, kSlots> slots;
    Com<ID3D11Texture2D> target;
    wgd::IDirect3DSurface targetSurface{nullptr};
    int texW = 0, texH = 0;
    uint64_t seq = 0;
    bool quit = false;
    bool shownSomething = false;
    std::thread reader;

    wmp::MediaPlayer player{nullptr};
    winrt::event_token tokFrame{}, tokEnded{}, tokFailed{};
    std::mutex playerMutex;  // serialises play()/stop()
    std::atomic<int> generation{0};  // bumped by stop(): stale frames are ignored
    double volume = 1.0;
    std::atomic<bool> autoCrop{true};

    // Crop state (reader thread only).
    Rect crop{}, candidate{};
    int candidateCount = 0;
    bool firstFrameReported = false;

    // Stats.
    mutable std::mutex statsMutex;
    Stats st;
    double copyMsSum = 0, readMsSum = 0;

    std::function<void()> onEnded;
    std::function<void(const std::wstring&)> onFailed;
    std::function<void(int, int)> onFirstFrame;

    void logf(const char* fmt, ...) {
        if (!log) return;
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        log(buf);
    }

    bool createDevice() {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                            D3D_FEATURE_LEVEL_10_0};
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                                       D3D11_SDK_VERSION, device.put(), nullptr, ctx.put());
        if (FAILED(hr))
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, device.put(), nullptr, ctx.put());
        if (FAILED(hr)) {
            logf("miracast: D3D11CreateDevice failed 0x%08lx", hr);
            return false;
        }
        if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(mt.put()))))
            mt->SetMultithreadProtected(TRUE);
        return true;
    }

    // Caller holds m.  Recreates the render target + staging ring for w x h.
    bool ensureTextures(int w, int h) {
        if (target && w == texW && h == texH) return true;
        // A slot being read back still uses its texture: drop this picture and
        // resize on a later one (sizes change rarely).
        for (auto& s : slots)
            if (s.state == Slot::State::Reading) return false;
        for (auto& s : slots) {
            s.staging.reset();
            s.done.reset();
            s.state = Slot::State::Free;
        }
        target.reset();
        targetSurface = nullptr;
        texW = texH = 0;

        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(w);
        d.Height = static_cast<UINT>(h);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device->CreateTexture2D(&d, nullptr, target.put());
        if (FAILED(hr)) {
            logf("miracast: CreateTexture2D(target %dx%d) failed 0x%08lx", w, h, hr);
            return false;
        }
        Com<IDXGISurface> dxgi;
        hr = target->QueryInterface(__uuidof(IDXGISurface), reinterpret_cast<void**>(dxgi.put()));
        winrt::com_ptr<::IInspectable> insp;
        if (SUCCEEDED(hr)) hr = CreateDirect3D11SurfaceFromDXGISurface(dxgi.p, insp.put());
        if (FAILED(hr)) {
            logf("miracast: CreateDirect3D11SurfaceFromDXGISurface failed 0x%08lx", hr);
            target.reset();
            return false;
        }
        targetSurface = insp.as<wgd::IDirect3DSurface>();

        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        for (auto& s : slots) {
            if (FAILED(device->CreateTexture2D(&sd, nullptr, s.staging.put())) ||
                FAILED(device->CreateQuery(&qd, s.done.put()))) {
                logf("miracast: staging ring allocation failed (%dx%d)", w, h);
                target.reset();
                targetSurface = nullptr;
                return false;
            }
        }
        texW = w;
        texH = h;
        crop = {};
        candidateCount = 0;
        logf("miracast: frame size %dx%d", w, h);
        return true;
    }

    // MediaPlayer.VideoFrameAvailable (MF worker thread).
    void onFrameAvailable(const wmp::MediaPlayer& p, int gen) {
        const auto t0 = std::chrono::steady_clock::now();
        {
            std::lock_guard sl(statsMutex);
            ++st.framesAvailable;
        }
        auto session = p.PlaybackSession();
        const int w = static_cast<int>(session.NaturalVideoWidth());
        const int h = static_cast<int>(session.NaturalVideoHeight());
        if (w <= 0 || h <= 0) return;

        std::unique_lock lk(m);
        if (quit || gen != generation.load()) return;
        if (!ensureTextures(w, h)) return;
        Slot* slot = nullptr;
        for (auto& s : slots)
            if (s.state == Slot::State::Free) {
                slot = &s;
                break;
            }
        if (!slot) {
            lk.unlock();
            std::lock_guard sl(statsMutex);
            ++st.framesDropped;
            return;
        }
        try {
            p.CopyFrameToVideoSurface(targetSurface);
        } catch (const winrt::hresult_error& e) {
            lk.unlock();
            std::lock_guard sl(statsMutex);
            if (st.copyErrors++ < 5) logf("miracast: CopyFrameToVideoSurface failed 0x%08lx", static_cast<long>(e.code()));
            return;
        }
        ctx->CopyResource(slot->staging.p, target.p);
        ctx->End(slot->done.p);
        ctx->Flush();
        slot->state = Slot::State::Pending;
        slot->seq = ++seq;
        slot->submittedNs = nowNs();
        slot->ptsNs = slot->submittedNs;
        lk.unlock();
        cv.notify_one();

        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::lock_guard sl(statsMutex);
        ++st.framesCopied;
        copyMsSum += ms;
        st.copyAvgMs = copyMsSum / static_cast<double>(st.framesCopied);
        st.width = w;
        st.height = h;
    }

    Rect chooseCrop(const uint8_t* data, int w, int h, int stride) {
        const Rect full{0, 0, w, h};
        if (!autoCrop.load()) return full;
        Rect c;
        if (!detectContent(data, w, h, stride, c)) return crop.w ? crop : full;  // all black: keep
        // Only symmetric bars (pillar/letterbox) count; tolerate 8 px.
        const int l = c.x, r = w - (c.x + c.w), t = c.y, b = h - (c.y + c.h);
        Rect want = full;
        if (std::abs(l - r) <= 8 && std::min(l, r) >= w / 50) {
            want.x = std::min(l, r);
            want.w = w - 2 * want.x;
        }
        if (std::abs(t - b) <= 8 && std::min(t, b) >= h / 50) {
            want.y = std::min(t, b);
            want.h = h - 2 * want.y;
        }
        want.x &= ~1;
        want.y &= ~1;
        want.w = (std::min(want.w, w - want.x)) & ~1;
        want.h = (std::min(want.h, h - want.y)) & ~1;
        // First picture with content: take it.  Then grow at once (never hide
        // content) and shrink only after ~1 s of agreement.
        if (crop.w == 0) {
            crop = want;
            return crop;
        }
        if (want.x < crop.x || want.y < crop.y || want.x + want.w > crop.x + crop.w ||
            want.y + want.h > crop.y + crop.h) {
            Rect u;
            u.x = std::min(want.x, crop.x);
            u.y = std::min(want.y, crop.y);
            u.w = std::max(want.x + want.w, crop.x + crop.w) - u.x;
            u.h = std::max(want.y + want.h, crop.y + crop.h) - u.y;
            crop = u;
            candidateCount = 0;
        } else if (!(want == crop)) {
            if (want == candidate) {
                if (++candidateCount >= 30) {
                    crop = want;
                    candidateCount = 0;
                }
            } else {
                candidate = want;
                candidateCount = 1;
            }
        } else {
            candidateCount = 0;
        }
        return crop;
    }

    // ~250 us wait (Sleep(1) can take 15.6 ms at the default timer resolution).
    HANDLE hrTimer = nullptr;
    void shortSleep() {
        if (!hrTimer)
            hrTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        LARGE_INTEGER due;
        due.QuadPart = -2500;  // 100 ns units, relative
        if (hrTimer && SetWaitableTimer(hrTimer, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(hrTimer, 5);
        else
            Sleep(1);
    }

    void readerLoop() {
        for (;;) {
            Slot* slot = nullptr;
            int gen, w = 0, h = 0;
            {
                std::unique_lock lk(m);
                cv.wait(lk, [&] {
                    if (quit) return true;
                    for (auto& s : slots)
                        if (s.state == Slot::State::Pending) return true;
                    return false;
                });
                if (quit) return;
                gen = generation.load();
                // Newest pending slot whose copy has finished; superseded older
                // finished ones are dropped.  Poll without holding the lock long.
                for (int spins = 0;; ++spins) {
                    Slot* best = nullptr;
                    for (auto& s : slots) {
                        if (s.state != Slot::State::Pending) continue;
                        if (ctx->GetData(s.done.p, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                            (!best || s.seq > best->seq))
                            best = &s;
                    }
                    if (best) {
                        long long dropped = 0;
                        for (auto& s : slots)
                            if (s.state == Slot::State::Pending && s.seq < best->seq &&
                                ctx->GetData(s.done.p, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
                                s.state = Slot::State::Free;
                                ++dropped;
                            }
                        if (dropped) {
                            std::lock_guard sl(statsMutex);
                            st.framesDropped += dropped;
                        }
                        best->state = Slot::State::Reading;
                        slot = best;
                        w = texW;
                        h = texH;
                        break;
                    }
                    if (quit) return;
                    lk.unlock();
                    if (spins < 20)
                        std::this_thread::yield();
                    else
                        shortSleep();
                    lk.lock();
                    bool any = false;
                    for (auto& s : slots) any |= s.state == Slot::State::Pending;
                    if (!any) break;
                }
                if (!slot) continue;
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};
            HRESULT hr = ctx->Map(slot->staging.p, 0, D3D11_MAP_READ, 0, &mapped);
            if (SUCCEEDED(hr)) {
                const auto* data = static_cast<const uint8_t*>(mapped.pData);
                const int stride = static_cast<int>(mapped.RowPitch);
                const Rect c = chooseCrop(data, w, h, stride);
                const uint64_t readNs = nowNs();
                if (gen == generation.load()) {
                    window->submitBgraFrame(data + static_cast<size_t>(c.y) * stride + static_cast<size_t>(c.x) * 4,
                                            c.w, c.h, stride, slot->ptsNs);
                    shownSomething = true;
                }
                ctx->Unmap(slot->staging.p, 0);
                std::lock_guard sl(statsMutex);
                ++st.framesDelivered;
                readMsSum += static_cast<double>(readNs - slot->submittedNs) / 1e6;
                st.readbackAvgMs = readMsSum / static_cast<double>(st.framesDelivered);
                st.cropX = c.x;
                st.cropY = c.y;
                st.cropW = c.w;
                st.cropH = c.h;
            } else {
                logf("miracast: Map failed 0x%08lx", hr);
            }
            if (!firstFrameReported && SUCCEEDED(hr)) {
                firstFrameReported = true;
                if (onFirstFrame) onFirstFrame(w, h);
            }
            std::lock_guard lk(m);
            slot->state = Slot::State::Free;
            cv.notify_all();
        }
    }
};

FramePump::FramePump(VideoWindow* window, Log log) : impl_(std::make_shared<Impl>()) {
    impl_->window = window;
    impl_->log = std::move(log);
}

FramePump::~FramePump() {
    stop();
    {
        std::lock_guard lk(impl_->m);
        impl_->quit = true;
    }
    impl_->cv.notify_all();
    if (impl_->reader.joinable()) impl_->reader.join();
    if (impl_->hrTimer) CloseHandle(impl_->hrTimer);
}

bool FramePump::init() {
    if (impl_->device) return true;
    if (!impl_->createDevice()) return false;
    impl_->reader = std::thread([i = impl_.get()] { i->readerLoop(); });
    return true;
}

bool FramePump::play(const wmc::MediaSource& source, bool realTime) {
    if (!init()) return false;
    stop();
    auto& I = *impl_;
    std::lock_guard pl(I.playerMutex);
    I.onEnded = onEnded;
    I.onFailed = onFailed;
    I.onFirstFrame = onFirstFrame;
    I.firstFrameReported = false;
    try {
        wmp::MediaPlayer p;
        p.CommandManager().IsEnabled(false);  // no system media controls overlay
        p.AutoPlay(true);
        p.RealTimePlayback(realTime);
        p.IsVideoFrameServerEnabled(true);
        p.Volume(I.volume);
        const int gen = I.generation.load();
        std::weak_ptr<Impl> weak = impl_;
        I.tokFrame = p.VideoFrameAvailable([weak, gen](const wmp::MediaPlayer& sender, const auto&) {
            if (auto s = weak.lock()) {
                try {
                    s->onFrameAvailable(sender, gen);
                } catch (const winrt::hresult_error& e) {
                    s->logf("miracast: frame handler error 0x%08lx", static_cast<long>(e.code()));
                }
            }
        });
        I.tokEnded = p.MediaEnded([weak](const auto&, const auto&) {
            if (auto s = weak.lock()) {
                s->logf("miracast: media ended");
                if (s->onEnded) s->onEnded();
            }
        });
        I.tokFailed = p.MediaFailed([weak](const auto&, const wmp::MediaPlayerFailedEventArgs& a) {
            if (auto s = weak.lock()) {
                const std::wstring msg = std::wstring(a.ErrorMessage());
                s->logf("miracast: media failed: error %d hr 0x%08lx", static_cast<int>(a.Error()),
                        static_cast<long>(a.ExtendedErrorCode()));
                if (s->onFailed) s->onFailed(msg.empty() ? L"MediaPlayer error" : msg);
            }
        });
        p.Source(wmp::MediaPlaybackItem(source));
        p.Play();
        I.player = p;
    } catch (const winrt::hresult_error& e) {
        I.logf("miracast: MediaPlayer setup failed 0x%08lx", static_cast<long>(e.code()));
        return false;
    }
    return true;
}

void FramePump::stop() {
    auto& I = *impl_;
    std::lock_guard pl(I.playerMutex);
    if (!I.player) return;
    I.generation.fetch_add(1);
    wmp::MediaPlayer p = I.player;
    I.player = nullptr;
    try {
        p.VideoFrameAvailable(I.tokFrame);
        p.MediaEnded(I.tokEnded);
        p.MediaFailed(I.tokFailed);
        p.Pause();
        p.Source(nullptr);
        p.Close();
    } catch (const winrt::hresult_error& e) {
        I.logf("miracast: MediaPlayer close error 0x%08lx", static_cast<long>(e.code()));
    }
    // Let the reader finish (it ignores slots of the old generation).
    {
        std::unique_lock lk(I.m);
        I.cv.wait_for(lk, std::chrono::milliseconds(500), [&] {
            for (auto& s : I.slots)
                if (s.state == Impl::Slot::State::Reading) return false;
            return true;
        });
        for (auto& s : I.slots)
            if (s.state == Impl::Slot::State::Pending) s.state = Impl::Slot::State::Free;
    }
    if (I.shownSomething) {
        I.shownSomething = false;
        I.window->onReset();
    }
}

void FramePump::setVolume(double v) {
    auto& I = *impl_;
    std::lock_guard pl(I.playerMutex);
    I.volume = std::clamp(v, 0.0, 1.0);
    if (I.player) {
        try {
            I.player.Volume(I.volume);
        } catch (...) {
        }
    }
}

void FramePump::setAutoCrop(bool enabled) { impl_->autoCrop = enabled; }

FramePump::Stats FramePump::stats() const {
    std::lock_guard sl(impl_->statsMutex);
    return impl_->st;
}

}  // namespace pm
