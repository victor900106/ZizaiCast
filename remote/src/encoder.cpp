#include "encoder.h"

#include <codecapi.h>
#include <mferror.h>
#include <strmif.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "util.h"

namespace pm::remote {

namespace {

std::string friendlyName(IMFActivate* a) {
    WCHAR* s = nullptr;
    UINT32 n = 0;
    std::string r = "?";
    if (SUCCEEDED(a->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &s, &n))) {
        r = narrow(s);
        CoTaskMemFree(s);
    }
    return r;
}

void setU32(ICodecAPI* api, const GUID& g, UINT32 v, const char* what) {
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = v;
    const HRESULT hr = api->SetValue(&g, &var);
    if (FAILED(hr)) logf("encoder: %s=%u not accepted (%s)", what, v, hresultText(hr).c_str());
}

void setBool(ICodecAPI* api, const GUID& g, bool v, const char* what) {
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_BOOL;
    var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
    const HRESULT hr = api->SetValue(&g, &var);
    if (FAILED(hr)) logf("encoder: %s not accepted (%s)", what, hresultText(hr).c_str());
}

// Annex B NAL units: [start, end) of each payload (after the start code).
struct Nal {
    size_t start, end;
};
std::vector<Nal> splitNals(const std::vector<uint8_t>& b) {
    std::vector<Nal> v;
    const size_t n = b.size();
    size_t i = 0, cur = SIZE_MAX;
    while (i + 3 <= n) {
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
            if (cur != SIZE_MAX) {
                size_t e = i;
                while (e > cur && b[e - 1] == 0) --e;  // trailing zero of a 4-byte start code
                v.push_back({cur, e});
            }
            i += 3;
            cur = i;
        } else {
            ++i;
        }
    }
    if (cur != SIZE_MAX && cur < n) v.push_back({cur, n});
    return v;
}

}  // namespace

H264Encoder::~H264Encoder() { close(); }

void H264Encoder::close() {
    if (!mft_ && !act_) return;
    running_ = false;
    if (mft_) {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        if (gpu_) mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
    }
    if (async_ && mft_) {
        ComPtr<IMFShutdown> sd;
        if (SUCCEEDED(mft_.As(&sd))) sd->Shutdown();
    }
    if (act_) act_->ShutdownObject();
    cv_.notify_all();
    if (evThread_.joinable()) evThread_.join();
    events_.Reset();
    mft_.Reset();
    act_.Reset();
    dxgiMgr_.Reset();
    gpu_ = false;
    needInput_ = 0;
    t0Us_ = -1;
    frameNo_ = 0;
    sps_.clear(), pps_.clear();
    loggedSps_ = false;
}

bool H264Encoder::open(const Options& o, OutputFn fn) {
    close();
    opt_ = o;
    out_ = std::move(fn);
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO outT{MFMediaType_Video, MFVideoFormat_H264};
    for (int pass = 0; pass < 2; ++pass) {
        const bool hw = pass == 0;
        if (hw && o.kind == Kind::Software) continue;
        if (!hw && o.kind == Kind::Hardware) continue;
        const UINT32 flags = (hw ? MFT_ENUM_FLAG_HARDWARE : MFT_ENUM_FLAG_SYNCMFT) | MFT_ENUM_FLAG_SORTANDFILTER;
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        HRESULT ehr;
        if (hw && o.device) {
            // Only encoders on the capture adapter: the textures never leave that GPU.
            ComPtr<IMFAttributes> ea;
            MFCreateAttributes(&ea, 1);
            ea->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&o.adapterLuid), sizeof(LUID));
            ehr = MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &outT, ea.Get(), &acts, &count);
        } else {
            ehr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &outT, &acts, &count);
        }
        if (FAILED(ehr)) continue;
        bool ok = false;
        for (UINT32 i = 0; i < count; ++i) {
            if (!ok) {
                logf("encoder: trying %s (%s)", friendlyName(acts[i]).c_str(), hw ? "hardware" : "software");
                ok = openMft(acts[i], hw);
                if (!ok) {
                    acts[i]->ShutdownObject();
                    mft_.Reset(), events_.Reset(), act_.Reset();
                }
            }
            acts[i]->Release();
        }
        CoTaskMemFree(acts);
        if (ok) return true;
    }
    logf("encoder: no usable H.264 encoder");
    return false;
}

