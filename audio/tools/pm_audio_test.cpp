// pm_audio_test: encodes a 440 Hz tone + log sweep with fdk-aac exactly the way
// AirPlay screen mirroring sends audio (AAC-ELD, 44.1 kHz stereo, 480 spf, raw
// access units), feeds it through pm::AudioPlayer at real-time pace and plays
// it on the default device, or (--wav) captures the rendered PCM to a WAV.
//
//   pm_audio_test [--wav out.wav] [--codec eld|lc|alac] [--tone S] [--sweep S]
//                 [--volume dB] [--jitter MS]
//
// --flush-test: reproduces "pause on the iPhone, then play again" during
// mirroring: stream a tone for --play S, stop sending for --pause S, resume.
// --resume setup (default) re-sends onFormat(same format) at resume, which is
// what the core does on the RTSP SETUP the iPhone sends on every resume;
// --resume flush calls onFlush at pause; --resume gap does neither. Per cycle
// it reports the player's own timing (first packet -> first PCM handed to the
// device) and, on the real device, the time until the tone actually appears
// in the default device's mix (WASAPI loopback capture). Default volume -20 dB.
//
// --bench [N]: decodes the encoded AUs N times (default 20) with the
// receiver's decoder (FFmpeg libavcodec) and with fdk-aac's decoder as the
// reference: per-AU CPU cost, output length, best alignment lag (must be 0 =
// same timing/codec delay) and the difference between the two outputs.
//
// The AAC test vectors come from fdk-aac's *encoder* (PM_TEST_FDK, test-only;
// this tool is not shipped). Without it only --codec alac works.
#include <pm/audio_player.h>

#include "audio_decoder.h"

#if PM_TEST_FDK
#include <fdk-aac/aacdecoder_lib.h>
#include <fdk-aac/aacenc_lib.h>
#endif

#if PM_HAVE_ALAC
#include <ALACAudioTypes.h>
#include <ALACEncoder.h>
#endif

#include <windows.h>
#include <timeapi.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ksmedia.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Encoded {
    std::vector<std::vector<uint8_t>> aus;
    std::vector<uint8_t> asc;
    int frameLength = 0;
    int delay = 0;
};

#if PM_TEST_FDK
bool encode(pm::AudioCodec codec, const std::vector<int16_t>& pcm, int rate, int ch, Encoded& out) {
    HANDLE_AACENCODER enc = nullptr;
    if (aacEncOpen(&enc, 0, ch) != AACENC_OK) return false;
    bool eld = codec == pm::AudioCodec::AAC_ELD;
    bool ok = true;
    auto set = [&](AACENC_PARAM p, UINT v, const char* name) {
        AACENC_ERROR e = aacEncoder_SetParam(enc, p, v);
        if (e != AACENC_OK) {
            std::fprintf(stderr, "aacEncoder_SetParam(%s=%u) failed: 0x%x\n", name, v, e);
            ok = false;
        }
    };
    set(AACENC_AOT, eld ? AOT_ER_AAC_ELD : AOT_AAC_LC, "AOT");
    // AirPlay's ELD has no LD-SBR. The vcpkg "stripped" fdk-aac has no SBR at
    // all and rejects this parameter, which is fine.
    if (eld) aacEncoder_SetParam(enc, AACENC_SBR_MODE, 0);
    set(AACENC_SAMPLERATE, UINT(rate), "SAMPLERATE");
    set(AACENC_CHANNELMODE, ch == 2 ? MODE_2 : MODE_1, "CHANNELMODE");
    if (eld) set(AACENC_GRANULE_LENGTH, 480, "GRANULE_LENGTH");
    set(AACENC_TRANSMUX, TT_MP4_RAW, "TRANSMUX");
    set(AACENC_BITRATE, 128000, "BITRATE");
    set(AACENC_AFTERBURNER, 1, "AFTERBURNER");
    AACENC_ERROR ie = ok ? aacEncEncode(enc, nullptr, nullptr, nullptr, nullptr) : AACENC_OK;
    if (!ok || ie != AACENC_OK) {
        std::fprintf(stderr, "encoder configuration failed (init 0x%x)\n", ie);
        aacEncClose(&enc);
        return false;
    }
    AACENC_InfoStruct info{};
    aacEncInfo(enc, &info);
    out.asc.assign(info.confBuf, info.confBuf + info.confSize);
    out.frameLength = int(info.frameLength);
    out.delay = int(info.nDelay);

    std::vector<uint8_t> obuf(8192);
    size_t pos = 0;
    const size_t total = pcm.size();
    for (;;) {
        bool flushing = pos >= total;
        int inChunk = int(std::min<size_t>(size_t(out.frameLength) * ch, total - std::min(pos, total)));
        void* inPtr = const_cast<int16_t*>(pcm.data() + std::min(pos, total));
        int inId = IN_AUDIO_DATA, inSize = inChunk * 2, inElSize = 2;
        AACENC_BufDesc inDesc{};
        inDesc.numBufs = 1;
        inDesc.bufs = &inPtr;
        inDesc.bufferIdentifiers = &inId;
        inDesc.bufSizes = &inSize;
        inDesc.bufElSizes = &inElSize;
        void* outPtr = obuf.data();
        int outId = OUT_BITSTREAM_DATA, outSize = int(obuf.size()), outElSize = 1;
        AACENC_BufDesc outDesc{};
        outDesc.numBufs = 1;
        outDesc.bufs = &outPtr;
        outDesc.bufferIdentifiers = &outId;
        outDesc.bufSizes = &outSize;
        outDesc.bufElSizes = &outElSize;
        AACENC_InArgs inArgs{};
        inArgs.numInSamples = flushing ? -1 : inChunk;
        AACENC_OutArgs outArgs{};
        AACENC_ERROR e = aacEncEncode(enc, &inDesc, &outDesc, &inArgs, &outArgs);
        if (e == AACENC_ENCODE_EOF) break;
        if (e != AACENC_OK) {
            std::fprintf(stderr, "aacEncEncode error 0x%x\n", e);
            aacEncClose(&enc);
            return false;
        }
        pos += size_t(outArgs.numInSamples);
        if (outArgs.numOutBytes > 0) out.aus.emplace_back(obuf.begin(), obuf.begin() + outArgs.numOutBytes);
    }
    aacEncClose(&enc);
    return true;
}

