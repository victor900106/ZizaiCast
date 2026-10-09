// pm_recorder_test: feeds pm::Recorder in real time with synthetic NV12
// pictures and a 440 Hz tone, the way the app does (video stamps on the steady
// clock, audio stamps on the UTC wall clock like the core's ntpLocalNs), then
// prints timing/CPU/GPU figures. Check the file with
//   python recorder\tools\verify_recording.py out.mp4 [same scenario options]
//
// Picture: 64 px vertical stripes (Y 16/176) moving 4 px per frame, UV
// gradient, the frame index as 16 binary 32x32 blocks at the top-left
// (bit 15 first; 235 = 1, 16 = 0). Flash markers: the whole picture except the
// blocks is white (Y 235) for 50 ms from each --flash time, and the tone jumps
// from amplitude 0.25 to 0.9 over the same 50 ms (beep).
//
// usage: pm_recorder_test [--out F] [--seconds S] [--size WxH] [--fps N]
//        [--rotate-at S] [--audio-gap S:LEN] [--video-stall S:LEN]
//        [--flash S,S,...] [--rate HZ] [--no-audio] [--audio-start S]
//        [--start-picture]   (setStartPicture() with frame 0 right after start;
//                             with --video-stall 0:S the phone "is paused")
#include <pm/recorder.h>

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

int64_t steadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
int64_t utcNs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return ((int64_t(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) - 116444736000000000LL) * 100;
}
double ftSec(FILETIME f) { return double(uint64_t(f.dwHighDateTime) << 32 | f.dwLowDateTime) / 1e7; }
double processCpu() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return ftSec(k) + ftSec(u);
}

void drawFrame(std::vector<uint8_t>& buf, int w, int h, int idx, bool flash) {
    buf.resize(size_t(w) * h * 3 / 2);
    uint8_t* Y = buf.data();
    std::vector<uint8_t> row(w);
    for (int x = 0; x < w; ++x) row[x] = flash ? 235 : ((((x + 4 * idx) >> 6) & 1) ? 176 : 16);
    for (int y = 0; y < h; ++y) std::memcpy(Y + size_t(y) * w, row.data(), w);
    // frame counter
    for (int b = 0; b < 16; ++b) {
        const uint8_t v = ((idx >> (15 - b)) & 1) ? 235 : 16;
        for (int y = 0; y < 32 && y < h; ++y)
            for (int x = b * 32; x < b * 32 + 32 && x < w; ++x) Y[size_t(y) * w + x] = v;
    }
    uint8_t* UV = Y + size_t(w) * h;
    for (int y = 0; y < h / 2; ++y) {
        uint8_t* r = UV + size_t(y) * w;
        const uint8_t v = uint8_t(64 + (y * 128) / (h / 2));
        for (int x = 0; x < w / 2; ++x) {
            r[2 * x] = flash ? 128 : uint8_t(64 + (x * 128) / (w / 2));
            r[2 * x + 1] = flash ? 128 : v;
        }
    }
}

struct GpuCounter {
    PDH_HQUERY q = nullptr;
    PDH_HCOUNTER enc = nullptr, gfx = nullptr;
    double sumEnc = 0, maxEnc = 0, sum3d = 0;
    int n = 0;
    GpuCounter() {
        if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return;
        PdhAddEnglishCounterW(q, L"\\GPU Engine(*engtype_VideoEncode)\\Utilization Percentage", 0, &enc);
        PdhAddEnglishCounterW(q, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &gfx);
        PdhCollectQueryData(q);
    }
    ~GpuCounter() {
        if (q) PdhCloseQuery(q);
    }
    static double sum(PDH_HCOUNTER c) {
        if (!c) return 0;
        DWORD size = 0, count = 0;
        PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &count, nullptr);
        if (!size) return 0;
        std::vector<uint8_t> mem(size);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(mem.data());
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS) return 0;
        double s = 0;
        for (DWORD i = 0; i < count; ++i)
            if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA) s += items[i].FmtValue.doubleValue;
        return s;
    }
    void sample() {
        if (!q || PdhCollectQueryData(q) != ERROR_SUCCESS) return;
        double e = sum(enc), g = sum(gfx);
        sumEnc += e;
        maxEnc = std::max(maxEnc, e);
        sum3d += g;
        ++n;
    }
};

