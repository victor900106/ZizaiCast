// pm::Recorder -- see recorder.h and docs/recorder.md.
#include <pm/recorder.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <strmif.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace pm {

namespace {

int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int64_t utcNowNs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    int64_t t = (int64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000LL) * 100;
}

// Maps a stamp on either the steady clock or the UTC wall clock to the steady
// clock (whichever "now" it is closer to). 0 / implausible -> now.
int64_t toSteadyNs(uint64_t stamp, int64_t steadyNow) {
    if (stamp == 0) return steadyNow;
    constexpr int64_t kWin = 30'000'000'000LL;
    const int64_t t = int64_t(stamp);
    const int64_t utcNow = utcNowNs();
    const int64_t ds = std::abs(t - steadyNow), du = std::abs(t - utcNow);
    if (std::min(ds, du) > kWin) return steadyNow;
    return du < ds ? t - utcNow + steadyNow : t;
}

constexpr int kMaxVideoQueue = 6;                 // frames waiting for the encoder thread
// Creating the hardware encoder takes ~0.5 s once the first picture (and so the
// size) is known; meanwhile up to this much picture data may queue up.
constexpr size_t kOpeningBudget = 256u << 20;
constexpr int64_t kMaxAudioQueueNs = 4'000'000'000;  // PCM waiting for the encoder thread
constexpr int64_t kMaxPendingAudioNs = 10'000'000'000;  // audio held before the first picture
constexpr int64_t kAudioOnlyOpenNs = 5'000'000'000;  // audio without video for this long -> black canvas
constexpr int64_t kAudioTolNs = 40'000'000;       // audio timestamp drift tolerated before gap fill / trim
constexpr int64_t kMinLagNs = 1'000'000'000;      // live fill stays this far behind "now"
constexpr int64_t kMaxAheadNs = 3'000'000'000;    // stamps later than now + this are clamped
constexpr int kDefaultW = 1280, kDefaultH = 720;  // canvas if no picture ever arrives
constexpr int64_t kStartPictureNs = 250'000'000;  // no live picture this long after start -> the start picture

struct VFrame {
    ComPtr<IMFMediaBuffer> buf;  // packed NV12, stride == w (from the buffer pool)
    int w = 0, h = 0;
    int64_t pts = 0;  // steady ns
};

struct AChunk {
    std::vector<int16_t> pcm;  // interleaved, ch channels
    size_t frames = 0;
    int ch = 0, rate = 0;
    int64_t when = 0;  // steady ns of the first frame
};

}  // namespace

struct Recorder::Impl {
    Recorder* owner = nullptr;

    // ---- control ----
    std::mutex ctlMu;  // serialises start/stop
    std::thread thread;
    std::atomic<bool> running{false};  // accepting data
    std::atomic<bool> failed{false};

    // ---- queues (guarded by qMu) ----
    std::mutex qMu;
    std::condition_variable qCv;
    bool stopReq = false;
    int64_t stopAtNs = 0;  // steady ns of stop()
    std::deque<VFrame> vq;
    // setStartPicture(): used if no picture arrives within kStartPictureNs of
    // start() (a paused / still phone sends none); dropped once one does.
    VFrame startPic;
    int64_t startNs = 0;

    // ---- NV12 buffer pool (guarded by poolMu) ----
    // Media buffers are recycled once nobody but the pool references them
    // (the encoder releases its samples asynchronously). A picture whose size
    // equals the canvas goes to the encoder in the buffer the caller filled
    // (one copy in total).
    std::mutex poolMu;
    std::vector<ComPtr<IMFMediaBuffer>> pool;
    ComPtr<IMFMediaBuffer> acquire(DWORD size) {
        std::lock_guard<std::mutex> lk(poolMu);
        for (auto& b : pool) {
            DWORD max = 0;
            if (FAILED(b->GetMaxLength(&max)) || max != size) continue;
            b->AddRef();
            if (b->Release() == 1) return b;  // only the pool holds it
        }
        ComPtr<IMFMediaBuffer> nb;
        if (FAILED(MFCreateMemoryBuffer(size, &nb))) return nullptr;
        if (pool.size() >= 40) {  // drop idle buffers (prefer other sizes)
            auto idle = [&](bool otherSizeOnly) {
                for (auto it = pool.begin(); it != pool.end(); ++it) {
                    DWORD max = 0;
                    (*it)->GetMaxLength(&max);
                    if (otherSizeOnly && max == size) continue;
                    (*it)->AddRef();
                    if ((*it)->Release() == 1) {
                        pool.erase(it);
                        return true;
                    }
                }
                return false;
            };
            if (!idle(true)) idle(false);
        }
        if (pool.size() < 40) pool.push_back(nb);
        return nb;
    }
    std::deque<AChunk> aq;
    int64_t aqNs = 0;
    int64_t maxLateNs = 0;  // max (now - stamp) seen at enqueue

    // ---- stats ----
    std::atomic<double> statSeconds{0};
    std::atomic<uint64_t> statVideoFrames{0}, statAudioFrames{0}, statDropped{0}, statBytes{0};

