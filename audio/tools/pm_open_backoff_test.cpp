// pm_open_backoff_test (0.8.4): the WASAPI retry schedule and its log merging (src/open_backoff.h). No device.
#include "../src/open_backoff.h"

#include <cstdio>

static int fails = 0;
#define CHECK(c)                                              \
    do {                                                      \
        if (!(c)) {                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #c); \
            ++fails;                                          \
        }                                                     \
    } while (0)

int main() {
    using pm::OpenBackoff;
    OpenBackoff b;
    const int want[] = {1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000, 60000};
    int64_t now = 0;
    unsigned logged = 0;
    for (int i = 0; i < 9; ++i) {
        auto v = b.onFail(0x80070490, now);
        CHECK(v.delayMs == want[i]);
        CHECK(v.log == (i == 0));  // the same error: logged once
        logged += v.log;
        now += v.delayMs;
    }
    CHECK(logged == 1);
    // 16 s of packets used to mean ~1600 tries; with the schedule: tries at 0, 1, 3, 7, 15 s = 5
    {
        OpenBackoff c;
        int64_t t = 0;
        int n = 0;
        while (t <= 16000) { ++n; t += c.onFail(0x80070490, t).delayMs; }
        CHECK(n == 5);
    }
    // a different error is logged at once; the same one again after 10 minutes, with the count
    auto d = b.onFail(0x88890004, now);
    CHECK(d.log && d.sinceLog == 9);  // 8 unlogged tries of the first error + this one
    now += 600000;
    auto e = b.onFail(0x88890004, now);
    CHECK(e.log && e.sinceLog == 1 && e.delayMs == 60000);
    auto f = b.onFail(0x88890004, now + 1000);
    CHECK(!f.log);
    auto g = b.onFail(0x88890004, now + 600000);
    CHECK(g.log && g.sinceLog == 2);
    // recovery reports the spell and restarts the schedule
    OpenBackoff h;
    h.onFail(1, 0), h.onFail(1, 1000);
    CHECK(h.onSuccess() == 2);
    CHECK(h.onFail(1, 5000).delayMs == 1000);
    CHECK(h.onSuccess() == 1);
    CHECK(h.onSuccess() == 0);
    std::printf(fails ? "FAILED (%d)\n" : "PASS pm_open_backoff_test\n", fails);
    return fails ? 1 : 0;
}
