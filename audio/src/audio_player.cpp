#include <pm/audio_player.h>

#include "audio_decoder.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ksmedia.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace pm {

namespace {

template <class T>
void safeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// UTC wall clock, Unix-epoch ns.
int64_t utcNowNs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    int64_t t = (int64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000LL) * 100;
}

// ntpLocalNs is usable if it is within 30 s of now on either clock.
bool plausibleStamp(uint64_t ts, int64_t steadyNow) {
    if (ts == 0) return false;
    constexpr int64_t kWin = 30'000'000'000LL;
    const int64_t t = int64_t(ts);
    if (t > steadyNow - kWin && t < steadyNow + kWin) return true;
    const int64_t u = utcNowNs();
    return t > u - kWin && t < u + kWin;
}

}  // namespace

struct AudioPlayer::Impl {
    explicit Impl(AudioPlayerConfig c) : cfg(std::move(c)) {
        wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    ~Impl() {
        if (wake) CloseHandle(wake);
    }

    AudioPlayerConfig cfg;

    void logf(const char* fmt, ...) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        if (cfg.log) {
            cfg.log(buf);
            return;
        }
        std::fprintf(stderr, "[audio] %s\n", buf);
        OutputDebugStringA("[audio] ");
        OutputDebugStringA(buf);
        OutputDebugStringA("\n");
    }

    // ---- decoder (touched by network thread) ----
    std::mutex decMu;
    std::unique_ptr<audio::AudioDecoder> dec;
    std::vector<int16_t> scratch;
    AudioPlayer::PcmMonitor monitor;  // guarded by decMu

    // ---- jitter buffer + format (shared with render thread) ----
    mutable std::mutex ringMu;
    std::vector<int16_t> ring;  // interleaved, capFrames * ch
    size_t capFrames = 0, rd = 0, count = 0;
    int rate = 0, ch = 0;
    uint64_t formatGen = 0;
    bool primed = false;
    size_t prefillFrames = 0;

    std::atomic<float> targetGain{1.0f};

    // ---- stream start timing (guarded by ringMu) ----
    // A "start" is the first packet after onFormat/onFlush, or after a gap of
    // more than kGapNs without packets (the phone stopped sending, e.g. pause).
    static constexpr int64_t kGapNs = 200'000'000;
    const char* markWhat = nullptr;  // "onFormat" / "onFlush" pending, else null
    int64_t markNs = 0;
    int64_t lastPacketNs = 0;
    int64_t startPacketNs = 0;  // first packet of the current start, 0 = none pending
    int64_t startGapNs = -1;    // mark/last packet -> first packet
    const char* startWhat = nullptr;
    std::atomic<double> statStartGapMs{-1}, statStartDelayMs{-1};
    std::atomic<uint64_t> statStarts{0};

    void markStart(const char* what) {  // caller holds ringMu
        markWhat = what;
        markNs = nowNs();
        startPacketNs = 0;
    }

    // ---- stats ----
    std::atomic<uint64_t> packets{0}, decodeErrors{0}, framesDecoded{0}, underruns{0},
        droppedFrames{0}, deviceReopens{0};
    std::atomic<double> deviceBufferMs{0};
    std::atomic<bool> deviceOpen{false};

    // ---- render thread ----
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> deviceChanged{false};
    HANDLE wake = nullptr;

