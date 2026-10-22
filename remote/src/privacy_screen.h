#pragma once
// 0.8.4 隱私黑屏: while a phone controls this PC, the PC's own screens show a warm dark cover ("正在遠端
//操控中", 投投) that the capture does not see: the phone keeps getting the real desktop.
//
// How: a child process (this same exe, "--pm-privacy-mask") puts one window on every monitor: WS_POPUP,
// WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW (clicks and the
// phone's injected mouse go through to what is under it, never focused, not in Alt+Tab / the taskbar),
// and SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) (Windows 10 2004, build 19041, and later) BEFORE it
// is shown: DXGI Desktop Duplication (this program's capture) leaves it out. If the affinity cannot be
// set the cover is never shown.
//
// Never left behind: the child ends when (a) the parent says so, (b) its stdin pipe breaks (the parent
// is gone), (c) the parent process ends (it waits on its handle), (d) no heartbeat came for
// kWatchdogMs (the host's housekeeping beats once a second: a hung host lifts the cover), and it sits in
// a kill-on-close job of the parent. At the PC: Ctrl+Alt+Shift+B, or Esc three times within 2 s (real
// keys only: the phone's injected keys do not count), lifts it at once (the child says "U").
//
// Tests: mode "offscreen" (1x1 windows far off every screen) or "tiny" (one 2x2 magenta window at the
// primary monitor's top-left pixel, the exclusion checked against the capture; "tiny-noexclude" = the
// same without the affinity, the control). The owner's screen is never covered by a test.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace pm::remote {

constexpr int kPrivacyWatchdogMs = 6000;

// Windows 10 2004 (build 19041) or later: WDA_EXCLUDEFROMCAPTURE exists.
bool privacyOsSupported();

// Esc three times within windowMs, counted on key-down transitions (pure, tested).
struct EscTriple {
    int64_t t[3]{};
    int n = 0;
    bool press(int64_t ms, int64_t windowMs = 2000) {
        t[n % 3] = ms;
        ++n;
        if (n < 3) return false;
        const int64_t oldest = t[n % 3];  // the press two before this one
        if (ms - oldest <= windowMs) {
            n = 0;
            return true;
        }
        return false;
    }
};

class PrivacyScreen {
public:
    // what: "local" (lifted at the PC with the keys), "exited" (the child ended by itself: watchdog / error)
    using Event = std::function<void(const std::string& what)>;
    ~PrivacyScreen() { stop(); }
    // exe: the program to run as the child (this one: GetModuleFileNameW). mode: "" (real), "offscreen",
    // "tiny", "tiny-noexclude". false + err ("unsupported", "affinity <code>", "spawn <code>", "timeout").
    bool start(const std::wstring& exe, int lang, const std::string& mode, Event onEvent, std::string& err);
    void stop();
    bool running() const { return running_; }
    void beat();
    // The child ended by itself (lifted at the PC / watchdog): close its handles. Not from onEvent (its thread).
    void reap() {
        bool gone = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            gone = proc_ && !running_ && !starting_;
        }
        if (gone) stop();
    }
    unsigned long pid() const { return pid_; }
    // Tests (a test mode only; the real cover ignores it): as if Ctrl+Alt+Shift+B was pressed at the PC.
    void testUnlock();

private:
    void readLoop();
    mutable std::mutex mu_;
    void* proc_ = nullptr;     // HANDLE
    void* job_ = nullptr;      // HANDLE
    void* inW_ = nullptr;      // HANDLE: the child's stdin, our end
    void* outR_ = nullptr;     // HANDLE: the child's stdout, our end
    std::thread reader_;
    std::atomic<bool> running_{false}, stopping_{false}, starting_{false};
    std::atomic<int> ready_{0};  // 0 waiting, 1 ready, -1 failed
    std::string readyErr_;
    unsigned long pid_ = 0;
    Event onEvent_;
};

// The child's main (see remote.h runPrivacyMaskIfAsked).
int privacyMaskMain(unsigned long parentPid, int lang, const std::string& mode);
// --pm-privacy-render <out.png> <w> <h> <lang> <dpi>: the cover as a PNG (screenshots; nothing is shown).
int privacyRenderPng(const std::wstring& out, int w, int h, int lang, int dpi);

}  // namespace pm::remote