// Reference decoder for --bench (what pm_audio used before FFmpeg). fdk's
// default PCM limiter (on for AAC-LC, off for ELD) adds ~15 ms (661 samples
// at 44.1 kHz) of delay; `limiter=false` gives the pure codec timing.
struct FdkRefDecoder {
    HANDLE_AACDECODER h = nullptr;
    std::vector<INT_PCM> pcm = std::vector<INT_PCM>(2048 * 8);
    int ch;
    FdkRefDecoder(const std::vector<uint8_t>& asc, int channels, bool limiter) : ch(channels) {
        h = aacDecoder_Open(TT_MP4_RAW, 1);
        UCHAR* conf[1] = {const_cast<UCHAR*>(asc.data())};
        UINT confLen[1] = {UINT(asc.size())};
        if (h && aacDecoder_ConfigRaw(h, conf, confLen) != AAC_DEC_OK) {
            aacDecoder_Close(h);
            h = nullptr;
        }
        if (h) {
            aacDecoder_SetParam(h, AAC_PCM_MAX_OUTPUT_CHANNELS, ch);
            aacDecoder_SetParam(h, AAC_PCM_MIN_OUTPUT_CHANNELS, ch);
            if (!limiter) aacDecoder_SetParam(h, AAC_PCM_LIMITER_ENABLE, 0);
        }
    }
    ~FdkRefDecoder() {
        if (h) aacDecoder_Close(h);
    }
    int decode(const uint8_t* data, size_t len, std::vector<int16_t>& out) {
        UCHAR* in[1] = {const_cast<UCHAR*>(data)};
        UINT inLen[1] = {UINT(len)};
        UINT valid = UINT(len);
        if (aacDecoder_Fill(h, in, inLen, &valid) != AAC_DEC_OK) return -1;
        if (aacDecoder_DecodeFrame(h, pcm.data(), INT(pcm.size()), 0) != AAC_DEC_OK) return -1;
        const CStreamInfo* info = aacDecoder_GetStreamInfo(h);
        if (!info || info->frameSize <= 0 || info->numChannels <= 0) return -1;
        pm::audio::remixAppend(pcm.data(), info->frameSize, info->numChannels, ch, out);
        return info->frameSize;
    }
};
#endif  // PM_TEST_FDK

