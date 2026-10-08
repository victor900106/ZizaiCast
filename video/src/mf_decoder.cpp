#include "mf_decoder.h"

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>

#include <cstring>

#include "log.h"

namespace pm::video {

namespace {

std::string narrow(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

const char* codecName(VideoCodec c) { return c == VideoCodec::H264 ? "H.264" : "HEVC"; }

}  // namespace

MfDecoder::~MfDecoder() { close(); }

void MfDecoder::close() {
    if (mft_) {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
    }
    // Note: the HEVC Video Extensions MFT still leaks one semaphore handle per
    // instance (measured: pm_video_test --decoder-cycles); decoders are
    // therefore kept across reconnects and only re-created on a codec change
    // or device loss.
    if (activate_) activate_->ShutdownObject();
    mft_.Reset();
    activate_.Reset();
    swSample_.Reset();
    hw_ = false;
    fmt_ = {};
}

bool MfDecoder::open(VideoCodec codec, IMFDXGIDeviceManager* dxgiManager) {
    close();
    codec_ = codec;
    const GUID subtype = codec == VideoCodec::H264 ? MFVideoFormat_H264 : MFVideoFormat_HEVC;

    // Enumerate synchronous decoder MFTs (hardware vendor MFTs are async-only
    // and need a different driving model; the Microsoft MFTs use DXVA
    // themselves when given a D3D11 device manager).
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, subtype};
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
              MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in,
              nullptr, &acts, &count);
    for (UINT32 i = 0; i < count; ++i) {
        if (!mft_) {
            ComPtr<IMFTransform> t;
            if (SUCCEEDED(acts[i]->ActivateObject(IID_PPV_ARGS(&t)))) {
                mft_ = t;
                activate_ = acts[i];
                wchar_t* fn = nullptr;
                UINT32 fl = 0;
                if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &fn, &fl))) {
                    name_ = narrow(fn);
                    CoTaskMemFree(fn);
                }
            }
        }
        acts[i]->Release();
    }
    CoTaskMemFree(acts);
    if (!mft_ && codec == VideoCodec::H264) {
        if (SUCCEEDED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&mft_))))
            name_ = "Microsoft H264 Video Decoder MFT";
    }
    if (!mft_) {
        log("no %s decoder MFT installed%s", codecName(codec),
            codec == VideoCodec::H265
                ? " - install 'HEVC Video Extensions' from the Microsoft Store to view HEVC streams"
                : "");
        return false;
    }

    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(mft_->GetAttributes(&attrs)) && attrs) {
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        if (dxgiManager && MFGetAttributeUINT32(attrs.Get(), MF_SA_D3D11_AWARE, FALSE)) {
            HRESULT hr = mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                              reinterpret_cast<ULONG_PTR>(dxgiManager));
            hw_ = SUCCEEDED(hr);
            if (!hw_) log("decoder refused D3D11 device manager (hr=0x%08lx), using software", hr);
        }
    }
    ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(mft_.As(&codecApi))) {
        VARIANT v{};
        v.vt = VT_UI4;
        v.ulVal = 1;
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &v);
    }

    ComPtr<IMFMediaType> inType;
    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, subtype);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    // Placeholder size/rate: the HEVC extension MFT offers no output types
    // without them.  The real values arrive in-band (SPS) and surface as
    // MF_E_TRANSFORM_STREAM_CHANGE, handled in drain().
    MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, 1920, 1080);
    MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, 60, 1);
    MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    HRESULT hr = mft_->SetInputType(0, inType.Get(), 0);
    const char* stage = "SetInputType";
    if (SUCCEEDED(hr)) {
        stage = "SetOutputType(NV12)";
        hr = setOutputType();
    }
    if (FAILED(hr)) {
        log("%s decoder '%s' rejected media types at %s (hr=0x%08lx, %s)", codecName(codec), name_.c_str(),
            stage, hr, hw_ ? "hw" : "sw");
        close();
        return false;
    }
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    log("%s decoder: %s (%s)", codecName(codec), name_.c_str(),
        hw_ ? "hardware DXVA/D3D11" : "software");
    return true;
}

