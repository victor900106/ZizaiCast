#pragma once
// 0.8.4 自動畫質: what the phone's receiver reports (RTCP RR) say about the video path, on the PC.
// The last handler of the video track's chain: it sees every RTP packet and sender report going out and
// every RTCP packet coming in, and changes none of them.
//   RTT       = RR arrival - when the sender report it answers (LSR) left - the phone's delay (DLSR); the
//               reports are libdatachannel's (RtcpSrReporter, stamped with the system clock)
//   loss      = the RR's fraction lost
//   delivered = bytes we sent up to the RR's highest sequence number, between two RRs, over their time
//   queued    = bytes sent but not reported received yet, in time at the delivered rate
// The RR's come on the phone -> PC direction: they arrive even when the PC -> phone direction is clogged.

#include <rtc/rtc.hpp>

#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>

namespace pm::remote {

class RtcpStats final : public rtc::MediaHandler {
public:
    struct Snapshot {
        double rttMs = NAN, rttMinMs = NAN, lossPct = NAN, deliveredKbps = NAN, queuedMs = NAN, jitterMs = NAN;
        double sentKbps = NAN;  // what we sent over the same window as deliveredKbps
        int64_t reports = 0;  // RRs seen so far
        double ageMs = NAN;   // since the last RR
    };
    explicit RtcpStats(uint32_t clockRate = 90000) : clock_(clockRate) {}
    void outgoing(rtc::message_vector& messages, const rtc::message_callback& send) override;
    void incoming(rtc::message_vector& messages, const rtc::message_callback& send) override;
    Snapshot snapshot(int64_t nowUs);
    // Tests: feed raw packets as if they went out / came in at this time.
    void noteOutgoing(const uint8_t* p, size_t n, int64_t nowUs);
    void noteIncoming(const uint8_t* p, size_t n, int64_t nowUs);

private:
    struct Sent {
        uint16_t seq;
        int64_t cumBytes;
    };
    struct Sr {
        uint32_t mid;  // middle 32 bits of its NTP time (what an RR's LSR repeats)
        int64_t us;
    };
    std::mutex m_;
    uint32_t clock_;
    std::deque<Sent> sent_;  // the last 16384 RTP packets
    std::deque<Sr> srs_;     // the last 32 sender reports
    int64_t cumBytes_ = 0;
    int64_t reports_ = 0, lastRrUs_ = 0;
    double rttMs_ = NAN, rttMinMs_ = NAN, lossPct_ = NAN, jitterMs_ = NAN, deliveredKbps_ = NAN, queuedMs_ = NAN, sentKbps_ = NAN;
    int64_t prevRrUs_ = 0, prevRrBytes_ = -1, prevRrSent_ = 0;
    int64_t rttMinAtUs_ = 0;
};

}  // namespace pm::remote