// --readback FILE: decodes the whole file with Windows' own Media Foundation
// demuxer + decoders (what Films & TV / Windows Media Player use) and reports
// per-stream sample counts, first/last timestamps and errors.
int readback(const std::wstring& path) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    IMFSourceReader* r = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &r);
    if (FAILED(hr)) {
        std::printf("readback: cannot open (hr=0x%08lx)\n", (unsigned long)hr);
        return 1;
    }
    // Ask for uncompressed output so the decoders actually run.
    IMFMediaType* t = nullptr;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    HRESULT hv = r->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, t);
    t->Release();
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    HRESULT ha = r->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, t);
    t->Release();
    std::printf("readback: decoder setup video hr=0x%08lx audio hr=0x%08lx\n", (unsigned long)hv, (unsigned long)ha);
    // Duration and seeking as the Windows players see them (a fragmented MP4
    // cut short by a crash has no duration in its header).
    {
        PROPVARIANT v;
        PropVariantInit(&v);
        double dur = -1;
        if (SUCCEEDED(r->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &v)) &&
            v.vt == VT_UI8)
            dur = v.uhVal.QuadPart / 1e7;
        PropVariantClear(&v);
        ULONG chars = 0;
        if (SUCCEEDED(r->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE),
                                                  MF_SOURCE_READER_MEDIASOURCE_CHARACTERISTICS, &v)) &&
            v.vt == VT_UI4)
            chars = v.ulVal;
        PropVariantClear(&v);
        const bool canSeek = (chars & MFMEDIASOURCE_CAN_SEEK) != 0;
        double seekTo = -1;
        if (canSeek && dur > 1) {
            PROPVARIANT pos;
            PropVariantInit(&pos);
            pos.vt = VT_I8;
            pos.hVal.QuadPart = LONGLONG(dur / 2 * 1e7);
            if (SUCCEEDED(r->SetCurrentPosition(GUID_NULL, pos))) {
                DWORD idx = 0, flags = 0;
                LONGLONG ts = 0;
                IMFSample* s = nullptr;
                if (SUCCEEDED(r->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &idx, &flags, &ts, &s)) &&
                    s) {
                    seekTo = ts / 1e7;
                    s->Release();
                }
            }
            PropVariantInit(&pos);
            pos.vt = VT_I8;
            pos.hVal.QuadPart = 0;
            r->SetCurrentPosition(GUID_NULL, pos);
        }
        std::printf("readback: duration %.3f s, %s", dur, canSeek ? "seekable" : "NOT seekable");
        if (seekTo >= 0) std::printf(" (seek to %.2f s -> first video frame %.3f s)", dur / 2, seekTo);
        std::printf("\n");
    }
    uint64_t n[2] = {0, 0}, bytesA = 0;
    LONGLONG first[2] = {-1, -1}, last[2] = {0, 0}, lastDur[2] = {0, 0};
    int errors = 0;
    for (;;) {
        DWORD idx = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample* s = nullptr;
        hr = r->ReadSample(DWORD(MF_SOURCE_READER_ANY_STREAM), 0, &idx, &flags, &ts, &s);
        if (FAILED(hr)) {
            ++errors;
            break;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (s) s->Release();
            // Keep reading until both streams ended.
            r->SetStreamSelection(idx, FALSE);
            DWORD sel = 0;
            BOOL any = FALSE;
            for (DWORD k = 0; k < 2; ++k)
                if (SUCCEEDED(r->GetStreamSelection(k, &any)) && any) ++sel;
            if (!sel) break;
            continue;
        }
        if (!s) continue;
        IMFMediaType* mt = nullptr;
        GUID major{};
        if (SUCCEEDED(r->GetCurrentMediaType(idx, &mt))) {
            mt->GetGUID(MF_MT_MAJOR_TYPE, &major);
            mt->Release();
        }
        const int k = major == MFMediaType_Audio ? 1 : 0;
        ++n[k];
        if (first[k] < 0) first[k] = ts;
        last[k] = ts;
        s->GetSampleDuration(&lastDur[k]);
        if (k == 1) {
            DWORD len = 0;
            s->GetTotalLength(&len);
            bytesA += len;
        }
        s->Release();
    }
    r->Release();
    std::printf("readback: video %llu decoded frames, %.4f .. %.4f s (+%.4f); audio %llu buffers, %.4f s of PCM, "
                "%.4f .. %.4f s; errors %d\n",
                (unsigned long long)n[0], first[0] / 1e7, last[0] / 1e7, lastDur[0] / 1e7, (unsigned long long)n[1],
                bytesA / 4.0 / 44100.0, first[1] / 1e7, last[1] / 1e7, errors);
    MFShutdown();
    return errors ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--readback") {
        std::string s = argv[2];
        return readback(std::wstring(s.begin(), s.end()));
    }
    std::wstring out = L"rec_test.mp4";
    double seconds = 20, rotateAt = 10, gapAt = 6, gapLen = 1, stallAt = 13, stallLen = 0.5, audioStart = 0;
    int W = 2560, H = 1440, fps = 60, rate = 44100;
    bool noAudio = false, startPicture = false;
    std::vector<double> flashes{3.0, 15.0};
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--out") {
            std::string s = next();
            out.assign(s.begin(), s.end());
        } else if (a == "--seconds") seconds = std::atof(next().c_str());
        else if (a == "--size") std::sscanf(next().c_str(), "%dx%d", &W, &H);
        else if (a == "--fps") fps = std::atoi(next().c_str());
        else if (a == "--rotate-at") rotateAt = std::atof(next().c_str());
        else if (a == "--audio-gap") std::sscanf(next().c_str(), "%lf:%lf", &gapAt, &gapLen);
        else if (a == "--video-stall") std::sscanf(next().c_str(), "%lf:%lf", &stallAt, &stallLen);
        else if (a == "--rate") rate = std::atoi(next().c_str());
        else if (a == "--no-audio") noAudio = true;
        else if (a == "--start-picture") startPicture = true;
        else if (a == "--audio-start") audioStart = std::atof(next().c_str());
        else if (a == "--flash") {
            flashes.clear();
            std::string s = next();
            for (size_t p = 0; p < s.size();) {
                flashes.push_back(std::atof(s.c_str() + p));
                p = s.find(',', p);
                if (p == std::string::npos) break;
                ++p;
            }
        } else {
            std::printf("see the header of pm_recorder_test.cpp for usage\n");
            return 2;
        }
    }
    auto inFlash = [&](double t) {
        for (double f : flashes)
            if (t >= f - 1e-9 && t < f + 0.05 - 1e-9) return true;
        return false;
    };

    GpuCounter gpu;  // before start(): opening the PDH query takes a while
    pm::Recorder rec;
    rec.log = [](const std::string& s) { std::printf("[recorder] %s\n", s.c_str()); };
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    const double cpuBefore = processCpu();
    if (!rec.start(out, fps)) {
        std::printf("start failed\n");
        return 1;
    }

    const int spf = 480;  // AAC-ELD mirroring packet size
    std::vector<uint8_t> frame;
    if (startPicture) {
        drawFrame(frame, W, H, 0, false);
        rec.setStartPicture(frame.data(), W, H, W);
    }
    std::vector<int16_t> pcm(spf * 2);
    const int64_t q0 = steadyNs() + 50'000'000, u0 = utcNs() + 50'000'000;
    const auto c0 = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    int64_t vIdx = 0, aIdx = 0;
    const int64_t vTotal = int64_t(seconds * fps);
    const int64_t aTotal = int64_t(seconds * rate / spf);
    double maxVideoCallUs = 0, maxAudioCallUs = 0, drawSec = 0;
    auto nextGpu = c0 + std::chrono::seconds(1);
    double nextStatsAt = 5;
    while (vIdx < vTotal || aIdx < aTotal) {
        const double tv = vIdx < vTotal ? double(vIdx) / fps : 1e18;
        const double ta = aIdx < aTotal ? double(aIdx) * spf / rate : 1e18;
        const bool isVideo = tv <= ta;
        const double t = std::min(tv, ta);
        std::this_thread::sleep_until(c0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                               std::chrono::duration<double>(t)));
        if (isVideo) {
            ++vIdx;
            if (t >= stallAt && t < stallAt + stallLen) continue;
            const bool rot = t >= rotateAt;
            const int w = rot ? H : W, h = rot ? W : H;
            auto d0 = std::chrono::steady_clock::now();
            drawFrame(frame, w, h, int(vIdx - 1), inFlash(t));
            auto d1 = std::chrono::steady_clock::now();
            drawSec += std::chrono::duration<double>(d1 - d0).count();
            rec.onVideoFrame(frame.data(), w, h, w, uint64_t(q0 + int64_t(std::llround(t * 1e9))));
            maxVideoCallUs = std::max(maxVideoCallUs,
                                      std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - d1).count());
        } else {
            const int64_t n0 = aIdx * spf;
            ++aIdx;
            if (noAudio || t < audioStart || (t >= gapAt && t < gapAt + gapLen)) continue;
            for (int i = 0; i < spf; ++i) {
                const double ts = double(n0 + i) / rate;
                const double amp = inFlash(ts) ? 0.9 : 0.25;
                const int16_t v = int16_t(std::lround(32767 * amp * std::sin(2 * kPi * 440.0 * ts)));
                pcm[2 * i] = v;
                pcm[2 * i + 1] = v;
            }
            auto a0 = std::chrono::steady_clock::now();
            rec.onPcm(pcm.data(), spf, 2, rate, uint64_t(u0 + int64_t(std::llround(double(n0) / rate * 1e9))));
            maxAudioCallUs = std::max(maxAudioCallUs,
                                      std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a0).count());
        }
        if (std::chrono::steady_clock::now() >= nextGpu) {
            gpu.sample();
            nextGpu += std::chrono::seconds(1);
        }
        if (t >= nextStatsAt) {
            auto s = rec.stats();
            std::printf("t=%.0fs stats: %.2f s, %llu video, %llu audio, %llu dropped, %.1f MB\n", t, s.seconds,
                        (unsigned long long)s.videoFrames, (unsigned long long)s.audioFrames,
                        (unsigned long long)s.droppedVideo, s.bytes / 1e6);
            nextStatsAt += 5;
        }
    }
    const auto feedEnd = std::chrono::steady_clock::now();
    const double cpuFeed = processCpu() - cpuBefore;
    rec.stop();  // with whatever is still queued
    const double stopMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - feedEnd).count();
    const double cpuAll = processCpu() - cpuBefore;
    rec.stop();  // second call must be harmless
    auto s = rec.stats();
    const double wall = std::chrono::duration<double>(feedEnd - c0).count();
    std::printf("final stats: %.3f s, %llu video frames, %llu audio frames, %llu dropped, %.2f MB (%.2f Mbps)\n",
                s.seconds, (unsigned long long)s.videoFrames, (unsigned long long)s.audioFrames,
                (unsigned long long)s.droppedVideo, s.bytes / 1e6, s.bytes * 8 / 1e6 / std::max(s.seconds, 1e-3));
    std::printf("stop() took %.1f ms; max onVideoFrame %.0f us, max onPcm %.0f us\n", stopMs, maxVideoCallUs,
                maxAudioCallUs);
    std::printf("process CPU: %.2f s over %.1f s feeding (%.1f %% of one core), of which picture synthesis %.2f s; "
                "recorder share ~%.1f %% of one core (%.1f %% of all %u cores)\n",
                cpuFeed, wall, 100 * cpuFeed / wall, drawSec, 100 * (cpuAll - drawSec) / wall,
                100 * (cpuAll - drawSec) / wall / std::thread::hardware_concurrency(),
                std::thread::hardware_concurrency());
    if (gpu.n)
        std::printf("GPU VideoEncode engine: avg %.1f %%, max %.1f %%; 3D engines avg %.1f %% (whole system)\n",
                    gpu.sumEnc / gpu.n, gpu.maxEnc, gpu.sum3d / gpu.n);
    return 0;
}