bool H264Encoder::openMft(IMFActivate* act, bool hw) {
    act_ = act;
    name_ = friendlyName(act);
    hw_ = hw;
    HRESULT hr = act->ActivateObject(IID_PPV_ARGS(&mft_));
    if (FAILED(hr)) return logf("encoder: activate %s", hresultText(hr).c_str()), false;
    ComPtr<IMFAttributes> attrs;
    async_ = false;
    if (SUCCEEDED(mft_->GetAttributes(&attrs))) {
        UINT32 isAsync = 0;
        if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync)) && isAsync) {
            attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            async_ = true;
        }
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }
    gpu_ = false;
    if (hw && opt_.device) {
        UINT32 aware = 0;
        if (attrs && SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &aware)) && aware) {
            if (!dxgiMgr_ && SUCCEEDED(MFCreateDXGIDeviceManager(&resetToken_, &dxgiMgr_)))
                dxgiMgr_->ResetDevice(opt_.device, resetToken_);
            const HRESULT mhr =
                dxgiMgr_ ? mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(dxgiMgr_.Get()))
                         : E_FAIL;
            gpu_ = SUCCEEDED(mhr);
            if (!gpu_) logf("encoder: SET_D3D_MANAGER failed (%s): system-memory input", hresultText(mhr).c_str());
        } else {
            logf("encoder: MFT is not D3D11 aware: system-memory input");
        }
    }
    configureCodecApi();
    if (!setTypes()) return false;
    configureCodecApi();  // some MFTs only take rate control after the types
    MFT_OUTPUT_STREAM_INFO si{};
    mft_->GetOutputStreamInfo(0, &si);
    providesSamples_ = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    outBufSize_ = std::max<DWORD>(si.cbSize, DWORD(opt_.width * opt_.height * 3 / 2));
    if (async_ && FAILED(mft_.As(&events_))) return logf("encoder: async MFT without event generator"), false;
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    running_ = true;
    if (async_) evThread_ = std::thread([this] { eventLoop(); });
    logf("encoder: %s ready (%s, %s input, %dx%d @%d fps, %d kbps, VBV %d frames, IDR every %.1f s of frames)",
         name_.c_str(), async_ ? "async" : "sync", gpu_ ? "D3D11 texture (zero-copy)" : "system-memory", opt_.width,
         opt_.height, opt_.fps, opt_.kbps, opt_.vbvFrames, opt_.idrSeconds);
    if (opt_.vbr) logf("encoder: rate control = peak-constrained VBR (max %d kbps)", opt_.kbps * 3 / 2);
    return true;
}

void H264Encoder::configureCodecApi() {
    ComPtr<ICodecAPI> api;
    if (FAILED(mft_.As(&api))) return;
    setBool(api.Get(), CODECAPI_AVLowLatencyMode, true, "AVLowLatencyMode");
    if (opt_.vbr) {  // peak-constrained VBR: quiet pictures cost (almost) nothing
        setU32(api.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_PeakConstrainedVBR,
               "RateControlMode=PeakConstrainedVBR");
        setU32(api.Get(), CODECAPI_AVEncCommonMeanBitRate, UINT32(opt_.kbps) * 1000, "MeanBitRate");
        setU32(api.Get(), CODECAPI_AVEncCommonMaxBitRate, UINT32(opt_.kbps) * 1500, "MaxBitRate");
    } else {
        setU32(api.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR, "RateControlMode=CBR");
        setU32(api.Get(), CODECAPI_AVEncCommonMeanBitRate, UINT32(opt_.kbps) * 1000, "MeanBitRate");
    }
    setU32(api.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0, "BPictureCount");
    setU32(api.Get(), CODECAPI_AVEncMPVGOPSize, UINT32(std::max(1.0, opt_.fps * opt_.idrSeconds)), "GOPSize");
    setU32(api.Get(), CODECAPI_AVEncCommonQualityVsSpeed, 20, "QualityVsSpeed");
    if (opt_.vbvFrames > 0)
        setU32(api.Get(), CODECAPI_AVEncCommonBufferSize,
               UINT32(int64_t(opt_.kbps) * 1000 * opt_.vbvFrames / std::max(1, opt_.fps)), "BufferSize(VBV)");
}

