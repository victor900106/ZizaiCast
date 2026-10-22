#include "rtcp_stats.h"

#include <algorithm>
#include <chrono>

#include "util.h"

namespace pm::remote {

namespace {
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
bool isRtcp(const uint8_t* p, size_t n) { return n >= 8 && (p[0] >> 6) == 2 && p[1] >= 192 && p[1] <= 223; }
// The middle 32 bits of the NTP time now, as libdatachannel's RtcpSrReporter stamps its sender reports
// (the system clock; it sends them itself, past this handler): what an RR's LSR repeats.
uint32_t ntpMiddleNow() {
    const double secs = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count() + 2208988800.0;
    return uint32_t(uint64_t(std::floor(secs * 65536.0)) & 0xFFFFFFFFu);
}
}  // namespace

void RtcpStats::outgoing(rtc::message_vector& messages, const rtc::message_callback&) {
    const int64_t now = nowUs();
    for (const auto& m : messages)
        if (m && !m->empty()) noteOutgoing(reinterpret_cast<const uint8_t*>(m->data()), m->size(), now);
}

void RtcpStats::incoming(rtc::message_vector& messages, const rtc::message_callback&) {
    const int64_t now = nowUs();
    for (const auto& m : messages)
        if (m && m->type == rtc::Message::Control && !m->empty()) noteIncoming(reinterpret_cast<const uint8_t*>(m->data()), m->size(), now);
}

void RtcpStats::noteOutgoing(const uint8_t* p, size_t n, int64_t now) {
    if (n < 12 || (p[0] >> 6) != 2) return;
    std::lock_guard<std::mutex> lk(m_);
    if (isRtcp(p, n)) {  // our sender report(s), possibly in a compound packet
        for (size_t off = 0; off + 8 <= n;) {
            const size_t len = (size_t(be16(p + off + 2)) + 1) * 4;
            if (off + len > n) break;
            if (p[off + 1] == 200 && len >= 28) {
                srs_.push_back(Sr{be32(p + off + 10), now});  // NTP: seconds at +8, fraction at +12: the middle 32 bits
                if (srs_.size() > 32) srs_.pop_front();
            }
            off += len;
        }
        return;
    }
    cumBytes_ += int64_t(n);
    sent_.push_back(Sent{be16(p + 2), cumBytes_});
    if (sent_.size() > 16384) sent_.pop_front();
}

void RtcpStats::noteIncoming(const uint8_t* p, size_t n, int64_t now) {
    if (!isRtcp(p, n)) return;
    std::lock_guard<std::mutex> lk(m_);
    for (size_t off = 0; off + 8 <= n;) {
        const size_t len = (size_t(be16(p + off + 2)) + 1) * 4;
        if (off + len > n) break;
        const int pt = p[off + 1], rc = p[off] & 0x1F;
        // RR (201): sender SSRC, then report blocks; an SR (200) from the phone carries them after 20 bytes of sender info
        const size_t first = pt == 201 ? 8 : pt == 200 ? 28 : 0;
        if (first && rc > 0 && off + first + 24 <= off + len) {
            const uint8_t* b = p + off + first;  // the first block (ours: the phone only receives our one video stream)
            const double lost = b[4] / 256.0 * 100.0;
            const uint16_t hi = be16(b + 10);
            const uint32_t jitter = be32(b + 12), lsr = be32(b + 16), dlsr = be32(b + 20);
            ++reports_;
            lossPct_ = lost;
            jitterMs_ = jitter * 1000.0 / clock_;
            if (lsr) {
                // our own sender report (noted on the way out, tests) or the system clock (the reporter's)
                double rtt = NAN;
                for (auto it = srs_.rbegin(); it != srs_.rend(); ++it)
                    if (it->mid == lsr) {
                        rtt = (now - it->us) / 1000.0 - dlsr * 1000.0 / 65536.0;
                        break;
                    }
                if (!std::isfinite(rtt) && srs_.empty()) rtt = double(uint32_t(ntpMiddleNow() - lsr - dlsr)) * 1000.0 / 65536.0;
                if (std::isfinite(rtt) && rtt > -5 && rtt < 60000) {
                    rttMs_ = std::max(0.0, rtt);
                    // the path without a queue: the lowest RTT, forgotten slowly (a route change)
                    if (!std::isfinite(rttMinMs_) || rttMs_ < rttMinMs_ || now - rttMinAtUs_ > 30000000)
                        rttMinMs_ = rttMs_, rttMinAtUs_ = now;
                }
            }
            // bytes up to the highest sequence number the phone has: the newest packet we sent with it
            int64_t got = -1;
            for (auto it = sent_.rbegin(); it != sent_.rend(); ++it)
                if (it->seq == hi) {
                    got = it->cumBytes;
                    break;
                }
            if (got >= 0) {
                if (prevRrBytes_ >= 0 && got >= prevRrBytes_ && now - prevRrUs_ >= 200000) {
                    const double ms = (now - prevRrUs_) / 1000.0;
                    deliveredKbps_ = double(got - prevRrBytes_) * 8.0 / ms;
                    sentKbps_ = double(cumBytes_ - prevRrSent_) * 8.0 / ms;  // the same window
                    prevRrBytes_ = got, prevRrUs_ = now, prevRrSent_ = cumBytes_;
                } else if (prevRrBytes_ < 0 || got < prevRrBytes_) {
                    prevRrBytes_ = got, prevRrUs_ = now, prevRrSent_ = cumBytes_;
                }
                const double behind = double(cumBytes_ - got);
                const double rate = std::isfinite(deliveredKbps_) && deliveredKbps_ > 50 ? deliveredKbps_ : NAN;
                queuedMs_ = std::isfinite(rate) ? behind * 8.0 / rate : NAN;  // kbps = bits per ms
            }
            lastRrUs_ = now;
        }
        off += len;
    }
}

RtcpStats::Snapshot RtcpStats::snapshot(int64_t now) {
    std::lock_guard<std::mutex> lk(m_);
    Snapshot s;
    s.reports = reports_;
    s.ageMs = lastRrUs_ ? (now - lastRrUs_) / 1000.0 : NAN;
    if (!lastRrUs_ || now - lastRrUs_ > 3000000) return s;  // too old to judge by
    s.rttMs = rttMs_, s.rttMinMs = rttMinMs_, s.lossPct = lossPct_, s.jitterMs = jitterMs_;
    s.deliveredKbps = deliveredKbps_, s.sentKbps = sentKbps_, s.queuedMs = queuedMs_;
    return s;
}

}  // namespace pm::remote
