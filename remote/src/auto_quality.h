#pragma once
// 0.8.4 自動畫質: the picture's quality follows the network, on the PC (no round trip through the phone:
// when the downlink is clogged the phone's own messages are stuck behind the video too, measured 16 s).
//
// A ladder of rungs (preset + a share of its bitrate). A session in 自動 starts low (a small first key frame:
// the first picture comes sooner on a slow link) and climbs while the link stays clean; it steps down at
// once on a clear sign of congestion (the delay grows, packets are lost, the PC's own send queue overflows)
// and after two seconds in a row of milder signs. Debounce: a climb needs a run of clean seconds, never
// sooner than a hold after the last step down; a climb that is followed by a step down within 15 s
// (the link could not take it) doubles that hold (10 s ... 2 minutes), a minute without trouble resets it.
// A preset chosen by hand on the phone is kept as it is (this is not used then).
//
// Pure logic: no clock, no threads (the caller passes the time). Unit test: tools/autoq_test.cpp.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace pm::remote {

struct AutoRung {
    int q;    // picture preset (qualityPreset level)
    int pct;  // share of that preset's bitrate (%)
};
// 流暢 at 35 / 60 / 100 %, 標準, 清晰.
constexpr AutoRung kAutoRungs[] = {{0, 35}, {0, 60}, {0, 100}, {1, 100}, {2, 100}};
constexpr int kAutoRungCount = int(sizeof(kAutoRungs) / sizeof(kAutoRungs[0]));
constexpr int kAutoTop = kAutoRungCount - 1;
// Where a session starts (owner, 0.8.4: low, then up): on the LAN 流暢 at its full bitrate, from outside
// (the fixed / temporary address, Tailscale) 流暢 at 60 %.
constexpr int kAutoStartLan = 2, kAutoStartOutside = 1;

// What the PC knows about the link, once a second. NaN = not known (no report yet / too old).
struct AutoSignals {
    double rttMs = NAN;        // RTCP RR (the phone's receiver report on our sender report)
    double rttMinMs = NAN;     // the lowest RTT of this session (the path without a queue)
    double lossPct = NAN;      // RTCP RR fraction lost (since its previous report)
    double deliveredKbps = NAN;  // what reached the phone (RTCP RR highest sequence number, bytes we sent up to it)
    double queuedMs = NAN;     // bytes sent but not yet reported received, in time at the delivered rate
    double sentKbps = 0;       // what the PC sent in this second
    double windowSentKbps = NAN;  // what it sent over the same window as deliveredKbps
    bool linkReport = false;   // a fresh report on the link (an RR, the tunnel's acks): without one a busy link is not "clean"
    int sendQueueDrops = 0;    // frames the PC's sender dropped (its queue was full) in this second
    bool tunnelFull = false;   // 外網連結 WebSocket: frames dropped / not acknowledged in time
    double dcBufferedKB = 0;   // the DataChannel's unsent bytes (SCTP is congestion controlled: a full path)
    bool phoneShort = false;   // the phone shows far fewer frames than the PC sends (it cannot keep up)
};

class AutoQuality {
public:
    struct Step {
        int from = -1, to = -1;  // rung before / after (to == from: nothing to do)
        std::string why;         // for the log / the phone ("delay", "loss", "queue", "clean", ...)
    };

    explicit AutoQuality(int startRung = kAutoStartLan, int top = kAutoTop) { reset(startRung, top); }
    void reset(int startRung, int top) {
        top_ = std::clamp(top, 0, kAutoTop);
        rung_ = std::clamp(startRung, 0, top_);
        bad_ = good_ = 0;
        holdUntilMs_ = 0, lastUpMs_ = -1e18, lastDownMs_ = -1e18, lastChangeMs_ = -1e18;
        backoffMs_ = kHoldMinMs;
        startup_ = true;
        bwKbps_ = NAN, bwAtMs_ = -1e18;
        prevDelay_ = NAN;
    }
    int rung() const { return rung_; }
    int top() const { return top_; }
    void setTop(int top) { top_ = std::clamp(top, 0, kAutoTop), rung_ = std::min(rung_, top_); }
    bool startup() const { return startup_; }
    double holdMs(double nowMs) const { return std::max(0.0, holdUntilMs_ - nowMs); }
    double bandwidthKbps() const { return bwKbps_; }
    // The last verdict (log / phone): "clean", "delay", "loss", "saturated", "queue", "phone", "quiet"
    const std::string& verdict() const { return verdict_; }

