// 0.8.4 自動畫質 (auto_quality.h) offline: the ladder on a model link (a bottleneck of C kbps with a FIFO of
// bufMs; what does not fit is lost), one tick a second as on the PC. No network, no window, no sound.
//   pm_autoq_test            -> prints each case and PASS / FAIL, exit code 0 when all pass
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "auto_quality.h"

using namespace pm::remote;

namespace {
int fails = 0, passes = 0;
void check(bool ok, const char* what, const std::string& detail = {}) {
    std::printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  -- ", detail.c_str());
    (ok ? passes : fails)++;
}

// the host's plan for a 3440x1440 monitor shown on a phone (rel-0836 numbers, 60 fps)
double rungKbps(int i) {
    static const double preset[] = {6000, 10000, 14000};
    return preset[kAutoRungs[i].q] * kAutoRungs[i].pct / 100.0;
}

struct Link {
    std::function<double(int)> capKbps;          // capacity at second t
    std::function<double(int)> lossPct = [](int) { return 0.0; };  // random loss on top
    double baseRttMs = 20, bufMs = 400;
    double queueKbit = 0;
};

struct Run {
    std::vector<int> rung;  // per second
    int changes = 0, downs = 0, ups = 0;
    double maxQueueMs = 0, lostKbit = 0, sentKbit = 0;
};

Run simulate(Link L, int secs, int startRung, bool motion = true, std::function<int(int)> hostDrops = {}, std::function<bool(int)> phoneShort = {}) {
    AutoQuality aq(startRung);
    Run r;
    double rttMin = NAN;
    for (int t = 0; t < secs; ++t) {
        const double C = L.capKbps(t);
        const double send = motion ? rungKbps(aq.rung()) : rungKbps(aq.rung()) * 0.03;
        // fluid FIFO: in = send, out = C; overflow beyond bufMs of C is lost
        double q = L.queueKbit + send - C;
        double lost = 0;
        const double qMax = C * L.bufMs / 1000.0;
        if (q > qMax) lost = q - qMax, q = qMax;
        if (q < 0) q = 0;
        const double delivered = std::min(send + L.queueKbit, C) * (1 - L.lossPct(t) / 100.0);
        L.queueKbit = q;
        const double delayMs = q / C * 1000.0;
        const double lossPct = send > 0 ? std::min(100.0, lost / send * 100.0 + L.lossPct(t)) : L.lossPct(t);
        AutoSignals s;
        s.rttMs = L.baseRttMs + delayMs;
        rttMin = std::isfinite(rttMin) ? std::min(rttMin, s.rttMs) : s.rttMs;
        s.rttMinMs = rttMin;
        s.lossPct = lossPct;
        s.deliveredKbps = delivered;
        s.queuedMs = delayMs;
        s.sentKbps = send;
        s.windowSentKbps = send;
        s.linkReport = true;
        s.sendQueueDrops = hostDrops ? hostDrops(t) : 0;
        s.phoneShort = phoneShort ? phoneShort(t) : false;
        const auto st = aq.tick(s, t * 1000.0, [](int i) { return rungKbps(i); });
        if (st.to != st.from) {
            ++r.changes;
            (st.to > st.from ? r.ups : r.downs)++;
        }
        r.rung.push_back(aq.rung());
        r.maxQueueMs = std::max(r.maxQueueMs, delayMs);
        r.lostKbit += lost;
        r.sentKbit += send;
    }
    return r;
}
int firstAt(const Run& r, int from, std::function<bool(int)> ok) {
    for (int t = from; t < int(r.rung.size()); ++t)
        if (ok(r.rung[size_t(t)])) return t;
    return -1;
}
std::string trace(const Run& r, int from, int to) {
    std::string s;
    for (int t = from; t < to && t < int(r.rung.size()); ++t) s += char('0' + r.rung[size_t(t)]);
    return s;
}
}  // namespace

