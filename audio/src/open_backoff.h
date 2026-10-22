// open_backoff.h (0.8.4): retry schedule + log merging for a WASAPI output that will not open.
// Before: a failed open was retried at once whenever any packet woke the render thread -- 1598 lines
// "WASAPI open failed (hr=0x80070490)" in 16 seconds on a PC whose output device had vanished.
// Now: 1 s, 2 s, 4 s ... up to 60 s between tries (a device change, a new format or stop() cuts the wait
// short); the same error is logged once, then again only every 10 minutes with the number of tries.
#pragma once

#include <algorithm>
#include <cstdint>

namespace pm {

class OpenBackoff {
public:
    static constexpr int kFirstMs = 1000;
    static constexpr int kMaxMs = 60000;
    static constexpr int64_t kRelogMs = 600000;

    struct Verdict {
        int delayMs = kFirstMs;   // wait this long before the next try
        bool log = false;         // write a line now
        unsigned tries = 0;       // failed tries so far in this spell
        unsigned sinceLog = 0;    // failed tries since the last written line (including this one)
    };

    Verdict onFail(unsigned long hr, int64_t nowMs) {
        ++tries_;
        ++sinceLog_;
        Verdict v;
        v.tries = tries_;
        v.delayMs = std::min<int64_t>(kMaxMs, int64_t(kFirstMs) << std::min<unsigned>(tries_ - 1, 6));
        if (tries_ == 1 || hr != lastHr_ || nowMs - lastLogMs_ >= kRelogMs) {
            v.log = true;
            v.sinceLog = sinceLog_;
            sinceLog_ = 0;
            lastLogMs_ = nowMs;
        }
        lastHr_ = hr;
        return v;
    }
    // The output opened: returns the number of failed tries of the spell that just ended (0 = it never failed).
    unsigned onSuccess() {
        const unsigned n = tries_;
        tries_ = 0;
        sinceLog_ = 0;
        return n;
    }
    unsigned tries() const { return tries_; }

private:
    unsigned tries_ = 0, sinceLog_ = 0;
    unsigned long lastHr_ = 0;
    int64_t lastLogMs_ = 0;
};

}  // namespace pm