    // ---- encoder thread state ----
    std::wstring path;
    int fps = 60;
    ComPtr<IMFSinkWriter> writer;
    DWORD vStream = 0, aStream = 0;
    bool opened = false;
    std::atomic<bool> encoderReady{false};  // false while the encoder is being created (bigger queue)
    bool hwCreate = true;  // writer was created with hardware transforms enabled
    int cw = 0, chh = 0;   // canvas
    int outRate = 44100;
    int64_t t0 = 0;
    std::deque<AChunk> pendingAudio;  // before open
    int64_t pendingAudioNs = 0;
    int64_t firstPendingArrival = 0;
    ComPtr<IMFMediaBuffer> held;  // picture covering slot nextSlot onwards
    int64_t nextSlot = 0;
    uint64_t audioWritten = 0;  // frames
    uint64_t inputFrames = 0, replaced = 0, dupFrames = 0, silenceFrames = 0, trimmedFrames = 0;
    int lastW = 0, lastH = 0;
    // resampler state (input rate != outRate)
    double rsPos = 0;
    int16_t rsPrev[2] = {0, 0};
    bool rsHavePrev = false;
    int rsRate = 0;
    std::vector<int16_t> stereo, resampled;
    // scaler tables
    int scSrcW = 0, scSrcH = 0;
    int dx = 0, dy = 0, dw = 0, dh = 0;
    std::vector<int> xIdx, xIdxC;
    std::vector<uint16_t> xFr, xFrC;

    void logf(const char* fmt, ...) {
        char buf[768];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        if (owner->log) {
            owner->log(buf);
            return;
        }
        OutputDebugStringA("[recorder] ");
        OutputDebugStringA(buf);
        OutputDebugStringA("\n");
    }

