#include "pm/video_window.h"

#include <windows.h>
#include <windowsx.h>

#include <d3d11.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cmath>
#include <functional>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "annexb.h"
#include "log.h"
#include "mf_decoder.h"
#include "renderer.h"
#include "ui_art.h"

namespace pm::video {

namespace {

std::mutex g_logM;
std::shared_ptr<std::function<void(const char*)>> g_logFn;  // VideoWindow::setLogHandler

void vlog(const char* tag, const char* fmt, va_list ap) {
    char buf[1024];
    int n = std::snprintf(buf, sizeof(buf), "%s ", tag);
    int m = std::vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    size_t end = std::min(sizeof(buf) - 2, static_cast<size_t>(n) + static_cast<size_t>(std::max(m, 0)));
    buf[end] = 0;
    std::shared_ptr<std::function<void(const char*)>> fn;
    {
        std::lock_guard lk(g_logM);
        fn = g_logFn;
    }
    if (fn && *fn) (*fn)(buf);  // the app's log file (no newline)
    buf[end] = '\n';
    buf[end + 1] = 0;
    std::fputs(buf, stderr);
    OutputDebugStringA(buf);
}

}  // namespace

void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog("[video]", fmt, ap);
    va_end(ap);
}

void wdlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog("[video-watchdog]", fmt, ap);
    va_end(ap);
}

}  // namespace pm::video

namespace pm {

using Microsoft::WRL::ComPtr;
using video::log;

namespace {

constexpr wchar_t kClassName[] = L"PhoneMirrorVideoWindow";
// AU queue: beyond kMaxQueuedAUs (~0.5 s at 60 fps) the queue skips to the
// newest queued IDR (clean).  Without one, AUs are only dropped beyond the
// hard limit: a dropped reference breaks every picture until the next IDR,
// and iOS sends one only every minute or so (the HEVC decoder then shows
// nothing at all), so a stalled render thread catches up instead (decoding
// is ~2 ms per AU; only the newest picture is presented).
constexpr size_t kMaxQueuedAUs = 32;
constexpr size_t kHardMaxQueuedAUs = 600;           // ~10 s at 60 fps
constexpr size_t kHardMaxQueuedBytes = 128u << 20;
constexpr size_t kMaxStatSamples = 1 << 20;
// A/V sync: decoded pictures wait at most this long (a stale or absurd
// timestamp cannot freeze the picture), and at most this many are held.
constexpr double kSyncMaxHoldMs = 500;
constexpr size_t kSyncMaxHeld = 40;
constexpr double kSyncSlackMs = 2;  // present this early (timer granularity)
// Device-loss / watchdog recovery re-feeds the access units since the last
// key frame (bounded; beyond that only the key frame is re-fed, and an HEVC
// stream stays frozen until the next IDR).  0.6.1: 300 AUs / 16 MB (5 s).
constexpr size_t kGopMaxAUs = 1800;  // 30 s at 60 fps
constexpr size_t kGopMaxBytes = 64u << 20;
// Watchdog (PM_VIDEO_WATCHDOG=0 turns it off together with the 0.6.2 queue /
// GOP limits and the deferred GPU switch: the 0.6.1 behaviour, for tests).
constexpr double kWdStallMs = 1500;      // no picture decoded / presented for this long
constexpr int kWdMinFeeds = 8;           // ... although at least this many AUs were fed
constexpr double kWdBackoffMs = 10000;   // later attempts in the same episode
constexpr double kOccludedPresentMs = 100;  // present rate while DXGI reports occlusion
constexpr double kHwRetryMs = 30000;     // after a watchdog SW fallback: HW again at an IDR

bool watchdogEnabled() {
    static const bool on = [] {
        char v[8]{};
        return !(GetEnvironmentVariableA("PM_VIDEO_WATCHDOG", v, sizeof(v)) > 0 && v[0] == '0');
    }();
    return on;
}
constexpr double kNoTime = -1e300;  // AccessUnit::due: show ASAP

double nowMs() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
}

// The two candidate clocks for ntpLocalNs (pm::AirPlayServer::localTimeNs()):
// QPC in ns (like the core's CLOCK_MONOTONIC / std::steady_clock) and the
// precise system time in ns since the Unix epoch (CLOCK_REALTIME).
int64_t qpcNs() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart / freq * 1000000000LL + c.QuadPart % freq * 1000000000LL / freq;
}

int64_t utcNs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const int64_t t = static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    return (t - 116444736000000000LL) * 100;
}

struct AccessUnit {
    std::vector<uint8_t> data;
    double tIn = 0;     // onFrame() time
    double due = kNoTime;  // presentation time on the nowMs() clock (sync mode, before audio latency)
    bool irap = false;  // IDR / IRAP picture
    bool keep = false;  // irap or carries parameter sets: never drop
    uint64_t pts = 0;   // frame tap: ntpLocalNs, or arrival time on the same clock
};

struct SnapshotRequest {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<uint8_t> pixels;  // BGRX
    UINT w = 0, h = 0;
    bool framed = false;  // device frame, transparent background (BGRA)
    bool ui = false;      // the whole window as drawn (saveWindowShot)
    bool grab = false;    // content picture for OCR (grabPicture): not mirrored
    bool ok = false;
    ~SnapshotRequest() { CloseHandle(done); }
};

using IdleOption = VideoWindow::IdleOption;

void percentile(std::vector<float> v, double& avg, double& p95) {
    avg = p95 = 0;
    if (v.empty()) return;
    double sum = 0;
    for (float x : v) sum += x;
    avg = sum / v.size();
    size_t k = std::min(v.size() - 1, static_cast<size_t>(v.size() * 0.95));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    p95 = v[k];
}

// Signed average and 95th percentile of the absolute value.
void absPercentile(std::vector<float> v, double& avg, double& p95abs) {
    double unused;
    percentile(v, avg, unused);
    for (float& x : v) x = std::fabs(x);
    percentile(std::move(v), unused, p95abs);
}

const UINT kTestMsg = RegisterWindowMessageW(L"PhoneMirror.Video.Test");
constexpr UINT_PTR kHoverTimer = 0x504D0001;  // trailing hover move (app timers use small ids)
// Posted by onReset(): the UI thread ends a region selection in progress.
const UINT kCancelSelectMsg = RegisterWindowMessageW(L"PhoneMirror.Video.CancelSelect");
constexpr double kHoverMinMs = 1000.0 / 60;    // hover moves at most ~60 Hz

// A small ring with a dot (touch point), 32x32, hot spot in the middle:
// shown over the picture while a pointer handler is set.
HCURSOR createTouchCursor() {
    constexpr int N = 32;
    BITMAPV5HEADER bh{};
    bh.bV5Size = sizeof(bh);
    bh.bV5Width = N;
    bh.bV5Height = -N;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000;
    bh.bV5GreenMask = 0x0000FF00;
    bh.bV5BlueMask = 0x000000FF;
    bh.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bh), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !bits) return LoadCursorW(nullptr, IDC_CROSS);
    auto* px = static_cast<uint32_t*>(bits);
    const float c = (N - 1) / 2.f;
    auto cover = [](float d, float r0, float r1) {  // anti-aliased band r0..r1
        return std::clamp(std::min(d - r0 + 0.5f, r1 - d + 0.5f), 0.f, 1.f);
    };
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            const float d = std::hypot(x - c, y - c);
            // white ring + dot over a dark outline (visible on any picture)
            const float white = std::max(cover(d, 7.f, 9.f), cover(d, 0.f, 1.6f));
            const float dark = std::max({cover(d, 6.f, 10.f), cover(d, 0.f, 2.6f)});
            const float a = std::max(white, dark * 0.75f);
            const float v = a > 0 ? white / a : 0;  // straight colour
            const uint8_t g = static_cast<uint8_t>(std::lround(v * 255 * a));  // premultiplied
            px[y * N + x] = (static_cast<uint32_t>(std::lround(a * 255)) << 24) | (g << 16) | (g << 8) | g;
        }
    HBITMAP mask = CreateBitmap(N, N, 1, 1, nullptr);
    ICONINFO ii{FALSE, N / 2, N / 2, mask, color};
    HCURSOR cur = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return cur ? cur : LoadCursorW(nullptr, IDC_CROSS);
}

}  // namespace

struct VideoWindow::Impl {
    std::atomic<HWND> hwnd{nullptr};  // read by close()/hwnd() from any thread
    std::thread worker;

