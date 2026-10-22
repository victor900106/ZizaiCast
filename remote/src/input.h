#pragma once
// Input events from the phone page (JSON over the reliable, ordered
// DataChannel) -> mouse / keyboard via SendInput. Constructed with
// inject = false it is a dry run (events are only written to the log; the
// spike's default).
//
// Messages (all have "t"):
//   {"t":"move","dx":12.5,"dy":-3}      relative pointer move, desktop pixels (fractions carried)
//   {"t":"abs","x":0.25,"y":0.5}        pointer to a point of the streamed monitor (0..1)
//   {"t":"down","b":0} / {"t":"up","b":0}   button 0 left, 1 middle, 2 right, 3 back, 4 forward (0.8.3.5)
//   {"t":"click","b":0|2}               down + up
//   {"t":"wheel","dx":0,"dy":-40}       scroll, WHEEL_DELTA (120) = one notch
//   {"t":"text","s":"hello"}            Unicode text
//   {"t":"key","k":"Enter"}             one named key (keyVk in input.cpp)
//   {"t":"combo","keys":["Control","C"]}  combination, pressed + released atomically
//   {"t":"release"}                     release everything held (page hidden / closing)
//   {"t":"kd","k":"KeyA"} / {"t":"ku",..}  0.8.3.5: a physical key (KeyboardEvent.code) down / up (scan code)
//   {"t":"rel","dx":3,"dy":-1}          0.8.3.5 遊戲模式: raw relative motion (MOUSEEVENTF_MOVE, no ABSOLUTE)
//   {"t":"kh"}                          0.8.3.5: keys still held (every 0.4 s; 1.5 s without = all let go)
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
    // stub (0.8.3.5 tests): the whole injection path runs (button state, coordinates, checks)
    // but SendInput is replaced by a stub that only logs each event: never a real mouse / key.
    explicit InputInjector(bool inject, bool stub = false) : inject_(inject || stub), stub_(stub) {}
    ~InputInjector() { releaseAll(); }
    // Returns false for unknown / malformed messages.
    bool handle(const std::string& type, double dx, double dy, int button, const std::string& text);
    void releaseAll();
    // Key combination, e.g. {"Control","C"} or {"Alt","Tab"}; also a single
    // named key. Names: see keyVk in input.cpp.
    bool combo(const std::vector<std::string>& keys);
    bool anyHeld() {
        std::lock_guard<std::mutex> lk(mu_);
        for (bool h : held_)
            if (h) return true;
        return !keysHeld_.empty();
    }
    void flushLog(bool force);  // aggregated move/wheel log lines
    void tick();                // 0.8.3.5: lets go of keys whose page went quiet (call ~every second)
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

    bool stubbed() const { return stub_; }
    // 0.8.3.5 UIPI: where a click / key would go and whether Windows lets it through.
    struct Target {
        DWORD pid = 0;
        std::string exe;      // "regedit.exe"
        bool higher = false;  // its integrity level is above ours (run as administrator): UIPI drops our input
        bool secure = false;  // the secure desktop (UAC prompt, lock screen): nothing gets through
    };
    // why = "admin" | "secure"; exe = the program (admin). Called from handle() (input lock held).
    std::function<void(const std::string& why, const std::string& exe)> onBlocked;
    // Tests: replaces the real look-up (window under the pointer / foreground window).
    std::function<Target(bool keyboard, POINT at)> targetProbe;
    int64_t uipiBlocked() const { return uipiBlocked_; }
    // Tests (stub + the test clipboard): what a program on the PC does on Ctrl+C (puts text on the clipboard).
    std::function<void()> onStubCopy;
    // The integrity level RID of a process (SECURITY_MANDATORY_MEDIUM_RID ...; 0 = unknown, denied = no access to its token).
    static DWORD integrityOf(HANDLE process, bool& denied);
    // Tests (pm_input_test): a made-up virtual desktop (monitors) for the stub; where the stub's pointer is.
    void setVirtualDeskForTest(const RECT& r) { testDesk_ = r; }
    POINT stubCursor() const { return stubCur_; }

private:
    UINT sendInputs(UINT n, INPUT* in, const char* what);
    void sendMouse(DWORD flags, LONG dx = 0, LONG dy = 0, DWORD data = 0);
    void sendVk(WORD vk, bool extended);
    void sendText(const std::wstring& w);
    POINT cursorPos();
    void button(int b, bool down);
    bool physKey(const std::string& code, bool down);
    void sendScan(WORD scan, WORD vk, bool up);
    void releaseKeysLocked(const char* why);
    struct HeldKey {
        std::string code;
        WORD scan, vk;
    };
    std::vector<HeldKey> keysHeld_;
    int64_t lastKeyMsgUs_ = 0;
    int keyDowns_ = 0;
    int relN_ = 0;
    double relDx_ = 0, relDy_ = 0, relFx_ = 0, relFy_ = 0;
    bool allowed(bool keyboard);
    void checkTarget(bool keyboard);
    Target realTarget(bool keyboard, POINT at);
    struct IlCache {
        DWORD pid;
        int64_t atUs;
        bool higher;
        std::string exe;
    };
    std::vector<IlCache> ilCache_;
    DWORD ownIntegrity_ = 0;
    std::string lastBlockedKey_;
    int64_t lastBlockedUs_ = 0;
    std::atomic<int64_t> uipiBlocked_{0};
    void moveTo(double x, double y);
    RECT virtualDesk() const;
    const bool inject_;
    const bool stub_;
    POINT stubCur_{};  // stub: where the pointer would be
    std::mutex mu_;
    static constexpr int kButtons = 5;  // left, middle, right, back, forward
    bool held_[kButtons] = {};
    RECT testDesk_{};   // tests: the virtual desktop (else the real one)
    POINT lastAbs_{};   // the last "abs" target (log)
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