#if PM_HAVE_ALAC
// ALAC 16-bit, 352 spf, like AirPlay ct=2 (last frame zero-padded to 352).
bool encodeAlac(const std::vector<int16_t>& pcm, int rate, int ch, Encoded& out) {
    const uint32_t spf = 352;
    ALACEncoder e;
    e.SetFrameSize(spf);
    AudioFormatDescription in{double(rate), kALACFormatLinearPCM,
                              kALACFormatFlagIsSignedInteger | kALACFormatFlagIsPacked,
                              uint32_t(2 * ch), 1, uint32_t(2 * ch), uint32_t(ch), 16, 0};
    AudioFormatDescription fmt{double(rate), kALACFormatAppleLossless, 1, 0, spf, 0, uint32_t(ch), 0, 0};
    if (e.InitializeEncoder(fmt) != 0) return false;
    uint32_t csz = e.GetMagicCookieSize(uint32_t(ch));
    out.asc.resize(csz);
    e.GetMagicCookie(out.asc.data(), &csz);
    out.asc.resize(csz);
    out.frameLength = int(spf);
    std::vector<int16_t> frame(spf * ch);
    std::vector<uint8_t> obuf(spf * ch * 2 * 2 + 64);
    for (size_t pos = 0; pos < pcm.size(); pos += spf * ch) {
        size_t n = std::min<size_t>(spf * ch, pcm.size() - pos);
        std::fill(frame.begin(), frame.end(), int16_t(0));
        std::copy(pcm.begin() + pos, pcm.begin() + pos + n, frame.begin());
        int32_t bytes = int32_t(frame.size() * 2);
        if (e.Encode(in, fmt, reinterpret_cast<unsigned char*>(frame.data()), obuf.data(), &bytes) != 0)
            return false;
        out.aus.emplace_back(obuf.begin(), obuf.begin() + bytes);
    }
    return true;
}
#endif

bool writeWav(const std::string& path, const std::vector<int16_t>& s, int rate, int ch) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    uint32_t data = uint32_t(s.size() * 2);
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + data);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);
    u16(uint16_t(ch));
    u32(uint32_t(rate));
    u32(uint32_t(rate * ch * 2));
    u16(uint16_t(ch * 2));
    u16(16);
    std::fwrite("data", 1, 4, f);
    u32(data);
    std::fwrite(s.data(), 2, s.size(), f);
    std::fclose(f);
    return true;
}

std::string hex(const std::vector<uint8_t>& v) {
    std::string s;
    char b[4];
    for (uint8_t x : v) {
        std::snprintf(b, sizeof b, "%02x", x);
        s += b;
    }
    return s;
}

void printStats(const char* tag, const pm::AudioStats& s) {
    std::printf("%-8s buffer=%5.1f ms dev=%4.1f ms packets=%llu decErr=%llu decoded=%llu underruns=%llu "
                "dropped=%llu reopens=%llu\n",
                tag, s.bufferMs, s.deviceBufferMs, (unsigned long long)s.packets,
                (unsigned long long)s.decodeErrors, (unsigned long long)s.framesDecoded,
                (unsigned long long)s.underruns, (unsigned long long)s.droppedFrames,
                (unsigned long long)s.deviceReopens);
}

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Captures what the default render device actually mixes (WASAPI loopback) as
// 1 ms blocks of (steady_clock ns, peak |sample|). steady_clock is QPC based,
// the capture QPC position is in 100 ns QPC units: same timeline.
class LoopbackMeter {
public:
    struct Block {
        int64_t ns;
        float peak;
    };
    bool start() {
        running_ = true;
        thread_ = std::thread([this] { run(); });
        for (int i = 0; i < 200 && !ready_ && running_; ++i) Sleep(5);
        return ready_;
    }
    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }
    std::vector<Block> blocks() {
        std::lock_guard<std::mutex> lk(mu_);
        return blocks_;
    }

private:
    void run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* en = nullptr;
        IMMDevice* dev = nullptr;
        IAudioClient* ac = nullptr;
        IAudioCaptureClient* cap = nullptr;
        WAVEFORMATEX* wf = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), (void**)&en);
        if (SUCCEEDED(hr)) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
        if (SUCCEEDED(hr)) hr = ac->GetMixFormat(&wf);
        if (SUCCEEDED(hr))
            hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, wf, nullptr);
        if (SUCCEEDED(hr)) hr = ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cap);
        if (SUCCEEDED(hr)) hr = ac->Start();
        bool isFloat = false;
        if (SUCCEEDED(hr)) {
            isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                      (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                       reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wf)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
            std::printf("loopback: %lu Hz %u ch %u bit %s\n", wf->nSamplesPerSec, wf->nChannels, wf->wBitsPerSample,
                        isFloat ? "float" : "int");
            ready_ = true;
        } else {
            std::printf("loopback capture unavailable (hr=0x%08lx)\n", (unsigned long)hr);
            running_ = false;
        }
        const int rate = ready_ ? int(wf->nSamplesPerSec) : 0, nch = ready_ ? wf->nChannels : 0;
        const int bps = ready_ ? wf->wBitsPerSample : 0;
        const int blk = std::max(1, rate / 250);  // 4 ms
        const double w = 2 * kPi * toneHz_ / std::max(rate, 1), coeff = 2 * std::cos(w);
        while (running_) {
            Sleep(2);
            UINT32 n = 0;
            while (SUCCEEDED(cap->GetNextPacketSize(&n)) && n) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 pos = 0, qpc = 0;
                if (FAILED(cap->GetBuffer(&data, &frames, &flags, &pos, &qpc))) break;
                std::lock_guard<std::mutex> lk(mu_);
                for (UINT32 f0 = 0; f0 < frames; f0 += blk) {
                    float pk = 0;
                    UINT32 f1 = std::min<UINT32>(frames, f0 + blk);
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                        double s1 = 0, s2 = 0;
                        for (UINT32 f = f0; f < f1; ++f) {
                            size_t i = size_t(f) * nch;
                            double v = isFloat ? reinterpret_cast<float*>(data)[i]
                                       : bps == 16 ? reinterpret_cast<int16_t*>(data)[i] / 32768.0
                                                   : reinterpret_cast<int32_t*>(data)[i] / 2147483648.0;
                            double s0 = v + coeff * s1 - s2;
                            s2 = s1;
                            s1 = s0;
                        }
                        double pw = s1 * s1 + s2 * s2 - coeff * s1 * s2;
                        pk = float(2.0 * std::sqrt(std::max(pw, 0.0)) / std::max<UINT32>(1, f1 - f0));
                    }
                    blocks_.push_back({int64_t(qpc) * 100 + int64_t(f0) * 1000000000LL / rate, pk});
                }
                cap->ReleaseBuffer(frames);
            }
        }
        if (ac) ac->Stop();
        if (wf) CoTaskMemFree(wf);
        if (cap) cap->Release();
        if (ac) ac->Release();
        if (dev) dev->Release();
        if (en) en->Release();
        CoUninitialize();
    }