    int64_t slotNs(int64_t slot) const { return slot * 1'000'000'000LL / fps; }
    LONGLONG slotHns(int64_t slot) const { return LONGLONG(slot * 10'000'000LL / fps); }

    // ------------------------------------------------------------------
    // Fragmented MP4 (moov up front, then a moof + mdat about every 0.3 s):
    // after a crash, a kill or a power cut everything up to the last
    // fragment still plays (a plain MP4 has no index until Finalize() and is
    // then unreadable). Finalize() adds the duration and an mfra index, so a
    // finished file plays and seeks in Media Player / Photos like a plain one.
    // Falls back to a plain MP4 if this Windows has no fragmented sink.
    bool fragmented = true;
    bool createWriter(bool hw) {
        writer.Reset();
        DeleteFileW(path.c_str());
        HRESULT hr = E_FAIL;
        for (int k = fragmented ? 0 : 1; k < 2 && !writer; ++k) {
            ComPtr<IMFAttributes> attr;
            MFCreateAttributes(&attr, 4);
            attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hw ? TRUE : FALSE);
            attr->SetGUID(MF_TRANSCODE_CONTAINERTYPE,
                          k == 0 ? MFTranscodeContainerType_FMPEG4 : MFTranscodeContainerType_MPEG4);
            hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attr.Get(), &writer);
            if (FAILED(hr)) {
                writer.Reset();
                DeleteFileW(path.c_str());
                if (k == 0) {
                    logf("fragmented MP4 not available (hr=0x%08lx); writing a plain MP4", (unsigned long)hr);
                    fragmented = false;
                }
            }
        }
        if (!writer) {
            logf("cannot create sink writer for the file (hr=0x%08lx)", (unsigned long)hr);
            return false;
        }
        hwCreate = hw;
        return true;
    }

    static uint32_t bitrateFor(int w, int h, int fps) {
        double bps = double(w) * h * fps * 0.07;  // 2560x1440@60 -> ~15.5 Mbps
        return uint32_t(std::clamp(bps, 3e6, 50e6));
    }

    HRESULT addStreams(int w, int h, bool hw) {
        const uint32_t br = bitrateFor(w, h, fps);
        ComPtr<IMFMediaType> vOut, vIn, aOut, aIn;
        HRESULT hr = MFCreateMediaType(&vOut);
        if (FAILED(hr)) return hr;
        vOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        vOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        vOut->SetUINT32(MF_MT_AVG_BITRATE, br);
        vOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        vOut->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        vOut->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        vOut->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        vOut->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        vOut->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        MFSetAttributeSize(vOut.Get(), MF_MT_FRAME_SIZE, UINT32(w), UINT32(h));
        MFSetAttributeRatio(vOut.Get(), MF_MT_FRAME_RATE, UINT32(fps), 1);
        MFSetAttributeRatio(vOut.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = writer->AddStream(vOut.Get(), &vStream);
        if (FAILED(hr)) return hr;

        MFCreateMediaType(&vIn);
        vIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        vIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        vIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        vIn->SetUINT32(MF_MT_DEFAULT_STRIDE, UINT32(w));
        vIn->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
        vIn->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        vIn->SetUINT32(MF_MT_SAMPLE_SIZE, UINT32(w * h * 3 / 2));
        vIn->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        vIn->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        vIn->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        vIn->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        MFSetAttributeSize(vIn.Get(), MF_MT_FRAME_SIZE, UINT32(w), UINT32(h));
        MFSetAttributeRatio(vIn.Get(), MF_MT_FRAME_RATE, UINT32(fps), 1);
        MFSetAttributeRatio(vIn.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        // Encoder settings (applied through ICodecAPI by the sink writer).
        ComPtr<IMFAttributes> enc;
        MFCreateAttributes(&enc, 6);
        enc->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_PeakConstrainedVBR);
        enc->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, br);
        enc->SetUINT32(CODECAPI_AVEncCommonMaxBitRate, br * 2);
        enc->SetUINT32(CODECAPI_AVEncMPVGOPSize, UINT32(fps * 2));
        enc->SetUINT32(CODECAPI_AVEncCommonQualityVsSpeed, 50);
        hr = writer->SetInputMediaType(vStream, vIn.Get(), enc.Get());
        if (FAILED(hr)) {
            // Some encoders reject one of the settings; retry with defaults.
            hr = writer->SetInputMediaType(vStream, vIn.Get(), nullptr);
            if (SUCCEEDED(hr)) logf("encoder rejected VBR settings; using its defaults at %u bps", br);
        }
        if (FAILED(hr)) return hr;

        MFCreateMediaType(&aOut);
        aOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        aOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        aOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        aOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, UINT32(outRate));
        aOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        aOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);  // 192 kbps
        aOut->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        aOut->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
        hr = writer->AddStream(aOut.Get(), &aStream);
        if (FAILED(hr)) return hr;
        MFCreateMediaType(&aIn);
        aIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        aIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        aIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        aIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, UINT32(outRate));
        aIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        aIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        aIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, UINT32(outRate * 4));
        hr = writer->SetInputMediaType(aStream, aIn.Get(), nullptr);
        if (FAILED(hr)) return hr;
        hr = writer->BeginWriting();
        if (FAILED(hr)) return hr;

        // Report which video encoder the sink writer picked.
        ComPtr<IMFSinkWriterEx> ex;
        std::string name = "?";
        bool isHw = false;
        if (SUCCEEDED(writer.As(&ex))) {
            for (DWORD i = 0;; ++i) {
                GUID cat{};
                ComPtr<IMFTransform> mft;
                if (FAILED(ex->GetTransformForStream(vStream, i, &cat, &mft))) break;
                if (cat != MFT_CATEGORY_VIDEO_ENCODER) continue;
                ComPtr<IMFAttributes> ma;
                if (SUCCEEDED(mft->GetAttributes(&ma))) {
                    wchar_t* fn = nullptr;
                    UINT32 len = 0;
                    if (SUCCEEDED(ma->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &fn, &len))) {
                        name.assign(len * 3 + 1, '\0');
                        int n = WideCharToMultiByte(CP_UTF8, 0, fn, int(len), name.data(), int(name.size()), nullptr,
                                                    nullptr);
                        name.resize(std::max(n, 0));
                        CoTaskMemFree(fn);
                    }
                    UINT32 dummy = 0;
                    isHw = SUCCEEDED(ma->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &dummy));
                } else {
                    name = "Microsoft H.264 encoder";
                }
                break;
            }
        }
        logf("writing %dx%d@%d H.264 %.1f Mbps (%s, %s) + AAC-LC %d Hz stereo 192 kbps, %s MP4", w, h, fps, br / 1e6,
             name.c_str(), isHw ? "hardware" : (hw ? "software fallback" : "software"), outRate,
             fragmented ? "fragmented" : "plain");
        return S_OK;
    }

    // Opens the file with canvas (w, h). Tries hardware, then software, then a
    // software-sized (<= 4096x2304 / 2304x4096) canvas.
    bool open(int w, int h) {
        w &= ~1;
        h &= ~1;
        struct Try {
            bool hw;
            int w, h;
        };
        std::vector<Try> tries{{true, w, h}, {false, w, h}};
        const int maxW = w >= h ? 4096 : 2304, maxH = w >= h ? 2304 : 4096;
        if (w > maxW || h > maxH) {
            double s = std::min(double(maxW) / w, double(maxH) / h);
            tries.push_back({false, int(w * s) & ~1, int(h * s) & ~1});
        }
        const int64_t openStart = steadyNowNs();
        for (size_t i = 0; i < tries.size(); ++i) {
            const Try& t = tries[i];
            if (!writer || hwCreate != t.hw || i > 0)
                if (!createWriter(t.hw)) return false;
            HRESULT hr = addStreams(t.w, t.h, t.hw);
            if (SUCCEEDED(hr)) {
                cw = t.w;
                chh = t.h;
                opened = true;
                logf("encoder ready %.0f ms after the first picture", (steadyNowNs() - openStart) / 1e6);
                return true;
            }
            logf("encoder setup failed for %dx%d (%s, hr=0x%08lx)", t.w, t.h, t.hw ? "hardware allowed" : "software",
                 (unsigned long)hr);
        }
        writer.Reset();
        return false;
    }

    // ------------------------------------------------------------------
    // Picture -> canvas-sized NV12 media buffer.
    ComPtr<IMFMediaBuffer> makeBuffer(const VFrame* f) {
        if (f && f->w == cw && f->h == chh) return f->buf;
        const DWORD size = DWORD(cw) * chh * 3 / 2;
        ComPtr<IMFMediaBuffer> buf = acquire(size);
        if (!buf) return nullptr;
        BYTE* p = nullptr;
        if (FAILED(buf->Lock(&p, nullptr, nullptr))) return nullptr;
        uint8_t* Y = p;
        uint8_t* UV = p + size_t(cw) * chh;
        BYTE* src = nullptr;
        if (f && SUCCEEDED(f->buf->Lock(&src, nullptr, nullptr))) {
            scale(*f, src, Y, UV);
            f->buf->Unlock();
        } else {
            std::memset(Y, 16, size_t(cw) * chh);
            std::memset(UV, 128, size_t(cw) * chh / 2);
        }
        buf->Unlock();
        buf->SetCurrentLength(size);
        return buf;
    }

    // Bilinear scale-to-fit (aspect kept, centred, black bars).
    void scale(const VFrame& f, const uint8_t* src, uint8_t* Y, uint8_t* UV) {
        if (f.w != scSrcW || f.h != scSrcH) {
            scSrcW = f.w;
            scSrcH = f.h;
            const double s = std::min(double(cw) / f.w, double(chh) / f.h);
            dw = std::clamp(int(std::lround(f.w * s)) & ~1, 2, cw);
            dh = std::clamp(int(std::lround(f.h * s)) & ~1, 2, chh);
            dx = ((cw - dw) / 2) & ~1;
            dy = ((chh - dh) / 2) & ~1;
            auto tables = [](int dst, int src, std::vector<int>& idx, std::vector<uint16_t>& fr) {
                idx.resize(dst);
                fr.resize(dst);
                const double r = double(src) / dst;
                for (int i = 0; i < dst; ++i) {
                    double x = std::clamp((i + 0.5) * r - 0.5, 0.0, double(src - 1));
                    int x0 = std::min(int(x), src - 2 < 0 ? 0 : src - 2);
                    idx[i] = x0;
                    fr[i] = uint16_t(std::lround((x - x0) * 256));
                }
            };
            tables(dw, f.w, xIdx, xFr);
            tables(dw / 2, f.w / 2, xIdxC, xFrC);
            logf("picture %dx%d scaled to %dx%d at (%d,%d) in the %dx%d canvas", f.w, f.h, dw, dh, dx, dy, cw, chh);
        }
        // Black bars (the pooled buffer may hold an older picture).
        for (int j = 0; j < chh; ++j) {
            uint8_t* r = Y + size_t(j) * cw;
            if (j < dy || j >= dy + dh) {
                std::memset(r, 16, cw);
            } else {
                std::memset(r, 16, dx);
                std::memset(r + dx + dw, 16, cw - dx - dw);
            }
        }
        for (int j = 0; j < chh / 2; ++j) {
            uint8_t* r = UV + size_t(j) * cw;
            if (j < dy / 2 || j >= (dy + dh) / 2) {
                std::memset(r, 128, cw);
            } else {
                std::memset(r, 128, dx);
                std::memset(r + dx + dw, 128, cw - dx - dw);
            }
        }
        const uint8_t* sY = src;
        const uint8_t* sUV = sY + size_t(f.w) * f.h;
        const int sw = f.w / 2, sh = f.h / 2, cdw = dw / 2, cdh = dh / 2;
        // Two passes per output row (vertical blend of two source rows into a
        // 16-bit row, then horizontal taps), split into stripes over threads.
        auto stripe = [&](int part, int parts) {
            std::vector<uint16_t> tmp(size_t(f.w) + 2);
            const int j0 = dh * part / parts, j1 = dh * (part + 1) / parts;
            for (int j = j0; j < j1; ++j) {
                const double y = std::clamp((j + 0.5) * f.h / dh - 0.5, 0.0, double(f.h - 1));
                const int y0 = std::min(int(y), std::max(f.h - 2, 0));
                const int fy = int(std::lround((y - y0) * 256));
                const uint8_t* r0 = sY + size_t(y0) * f.w;
                const uint8_t* r1 = r0 + (f.h > 1 ? f.w : 0);
                for (int x = 0; x < f.w; ++x) tmp[x] = uint16_t(r0[x] * (256 - fy) + r1[x] * fy);
                uint8_t* d = Y + size_t(dy + j) * cw + dx;
                for (int i = 0; i < dw; ++i) {
                    const int x0 = xIdx[i], fx = xFr[i];
                    d[i] = uint8_t((tmp[x0] * (256 - fx) + tmp[x0 + 1] * fx + 32768) >> 16);
                }
            }
            const int c0 = cdh * part / parts, c1 = cdh * (part + 1) / parts;
            for (int j = c0; j < c1; ++j) {
                const double y = std::clamp((j + 0.5) * sh / cdh - 0.5, 0.0, double(sh - 1));
                const int y0 = std::min(int(y), std::max(sh - 2, 0));
                const int fy = int(std::lround((y - y0) * 256));
                const uint8_t* r0 = sUV + size_t(y0) * f.w;
                const uint8_t* r1 = r0 + (sh > 1 ? f.w : 0);
                for (int x = 0; x < f.w; ++x) tmp[x] = uint16_t(r0[x] * (256 - fy) + r1[x] * fy);
                uint8_t* d = UV + size_t(dy / 2 + j) * cw + dx;
                for (int i = 0; i < cdw; ++i) {
                    const int x0 = xIdxC[i], fx = xFrC[i];
                    const int x1 = sw > 1 ? x0 + 1 : x0;
                    d[i * 2] = uint8_t((tmp[x0 * 2] * (256 - fx) + tmp[x1 * 2] * fx + 32768) >> 16);
                    d[i * 2 + 1] = uint8_t((tmp[x0 * 2 + 1] * (256 - fx) + tmp[x1 * 2 + 1] * fx + 32768) >> 16);
                }
            }
        };
        const int parts = std::clamp(int(std::thread::hardware_concurrency()) / 2, 1, 4);
        std::vector<std::thread> helpers;
        for (int p = 1; p < parts; ++p) helpers.emplace_back(stripe, p, parts);
        stripe(0, parts);
        for (auto& t : helpers) t.join();
    }

    bool check(HRESULT hr, const char* what) {
        if (SUCCEEDED(hr)) return true;
        if (!failed.exchange(true)) logf("%s failed (hr=0x%08lx); recording stopped", what, (unsigned long)hr);
        return false;
    }

    // Emits the held picture for slots [nextSlot, endSlot).
    void emitHeldUntil(int64_t endSlot) {
        while (nextSlot < endSlot && held && !failed) {
            ComPtr<IMFSample> s;
            if (!check(MFCreateSample(&s), "MFCreateSample")) return;
            s->AddBuffer(held.Get());
            const LONGLONG t = slotHns(nextSlot);
            s->SetSampleTime(t);
            s->SetSampleDuration(slotHns(nextSlot + 1) - t);
            const int64_t w0 = steadyNowNs();
            const HRESULT whr = writer->WriteSample(vStream, s.Get());
            writeNs += steadyNowNs() - w0;
            if (!check(whr, "WriteSample(video)")) return;
            ++nextSlot;
            ++statVideoFrames;
            if (heldEmitted) ++dupFrames;
            heldEmitted = true;
        }
    }
    bool heldEmitted = false;
    int64_t convNs = 0, writeNs = 0;

    void onPicture(const VFrame& f) {
        ++inputFrames;
        if (f.w != lastW || f.h != lastH) {
            if (lastW) logf("source size %dx%d -> %dx%d", lastW, lastH, f.w, f.h);
            lastW = f.w;
            lastH = f.h;
        }
        const int64_t pts = std::min(f.pts, steadyNowNs() + kMaxAheadNs);  // bad clock: no burst of duplicates
        const int64_t slot = (int64_t)std::llround(double(pts - t0) * fps / 1e9);
        if (slot > nextSlot) {
            emitHeldUntil(slot);
        } else if (held && !heldEmitted) {
            ++replaced;  // two pictures in one slot: newest wins
        }
        const int64_t c0 = steadyNowNs();
        ComPtr<IMFMediaBuffer> b = makeBuffer(&f);
        convNs += steadyNowNs() - c0;
        if (!b) {
            check(E_OUTOFMEMORY, "video buffer");
            return;
        }
        held = b;
        heldEmitted = false;
    }

    // ------------------------------------------------------------------
    HRESULT writePcm(const int16_t* st, size_t frames) {
        const size_t kBlock = 4096;
        while (frames > 0) {
            size_t n = std::min(frames, kBlock);
            ComPtr<IMFMediaBuffer> buf;
            HRESULT hr = MFCreateMemoryBuffer(DWORD(n * 4), &buf);
            if (FAILED(hr)) return hr;
            BYTE* p = nullptr;
            buf->Lock(&p, nullptr, nullptr);
            if (st)
                std::memcpy(p, st, n * 4);
            else
                std::memset(p, 0, n * 4);
            buf->Unlock();
            buf->SetCurrentLength(DWORD(n * 4));
            ComPtr<IMFSample> s;
            MFCreateSample(&s);
            s->AddBuffer(buf.Get());
            const LONGLONG t = LONGLONG(audioWritten * 10'000'000ULL / outRate);
            const LONGLONG e = LONGLONG((audioWritten + n) * 10'000'000ULL / outRate);
            s->SetSampleTime(t);
            s->SetSampleDuration(e - t);
            hr = writer->WriteSample(aStream, s.Get());
            if (FAILED(hr)) return hr;
            audioWritten += n;
            statAudioFrames = audioWritten;
            if (st) st += n * 2;
            frames -= n;
        }
        return S_OK;
    }

    void writeSilence(uint64_t frames) {
        silenceFrames += frames;
        rsHavePrev = false;
        check(writePcm(nullptr, size_t(frames)), "WriteSample(audio)");
    }

    void onAudio(const AChunk& c) {
        if (c.frames == 0 || c.ch <= 0 || c.rate <= 0) return;
        // To stereo.
        stereo.resize(c.frames * 2);
        for (size_t i = 0; i < c.frames; ++i) {
            const int16_t* s = &c.pcm[i * c.ch];
            stereo[i * 2] = s[0];
            stereo[i * 2 + 1] = c.ch > 1 ? s[1] : s[0];
        }
        const int16_t* src = stereo.data();
        size_t n = c.frames;
        if (c.rate != outRate) {  // linear resampler, continuous across chunks
            if (rsRate != c.rate) {
                rsRate = c.rate;
                rsHavePrev = false;
            }
            const double step = double(c.rate) / outRate;
            resampled.clear();
            // Virtual input sequence: [prev, stereo...]; rsPos indexes it.
            const int off = rsHavePrev ? 1 : 0;
            auto at = [&](size_t k, int ch) -> int { return (off && k == 0) ? rsPrev[ch] : stereo[(k - off) * 2 + ch]; };
            const size_t total = n + off;
            double pos = rsHavePrev ? rsPos : 0.0;
            while (pos + 1 < double(total)) {
                size_t k = size_t(pos);
                double fr = pos - k;
                for (int ch = 0; ch < 2; ++ch)
                    resampled.push_back(int16_t(std::lround(at(k, ch) * (1 - fr) + at(k + 1, ch) * fr)));
                pos += step;
            }
            rsPos = pos - double(total - 1);
            rsPrev[0] = stereo[(n - 1) * 2];
            rsPrev[1] = stereo[(n - 1) * 2 + 1];
            rsHavePrev = true;
            src = resampled.data();
            n = resampled.size() / 2;
            if (n == 0) return;
        }
        // Place by timestamp.
        // A stamp far in the future (bad clock) must not insert minutes of silence.
        const int64_t when = std::min(c.when, steadyNowNs() + kMaxAheadNs);
        const int64_t e = (int64_t)std::llround(double(when - t0) * outRate / 1e9);
        const int64_t A = int64_t(audioWritten);
        const int64_t tol = audioWritten == 0 ? 0 : kAudioTolNs * outRate / 1'000'000'000;
        if (e - A > tol) {
            writeSilence(uint64_t(e - A));
        } else if (A - e > tol) {
            const size_t drop = size_t(std::min<int64_t>(A - e, int64_t(n)));
            trimmedFrames += drop;
            src += drop * 2;
            n -= drop;
            if (n == 0) return;
        }
        check(writePcm(src, n), "WriteSample(audio)");
    }

    // ------------------------------------------------------------------
    void fillTo(int64_t relNs) {
        if (relNs <= 0) return;
        const int64_t slot = relNs * fps / 1'000'000'000;
        if (slot > nextSlot) {
            emitHeldUntil(slot);
        }
        const int64_t a = relNs * outRate / 1'000'000'000;
        if (a > int64_t(audioWritten)) writeSilence(uint64_t(a - int64_t(audioWritten)));
    }

    void updateStats() {
        const double v = double(nextSlot) / fps, a = double(audioWritten) / outRate;
        statSeconds = std::max(v, a);
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa))
            statBytes = (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    }

    bool openFor(const VFrame* firstPicture) {
        if (!pendingAudio.empty()) {
            const int r = pendingAudio.front().rate;
            outRate = (r == 44100 || r == 48000) ? r : (r > 44100 ? 48000 : 44100);
        } else {
            outRate = 44100;
        }
        const int w = firstPicture ? firstPicture->w : kDefaultW;
        const int h = firstPicture ? firstPicture->h : kDefaultH;
        if (!open(w, h)) {
            failed = true;
            encoderReady = true;
            logf("cannot start the encoder; nothing will be recorded");
            return false;
        }
        t0 = firstPicture ? firstPicture->pts : INT64_MAX;
        if (!pendingAudio.empty()) t0 = std::min(t0, pendingAudio.front().when);
        if (!firstPicture) held = makeBuffer(nullptr);  // black until a picture arrives
        heldEmitted = false;
        for (auto& c : pendingAudio) onAudio(c);
        pendingAudio.clear();
        pendingAudioNs = 0;
        return true;
    }

    void run(std::promise<bool> started) {
        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HRESULT hrMf = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        bool ok = SUCCEEDED(hrMf) && createWriter(true);
        if (!ok && FAILED(hrMf)) logf("MFStartup failed (hr=0x%08lx)", (unsigned long)hrMf);
        startNs = steadyNowNs();  // start() returns now (the start picture's stamp and grace period)
        started.set_value(ok);
        if (!ok) {
            if (SUCCEEDED(hrMf)) MFShutdown();
            if (SUCCEEDED(hrCo)) CoUninitialize();
            return;
        }
        const auto wallStart = std::chrono::steady_clock::now();
        FILETIME c0, e0, k0, u0;
        GetThreadTimes(GetCurrentThread(), &c0, &e0, &k0, &u0);

        std::deque<VFrame> vLocal;
        std::deque<AChunk> aLocal;
        bool stopping = false;
        while (true) {
            int64_t lag;
            {
                std::unique_lock<std::mutex> lk(qMu);
                qCv.wait_for(lk, std::chrono::milliseconds(50),
                             [&] { return stopReq || !vq.empty() || !aq.empty(); });
                vLocal.swap(vq);
                aLocal.swap(aq);
                aqNs = 0;
                stopping = stopReq;
                lag = std::max(kMinLagNs, maxLateNs + 500'000'000);
                if (startPic.buf) {
                    if (!vLocal.empty() || opened) {
                        startPic = {};  // a live picture came first
                    } else if (stopping || steadyNowNs() - startNs >= kStartPictureNs) {
                        logf("no live picture %.0f ms after start: starting from the picture on screen",
                             (steadyNowNs() - startNs) / 1e6);
                        vLocal.push_back(std::move(startPic));
                        startPic = {};
                    }
                }
            }
            if (!failed) {
                if (!opened) {
                    for (auto& c : aLocal) {
                        if (pendingAudio.empty()) firstPendingArrival = steadyNowNs();
                        pendingAudioNs += int64_t(c.frames) * 1'000'000'000 / c.rate;
                        pendingAudio.push_back(std::move(c));
                        while (pendingAudioNs > kMaxPendingAudioNs && pendingAudio.size() > 1) {
                            pendingAudioNs -= int64_t(pendingAudio.front().frames) * 1'000'000'000 /
                                              pendingAudio.front().rate;
                            pendingAudio.pop_front();
                        }
                    }
                    aLocal.clear();
                    if (!vLocal.empty())
                        openFor(&vLocal.front());
                    else if (!pendingAudio.empty() &&
                             (stopping || steadyNowNs() - firstPendingArrival > kAudioOnlyOpenNs))
                        openFor(nullptr);
                }
                if (opened && !failed) {
                    for (auto& c : aLocal) onAudio(c);
                    for (auto& f : vLocal) {
                        if (failed) break;
                        onPicture(f);
                    }
                    // Keep both tracks moving with real time (pauses, no audio).
                    if (!stopping) fillTo(steadyNowNs() - lag - t0);
                }
            }
            {
                std::lock_guard<std::mutex> lk(qMu);
                // Back to the small queue once the backlog from the encoder start is drained.
                if (opened && vq.size() < kMaxVideoQueue) encoderReady = true;
            }
            vLocal.clear();
            aLocal.clear();
            if (opened) updateStats();
            if (stopping) {
                std::lock_guard<std::mutex> lk(qMu);
                if (vq.empty() && aq.empty()) break;
            }
        }

        if (opened) {
            if (!failed) {
                // The live fill stays ~1 s behind; a still picture (or silence)
                // at the end lasts until stop() was called.
                if (stopAtNs > t0) fillTo(stopAtNs - t0);
                // Pad both tracks to a common end (last picture lasts one frame).
                const int64_t vEnd = slotNs(nextSlot + (held && !heldEmitted ? 1 : 0));
                const int64_t aEnd = int64_t(audioWritten) * 1'000'000'000 / outRate;
                int64_t endSlot = (std::max(vEnd, aEnd) * fps + 999'999'999) / 1'000'000'000;
                endSlot = std::max<int64_t>(endSlot, 1);
                emitHeldUntil(endSlot);
                const uint64_t aTarget = uint64_t(endSlot) * outRate / fps;
                if (aTarget > audioWritten) writeSilence(aTarget - audioWritten);
            }
            HRESULT hr = writer->Finalize();
            if (FAILED(hr)) logf("Finalize failed (hr=0x%08lx)", (unsigned long)hr);
            writer.Reset();
            updateStats();
            FILETIME c1, e1, k1, u1;
            GetThreadTimes(GetCurrentThread(), &c1, &e1, &k1, &u1);
            auto ft = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32 | f.dwLowDateTime) / 1e7; };
            const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
            const double cpu = ft(k1) - ft(k0) + ft(u1) - ft(u0);
            logf("finished: %.3f s, %llu video frames (%llu pictures in, %llu duplicated, %llu replaced, %llu dropped "
                 "in queue), %llu audio frames (%llu silence, %llu trimmed), %.1f MB; recorder thread CPU %.2f s over "
                 "%.1f s",
                 statSeconds.load(), (unsigned long long)statVideoFrames.load(), (unsigned long long)inputFrames,
                 (unsigned long long)std::max<int64_t>(int64_t(dupFrames), 0), (unsigned long long)replaced,
                 (unsigned long long)statDropped.load(), (unsigned long long)audioWritten,
                 (unsigned long long)silenceFrames, (unsigned long long)trimmedFrames, statBytes / 1e6, cpu, wall);
            logf("time in picture copy/scale %.2f s, in WriteSample(video) %.2f s", convNs / 1e9, writeNs / 1e9);
        } else {
            writer.Reset();
            DeleteFileW(path.c_str());
            logf("nothing was recorded (no picture or audio arrived); file not kept");
        }
        held.Reset();
        {
            std::lock_guard<std::mutex> lk(poolMu);
            pool.clear();
        }
        MFShutdown();
        if (SUCCEEDED(hrCo)) CoUninitialize();
    }
};

