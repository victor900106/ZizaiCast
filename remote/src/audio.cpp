#include "audio.h"

#include <windows.h>
#include <initguid.h>  // PKEY_Device_FriendlyName defined in this file
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <opus.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>

#include "level.h"
#include "synth_audio.h"
#include "util.h"

namespace pm::remote {

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kRate = 48000, kFrame = 960;  // 20 ms
constexpr float kSounding = 0.002f;         // ~ -54 dBFS: this device is playing something
constexpr int64_t kWindowUs = 250000;       // level windows for the automatic choice

std::string idOf(IMMDevice* d) {
    LPWSTR id = nullptr;
    std::string s;
    if (d && SUCCEEDED(d->GetId(&id)) && id) s = narrow(id), CoTaskMemFree(id);
    return s;
}

std::string nameOf(IMMDevice* d) {
    ComPtr<IPropertyStore> ps;
    std::string s;
    if (d && SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) s = narrow(v.pwszVal);
        PropVariantClear(&v);
    }
    return s.empty() ? "(unnamed device)" : s;
}

std::string defaultId(IMMDeviceEnumerator* en, ERole role) {
    ComPtr<IMMDevice> d;
    if (!en || FAILED(en->GetDefaultAudioEndpoint(eRender, role, &d))) return {};
    return idOf(d.Get());
}

// Device list / default changes -> the capture loop rebuilds (on its own thread).
class AudioNotify final : public IMMNotificationClient {
public:
    explicit AudioNotify(std::atomic<bool>* dirty) : dirty_(dirty) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** p) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *p = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return mark(); }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return mark(); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return mark(); }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) defaultChanged = true, mark();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
    std::atomic<bool> defaultChanged{false};

private:
    HRESULT mark() {
        *dirty_ = true;
        return S_OK;
    }
    std::atomic<ULONG> ref_{1};
    std::atomic<bool>* dirty_;
};

// One loopback capture (one playback device), converted to 48 kHz stereo float.
struct Cap {
    std::string id, name;
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioClient> ac;
    ComPtr<IAudioCaptureClient> cc;
    int channels = 2, rate = kRate, bits = 32;
    bool isFloat = true, failed = false;
    double resPos = 0;
    float lastL = 0, lastR = 0;
    float winPeak = 0, lastWin = 0, statPeak = 0;
    float wins[4] = {0, 0, 0, 0};  // the last ~1 s of window peaks
    int winIdx = 0;
    // 0.8.3 volume compensation: the endpoint's master volume (dB) when Windows applies it in software
    ComPtr<IAudioEndpointVolume> vol;
    bool hwVolume = false;
    float comp = 1.f, compDb = 0.f;

