#include "ui_art.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "log.h"
#include "toutou_png.h"

namespace pm::video {

namespace {

struct Embedded {
    const unsigned char* data;
    size_t size;
};
// Same order as ToutouArt::Layer.
const Embedded kEmbedded[ToutouArt::kLayers] = {
    {kToutouCloud, sizeof(kToutouCloud)},           {kToutouRim, sizeof(kToutouRim)},
    {kToutouFaceIdle, sizeof(kToutouFaceIdle)},     {kToutouFaceBlink, sizeof(kToutouFaceBlink)},
    {kToutouFaceHappy, sizeof(kToutouFaceHappy)},   {kToutouFaceConnecting, sizeof(kToutouFaceConnecting)},
    {kToutouFaceSurprised, sizeof(kToutouFaceSurprised)}, {kToutouFaceSleepy, sizeof(kToutouFaceSleepy)},
    {kToutouFaceAsleep, sizeof(kToutouFaceAsleep)}, {kToutouHandHold, sizeof(kToutouHandHold)},
    {kToutouHandSleepy, sizeof(kToutouHandSleepy)}, {kToutouHandAsleep, sizeof(kToutouHandAsleep)},
};

}  // namespace

void ToutouArt::reset() {
    for (auto& b : cache_) b.Reset();
    for (auto& b : src_) b.Reset();
    wic_.Reset();
    cacheScale_ = 0;
    loaded_ = false;
}

bool ToutouArt::load() {
    if (loaded_) return true;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_)))) {
        log("WIC unavailable: idle screen without mascot");
        return false;
    }
    using namespace toutou;
    // Hit-test mask over the canvas, 96 cells wide.
    maskW_ = 96;
    maskH_ = static_cast<UINT>(std::lround(96.0 * kCanvas.h / kCanvas.w));
    mask_.assign(static_cast<size_t>(maskW_) * maskH_, 0);
    for (int l = 0; l < kLayers; ++l) {
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> dec;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> conv;
        UINT w = 0, h = 0;
        HRESULT hr = wic_->CreateStream(&stream);
        if (SUCCEEDED(hr))
            hr = stream->InitializeFromMemory(const_cast<BYTE*>(kEmbedded[l].data), static_cast<DWORD>(kEmbedded[l].size));
        if (SUCCEEDED(hr)) hr = wic_->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec);
        if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &frame);
        if (SUCCEEDED(hr)) hr = wic_->CreateFormatConverter(&conv);
        if (SUCCEEDED(hr))
            hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                  WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(hr)) hr = conv->GetSize(&w, &h);
        std::vector<uint8_t> px;
        if (SUCCEEDED(hr)) {
            px.resize(static_cast<size_t>(w) * h * 4);
            hr = conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data());
        }
        if (FAILED(hr) || !w || !h) {
            log("toutou layer %d decode failed hr=0x%08lx", l, hr);
            reset();
            return false;
        }
        // Opaque bounding box (layers are mostly transparent: faces, hands).
        UINT x0 = w, y0 = h, x1 = 0, y1 = 0;
        for (UINT y = 0; y < h; ++y) {
            const uint8_t* row = px.data() + static_cast<size_t>(y) * w * 4;
            for (UINT x = 0; x < w; ++x)
                if (row[x * 4 + 3]) {
                    x0 = std::min(x0, x);
                    x1 = std::max(x1, x + 1);
                    y0 = std::min(y0, y);
                    y1 = std::max(y1, y + 1);
                }
        }
        if (x1 <= x0 || y1 <= y0) x0 = y0 = 0, x1 = y1 = 1;
        const float k = kCanvas.w / w;  // frame units per pixel
        place_[l] = {kCanvas.x + x0 * k, kCanvas.y + y0 * k, (x1 - x0) * k, (y1 - y0) * k};
        // Hit mask: the cloud and the hand at rest.
        if (l == Cloud || l == HandHold)
            for (UINT my = 0; my < maskH_; ++my)
                for (UINT mx = 0; mx < maskW_; ++mx) {
                    const UINT x = std::min(w - 1, (2 * mx + 1) * w / (2 * maskW_));
                    const UINT y = std::min(h - 1, (2 * my + 1) * h / (2 * maskH_));
                    if (px[(static_cast<size_t>(y) * w + x) * 4 + 3] >= 128) mask_[static_cast<size_t>(my) * maskW_ + mx] = 1;
                }
        hr = wic_->CreateBitmapFromMemory(x1 - x0, y1 - y0, GUID_WICPixelFormat32bppPBGRA, w * 4,
                                          static_cast<UINT>(w * 4 * (y1 - y0 - 1) + (x1 - x0) * 4),
                                          px.data() + (static_cast<size_t>(y0) * w + x0) * 4, &src_[l]);
        if (FAILED(hr)) {
            reset();
            return false;
        }
    }
    // The phone (drawn procedurally) at rest, in the hold pose, tilted with the art.
    {
        const Phone& p = kPhone[kHold];
        const float a = (p.rot + kTilt) * 3.14159265f / 180.f, ca = std::cos(a), sa = std::sin(a);
        const float t = kTilt * 3.14159265f / 180.f;
        const float cx = kPivotX + (p.x - kPivotX) * std::cos(t) - (p.y - kPivotY) * std::sin(t);
        const float cy = kPivotY + (p.x - kPivotX) * std::sin(t) + (p.y - kPivotY) * std::cos(t);
        for (UINT my = 0; my < maskH_; ++my)
            for (UINT mx = 0; mx < maskW_; ++mx) {
                const float fx = kCanvas.x + (mx + 0.5f) * kCanvas.w / maskW_ - cx;
                const float fy = kCanvas.y + (my + 0.5f) * kCanvas.h / maskH_ - cy;
                const float u = fx * ca + fy * sa, v = -fx * sa + fy * ca;  // phone-local
                if (std::abs(u) <= p.w / 2 && std::abs(v) <= p.h / 2) mask_[static_cast<size_t>(my) * maskW_ + mx] = 1;
            }
    }
    loaded_ = true;
    return true;
}

