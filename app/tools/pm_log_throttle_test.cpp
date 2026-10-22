// pm_log_throttle_test (0.8.4): which log lines are written (core/base/log_throttle.h). No files, no clock.
#include "core/base/log_throttle.h"

#include <cstdio>
#include <string>

using pm_app::LogThrottle;
static int fails = 0;
#define CHECK(c)                                              \
    do {                                                      \
        if (!(c)) {                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #c); \
            ++fails;                                          \
        }                                                     \
    } while (0)

int main() {
    {  // a chatty kind: once per 30 s, the count of the left-out ones rides on the next written line
        LogThrottle t;
        int written = 0;
        unsigned lastSup = 0;
        for (int i = 0; i < 60; ++i) {  // 60 x 5 s = 300 s of "[video-timing]"
            auto v = t.check("info", "[video-timing] 5.0 s: 300 frames", i * 5);
            if (v.write) { ++written; lastSup = v.suppressed; }
        }
        CHECK(written == 10);  // t = 0, 30, 60 ... 270
        CHECK(lastSup == 5);
    }
    {  // level-specific rule: [video] AU lines throttled, the same text at another level is not
        LogThrottle t;
        CHECK(t.check("video", "5s: AU in 100", 0).write);
        CHECK(!t.check("video", "10s: AU in 101", 5).write);
        CHECK(t.check("remote", "10s: AU in 101", 5).write);
        CHECK(t.check("video", "35s: AU in 102", 35).write);
    }
    {  // the same line again and again: one line + "repeated K more times" when something else comes
        LogThrottle t;
        const std::string m = "WASAPI open failed (hr=0x80070490); retrying";
        CHECK(t.check("audio", m, 100).write);
        for (int i = 0; i < 200; ++i) CHECK(!t.check("audio", m, 100 + i / 100).write);
        auto v = t.check("audio", "default audio device changed", 103);
        CHECK(v.write && v.flushDup == 200);
        CHECK(t.check("audio", m, 104).write);  // a different line in between: the run starts again
    }
    {  // remote lines differ only by their own time stamp
        LogThrottle t;
        CHECK(t.check("remote", "22:57:46.852 [remote] input: SendInput failed (blocked by UIPI?)", 1).write);
        CHECK(!t.check("remote", "22:57:46.869 [remote] input: SendInput failed (blocked by UIPI?)", 1).write);
        CHECK(!t.check("remote", "22:57:46.884 [remote] input: SendInput failed (blocked by UIPI?)", 1).write);
        CHECK(t.check("remote", "22:57:47.001 [remote] input: left click at 3,4", 1).flushDup == 2);
        CHECK(t.check("remote", "22:57:47.002 [remote] input: left click at 5,6", 1).write);  // other numbers: written
    }
    {  // a long run is not silent forever: after 60 s one line says how many were left out
        LogThrottle t;
        CHECK(t.check("info", "same", 0).write);
        CHECK(!t.check("info", "same", 30).write);
        auto v = t.check("info", "same", 61);
        CHECK(v.write && v.dupNow == 1);
        CHECK(!t.check("info", "same", 62).write);
    }
    {  // the kernel-timestamps how-to: once an hour
        LogThrottle t;
        const std::string m = "*** Windows support for kernel timestamps on NetAdapter \"x\" is \"Disabled\":\nTo enable it";
        CHECK(t.check("info", m, 0).write);
        CHECK(!t.check("info", m, 26).write);
        CHECK(!t.check("info", m, 3599).write);
        CHECK(t.check("info", m, 3601).write);
    }
    std::printf(fails ? "FAILED (%d)\n" : "PASS pm_log_throttle_test\n", fails);
    return fails ? 1 : 0;
}