Recorder::Recorder() : d_(std::make_unique<Impl>()) { d_->owner = this; }

Recorder::~Recorder() { stop(); }

bool Recorder::start(const std::wstring& mp4Path, int fps) {
    std::lock_guard<std::mutex> ctl(d_->ctlMu);
    if (d_->thread.joinable()) return false;  // already recording
    Impl& d = *d_;
    d.path = mp4Path;
    d.fps = std::clamp(fps, 1, 240);
    d.failed = false;
    d.fragmented = true;
    d.opened = false;
    d.encoderReady = false;
    d.writer.Reset();
    d.held.Reset();
    d.heldEmitted = false;
    d.convNs = d.writeNs = 0;
    d.nextSlot = 0;
    d.audioWritten = 0;
    d.inputFrames = d.replaced = d.dupFrames = d.silenceFrames = d.trimmedFrames = 0;
    d.lastW = d.lastH = 0;
    d.rsHavePrev = false;
    d.rsRate = 0;
    d.scSrcW = d.scSrcH = 0;
    d.pendingAudio.clear();
    d.startPic = {};
    d.pendingAudioNs = 0;
    d.statSeconds = 0;
    d.statVideoFrames = d.statAudioFrames = d.statDropped = d.statBytes = 0;
    {
        std::lock_guard<std::mutex> lk(d.qMu);
        d.stopReq = false;
        d.vq.clear();
        d.aq.clear();
        d.aqNs = 0;
        d.maxLateNs = 0;
    }
    std::promise<bool> started;
    auto fut = started.get_future();
    d.thread = std::thread([this, p = std::move(started)]() mutable { d_->run(std::move(p)); });
    if (!fut.get()) {
        d.thread.join();
        return false;
    }
    d.running = true;
    return true;
}

