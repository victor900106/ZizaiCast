// VideoWindow internals shared by its .cpp files: queue / sync / watchdog limits, clocks,
// the access unit and snapshot request types, window messages and the touch cursor.
// 拆檔 0.7.9：自 video_window.cpp 的匿名 namespace 原樣搬出，改放 pm::detail
// （Impl 的成員型別不能留在匿名 namespace，否則違反 ODR；函式加 inline）。
#pragma once


#include <windows.h>
#include <windowsx.h>

#include <d3d11.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cmath>
#include <functional>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "annexb.h"
#include "log.h"
#include "mf_decoder.h"
#include "renderer.h"
#include "ui_art.h"

namespace pm {

using Microsoft::WRL::ComPtr;
using video::log;

namespace detail {

inline constexpr wchar_t kClassName[] = L"PhoneMirrorVideoWindow";
// AU queue: beyond kMaxQueuedAUs (~0.5 s at 60 fps) the queue skips to the
// newest queued IDR (clean).  Without one, AUs are only dropped beyond the
// hard limit: a dropped reference breaks every picture until the next IDR,
// and iOS sends one only every minute or so (the HEVC decoder then shows
// nothing at all), so a stalled render thread catches up instead (decoding
// is ~2 ms per AU; only the newest picture is presented).
constexpr size_t kMaxQueuedAUs = 32;
constexpr size_t kHardMaxQueuedAUs = 600;           // ~10 s at 60 fps
constexpr size_t kHardMaxQueuedBytes = 128u << 20;
constexpr size_t kMaxStatSamples = 1 << 20;
// A/V sync: decoded pictures wait at most this long (a stale or absurd
// timestamp cannot freeze the picture), and at most this many are held.
constexpr double kSyncMaxHoldMs = 500;
constexpr size_t kSyncMaxHeld = 40;
constexpr double kSyncSlackMs = 2;  // present this early (timer granularity)
// Device-loss / watchdog recovery re-feeds the access units since the last
// key frame (bounded; beyond that only the key frame is re-fed, and an HEVC
// stream stays frozen until the next IDR).  0.6.1: 300 AUs / 16 MB (5 s).
constexpr size_t kGopMaxAUs = 1800;  // 30 s at 60 fps
constexpr size_t kGopMaxBytes = 64u << 20;
// Watchdog (PM_VIDEO_WATCHDOG=0 turns it off together with the 0.6.2 queue /
// GOP limits and the deferred GPU switch: the 0.6.1 behaviour, for tests).
constexpr double kWdStallMs = 1500;      // no picture decoded / presented for this long
constexpr int kWdMinFeeds = 8;           // ... although at least this many AUs were fed
constexpr double kWdBackoffMs = 10000;   // later attempts in the same episode
constexpr double kOccludedPresentMs = 100;  // present rate while DXGI reports occlusion
constexpr double kHwRetryMs = 30000;     // after a watchdog SW fallback: HW again at an IDR

inline bool watchdogEnabled() {
    static const bool on = [] {
        char v[8]{};
        return !(GetEnvironmentVariableA("PM_VIDEO_WATCHDOG", v, sizeof(v)) > 0 && v[0] == '0');
    }();
    return on;
}
constexpr double kNoTime = -1e300;  // AccessUnit::due: show ASAP

inline double nowMs() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
}

// The two candidate clocks for ntpLocalNs (pm::AirPlayServer::localTimeNs()):
// QPC in ns (like the core's CLOCK_MONOTONIC / std::steady_clock) and the
// precise system time in ns since the Unix epoch (CLOCK_REALTIME).
inline int64_t qpcNs() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart / freq * 1000000000LL + c.QuadPart % freq * 1000000000LL / freq;
}

inline int64_t utcNs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const int64_t t = static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    return (t - 116444736000000000LL) * 100;
}

struct AccessUnit {
    std::vector<uint8_t> data;
    double tIn = 0;     // onFrame() time
    double due = kNoTime;  // presentation time on the nowMs() clock (sync mode, before audio latency)
    bool irap = false;  // IDR / IRAP picture
    bool keep = false;  // irap or carries parameter sets: never drop
    uint64_t pts = 0;   // frame tap: ntpLocalNs, or arrival time on the same clock
};

struct SnapshotRequest {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<uint8_t> pixels;  // BGRX
    UINT w = 0, h = 0;
    bool framed = false;  // device frame, transparent background (BGRA)
    bool ui = false;      // the whole window as drawn (saveWindowShot)
    bool grab = false;    // content picture for OCR (grabPicture): not mirrored
    bool ok = false;
    ~SnapshotRequest() { CloseHandle(done); }
};

using IdleOption = VideoWindow::IdleOption;

inline void percentile(std::vector<float> v, double& avg, double& p95) {
    avg = p95 = 0;
    if (v.empty()) return;
    double sum = 0;
    for (float x : v) sum += x;
    avg = sum / v.size();
    size_t k = std::min(v.size() - 1, static_cast<size_t>(v.size() * 0.95));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    p95 = v[k];
}

extern const UINT kTestMsg;  // defined in video_window.cpp (RegisterWindowMessageW)
constexpr UINT_PTR kHoverTimer = 0x504D0001;  // trailing hover move (app timers use small ids)
// Posted by onReset(): the UI thread ends a region selection in progress.
extern const UINT kCancelSelectMsg;  // defined in video_window.cpp (RegisterWindowMessageW)
constexpr double kHoverMinMs = 1000.0 / 60;    // hover moves at most ~60 Hz

// A small ring with a dot (touch point), 32x32, hot spot in the middle:
// shown over the picture while a pointer handler is set.
inline HCURSOR createTouchCursor() {
    constexpr int N = 32;
    BITMAPV5HEADER bh{};
    bh.bV5Size = sizeof(bh);
    bh.bV5Width = N;
    bh.bV5Height = -N;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000;
    bh.bV5GreenMask = 0x0000FF00;
    bh.bV5BlueMask = 0x000000FF;
    bh.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bh), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !bits) return LoadCursorW(nullptr, IDC_CROSS);
    auto* px = static_cast<uint32_t*>(bits);
    const float c = (N - 1) / 2.f;
    auto cover = [](float d, float r0, float r1) {  // anti-aliased band r0..r1
        return std::clamp(std::min(d - r0 + 0.5f, r1 - d + 0.5f), 0.f, 1.f);
    };
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            const float d = std::hypot(x - c, y - c);
            // white ring + dot over a dark outline (visible on any picture)
            const float white = std::max(cover(d, 7.f, 9.f), cover(d, 0.f, 1.6f));
            const float dark = std::max({cover(d, 6.f, 10.f), cover(d, 0.f, 2.6f)});
            const float a = std::max(white, dark * 0.75f);
            const float v = a > 0 ? white / a : 0;  // straight colour
            const uint8_t g = static_cast<uint8_t>(std::lround(v * 255 * a));  // premultiplied
            px[y * N + x] = (static_cast<uint32_t>(std::lround(a * 255)) << 24) | (g << 16) | (g << 8) | g;
        }
    HBITMAP mask = CreateBitmap(N, N, 1, 1, nullptr);
    ICONINFO ii{FALSE, N / 2, N / 2, mask, color};
    HCURSOR cur = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return cur ? cur : LoadCursorW(nullptr, IDC_CROSS);
}

}  // namespace detail

using namespace detail;  // 拆檔 0.7.9: was video_window.cpp's anonymous namespace

}  // namespace pm