    // Push decoded PCM. Drops oldest on overflow. Never blocks for long.
    void push(const int16_t* pcm, size_t frames, int pcmCh, int64_t arrivalNs) {
        std::lock_guard<std::mutex> lk(ringMu);
        if (pcmCh != ch || capFrames == 0) return;
        if (markWhat || (lastPacketNs && arrivalNs - lastPacketNs > kGapNs)) {
            startWhat = markWhat ? markWhat : "pause";
            startGapNs = arrivalNs - (markWhat ? markNs : lastPacketNs);
            startPacketNs = arrivalNs;
            markWhat = nullptr;
        }
        lastPacketNs = arrivalNs;
        if (frames > capFrames) {
            droppedFrames += frames - capFrames;
            pcm += (frames - capFrames) * ch;
            frames = capFrames;
        }
        if (count + frames > capFrames) {
            size_t drop = count + frames - capFrames;
            rd = (rd + drop) % capFrames;
            count -= drop;
            droppedFrames += drop;
        }
        size_t wr = (rd + count) % capFrames;
        size_t first = std::min(frames, capFrames - wr);
        std::memcpy(&ring[wr * ch], pcm, first * ch * sizeof(int16_t));
        if (frames > first) std::memcpy(&ring[0], pcm + first * ch, (frames - first) * ch * sizeof(int16_t));
        count += frames;
    }

    // Pull `frames` frames for format generation `gen` into float `out` with
    // `outCh` channels. Returns false if the format changed (caller must
    // reconfigure). Missing data is rendered as silence.
    bool pull(float* out, size_t frames, int outCh, uint64_t gen, float& curGain) {
        std::fill(out, out + frames * outCh, 0.0f);
        size_t got = 0;
        const char* startedWhat = nullptr;
        double gapMs = -1, delayMs = -1;
        {
            std::lock_guard<std::mutex> lk(ringMu);
            if (gen != formatGen) return false;
            if (!primed && count >= prefillFrames && count > 0) primed = true;
            if (primed) {
                got = std::min(frames, count);
                for (size_t f = 0; f < got; ++f) {
                    const int16_t* s = &ring[((rd + f) % capFrames) * ch];
                    for (int c = 0; c < outCh; ++c) out[f * outCh + c] = s[std::min(c, ch - 1)] * (1.0f / 32768.0f);
                }
                rd = (rd + got) % capFrames;
                count -= got;
                if (got && startPacketNs) {
                    startedWhat = startWhat;
                    delayMs = (nowNs() - startPacketNs) / 1e6;
                    gapMs = startGapNs / 1e6;
                    startPacketNs = 0;
                }
                if (got < frames) {
                    // Ran dry while playing: re-prime before resuming.
                    ++underruns;
                    primed = false;
                }
            }
        }
        if (startedWhat) {
            bool pause = startedWhat[0] == 'p';
            statStartGapMs = pause ? -1.0 : gapMs;
            statStartDelayMs = delayMs;
            ++statStarts;
            logf("stream start: first packet %.1f ms after %s, first PCM to device %.1f ms after first packet",
                 gapMs, pause ? "the previous packet (gap)" : startedWhat, delayMs);
        }
        // Volume, ramped across the block to avoid zipper noise.
        float tg = targetGain.load(std::memory_order_relaxed);
        if (got == 0) {
            curGain = tg;
            return true;
        }
        float g = curGain, step = (tg - curGain) / float(frames);
        for (size_t f = 0; f < frames; ++f, g += step)
            for (int c = 0; c < outCh; ++c) out[f * outCh + c] *= g;
        curGain = tg;
        return true;
    }

    bool currentFormat(int& r, int& c, uint64_t& gen) {
        std::lock_guard<std::mutex> lk(ringMu);
        r = rate;
        c = ch;
        gen = formatGen;
        return r > 0 && c > 0;
    }

    void run();
    void runTap();
    void runWasapi();
};

// ---------------------------------------------------------------------------
// Default-device change notifications.
class DeviceNotifier final : public IMMNotificationClient {
public:
    explicit DeviceNotifier(AudioPlayer::Impl* o) : owner_(o) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) {
            owner_->deviceChanged = true;
            SetEvent(owner_->wake);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    AudioPlayer::Impl* owner_;
    std::atomic<ULONG> ref_{1};
};

void AudioPlayer::Impl::run() {
    DWORD taskIdx = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);
    if (cfg.pcmTap)
        runTap();
    else
        runWasapi();
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