    bool open() {
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(ac.GetAddressOf())))) return false;
        // 48 kHz float stereo (the audio engine converts); else the device's own mix format, converted here.
        WAVEFORMATEXTENSIBLE want{};
        want.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        want.Format.nChannels = 2;
        want.Format.nSamplesPerSec = kRate;
        want.Format.wBitsPerSample = 32;
        want.Format.nBlockAlign = 8;
        want.Format.nAvgBytesPerSec = kRate * 8;
        want.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        want.Samples.wValidBitsPerSample = 32;
        want.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        want.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        const REFERENCE_TIME bufDur = 200000;  // 20 ms
        WAVEFORMATEX* mix = nullptr;
        ac->GetMixFormat(&mix);
        HRESULT hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                    bufDur, 0, &want.Format, nullptr);
        channels = 2, rate = kRate, bits = 32, isFloat = true;
        if (FAILED(hr) && mix) {
            ac.Reset();
            dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(ac.GetAddressOf()));
            hr = ac ? ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, bufDur, 0, mix, nullptr) : E_FAIL;
            channels = mix->nChannels, rate = int(mix->nSamplesPerSec), bits = mix->wBitsPerSample;
            isFloat = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                      (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                       reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        }
        if (mix) CoTaskMemFree(mix);
        if (FAILED(hr) || FAILED(ac->GetService(IID_PPV_ARGS(&cc))) || FAILED(ac->Start())) {
            logf("audio: loopback of %s failed %s", name.c_str(), hresultText(hr).c_str());
            ac.Reset(), cc.Reset();
            return false;
        }
        return true;
    }
    void close() {
        if (ac) ac->Stop();
        cc.Reset(), ac.Reset();
    }
    // Drains what the device played; appended to `out` (48 kHz stereo) when given.
    float drainPeak = 0;  // of the last drain() call
    void drain(std::vector<float>* out, int64_t& firstUs) {
        drainPeak = 0;
        UINT32 n = 0;
        HRESULT hr;
        firstUs = 0;
        for (hr = cc->GetNextPacketSize(&n); SUCCEEDED(hr) && n > 0; hr = cc->GetNextPacketSize(&n)) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 qpc = 0;
            if (FAILED(hr = cc->GetBuffer(&data, &frames, &flags, nullptr, &qpc))) break;
            if (!firstUs) firstUs = qpc ? int64_t(qpc / 10) : nowUs();
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            auto sample = [&](UINT32 f, int c) -> float {
                if (silent || !data) return 0.f;
                const int ch = std::min(c, channels - 1);
                const size_t i = size_t(f) * size_t(channels) + size_t(ch);
                if (isFloat) return reinterpret_cast<const float*>(data)[i];
                if (bits == 16) return reinterpret_cast<const int16_t*>(data)[i] / 32768.f;
                if (bits == 32) return float(reinterpret_cast<const int32_t*>(data)[i] / 2147483648.0);
                return 0.f;
            };
            float peak = 0;
            if (rate == kRate) {
                for (UINT32 f = 0; f < frames; ++f) {
                    const float l = sample(f, 0), r = sample(f, 1);
                    if (out) out->push_back(l), out->push_back(r);
                    peak = std::max({peak, std::fabs(l), std::fabs(r)});
                }
            } else {
                const double step = double(rate) / kRate;  // input frames per output frame
                for (UINT32 f = 0; f < frames; ++f) {
                    const float l = sample(f, 0), r = sample(f, 1);
                    while (resPos < 1.0) {
                        if (out) {
                            out->push_back(float(lastL + (l - lastL) * resPos));
                            out->push_back(float(lastR + (r - lastR) * resPos));
                        }
                        resPos += step;
                    }
                    resPos -= 1.0;
                    lastL = l, lastR = r;
                    peak = std::max({peak, std::fabs(l), std::fabs(r)});
                }
            }
            cc->ReleaseBuffer(frames);
            winPeak = std::max(winPeak, std::min(1.f, peak));
            drainPeak = std::max(drainPeak, std::min(1.f, peak));
        }
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED || FAILED(hr)) failed = true;
    }
    float recent() const { return std::max({wins[0], wins[1], wins[2], wins[3]}); }
};

}  // namespace

// 0.8.3: how many programs play to this device right now (sessions in the Active state).
int activeSessionsOf(IMMDevice* dev) {
    ComPtr<IAudioSessionManager2> mgr;
    if (!dev || FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(mgr.GetAddressOf()))))
        return 0;
    ComPtr<IAudioSessionEnumerator> e;
    int n = 0, active = 0;
    if (FAILED(mgr->GetSessionEnumerator(&e)) || FAILED(e->GetCount(&n))) return 0;
    for (int i = 0; i < n; ++i) {
        ComPtr<IAudioSessionControl> c;
        AudioSessionState st = AudioSessionStateInactive;
        if (SUCCEEDED(e->GetSession(i, &c)) && SUCCEEDED(c->GetState(&st)) && st == AudioSessionStateActive) ++active;
    }
    return active;
}

// The master volume scalar and whether it is applied in hardware (read only).
void volumeOf(IMMDevice* dev, float& scalar, bool& hw, bool& muted) {
    scalar = -1, hw = false, muted = false;
    ComPtr<IAudioEndpointVolume> v;
    if (!dev || FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(v.GetAddressOf())))) return;
    DWORD mask = 0;
    if (SUCCEEDED(v->QueryHardwareSupport(&mask))) hw = (mask & ENDPOINT_HARDWARE_SUPPORT_VOLUME) != 0;
    float s = -1;
    if (SUCCEEDED(v->GetMasterVolumeLevelScalar(&s))) scalar = s;
    BOOL m = FALSE;
    if (SUCCEEDED(v->GetMute(&m))) muted = m != FALSE;
}

