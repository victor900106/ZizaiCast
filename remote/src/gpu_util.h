#pragma once
// 0.8.3.7: the GPU engine percentages of the stats line, from PDH's RAW "GPU Engine(pid_N_*)\Utilization
// Percentage" values (header only: pm_gpu_test checks it without PDH).
//
// Why raw (owner's log, 2026-10-22 15:11:29, 120 fps to an iPhone): "GPU 3D 36713195488442.6%". That counter
// is a PERF_PRECISION_100NS_TIMER: FirstValue = the engine's running time, SecondValue = a time stamp, both in
// 100 ns; percent = 100 * dRun / dStamp. 0.8.3.6 asked PDH for the formatted value with PDH_FMT_NOCAP100: when
// an engine's running time reads a little LOWER than in the sample before (the driver's per-engine running
// time is not strictly monotonic across reads; a process's engine context re-created does the same), PDH
// takes dRun as an unsigned 64-bit difference = about 2^64, still with a "valid" status:
// 100 * 2^64 / (5.0247 s in 100 ns) = 36713195488442.6 % -- exactly the logged figure with the log's 5.02 s
// between samples. Here: a negative dRun or a dStamp <= 0 makes that instance unknown for this sample (left
// out), each instance is clamped to 0..100, and a type shows its busiest engine (like Task Manager), so a
// figure is either a real 0..100 % or not shown at all.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

namespace pm::remote {

struct GpuRaw {
    std::string type;       // "3D", "VideoEncode", ... (after "engtype_")
    long long run = 0;      // FirstValue: running time, 100 ns
    long long stamp = 0;    // SecondValue: time stamp, 100 ns
    bool valid = true;      // the raw item's CStatus was valid
};

struct GpuSample {
    std::map<std::string, double> pct;  // type -> 0..100, only types with at least one sound instance
    int dropped = 0;                    // instances left out (went backwards, no time, bad status)
};

// What 0.8.3.6 effectively did (PDH's formatted value with NOCAP100, summed per type): kept for the test's
// before / after comparison only.
inline double gpuPdhStylePercent(const GpuRaw& a, const GpuRaw& b) {
    const uint64_t dRun = uint64_t(b.run) - uint64_t(a.run);  // unsigned: wraps when the running time goes back
    const double dT = double(b.stamp - a.stamp);
    return dT > 0 ? 100.0 * double(dRun) / dT : 0.0;
}

template <class Key>
GpuSample gpuPercent(const std::map<Key, GpuRaw>& prev, const std::map<Key, GpuRaw>& cur) {
    GpuSample out;
    for (const auto& [name, c] : cur) {
        auto it = prev.find(name);
        if (it == prev.end()) continue;  // new instance: known from the next sample on
        const GpuRaw& p = it->second;
        const long long dRun = c.run - p.run, dT = c.stamp - p.stamp;
        if (!c.valid || !p.valid || dT <= 0 || dRun < 0) {
            ++out.dropped;
            continue;
        }
        const double v = std::clamp(100.0 * double(dRun) / double(dT), 0.0, 100.0);
        double& slot = out.pct[c.type];
        slot = std::max(slot, v);
    }
    return out;
}

// "3D 1.2% VideoEncode 8.5%" (types under 0.05 % left out; "0" when nothing is busy, "n/a" when nothing is known).
inline std::string gpuText(const GpuSample& s) {
    std::string out;
    char b[64];
    for (const auto& [t, v] : s.pct) {
        if (v < 0.05) continue;
        snprintf(b, sizeof(b), "%s%s %.1f%%", out.empty() ? "" : " ", t.c_str(), v);
        out += b;
    }
    if (out.empty()) return s.pct.empty() && s.dropped ? "n/a" : "0";
    return out;
}

}  // namespace pm::remote