// Real-time paced pull clock without a device (tests / WAV capture).
void AudioPlayer::Impl::runTap() {
    using clock = std::chrono::steady_clock;
    std::vector<float> fbuf;
    std::vector<int16_t> sbuf;
    float curGain = targetGain.load();
    while (running) {
        int r, c;
        uint64_t gen;
        if (!currentFormat(r, c, gen)) {
            WaitForSingleObject(wake, 20);
            continue;
        }
        deviceOpen = true;
        ++deviceReopens;
        auto t0 = clock::now();
        uint64_t rendered = 0;
        while (running) {
            WaitForSingleObject(wake, 5);
            double el = std::chrono::duration<double>(clock::now() - t0).count();
            uint64_t due = uint64_t(el * r);
            if (due <= rendered) continue;
            size_t n = size_t(due - rendered);
            fbuf.resize(n * c);
            if (!pull(fbuf.data(), n, c, gen, curGain)) break;
            sbuf.resize(n * c);
            for (size_t i = 0; i < n * size_t(c); ++i)
                sbuf[i] = int16_t(std::lround(std::clamp(fbuf[i], -1.0f, 1.0f) * 32767.0f));
            cfg.pcmTap(sbuf.data(), n, c, r);
            rendered += n;
        }
    }
    deviceOpen = false;
}

void AudioPlayer::Impl::runWasapi() {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* enumr = nullptr;
    DeviceNotifier* notifier = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), (void**)&enumr))) {
        notifier = new DeviceNotifier(this);
        enumr->RegisterEndpointNotificationCallback(notifier);
    } else {
        logf("cannot create MMDeviceEnumerator");
    }

    HANDLE audioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::vector<float> fbuf;
    float curGain = targetGain.load();

    while (running && enumr) {
        int r, c;
        uint64_t gen;
        if (!currentFormat(r, c, gen)) {
            WaitForSingleObject(wake, 50);
            continue;
        }
        deviceChanged = false;
        const int64_t openStartNs = nowNs();

        IMMDevice* dev = nullptr;
        IAudioClient* client = nullptr;
        IAudioRenderClient* render = nullptr;
        UINT32 bufFrames = 0;
        HRESULT hr = enumr->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
        if (SUCCEEDED(hr)) {
            WAVEFORMATEXTENSIBLE wf{};
            wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            wf.Format.nChannels = WORD(c);
            wf.Format.nSamplesPerSec = DWORD(r);
            wf.Format.wBitsPerSample = 32;
            wf.Format.nBlockAlign = WORD(4 * c);
            wf.Format.nAvgBytesPerSec = DWORD(r) * wf.Format.nBlockAlign;
            wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            wf.Samples.wValidBitsPerSample = 32;
            wf.dwChannelMask = c == 1 ? SPEAKER_FRONT_CENTER
                             : c == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                                      : 0;  // let the engine pick for >2
            wf.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
            REFERENCE_TIME dur = REFERENCE_TIME(cfg.deviceBufferMs) * 10000;
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_NOPERSIST,
                                    dur, 0, &wf.Format, nullptr);
        }
        if (SUCCEEDED(hr)) hr = client->SetEventHandle(audioEvent);
        if (SUCCEEDED(hr)) hr = client->GetBufferSize(&bufFrames);
        if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioRenderClient), (void**)&render);
        if (SUCCEEDED(hr)) {
            BYTE* p = nullptr;
            if (SUCCEEDED(render->GetBuffer(bufFrames, &p)))
                render->ReleaseBuffer(bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
            hr = client->Start();
        }
        if (FAILED(hr)) {
            logf("WASAPI open failed (hr=0x%08lx); retrying", (unsigned long)hr);
            safeRelease(render);
            safeRelease(client);
            safeRelease(dev);
            deviceOpen = false;
            WaitForSingleObject(wake, 500);
            continue;
        }
        ++deviceReopens;
        deviceOpen = true;
        deviceBufferMs = 1000.0 * bufFrames / r;
        logf("WASAPI open: %d Hz %d ch, device buffer %u frames (%.1f ms), open took %.1f ms", r, c, bufFrames,
             1000.0 * bufFrames / r, (nowNs() - openStartNs) / 1e6);

        HANDLE waits[2] = {audioEvent, wake};
        while (running && !deviceChanged) {
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, 200);
            if (!running || deviceChanged) break;
            if (w == WAIT_OBJECT_0 + 1) {
                // Woken: format change / flush; check generation via pull below.
            }
            UINT32 pad = 0;
            hr = client->GetCurrentPadding(&pad);
            if (FAILED(hr)) break;
            UINT32 avail = bufFrames - pad;
            if (avail == 0) continue;
            fbuf.resize(size_t(avail) * c);
            bool same = pull(fbuf.data(), avail, c, gen, curGain);
            if (!same) break;  // format changed -> reopen with new rate/channels
            BYTE* p = nullptr;
            hr = render->GetBuffer(avail, &p);
            if (FAILED(hr)) break;
            std::memcpy(p, fbuf.data(), size_t(avail) * c * sizeof(float));
            hr = render->ReleaseBuffer(avail, 0);
            if (FAILED(hr)) break;
        }
        if (FAILED(hr)) logf("WASAPI stream error (hr=0x%08lx); reopening", (unsigned long)hr);
        else if (deviceChanged) logf("default audio device changed; reopening");
        client->Stop();
        safeRelease(render);
        safeRelease(client);
        safeRelease(dev);
        deviceOpen = false;
        deviceBufferMs = 0;
        if (FAILED(hr)) WaitForSingleObject(wake, 200);
    }

    if (enumr && notifier) enumr->UnregisterEndpointNotificationCallback(notifier);
    if (notifier) notifier->Release();
    safeRelease(enumr);
    CloseHandle(audioEvent);
    if (SUCCEEDED(hrCo)) CoUninitialize();
}