std::vector<PlaybackDevice> playbackDevicesList() {
    std::vector<PlaybackDevice> v;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // an STA caller (UI thread) keeps its STA
    {
        ComPtr<IMMDeviceEnumerator> en;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
            const std::string def = defaultId(en.Get(), eConsole);
            ComPtr<IMMDeviceCollection> c;
            UINT n = 0;
            if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &c)) && SUCCEEDED(c->GetCount(&n)))
                for (UINT i = 0; i < n; ++i) {
                    ComPtr<IMMDevice> d;
                    if (FAILED(c->Item(i, &d))) continue;
                    PlaybackDevice p;
                    p.id = idOf(d.Get());
                    p.name = nameOf(d.Get());
                    p.isDefault = p.id == def;
                    volumeOf(d.Get(), p.volume, p.hwVolume, p.muted);
                    p.activeSessions = activeSessionsOf(d.Get());
                    v.push_back(std::move(p));
                }
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return v;
}

bool LoopbackAudio::start(OutputFn fn, bool mutePc, const std::string& source) {
    stop();
    out_ = std::move(fn);
    muteWanted_ = mutePc;
    dirty_mute_ = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        source_ = source;
        curName_.clear(), others_.clear();
    }
    stop_ = false;
    running_ = true;
    th_ = std::thread([this] { loop(); });
    return true;
}

void LoopbackAudio::stop() {
    stop_ = true;
    if (th_.joinable()) th_.join();
    running_ = false;
    recentPeak_ = 0;
}

void LoopbackAudio::setSource(const std::string& source) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (source_ == source) return;
        source_ = source;
    }
    dirty_ = true;
}

void LoopbackAudio::takeStats(int& packets, double& avgMs, double& peak, std::string& device, std::string& others) {
    packets = packets_.exchange(0);
    const int64_t sum = latSumUs_.exchange(0);
    avgMs = packets ? sum / 1000.0 / packets : 0;
    peak = peakMicro_.exchange(0) / 1e6;
    std::lock_guard<std::mutex> lk(mu_);
    device = curName_;
    others = others_;
    statsTaken_ = true;
}

void LoopbackAudio::setMute(bool on) {
    if (muteWanted_.exchange(on) != on) dirty_mute_ = true;
}