int main() {
    std::printf("rungs:");
    for (int i = 0; i < kAutoRungCount; ++i) std::printf(" %d=q%d/%d%%(%.0f kbps)", i, kAutoRungs[i].q, kAutoRungs[i].pct, rungKbps(i));
    std::printf("\n");

    std::printf("1) a fast LAN (100 Mbps): starts at rung %d, climbs to the top, stays\n", kAutoStartLan);
    {
        Run r = simulate(Link{[](int) { return 100000.0; }}, 120, kAutoStartLan);
        const int top = firstAt(r, 0, [](int x) { return x == kAutoTop; });
        check(top >= 0 && top <= 6, "top rung within 6 s", "at " + std::to_string(top) + " s, " + trace(r, 0, 12));
        check(r.downs == 0, "never steps down", std::to_string(r.downs) + " downs");
    }
    std::printf("2) from outside on a clean 20 Mbps link: starts at rung %d, top within 10 s\n", kAutoStartOutside);
    {
        Run r = simulate(Link{[](int) { return 20000.0; }, [](int) { return 0.0; }, 60}, 60, kAutoStartOutside);
        const int top = firstAt(r, 0, [](int x) { return x == kAutoTop; });
        check(top >= 0 && top <= 10, "top rung within 10 s", "at " + std::to_string(top) + " s, " + trace(r, 0, 14));
    }
    std::printf("3) a 5 Mbps link (cellular): settles below it, few steps over 10 minutes\n");
    {
        Run r = simulate(Link{[](int) { return 5000.0; }, [](int) { return 0.0; }, 60}, 600, kAutoStartOutside);
        int over = 0;
        for (int t = 60; t < 600; ++t) over += rungKbps(r.rung[size_t(t)]) > 5000;
        const int last = r.rung.back();
        check(rungKbps(last) <= 5000, "ends on a rung that fits (<= 5 Mbps)", "rung " + std::to_string(last) + " (" + std::to_string(int(rungKbps(last))) + " kbps)");
        check(over <= 30, "after the first minute, at most 30 s on a rung over the link", std::to_string(over) + " s over");
        check(r.changes <= 16, "at most 16 steps in 10 minutes (debounce, growing hold)", std::to_string(r.changes) + " steps: " + trace(r, 0, 80) + "...");
    }
    std::printf("4) a squeeze: 50 Mbps, 2.5 Mbps from 60 s to 120 s, 50 Mbps again\n");
    {
        Run r = simulate(Link{[](int t) { return t >= 60 && t < 120 ? 2500.0 : 50000.0; }, [](int) { return 0.0; }, 40}, 240, kAutoStartLan);
        const int down = firstAt(r, 60, [](int x) { return rungKbps(x) <= 2500; });
        check(r.rung[59] == kAutoTop, "at the top before the squeeze");
        check(down >= 60 && down <= 63, "down to a rung that fits within 3 s", "at +" + std::to_string(down - 60) + " s, " + trace(r, 58, 72));
        const int back = firstAt(r, 120, [](int x) { return x == kAutoTop; });
        check(back >= 120 && back <= 120 + 45, "back at the top within 45 s after it", "at +" + std::to_string(back - 120) + " s, " + trace(r, 118, 170));
    }
    std::printf("5) heavy random loss (15 %%) on a fast link: down to the bottom, quickly\n");
    {
        Run r = simulate(Link{[](int) { return 100000.0; }, [](int t) { return t >= 30 ? 15.0 : 0.0; }}, 60, kAutoStartLan);
        const int bottom = firstAt(r, 30, [](int x) { return x == 0; });
        check(bottom >= 30 && bottom <= 36, "bottom rung within 6 s", "at +" + std::to_string(bottom - 30) + " s, " + trace(r, 28, 42));
    }
    std::printf("6) the PC's own send queue overflows: one step down at once\n");
    {
        Run r = simulate(Link{[](int) { return 100000.0; }}, 40, kAutoStartLan, true, [](int t) { return t == 20 ? 3 : 0; });
        check(r.rung[19] == kAutoTop && r.rung[20] == kAutoTop - 1, "down in the same second", trace(r, 16, 26));
    }
    std::printf("7) the phone cannot keep up (decoder): steps down after 2 s, not back up while it says so\n");
    {
        Run r = simulate(Link{[](int) { return 100000.0; }}, 60, kAutoStartLan, true, {}, [](int t) { return t >= 20; });
        check(r.rung[21] == kAutoTop - 1, "down after 2 s", trace(r, 18, 30));
        check(r.rung.back() < kAutoTop - 1 && r.ups == 2, "no climb while the phone is behind", trace(r, 18, 60));
    }
    std::printf("8) a still picture (almost nothing sent) on a slow link: no sign of trouble, it climbs\n");
    {
        Run r = simulate(Link{[](int) { return 3000.0; }}, 30, kAutoStartOutside, false);
        check(r.rung.back() == kAutoTop && r.downs == 0, "top, no step down", trace(r, 0, 30));
    }
    std::printf("9) a link right at a rung's edge (6.2 Mbps): no back-and-forth every few seconds\n");
    {
        Run r = simulate(Link{[](int) { return 6200.0; }, [](int) { return 0.0; }, 50, 200}, 600, kAutoStartLan);
        int flips = 0;
        for (size_t t = 2; t < r.rung.size(); ++t)
            if (r.rung[t] > r.rung[t - 1] && r.rung[t] == r.rung[t - 2]) ++flips;  // down, then up again within 2 s
        check(flips == 0, "never back up within 2 s of a step down", std::to_string(flips));
        check(r.changes <= 16, "at most 16 steps in 10 minutes", std::to_string(r.changes) + ": " + trace(r, 0, 120) + "...");
    }
    std::printf("%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