HRESULT MfDecoder::setOutputType() {
    // NV12 preferred (10-bit streams are dithered to 8 bits by the MFT if it
    // offers NV12); P010 if that is all a Main10 stream offers.
    ComPtr<IMFMediaType> type, p010;
    HRESULT hr = E_FAIL;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> t;
        if (FAILED(mft_->GetOutputAvailableType(0, i, &t))) break;
        GUID st{};
        t->GetGUID(MF_MT_SUBTYPE, &st);
        if (st == MFVideoFormat_NV12) {
            type = t;
            break;
        }
        if (st == MFVideoFormat_P010 && !p010) p010 = t;
    }
    if (!type) type = p010;
    if (!type) return MF_E_INVALIDMEDIATYPE;
    hr = mft_->SetOutputType(0, type.Get(), 0);
    if (FAILED(hr)) return hr;

    VideoFormat f;
    f.tenBit = type == p010;
    MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &f.codedWidth, &f.codedHeight);
    f.crop = {0, 0, static_cast<LONG>(f.codedWidth), static_cast<LONG>(f.codedHeight)};
    MFVideoArea area{};
    UINT32 sz = 0;
    if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area),
                                sizeof(area), &sz)) ||
        SUCCEEDED(type->GetBlob(MF_MT_GEOMETRIC_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area),
                                &sz))) {
        if (area.Area.cx > 0 && area.Area.cy > 0) {
            f.crop.left = area.OffsetX.value;
            f.crop.top = area.OffsetY.value;
            f.crop.right = f.crop.left + area.Area.cx;
            f.crop.bottom = f.crop.top + area.Area.cy;
        }
    }
    UINT32 matrix = MFGetAttributeUINT32(type.Get(), MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    f.bt601 = matrix == MFVideoTransferMatrix_BT601 || matrix == MFVideoTransferMatrix_SMPTE240M;
    f.fullRange =
        MFGetAttributeUINT32(type.Get(), MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235) ==
        MFNominalRange_0_255;
    f.stride = static_cast<LONG>(MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, f.codedWidth));

    MFT_OUTPUT_STREAM_INFO si{};
    mft_->GetOutputStreamInfo(0, &si);
    providesSamples_ =
        (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    if (si.cbSize != outBufSize_) swSample_.Reset();
    outBufSize_ = si.cbSize;

    if (f.codedWidth && (f.codedWidth != fmt_.codedWidth || f.codedHeight != fmt_.codedHeight ||
                         !EqualRect(&f.crop, &fmt_.crop)))
        log("output format: %s %ux%u visible %dx%d @(%ld,%ld) %s %s-range", f.tenBit ? "P010" : "NV12", f.codedWidth, f.codedHeight,
            f.visibleWidth(), f.visibleHeight(), f.crop.left, f.crop.top, f.bt601 ? "BT.601" : "BT.709",
            f.fullRange ? "full" : "video");
    fmt_ = f;
    return S_OK;
}

HRESULT MfDecoder::decode(const uint8_t* au, size_t len, int64_t time100ns, const OutputFn& out) {
    if (!mft_) return E_UNEXPECTED;
    ComPtr<IMFMediaBuffer> buf;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(len), &buf);
    if (FAILED(hr)) return hr;
    BYTE* dst = nullptr;
    buf->Lock(&dst, nullptr, nullptr);
    std::memcpy(dst, au, len);
    buf->Unlock();
    buf->SetCurrentLength(static_cast<DWORD>(len));
    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(buf.Get());
    sample->SetSampleTime(time100ns);
    sample->SetSampleDuration(166667);

    hr = mft_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        hr = drain(out);
        if (FAILED(hr)) return hr;
        hr = mft_->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) return hr;
    return drain(out);
}

HRESULT MfDecoder::drain(const OutputFn& out) {
    for (int guard = 0; guard < 64; ++guard) {
        MFT_OUTPUT_DATA_BUFFER ob{};
        ob.dwStreamID = 0;
        if (!providesSamples_) {
            if (!swSample_) {
                ComPtr<IMFMediaBuffer> b;
                HRESULT hr = MFCreateAlignedMemoryBuffer(outBufSize_, MF_64_BYTE_ALIGNMENT, &b);
                if (FAILED(hr)) return hr;
                MFCreateSample(&swSample_);
                swSample_->AddBuffer(b.Get());
            }
            ob.pSample = swSample_.Get();
        }
        DWORD status = 0;
        HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) ob.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (providesSamples_ && ob.pSample) ob.pSample->Release();
            hr = setOutputType();
            if (FAILED(hr)) return hr;
            continue;
        }
        if (FAILED(hr)) {
            if (providesSamples_ && ob.pSample) ob.pSample->Release();
            return hr;
        }
        if (ob.pSample) {
            out(ob.pSample, fmt_);
            if (providesSamples_) ob.pSample->Release();
            else swSample_.Reset();
        }
    }
    return S_OK;
}

void MfDecoder::flush() {
    if (!mft_) return;
    mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
}

}  // namespace pm::video
