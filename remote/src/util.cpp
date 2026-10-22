#include "util.h"

#include <windows.h>
#include <intrin.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <map>

namespace pm::remote {

namespace {
std::mutex g_logMu;
FILE* g_logFile = nullptr;
std::function<void(const std::string&)> g_logFn;
}  // namespace

void setLogFile(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(g_logMu);
    if (g_logFile) fclose(g_logFile), g_logFile = nullptr;
    if (!path.empty()) g_logFile = _wfopen(path.c_str(), L"ab");
}

void setLogHandler(std::function<void(const std::string&)> fn) {
    std::lock_guard<std::mutex> lk(g_logMu);
    g_logFn = std::move(fn);
}

void logf(const char* fmt, ...) {
    char body[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[2200];
    snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [remote] %s\n", st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds, body);
    std::lock_guard<std::mutex> lk(g_logMu);
    fputs(line, stderr);
    if (g_logFile) fputs(line, g_logFile), fflush(g_logFile);
    OutputDebugStringA(line);
    if (g_logFn) g_logFn(line);
}

int64_t nowUs() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return int64_t(c.QuadPart / freq * 1000000 + (c.QuadPart % freq) * 1000000 / freq);
}

int64_t epochMs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;  // 100 ns since 1601
    return int64_t((t - 116444736000000000ull) / 10000);
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string hresultText(long hr) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

void Samples::add(double v) {
    std::lock_guard<std::mutex> lk(mu_);
    if (v_.size() < 100000) v_.push_back(v);
}

Samples::Summary Samples::take() {
    std::vector<double> v;
    {
        std::lock_guard<std::mutex> lk(mu_);
        v.swap(v_);
    }
    Summary s;
    s.n = int(v.size());
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    s.avg = sum / double(v.size());
    s.p50 = v[v.size() / 2];
    s.p95 = v[std::min(v.size() - 1, size_t(double(v.size()) * 0.95))];
    s.max = v.back();
    return s;
}

// CPU use from QueryProcessCycleTime (TSC cycles, exact) against the TSC
// itself over the same interval; GetProcessTimes is charged per clock tick
// (15.6 ms) and badly under-counts short bursts like ours.
namespace {
uint64_t procCycles() {
    ULONG64 c = 0;
    QueryProcessCycleTime(GetCurrentProcess(), &c);
    return c;
}
}  // namespace

CpuMeter::CpuMeter() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    cores_ = std::max<int>(1, int(si.dwNumberOfProcessors));
    lastProc_ = procCycles();
    lastWall_ = __rdtsc();
}

void CpuMeter::sample(double& machinePct, double& oneCorePct) {
    const uint64_t p = procCycles(), w = __rdtsc();
    const double dp = double(p - lastProc_), dw = double(w - lastWall_);
    lastProc_ = p, lastWall_ = w;
    oneCorePct = dw > 0 ? 100.0 * dp / dw : 0;
    machinePct = oneCorePct / cores_;
}

GpuMeter::GpuMeter() {
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return;
    wchar_t path[128];
    swprintf(path, 128, L"\\GPU Engine(pid_%lu_*)\\Utilization Percentage", GetCurrentProcessId());
    PDH_HCOUNTER c = nullptr;
    if (PdhAddEnglishCounterW(q, path, 0, &c) != ERROR_SUCCESS) {
        PdhCloseQuery(q);
        return;
    }
    query_ = q, counter_ = c;
    sample();  // 0.8.3.7: the first raw values (the next call has something to compare with)
}

GpuMeter::~GpuMeter() {
    if (query_) PdhCloseQuery(static_cast<PDH_HQUERY>(query_));
}

std::string GpuMeter::sample() {
    if (!query_) return "n/a";
    if (PdhCollectQueryData(static_cast<PDH_HQUERY>(query_)) != ERROR_SUCCESS) return "n/a";
    // 0.8.3.7: the RAW values, the percentages computed here (gpu_util.h: 0.8.3.6's formatted values with
    // PDH_FMT_NOCAP100 logged "GPU 3D 36713195488442.6%" once a running time read lower than before).
    DWORD size = 0, count = 0;
    PdhGetRawCounterArrayW(static_cast<PDH_HCOUNTER>(counter_), &size, &count, nullptr);
    std::map<std::wstring, GpuRaw> cur;
    if (size) {
        std::vector<uint8_t> buf(size);
        auto* items = reinterpret_cast<PDH_RAW_COUNTER_ITEM_W*>(buf.data());
        if (PdhGetRawCounterArrayW(static_cast<PDH_HCOUNTER>(counter_), &size, &count, items) != ERROR_SUCCESS) return "n/a";
        for (DWORD i = 0; i < count; ++i) {
            const std::wstring name = items[i].szName ? items[i].szName : L"";
            const size_t p = name.find(L"engtype_");
            GpuRaw r;
            r.type = p == std::wstring::npos ? "?" : narrow(name.substr(p + 8));
            r.run = items[i].RawValue.FirstValue, r.stamp = items[i].RawValue.SecondValue;
            r.valid = items[i].RawValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].RawValue.CStatus == PDH_CSTATUS_NEW_DATA;
            auto [it, fresh] = cur.emplace(name, r);
            if (!fresh) it->second.valid = false;  // two instances with one name: cannot be paired, left out
        }
    }
    const GpuSample s = gpuPercent(prev_, cur);
    prev_ = std::move(cur);
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto get = [&](const char* t) { auto it = s.pct.find(t); return it == s.pct.end() ? -1.0 : it->second; };
        last3d_ = get("3D"), lastEnc_ = get("VideoEncode");
    }
    return gpuText(s);
}

void GpuMeter::last(double& d3, double& enc) const {
    std::lock_guard<std::mutex> lk(mu_);
    d3 = last3d_, enc = lastEnc_;
}

}  // namespace pm::remote