// ---------------------------------------------------------------------------

AudioPlayer::AudioPlayer(AudioPlayerConfig cfg) : d_(std::make_unique<Impl>(std::move(cfg))) {}

AudioPlayer::~AudioPlayer() { stop(); }

bool AudioPlayer::start() {
    if (d_->running.exchange(true)) return true;
    if (!d_->wake) return false;
    d_->thread = std::thread([this] { d_->run(); });
    return true;
}

void AudioPlayer::stop() {
    if (!d_->running.exchange(false)) return;
    SetEvent(d_->wake);
    if (d_->thread.joinable()) d_->thread.join();
}

AudioStats AudioPlayer::stats() const {
    AudioStats s;
    {
        std::lock_guard<std::mutex> lk(d_->ringMu);
        s.bufferMs = d_->rate > 0 ? 1000.0 * double(d_->count) / d_->rate : 0.0;
    }
    s.deviceBufferMs = d_->deviceBufferMs;
    s.packets = d_->packets;
    s.decodeErrors = d_->decodeErrors;
    s.framesDecoded = d_->framesDecoded;
    s.underruns = d_->underruns;
    s.droppedFrames = d_->droppedFrames;
    s.deviceReopens = d_->deviceReopens;
    s.deviceOpen = d_->deviceOpen;
    s.startGapMs = d_->statStartGapMs;
    s.startDelayMs = d_->statStartDelayMs;
    s.starts = d_->statStarts;
    return s;
}

float AudioPlayer::gain() const { return d_->targetGain.load(); }

float AudioPlayer::airplayDbToGain(float db) {
    if (!(db >= -30.0f)) return 0.0f;  // -144 = mute, NaN/below range -> mute
    if (db > 0.0f) db = 0.0f;
    return std::pow(10.0f, db / 20.0f);
}