    // One second of signals. rungKbps(i): the bitrate rung i would get now (the host's plan).
    template <class KbpsOf>
    Step tick(const AutoSignals& s, double nowMs, KbpsOf rungKbps) {
        Step st{rung_, rung_, {}};
        const bool settling = nowMs - lastChangeMs_ < kSettleMs;  // a new size: its key frame is a burst
        const double base = std::isfinite(s.rttMinMs) ? s.rttMinMs : (std::isfinite(s.rttMs) ? s.rttMs : 0);
        const double qd = std::isfinite(s.rttMs) ? std::max(0.0, s.rttMs - base) : NAN;
        const double loss = std::isfinite(s.lossPct) ? s.lossPct : 0;
        const bool busy = s.sentKbps > 0.4 * rungKbps(rung_);  // enough traffic to judge the link by
        // just stepped down and the queue the step before left is draining: the delay is old news
        const double delayNow = std::isfinite(qd) ? qd : (std::isfinite(s.queuedMs) ? s.queuedMs : NAN);
        // (or, for 3 s after a step down to a rung well under the capacity measured, still arriving)
        const bool underCap = std::isfinite(bwKbps_) && nowMs - bwAtMs_ < 10000 && rungKbps(rung_) < 0.9 * bwKbps_;
        const bool draining = (nowMs - lastDownMs_ < kDrainMs && std::isfinite(delayNow) && std::isfinite(prevDelay_) && delayNow < prevDelay_ - 5) ||
                              (nowMs - lastDownMs_ < kPostDownMs && underCap);
        prevDelay_ = delayNow;
        const bool delayBad = !draining && std::isfinite(qd) && qd > std::max(kDelayBadMs, base * 0.5);
        const bool delaySevere = !draining && std::isfinite(qd) && qd > std::max(kDelaySevereMs, base);
        const bool queueBad = !draining && std::isfinite(s.queuedMs) && busy && s.queuedMs > kQueueBadMs;
        const bool queueSevere = !draining && std::isfinite(s.queuedMs) && s.queuedMs > kQueueSevereMs;
        // more goes in than comes out, and it piles up
        const bool saturated = busy && std::isfinite(s.deliveredKbps) && std::isfinite(s.windowSentKbps) &&
                               s.deliveredKbps < 0.75 * s.windowSentKbps && (!std::isfinite(s.queuedMs) || s.queuedMs > 60);
        const bool hostQueue = s.sendQueueDrops > 0 || s.tunnelFull || s.dcBufferedKB > kDcBufSevereKB;
        // the link's capacity: what got through while it was full
        if ((saturated || delayBad || queueBad) && std::isfinite(s.deliveredKbps) && s.deliveredKbps > 100) {
            bwKbps_ = std::isfinite(bwKbps_) && nowMs - bwAtMs_ < 5000 ? 0.5 * bwKbps_ + 0.5 * s.deliveredKbps : s.deliveredKbps;
            bwAtMs_ = nowMs;
        }
        const bool bad = hostQueue || delaySevere || queueSevere || loss > kLossSeverePct || delayBad || queueBad || saturated || loss > kLossBadPct || s.phoneShort;
        const bool clean = !bad && loss < kLossCleanPct && (!std::isfinite(qd) || qd < kDelayCleanMs) && (!busy || s.linkReport);
        verdict_ = hostQueue ? "queue" : (delaySevere || delayBad) ? "delay" : (queueSevere || queueBad) ? "queue"
                 : loss > kLossBadPct ? "loss" : saturated ? "saturated" : s.phoneShort ? "phone" : clean ? (busy ? "clean" : "quiet") : "unsure";
        if (bad) ++bad_, good_ = 0;
        else if (clean) ++good_, bad_ = 0;
        else bad_ = good_ = 0;
        // down: at once on a clear sign, else after 2 s of milder ones. While a new size settles (its key
        // frame is a burst) the delay alone waits; a full queue or heavy loss does not.
        const bool severeNow = hostQueue || queueSevere || loss > kLossSeverePct || (delaySevere && !settling);
        if (severeNow || (bad_ >= 2 && !settling)) {
            if (rung_ > 0) {
                int to = rung_ - 1;
                // one step would still be far over what gets through: straight to the rung that fits (85 % of
                // the measured capacity)
                if (std::isfinite(bwKbps_) && nowMs - bwAtMs_ < 3000 && rungKbps(to) > 1.2 * bwKbps_)
                    while (to > 0 && rungKbps(to) > 0.85 * bwKbps_) --to;
                const bool probeFailed = nowMs - lastUpMs_ < kProbeFailMs;
                backoffMs_ = probeFailed ? std::min(backoffMs_ * 2, kHoldMaxMs) : std::max(backoffMs_, kHoldMinMs);
                holdUntilMs_ = nowMs + backoffMs_;
                lastDownMs_ = lastChangeMs_ = nowMs;
                startup_ = false;
                st.to = rung_ = to;
                st.why = verdict_;
            }
            bad_ = good_ = 0;
            return st;
        }
        // the last climb held for a minute: the hold is short again
        if (lastUpMs_ > lastDownMs_ && nowMs - lastUpMs_ > kCalmResetMs) backoffMs_ = kHoldMinMs;
        // up: a run of clean seconds (2 while starting, 4 after a step down), never inside the hold,
        // never to a rung well above what the link carried when it was last full (for a minute)
        const int need = startup_ ? kUpTicksStartup : kUpTicks;
        if (good_ >= need && rung_ < top_ && nowMs >= holdUntilMs_ && !settling) {
            const bool capKnown = std::isfinite(bwKbps_) && nowMs - bwAtMs_ < kCapMemoryMs;
            if (!capKnown || rungKbps(rung_ + 1) <= 1.1 * bwKbps_) {
                st.to = ++rung_;
                st.why = busy ? "clean" : "quiet";
                lastUpMs_ = lastChangeMs_ = nowMs;
                good_ = 0;
            }
        }
        return st;
    }

    static constexpr double kDelayBadMs = 80, kDelaySevereMs = 250, kDelayCleanMs = 40;
    static constexpr double kQueueBadMs = 150, kQueueSevereMs = 400;
    static constexpr double kLossBadPct = 4, kLossSeverePct = 12, kLossCleanPct = 1.5;
    static constexpr double kDcBufSevereKB = 512;
    static constexpr double kSettleMs = 1500, kProbeFailMs = 15000, kHoldMinMs = 10000, kHoldMaxMs = 120000;
    static constexpr double kCalmResetMs = 60000, kCapMemoryMs = 60000, kDrainMs = 8000, kPostDownMs = 3000;
    static constexpr int kUpTicksStartup = 2, kUpTicks = 4;

private:
    int rung_ = 0, top_ = kAutoTop;
    int bad_ = 0, good_ = 0;
    double holdUntilMs_ = 0, lastUpMs_ = 0, lastDownMs_ = 0, lastChangeMs_ = 0, backoffMs_ = kHoldMinMs;
    bool startup_ = true;
    double bwKbps_ = NAN, bwAtMs_ = 0;
    double prevDelay_ = NAN;
    std::string verdict_ = "quiet";
};

}  // namespace pm::remote