bool H264Encoder::setTypes() {
    const UINT32 profiles[] = {UINT32(eAVEncH264VProfile_ConstrainedBase), UINT32(eAVEncH264VProfile_Base)};
    HRESULT hr = E_FAIL;
    for (UINT32 prof : profiles) {
        ComPtr<IMFMediaType> t;
        MFCreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        t->SetUINT32(MF_MT_AVG_BITRATE, UINT32(opt_.kbps) * 1000);
        t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        t->SetUINT32(MF_MT_MPEG2_PROFILE, prof);
        t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        t->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        t->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, UINT32(opt_.width), UINT32(opt_.height));
        MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, UINT32(opt_.fps), 1);
        MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = mft_->SetOutputType(0, t.Get(), 0);
        if (SUCCEEDED(hr)) {
            logf("encoder: output profile %s", prof == UINT32(eAVEncH264VProfile_Base) ? "Baseline" : "Constrained Baseline");
            break;
        }
    }
    if (FAILED(hr)) return logf("encoder: SetOutputType %s", hresultText(hr).c_str()), false;
    ComPtr<IMFMediaType> t;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_DEFAULT_STRIDE, UINT32(opt_.width));
    t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    t->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    t->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, UINT32(opt_.width), UINT32(opt_.height));
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, UINT32(opt_.fps), 1);
    MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = mft_->SetInputType(0, t.Get(), 0);
    if (FAILED(hr)) return logf("encoder: SetInputType %s", hresultText(hr).c_str()), false;
    return true;
}

bool H264Encoder::setBitrate(int kbps) {
    if (!mft_ || kbps <= 0) return false;
    ComPtr<ICodecAPI> api;
    if (FAILED(mft_.As(&api))) return false;
    std::lock_guard<std::mutex> lk(mu_);
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = UINT32(kbps) * 1000;
    const HRESULT hr = api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);
    logf("encoder: bitrate -> %d kbps (%s)", kbps, SUCCEEDED(hr) ? "ok" : hresultText(hr).c_str());
    if (SUCCEEDED(hr)) opt_.kbps = kbps;
    return SUCCEEDED(hr);
}

bool H264Encoder::encode(const uint8_t* nv12, size_t size, int64_t captureUs) {
    if (!mft_ || !running_) return false;
    if (t0Us_ < 0) t0Us_ = captureUs;
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(MFCreateMemoryBuffer(DWORD(size), &buf))) return false;
    BYTE* p = nullptr;
    buf->Lock(&p, nullptr, nullptr);
    memcpy(p, nv12, size);
    buf->Unlock();
    buf->SetCurrentLength(DWORD(size));
    ComPtr<IMFSample> s;
    MFCreateSample(&s);
    s->AddBuffer(buf.Get());
    return submit(s.Get(), captureUs);
}

namespace {
// IMFTrackedSample allocator callback: runs when the encoder lets go of the
// sample (its texture can be reused).
class ReleaseCallback final : public IMFAsyncCallback {
public:
    explicit ReleaseCallback(std::function<void()> fn) : fn_(std::move(fn)) {}
    STDMETHODIMP QueryInterface(REFIID iid, void** pv) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFAsyncCallback)) {
            *pv = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        *pv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ULONG(++ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        const long r = --ref_;
        if (r == 0) delete this;
        return ULONG(r);
    }
    STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult*) override {
        if (fn_) fn_(), fn_ = nullptr;
        return S_OK;
    }
