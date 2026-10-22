// 自在投影 app: log_throttle.h — decides which log lines are written (0.8.4).
// phonemirror.log of a real PC (17 549 lines in 15 days) was mostly the same few lines over and over:
// "WASAPI open failed" 1598x, "idle (no viewer)" 1365x, "[video-timing]" 1273x ...
// Two rules, both keep a count so nothing is lost silently:
//   1. kRules: known chatty lines are written at most once per N seconds ("(+K similar lines left out)").
//   2. the same line again and again (compared without a leading remote "HH:MM:SS.mmm [remote] " stamp)
//      is written once; when something else comes, or after kDupFlushSec, one line says "repeated K more times".
// Pure logic (no I/O, no clock): tested by app/tools/pm_log_throttle_test.cpp.
#pragma once

#include <cstdint>
#include <string>

namespace pm_app {

class LogThrottle {
public:
    struct Verdict {
        bool write = true;
        unsigned flushDup = 0;     // write "previous line repeated K more times" BEFORE this line (K > 0)
        unsigned suppressed = 0;   // lines of this kind left out since the last written one (rule throttle)
        unsigned dupNow = 0;       // write = true by the dup timer: this line stands for K more identical ones
        int ruleSeconds = 0;
    };

    static constexpr int64_t kDupFlushSec = 60;

    Verdict check(const std::string& level, const std::string& msg, int64_t nowSec) {
        Verdict v;
        // 1. rule-throttled kinds
        for (size_t i = 0; i < kRuleCount; ++i) {
            const Rule& r = kRules[i];
            if (msg.find(r.contains) == std::string::npos) continue;
            if (r.level[0] && level != r.level) continue;
            State& s = state_[i];
            if (s.have && nowSec - s.lastWrite < r.seconds) {
                ++s.suppressed;
                v.write = false;
                return v;
            }
            v.suppressed = s.suppressed;
            v.ruleSeconds = r.seconds;
            s.suppressed = 0;
            s.have = true;
            s.lastWrite = nowSec;
            v.flushDup = takeDup();
            lastKey_.clear();  // a throttled kind never takes part in the duplicate run
            return v;
        }
        // 2. identical line again
        const std::string key = level + "|" + stripStamp(msg);
        if (!lastKey_.empty() && key == lastKey_) {
            if (nowSec - dupSince_ < kDupFlushSec) {
                ++dup_;
                v.write = false;
                return v;
            }
            v.dupNow = dup_;  // the timer: write this one, saying how many were left out
            dup_ = 0;
            dupSince_ = nowSec;
            return v;
        }
        v.flushDup = takeDup();
        lastKey_ = key;
        dupSince_ = nowSec;
        return v;
    }

    static std::string stripStamp(const std::string& m) {
        // "22:57:46.852 [remote] input: ..." -> "input: ..."
        if (m.size() > 13 && m[2] == ':' && m[5] == ':' && m[8] == '.') {
            const size_t p = m.find("] ", 12);
            if (p != std::string::npos && p < 24) return m.substr(p + 2);
        }
        return m;
    }

private:
    struct Rule {
        const char* level;     // "" = any
        const char* contains;
        int seconds;
    };
    static constexpr Rule kRules[] = {
        {"", "[video-timing]", 30},                                  // airplay_server: every 5 s while mirroring
        {"video", "s: AU in ", 30},                                  // video module summary: every 5 s while mirroring
        {"", "kernel timestamps on NetAdapter", 3600},               // a 3-line how-to, once per connection attempt
        {"", "[network] mDNS announcement repeated", 600},
        {"video", "adapter check (display / monitor change): keeping", 300},
    };
    static constexpr size_t kRuleCount = sizeof(kRules) / sizeof(kRules[0]);
    struct State {
        bool have = false;
        int64_t lastWrite = 0;
        unsigned suppressed = 0;
    };
    State state_[kRuleCount];
    std::string lastKey_;
    unsigned dup_ = 0;
    int64_t dupSince_ = 0;

    unsigned takeDup() {
        const unsigned d = dup_;
        dup_ = 0;
        return d;
    }
};

}  // namespace pm_app