public:
    double toneHz_ = 440.0;

private:
    std::thread thread_;
    std::atomic<bool> running_{false}, ready_{false};
    std::mutex mu_;
    std::vector<Block> blocks_;
};

int runFlushTest(pm::AudioCodec codec, const Encoded& enc, int rate, int ch, int spf, bool tap, double volumeDb,
                 int cycles, double playSec, double pauseSec, const std::string& resume, double setupGapMs,
                 double toneHz) {
    pm::AudioPlayerConfig cfg;
    cfg.log = [](const std::string& l) {
        std::printf("  [player %.3f] %s\n", (nowNs() % 100000000000LL) / 1e9, l.c_str());
    };
    if (tap) cfg.pcmTap = [](const int16_t*, size_t, int, int) {};
    LoopbackMeter meter;
    meter.toneHz_ = toneHz;
    bool haveMeter = !tap && meter.start();
    pm::AudioPlayer player(cfg);
    player.start();
    player.onVolume(float(volumeDb));
    player.onFormat(codec, rate, ch, spf);
    std::printf("flush test: %d cycles, play %.2f s, pause %.2f s, resume=%s, volume %.1f dB (gain %.3f), %s\n",
                cycles, playSec, pauseSec, resume.c_str(), volumeDb, player.gain(),
                tap ? "tap clock (no device)" : "default WASAPI device");
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    using clock = std::chrono::steady_clock;
    const size_t perCycle = std::min(enc.aus.size(), size_t(playSec * rate / spf));
    struct Cycle {
        int64_t firstPacketNs, pauseNs;
        double gapMs, delayMs;
    };
    std::vector<Cycle> res;
    for (int cyc = 0; cyc <= cycles; ++cyc) {
        if (cyc > 0) {
            if (resume == "setup") {
                player.onFormat(codec, rate, ch, spf);
                if (setupGapMs > 0) std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(setupGapMs));
            }
        }
        uint64_t startsBefore = player.stats().starts;
        auto t0 = clock::now();
        int64_t firstNs = 0;
        for (size_t i = 0; i < perCycle; ++i) {
            double due = double(i) * spf / rate;
            std::this_thread::sleep_until(t0 + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(due)));
            if (i == 0) firstNs = nowNs();
            player.onPacket(enc.aus[i].data(), enc.aus[i].size(), 0);
        }
        auto st = player.stats();
        bool measured = st.starts > startsBefore;
        res.push_back({firstNs, 0, measured ? st.startGapMs : -1, measured ? st.startDelayMs : -1});
        // pause: the phone stops sending
        res.back().pauseNs = nowNs();
        if (resume == "flush") player.onFlush();
        std::this_thread::sleep_for(std::chrono::duration<double>(pauseSec));
    }
    auto fin = player.stats();
    player.stop();
    timeEndPeriod(1);
    std::vector<LoopbackMeter::Block> blocks;
    if (haveMeter) {
        meter.stop();
        blocks = meter.blocks();
    }
    // Steady level of the tone in the mix (cycle 0, 300 ms after start .. pause).
    float steady = 0;
    for (auto& b : blocks)
        if (b.ns > res[0].firstPacketNs + 300000000LL && b.ns < res[0].pauseNs) steady = std::max(steady, b.peak);
    // Other audio playing on the PC also leaks into the detector: stay above
    // the loudest level seen while we were paused.
    float pauseNoise = 0;
    for (size_t k = 1; k < res.size(); ++k)
        for (auto& b : blocks)
            if (b.ns >= res[k - 1].pauseNs + 300000000LL && b.ns < res[k].firstPacketNs - 5000000LL)
                pauseNoise = std::max(pauseNoise, b.peak);
    const float thr = std::max(steady * 0.25f, pauseNoise * 1.2f);
    if (haveMeter)
        std::printf("loopback: steady %.0f Hz tone level %.4f, onset threshold %.4f%s\n", toneHz, steady, thr,
                    steady < 1e-4f ? " (no signal captured: muted/other device?)" : "");
    double sumDelay = 0, sumAudible = 0;
    int nDelay = 0, nAudible = 0;
    for (size_t k = 0; k < res.size(); ++k) {
        double audibleMs = -1, noise = 0;
        if (haveMeter && steady >= 1e-4f) {
            int64_t from = res[k].firstPacketNs - 5000000LL;
            int64_t prevPause = k ? res[k - 1].pauseNs + 300000000LL : 0;
            for (auto& b : blocks) {
                if (k && b.ns >= prevPause && b.ns < res[k].firstPacketNs) noise = std::max<double>(noise, b.peak);
                if (b.ns >= from && b.peak > thr) {
                    audibleMs = (b.ns - res[k].firstPacketNs) / 1e6;
                    break;
                }
            }
        }
        std::printf("cycle %zu%s: player first packet->first PCM to device %6.1f ms; tone in device mix %7.1f ms "
                    "after first packet%s",
                    k, k == 0 ? " (initial open)" : "", res[k].delayMs, audibleMs, haveMeter ? "" : " (n/a)");
        if (k) std::printf(" (pause noise peak %.4f)", noise);
        std::printf("\n");
        if (k) {
            if (res[k].delayMs >= 0) sumDelay += res[k].delayMs, ++nDelay;
            if (audibleMs >= 0) sumAudible += audibleMs, ++nAudible;
        }
    }
    std::printf("RESULT resume=%s: mean first packet->first PCM %.1f ms (%d), mean first packet->tone in mix %.1f ms "
                "(%d); device opens %llu, underruns %llu\n",
                resume.c_str(), nDelay ? sumDelay / nDelay : -1.0, nDelay, nAudible ? sumAudible / nAudible : -1.0,
                nAudible, (unsigned long long)fin.deviceReopens, (unsigned long long)fin.underruns);
    return 0;
}

}  // namespace

