// log_replay (0.8.4 measurement tool): replays an existing phonemirror.log through LogThrottle (core/base/log_throttle.h) and
// prints how many lines the new logger would have written.   log_replay <phonemirror.log> [v]   (v = per-kind table)
// Modelled exactly as the host writes them: "idle (no viewer)" once per idle spell (the first of a run between two other lines);
// everything else as Log::write decides (LogThrottle). "WASAPI open failed" is a MODEL of audio/src/open_backoff.h (the same
// error once, then every 10 minutes): the retries themselves (1598 in 16 s) cannot be replayed from a log.
// Read-only: the input file is only read.
#include "core/base/log_throttle.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::ifstream in(argv[1], std::ios::binary);
    pm_app::LogThrottle t;
    long long entries = 0, lines = 0, written = 0, writtenLines = 0, extra = 0;
    std::string line;
    long long curLines = 0, curTime = 0, lastWasapi = -1000000;
    std::string curLevel, curMsg;
    bool lastWasIdle = false;
    std::map<std::string, std::pair<long long, long long>> kinds;  // kind -> {before, after}
    auto kindOf = [](std::string m) {
        if (m.size() > 13 && m[2] == ':' && m[5] == ':' && m[8] == '.') {
            const size_t p = m.find("] ", 12);
            if (p != std::string::npos && p < 24) m = m.substr(p + 2);
        }
        std::string k = m.substr(0, 30);
        for (char& c : k)
            if (c >= '0' && c <= '9') c = '#';
        return k;
    };
    auto flush = [&]() {
        if (curLevel.empty()) return;
        ++entries;
        const bool idle = curMsg.find("idle (no viewer)") != std::string::npos;
        bool keep;
        if (curMsg.find("WASAPI open failed") != std::string::npos) {
            keep = curTime - lastWasapi >= 600;
            if (keep) lastWasapi = curTime;
            lastWasIdle = false;
        } else if (idle) {
            keep = !lastWasIdle;  // the host writes it once per spell
            lastWasIdle = true;
        } else {
            lastWasIdle = false;
            const auto v = t.check(curLevel, curMsg, curTime);
            keep = v.write;
            if (v.flushDup) ++extra;
        }
        auto& kc = kinds[kindOf(curMsg)];
        ++kc.first;
        if (keep) ++kc.second, ++written, writtenLines += curLines;
        curLevel.clear();
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ++lines;
        int Y, M, D, h, m, s, n = 0;
        if (std::sscanf(line.c_str(), "%d-%d-%d %d:%d:%d [%*[^]]]%n", &Y, &M, &D, &h, &m, &s, &n) >= 6 && n > 0) {
            flush();
            std::tm tm{};
            tm.tm_year = Y - 1900, tm.tm_mon = M - 1, tm.tm_mday = D, tm.tm_hour = h, tm.tm_min = m, tm.tm_sec = s;
            curTime = (long long)std::mktime(&tm);
            const size_t a = line.find('[', 19), b = line.find(']', a);
            curLevel = line.substr(a + 1, b - a - 1);
            curMsg = line.substr(b + 2);
            curLines = 1;
        } else if (!curLevel.empty()) {
            curMsg += "\n" + line;
            ++curLines;
        }
    }
    flush();
    std::printf("entries %lld (lines %lld) -> written %lld entries (%lld lines, +%lld 'repeated' notes)\n", entries, lines, written, writtenLines, extra);
    if (argc > 2) {
        std::vector<std::pair<std::string, std::pair<long long, long long>>> v(kinds.begin(), kinds.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.first > b.second.first; });
        for (size_t i = 0; i < v.size() && i < 12; ++i)
            std::printf("  %-32s %6lld -> %6lld\n", v[i].first.c_str(), v[i].second.first, v[i].second.second);
    }
    return 0;
}