float ToutouArt::quantize(float pxPerUnit) const {
    // 1/8 px-per-unit steps (~3-6 %), never above the source resolution.
    const float q = std::ceil(pxPerUnit * 8.f) / 8.f;
    return std::clamp(q, 0.25f, toutou::kLayerScale);
}

IWICBitmap* ToutouArt::scaled(Layer l, float pxPerUnit) {
    if (!loaded_ || l < 0 || l >= kLayers) return nullptr;
    const float q = quantize(pxPerUnit);
    if (q >= toutou::kLayerScale) return src_[l].Get();
    if (q != cacheScale_) {
        for (auto& b : cache_) b.Reset();
        cacheScale_ = q;
    }
    if (cache_[l]) return cache_[l].Get();
    UINT sw = 0, sh = 0;
    src_[l]->GetSize(&sw, &sh);
    const float k = q / toutou::kLayerScale;
    const UINT w = std::max(1u, static_cast<UINT>(std::lround(sw * k)));
    const UINT h = std::max(1u, static_cast<UINT>(std::lround(sh * k)));
    ComPtr<IWICBitmapScaler> sc;
    ComPtr<IWICBitmap> out;
    if (FAILED(wic_->CreateBitmapScaler(&sc)) ||
        FAILED(sc->Initialize(src_[l].Get(), w, h, WICBitmapInterpolationModeHighQualityCubic)) ||
        FAILED(wic_->CreateBitmapFromSource(sc.Get(), WICBitmapCacheOnLoad, &out)))
        return src_[l].Get();
    cache_[l] = out;
    return out.Get();
}

bool writePng(const std::wstring& path, const uint8_t* bgrx, UINT w, UINT h, bool alpha) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    {
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> frame;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        if (SUCCEEDED(hr)) hr = wic->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
        if (SUCCEEDED(hr)) hr = enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, nullptr);
        if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
        if (SUCCEEDED(hr)) hr = frame->SetSize(w, h);
        const WICPixelFormatGUID want = alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
        WICPixelFormatGUID fmt = want;
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
        if (SUCCEEDED(hr) && fmt != want) hr = E_FAIL;
        if (SUCCEEDED(hr) && alpha) {
            hr = frame->WritePixels(h, w * 4, w * h * 4, const_cast<BYTE*>(bgrx));
        } else if (SUCCEEDED(hr)) {
            const UINT stride = w * 3;
            std::vector<uint8_t> rgb(static_cast<size_t>(stride) * h);
            for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
                rgb[i * 3 + 0] = bgrx[i * 4 + 0];
                rgb[i * 3 + 1] = bgrx[i * 4 + 1];
                rgb[i * 3 + 2] = bgrx[i * 4 + 2];
            }
            hr = frame->WritePixels(h, stride, static_cast<UINT>(rgb.size()), rgb.data());
        }
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = enc->Commit();
        ok = SUCCEEDED(hr);
        if (!ok) log("snapshot PNG write failed hr=0x%08lx", hr);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}

}  // namespace pm::video
