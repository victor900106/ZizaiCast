// Internal: decides when a Miracast cast's picture is frozen and what to do
// (pure; unit-tested by pm_miracast_test --selftest).
//
// Fed every ~500 ms with the FramePump counters:
//   * pictures arrive (VideoFrameAvailable) but none reaches the window for
//     pipelineMs  -> our pipeline is stuck (copy / read-back)      -> Retry
//   * nothing arrives at all for stallMs (firstFrameMs before the first
//     picture)                                                     -> Retry
//   * MediaPlayer reported MediaFailed / MediaEnded                -> Retry now
// Retries back off exponentially: after retry n the next check waits
// min(retryCapMs, retryBaseMs * 2^(n-1)) for a picture; after `retries`
// failed retries -> GiveUp (the receiver disconnects the cast and says why).
// A picture resets the wait; stableMs of pictures after a retry forgets it.
#pragma once

#include <algorithm>
#include <cstdint>

namespace pm::miracast {

class FreezeDetector {
public:
    struct Config {
        int firstFrameMs = 15000;
        int stallMs = 10000;
        int pipelineMs = 3000;
        int retries = 3;
        int retryBaseMs = 2000;
        int retryCapMs = 8000;
        int stableMs = 10000;
    };
    enum class Action { None, Retry, GiveUp };

    FreezeDetector() = default;
    explicit FreezeDetector(Config c) : cfg_(c) {}

    // A cast's media source started playing.
    void start(int64_t nowMs) {
        lastDelivered_ = lastAvailable_ = -1;
        lastProgress_ = lastArrival_ = nowMs;
        everDelivered_ = false;
        retries_ = 0;
        waitMs_ = 0;
        lastRetry_ = 0;
        gaveUp_ = false;
    }

    Action tick(int64_t nowMs, long long delivered, long long available, bool failed) {
        if (gaveUp_) return Action::None;
        if (available != lastAvailable_) {
            lastAvailable_ = available;
            lastArrival_ = nowMs;
        }
        if (delivered != lastDelivered_) {
            const bool first = lastDelivered_ < 0;
            lastDelivered_ = delivered;
            if (!first || delivered > 0) {
                if (!first) everDelivered_ = true;
                lastProgress_ = nowMs;
                waitMs_ = 0;
                if (retries_ && nowMs - lastRetry_ >= cfg_.stableMs) retries_ = 0;  // recovered for good
                if (!failed) return Action::None;
            }
        }
        if (!failed) {
            const int64_t quiet = nowMs - lastProgress_;
            const bool stuckPipeline = everDelivered_ && waitMs_ == 0 && quiet >= cfg_.pipelineMs &&
                                       nowMs - lastArrival_ < cfg_.pipelineMs;  // pictures arrive, none shown
            const int limit = waitMs_ ? waitMs_ : everDelivered_ ? cfg_.stallMs : cfg_.firstFrameMs;
            if (!stuckPipeline && quiet < limit) return Action::None;
        }
        if (retries_ >= cfg_.retries) {
            gaveUp_ = true;
            return Action::GiveUp;
        }
        ++retries_;
        lastRetry_ = lastProgress_ = nowMs;
        long long w = cfg_.retryBaseMs;
        for (int i = 1; i < retries_ && w < cfg_.retryCapMs; ++i) w *= 2;
        waitMs_ = int(std::min<long long>(w, cfg_.retryCapMs));
        return Action::Retry;
    }

    int retries() const { return retries_; }
    // How long the detector waits for a picture after the last retry (tests).
    int waitMs() const { return waitMs_; }

private:
    Config cfg_;
    long long lastDelivered_ = -1, lastAvailable_ = -1;
    int64_t lastProgress_ = 0, lastArrival_ = 0, lastRetry_ = 0;
    bool everDelivered_ = false;
    bool gaveUp_ = false;
    int retries_ = 0;
    int waitMs_ = 0;
};

}  // namespace pm::miracast