// Mixes every captured device (0.8.2; 0.8.1 picked one and kept jumping to the
// HDMI noise in the music's short pauses, owner's log 23:10:18-23:11:13):
// each device's loopback goes into its own FIFO (48 kHz stereo); every 20 ms
// on the wall clock one frame is taken from each FIFO that has sound, summed,
// soft-limited, encoded. A device's FIFO is kept between 30 and 150 ms.
void LoopbackAudio::loop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(kRate, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    if (!enc) {
        logf("audio: opus_encoder_create failed (%d)", err);
        running_ = false;
        CoUninitialize();
        return;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(128000));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(6));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    const int64_t t0 = nowUs();
    constexpr size_t kPrime = 1440, kKeep = 2880, kMax = 7200;  // 30 / 60 / 150 ms of frames
    std::vector<uint8_t> packet(4000);
    std::vector<float> mixed(size_t(kFrame) * 2);

    ComPtr<IMMDeviceEnumerator> en;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    AudioNotify* notify = en ? new AudioNotify(&dirty_) : nullptr;
    if (notify && FAILED(en->RegisterEndpointNotificationCallback(notify))) notify->Release(), notify = nullptr;

    struct Fifo {
        std::vector<float> s;  // interleaved stereo
        size_t head = 0;       // frames consumed (in samples / 2)
        int64_t lastDataUs = 0, firstUs = 0;
        int64_t dropped = 0;
        size_t frames() const { return s.size() / 2 - head; }
        void compact() {
            if (head > 9600) s.erase(s.begin(), s.begin() + ptrdiff_t(head * 2)), head = 0;
        }
    };
    std::vector<std::unique_ptr<Cap>> caps;
    std::vector<Fifo> fifos;
    std::string defId;
    // 手機播聲音時電腦靜音: every captured device, restored to how it was.
    struct Muted {
        ComPtr<IAudioEndpointVolume> vol;
        std::string name;
        int64_t checkUntil = 0, packets = 0;
        float peak = 0;
    };
    std::map<std::string, Muted> muted;  // device id -> muted by us
    bool muteOn = false;
    auto unmuteAll = [&](const char* why) {
        for (auto& [id, m] : muted)
            if (m.vol) m.vol->SetMute(FALSE, nullptr), logf("audio: %s unmuted (%s)", m.name.c_str(), why);
        muted.clear();
    };
    auto applyMute = [&] {
        const bool want = muteWanted_.load();
        if (!want) {
            if (!muted.empty()) unmuteAll("the phone's sound is off, or the option is off");
            muteOn = false;
            return;
        }
        muteOn = true;
        for (auto& c : caps) {
            if (muted.count(c->id)) continue;
            Muted m;
            m.name = c->name;
            if (FAILED(c->dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(m.vol.GetAddressOf()))))
                continue;
            BOOL was = FALSE;
            m.vol->GetMute(&was);
            if (was) continue;  // already muted by the user: left alone
            if (SUCCEEDED(m.vol->SetMute(TRUE, nullptr))) {
                m.checkUntil = nowUs() + 3000000;
                logf("audio: %s muted while the phone plays the sound (endpoint mute; the loopback is checked for 3 s)", c->name.c_str());
                muted[c->id] = std::move(m);
            }
        }
    };
    auto rebuild = [&] {
        std::string src;
        {
            std::lock_guard<std::mutex> lk(mu_);
            src = source_;
        }
        defId = defaultId(en.Get(), eConsole);
        const std::string defMm = defaultId(en.Get(), eMultimedia), defComm = defaultId(en.Get(), eCommunications);
        struct D {
            ComPtr<IMMDevice> dev;
            std::string id, name;
        };
        std::vector<D> act;
        ComPtr<IMMDeviceCollection> col;
        UINT n = 0;
        if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) && SUCCEEDED(col->GetCount(&n)))
            for (UINT i = 0; i < n; ++i) {
                D d;
                if (FAILED(col->Item(i, &d.dev))) continue;
                d.id = idOf(d.dev.Get()), d.name = nameOf(d.dev.Get());
                act.push_back(std::move(d));
            }
        auto present = [&](const std::string& id) {
            return std::any_of(act.begin(), act.end(), [&](const D& d) { return d.id == id; });
        };
        std::vector<std::string> want;
        const bool automatic = src.empty();
        if (automatic)
            for (const D& d : act) want.push_back(d.id);
        else if (src != "default" && present(src))
            want.push_back(src);
        else {
            if (src != "default") logf("audio: the chosen playback device is not there now: the Windows default instead");
            want.push_back(defId);
        }
        std::string list;
        for (const D& d : act) {
            std::string roles;
            if (d.id == defId) roles += "console";
            if (d.id == defMm) roles += roles.empty() ? "multimedia" : "+multimedia";
            if (d.id == defComm) roles += roles.empty() ? "communications" : "+communications";
            const bool cap = std::find(want.begin(), want.end(), d.id) != want.end();
            list += (list.empty() ? "" : "; ") + d.name + (roles.empty() ? "" : " [Windows default: " + roles + "]") + (cap ? "" : " (not captured)");
        }
        logf("audio: playback devices (source: %s): %s", automatic ? "automatic = all of them mixed" : src == "default" ? "Windows default" : "chosen device",
             list.empty() ? "none" : list.c_str());
        std::vector<std::unique_ptr<Cap>> next;
        std::vector<Fifo> nextF;
        for (const std::string& id : want) {
            auto it = std::find_if(caps.begin(), caps.end(), [&](const std::unique_ptr<Cap>& c) { return c && c->id == id && !c->failed; });
            if (it != caps.end()) {
                nextF.push_back(std::move(fifos[size_t(it - caps.begin())]));
                next.push_back(std::move(*it));
                continue;
            }
            auto d = std::find_if(act.begin(), act.end(), [&](const D& x) { return x.id == id; });
            if (d == act.end()) continue;
            auto c = std::make_unique<Cap>();
            c->id = d->id, c->name = d->name, c->dev = d->dev;
            if (c->open()) {
                if (SUCCEEDED(c->dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(c->vol.GetAddressOf())))) {
                    DWORD mask = 0;
                    if (SUCCEEDED(c->vol->QueryHardwareSupport(&mask))) c->hwVolume = (mask & ENDPOINT_HARDWARE_SUPPORT_VOLUME) != 0;
                }
                logf("audio: capturing %s (%d ch, %d Hz %s%d, %s)", c->name.c_str(), c->channels, c->rate, c->isFloat ? "float" : "int", c->bits,
                     c->hwVolume ? "hardware volume: the loopback is at full level" : "software volume: compensated in the loopback (0.8.3)");
                next.push_back(std::move(c));
                nextF.emplace_back();
            }
        }
        for (auto& c : caps)
            if (c) c->close();
        // Devices no longer captured get their mute back.
        for (auto it = muted.begin(); it != muted.end();) {
            const bool still = std::any_of(next.begin(), next.end(), [&](const std::unique_ptr<Cap>& c) { return c->id == it->first; });
            if (!still && it->second.vol) it->second.vol->SetMute(FALSE, nullptr), logf("audio: %s unmuted (not captured any more)", it->second.name.c_str());
            it = still ? std::next(it) : muted.erase(it);
        }
        caps = std::move(next), fifos = std::move(nextF);
        std::string names;
        for (auto& c : caps) names += (names.empty() ? "" : " + ") + c->name;
        {
            std::lock_guard<std::mutex> lk(mu_);
            curName_ = caps.size() > 1 ? "mix: " + names : names;
        }
        if (caps.empty()) logf("audio: no playback device to capture");
        if (muteOn || (muteWanted_.load())) applyMute();
    };
    // Tests (silent): a synthetic source instead of WASAPI, two "devices" fed
    // into the same FIFO -> mix -> limiter -> Opus path. Nothing is played.
    //   "music": A = 440 Hz at 0.5, 3 s on / 3 s off; B = noise floor at 0.02 (like the HDMI device)
    //   "loud":  A and B = full-scale 440 / 660 Hz at 0.9 each (the sum would clip without the limiter)
    //   "quiet": (0.8.3.2) A = music-like material at peak 0.03 (synth_audio.h; 0.8.3 used a 0.03 sine,
    //            which has 3 dB between peak and RMS where music has ~13), B = noise floor 0.002
    //   "speech": (0.8.3.2) A = speech-like material at peak 0.03, B = noise floor 0.002
    std::string synth;
    {
        std::lock_guard<std::mutex> lk(mu_);
        synth = synthetic_;
    }
    std::vector<int64_t> synthPos;
    std::vector<float> synthLoop;  // 0.8.3.2: "quiet" / "speech" (8 s, peak 1)
    if (synth == "quiet") synthLoop = pm::remote::synth::musicLike();
    else if (synth == "speech") synthLoop = pm::remote::synth::speechLike();
    uint32_t noise = 12345;
    const int64_t synthT0 = nowUs();
    if (!synth.empty()) {
        fifos.resize(2);
        synthPos.assign(2, 0);
        std::lock_guard<std::mutex> lk(mu_);
        curName_ = synth == "loud"     ? "mix: synthetic A 440 Hz 0.9 + synthetic B 660 Hz 0.9"
                   : synth == "quiet"  ? "mix: synthetic quiet music-like material, peak 0.03 + synthetic noise floor 0.002"
                   : synth == "speech" ? "mix: synthetic quiet speech-like material, peak 0.03 + synthetic noise floor 0.002"
                                       : "mix: synthetic music (3 s on / 3 s off) + synthetic noise floor 0.02";
        logf("audio: TEST synthetic source \"%s\" (no device captured, nothing played): %s", synth.c_str(), curName_.c_str());
    }
    dirty_ = synth.empty();
    bool mixing = false;      // frames are being sent (something plays)
    int64_t nextFrameUs = 0;  // wall-clock slot of the next frame
    double rtpTs = 0;         // RTP time of the next frame, in samples
    int64_t nextWin = nowUs() + kWindowUs;
    float mixWin = 0, mixWins[4] = {0, 0, 0, 0};
    int mixWinIdx = 0;
    const bool agcOn = agc_.load(), compOn = comp_.load();
    LevelChain level(agcOn);  // 0.8.3.2
    agcGain_ = 1;
    char agcText[256];
    snprintf(agcText, sizeof(agcText),
             "on (0.8.3.2: towards %.0f dBFS RMS, %+.0f..%+.0f dB, learns from frames above %.0f dBFS RMS, never from a steady noise floor; "
             "noise gate: the pauses stay at x1)",
             LevelChain::kTargetDb, LevelChain::kMinGainDb, LevelChain::kMaxGainDb, LevelChain::kGateDb);
    logf("audio: level: AGC %s, limiter -1 dBFS, volume compensation %s", agcOn ? agcText : "off",
         compOn ? "on (software-volume devices, up to +18 dB)" : "off");
    while (!stop_ && en) {
        if (dirty_.exchange(false)) {
            if (notify && notify->defaultChanged.exchange(false)) logf("audio: the Windows default playback device changed");
            rebuild();
        }
        if (dirty_mute_.exchange(false)) applyMute();
        Sleep(5);
        int64_t now = nowUs();
        if (!synth.empty()) {
            const int64_t due = (now - synthT0) * kRate / 1000000;
            for (size_t i = 0; i < 2; ++i) {
                Fifo& f = fifos[i];
                for (; synthPos[i] < due; ++synthPos[i]) {
                    const double t = double(synthPos[i]) / kRate;
                    float x = 0;
                    if (synth == "loud") x = float(0.9 * std::sin(2 * 3.14159265358979 * (i ? 660 : 440) * t));
                    else if (!synthLoop.empty())  // 0.8.3.2: quiet music / speech (peak 0.03) over a -59 dBFS RMS noise floor
                        x = i == 0 ? 0.03f * synthLoop[size_t(synthPos[i]) % synthLoop.size()]
                                   : (noise = noise * 1664525u + 1013904223u, 0.002f * (float(noise >> 8) / 8388608.f - 1.f));
                    else if (i == 0) x = std::fmod(t, 6.0) < 3.0 ? float(0.5 * std::sin(2 * 3.14159265358979 * 440 * t)) : 0.f;
                    else noise = noise * 1664525u + 1013904223u, x = 0.02f * (float(noise >> 8) / 8388608.f - 1.f);
                    f.s.push_back(x), f.s.push_back(x);
                }
                f.lastDataUs = now;
                if (!f.firstUs) f.firstUs = now;
                if (f.frames() > kMax) f.head += f.frames() - kKeep;
                f.compact();
            }
        }
        for (size_t i = 0; i < caps.size(); ++i) {
            Cap& c = *caps[i];
            if (c.failed) continue;
            Fifo& f = fifos[i];
            const size_t before = f.s.size();
            int64_t firstUs = 0;
            c.drain(&f.s, firstUs);
            if (c.failed) {
                logf("audio: %s went away, rebuilding", c.name.c_str());
                dirty_ = true;
                continue;
            }
            if (f.s.size() > before) {
                if (!f.frames() || now - f.lastDataUs > 200000) f.firstUs = firstUs ? firstUs : now;
                f.lastDataUs = now;
                if (auto m = muted.find(c.id); m != muted.end() && m->second.checkUntil) ++m->second.packets, m->second.peak = std::max(m->second.peak, c.drainPeak);
            }
            if (f.frames() > kMax) {  // this device runs ahead of the wall clock: keep 60 ms
                const size_t drop = f.frames() - kKeep;
                f.head += drop, f.dropped += int64_t(drop);
            }
            f.compact();
        }
        // The mute check: a device whose loopback went all-silent after the mute
        // (drivers that mute before the loopback tap) gets its sound back.
        for (auto it = muted.begin(); it != muted.end();) {
            Muted& m = it->second;
            if (m.checkUntil && now > m.checkUntil) {
                m.checkUntil = 0;
                if (m.packets > 50 && m.peak == 0 && m.vol) {
                    m.vol->SetMute(FALSE, nullptr);
                    logf("audio: %s went silent in the loopback when muted: its mute is undone (this device mutes before the capture)", m.name.c_str());
                    it = muted.erase(it);
                    continue;
                }
                logf("audio: %s muted and still captured (loopback peak %.4f in 3 s)", m.name.c_str(), m.peak);
            }
            ++it;
        }
        // Start when a device has 30 ms waiting; stop when all are quiet for 200 ms.
        bool any = false, primed = false;
        for (auto& f : fifos) {
            if (now - f.lastDataUs < 200000) any = true;
            if (f.frames() >= kPrime) primed = true;
        }
        if (!mixing && primed) {
            mixing = true;
            nextFrameUs = now;
            rtpTs = double(now - t0) * kRate / 1e6;  // after a pause the RTP time jumps to the wall clock
        } else if (mixing && !any) {
            mixing = false;
            for (auto& f : fifos) f.head = f.s.size() / 2, f.compact();
        }
        while (mixing && now >= nextFrameUs) {
            std::fill(mixed.begin(), mixed.end(), 0.f);
            int64_t capUs = now;
            for (size_t i = 0; i < fifos.size(); ++i) {
                Fifo& f = fifos[i];
                const size_t have = std::min<size_t>(f.frames(), kFrame);
                if (!have) continue;
                if (have < kFrame && now - f.lastDataUs > 100000) {  // a stale tail of a device that stopped
                    f.head += have;
                    continue;
                }
                const float* p = &f.s[f.head * 2];
                const float cg = i < caps.size() ? caps[i]->comp : 1.f;  // 0.8.3: the device's volume compensation
                for (size_t k = 0; k < have * 2; ++k) mixed[k] += p[k] * cg;
                capUs = std::min(capUs, f.firstUs);
                f.head += have;
                f.firstUs += int64_t(have) * 1000000 / kRate;
            }
            // 0.8.3.2: AGC (towards -17 dBFS RMS, -6..+30 dB), noise gate and limiter: level.h.
            level.process(mixed.data(), mixed.size());
            if (agcOn) agcGain_ = level.gain();
            float peak = 0;
            double sentSq = 0;
            for (float x : mixed) sentSq += double(x) * x;
            sentSqPico_ += int64_t(sentSq / double(mixed.size()) * 1e12), ++sentFrames_;  // mean square x 1e12
            for (float x : mixed) peak = std::max(peak, std::fabs(x));
            mixWin = std::max(mixWin, peak);
            const int pm = int(std::min(1.f, peak) * 1000000);
            for (int p = peakMicro_.load(); pm > p && !peakMicro_.compare_exchange_weak(p, pm);) {
            }
            const opus_int32 len = opus_encode_float(enc, mixed.data(), kFrame, packet.data(), opus_int32(packet.size()));
            if (len > 0 && out_) {
                packets_++;
                latSumUs_ += nowUs() - capUs;
                out_(std::vector<uint8_t>(packet.begin(), packet.begin() + len), rtpTs / kRate, capUs);
            }
            rtpTs += kFrame;
            nextFrameUs += 20000;
            if (nextFrameUs < now - 100000) nextFrameUs = now;  // resync after a stall
        }
        if (now < nextWin) continue;
        nextWin = now + kWindowUs;
        // 0.8.3: each software-volume device's compensation follows its volume slider.
        float compMax = 1.f;
        for (auto& cp : caps) {
            Cap& c = *cp;
            float db = 0.f;
            const bool sw = compOn && c.vol && !c.hwVolume && SUCCEEDED(c.vol->GetMasterVolumeLevel(&db));
            const float want = sw ? std::clamp(std::pow(10.f, -db / 20.f), 1.f, 7.94f) : 1.f;  // at most +18 dB
            if (std::fabs(want - c.comp) > 0.01f) {
                if (std::fabs(db - c.compDb) >= 1.f || want == 1.f)
                    logf("audio: %s volume %.1f dB -> the loopback is raised x%.2f (%+.1f dB)", c.name.c_str(), db, want, 20 * std::log10(want));
                c.comp = want, c.compDb = db;
            }
            compMax = std::max(compMax, c.comp);
        }
        compGain_ = compMax;
        // Level windows (250 ms): per device (the log), and of what the phone gets.
        for (auto& cp : caps) {
            Cap& c = *cp;
            c.lastWin = c.winPeak, c.winPeak = 0;
            c.wins[c.winIdx++ & 3] = c.lastWin;
            c.statPeak = std::max(c.statPeak, c.lastWin);
        }
        mixWins[mixWinIdx++ & 3] = mixWin, mixWin = 0;
        recentPeak_ = std::max({mixWins[0], mixWins[1], mixWins[2], mixWins[3]});
        std::lock_guard<std::mutex> lk(mu_);
        others_.clear();
        if (statsTaken_) {
            statsTaken_ = false;
            for (auto& c : caps) c->statPeak = c->lastWin;
        }
        for (size_t i = 0; i < caps.size(); ++i) {
            char b[48];
            snprintf(b, sizeof(b), " %.4f%s", caps[i]->statPeak, fifos[i].dropped ? " (drift trimmed)" : "");
            others_ += (others_.empty() ? "" : ", ") + caps[i]->name + b;
        }
    }
    for (auto& c : caps)
        if (c) c->close();
    unmuteAll("the capture stopped");
    if (notify) {
        en->UnregisterEndpointNotificationCallback(notify);
        notify->Release();
    }
    caps.clear();
    en.Reset();
    opus_encoder_destroy(enc);
    running_ = false;
    CoUninitialize();
}

}  // namespace pm::remote
