#pragma once
// Input events from the phone page (JSON over the reliable, ordered
// DataChannel) -> mouse / keyboard via SendInput. Constructed with
// inject = false it is a dry run (events are only written to the log; the
// spike's default).
//
// Messages (all have "t"):
//   {"t":"move","dx":12.5,"dy":-3}      relative pointer move, desktop pixels (fractions carried)
//   {"t":"abs","x":0.25,"y":0.5}        pointer to a point of the streamed monitor (0..1)
//   {"t":"down","b":0} / {"t":"up","b":0}   button 0 left, 1 middle, 2 right
//   {"t":"click","b":0|2}               down + up
//   {"t":"wheel","dx":0,"dy":-40}       scroll, WHEEL_DELTA (120) = one notch
//   {"t":"text","s":"hello"}            Unicode text
//   {"t":"key","k":"Enter"}             one named key (keyVk in input.cpp)
//   {"t":"combo","keys":["Control","C"]}  combination, pressed + released atomically
//   {"t":"release"}                     release everything held (page hidden / closing)
//
// Keys are never left down: a key and a combination go out as one SendInput
// (all down, then up in reverse). Only mouse buttons can be held (drag);
// releaseAll() lets them go (connection lost, page hidden, page silent).

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace pm::remote {

class InputInjector {
public:
    explicit InputInjector(bool inject) : inject_(inject) {}
    ~InputInjector() { releaseAll(); }
    // Returns false for unknown / malformed messages.
    bool handle(const std::string& type, double dx, double dy, int button, const std::string& text);
    void releaseAll();
    // Key combination, e.g. {"Control","C"} or {"Alt","Tab"}; also a single
    // named key. Names: see keyVk in input.cpp.
    bool combo(const std::vector<std::string>& keys);
    bool anyHeld() {
        std::lock_guard<std::mutex> lk(mu_);
        return held_[0] || held_[1] || held_[2];
    }
    void flushLog(bool force);  // aggregated move/wheel log lines
    bool injecting() const { return inject_; }
    int64_t events() const { return events_; }
    // The streamed monitor (virtual-desktop pixels): relative moves stay
    // inside it (the cursor never leaves the picture), "abs" maps onto it.
    void setScreen(const RECT& r);
    RECT screen() {
        std::lock_guard<std::mutex> lk(mu_);
        return screen_;
    }
    // Tests: asked before every click / button / wheel / key (keyboard =
    // true for keys and text); false = not injected, only logged. Lets an
    // automated test inject into its own window only.
    std::function<bool(bool keyboard)> guard;
    std::function<bool(int x, int y)> moveGuard;  // tests: pointer target (virtual-desktop pixels)
    int64_t blocked() const { return blocked_; }
    // When the phone last clicked / pressed a key (nowUs clock, 0 = never), injected or dry-run.
    int64_t lastPressUs() const { return lastPressUs_; }

private:
    void button(int b, bool down);
    bool allowed(bool keyboard);
    void moveTo(double x, double y);
    const bool inject_;
    std::mutex mu_;
    bool held_[3] = {false, false, false};
    RECT screen_{};
    double fracX_ = 0, fracY_ = 0, wheelY_ = 0, wheelX_ = 0;
    // log aggregation
    int moveN_ = 0, wheelN_ = 0, absN_ = 0;
    double moveDx_ = 0, moveDy_ = 0, wheelDx_ = 0, wheelDy_ = 0;
    int64_t lastFlushUs_ = 0;
    int64_t events_ = 0, blocked_ = 0;
    std::atomic<int64_t> lastPressUs_{0};
};

}  // namespace pm::remote
