// PM: offline replay of the NTP re-latch storm seen after an iPhone resumes a
// paused mirror stream (log 10-08 22:27:57-22:29:17: "NTP offset moved by
// +205.9 / +94.8 / +90.0 / +85.5 ms ... timeline re-latched" every 3 s, each
// move 0.95x the one before = the alpha 0.05 EWMA of raop_ntp.c chasing a
// clock offset that jumped by about 2.7 s; video lead down to -2.6 s).
//
// Simulates the phone clock (offset jump during a pause), NTP samples every
// 3 s with jitter, and video packets at 60 fps through
//   legacy: EWMA filter + "re-latch on every move > 20 ms"
//   fixed : pm_ntp_filter (step on a confirmed jump) + pm::ClockLatch
// and checks that the legacy pipeline reproduces the storm while the fixed
// one re-latches once and keeps the timeline right.
//
//   pm_clock_test     exit code 0 = all checks passed
// GPL-3.0-or-later.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>

extern "C" {
#include "pm_ntp_filter.h"
}
#include "pm_clock_latch.h"

namespace {

constexpr int64_t kSec = 1'000'000'000;
constexpr int64_t kMs = 1'000'000;
constexpr double kAlpha = 0.05;  // RAOP_NTP_ALPHA

int g_fail = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

struct Scenario {
    const char* name;
    double jumpS;        // phone clock offset jump (s)
    int64_t jumpAt;      // when (ns)
    int64_t pauseFrom;   // video paused (no packets) from ... (ns)
    int64_t resumeAt;    // ... to (video_resume)
    int64_t end;
    double driftPpm;     // phone clock drift
};

struct Outcome {
    unsigned relatchesAfterResume = 0;
    double worstLeadMsAfter = 0;  // |lead| from resumeAt + settle allowance to the end
    double finalLeadMs = 0;
};

// true offset T(t) = local - remote (ns)
int64_t trueOffset(const Scenario& sc, int64_t t) {
    int64_t T = 5 * kSec + static_cast<int64_t>(sc.driftPpm * 1e-6 * static_cast<double>(t));
    if (t >= sc.jumpAt) T += static_cast<int64_t>(sc.jumpS * 1e9);
    return T;
}

Outcome run(const Scenario& sc, bool fixed, int64_t settleAllowance) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> jitter(-0.002, 0.002);  // +-2 ms NTP noise

    double ewma = 0;  // legacy filter (s)
    bool haveEwma = false;
    pm_ntp_filter_t filt{};
    double estS = 0;  // current NTP estimate of T (s), what raop_ntp publishes

    // legacy latch (airplay_server.cpp before the fix)
    bool lHave = false;
    int64_t lOff = 0;
    pm::ClockLatch latch;

    Outcome o;
    int64_t nextNtp = 0;
    const int64_t frame = kSec / 60;
    for (int64_t t = 0; t <= sc.end; t += frame) {
        while (t >= nextNtp) {  // one NTP exchange every 3 s
            const double best = static_cast<double>(trueOffset(sc, nextNtp)) / 1e9 + jitter(rng);
            if (fixed) {
                pm_ntp_filter_update(&filt, best, kAlpha);
                estS = filt.smoothed;
            } else {
                ewma = haveEwma ? (1 - kAlpha) * ewma + kAlpha * best : best;
                haveEwma = true;
                estS = ewma;
            }
            nextNtp += 3 * kSec;
        }
        if (fixed && t >= sc.resumeAt && t - frame < sc.resumeAt) latch.onResume();
        if (t >= sc.pauseFrom && t < sc.resumeAt) continue;  // paused: no video packets

        const uint64_t remote = static_cast<uint64_t>(t - trueOffset(sc, t) + 100 * kSec);
        // (remote is shifted by +100 s to stay positive; undone here)
        const uint64_t ntpLocal = static_cast<uint64_t>(static_cast<int64_t>(remote) - 100 * kSec +
                                                        static_cast<int64_t>(std::llround(estS * 1e9)));
        int64_t mapped;
        if (fixed) {
            const auto r = latch.map(t, ntpLocal, remote);
            if (r.event == pm::ClockLatch::Event::Relatched && t >= sc.resumeAt) ++o.relatchesAfterResume;
            mapped = static_cast<int64_t>(r.localNs);
        } else {
            const int64_t off = static_cast<int64_t>(ntpLocal) - static_cast<int64_t>(remote);
            if (!lHave) {
                lOff = off;
                lHave = true;
            } else if (std::llabs(off - lOff) > 20 * kMs) {
                lOff = off;
                if (t >= sc.resumeAt) ++o.relatchesAfterResume;
            }
            mapped = static_cast<int64_t>(remote) + lOff;
        }
        const double leadMs = static_cast<double>(mapped - t) / 1e6;  // 0 = perfect
        if (t >= sc.resumeAt + settleAllowance && std::fabs(leadMs) > o.worstLeadMsAfter)
            o.worstLeadMsAfter = std::fabs(leadMs);
        o.finalLeadMs = leadMs;
    }
    return o;
}

}  // namespace

