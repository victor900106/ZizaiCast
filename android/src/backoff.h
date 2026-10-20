// Exponential backoff with bounded jitter (pure; unit-tested).
//   delay(n) = min(cap, base * 2^n) * (1 ± jitter), n = attempts so far.
// Used for every retry in pm_android: reconnecting a dropped phone,
// restarting a stalled scrcpy-server, polling adb for a device to show up,
// and connecting to the scrcpy sockets while the server starts.
#pragma once

#include <algorithm>
#include <cstdint>

namespace pm::android {

class Backoff {
public:
    Backoff(int baseMs, int capMs, double jitter = 0.2, uint32_t seed = 0x9e3779b9u)
        : base_(std::max(1, baseMs)), cap_(std::max(base_, capMs)), jitter_(std::clamp(jitter, 0.0, 0.5)),
          state_(seed ? seed : 1u) {}
    // Delay before the next attempt; advances the attempt counter.
    int next() {
        long long d = base_;
        for (int i = 0; i < attempt_ && d < cap_; ++i) d *= 2;
        d = std::min<long long>(d, cap_);
        ++attempt_;
        if (jitter_ <= 0) return int(d);
        // xorshift32: deterministic per seed (tests), decorrelated enough
        // that two retry loops do not stay in lock step.
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        const double u = double(state_ % 10001u) / 10000.0;  // 0..1
        const double f = 1.0 - jitter_ + 2.0 * jitter_ * u;
        return std::max(1, int(double(d) * f));
    }
    int attempts() const { return attempt_; }
    void reset() { attempt_ = 0; }
    // Bounds of the delay the next call to next() can return (tests).
    int nominal() const {
        long long d = base_;
        for (int i = 0; i < attempt_ && d < cap_; ++i) d *= 2;
        return int(std::min<long long>(d, cap_));
    }
    double jitter() const { return jitter_; }

private:
    int base_, cap_;
    double jitter_;
    uint32_t state_;
    int attempt_ = 0;
};

}  // namespace pm::android
