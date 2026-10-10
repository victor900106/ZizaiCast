// VideoWindow::Impl: the window, decoder / render worker and UI state behind pm::VideoWindow.
// 拆檔 0.7.9：自 video_window.cpp 原樣搬出（資料成員順序不變）。
#pragma once

#include "pm/video_window.h"
#include "video_window_internal.h"

namespace pm {

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
    // Slider items (volume): the last items handed over (UI thread copy, so a
    // drag shows at once) and the slide / wheel handlers.
    std::vector<video::Renderer::ToolItem> toolItemsUi;
    std::function<void(int, float, bool)> toolSlideFn;
    std::function<void(int, int)> toolWheelFn;
    bool toolActivityReq = false, toolLeaveReq = false;
    std::optional<std::pair<double, std::wstring>> toolRevealReq;  // revealToolbar (0.7.8)
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
    std::vector<std::shared_ptr<SnapshotRequest>> snapReqs;  // all served by the next frame (several callers at once)
    std::vector<RECT> optionRects;  // published by the worker (client pixels)
    // Idle hints / action requests and the published action / picture
    // geometry (client pixels; picRect empty unless a live picture is up).
    std::optional<std::vector<std::wstring>> hintsReq;
    std::optional<std::vector<video::Renderer::Card>> cardsReq;  // setIdleCards (0.7.8)
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
    int motionReq = -1;           // 1 reduced motion (Windows 「顯示動畫」 off), 0 full
    // Pressed element / keyboard focus ring (kind * 1000 + index, -1 none, -2 no request).
    int pressReq = -2, focusReq = -2;
    bool flashReq = false;
    bool pressSent = false;       // UI thread: an element is shown pressed
    int focusUi = -1;             // UI thread: idle screen element with the focus ring (in reading order, see idleKey)
    int motionOverride = -1;      // UI thread: test hook 17 (-1 follow Windows)
    int motionSent = -1;          // UI thread: last value sent
    bool xformChanged = false, mascotClickReq = false, tapChanged = false;
    // Magnifier / filter / freeze / text overlay / region selection requests.
    // viewUi + frozenUi: the latest requested state (any thread, under m);
    // pubView: what the worker last drew (mapPoint uses it with picRect).
    video::Renderer::View viewUi, pubView;
    bool frozenUi = false;
    bool viewChanged = false;
    int frozenReq = -1, originalReq = -1;
    std::optional<std::vector<video::Renderer::TextBox>> boxesReq;
    std::optional<std::vector<std::wstring>> onlineReq;  // setTextOverlayOnline
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
    std::atomic<int> liveOvReq{-1};  // setTextOverlayLive (-1 none)
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
    // stats() callers (ScreenTranslator polls it every 100-200 ms): the
    // samples are append-only, so only the new ones are copied under sm, and
    // the 95th percentiles over up to 1M samples are recomputed at most once a
    // second (the averages are always exact).
    struct StatCopy {
        std::vector<float> v;
        double sum = 0, p95 = 0;
    };
    mutable std::mutex statCopyM;
    mutable StatCopy decCopy, e2eCopy, syncCopy, tapCopy;
    mutable double statP95At = -1e18;

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
    int slidingTool = -1;          // slider item being dragged (capture held), -1 none
    int toolWheelAcc = 0;          // wheel delta not yet a whole notch (touchpads)
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
    // Inside feed() (the decoder's output callback runs within
    // MfDecoder::drain) the decoder must not be closed: a device failure
    // seen there only sets lostInDecode, and feed() checks the device once
    // decode() has returned.
    bool inFeed = false, lostInDecode = false;
    // Test hook (--test-at T:0:1): the next decoded picture's upload fails
    // and the device reports removal, from inside the decoder callback.
    bool faultLoseInDecode = false, faultRemoved = false;
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
    // Minimized / hidden (tray): decoded pictures are not uploaded; the newest
    // one waits here (a BGRA one in bgraCur) and goes up when the window shows.
    ComPtr<IMFSample> hiddenSample;
    video::VideoFormat hiddenFmt{};
    double hiddenTIn = -1;
    bool bgraHidden = false;
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
            presentOk{0}, presentOccl{0}, presentErr{0}, waitTimeouts{0}, recoveries{0}, auBytes{0};
        std::atomic<double> lastIdrAt{-1}, lastAuAt{-1}, stageAt{0};
        // Freeze diagnostics: when the shown picture was frozen (-1: live) and
        // when the newest live picture was decoded.
        std::atomic<double> frozenSince{-1}, lastPictureAt{-1};
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
    void enqueue(AccessUnit&& au);

