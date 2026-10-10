// 自在投影 app: 來源仲裁的全域狀態與 AirPlay / Android 影音閘門、狀態 sink。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出
#pragma once

#include "core/base/base.h"
#include "core/base/log.h"

namespace pm_app {

// Source arbitration state (defined in core/base/source_gate.cpp).
extern std::atomic<int> g_active;
extern std::atomic<bool> g_takeoverNew;  // Settings::takeoverKeep == false
extern std::atomic<HWND> g_uiHwnd;
extern pm::MiracastReceiver* g_miracast;  // set in wWinMain, lives until exit
extern pm::AndroidSource* g_android;


// Video from AirPlay / Android: forwarded only while that source owns the
// window. A new stream (onCodec) claims a free window, or a busy one with
// takeover=new. The codec is replayed when a blocked stream gets the window.
class GateVideoSink final : public pm::VideoSink {
public:
    GateVideoSink(int me, pm::VideoSink& inner) : me_(me), inner_(inner) {}
    void onCodec(pm::VideoCodec c) override {
        codec_ = c;
        haveCodec_ = true;
        if (claimSource(me_, g_takeoverNew.load())) {
            synced_ = true;
            inner_.onCodec(c);
        } else {
            synced_ = false;
        }
    }
    void onFrame(const uint8_t* d, size_t n, uint64_t t) override {
        if (!pass()) return;
        inner_.onFrame(d, n, t);
    }
    void onSourceSize(int w, int h) override {
        if (pass()) inner_.onSourceSize(w, h);
    }
    void onReset() override {
        if (g_active.load() == me_) inner_.onReset();
        synced_ = false;
    }
    void onPaused(bool p) override {
        if (g_active.load() == me_) inner_.onPaused(p);
    }

private:
    bool pass() {
        if (g_active.load() != me_ && !(g_active.load() == SrcNone && claimSource(me_, false))) {
            synced_ = false;
            return false;
        }
        if (!synced_ && haveCodec_) inner_.onCodec(codec_);  // got the window mid-stream
        synced_ = true;
        return true;
    }
    const int me_;
    pm::VideoSink& inner_;
    std::atomic<bool> synced_{false}, haveCodec_{false};
    pm::VideoCodec codec_ = pm::VideoCodec::H264;
};

// Audio from AirPlay / Android: played while that source owns the window (or
// nobody does — AirPlay audio may start before its video). The last format is
// replayed when a blocked stream becomes audible.
class GateAudioSink final : public pm::AudioSink {
public:
    GateAudioSink(int me, pm::AudioSink& inner) : me_(me), inner_(inner) {}
    void onFormat(pm::AudioCodec c, int rate, int ch, int spf) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            fmt_ = {c, rate, ch, spf};
            haveFmt_ = true;
        }
        if (open()) {
            inner_.onFormat(c, rate, ch, spf);
            synced_ = true;
        } else {
            synced_ = false;
        }
    }
    void onPacket(const uint8_t* d, size_t n, uint64_t t) override {
        packets.fetch_add(1, std::memory_order_relaxed);
        if (!open()) {
            synced_ = false;
            return;
        }
        if (!synced_) {
            Fmt f;
            bool have;
            {
                std::lock_guard<std::mutex> lock(mu_);
                f = fmt_;
                have = haveFmt_;
            }
            if (!have) return;
            inner_.onFormat(f.c, f.rate, f.ch, f.spf);
            synced_ = true;
        }
        inner_.onPacket(d, n, t);
    }
    void onVolume(float db) override {
        if (open()) inner_.onVolume(db);
    }
    void onFlush() override {
        if (open()) inner_.onFlush();
    }
    std::atomic<long long> packets{0};  // every packet seen (liveness of the source)

private:
    bool open() const {
        const int a = g_active.load();
        return a == me_ || a == SrcNone;
    }
    struct Fmt {
        pm::AudioCodec c = pm::AudioCodec::AAC_ELD;
        int rate = 44100, ch = 2, spf = 480;
    };
    const int me_;
    pm::AudioSink& inner_;
    std::mutex mu_;
    Fmt fmt_;
    bool haveFmt_ = false;
    std::atomic<bool> synced_{false};
};