void AudioPlayer::onFormat(AudioCodec codec, int sampleRate, int channels, int samplesPerFrame) {
    if (codec == AudioCodec::AAC_ELD || codec == AudioCodec::AAC_LC) {
        static std::once_flag once;  // once per process
        std::call_once(once, [this] {
            d_->logf("AAC decoder: %s%s", audio::aacDecoderInfo().c_str(),
                     audio::aacDecoderIsLgpl() ? "" : "  ** WARNING: not an LGPL-only build **");
        });
    }
    std::string err;
    auto dec = audio::createDecoder(codec, sampleRate, channels, samplesPerFrame, &err);
    if (!dec) d_->logf("onFormat ct=%d %d Hz %d ch spf=%d: %s", int(codec), sampleRate, channels, samplesPerFrame,
                   err.c_str());
    else
        d_->logf("onFormat ct=%d %d Hz %d ch spf=%d", int(codec), sampleRate, channels, samplesPerFrame);
    {
        std::lock_guard<std::mutex> lk(d_->decMu);
        d_->dec = std::move(dec);
    }
    {
        std::lock_guard<std::mutex> lk(d_->ringMu);
        // The core calls onFormat on every RTSP SETUP of the audio stream, and
        // the iPhone re-SETUPs on every play after a pause. The device only
        // depends on rate/channels: if those are unchanged keep the WASAPI
        // stream running (no reopen) and just start over with the new decoder.
        const bool sameOutput = d_->rate == sampleRate && d_->ch == channels && d_->capFrames > 0;
        d_->rate = sampleRate;
        d_->ch = channels;
        d_->capFrames = size_t(int64_t(sampleRate) * std::max(d_->cfg.maxBufferMs, 10) / 1000);
        d_->prefillFrames = size_t(int64_t(sampleRate) * std::max(d_->cfg.prefillMs, 0) / 1000);
        d_->prefillFrames = std::min(d_->prefillFrames, d_->capFrames);
        d_->ring.assign(d_->capFrames * size_t(channels), 0);
        d_->rd = d_->count = 0;
        d_->primed = false;
        d_->markStart("onFormat");
        if (sameOutput) return;
        ++d_->formatGen;  // render thread reopens the device (also picks up a new default device)
    }
    SetEvent(d_->wake);
}

void AudioPlayer::onPacket(const uint8_t* data, size_t len, uint64_t ntpLocalNs) {
    if (!data || len == 0) return;
    const int64_t arrival = nowNs();
    ++d_->packets;
    std::lock_guard<std::mutex> lk(d_->decMu);
    if (!d_->dec) return;
    d_->scratch.clear();
    int n = d_->dec->decode(data, len, d_->scratch);
    if (n < 0) {
        ++d_->decodeErrors;
        return;
    }
    d_->framesDecoded += uint64_t(n);
    if (n > 0) {
        d_->push(d_->scratch.data(), size_t(n), d_->dec->channels(), arrival);
        if (d_->monitor) {
            const uint64_t when = plausibleStamp(ntpLocalNs, arrival) ? ntpLocalNs : uint64_t(arrival);
            d_->monitor(d_->scratch.data(), size_t(n), d_->dec->channels(), d_->dec->sampleRate(), when);
        }
    }
}

void AudioPlayer::setPcmMonitor(PcmMonitor fn) {
    PcmMonitor old;
    {
        std::lock_guard<std::mutex> lk(d_->decMu);
        old = std::move(d_->monitor);
        d_->monitor = std::move(fn);
    }
    // `old` (and whatever it captured) is destroyed outside the lock.
}

void AudioPlayer::onVolume(float airplayDb) { d_->targetGain = airplayDbToGain(airplayDb); }

void AudioPlayer::onFlush() {
    std::lock_guard<std::mutex> lk(d_->ringMu);
    d_->rd = d_->count = 0;
    d_->primed = false;
    d_->markStart("onFlush");
}

}  // namespace pm