    // ---- shared state (guarded by m) ----
    std::mutex m;
    HANDLE wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset: "state changed"
    // High-resolution timer paces animation frames (a condition-variable
    // timeout would round to the 15.6 ms system tick and stutter).
    HANDLE frameTimer = [] {
        HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        return t ? t : CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }();
    void wake() { SetEvent(wakeEvent); }
    ~Impl() {
        if (wakeEvent) CloseHandle(wakeEvent);
        if (frameTimer) CloseHandle(frameTimer);
        stopMonitor();
        if (monEvent) CloseHandle(monEvent);
    }
    std::deque<AccessUnit> queue;
    // UI requests (applied by the worker in take()).
    std::optional<std::wstring> connectingReq, pinReq, toastReq;
    double toastHoldReq = 0;
    // Live toolbar: items (worker), ids + click handler (UI thread), activity /
    // hover requests and the published hit rectangles (client pixels).
    std::optional<std::vector<video::Renderer::ToolItem>> toolbarReq;
    std::vector<int> toolIds;
    std::function<void(int)> toolFn;
    bool toolActivityReq = false, toolLeaveReq = false;
    int toolHoverReq = -2;  // -2 none, else hovered button (-1: none)
    bool toolInsideReq = false;
    std::vector<RECT> toolRects;
    RECT toolPill{};
    std::vector<IdleOption> options;
    std::function<void(int, bool)> onToggle;
    bool optionsChanged = false;
    int hover = -1;
    bool hoverChanged = false;
    bool pokeReq = false;
    UINT dpiReq = 0;
    int visibleReq = -1;
    std::shared_ptr<SnapshotRequest> snapReq;
    std::vector<RECT> optionRects;  // published by the worker (client pixels)
    // Idle hints / action requests and the published action / picture
    // geometry (client pixels; picRect empty unless a live picture is up).
    std::optional<std::vector<std::wstring>> hintsReq;
    std::optional<std::vector<video::Renderer::Action>> actionsReq;
    std::vector<std::function<void()>> actionFns;  // per action (UI thread callbacks)
    int linkHoverReq = -2;                          // -2 none, else hovered action (-1: none hot)
    std::vector<RECT> linkRects;
    RECT picRect{};
    int picRot = 0;
    bool picMirror = false;
    // Remote-control handlers (called on the UI thread; copied under m).
    std::shared_ptr<std::function<void(const PointerEvent&)>> pointerFn;
    std::shared_ptr<std::function<void(unsigned, bool, wchar_t)>> keyFn;
    // BGRA mailbox (submitBgraFrame): the newest picture only, guarded by
    // bgraM; bgraNew tells the worker (read without bgraM in pendingLocked).
    std::mutex bgraM;
    std::vector<uint8_t> bgraBuf;
    int bgraW = 0, bgraH = 0;
    double bgraTIn = 0;
    uint64_t bgraPts = 0;
    std::atomic<bool> bgraNew{false};
    VideoCodec codec = VideoCodec::H264;
    bool codecChanged = false;
    bool resetPending = false;
    int pausePending = -1;  // -1 none, 0 resume, 1 pause
    bool dirty = true;
    bool resizePending = false;
    UINT pendW = 0, pendH = 0;
    bool stop = false;
    bool warnedRefDrop = false;
    bool codecAnnounced = false;
    bool syncChanged = false;
    bool adapterCheckReq = false;  // monitor / display configuration changed
    int testReq = -1;              // kTestMsg: 0 simulate device removal, 1 power-saving GPU, 2 WARP, 3 auto
    LPARAM testArg = 0;            // its lParam (fault injection: duration / count)
    size_t dropNextRefs = 0;       // fault 24: drop this many non-IDR AUs in onFrame (guarded by m)
    double queueFullSince = -1;    // first AU dropped in this backlog (guarded by m)
    std::atomic<bool> refsDropped{false};  // a non-IDR AU was dropped before decoding
    // Presentation / theme / recording / mascot requests (-1 = none).
    int themeReq = -1, dimReq = -1, frameReq = -1, recReq = -1, mascotHoverReq = -1;
    bool xformChanged = false, mascotClickReq = false, tapChanged = false;
    // Magnifier / filter / freeze / text overlay / region selection requests.
    // viewUi + frozenUi: the latest requested state (any thread, under m);
    // pubView: what the worker last drew (mapPoint uses it with picRect).
    video::Renderer::View viewUi, pubView;
    bool frozenUi = false;
    bool viewChanged = false;
    int frozenReq = -1, originalReq = -1;
    std::optional<std::vector<video::Renderer::TextBox>> boxesReq;
    std::optional<std::wstring> busyReq;
    // Text overlay list panel / markers: requests (UI thread) and the
    // published hit rectangles (worker).
    int ovHotReq = -2, ovSelReq = -2;  // -2 none, else item (-1: none)
    int ovModeReq = -1, ovDarkReq = -1;  // -1 none (setTextOverlayStyle)
    bool ovRevealReq = false;
    float ovScrollReq = 0;
    video::Renderer::OverlayHits ovHits;
    int ovSelUi = -1;  // selected list item (UI thread; reset by setTextOverlay)
    bool selChanged = false, selActiveReq = false;
    float selReq[4] = {-1, -1, -1, -1};
    std::shared_ptr<std::function<void(const VideoWindow::ViewState&)>> viewFn;
    RECT mascotRect{};  // published by the worker (client pixels), empty if not clickable
    std::vector<uint8_t> mascotMask;  // copy of the art's hit mask (UI thread)
    UINT maskW = 0, maskH = 0;
    std::atomic<int> rotation{0};
    std::atomic<bool> mirrored{false};
    std::atomic<int> srcW{0}, srcH{0};  // visible size of the newest decoded picture

    // Frame tap: tapFn is guarded by tapCallM, which the worker holds while
    // calling it (so setFrameTap() returning means the old tap is done).
    std::mutex tapCallM;
    std::shared_ptr<VideoWindow::FrameTap> tapFn;
    std::atomic<bool> tapOn{false};
    std::atomic<DWORD> workerTid{0};

    // A/V sync settings (any thread).
    std::atomic<bool> syncEnabled{false};
    std::atomic<int> syncLatencyMs{0};
    std::atomic<int> ntpClock{0};  // 1 QPC, 2 UTC: which clock ntpLocalNs was found on (logging)

    // ---- stats (guarded by sm) ----
    mutable std::mutex sm;
    Stats st;
    std::vector<float> decMs, e2eMs, syncErrMs, tapMs;

    // ---- UI thread only ----
    bool fullscreen = false;
    WINDOWPLACEMENT savedPlacement{sizeof(WINDOWPLACEMENT)};
    bool stopping = false;
    bool trackingMouse = false;
    int hoverUi = -1;     // option under the cursor
    int pressedUi = -1;   // option where the left button went down
    bool mascotHotUi = false;      // cursor over the mascot
    bool pressedMascot = false;    // left button went down on her
    int linkHotUi = -1, pressedLink = -1;  // idle action under the cursor / pressed (-1 none)
    int toolHotUi = -1;            // toolbar button under the cursor
    bool toolInsideUi = false;     // cursor on the toolbar pill
    int pressedTool = -2;          // left press on the pill: button (-1 padding), -2 none
    double lastToolPokeUi = -1e9;
    bool synthMouse = false;       // test hook: no TrackMouseEvent (posted moves)
    // Remote control
    HCURSOR touchCursor = nullptr;
    int buttonsDown = 0;           // bit per button forwarded as Down (capture held)
    float lastPx = 0.5f, lastPy = 0.5f;  // last forwarded position
    double lastHoverUi = -1e9;
    bool hoverPending = false;     // a throttled hover move waits for kHoverTimer
    LPARAM hoverLp = 0;
    std::bitset<256> keysDown;     // keys forwarded as down (their up is forwarded too)
    double lastPokeUi = -1e9;
    HMONITOR lastMonitor = nullptr;

    // ---- worker thread only ----
    ComPtr<ID3D11Device> dev;
    ComPtr<IMFDXGIDeviceManager> dxgiMgr;
    UINT resetToken = 0;
    LUID adapterLuid{};
    int forceAdapter = 0;     // test hook: 0 auto, 1 power-saving GPU, 2 WARP
    bool deviceLost = false;  // device objects released, re-creation pending
    double lostAt = 0, nextRecoverAt = 0;
    int recoverAttempts = 0;
    video::MfDecoder dec;
    video::Renderer ren;
    VideoCodec decCodec = VideoCodec::H264;
    bool hwBroken = false;       // HW decode failed once: stay on SW
    bool decoderMissing = false; // no MFT for this codec: drop until codec change
    bool waitKey = true;         // need parameter sets + IRAP before decoding
    // Access units since the last key frame (first = the key frame): re-fed
    // after a device loss or a HW -> SW decoder switch.
    std::vector<AccessUnit> gop;
    size_t gopBytes = 0;
    bool gopTruncated = false;
    int64_t seq = 0;
    static constexpr int kRing = 256;
    double ringIn[kRing]{}, ringSubmit[kRing]{}, ringDue[kRing]{};
    uint64_t ringPts[kRing]{};
    double pictureTIn = -1;     // tIn of the most recent uploaded picture
    double pictureTarget = kNoTime;  // sync mode: when it was meant to be shown
    bool newPicture = false;
    int errorStreak = 0;
    // A/V sync: decoded pictures waiting for their presentation time.
    struct Held {
        video::Renderer::Picture pic;
        double due;      // nowMs() clock, without the audio latency
        double tIn;      // onFrame() time
        double decoded;  // when it was decoded (caps the hold time)
    };
    std::deque<Held> held;
    bool syncActive = false;
    bool skipOutput = false;  // re-feeding after device loss: discard all but the last picture
    bool reportPictureBack = false;  // log when the first picture after a recovery is up
    // BGRA source: the picture on screen (CPU copy, re-uploaded after a
    // device loss) and whether it is the current source.
    std::vector<uint8_t> bgraCur;
    int bgraCurW = 0, bgraCurH = 0;
    bool bgraLive = false;

    // ---- watchdog (worker thread only) ----
    const bool wd = watchdogEnabled();
    double starvedSince = -1;     // first AU fed after the last decoded picture (-1: a picture came)
    int starvedFeeds = 0;         // AUs fed since then
    double keyWaitSince = -1;     // waitKey: first AU skipped while waiting for an IDR
    int decRecoveries = 0;        // watchdog decoder restarts in this episode
    double decRecoverAt = -1e300;  // last watchdog decoder restart
    std::deque<double> decFailTimes;  // watchdog decoder restarts in the last minute
    bool reportDecodeBack = false;
    double pendingSince = -1;     // a decoded picture waits for a successful Present since
    int pendingPictures = 0;
    double waitStuckSince = -1;   // frame waitable timing out continuously since
    double swapRecoverAt = -1e300;
    bool reportPresentBack = false;
    double lastPicturePresentAt = -1e300;  // a picture went to Present (occlusion pacing)
    bool wasOccluded = false;
    double occludedSince = 0;
    long long occludedPresents = 0;
    double swSince = -1;          // watchdog switched HW -> SW at
    bool adapterSwitchPending = false;  // display change: another GPU is better, switch at the next IDR
    std::string adapterSwitchWhy;
    int faultWedgeHw = 0;         // fault 20: wedge this many more HW decoder instances

    // ---- diagnostics (written by the worker / onFrame, read by the monitor thread) ----
    enum Stage : int { StWait, StDecode, StFrameWait, StPresent, StRecover, StTap, StOther };
    struct Diag {
        std::atomic<long long> auIn{0}, auDropped{0}, auFed{0}, auSkipped{0}, mftOut{0}, decoded{0}, presented{0},
            presentOk{0}, presentOccl{0}, presentErr{0}, waitTimeouts{0}, recoveries{0};
        std::atomic<double> lastIdrAt{-1}, lastAuAt{-1}, stageAt{0};
        std::atomic<int> stage{StWait};
        std::atomic<long> lastPresentHr{0}, lastDecodeHr{0};
        std::atomic<bool> occluded{false}, hw{false};
        std::atomic<int> gopAUs{0};
        std::atomic<bool> gopTrunc{false};
    } dg;
    void setStage(Stage s) {
        dg.stage.store(s, std::memory_order_relaxed);
        dg.stageAt.store(nowMs(), std::memory_order_relaxed);
    }
    std::thread monitor;
    HANDLE monEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // wakes the monitor (stream starts / stop)
    std::atomic<bool> monStop{false}, monIdle{false};