// Forwards to the video window and tracks the mirroring state for the UI.
// An unexpected end of a session (anything but the phone's own "stop
// mirroring", which the core logs as "video_reset: RTP shutdown" right
// before onReset) does not drop to the idle screen at once: the last frame is
// held (StateLost) until the UI calls releaseHold() ~3 s later, or until the
// next session's video arrives.
class StatusVideoSink final : public pm::VideoSink {
public:
    StatusVideoSink(pm::VideoWindow& win, Log& log) : win_(win), log_(log) {}

    // Fed from the core's log callback (same thread, just before onReset).
    void noteCoreLog(const std::string& msg) {
        if (msg.rfind("video_reset: RTP shutdown", 0) == 0) cleanStopAt_ = nowMs();
    }

    void onCodec(pm::VideoCodec codec) override {
        releaseHold();
        log_.write("info", codec == pm::VideoCodec::H265 ? "video codec H.265" : "video codec H.264");
        win_.onCodec(codec);
    }
    void onFrame(const uint8_t* data, size_t len, uint64_t ntp) override {
        frames.fetch_add(1, std::memory_order_relaxed);
        if (holding_) releaseHold();
        if (!mirroring_.exchange(true)) {
            log_.write("info", "mirroring started");
            post(StateMirroring);
        } else if (paused_.exchange(false)) {  // pictures again without video_resume: not paused any more
            log_.write("info", "frames after video_pause: resumed");
            post(StateMirroring);
            win_.onPaused(false);
        }
        win_.onFrame(data, len, ntp);
    }
    void onSourceSize(int w, int h) override {
        releaseHold();
        log_.write("info", "source size " + std::to_string(w) + "x" + std::to_string(h));
        win_.onSourceSize(w, h);
    }
    void onReset() override {
        std::lock_guard<std::mutex> lock(mu_);
        if (holding_) return;  // keep showing the last frame; the UI ends the hold
        const bool wasPaused = paused_.exchange(false);
        if (mirroring_.exchange(false)) {
            const bool clean = nowMs() - cleanStopAt_.load() < 2000;
            log_.write("info", clean ? "mirroring stopped" : "mirroring lost (unexpected reset)");
            if (!clean && !wasPaused) {
                holding_ = true;
                post(StateLost);
                return;
            }
            post(StateIdle);
        }
        win_.onReset();
    }
    void onPaused(bool paused) override {
        paused_ = paused;
        if (mirroring_) post(paused ? StatePaused : StateMirroring);
        win_.onPaused(paused);
    }

    // UI thread: the phone went away without onReset (connection dropped).
    void lostWithoutReset() {
        std::lock_guard<std::mutex> lock(mu_);
        if (holding_ || !mirroring_.exchange(false)) return;
        log_.write("info", "mirroring lost (client disconnected)");
        if (paused_.exchange(false)) {
            post(StateIdle);
            win_.onReset();
            return;
        }
        holding_ = true;
        post(StateLost);
    }
    // Ends a hold: the window returns to the idle screen. Returns true if a
    // hold was active.
    bool releaseHold() {
        std::lock_guard<std::mutex> lock(mu_);
        if (!holding_) return false;
        holding_ = false;
        win_.onReset();
        return true;
    }
    // UI thread: drop a stale picture (e.g. after resume from sleep).
    void forceIdle() {
        std::lock_guard<std::mutex> lock(mu_);
        holding_ = false;
        paused_ = false;
        if (mirroring_.exchange(false)) log_.write("info", "mirroring dropped (stale after resume, or stopped by the user)");
        win_.onReset();
        post(StateIdle);
    }
    // UI thread: another source took the window. Forget the old stream's
    // state without touching the window (the newcomer's picture replaces
    // it), so the next stream's first frame counts as "mirroring started".
    void switchSource() {
        std::lock_guard<std::mutex> lock(mu_);
        holding_ = false;
        paused_ = false;
        mirroring_ = false;
    }
    bool mirroring() const { return mirroring_; }
    bool holding() const { return holding_; }
    std::atomic<long long> frames{0};  // every frame passed on (liveness watchdog)

private:
    void post(WPARAM state) {
        if (HWND h = reinterpret_cast<HWND>(win_.hwnd())) PostMessageW(h, WM_PM_STATE, state, 0);
    }
    pm::VideoWindow& win_;
    Log& log_;
    std::mutex mu_;
    std::atomic<bool> mirroring_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> holding_{false};
    std::atomic<long long> cleanStopAt_{-100000};
};

}  // namespace pm_app