void Recorder::stop() {
    std::lock_guard<std::mutex> ctl(d_->ctlMu);
    d_->running = false;
    if (!d_->thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(d_->qMu);
        d_->stopReq = true;
        d_->stopAtNs = steadyNowNs();
    }
    d_->qCv.notify_one();
    d_->thread.join();
}

bool Recorder::active() const { return d_->running && !d_->failed; }

Recorder::Stats Recorder::stats() const {
    Stats s;
    s.seconds = d_->statSeconds;
    s.videoFrames = d_->statVideoFrames;
    s.audioFrames = d_->statAudioFrames;
    s.droppedVideo = d_->statDropped;
    s.bytes = d_->statBytes;
    return s;
}

void Recorder::onVideoFrame(const uint8_t* nv12, int width, int height, int stride, uint64_t ptsNs) {
    if (!d_->running || d_->failed || !nv12 || width < 2 || height < 2 || stride < width) return;
    const int64_t now = steadyNowNs();
    const int64_t pts = toSteadyNs(ptsNs, now);
    const int w = width & ~1, h = height & ~1;
    const size_t frameBytes = size_t(w) * h * 3 / 2;
    {
        std::lock_guard<std::mutex> lk(d_->qMu);
        if (d_->stopReq) return;
        const int limit = d_->encoderReady ? kMaxVideoQueue
                                           : std::max<int>(kMaxVideoQueue, int(kOpeningBudget / frameBytes));
        if (int(d_->vq.size()) >= limit) {
            ++d_->statDropped;
            return;
        }
        d_->maxLateNs = std::clamp(now - pts, d_->maxLateNs, int64_t(10'000'000'000));
    }
    // Copy outside the queue lock.
    ComPtr<IMFMediaBuffer> buf = d_->acquire(DWORD(frameBytes));
    BYTE* p = nullptr;
    if (!buf || FAILED(buf->Lock(&p, nullptr, nullptr))) {
        ++d_->statDropped;
        return;
    }
    if (stride == w) {
        std::memcpy(p, nv12, size_t(w) * h);
    } else {
        for (int y = 0; y < h; ++y) std::memcpy(p + size_t(y) * w, nv12 + size_t(y) * stride, w);
    }
    const uint8_t* uv = nv12 + size_t(stride) * height;
    uint8_t* duv = p + size_t(w) * h;
    if (stride == w) {
        std::memcpy(duv, uv, size_t(w) * h / 2);
    } else {
        for (int y = 0; y < h / 2; ++y) std::memcpy(duv + size_t(y) * w, uv + size_t(y) * stride, w);
    }
    buf->Unlock();
    buf->SetCurrentLength(DWORD(frameBytes));
    {
        std::lock_guard<std::mutex> lk(d_->qMu);
        if (d_->stopReq) return;
        VFrame f;
        f.buf = std::move(buf);
        f.w = w;
        f.h = h;
        f.pts = pts;
        d_->vq.push_back(std::move(f));
    }
    d_->qCv.notify_one();
}

void Recorder::setStartPicture(const uint8_t* nv12, int width, int height, int stride) {
    if (!d_->running || d_->failed || !nv12 || width < 2 || height < 2 || stride < width) return;
    const int w = width & ~1, h = height & ~1;
    const size_t frameBytes = size_t(w) * h * 3 / 2;
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(MFCreateMemoryBuffer(DWORD(frameBytes), &buf))) return;
    BYTE* p = nullptr;
    if (FAILED(buf->Lock(&p, nullptr, nullptr))) return;
    for (int y = 0; y < h; ++y) std::memcpy(p + size_t(y) * w, nv12 + size_t(y) * stride, w);
    const uint8_t* uv = nv12 + size_t(stride) * height;
    for (int y = 0; y < h / 2; ++y) std::memcpy(p + size_t(w) * h + size_t(y) * w, uv + size_t(y) * stride, w);
    buf->Unlock();
    buf->SetCurrentLength(DWORD(frameBytes));
    {
        std::lock_guard<std::mutex> lk(d_->qMu);
        if (d_->stopReq) return;
        d_->startPic.buf = std::move(buf);
        d_->startPic.w = w;
        d_->startPic.h = h;
        d_->startPic.pts = d_->startNs;  // the recording starts with it
    }
    d_->qCv.notify_one();
}

void Recorder::onPcm(const int16_t* pcm, size_t frames, int channels, int sampleRate, uint64_t whenNs) {
    if (!d_->running || d_->failed || !pcm || frames == 0 || channels <= 0 || sampleRate < 8000) return;
    const int64_t now = steadyNowNs();
    AChunk c;
    c.pcm.assign(pcm, pcm + frames * size_t(channels));
    c.frames = frames;
    c.ch = channels;
    c.rate = sampleRate;
    c.when = toSteadyNs(whenNs, now);
    const int64_t ns = int64_t(frames) * 1'000'000'000 / sampleRate;
    {
        std::lock_guard<std::mutex> lk(d_->qMu);
        if (d_->stopReq) return;
        d_->maxLateNs = std::clamp(now - c.when, d_->maxLateNs, int64_t(10'000'000'000));
        d_->aq.push_back(std::move(c));
        d_->aqNs += ns;
        while (d_->aqNs > kMaxAudioQueueNs && d_->aq.size() > 1) {  // encoder stalled: drop oldest
            d_->aqNs -= int64_t(d_->aq.front().frames) * 1'000'000'000 / d_->aq.front().rate;
            d_->aq.pop_front();
        }
    }
    d_->qCv.notify_one();
}

}  // namespace pm