private:
    std::atomic<long> ref_{1};
    std::function<void()> fn_;
};
}  // namespace

bool H264Encoder::encodeTexture(ID3D11Texture2D* tex, int64_t captureUs, std::function<void()> onRelease) {
    if (!mft_ || !running_ || !gpu_) {
        if (onRelease) onRelease();
        return false;
    }
    if (t0Us_ < 0) t0Us_ = captureUs;
    ComPtr<IMFMediaBuffer> buf;
    HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), tex, 0, FALSE, &buf);
    if (FAILED(hr)) {
        if (onRelease) onRelease();
        return logf("encoder: MFCreateDXGISurfaceBuffer %s", hresultText(hr).c_str()), false;
    }
    ComPtr<IMF2DBuffer> b2;
    DWORD len = 0;
    if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->GetContiguousLength(&len))) buf->SetCurrentLength(len);
    ComPtr<IMFTrackedSample> tracked;
    if (FAILED(MFCreateTrackedSample(&tracked))) {
        if (onRelease) onRelease();
        return false;
    }
    ComPtr<IMFSample> s;
    tracked.As(&s);
    s->AddBuffer(buf.Get());
    auto* cb = new ReleaseCallback(std::move(onRelease));
    tracked->SetAllocator(cb, nullptr);
    cb->Release();  // the sample holds it now
    return submit(s.Get(), captureUs);
}

bool H264Encoder::submit(IMFSample* s, int64_t captureUs) {
    s->SetSampleTime((captureUs - t0Us_) * 10);
    s->SetSampleDuration(10000000 / std::max(1, opt_.fps));
    if (wantKey_.exchange(false)) {
        ComPtr<ICodecAPI> api;
        if (SUCCEEDED(mft_.As(&api))) setU32(api.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1, "ForceKeyFrame");
    }
    ++frameNo_;
    if (async_) {
        std::unique_lock<std::mutex> lk(mu_);
        if (!cv_.wait_for(lk, std::chrono::milliseconds(100), [this] { return needInput_ > 0 || !running_; }) ||
            !running_) {
            ++dropped_;
            return false;
        }
        --needInput_;
        const HRESULT hr = mft_->ProcessInput(0, s, 0);
        if (FAILED(hr)) {
            logf("encoder: ProcessInput %s", hresultText(hr).c_str());
            ++dropped_;
            return false;
        }
        return true;
    }
    HRESULT hr = mft_->ProcessInput(0, s, 0);
    if (hr == MF_E_NOTACCEPTING) {
        drainOutput();
        hr = mft_->ProcessInput(0, s, 0);
    }
    if (FAILED(hr)) {
        logf("encoder: ProcessInput %s", hresultText(hr).c_str());
        ++dropped_;
        return false;
    }
    drainOutput();
    return true;
}

void H264Encoder::eventLoop() {
    while (running_) {
        ComPtr<IMFMediaEvent> ev;
        const HRESULT hr = events_->GetEvent(0, &ev);
        if (FAILED(hr)) {
            if (running_) logf("encoder: GetEvent %s", hresultText(hr).c_str());
            break;
        }
        MediaEventType type = MEUnknown;
        ev->GetType(&type);
        if (type == METransformNeedInput) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                ++needInput_;
            }
            cv_.notify_all();
        } else if (type == METransformHaveOutput) {
            std::lock_guard<std::mutex> lk(mu_);
            drainOutput();
        }
    }
    running_ = false;
    cv_.notify_all();
}