int main() {
    // 10-08 22:27: offset jump of about 2.7 s while the phone had paused the stream
    const Scenario big{"resume after pause, phone clock offset +2.7 s", 2.7, 98'500 * kMs, 70 * kSec,
                       100 * kSec, 200 * kSec, 0.0};
    std::printf("%s\n", big.name);
    {
        const Outcome legacy = run(big, false, 0);
        std::printf("  legacy: %u re-latches after resume, final lead %+.1f ms\n", legacy.relatchesAfterResume,
                    legacy.finalLeadMs);
        check(legacy.relatchesAfterResume >= 15, "legacy pipeline reproduces the storm (>= 15 re-latches)");
        const Outcome fixed = run(big, true, 7 * kSec);
        std::printf("  fixed : %u re-latch(es) after resume, worst |lead| %.1f ms from resume+7 s, final %+.1f ms\n",
                    fixed.relatchesAfterResume, fixed.worstLeadMsAfter, fixed.finalLeadMs);
        check(fixed.relatchesAfterResume == 1, "fixed pipeline re-latches exactly once");
        check(fixed.worstLeadMsAfter < 25.0, "timeline right (|lead| < 25 ms) from 7 s after the resume");
    }

    // 10-09 00:41: small move (-88 ms -> 0), below the NTP step threshold
    const Scenario small{"resume after pause, phone clock offset +88 ms", 0.088, 98'500 * kMs, 70 * kSec,
                         100 * kSec, 200 * kSec, 0.0};
    std::printf("%s\n", small.name);
    {
        const Outcome legacy = run(small, false, 0);
        std::printf("  legacy: %u re-latches after resume\n", legacy.relatchesAfterResume);
        const Outcome fixed = run(small, true, 30 * kSec);
        std::printf("  fixed : %u re-latch(es) after resume, worst |lead| %.1f ms from resume+30 s, final %+.1f ms\n",
                    fixed.relatchesAfterResume, fixed.worstLeadMsAfter, fixed.finalLeadMs);
        check(fixed.relatchesAfterResume <= 1, "at most one re-latch");
        check(fixed.worstLeadMsAfter < 25.0, "timeline right (|lead| < 25 ms) from 30 s after the resume");
    }

    // steady session, 40 ppm drift for 10 minutes, no pause
    const Scenario drift{"10 min, 40 ppm drift, no pause", 0.0, 1'000'000 * kSec, 1'000'000 * kSec,
                         1'000'000 * kSec, 600 * kSec, 40.0};
    std::printf("%s\n", drift.name);
    {
        Scenario d = drift;
        d.resumeAt = 0;
        d.pauseFrom = d.end + kSec;  // never paused
        const Outcome fixed = run(d, true, 30 * kSec);
        std::printf("  fixed : %u re-latch(es), worst |lead| %.1f ms after 30 s\n", fixed.relatchesAfterResume,
                    fixed.worstLeadMsAfter);
        check(fixed.relatchesAfterResume <= 2, "drift: at most 2 re-latches in 10 min");
        check(fixed.worstLeadMsAfter < 25.0, "drift: |lead| < 25 ms");
    }

    std::printf("NTP filter unit checks\n");
    {
        pm_ntp_filter_t f{};
        pm_ntp_filter_update(&f, 1.000, kAlpha);
        const int s1 = pm_ntp_filter_update(&f, 1.500, kAlpha);  // single outlier
        check(!s1 && std::fabs(f.smoothed - 1.000) < 1e-9, "a single outlier is ignored");
        const int s2 = pm_ntp_filter_update(&f, 1.001, kAlpha);
        check(!s2 && f.far_count == 0, "back to normal resets the outlier count");
        pm_ntp_filter_update(&f, 2.000, kAlpha);
        const int s3 = pm_ntp_filter_update(&f, 3.000, kAlpha);  // two far samples that disagree
        check(!s3, "two disagreeing far samples do not step");
        const int s4 = pm_ntp_filter_update(&f, 3.002, kAlpha);
        check(s4 && std::fabs(f.smoothed - 3.002) < 1e-9, "two agreeing far samples step at once");
        const double before = f.smoothed;
        pm_ntp_filter_update(&f, before + 0.010, kAlpha);
        check(std::fabs(f.smoothed - (before + 0.0005)) < 1e-9, "small moves are blended with alpha 0.05");
    }

    std::printf("ClockLatch unit checks\n");
    {
        pm::ClockLatch c;
        auto r = c.map(1 * kSec, 0, 500 * kSec);
        check(r.event == pm::ClockLatch::Event::Provisional, "provisional offset before NTP");
        r = c.map(2 * kSec, 600 * kSec, 501 * kSec);
        check(r.event == pm::ClockLatch::Event::NtpEstablished && r.localNs == 600 * kSec, "NTP replaces provisional");
        // offset is now 99 s (local - remote)
        r = c.map(3 * kSec, 601 * kSec + 15 * kMs, 502 * kSec);
        check(r.event == pm::ClockLatch::Event::None && r.localNs == 601 * kSec, "move <= 20 ms ignored");
        r = c.map(4 * kSec, 602 * kSec + 100 * kMs, 503 * kSec);
        check(r.event == pm::ClockLatch::Event::Relatched && r.localNs == 602 * kSec + 100 * kMs, "move > 20 ms steps");
        r = c.map(5 * kSec, 603 * kSec + 200 * kMs, 504 * kSec);  // another 100 ms, inside the settle window
        check(r.event == pm::ClockLatch::Event::None && r.localNs == 603 * kSec + 105 * kMs,
              "inside the settle window: slewed 5 ms per s, no step");
        c.onResume();
        r = c.map(6 * kSec, 604 * kSec + 500 * kMs, 505 * kSec);
        check(r.event == pm::ClockLatch::Event::Relatched, "onResume allows one step inside the window");
        r = c.map(7 * kSec, 605 * kSec + 900 * kMs, 506 * kSec);
        check(r.event == pm::ClockLatch::Event::None, "... but only one");
        r = c.map(27 * kSec, 625 * kSec + 505 * kMs, 526 * kSec);  // NTP agrees with the slewed offset
        check(r.event == pm::ClockLatch::Event::Settled, "settle window ends with one summary event");
        check(c.relatchCount() == 2, "re-latch count");
    }

    std::printf(g_fail ? "FAILED (%d)\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