    // ---------------------------------------------------------------------
    // Adapter choice: the high-performance GPU (hybrid laptops: the discrete
    // GPU; DWM composes it onto the integrated GPU's panel).  Exception: on a
    // multi-GPU desktop where the window's monitor is driven by another GPU
    // that has its own outputs, render on that GPU (no cross-adapter copy).
    ComPtr<IDXGIAdapter1> chooseAdapter(const char*& why);

    static bool sameLuid(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

    bool createDevice();

    bool initGraphics();

    // ---------------------------------------------------------------------
    // Device loss (TDR, driver update/reset, GPU removed or switched): drop
    // every device object and re-create them; the window and UI state stay.
    void loseDevice(const char* why, HRESULT reason);

    // Freeze on / off on the renderer, with a diagnostic line.  Unfreezing
    // always returns to the live picture (the newest decoded one); the log
    // says how old that is and what arrived while frozen.
    void applyFrozen(bool on, const char* why);
    long long freezeAuIn = 0, freezeDecoded = 0;

    // The renderer ended a freeze whose picture did not survive a device
    // loss (and cleared the text overlay): the UI state follows, and the
    // view handler (UI thread) hears about it, like a user change.
    void freezeDropped();

    // Re-feeds the access units since the last key frame into a freshly opened
    // decoder; only the newest picture is kept (no GPU copies for the
    // others).  Returns the number of AUs fed.  A truncated cache feeds only
    // the key frame: the pictures after it lack their references.
    size_t refeedGop();
    size_t gopMaxAUs() const { return wd ? kGopMaxAUs : 300; }
    size_t gopMaxBytes() const { return wd ? kGopMaxBytes : 16u << 20; }
    bool refsLost = false;  // AUs lost / decoder restarted without the full GOP since the last IDR

    // True if the device is gone (or was just found removed).
    bool checkDevice();

    // A device call failed: check the device now, or (inside the decoder
    // callback) once feed() returns.
    void deviceFailed();

    bool recoverDevice();

    // A live AirPlay stream: AUs arrived within the last 2 s.
    bool streamLive() const {
        const double t = dg.lastAuAt.load(std::memory_order_relaxed);
        return t >= 0 && nowMs() - t < 2000;
    }

    // Monitor / display configuration changed: switch GPU if another one is
    // now the better choice.
    void checkAdapter(const char* trigger);

    bool openDecoder();

    // ---------------------------------------------------------------------
    // A/V sync queue
    void dropHeld();

    double heldTarget(const Held& h, double latency) const {
        return std::min(h.due + latency, h.decoded + kSyncMaxHoldMs);
    }

    void showHeld(Held& h, double target);

    // Shows the newest held picture whose time has come (older ones are late:
    // skipped).  Called before every present decision.
    void syncTick();

    // Milliseconds until the next held picture is due, < 0 if none.
    double syncWaitMs() const {
        if (held.empty()) return -1;
        return std::max(0.0, heldTarget(held.front(), syncLatencyMs.load()) - kSyncSlackMs - nowMs());
    }

    // A decoded picture reached the renderer (shown or held).
    // live: from a newly received AU (re-fed ones do not end a stall).
    void pictureDecoded(double t, bool live);

    void onDecoded(IMFSample* sample, const video::VideoFormat& fmt);

    // ---------------------------------------------------------------------
    // BGRA pictures (submitBgraFrame): shown ASAP, like a decoded picture.
    void takeBgra();

    void showBgra(double tIn, uint64_t pts);

    // The window shows again (or a snapshot wants the picture): upload the
    // newest picture that arrived while minimized / hidden.
    void showHidden();

    // Source ended (onReset): free the last picture.  The mailbox is
    // cleared by onReset() itself, so a frame submitted after it survives.
    void dropBgra();

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

    HRESULT feed(const uint8_t* p, size_t n, double tIn, double due, uint64_t pts = 0);

    void remember(AccessUnit& au);

    void decodeOne(AccessUnit& au);

    // ---------------------------------------------------------------------
    // Frame tap
    void tapPicture(const video::Renderer::Picture* pic, uint64_t pts);

    void deliverTaps(bool all);

    void workerMain(std::promise<bool>& ready);

    bool pendingLocked() const {
        return stop || !queue.empty() || dirty || resizePending || resetPending || codecChanged || pausePending >= 0 ||
               pokeReq || !snapReqs.empty() || syncChanged || adapterCheckReq || testReq >= 0 || themeReq >= 0 || dimReq >= 0 ||
               frameReq >= 0 || recReq >= 0 || mascotHoverReq >= 0 || xformChanged || mascotClickReq || tapChanged ||
               bgraNew.load() || hintsReq || actionsReq || linkHoverReq != -2 || toolbarReq || toolActivityReq ||
               toolLeaveReq || toolHoverReq != -2 || viewChanged || frozenReq >= 0 || originalReq >= 0 || boxesReq ||
               onlineReq || busyReq || selChanged || motionReq >= 0 || pressReq != -2 || focusReq != -2 || flashReq;
    }

    // Takes everything queued plus control flags; returns false on stop.
    // waitMs: 0 = don't wait, < 0 = until something happens, else at most
    // that long (animation frame / held picture due / recovery retry).
    bool take(std::deque<AccessUnit>& out, double waitMs);

    bool needPresent = true;
    bool snapshot(const std::wstring& pngPath, bool framed, bool ui = false);
    std::shared_ptr<SnapshotRequest> runSnapshot(bool framed, bool ui, bool grab);

    void publishOptionRects();

    // Earliest of two "ms until" values where < 0 means "never".
    static double earliest(double a, double b) {
        if (a < 0) return b;
        if (b < 0) return a;
        return std::min(a, b);
    }

    void decodeBatch(std::deque<AccessUnit>& batch);

    // ---------------------------------------------------------------------
    // Fault injection (kTestMsg wParam 20..26, pm_video_test --test-at T:C:ARG).
    void injectFault(int code, LPARAM arg);

    // Watchdog (a): AUs are fed but no picture comes out.  Restarts the
    // decoder and re-feeds the GOP cache (2nd time within a minute: in
    // software).  Without the references (AUs lost, cache truncated) a
    // restart cannot help: logged once, the next IDR resumes the picture.
    void watchdogDecode();

    // Watchdog (b): pictures are decoded but none reaches the screen (Present
    // not called / failing, or the swap chain no longer releasing frames).
    // Re-creates the swap chain (twice within 10 s: the whole device).
    void watchdogPresent();

    void loop();

    // 0 if a new picture may be presented now, else ms until it may (occluded).
    double occludedWaitMs() const {
        if (!wd || !ren.occluded()) return 0;
        return std::max(0.0, lastPicturePresentAt + kOccludedPresentMs - nowMs());
    }

    // Present bookkeeping: counters, occlusion transitions, watchdog state.
    void notePresent(bool picture);


    // ---------------------------------------------------------------------
    // Monitor thread: one summary line per 5 s while AUs arrive, and a
    // warning when the render thread is stuck (it cannot report that
    // itself).  Sleeps on monEvent while no stream is live.
    struct Counts {
        long long in, drop, skip, fed, mft, dec, pres, ok, occl, err, wto, bytes;
    };
    Counts counts() const;

    static const char* stageName(int s);

    void monitorMain();

    void summary(Counts& prev, double& last, double now);

    void startMonitor();
    void stopMonitor();

    void stopWorker();

    void toggleFullscreen();

    // Fullscreen: cover the (nearest) monitor again after a display change
    // (resolution, scaling, monitor removed / added: rcMonitor was read once
    // by toggleFullscreen()).  No-op if not fullscreen or already fitted.
    void refitFullscreen(const char* why);

    // Index of the idle check box at the mouse position (client pixels), or -1.
    int hitTest(LPARAM lp);

    // Is the mouse position (client pixels) on an opaque part of the mascot?
    bool hitMascot(LPARAM lp);

    void setMascotHot(bool hot);

    void clickMascot();

    void setHover(int i);

    void toggleOption(int i);

    // Windows 「顯示動畫」 (SPI_GETCLIENTAREAANIMATION), or the test override,
    // to the renderer when it changed.
    void sendMotion();

    // Press feedback (kind: video::Renderer::UiKind; < 0 released).
    void sendPress(int kind, int index);
    void sendFocus(int kind, int index);
    void clearFocus();

    // Idle screen keyboard: Tab / Shift+Tab move a focus ring over the check
    // boxes and the actions, Space / Enter use the focused one, Esc hides the
    // ring.  Only while they are shown (never live: keys go to the phone).
    bool idleKey(UINT msg, WPARAM wp);

    void markDirty();

    void requestAdapterCheck();

    // ---------------------------------------------------------------------
    // Idle actions: pill buttons / links (UI thread)
    // Index of the action at the mouse position, or -1.
    int hitLink(LPARAM lp);

    void setLinkHot(int hot);

    void clickLink(int i);

    // ---------------------------------------------------------------------
    // Live toolbar (UI thread)
    // Button at the mouse position (client pixels), or -1.
    int hitTool(POINT pt);
    bool inToolPill(POINT pt);

    void setToolHot(int hot, bool inside);

    // Mouse moved: (re)show the toolbar for ~2 s (requests at most every 200 ms).
    void toolActivity();

    void toolLeave();

    void clickTool(int i);

    // Left press: on the toolbar pill (not during a drag for the phone) it is
    // the toolbar's (never forwarded); the click happens on release.
    bool toolPress(LPARAM lp);

    bool toolIsSlider(int i);

    // Slider drag: the value at client x (track = the hit rect less
    // kToolSliderInset x its height at each end), shown at once and passed to
    // the slide handler (done = released / capture lost).
    void slideTool(int x, bool done);

    // Wheel over a slider, or over the button just before one (the speaker of
    // a volume slider, even when the slider is hidden): whole notches to the
    // wheel handler (+ up).  False if neither.
    bool toolWheel(POINT pt, int delta);

    // ---------------------------------------------------------------------
    // Remote control (UI thread)
    using PointerEvent = VideoWindow::PointerEvent;

    std::shared_ptr<std::function<void(const PointerEvent&)>> pointerHandler();

    std::shared_ptr<std::function<void(unsigned, bool, wchar_t)>> keyHandler();

    // Client pixel -> normalised picture position, undoing letterbox /
    // device frame, rotation and mirroring.  Outside the picture: false,
    // unless clamp (drags that leave the picture stick to its edge).
    bool mapPoint(POINT pt, float& x, float& y, bool clamp);

    bool overPicture();

    void sendPointer(const std::shared_ptr<std::function<void(const PointerEvent&)>>& fn, PointerEvent::Kind kind,
                     float x, float y, int button);

    static int lowestButton(int mask) { return mask & 1 ? 0 : mask & 2 ? 1 : 2; }

    // All forwarded buttons go up at the last position (capture lost, picture
    // gone, handler removed).
    void cancelButtons();

    // True if the press went to the pointer handler.
    bool pointerDown(int button, LPARAM lp);

    bool pointerUp(int button, LPARAM lp);

    // Moves: every one while a button is down (drag), hover moves over the
    // picture at most ~60 Hz (the last one is delivered by kHoverTimer).
    bool pointerMove(LPARAM lp);

    bool pointerWheel(UINT msg, WPARAM wp, LPARAM lp);

    // Keyboard: forwarded only without Ctrl / Alt (those are the app's
    // shortcuts), except Ctrl+C / V / X / A / Z.  F11 and Esc in fullscreen
    // stay with the window; Ctrl / Alt / Win themselves are not forwarded.
    bool keyEvent(UINT msg, WPARAM wp);

    // Text without a forwarded key: IME results, dead-key compositions,
    // AltGr characters, the second half of a surrogate pair.
    bool charEvent(WPARAM wp);

    void releaseKeys();

    // ---------------------------------------------------------------------
    // Magnifier / region selection (UI thread unless noted)
    bool panning = false;          // left drag pans the magnified picture
    POINT panLast{};
    bool selecting = false;        // beginRegionSelect() active
    bool selDragging = false;
    float selV[4] = {-1, -1, -1, -1};  // viewport coords of the drag
    std::function<void(bool, float, float, float, float)> selDone;

    // Any thread: a new view (clamped) for the worker; user: reported to viewFn.
    void applyView(video::Renderer::View v, bool user);
    VideoWindow::ViewState viewStateLocked() const;
    video::Renderer::View currentView();
    // Zoom keeping viewport point (vx, vy) where it is.
    void zoomAtV(float zoom, float vx, float vy, bool user);
    void panV(float dx, float dy, bool user);
    static float stepZoom(float z, int steps) {
        z *= std::pow(1.25f, static_cast<float>(steps));
        if (std::fabs(z - 1) < 0.06f) z = 1;
        return z;
    }
    // Client pixel -> viewport coords of the live picture (false: not on it).
    bool viewportPoint(POINT pt, float& vx, float& vy, bool clamp);
    void viewportToContent(float vx, float vy, float& dx, float& dy);

    // Wheel: Ctrl = zoom at the cursor; zoomed without a pointer handler =
    // pan (Shift / tilt: sideways).
    bool magWheel(UINT msg, WPARAM wp, LPARAM lp);

    // Left press: start panning (zoomed; Ctrl needed with a pointer handler).
    bool panPress(LPARAM lp);
    void panMove(LPARAM lp);
    void panEnd();

    // Keys: Ctrl+= / Ctrl+- zoom, Ctrl+Shift+0 1x, arrows pan while zoomed.
    bool magKey(UINT msg, WPARAM wp);

    // ---- Text overlay list panel / markers (UI thread) ----
    static constexpr int kOvNone = -3, kOvZoom = -2, kOvPanel = -4;
    int ovHotUi = -1;           // list item under the cursor
    int ovPress = kOvNone;      // where the left button went down
    int ovOnUi = kOvNone;       // what the cursor is on
    // Item (row or marker; *marker tells which), kOvZoom, kOvPanel or kOvNone at pt.
    int ovHit(POINT pt, bool* marker = nullptr);
    bool hitIsMarker(POINT pt);
    void ovSetHot(int i);
    void ovClick(int hit, bool marker);

    // ---- Region selection (beginRegionSelect) ----
    void sendSelection();
    void startSelect(std::function<void(bool, float, float, float, float)> done);
    void endSelect(bool ok);
    // Mouse in selection mode; true = handled.
    bool selectMouse(UINT msg, LPARAM lp);

    LRESULT wndProc(UINT msg, WPARAM wp, LPARAM lp);

    static LRESULT CALLBACK staticWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
};

}  // namespace pm