// Sync: until NEED_MORE_INPUT. Async: one ProcessOutput per HaveOutput event.
void H264Encoder::drainOutput() {
    for (int guard = 0; guard < 8; ++guard) {
        MFT_OUTPUT_DATA_BUFFER odb{};
        ComPtr<IMFSample> own;
        if (!providesSamples_) {
            ComPtr<IMFMediaBuffer> b;
            MFCreateMemoryBuffer(outBufSize_, &b);
            MFCreateSample(&own);
            own->AddBuffer(b.Get());
            odb.pSample = own.Get();
        }
        DWORD status = 0;
        const HRESULT hr = mft_->ProcessOutput(0, 1, &odb, &status);
        if (odb.pEvents) odb.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> t;
            if (SUCCEEDED(mft_->GetOutputAvailableType(0, 0, &t))) mft_->SetOutputType(0, t.Get(), 0);
            logf("encoder: output stream change");
            continue;
        }
        if (FAILED(hr)) {
            logf("encoder: ProcessOutput %s", hresultText(hr).c_str());
            return;
        }
        if (odb.pSample) emit(odb.pSample);
        if (providesSamples_ && odb.pSample) odb.pSample->Release();
        if (async_) return;
    }
}

void H264Encoder::emit(IMFSample* s) {
    LONGLONG t = 0;
    s->GetSampleTime(&t);
    ComPtr<IMFMediaBuffer> b;
    if (FAILED(s->ConvertToContiguousBuffer(&b))) return;
    BYTE* p = nullptr;
    DWORD len = 0;
    if (FAILED(b->Lock(&p, nullptr, &len))) return;
    std::vector<uint8_t> au(p, p + len);
    b->Unlock();
    bool idr = false;
    fixup(au, idr);
    if (au.empty()) return;
    if (out_) out_(std::move(au), t0Us_ + t / 10, idr);
}

// Drops AUDs, makes a Baseline SPS say Constrained Baseline (constraint_set0+1:
// the MF encoders use no FMO/ASO/redundant slices), remembers SPS/PPS and
// puts them in front of IDRs that come without them.
void H264Encoder::fixup(std::vector<uint8_t>& au, bool& idr) {
    const auto nals = splitNals(au);
    bool hasSps = false;
    idr = false;
    std::vector<uint8_t> out;
    out.reserve(au.size() + sps_.size() + pps_.size() + 8);
    static const uint8_t sc[4] = {0, 0, 0, 1};
    std::vector<Nal> keep;
    for (const Nal& n : nals) {
        if (n.end <= n.start) continue;
        const int type = au[n.start] & 0x1F;
        if (type == 9) continue;  // AUD
        if (type == 12) {          // filler data (CBR padding): pure waste on the wire
            fillerBytes_ += int64_t(n.end - n.start + 4);
            continue;
        }
        if (type == 7) {
            hasSps = true;
            if (n.end - n.start > 3 && au[n.start + 1] == 66) au[n.start + 2] |= 0xC0;
            sps_.assign(au.begin() + long(n.start), au.begin() + long(n.end));
            if (!loggedSps_) {
                loggedSps_ = true;
                logf("encoder: SPS profile_idc=%d constraints=0x%02X level_idc=%d", au[n.start + 1],
                     au[n.start + 2], n.end - n.start > 3 ? au[n.start + 3] : 0);
            }
        }
        if (type == 8) pps_.assign(au.begin() + long(n.start), au.begin() + long(n.end));
        if (type == 5) idr = true;
        keep.push_back(n);
    }
    if (idr && !hasSps && !sps_.empty() && !pps_.empty()) {
        out.insert(out.end(), sc, sc + 4), out.insert(out.end(), sps_.begin(), sps_.end());
        out.insert(out.end(), sc, sc + 4), out.insert(out.end(), pps_.begin(), pps_.end());
    }
    for (const Nal& n : keep) {
        out.insert(out.end(), sc, sc + 4);
        out.insert(out.end(), au.begin() + long(n.start), au.begin() + long(n.end));
    }
    au.swap(out);
}

}  // namespace pm::remote