    // ---------------------------------------------------------------------
    void enqueue(AccessUnit&& au) {
        long long dropped = 0;
        const double now = nowMs();
        dg.auIn.fetch_add(1, std::memory_order_relaxed);
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

    // ---------------------------------------------------------------------
    // Adapter choice: the high-performance GPU (hybrid laptops: the discrete
    // GPU; DWM composes it onto the integrated GPU's panel).  Exception: on a
    // multi-GPU desktop where the window's monitor is driven by another GPU
    // that has its own outputs, render on that GPU (no cross-adapter copy).
    ComPtr<IDXGIAdapter1> chooseAdapter(const char*& why) {
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

    static bool sameLuid(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

    bool createDevice() {
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

    bool initGraphics() { return createDevice() && ren.init(hwnd, dev.Get()); }

    // ---------------------------------------------------------------------
    // Device loss (TDR, driver update/reset, GPU removed or switched): drop
    // every device object and re-create them; the window and UI state stay.
    void loseDevice(const char* why, HRESULT reason) {
        if (deviceLost) return;
        log("D3D11 device lost: %s (reason=0x%08lx); re-creating device, swap chain and decoder", why, reason);
        dropHeld();
        dec.close();
        ren.releaseDevice();
        dxgiMgr.Reset();
        dev.Reset();
        deviceLost = true;
        lostAt = nowMs();
        nextRecoverAt = 0;
        recoverAttempts = 0;
        newPicture = false;
        needPresent = true;
    }

    // Re-feeds the access units since the last key frame into a freshly opened
    // decoder; only the newest picture is kept (no GPU copies for the
    // others).  Returns the number of AUs fed.  A truncated cache feeds only
    // the key frame: the pictures after it lack their references.
    size_t refeedGop() {
        const size_t n = gopTruncated ? std::min<size_t>(gop.size(), 1) : gop.size();
        if (gopTruncated && n) {
            refsLost = true;
            log("GOP since the last IDR exceeds the cache (%zu AUs / %zu MB): only the IDR is re-fed; pictures "
                "until the next IDR lack references",
                gopMaxAUs(), gopMaxBytes() >> 20);
        }
        for (size_t i = 0; i < n; ++i) {
            skipOutput = i + 1 < n;
            feed(gop[i].data.data(), gop[i].data.size(), -1, gop[i].due);
        }
        skipOutput = false;
        return n;
    }
    size_t gopMaxAUs() const { return wd ? kGopMaxAUs : 300; }
    size_t gopMaxBytes() const { return wd ? kGopMaxBytes : 16u << 20; }
    bool refsLost = false;  // AUs lost / decoder restarted without the full GOP since the last IDR

    // True if the device is gone (or was just found removed).
    bool checkDevice() {
        if (deviceLost) return true;
        if (!dev) return false;
        const HRESULT r = dev->GetDeviceRemovedReason();
        if (SUCCEEDED(r)) return false;
        loseDevice("device removed", r);
        return true;
    }

    bool recoverDevice() {
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

    // A live AirPlay stream: AUs arrived within the last 2 s.
    bool streamLive() const {
        const double t = dg.lastAuAt.load(std::memory_order_relaxed);
        return t >= 0 && nowMs() - t < 2000;
    }

    // Monitor / display configuration changed: switch GPU if another one is
    // now the better choice.
    void checkAdapter(const char* trigger) {
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

    bool openDecoder() {
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

    // ---------------------------------------------------------------------
    // A/V sync queue
    void dropHeld() {
        for (auto& h : held) ren.recycle(std::move(h.pic));
        held.clear();
    }

    double heldTarget(const Held& h, double latency) const {
        return std::min(h.due + latency, h.decoded + kSyncMaxHoldMs);
    }

    void showHeld(Held& h, double target) {
        ren.show(std::move(h.pic));
        pictureTIn = h.tIn;
        pictureTarget = target;
        newPicture = true;
    }

    // Shows the newest held picture whose time has come (older ones are late:
    // skipped).  Called before every present decision.
    void syncTick() {
        if (held.empty()) return;
        const double now = nowMs(), lat = syncLatencyMs.load();
        size_t n = 0;
        while (n < held.size() && heldTarget(held[n], lat) <= now + kSyncSlackMs) ++n;
        if (!n) return;
        for (size_t i = 0; i + 1 < n; ++i) ren.recycle(std::move(held[i].pic));
        showHeld(held[n - 1], heldTarget(held[n - 1], lat));
        held.erase(held.begin(), held.begin() + n);
    }

    // Milliseconds until the next held picture is due, < 0 if none.
    double syncWaitMs() const {
        if (held.empty()) return -1;
        return std::max(0.0, heldTarget(held.front(), syncLatencyMs.load()) - kSyncSlackMs - nowMs());
    }

    // A decoded picture reached the renderer (shown or held).
    // live: from a newly received AU (re-fed ones do not end a stall).
    void pictureDecoded(double t, bool live) {
        dg.decoded.fetch_add(1, std::memory_order_relaxed);
        if (pendingSince < 0) pendingSince = t;
        ++pendingPictures;
        if (!live) return;
        if (reportDecodeBack) {
            reportDecodeBack = false;
            video::wdlog("pictures decoded again %.0f ms after the decoder restart", t - decRecoverAt);
        }
        starvedSince = -1;
        starvedFeeds = 0;
        if (!refsLost) decRecoveries = 0;  // a stray picture without references ends nothing: the IDR does
    }

    void onDecoded(IMFSample* sample, const video::VideoFormat& fmt) {
        LONGLONG ts = 0;
        sample->GetSampleTime(&ts);
        int64_t s = ts / 166667;
        int slot = static_cast<int>(s % kRing);
        double due = ringDue[slot];
        const double tIn = ringIn[slot];
        const uint64_t pts = ringPts[slot];
        if (skipOutput) return;
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
        if (syncActive && due != kNoTime) {
            Held h;
            if (!ren.hold(sample, fmt, h.pic)) {  // waits for GPU completion
                checkDevice();
                return;
            }
            h.due = due;
            h.tIn = tIn;
            h.decoded = nowMs();
            if (tap) tapPicture(&h.pic, pts);
            if (held.size() >= kSyncMaxHeld) {  // more than the cap: show the oldest now
                showHeld(held.front(), kNoTime);
                held.pop_front();
            }
            held.push_back(std::move(h));
        } else {
            dropHeld();  // older than this one
            if (!ren.upload(sample, fmt)) {  // waits for GPU completion
                checkDevice();
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

    // ---------------------------------------------------------------------
    // BGRA pictures (submitBgraFrame): shown ASAP, like a decoded picture.
    void takeBgra() {
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

    void showBgra(double tIn, uint64_t pts) {
        if (bgraCur.empty()) return;
        dropHeld();
        if (!ren.uploadBgra(bgraCur.data(), bgraCurW, bgraCurH, bgraCurW * 4)) {
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

    // Source ended (onReset): free the last picture.  The mailbox is
    // cleared by onReset() itself, so a frame submitted after it survives.
    void dropBgra() {
        bgraCur = {};
        bgraLive = false;
    }

    // Frame tap pts: ntpLocalNs when plausible (within 30 s of now on either
    // clock), else the arrival time on localTimeNs()'s clock (UTC wall clock;
    // QPC if the stamps were found on QPC).
    uint64_t tapPts(uint64_t ntpLocalNs) const {
        const int64_t t = static_cast<int64_t>(ntpLocalNs);
        const int64_t q = qpcNs(), u = utcNs();
        if (ntpLocalNs && (std::llabs(t - q) < 30'000'000'000LL || std::llabs(t - u) < 30'000'000'000LL))
            return ntpLocalNs;
        return static_cast<uint64_t>(ntpClock.load() == 1 ? q : u);
    }

    HRESULT feed(const uint8_t* p, size_t n, double tIn, double due, uint64_t pts = 0) {
        ++seq;
        int slot = static_cast<int>(seq % kRing);
        ringIn[slot] = tIn;
        ringDue[slot] = due;
        ringPts[slot] = pts;
        ringSubmit[slot] = nowMs();
        const long long out0 = dec.counters().outputs;
        const HRESULT hr =
            dec.decode(p, n, seq * 166667, [this](IMFSample* s, const video::VideoFormat& f) { onDecoded(s, f); });
        dg.mftOut.fetch_add(dec.counters().outputs - out0, std::memory_order_relaxed);
        if (FAILED(hr)) dg.lastDecodeHr.store(hr, std::memory_order_relaxed);
        return hr;
    }

    void remember(AccessUnit& au) {
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

    void decodeOne(AccessUnit& au) {
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
                    for (size_t i = 0; i < n; ++i) feed(gop[i].data.data(), gop[i].data.size(), -1, kNoTime);
                    feed(au.data.data(), au.data.size(), au.tIn, au.due, au.pts);
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

    // ---------------------------------------------------------------------
    // Frame tap
    void tapPicture(const video::Renderer::Picture* pic, uint64_t pts) {
        // Keep a ring slot free (several pictures in one decode batch); the
        // usual delivery happens after Present (off the latency path).
        if (ren.tapPending() >= video::Renderer::kTapRing - 1) deliverTaps(false);
        if (!ren.tapSubmit(pic, pts)) checkDevice();
    }

    void deliverTaps(bool all) {
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

    void workerMain(std::promise<bool>& ready) {
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
            if (snapReq) SetEvent(snapReq->done);
            snapReq.reset();
        }
        dec.close();
        ren.shutdown();
        dxgiMgr.Reset();
        dev.Reset();
        MFShutdown();
        CoUninitialize();
    }

    bool pendingLocked() const {
        return stop || !queue.empty() || dirty || resizePending || resetPending || codecChanged || pausePending >= 0 ||
               pokeReq || snapReq || syncChanged || adapterCheckReq || testReq >= 0 || themeReq >= 0 || dimReq >= 0 ||
               frameReq >= 0 || recReq >= 0 || mascotHoverReq >= 0 || xformChanged || mascotClickReq || tapChanged ||
               bgraNew.load() || hintsReq || actionsReq || linkHoverReq != -2 || toolbarReq || toolActivityReq ||
               toolLeaveReq || toolHoverReq != -2 || viewChanged || frozenReq >= 0 || originalReq >= 0 || boxesReq ||
               busyReq || selChanged;
    }

    // Takes everything queued plus control flags; returns false on stop.
    // waitMs: 0 = don't wait, < 0 = until something happens, else at most
    // that long (animation frame / held picture due / recovery retry).
    bool take(std::deque<AccessUnit>& out, double waitMs) {
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
        LPARAM testLp = 0;
        bool xform = false, mClick = false, tapCh = false;
        UINT w = 0, h = 0, dpi = 0;
        VideoCodec c;
        std::optional<std::wstring> connecting, pin, toast;
        double toastHold = 0;
        std::optional<std::vector<video::Renderer::ToolItem>> tools;
        bool toolAct = false, toolLeave = false, toolInside = false;
        int toolHover = -2;
        std::optional<std::vector<video::Renderer::Action>> actions;
        std::optional<std::vector<std::wstring>> hints;
        int linkHover = -2;
        std::vector<video::Renderer::Option> opts;
        std::shared_ptr<SnapshotRequest> snap;
        std::optional<video::Renderer::View> view;
        int frozenR = -1, originalR = -1;
        std::optional<std::vector<video::Renderer::TextBox>> boxes;
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
            toolLeave = std::exchange(toolLeaveReq, false);
            toolHover = std::exchange(toolHoverReq, -2);
            toolInside = toolInsideReq;
            snap = std::exchange(snapReq, nullptr);
            syncCh = std::exchange(syncChanged, false);
            adapterCh = std::exchange(adapterCheckReq, false);
            test = std::exchange(testReq, -1);
            testLp = testArg;
            theme = std::exchange(themeReq, -1);
            dim = std::exchange(dimReq, -1);
            frame = std::exchange(frameReq, -1);
            rec = std::exchange(recReq, -1);
            mHover = std::exchange(mascotHoverReq, -1);
            xform = std::exchange(xformChanged, false);
            mClick = std::exchange(mascotClickReq, false);
            tapCh = std::exchange(tapChanged, false);
            hints = std::exchange(hintsReq, std::nullopt);
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
            if (test <= 3) loseDevice(kTests[test], DXGI_ERROR_DEVICE_REMOVED);
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
        if (tapCh && !tapOn.load()) ren.tapReset();
        if ((codecCh || reset) && tapOn.load()) deliverTaps(true);  // the last pictures of the stream
        if (theme >= 0) {
            ren.setTheme(theme);
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
        if (poke) {
            const bool wasIdle = ren.nextFrameInMs() < 0;
            ren.poke();
            if (wasIdle) paint = true;
        }
        if (view) {
            ren.setView(*view);
            paint = true;
        }
        if (frozenR >= 0) {
            ren.setFrozen(frozenR == 1);
            paint = true;
        }
        if (boxes) {
            ren.setTextOverlay(std::move(*boxes));
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
        if (snap) {
            snap->ok = snap->ui     ? ren.renderCapture(snap->pixels, snap->w, snap->h)
                       : snap->grab ? ren.grab(snap->pixels, snap->w, snap->h, false)
                                    : ren.snapshot(snap->pixels, snap->w, snap->h, snap->framed);
            SetEvent(snap->done);
        }
        if (paint) needPresent = true;
        return true;
    }

    bool needPresent = true;
    bool snapshot(const std::wstring& pngPath, bool framed, bool ui = false);
    std::shared_ptr<SnapshotRequest> runSnapshot(bool framed, bool ui, bool grab);

    void publishOptionRects() {
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

    // Earliest of two "ms until" values where < 0 means "never".
    static double earliest(double a, double b) {
        if (a < 0) return b;
        if (b < 0) return a;
        return std::min(a, b);
    }

    void decodeBatch(std::deque<AccessUnit>& batch) {
        if (refsDropped.exchange(false)) refsLost = true;
        for (auto& au : batch) decodeOne(au);
        batch.clear();
        if (!deviceLost) checkDevice();
        syncTick();
        setStage(StOther);
    }

    // ---------------------------------------------------------------------
    // Fault injection (kTestMsg wParam 20..26, pm_video_test --test-at T:C:ARG).
    void injectFault(int code, LPARAM arg) {
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

    // Watchdog (a): AUs are fed but no picture comes out.  Restarts the
    // decoder and re-feeds the GOP cache (2nd time within a minute: in
    // software).  Without the references (AUs lost, cache truncated) a
    // restart cannot help: logged once, the next IDR resumes the picture.
    void watchdogDecode() {
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

    // Watchdog (b): pictures are decoded but none reaches the screen (Present
    // not called / failing, or the swap chain no longer releasing frames).
    // Re-creates the swap chain (twice within 10 s: the whole device).
    void watchdogPresent() {
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

    void loop() {
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

    // 0 if a new picture may be presented now, else ms until it may (occluded).
    double occludedWaitMs() const {
        if (!wd || !ren.occluded()) return 0;
        return std::max(0.0, lastPicturePresentAt + kOccludedPresentMs - nowMs());
    }

    // Present bookkeeping: counters, occlusion transitions, watchdog state.
    void notePresent(bool picture) {
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


    // ---------------------------------------------------------------------
    // Monitor thread: one summary line per 5 s while AUs arrive, and a
    // warning when the render thread is stuck (it cannot report that
    // itself).  Sleeps on monEvent while no stream is live.
    struct Counts {
        long long in, drop, skip, fed, mft, dec, pres, ok, occl, err, wto;
    };
    Counts counts() const {
        auto l = [](const std::atomic<long long>& a) { return a.load(std::memory_order_relaxed); };
        return {l(dg.auIn),   l(dg.auDropped),  l(dg.auSkipped), l(dg.auFed),      l(dg.mftOut),     l(dg.decoded),
                l(dg.presented), l(dg.presentOk), l(dg.presentOccl), l(dg.presentErr), l(dg.waitTimeouts)};
    }

    static const char* stageName(int s) {
        static const char* const k[] = {"wait", "decode", "frame wait", "Present", "recovery", "frame tap", "other"};
        return s >= 0 && s < 7 ? k[s] : "?";
    }

    void monitorMain() {
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

    void summary(Counts& prev, double& last, double now) {
        const Counts c = counts();
        size_t q;
        {
            std::lock_guard lk(m);
            q = queue.size();
        }
        const double idr = dg.lastIdrAt.load();
        log("%.0fs: AU in %lld drop %lld skip %lld | fed %lld > MFT %lld > pictures %lld (%s) | presented %lld (ok %lld, "
            "occluded %lld, failed %lld, last 0x%08lx; frame-wait timeouts %lld) | queue %zu | last IDR %.1f s ago, GOP %d%s",
            (now - last) / 1000, c.in - prev.in, c.drop - prev.drop, c.skip - prev.skip, c.fed - prev.fed,
            c.mft - prev.mft, c.dec - prev.dec, dg.hw.load() ? "hw" : "sw", c.pres - prev.pres, c.ok - prev.ok,
            c.occl - prev.occl, c.err - prev.err, static_cast<unsigned long>(dg.lastPresentHr.load()),
            c.wto - prev.wto, q, idr >= 0 ? (now - idr) / 1000 : -1.0, dg.gopAUs.load(),
            dg.gopTrunc.load() ? " (truncated)" : "");
        prev = c;
        last = now;
    }

    void startMonitor() {
        if (!monitor.joinable()) monitor = std::thread([this] { monitorMain(); });
    }
    void stopMonitor() {
        if (!monitor.joinable()) return;
        monStop = true;
        SetEvent(monEvent);
        monitor.join();
    }

    void stopWorker() {
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

    void toggleFullscreen() {
        DWORD style = static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE));
        if (!fullscreen) {
            MONITORINFO mi{sizeof(mi)};
            if (!GetWindowPlacement(hwnd, &savedPlacement) ||
                !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
                return;
            SetWindowLongW(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            fullscreen = true;
        } else {
            SetWindowLongW(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
            SetWindowPlacement(hwnd, &savedPlacement);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            fullscreen = false;
        }
    }

    // Index of the idle check box at the mouse position (client pixels), or -1.
    int hitTest(LPARAM lp) {
        const POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        std::lock_guard lk(m);
        for (size_t i = 0; i < optionRects.size(); ++i)
            if (PtInRect(&optionRects[i], pt)) return static_cast<int>(i);
        return -1;
    }

    // Is the mouse position (client pixels) on an opaque part of the mascot?
    bool hitMascot(LPARAM lp) {
        const POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        std::lock_guard lk(m);
        const RECT& r = mascotRect;
        if (r.right <= r.left || r.bottom <= r.top || !PtInRect(&r, pt) || !maskW || !maskH) return false;
        const UINT mx = std::min(maskW - 1, static_cast<UINT>((pt.x - r.left) * static_cast<LONGLONG>(maskW) / (r.right - r.left)));
        const UINT my = std::min(maskH - 1, static_cast<UINT>((pt.y - r.top) * static_cast<LONGLONG>(maskH) / (r.bottom - r.top)));
        return mascotMask[static_cast<size_t>(my) * maskW + mx] != 0;
    }

    void setMascotHot(bool hot) {
        if (hot == mascotHotUi) return;
        mascotHotUi = hot;
        if (hot) SetCursor(LoadCursorW(nullptr, IDC_HAND));
        {
            std::lock_guard lk(m);
            mascotHoverReq = hot ? 1 : 0;
        }
        wake();
    }

    void clickMascot() {
        {
            std::lock_guard lk(m);
            mascotClickReq = true;
        }
        wake();
    }

    void setHover(int i) {
        if (i == hoverUi) return;
        hoverUi = i;
        if (i >= 0) SetCursor(LoadCursorW(nullptr, IDC_HAND));
        {
            std::lock_guard lk(m);
            hover = i;
            hoverChanged = true;
            dirty = true;
        }
        wake();
    }

    void toggleOption(int i) {
        std::function<void(int, bool)> cb;
        int id = 0;
        bool checked = false;
        {
            std::lock_guard lk(m);
            if (i < 0 || static_cast<size_t>(i) >= options.size()) return;
            checked = options[i].checked = !options[i].checked;
            id = options[i].id;
            cb = onToggle;
            optionsChanged = true;
            dirty = true;
        }
        wake();
        if (cb) cb(id, checked);  // UI thread
    }

    void markDirty() {
        {
            std::lock_guard lk(m);
            dirty = true;
        }
        wake();
    }

    void requestAdapterCheck() {
        {
            std::lock_guard lk(m);
            adapterCheckReq = true;
        }
        wake();
    }

    // ---------------------------------------------------------------------
    // Idle actions: pill buttons / links (UI thread)
    // Index of the action at the mouse position, or -1.
    int hitLink(LPARAM lp) {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        std::lock_guard lk(m);
        for (size_t i = 0; i < linkRects.size(); ++i)
            if (PtInRect(&linkRects[i], pt)) return static_cast<int>(i);
        return -1;
    }

    void setLinkHot(int hot) {
        if (hot == linkHotUi) return;
        linkHotUi = hot;
        if (hot >= 0) SetCursor(LoadCursorW(nullptr, IDC_HAND));
        {
            std::lock_guard lk(m);
            linkHoverReq = hot;
        }
        wake();
    }

    void clickLink(int i) {
        std::function<void()> cb;
        {
            std::lock_guard lk(m);
            if (i >= 0 && i < static_cast<int>(actionFns.size())) cb = actionFns[i];
        }
        if (cb) cb();  // UI thread
    }

    // ---------------------------------------------------------------------
    // Live toolbar (UI thread)
    // Button at the mouse position (client pixels), or -1.
    int hitTool(POINT pt) {
        std::lock_guard lk(m);
        for (size_t i = 0; i < toolRects.size(); ++i)
            if (PtInRect(&toolRects[i], pt)) return static_cast<int>(i);
        return -1;
    }
    bool inToolPill(POINT pt) {
        std::lock_guard lk(m);
        return PtInRect(&toolPill, pt) != FALSE;
    }

    void setToolHot(int hot, bool inside) {
        if (hot == toolHotUi && inside == toolInsideUi) return;
        toolHotUi = hot;
        toolInsideUi = inside;
        if (hot >= 0) SetCursor(LoadCursorW(nullptr, IDC_HAND));
        {
            std::lock_guard lk(m);
            toolHoverReq = hot;
            toolInsideReq = inside;
        }
        wake();
    }

    // Mouse moved: (re)show the toolbar for ~2 s (requests at most every 200 ms).
    void toolActivity() {
        const double now = nowMs();
        if (now - lastToolPokeUi < 200) return;
        lastToolPokeUi = now;
        {
            std::lock_guard lk(m);
            if (toolIds.empty()) return;
            toolActivityReq = true;
        }
        wake();
    }

    void toolLeave() {
        lastToolPokeUi = -1e9;
        {
            std::lock_guard lk(m);
            toolLeaveReq = true;
        }
        wake();
    }

    void clickTool(int i) {
        std::function<void(int)> cb;
        int id = 0;
        {
            std::lock_guard lk(m);
            if (i < 0 || i >= static_cast<int>(toolIds.size())) return;
            id = toolIds[i];
            cb = toolFn;
        }
        if (cb) cb(id);  // UI thread
    }

    // Left press: on the toolbar pill (not during a drag for the phone) it is
    // the toolbar's (never forwarded); the click happens on release.
    bool toolPress(LPARAM lp) {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (buttonsDown || !inToolPill(pt)) return false;
        pressedTool = hitTool(pt);
        return true;
    }

    // ---------------------------------------------------------------------
    // Remote control (UI thread)
    using PointerEvent = VideoWindow::PointerEvent;

    std::shared_ptr<std::function<void(const PointerEvent&)>> pointerHandler() {
        std::lock_guard lk(m);
        return pointerFn;
    }

    std::shared_ptr<std::function<void(unsigned, bool, wchar_t)>> keyHandler() {
        std::lock_guard lk(m);
        return keyFn;
    }

    // Client pixel -> normalised picture position, undoing letterbox /
    // device frame, rotation and mirroring.  Outside the picture: false,
    // unless clamp (drags that leave the picture stick to its edge).
    bool mapPoint(POINT pt, float& x, float& y, bool clamp) {
        RECT r;
        int rot;
        bool mir;
        video::Renderer::View v;
        {
            std::lock_guard lk(m);
            r = picRect;
            rot = picRot;
            mir = picMirror;
            v = pubView;
        }
        if (r.right <= r.left || r.bottom <= r.top) return false;
        if (!clamp && (pt.x < r.left || pt.x >= r.right || pt.y < r.top || pt.y >= r.bottom)) return false;
        // Pixel centres: the first column maps to 0.5 / width, never 0.
        const float vx = std::clamp((pt.x + 0.5f - r.left) / (r.right - r.left), 0.f, 1.f);
        const float vy = std::clamp((pt.y + 0.5f - r.top) / (r.bottom - r.top), 0.f, 1.f);
        // Magnifier: viewport -> displayed picture.
        const float tx = v.cx + (vx - 0.5f) / v.zoom, ty = v.cy + (vy - 0.5f) / v.zoom;
        video::Renderer::screenToPicture(rot, mir, tx, ty, x, y);
        x = std::clamp(x, 0.f, 1.f);
        y = std::clamp(y, 0.f, 1.f);
        return true;
    }

    bool overPicture() {
        if (!pointerHandler()) return false;
        POINT pt;
        float x, y;
        return GetCursorPos(&pt) && ScreenToClient(hwnd, &pt) && mapPoint(pt, x, y, false);
    }

    void sendPointer(const std::shared_ptr<std::function<void(const PointerEvent&)>>& fn, PointerEvent::Kind kind,
                     float x, float y, int button) {
        PointerEvent e;
        e.kind = kind;
        e.x = lastPx = x;
        e.y = lastPy = y;
        e.button = button;
        if (fn && *fn) (*fn)(e);
    }

    static int lowestButton(int mask) { return mask & 1 ? 0 : mask & 2 ? 1 : 2; }

    // All forwarded buttons go up at the last position (capture lost, picture
    // gone, handler removed).
    void cancelButtons() {
        if (!buttonsDown) return;
        auto fn = pointerHandler();
        const int mask = std::exchange(buttonsDown, 0);
        for (int b = 0; b < 3; ++b)
            if (mask & (1 << b)) sendPointer(fn, PointerEvent::Kind::Up, lastPx, lastPy, b);
        if (GetCapture() == hwnd) ReleaseCapture();
    }

    // True if the press went to the pointer handler.
    bool pointerDown(int button, LPARAM lp) {
        auto fn = pointerHandler();
        float x, y;
        if (!fn || !mapPoint({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, x, y, false)) return false;
        if (!buttonsDown) SetCapture(hwnd);  // drags keep reporting outside the window
        buttonsDown |= 1 << button;
        hoverPending = false;
        sendPointer(fn, PointerEvent::Kind::Down, x, y, button);
        return true;
    }

    bool pointerUp(int button, LPARAM lp) {
        if (!(buttonsDown & (1 << button))) return false;
        buttonsDown &= ~(1 << button);
        auto fn = pointerHandler();
        float x = lastPx, y = lastPy;
        mapPoint({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, x, y, true);
        sendPointer(fn, PointerEvent::Kind::Up, x, y, button);
        if (!buttonsDown && GetCapture() == hwnd) ReleaseCapture();  // WM_CAPTURECHANGED: nothing left to cancel
        return true;
    }

    // Moves: every one while a button is down (drag), hover moves over the
    // picture at most ~60 Hz (the last one is delivered by kHoverTimer).
    bool pointerMove(LPARAM lp) {
        auto fn = pointerHandler();
        if (!fn) {
            if (buttonsDown) {
                buttonsDown = 0;
                if (GetCapture() == hwnd) ReleaseCapture();
            }
            return false;
        }
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        float x, y;
        if (buttonsDown) {
            if (!mapPoint(pt, x, y, true)) cancelButtons();  // the picture went away mid-drag
            else sendPointer(fn, PointerEvent::Kind::Move, x, y, lowestButton(buttonsDown));
            return true;
        }
        if (!mapPoint(pt, x, y, false)) {
            hoverPending = false;
            return false;
        }
        const double now = nowMs();
        if (now - lastHoverUi >= kHoverMinMs) {
            lastHoverUi = now;
            hoverPending = false;
            sendPointer(fn, PointerEvent::Kind::Move, x, y, -1);
        } else {
            hoverLp = lp;
            if (!hoverPending)
                SetTimer(hwnd, kHoverTimer, static_cast<UINT>(std::ceil(kHoverMinMs - (now - lastHoverUi))), nullptr);
            hoverPending = true;
        }
        return true;
    }

    bool pointerWheel(UINT msg, WPARAM wp, LPARAM lp) {
        auto fn = pointerHandler();
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};  // screen coordinates
        float x, y;
        if (!fn || !ScreenToClient(hwnd, &pt) || !mapPoint(pt, x, y, false)) return false;
        PointerEvent e;
        e.kind = PointerEvent::Kind::Wheel;
        e.x = x;
        e.y = y;
        // Notches: +1 = one detent up (away from the user) / to the right.
        const float notches = GET_WHEEL_DELTA_WPARAM(wp) / static_cast<float>(WHEEL_DELTA);
        (msg == WM_MOUSEWHEEL ? e.wheelY : e.wheelX) = notches;
        (*fn)(e);
        return true;
    }

    // Keyboard: forwarded only without Ctrl / Alt (those are the app's
    // shortcuts), except Ctrl+C / V / X / A / Z.  F11 and Esc in fullscreen
    // stay with the window; Ctrl / Alt / Win themselves are not forwarded.
    bool keyEvent(UINT msg, WPARAM wp) {
        auto fn = keyHandler();
        const unsigned vk = static_cast<unsigned>(wp) & 0xff;
        if (msg == WM_KEYUP) {
            if (!keysDown.test(vk)) return false;
            keysDown.reset(vk);
            if (fn && *fn) (*fn)(vk, false, 0);
            return true;
        }
        if (!fn) return false;
        switch (vk) {
        case VK_F11:
        case VK_PROCESSKEY:  // IME composition: the result arrives as WM_CHAR
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU: case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN:
            return false;
        case VK_ESCAPE:
            if (fullscreen) return false;
            break;
        }
        const bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
        if (alt) return false;  // incl. AltGr (Ctrl+Alt): its character still comes as WM_CHAR
        if (ctrl && vk != 'C' && vk != 'V' && vk != 'X' && vk != 'A' && vk != 'Z') return false;
        // The character TranslateMessage queued for this key (dead keys: none).
        wchar_t ch = 0;
        MSG cm;
        if (PeekMessageW(&cm, hwnd, WM_CHAR, WM_DEADCHAR, PM_REMOVE) && cm.message == WM_CHAR)
            ch = static_cast<wchar_t>(cm.wParam);
        if (ctrl || ch < 0x20 || ch == 0x7f) ch = 0;  // control characters: the vk says it
        keysDown.set(vk);
        (*fn)(vk, true, ch);
        return true;
    }

    // Text without a forwarded key: IME results, dead-key compositions,
    // AltGr characters, the second half of a surrogate pair.
    bool charEvent(WPARAM wp) {
        auto fn = keyHandler();
        const wchar_t ch = static_cast<wchar_t>(wp);
        if (!fn || ch < 0x20 || ch == 0x7f) return false;
        if (GetKeyState(VK_MENU) < 0 && GetKeyState(VK_CONTROL) >= 0) return false;  // Alt+numpad etc. stay
        (*fn)(0, true, ch);
        return true;
    }

    void releaseKeys() {
        if (keysDown.none()) return;
        auto fn = keyHandler();
        for (unsigned vk = 0; vk < 256; ++vk)
            if (keysDown.test(vk) && fn && *fn) (*fn)(vk, false, 0);
        keysDown.reset();
    }

    // ---------------------------------------------------------------------
    // Magnifier / region selection (UI thread unless noted)
    bool panning = false;          // left drag pans the magnified picture
    POINT panLast{};
    bool selecting = false;        // beginRegionSelect() active
    bool selDragging = false;
    float selV[4] = {-1, -1, -1, -1};  // viewport coords of the drag
    std::function<void(bool, float, float, float, float)> selDone;

    // Any thread: a new view (clamped) for the worker; user: reported to viewFn.
    void applyView(video::Renderer::View v, bool user) {
        v = video::Renderer::clampView(v);
        std::shared_ptr<std::function<void(const VideoWindow::ViewState&)>> fn;
        VideoWindow::ViewState vs;
        {
            std::lock_guard lk(m);
            viewUi = v;
            viewChanged = true;
            fn = viewFn;
            vs = viewStateLocked();
        }
        wake();
        if (user && fn && *fn) (*fn)(vs);
    }
    VideoWindow::ViewState viewStateLocked() const {
        VideoWindow::ViewState vs;
        vs.zoom = viewUi.zoom;
        vs.centerX = viewUi.cx;
        vs.centerY = viewUi.cy;
        vs.filter = static_cast<VideoWindow::Filter>(viewUi.filter);
        vs.frozen = frozenUi;
        return vs;
    }
    video::Renderer::View currentView() {
        std::lock_guard lk(m);
        return viewUi;
    }
    // Zoom keeping viewport point (vx, vy) where it is.
    void zoomAtV(float zoom, float vx, float vy, bool user) {
        video::Renderer::View v = currentView();
        zoom = std::clamp(zoom, 1.f, video::Renderer::kMaxZoom);
        if (std::fabs(zoom - 1) < 0.06f) zoom = 1;
        // The picture point under (vx, vy) stays: c' = c + (v - .5) * (1/z - 1/z').
        v.cx += (vx - 0.5f) * (1 / v.zoom - 1 / zoom);
        v.cy += (vy - 0.5f) * (1 / v.zoom - 1 / zoom);
        v.zoom = zoom;
        applyView(v, user);
    }
    void panV(float dx, float dy, bool user) {
        video::Renderer::View v = currentView();
        if (v.zoom <= 1.001f) return;
        v.cx += dx / v.zoom;
        v.cy += dy / v.zoom;
        applyView(v, user);
    }
    static float stepZoom(float z, int steps) {
        z *= std::pow(1.25f, static_cast<float>(steps));
        if (std::fabs(z - 1) < 0.06f) z = 1;
        return z;
    }
    // Client pixel -> viewport coords of the live picture (false: not on it).
    bool viewportPoint(POINT pt, float& vx, float& vy, bool clamp) {
        RECT r;
        {
            std::lock_guard lk(m);
            r = picRect;
        }
        if (r.right <= r.left || r.bottom <= r.top) return false;
        if (!clamp && (pt.x < r.left || pt.x >= r.right || pt.y < r.top || pt.y >= r.bottom)) return false;
        vx = std::clamp((pt.x + 0.5f - r.left) / (r.right - r.left), 0.f, 1.f);
        vy = std::clamp((pt.y + 0.5f - r.top) / (r.bottom - r.top), 0.f, 1.f);
        return true;
    }
    void viewportToContent(float vx, float vy, float& dx, float& dy) {
        video::Renderer::View v;
        bool mir;
        {
            std::lock_guard lk(m);
            v = pubView;
            mir = picMirror;
        }
        const float tx = v.cx + (vx - 0.5f) / v.zoom, ty = v.cy + (vy - 0.5f) / v.zoom;
        dx = std::clamp(mir ? 1 - tx : tx, 0.f, 1.f);
        dy = std::clamp(ty, 0.f, 1.f);
    }

    // Wheel: Ctrl = zoom at the cursor; zoomed without a pointer handler =
    // pan (Shift / tilt: sideways).
    bool magWheel(UINT msg, WPARAM wp, LPARAM lp) {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        float vx, vy;
        if (!ScreenToClient(hwnd, &pt) || !viewportPoint(pt, vx, vy, false)) return false;
        const float notches = GET_WHEEL_DELTA_WPARAM(wp) / static_cast<float>(WHEEL_DELTA);
        const bool ctrl = (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) != 0;
        if (ctrl && msg == WM_MOUSEWHEEL) {
            const float z = currentView().zoom * std::pow(1.25f, notches);
            zoomAtV(z, vx, vy, true);
            return true;
        }
        if (currentView().zoom <= 1.001f || pointerHandler()) return false;
        const bool side = msg == WM_MOUSEHWHEEL || (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) != 0;
        const float step = 0.15f * notches;
        if (msg == WM_MOUSEHWHEEL) panV(step, 0, true);
        else if (side) panV(-step, 0, true);
        else panV(0, -step, true);
        return true;
    }

    // Left press: start panning (zoomed; Ctrl needed with a pointer handler).
    bool panPress(LPARAM lp) {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        float vx, vy;
        if (buttonsDown || currentView().zoom <= 1.001f || !viewportPoint(pt, vx, vy, false) || inToolPill(pt))
            return false;
        if (pointerHandler() && GetKeyState(VK_CONTROL) >= 0) return false;
        panning = true;
        panLast = pt;
        SetCapture(hwnd);
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        return true;
    }
    void panMove(LPARAM lp) {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT r;
        {
            std::lock_guard lk(m);
            r = picRect;
        }
        if (r.right > r.left && r.bottom > r.top && (pt.x != panLast.x || pt.y != panLast.y))
            panV(-static_cast<float>(pt.x - panLast.x) / (r.right - r.left),
                 -static_cast<float>(pt.y - panLast.y) / (r.bottom - r.top), true);
        panLast = pt;
    }
    void panEnd() {
        if (!panning) return;
        panning = false;
        if (GetCapture() == hwnd) ReleaseCapture();
    }

    // Keys: Ctrl+= / Ctrl+- zoom, Ctrl+Shift+0 1x, arrows pan while zoomed.
    bool magKey(UINT msg, WPARAM wp) {
        const unsigned vk = static_cast<unsigned>(wp) & 0xff;
        const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        const bool alt = msg == WM_SYSKEYDOWN || GetKeyState(VK_MENU) < 0;
        if (ctrl && !alt && (vk == VK_OEM_PLUS || vk == VK_ADD)) {
            const auto v = currentView();
            zoomAtV(stepZoom(v.zoom, 1), 0.5f, 0.5f, true);
            return true;
        }
        if (ctrl && !alt && (vk == VK_OEM_MINUS || vk == VK_SUBTRACT)) {
            const auto v = currentView();
            zoomAtV(stepZoom(v.zoom, -1), 0.5f, 0.5f, true);
            return true;
        }
        if (ctrl && shift && !alt && (vk == '0' || vk == VK_NUMPAD0)) {
            auto v = currentView();
            v.zoom = 1;
            applyView(v, true);
            return true;
        }
        if (ctrl || currentView().zoom <= 1.001f) return false;
        float dx = 0, dy = 0;
        switch (vk) {
        case VK_LEFT: dx = -0.1f; break;
        case VK_RIGHT: dx = 0.1f; break;
        case VK_UP: dy = -0.1f; break;
        case VK_DOWN: dy = 0.1f; break;
        default: return false;
        }
        panV(dx, dy, true);
        return true;
    }

    // ---- Text overlay list panel / markers (UI thread) ----
    static constexpr int kOvNone = -3, kOvZoom = -2, kOvPanel = -4;
    int ovHotUi = -1;           // list item under the cursor
    int ovPress = kOvNone;      // where the left button went down
    int ovOnUi = kOvNone;       // what the cursor is on
    // Item (row or marker; *marker tells which), kOvZoom, kOvPanel or kOvNone at pt.
    int ovHit(POINT pt, bool* marker = nullptr) {
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
    bool hitIsMarker(POINT pt) {
        bool marker = false;
        ovHit(pt, &marker);
        return marker;
    }
    void ovSetHot(int i) {
        if (i == ovHotUi) return;
        ovHotUi = i;
        {
            std::lock_guard lk(m);
            ovHotReq = i;
        }
        wake();
    }
    void ovClick(int hit, bool marker) {
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

    // ---- Region selection (beginRegionSelect) ----
    void sendSelection() {
        {
            std::lock_guard lk(m);
            selChanged = true;
            selActiveReq = selecting;
            std::copy(std::begin(selV), std::end(selV), selReq);
        }
        wake();
    }
    void startSelect(std::function<void(bool, float, float, float, float)> done) {
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
    void endSelect(bool ok) {
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
    // Mouse in selection mode; true = handled.
    bool selectMouse(UINT msg, LPARAM lp) {
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

    LRESULT wndProc(UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == kCancelSelectMsg && kCancelSelectMsg) {
            endSelect(false);
            return 0;
        }
        static const UINT runOnUiMsg = RegisterWindowMessageW(L"PhoneMirror.Video.RunOnUi");
        if (msg == runOnUiMsg && runOnUiMsg && lp) {
            std::unique_ptr<std::function<void()>> fn(reinterpret_cast<std::function<void()>*>(lp));
            (*fn)();
            return 0;
        }
        if (msg == kTestMsg && kTestMsg && wp == 10) {
            // Test hook: centre of idle action lp (0 = first; the help link
            // when it is the only one) in client px, -1 if not shown.
            std::lock_guard lk(m);
            const size_t i = static_cast<size_t>(lp);
            if (i >= linkRects.size() || linkRects[i].right <= linkRects[i].left) return -1;
            const RECT& r = linkRects[i];
            return MAKELRESULT((r.left + r.right) / 2, (r.top + r.bottom) / 2);
        }
        if (msg == kTestMsg && kTestMsg && wp == 13) {
            // Test hook: a point on the mascot's cloud (its middle) in client
            // px, -1 if it cannot be clicked right now.
            std::lock_guard lk(m);
            const RECT& r = mascotRect;
            if (r.right <= r.left || r.bottom <= r.top) return -1;
            return MAKELRESULT(r.left + (r.right - r.left) * 43 / 100, r.top + (r.bottom - r.top) * 55 / 100);
        }
        if (msg == kTestMsg && kTestMsg && wp == 12) {
            // Test hook: lp = 1: posted mouse messages only (scripted
            // screenshots without moving the real cursor): no leave tracking.
            synthMouse = lp != 0;
            return 0;
        }
        if (msg == kTestMsg && kTestMsg && wp >= 14 && wp <= 16) {
            // Test hook: centre (client px) of translation list row lp (14),
            // marker lp (15) or the 放大這一塊 button (16); -1 if not shown.
            std::lock_guard lk(m);
            const RECT* r = nullptr;
            if (wp == 16) r = &ovHits.zoomBtn;
            else
                for (const auto& [rr, i] : wp == 14 ? ovHits.rows : ovHits.markers)
                    if (i == static_cast<int>(lp)) r = &rr;
            if (!r || r->right <= r->left) return -1;
            return MAKELRESULT((r->left + r->right) / 2, (r->top + r->bottom) / 2);
        }
        if (msg == kTestMsg && kTestMsg && wp == 11) {
            // Test hook: centre of live-toolbar button lp in client px, -1
            // if the toolbar is not up.
            std::lock_guard lk(m);
            const size_t i = static_cast<size_t>(lp);
            if (i >= toolRects.size()) return -1;
            const RECT& r = toolRects[i];
            return MAKELRESULT((r.left + r.right) / 2, (r.top + r.bottom) / 2);
        }
        if (msg == kTestMsg && kTestMsg) {
            // Test hook (pm_video_test --device-loss-at / --gpu-switch-at):
            // 0 simulated device removal, 1 power-saving GPU, 2 WARP, 3 automatic.
            {
                std::lock_guard lk(m);
                testReq = static_cast<int>(wp);
                testArg = lp;
            }
            wake();
            return 0;
        }
        switch (msg) {
        case WM_SIZE: {
            {
                std::lock_guard lk(m);
                pendW = LOWORD(lp);
                pendH = HIWORD(lp);
                resizePending = true;
            }
            wake();
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            markDirty();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE: {
            if (!trackingMouse && !synthMouse) {
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
                trackingMouse = TrackMouseEvent(&t) != FALSE;
            }
            if (selecting) {
                selectMouse(msg, lp);
                return 0;
            }
            if (panning) {
                panMove(lp);
                return 0;
            }
            // Live toolbar: shown by any movement; on it, nothing goes to the phone.
            toolActivity();
            const POINT mpt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const bool onTool = !buttonsDown && inToolPill(mpt);
            setToolHot(onTool ? hitTool(mpt) : -1, onTool);
            if (onTool) hoverPending = false;
            // Translation list / markers: theirs, nothing goes to the phone.
            ovOnUi = onTool || buttonsDown ? kOvNone : ovHit(mpt);
            ovSetHot(ovOnUi >= 0 ? ovOnUi : -1);
            if (ovOnUi != kOvNone) {
                hoverPending = false;
                setHover(-1);
                setLinkHot(-1);
                return 0;
            }
            const bool remote = onTool || pointerMove(lp);
            const int opt = remote ? -1 : hitTest(lp);
            setHover(opt);
            const int link = opt < 0 && !remote ? hitLink(lp) : -1;
            setLinkHot(link);
            setMascotHot(opt < 0 && link < 0 && !remote && hitMascot(lp));
            // Activity restarts the idle animation (at most once a second).
            const double now = nowMs();
            if (now - lastPokeUi > 1000) {
                lastPokeUi = now;
                {
                    std::lock_guard lk(m);
                    pokeReq = true;
                }
                wake();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            trackingMouse = false;
            ovSetHot(-1);
            ovOnUi = kOvNone;
            setToolHot(-1, false);
            toolLeave();
            setHover(-1);
            setLinkHot(-1);
            setMascotHot(false);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && (selecting || panning)) {
                SetCursor(LoadCursorW(nullptr, selecting ? IDC_CROSS : IDC_SIZEALL));
                return TRUE;
            }
            if (LOWORD(lp) == HTCLIENT && (hoverUi >= 0 || mascotHotUi || linkHotUi >= 0 || toolHotUi >= 0)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            if (LOWORD(lp) == HTCLIENT && ovOnUi != kOvNone) {
                SetCursor(LoadCursorW(nullptr, ovOnUi == kOvPanel ? IDC_ARROW : IDC_HAND));
                return TRUE;
            }
            if (LOWORD(lp) == HTCLIENT && toolInsideUi) {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            if (LOWORD(lp) == HTCLIENT && touchCursor && overPicture()) {
                SetCursor(touchCursor);
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN:
            if (selectMouse(msg, lp)) return 0;
            if (toolPress(lp)) return 0;
            if (!buttonsDown && (ovPress = ovHit({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) != kOvNone) return 0;
            if (panPress(lp)) return 0;
            if (pointerDown(0, lp)) return 0;
            pressedUi = hitTest(lp);
            pressedLink = pressedUi < 0 ? hitLink(lp) : -1;
            pressedMascot = pressedUi < 0 && pressedLink < 0 && hitMascot(lp);
            return 0;
        case WM_LBUTTONUP: {
            if (selectMouse(msg, lp)) return 0;
            if (ovPress != kOvNone) {  // pressed on the list / a marker: a click if released on the same thing
                const int p = std::exchange(ovPress, kOvNone);
                bool marker = false;
                if (ovHit({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, &marker) == p) ovClick(p, marker);
                return 0;
            }
            if (panning) {
                panEnd();
                return 0;
            }
            if (pressedTool != -2) {  // pressed on the toolbar: a click if released on the same button
                const int i = std::exchange(pressedTool, -2);
                if (i >= 0 && hitTool({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}) == i) clickTool(i);
                return 0;
            }
            if (pointerUp(0, lp)) return 0;
            const int i = hitTest(lp);
            if (i >= 0 && i == pressedUi) toggleOption(i);
            else if (i < 0 && pressedLink >= 0 && hitLink(lp) == pressedLink) clickLink(pressedLink);
            else if (i < 0 && pressedMascot && hitMascot(lp)) clickMascot();
            pressedUi = -1;
            pressedLink = -1;
            pressedMascot = false;
            return 0;
        }
        case WM_LBUTTONDBLCLK:
            if (selectMouse(msg, lp)) return 0;
            if (toolPress(lp)) return 0;  // second click of a fast double click on the toolbar
            if (!buttonsDown && (ovPress = ovHit({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) != kOvNone) return 0;
            if (panPress(lp)) return 0;   // magnified: a fast second drag, not fullscreen
            // On the picture with a pointer handler: a press for the phone.
            if (pointerDown(0, lp)) return 0;
            // Second click of a fast double click on a check box, the help
            // link or the mascot: a click, not fullscreen.
            pressedUi = hitTest(lp);
            pressedLink = pressedUi < 0 ? hitLink(lp) : -1;
            pressedMascot = pressedUi < 0 && pressedLink < 0 && hitMascot(lp);
            if (pressedUi < 0 && pressedLink < 0 && !pressedMascot) toggleFullscreen();
            return 0;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
            if (selectMouse(WM_RBUTTONDOWN, lp)) return 0;
            // Shift+right click keeps the window's context menu on the picture;
            // so does a right click on the toolbar (never sent to the phone).
            if (!buttonsDown && inToolPill({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) break;
            if (!buttonsDown && ovHit({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}) != kOvNone) break;  // the window's menu
            if (GetKeyState(VK_SHIFT) >= 0 && pointerDown(1, lp)) return 0;
            break;
        case WM_RBUTTONUP:
            if (selectMouse(msg, lp)) return 0;
            if (pointerUp(1, lp)) return 0;  // no WM_CONTEXTMENU for a click that went to the phone
            break;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK:
            if (selectMouse(WM_MBUTTONDOWN, lp)) return 0;
            if (!buttonsDown && inToolPill({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) return 0;
            if (pointerDown(2, lp)) return 0;
            break;
        case WM_MBUTTONUP:
            if (pointerUp(2, lp)) return 0;
            break;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            POINT wpt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (ScreenToClient(hwnd, &wpt) && inToolPill(wpt)) return 0;
            if (selecting) return 0;
            if (const int oh = ovHit(wpt); oh != kOvNone && !hitIsMarker(wpt)) {  // scrolls the translation list
                {
                    std::lock_guard lk(m);
                    ovScrollReq -= GET_WHEEL_DELTA_WPARAM(wp) / static_cast<float>(WHEEL_DELTA) * 60.f;
                }
                wake();
                return 0;
            }
            if (magWheel(msg, wp, lp)) return 0;
            if (pointerWheel(msg, wp, lp)) return 0;
            break;
        }
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lp) != hwnd) {
                cancelButtons();
                panning = false;
                if (selecting && selDragging) endSelect(false);
            }
            break;
        case WM_TIMER:
            if (wp == kHoverTimer) {
                KillTimer(hwnd, kHoverTimer);
                if (std::exchange(hoverPending, false) && !buttonsDown) {
                    lastHoverUi = -1e9;
                    pointerMove(hoverLp);
                }
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (selecting && wp == VK_ESCAPE) {
                endSelect(false);
                return 0;
            }
            if (keyEvent(msg, wp)) return 0;
            if (magKey(msg, wp)) return 0;
            if (wp == VK_F11 || (wp == VK_ESCAPE && fullscreen)) toggleFullscreen();
            return 0;
        case WM_KEYUP:
            if (keyEvent(msg, wp)) return 0;
            break;
        case WM_CHAR:
            if (charEvent(wp)) return 0;
            break;
        case WM_SYSKEYDOWN:
            if (magKey(msg, wp)) return 0;  // Alt+arrows pan while magnified
            break;
        case WM_KILLFOCUS:
            releaseKeys();
            endSelect(false);
            panEnd();
            break;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            const int dpi = hwnd ? static_cast<int>(GetDpiForWindow(hwnd)) : 96;
            mmi->ptMinTrackSize = {MulDiv(320, dpi, 96), MulDiv(240, dpi, 96)};
            return 0;
        }
        case WM_DPICHANGED: {
            {
                std::lock_guard lk(m);
                dpiReq = HIWORD(wp);
            }
            wake();
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            if (!fullscreen)
                SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DISPLAYCHANGE:  // display mode / GPU / monitor topology changed
            requestAdapterCheck();
            break;
        case WM_WINDOWPOSCHANGED: {
            // Moved to a monitor on another GPU?
            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            if (mon != lastMonitor) {
                const bool first = lastMonitor == nullptr;
                lastMonitor = mon;
                if (!first) requestAdapterCheck();
            }
            break;  // DefWindowProc sends WM_SIZE / WM_MOVE
        }
        case WM_SHOWWINDOW: {
            {
                std::lock_guard lk(m);
                visibleReq = wp ? 1 : 0;
            }
            wake();
            break;
        }
        case WM_CLOSE:
            if (stopping) return 0;
            stopping = true;
            stopWorker();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY:
            hwnd = nullptr;
            break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static LRESULT CALLBACK staticWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (!self) return DefWindowProcW(h, msg, wp, lp);
        LRESULT r = self->wndProc(msg, wp, lp);
        if (msg == WM_NCDESTROY) SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return r;
    }
};

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
    if (!impl_->wd) log("watchdog off (PM_VIDEO_WATCHDOG=0): 0.6.1 behaviour");
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
    std::vector<float> d, e, y, t;
    Stats s;
    {
        std::lock_guard sl(impl_->sm);
        s = impl_->st;
        t = impl_->tapMs;
        d = impl_->decMs;
        e = impl_->e2eMs;
        y = impl_->syncErrMs;
    }
    percentile(std::move(d), s.decodeAvgMs, s.decodeP95Ms);
    percentile(std::move(e), s.e2eAvgMs, s.e2eP95Ms);
    percentile(std::move(t), s.tapAvgMs, s.tapP95Ms);
    s.syncPresented = static_cast<long long>(y.size());
    absPercentile(std::move(y), s.syncErrAvgMs, s.syncErrAbsP95Ms);
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

void VideoWindow::setPointerHandler(std::function<void(const PointerEvent&)> handler) {
    auto fn = handler ? std::make_shared<std::function<void(const PointerEvent&)>>(std::move(handler)) : nullptr;
    std::lock_guard lk(impl_->m);
    impl_->pointerFn = std::move(fn);
}

void VideoWindow::setKeyHandler(std::function<void(unsigned vk, bool down, wchar_t ch)> handler) {
    auto fn = handler ? std::make_shared<std::function<void(unsigned, bool, wchar_t)>>(std::move(handler)) : nullptr;
    std::lock_guard lk(impl_->m);
    impl_->keyFn = std::move(fn);
}

void VideoWindow::setIdleActions(std::vector<IdleAction> actions) {
    std::vector<video::Renderer::Action> ra;
    std::vector<std::function<void()>> fns;
    for (auto& a : actions) {
        ra.push_back({a.label, a.primary});
        fns.push_back(std::move(a.onClick));
    }
    {
        std::lock_guard lk(impl_->m);
        impl_->actionsReq = std::move(ra);
        impl_->actionFns = std::move(fns);
        impl_->dirty = true;
    }
    impl_->wake();
}

void VideoWindow::setIdleHelpLink(const std::wstring& label, std::function<void()> onClick) {
    std::vector<IdleAction> a;
    if (!label.empty()) a.push_back({label, false, std::move(onClick)});
    setIdleActions(std::move(a));
}

void VideoWindow::setIdleHints(std::vector<std::wstring> lines) {
    if (lines.size() > 3) lines.resize(3);
    {
        std::lock_guard lk(impl_->m);
        impl_->hintsReq = std::move(lines);
        impl_->dirty = true;
    }
    impl_->wake();
}

// ---- status / interaction UI ----

void VideoWindow::setConnecting(const std::wstring& deviceName) {
    {
        std::lock_guard lk(impl_->m);
        impl_->connectingReq = deviceName;
        impl_->dirty = true;
    }
    impl_->wake();
}

void VideoWindow::setIdleOptions(std::vector<IdleOption> options, std::function<void(int id, bool checked)> onToggle) {
    {
        std::lock_guard lk(impl_->m);
        impl_->options = std::move(options);
        impl_->onToggle = std::move(onToggle);
        impl_->optionsChanged = true;
        impl_->dirty = true;
    }
    impl_->wake();
}

void VideoWindow::showPin(const std::wstring& pin) {
    {
        std::lock_guard lk(impl_->m);
        impl_->pinReq = pin;
        impl_->dirty = true;
    }
    impl_->wake();
}

void VideoWindow::showToast(const std::wstring& text) { showToast(text, 0); }

void VideoWindow::showToast(const std::wstring& text, int holdMs) {
    {
        std::lock_guard lk(impl_->m);
        impl_->toastReq = text;
        impl_->toastHoldReq = holdMs;
        impl_->dirty = true;
    }
    impl_->wake();
}

void VideoWindow::setLiveToolbar(std::vector<ToolbarItem> items, std::function<void(int id)> onClick) {
    std::vector<video::Renderer::ToolItem> ri;
    std::vector<int> ids;
    for (auto& t : items) {
        ri.push_back({t.glyph, std::move(t.tooltip), t.toggled, t.danger, t.groupStart, t.recording, t.optional});
        ids.push_back(t.id);
    }
    {
        std::lock_guard lk(impl_->m);
        impl_->toolbarReq = std::move(ri);
        impl_->toolIds = std::move(ids);
        impl_->toolFn = std::move(onClick);
        impl_->dirty = true;
    }
    impl_->wake();
}

namespace {
constexpr uint32_t hex(const D2D1_COLOR_F& c) {
    return (static_cast<uint32_t>(c.r * 255 + 0.5f) << 16) | (static_cast<uint32_t>(c.g * 255 + 0.5f) << 8) |
           static_cast<uint32_t>(c.b * 255 + 0.5f);
}
}  // namespace

void VideoWindow::setDimmed(bool dimmed) {
    {
        std::lock_guard lk(impl_->m);
        impl_->dimReq = dimmed ? 1 : 0;
    }
    impl_->wake();
}

void VideoWindow::setRotation(int quarterTurnsClockwise) {
    const int r = ((quarterTurnsClockwise % 4) + 4) % 4;
    if (impl_->rotation.exchange(r) == r) return;
    {
        std::lock_guard lk(impl_->m);
        impl_->xformChanged = true;
    }
    impl_->wake();
}

void VideoWindow::setMirrored(bool horizontal) {
    if (impl_->mirrored.exchange(horizontal) == horizontal) return;
    {
        std::lock_guard lk(impl_->m);
        impl_->xformChanged = true;
    }
    impl_->wake();
}

void VideoWindow::desiredClientAspect(int& w, int& h) const {
    w = impl_->srcW.load();
    h = impl_->srcH.load();
    if (impl_->rotation.load() & 1) std::swap(w, h);
}

void VideoWindow::setDeviceFrame(bool enabled) {
    {
        std::lock_guard lk(impl_->m);
        impl_->frameReq = enabled ? 1 : 0;
    }
    impl_->wake();
}

void VideoWindow::setTheme(Theme t) {
    {
        std::lock_guard lk(impl_->m);
        impl_->themeReq = std::clamp(static_cast<int>(t), 0, 3);
    }
    impl_->wake();
}

std::array<uint32_t, 3> VideoWindow::themeSwatch(Theme t) {
    const auto& p = video::Renderer::palette(static_cast<int>(t));
    return {hex(p.bgTop), hex(p.card), hex(p.accent)};
}

void VideoWindow::setRecording(bool active) {
    {
        std::lock_guard lk(impl_->m);
        impl_->recReq = active ? 1 : 0;
    }
    impl_->wake();
}

void VideoWindow::setFrameTap(FrameTap tap) {
    auto fn = tap ? std::make_shared<FrameTap>(std::move(tap)) : nullptr;
    const bool on = fn != nullptr;
    if (GetCurrentThreadId() == impl_->workerTid.load()) {
        impl_->tapFn = std::move(fn);  // from inside the tap: the worker holds tapCallM
    } else {
        std::lock_guard lk(impl_->tapCallM);  // waits for a running tap call
        impl_->tapFn = std::move(fn);
    }
    impl_->tapOn = on;
    {
        std::lock_guard lk(impl_->m);
        impl_->tapChanged = true;
    }
    impl_->wake();
}

// ---- Magnifier / filters / freeze / text overlay ----

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
    for (auto& b : boxes)
        out.push_back({b.x0, b.y0, b.x1, b.y1, std::move(b.text), std::move(b.original), std::max(1, b.lines), b.bg, b.fg,
                       b.colors});
    {
        std::lock_guard lk(impl_->m);
        impl_->boxesReq = std::move(out);
        impl_->ovSelUi = -1;
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
        snapReq = req;
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
        if (snapReq == req) snapReq.reset();
        log("snapshot timed out");
        return nullptr;
    }
    return req->ok ? req : nullptr;
}

}  // namespace pm
