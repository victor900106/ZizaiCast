#pragma once
// Small shared helpers for pm_remote: logging, clocks, rolling statistics.

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace pm::remote {

// printf-style log line ("[remote] ..."): stderr, the optional log file and
// the optional handler (Host::Options::log). Thread-safe.
void logf(const char* fmt, ...);
void setLogFile(const std::wstring& path);
void setLogHandler(std::function<void(const std::string&)> fn);

// Monotonic microseconds (QueryPerformanceCounter).
int64_t nowUs();
// Wall clock, milliseconds since the Unix epoch (GetSystemTimePreciseAsFileTime).
int64_t epochMs();

std::string narrow(const std::wstring& w);
std::wstring widen(const std::string& s);
std::string hresultText(long hr);
std::string hmacSha256Hex(const std::string& key, const std::string& msg);  // secure.cpp

// Rolling sample set for latency figures (ms): avg / p50 / p95 / max over
// the samples added since the last take().
class Samples {
public:
    void add(double v);
    struct Summary {
        int n = 0;
        double avg = 0, p50 = 0, p95 = 0, max = 0;
    };
    Summary take();  // summary + reset
private:
    std::mutex mu_;
    std::vector<double> v_;
};

// Process CPU use between calls: percent of the whole machine and percent of
// one core.
class CpuMeter {
public:
    CpuMeter();
    void sample(double& machinePct, double& oneCorePct);
private:
    uint64_t lastProc_ = 0, lastWall_ = 0;
    int cores_ = 1;
};

// Per-process GPU engine utilisation (PDH "GPU Engine(pid_N_*)"), percent
// summed per engine type (3D, VideoEncode, VideoProcessing, Copy, ...).
class GpuMeter {
public:
    GpuMeter();
    ~GpuMeter();
    std::string sample();  // "3D 1.2% VideoEncode 8.5% ..." since the last call
private:
    void* query_ = nullptr;
    void* counter_ = nullptr;
};

}  // namespace pm::remote