// --bench: CPU cost + output equivalence of the receiver's decoder vs fdk-aac.
int runBench(pm::AudioCodec codec, const Encoded& enc, int rate, int ch, int spf, int reps) {
    using clock = std::chrono::steady_clock;
    std::printf("decoder: %s\n", pm::audio::aacDecoderInfo().c_str());
    std::vector<int16_t> outPm, outRef;
    double pmUs = 1e30, refUs = 1e30;
    uint64_t pmErr = 0, refErr = 0;
    for (int r = 0; r < reps; ++r) {
        std::string err;
        auto dec = pm::audio::createDecoder(codec, rate, ch, spf, &err);
        if (!dec) {
            std::fprintf(stderr, "createDecoder: %s\n", err.c_str());
            return 1;
        }
        std::vector<int16_t> o;
        o.reserve(enc.aus.size() * size_t(spf) * ch);
        auto t = clock::now();
        for (auto& au : enc.aus)
            if (dec->decode(au.data(), au.size(), o) < 0) ++pmErr;
        pmUs = std::min(pmUs, std::chrono::duration<double, std::micro>(clock::now() - t).count());
        if (r == 0) outPm = std::move(o);
    }
#if PM_TEST_FDK
    for (int r = 0; r < reps; ++r) {
        FdkRefDecoder ref(pm::audio::makeAacAsc(codec, rate, ch, spf), ch, false);
        if (!ref.h) return 1;
        std::vector<int16_t> o;
        o.reserve(enc.aus.size() * size_t(spf) * ch);
        auto t = clock::now();
        for (auto& au : enc.aus)
            if (ref.decode(au.data(), au.size(), o) < 0) ++refErr;
        refUs = std::min(refUs, std::chrono::duration<double, std::micro>(clock::now() - t).count());
        if (r == 0) outRef = std::move(o);
    }
    std::vector<int16_t> outOld;  // fdk with its default config, as pm_audio used it
    {
        FdkRefDecoder ref(pm::audio::makeAacAsc(codec, rate, ch, spf), ch, true);
        for (auto& au : enc.aus) ref.decode(au.data(), au.size(), outOld);
    }
#endif
    const double n = double(enc.aus.size());
    std::printf("bench %zu AUs x %d reps (best rep): libavcodec %.2f us/AU (%.3f%% of real time), errors %llu\n",
                enc.aus.size(), reps, pmUs / n, 100.0 * pmUs / (n * spf * 1e6 / rate),
                (unsigned long long)(pmErr / reps));
    if (outRef.empty()) {
        std::printf("(no fdk-aac reference decoder: PM_TEST_FDK off)\n");
        return pmErr ? 1 : 0;
    }
    std::printf("                                       fdk-aac    %.2f us/AU (%.3f%% of real time), errors %llu\n",
                refUs / n, 100.0 * refUs / (n * spf * 1e6 / rate), (unsigned long long)(refErr / reps));
    std::printf("output frames: libavcodec %zu, fdk-aac %zu\n", outPm.size() / ch, outRef.size() / ch);
    // Best lag of libavcodec vs fdk (channel 0, normalised cross-correlation).
    const size_t frames = std::min(outPm.size(), outRef.size()) / ch;
    int bestLag = 0;
    double best = -2;
    for (int lag = -2048; lag <= 2048; ++lag) {
        double xy = 0, xx = 0, yy = 0;
        for (size_t f = 4096; f + 4096 < frames && f < 4096 + 44100; ++f) {
            double x = outRef[f * ch], y = outPm[(f + lag) * ch];
            xy += x * y, xx += x * x, yy += y * y;
        }
        double c = xx > 0 && yy > 0 ? xy / std::sqrt(xx * yy) : 0;
        if (c > best) best = c, bestLag = lag;
    }
    double err2 = 0, sig2 = 0;
    int maxDiff = 0;
    for (size_t i = 0; i < frames * ch; ++i) {
        int d = int(outPm[i]) - int(outRef[i]);
        maxDiff = std::max(maxDiff, std::abs(d));
        err2 += double(d) * d, sig2 += double(outRef[i]) * outRef[i];
    }
    // First non-silent sample (|x| > 64) in each output: same codec delay?
    auto onset = [&](const std::vector<int16_t>& v) {
        for (size_t i = 0; i < v.size(); ++i)
            if (std::abs(v[i]) > 64) return long(i / ch);
        return -1L;
    };
    std::printf("alignment vs fdk (limiter off): best lag %d samples (corr %.6f), onset libavcodec %ld / fdk %ld; "
                "at lag 0: max |diff| %d LSB, diff %.1f dB below signal\n",
                bestLag, best, onset(outPm), onset(outRef), maxDiff,
                err2 > 0 ? 10 * std::log10(sig2 / err2) : 999.0);
#if PM_TEST_FDK
    std::printf("fdk default config (previous pm_audio): onset %ld -> libavcodec output is %.2f ms earlier\n",
                onset(outOld), double(onset(outOld) - onset(outPm)) * 1000.0 / rate);
#endif
    const bool ok = pmErr == 0 && outPm.size() == outRef.size() && bestLag == 0;
    std::printf("bench: %s\n", ok ? "OK (same length, same timing)" : "FAIL");
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    std::string wavPath;
    pm::AudioCodec codec = pm::AudioCodec::AAC_ELD;
    double toneSec = 3.0, sweepSec = 3.0, volumeDb = 0.0, jitterMs = 0.0;
    bool flushTest = false, volumeSet = false, monitorTest = false;
    int benchReps = 0;
    int cycles = 4;
    double playSec = 1.0, pauseSec = 1.0, setupGapMs = 0.0;
    std::string resume = "setup";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--wav") wavPath = next();
        else if (a == "--codec") {
            std::string c = next();
            codec = c == "lc" ? pm::AudioCodec::AAC_LC : c == "alac" ? pm::AudioCodec::ALAC : pm::AudioCodec::AAC_ELD;
        }
        else if (a == "--tone") toneSec = std::atof(next());
        else if (a == "--sweep") sweepSec = std::atof(next());
        else if (a == "--volume") volumeDb = std::atof(next()), volumeSet = true;
        else if (a == "--flush-test") flushTest = true;
        else if (a == "--cycles") cycles = std::atoi(next());
        else if (a == "--play") playSec = std::atof(next());
        else if (a == "--pause") pauseSec = std::atof(next());
        else if (a == "--resume") resume = next();
        else if (a == "--setup-gap") setupGapMs = std::atof(next());
        else if (a == "--tap") wavPath = "-";
        else if (a == "--jitter") jitterMs = std::atof(next());
        else if (a == "--monitor") monitorTest = true;
        else if (a == "--bench") benchReps = (i + 1 < argc && std::atoi(argv[i + 1]) > 0) ? std::atoi(argv[++i]) : 20;
        else {
            std::printf("usage: pm_audio_test [--wav out.wav] [--codec eld|lc|alac] [--tone S] [--sweep S] "
                        "[--volume dB] [--jitter MS] [--monitor]\n"
                        "       pm_audio_test --bench [N] [--codec eld|lc]\n"
                        "       pm_audio_test --flush-test [--tap] [--cycles N] [--play S] [--pause S] "
                        "[--resume setup|flush|gap] [--setup-gap MS] [--volume dB]\n");
            return 2;
        }
    }

    const int rate = 44100, ch = 2;
    const int spf = codec == pm::AudioCodec::AAC_ELD ? 480 : codec == pm::AudioCodec::ALAC ? 352 : 1024;
    if (flushTest) {
        toneSec = playSec + 0.1;
        sweepSec = 0;
        if (!volumeSet) volumeDb = -20.0;
    }

    // 440 Hz tone, then 100 Hz -> 10 kHz log sweep, both at -6 dBFS.
    std::vector<int16_t> pcm;
    size_t toneN = size_t(toneSec * rate), sweepN = size_t(sweepSec * rate);
    pcm.reserve((toneN + sweepN) * ch);
    const double toneFreq = flushTest ? 1000.0 : 440.0;  // 4 ms detector blocks need >= 250 Hz
    for (size_t n = 0; n < toneN; ++n) {
        int16_t v = int16_t(std::lround(16384.0 * std::sin(2 * kPi * toneFreq * n / rate)));
        for (int c = 0; c < ch; ++c) pcm.push_back(v);
    }
    const double f0 = 100.0, f1 = 10000.0, k = std::log(f1 / f0);
    for (size_t n = 0; n < sweepN; ++n) {
        double t = double(n) / rate;
        double phase = 2 * kPi * f0 * sweepSec / k * (std::exp(t / sweepSec * k) - 1.0);
        int16_t v = int16_t(std::lround(16384.0 * std::sin(phase)));
        for (int c = 0; c < ch; ++c) pcm.push_back(v);
    }

    Encoded enc;
    std::vector<uint8_t> expectAsc;
    const char* codecName = "AAC-ELD";
    if (codec == pm::AudioCodec::ALAC) {
#if PM_HAVE_ALAC
        codecName = "ALAC";
        if (!encodeAlac(pcm, rate, ch, enc)) {
            std::fprintf(stderr, "ALAC encode failed\n");
            return 1;
        }
        expectAsc = pm::audio::makeAlacCookie(rate, ch, spf);
#else
        std::fprintf(stderr, "built without ALAC\n");
        return 1;
#endif
    } else {
        if (codec == pm::AudioCodec::AAC_LC) codecName = "AAC-LC";
#if PM_TEST_FDK
        if (!encode(codec, pcm, rate, ch, enc)) return 1;
#else
        std::fprintf(stderr, "built without PM_TEST_FDK: no AAC test encoder (use --codec alac)\n");
        return 1;
#endif
        expectAsc = pm::audio::makeAacAsc(codec, rate, ch, spf);
    }
    // For ALAC, maxFrameBytes/avgBitRate (cookie bytes 12..19) legitimately differ.
    auto cfgEq = [&] {
        if (codec != pm::AudioCodec::ALAC) return enc.asc == expectAsc;
        return enc.asc.size() >= 24 && std::equal(expectAsc.begin(), expectAsc.begin() + 12, enc.asc.begin()) &&
               std::equal(expectAsc.begin() + 20, expectAsc.begin() + 24, enc.asc.begin() + 20);
    };
    std::printf("encoder: %s %d Hz %d ch, frameLength=%d delay=%d, %zu AUs, config=%s (receiver uses %s) %s\n",
                codecName, rate, ch, enc.frameLength, enc.delay, enc.aus.size(), hex(enc.asc).c_str(),
                hex(expectAsc).c_str(), cfgEq() ? "MATCH" : "MISMATCH");
    if (codec == pm::AudioCodec::ALAC) {
        // Lossless: decoding directly must reproduce the source bit-exactly.
        std::string err;
        auto dec = pm::audio::createDecoder(codec, rate, ch, spf, &err);
        std::vector<int16_t> outPcm;
        for (auto& au : enc.aus)
            if (!dec || dec->decode(au.data(), au.size(), outPcm) < 0) break;
        outPcm.resize(std::min(outPcm.size(), pcm.size()));
        std::printf("ALAC direct decode: %zu/%zu samples, bit-exact=%s\n", outPcm.size(), pcm.size(),
                    outPcm.size() == pcm.size() && outPcm == pcm ? "yes" : "NO");
    }
    if (enc.frameLength != spf) {
        std::fprintf(stderr, "unexpected encoder frame length\n");
        return 1;
    }
    if (!enc.aus.empty()) std::printf("first AU byte: 0x%02x\n", enc.aus[0][0]);
    if (benchReps > 0) {
        if (codec == pm::AudioCodec::ALAC) {
            std::fprintf(stderr, "--bench is for AAC\n");
            return 2;
        }
        return runBench(codec, enc, rate, ch, spf, benchReps);
    }
    if (flushTest)
        return runFlushTest(codec, enc, rate, ch, spf, !wavPath.empty(), volumeDb, cycles, playSec, pauseSec, resume,
                            setupGapMs, toneFreq);

    pm::AudioPlayerConfig cfg;
    std::mutex wavMu;
    std::vector<int16_t> wav;
    if (!wavPath.empty()) {
        cfg.pcmTap = [&](const int16_t* p, size_t frames, int c, int) {
            std::lock_guard<std::mutex> lk(wavMu);
            wav.insert(wav.end(), p, p + frames * c);
        };
    }
    pm::AudioPlayer player(cfg);
    player.start();
    player.onFormat(codec, rate, ch, spf);
    player.onVolume(float(volumeDb));
    std::printf("volume %.1f dB -> gain %.4f\n", volumeDb, player.gain());

    // 1 ms timer resolution so the sleep-paced sender (and the --wav pull
    // clock) are not quantised to the 15.6 ms default tick.
    timeBeginPeriod(1);
    // Behave like a network receive thread, not a background task.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    using clock = std::chrono::steady_clock;
    std::mt19937 rng(1234);
    std::uniform_real_distribution<double> jit(0.0, jitterMs);
    auto t0 = clock::now();
    auto nextReport = t0 + std::chrono::seconds(1);
    double maxPushUs = 0, maxLateMs = 0;
    // --monitor: setPcmMonitor alongside normal playback. Odd packets carry a
    // plausible ntpLocalNs (steady clock, +100 ms) which must come back
    // unchanged; even packets carry 0 -> arrival time (within 5 ms of the call).
    std::atomic<uint64_t> monFrames{0}, monCalls{0}, monBadStamp{0};
    std::atomic<uint64_t> expectStamp{0};
    std::atomic<int64_t> callNs{0};
    if (monitorTest)
        player.setPcmMonitor([&](const int16_t*, size_t frames, int c, int r, uint64_t when) {
            monFrames += frames;
            ++monCalls;
            const uint64_t e = expectStamp.load();
            const int64_t d = e ? int64_t(when - e) : int64_t(when) - callNs.load();
            if (c != ch || r != rate || (e ? d != 0 : (d < 0 || d > 5'000'000))) ++monBadStamp;
        });
    auto steadyNs = [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count();
    };
    for (size_t i = 0; i < enc.aus.size(); ++i) {
        double due = double(i) * spf / rate + (jitterMs > 0 ? jit(rng) / 1000.0 : 0.0);
        std::this_thread::sleep_until(t0 + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(due)));
        auto a = clock::now();
        maxLateMs = std::max(maxLateMs, std::chrono::duration<double, std::milli>(a - t0).count() - due * 1000.0);
        uint64_t ntp = 0;
        if (monitorTest) {
            callNs = steadyNs();
            ntp = (i & 1) ? uint64_t(callNs.load() + 100'000'000) : 0;
            expectStamp = ntp;
        }
        player.onPacket(enc.aus[i].data(), enc.aus[i].size(), ntp);
        maxPushUs = std::max(maxPushUs, std::chrono::duration<double, std::micro>(clock::now() - a).count());
        if (clock::now() >= nextReport) {
            printStats("live", player.stats());
            nextReport += std::chrono::seconds(1);
        }
    }
    auto endStats = player.stats();
    printStats("fed-all", endStats);
    if (monitorTest) {
        player.setPcmMonitor(nullptr);
        std::printf("monitor: %llu calls, %llu frames (decoder produced %llu), bad stamp/format %llu -> %s\n",
                    (unsigned long long)monCalls.load(), (unsigned long long)monFrames.load(),
                    (unsigned long long)endStats.framesDecoded, (unsigned long long)monBadStamp.load(),
                    monFrames == endStats.framesDecoded && monBadStamp == 0 ? "OK" : "FAIL");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // drain
    player.stop();
    auto fin = player.stats();
    printStats("final", fin);
    std::printf("max onPacket time: %.1f us; max sender lateness %.1f ms; underruns while streaming: %llu\n",
                maxPushUs, maxLateMs, (unsigned long long)endStats.underruns);

    if (!wavPath.empty()) {
        if (!writeWav(wavPath, wav, rate, ch)) {
            std::fprintf(stderr, "cannot write %s\n", wavPath.c_str());
            return 1;
        }
        std::printf("wrote %s: %zu frames (%.3f s), source %.3f s, decoded %.3f s\n", wavPath.c_str(),
                    wav.size() / ch, double(wav.size() / ch) / rate, double(toneN + sweepN) / rate,
                    double(fin.framesDecoded) / rate);
    }
    return fin.decodeErrors == 0 ? 0 : 1;
}
