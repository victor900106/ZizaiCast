// PM: remote (sender) timestamp -> local time mapping with one offset shared by
// audio and video (pm::AirPlayServer::Impl::toLocal). Pure logic, no locking:
// the caller serialises calls. Unit-tested in tests/pm_clock_test.cpp.
//
// The offset is latched from the NTP clock sync at the first synced packet.
// Later NTP moves:
//   * |move| <= kRelatchNs: ignored (no jitter from NTP updates), except in
//     the settle window below, where it is slewed out.
//   * |move| >  kRelatchNs: the timeline is re-latched (stepped) once, then a
//     settle window of kSettleNs starts in which the offset is only slewed
//     (at most kSlewNsPerSec per second of wall time), never stepped again.
//     After an iPhone pause/resume the NTP estimate used to move by 85-205 ms
//     every 3 s for about a minute; each move re-based the timeline
//     ("NTP offset moved by ... re-latched" storm, video lead down to -2.6 s).
//   * onResume() (video_resume) arms one immediate re-latch even inside a
//     settle window: the clock jump of a resume is real and must not be slewed.
// When a settle window ends, one Settled event reports what was slewed.
// GPL-3.0-or-later.
#pragma once

#include <cstdint>
#include <cstdlib>

namespace pm {

class ClockLatch {
public:
    static constexpr int64_t kRelatchNs = 20'000'000;      // step threshold (20 ms)
    static constexpr int64_t kSettleNs = 20'000'000'000;   // no second step within 20 s
    static constexpr int64_t kSlewNsPerSec = 5'000'000;    // slew 5 ms per s (0.5 %)

    enum class Event {
        None,
        Provisional,     // no NTP yet: offset from the arrival time
        NtpLatched,      // first packet, NTP already synced
        NtpEstablished,  // provisional offset replaced by NTP
        Relatched,       // step (deltaNs = the step)
        Settled,         // settle window over (deltaNs = slewed total, residualNs = left over)
    };
    struct Result {
        uint64_t localNs = 0;
        Event event = Event::None;
        int64_t deltaNs = 0;
        int64_t residualNs = 0;
    };

    void reset() { *this = ClockLatch{}; }

    // video_resume: the next NTP move over kRelatchNs is applied at once.
    void onResume() { resumeArmed_ = true; }

    bool haveOffset() const { return have_; }
    bool fromNtp() const { return fromNtp_; }
    int64_t offsetNs() const { return offset_; }
    unsigned relatchCount() const { return relatches_; }

    // nowNs: local clock (same base as ntpLocal); ntpLocal: ntpRemote mapped
    // with the current NTP estimate (0 = not synced yet); ntpRemote != 0.
    Result map(int64_t nowNs, uint64_t ntpLocal, uint64_t ntpRemote) {
        Result r;
        const int64_t dt = lastNs_ && nowNs > lastNs_ ? nowNs - lastNs_ : 0;
        lastNs_ = nowNs;
        if (ntpLocal) {
            const int64_t off = static_cast<int64_t>(ntpLocal) - static_cast<int64_t>(ntpRemote);
            const int64_t delta = off - offset_;
            if (!have_ || !fromNtp_) {
                r.event = have_ ? Event::NtpEstablished : Event::NtpLatched;
                r.deltaNs = have_ ? delta : 0;
                offset_ = off;
                have_ = fromNtp_ = true;
            } else if (std::llabs(delta) > kRelatchNs && (resumeArmed_ || nowNs >= settleUntil_)) {
                r.event = Event::Relatched;
                r.deltaNs = delta;
                offset_ = off;
                ++relatches_;
                resumeArmed_ = false;
                settleUntil_ = nowNs + kSettleNs;
                slewed_ = 0;
            } else if (settleUntil_) {
                if (nowNs < settleUntil_) {
                    const int64_t cap = dt * kSlewNsPerSec / 1'000'000'000;
                    const int64_t step = delta > cap ? cap : (delta < -cap ? -cap : delta);
                    offset_ += step;
                    slewed_ += step;
                } else {
                    r.event = Event::Settled;
                    r.deltaNs = slewed_;
                    r.residualNs = delta;
                    settleUntil_ = 0;
                    slewed_ = 0;
                }
            }
        } else if (!have_) {
            offset_ = nowNs - static_cast<int64_t>(ntpRemote);
            have_ = true;
            fromNtp_ = false;
            r.event = Event::Provisional;
        }
        r.localNs = static_cast<uint64_t>(static_cast<int64_t>(ntpRemote) + offset_);
        return r;
    }

private:
    bool have_ = false;
    bool fromNtp_ = false;
    bool resumeArmed_ = false;
    int64_t offset_ = 0;       // local - remote (ns)
    int64_t settleUntil_ = 0;  // 0 = no settle window
    int64_t slewed_ = 0;
    int64_t lastNs_ = 0;
    unsigned relatches_ = 0;
};

}  // namespace pm
