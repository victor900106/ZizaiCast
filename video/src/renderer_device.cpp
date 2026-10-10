// Renderer: D3D11 device objects, shaders, swap chain, targets, resize.
// 拆檔 0.7.9：自 renderer.cpp 原樣搬出。
#include "renderer.h"

#include <d3dcompiler.h>
#include <mfapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "log.h"
#include "pm/i18n.h"
#include "renderer_internal.h"

namespace pm::video {

using namespace detail;

namespace {

const char kShader[] = R"(
cbuffer CB : register(b0) {
    float4 uvRect;   // u0 v0 u1 v1 (visible area in texture coords)
    float4 range;    // yOffset, yScale, cScale, 0
    float4 mat;      // R.cr, G.cb, G.cr, B.cb
    float4 xfU;      // rotation / mirror: texture coord = (dot(xfU.xy, t) + xfU.z,
    float4 xfV;      //                                    dot(xfV.xy, t) + xfV.z)
    float4 view;     // magnifier: t = t * view.x + view.yz; view.w = filter
};
cbuffer TapCB : register(b1) {
    int4 tapOff;     // crop offset: luma x, y; chroma x, y
};
Texture2D<float>  texY  : register(t0);
Texture2D<float2> texUV : register(t1);
Texture2D<float4> texRGB : register(t2);  // BGRA pictures (submitBgraFrame)
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID) {
    float2 t = float2(id & 1, id >> 1);
    VSOut o;
    o.pos = float4(t.x * 2 - 1, 1 - t.y * 2, 0, 1);
    t = t * view.x + view.yz;
    float2 s = float2(dot(xfU.xy, t) + xfU.z, dot(xfV.xy, t) + xfV.z);
    o.uv = lerp(uvRect.xy, uvRect.zw, s);
    return o;
}
// Frame tap: exact texel copies of the visible area (and 10 -> 8 bit via the
// UNORM render targets).
float ps_tap_y(VSOut i) : SV_Target {
    return texY.Load(int3(int2(i.pos.xy) + tapOff.xy, 0));
}
float2 ps_tap_uv(VSOut i) : SV_Target {
    return texUV.Load(int3(int2(i.pos.xy) + tapOff.zw, 0));
}
// BGRA pictures: frame tap to BT.709 video-range NV12 (what the recorder
// declares), chroma = average of the 2x2 block.
static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
float ps_tap_rgb_y(VSOut i) : SV_Target {
    return 16.0 / 255 + 219.0 / 255 * dot(texRGB.Load(int3(int2(i.pos.xy), 0)).rgb, kLuma);
}
float2 ps_tap_rgb_uv(VSOut i) : SV_Target {
    int2 p = int2(i.pos.xy) * 2;
    float3 c = (texRGB.Load(int3(p, 0)).rgb + texRGB.Load(int3(p + int2(1, 0), 0)).rgb +
                texRGB.Load(int3(p + int2(0, 1), 0)).rgb + texRGB.Load(int3(p + int2(1, 1), 0)).rgb) * 0.25;
    float y = dot(c, kLuma);
    return float2(128.0 / 255 + 224.0 / 255 * (c.b - y) / 1.8556, 128.0 / 255 + 224.0 / 255 * (c.r - y) / 1.5748);
}
// Low-vision filters (view.w): 1 contrast boost, 2 greyscale, 3 invert
// (white-on-black UI -> black on white and vice versa), 4 yellow on black
// (dark text on a light background -> yellow text on black).
float3 viewFilter(float3 c) {
    int f = (int)(view.w + 0.5);
    if (f == 0) return c;
    float l = dot(c, kLuma);
    if (f == 1) {
        float3 g = lerp(l.xxx, c, 1.35);                   // more saturation
        return saturate((g - 0.5) * 1.7 + 0.5);            // steeper contrast around mid grey
    }
    if (f == 2) return saturate((l.xxx - 0.5) * 1.25 + 0.5);
    if (f == 3) return 1 - c;
    float k = saturate((0.80 - l) / 0.55);                 // 0 for light pixels, 1 for dark (text)
    k = k * k * (3 - 2 * k);
    return k * float3(1, 0.92, 0.10);
}
float4 ps_bgra(VSOut i) : SV_Target {
    return float4(viewFilter(texRGB.Sample(smp, i.uv).rgb), 1);
}
float4 ps_main(VSOut i) : SV_Target {
    float  y = (texY.Sample(smp, i.uv) - range.x) * range.y;
    float2 c = (texUV.Sample(smp, i.uv) - 0.5) * range.z;
    float3 rgb = float3(y + mat.x * c.y, y + mat.y * c.x + mat.z * c.y, y + mat.w * c.x);
    return float4(viewFilter(saturate(rgb)), 1);
}
)";

}  // namespace

namespace {

ComPtr<ID3DBlob> compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> code, err;
    HRESULT hr = D3DCompile(kShader, sizeof(kShader) - 1, "pm_video", nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr))
        log("shader %s compile failed: %s", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    return code;
}

}  // namespace

bool Renderer::init(HWND hwnd, ID3D11Device* device) {
    hwnd_ = hwnd;
    dpi_ = GetDpiForWindow(hwnd);
    if (!dpi_) dpi_ = 96;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    width_ = std::max<LONG>(rc.right - rc.left, 1);
    height_ = std::max<LONG>(rc.bottom - rc.top, 1);

    // Device-independent objects: they survive device loss.
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
    if (d2d_) {
        D2D1_STROKE_STYLE_PROPERTIES sp{};
        sp.startCap = sp.endCap = sp.dashCap = D2D1_CAP_STYLE_ROUND;
        sp.lineJoin = D2D1_LINE_JOIN_ROUND;
        d2d_->CreateStrokeStyle(&sp, nullptr, 0, &round_);
        // Unit particle shapes centred on the origin (~1 DIP across).
        ComPtr<ID2D1GeometrySink> s;
        if (SUCCEEDED(d2d_->CreatePathGeometry(&heart_)) && SUCCEEDED(heart_->Open(&s))) {
            s->BeginFigure({0, 0.42f}, D2D1_FIGURE_BEGIN_FILLED);
            s->AddBezier({{-0.12f, 0.30f}, {-0.52f, 0.05f}, {-0.50f, -0.20f}});
            s->AddBezier({{-0.48f, -0.46f}, {-0.12f, -0.52f}, {0, -0.24f}});
            s->AddBezier({{0.12f, -0.52f}, {0.48f, -0.46f}, {0.50f, -0.20f}});
            s->AddBezier({{0.52f, 0.05f}, {0.12f, 0.30f}, {0, 0.42f}});
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
            s->Close();
        }
        s.Reset();
        if (SUCCEEDED(d2d_->CreatePathGeometry(&sparkle_)) && SUCCEEDED(sparkle_->Open(&s))) {
            s->BeginFigure({0, -0.5f}, D2D1_FIGURE_BEGIN_FILLED);
            s->AddQuadraticBezier({{0.06f, -0.06f}, {0.5f, 0}});
            s->AddQuadraticBezier({{0.06f, 0.06f}, {0, 0.5f}});
            s->AddQuadraticBezier({{-0.06f, 0.06f}, {-0.5f, 0}});
            s->AddQuadraticBezier({{-0.06f, -0.06f}, {0, -0.5f}});
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
            s->Close();
        }
        s.Reset();
        if (SUCCEEDED(d2d_->CreatePathGeometry(&playTri_)) && SUCCEEDED(playTri_->Open(&s))) {
            s->BeginFigure({-3.6f, -5.6f}, D2D1_FIGURE_BEGIN_FILLED);
            s->AddLine({5.4f, 0});
            s->AddLine({-3.6f, 5.6f});
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
            s->Close();
        }
    }
    toutou_.load();
    pal_ = kPalettes[0];
    sceneAt_ = clockMs();
    poke();
    return attachDevice(device);
}

bool Renderer::attachDevice(ID3D11Device* device) {
    dev_ = device;
    dev_->GetImmediateContext(&ctx_);
    if (createDeviceObjects()) {
        restoreFrozen();
        return true;
    }
    releaseDevice();
    return false;
}

bool Renderer::createDeviceObjects() {
    if (!createSwapChain()) return false;

    auto vsCode = compile("vs_main", "vs_4_0");
    auto psCode = compile("ps_main", "ps_4_0");
    if (!vsCode || !psCode) return false;
    if (FAILED(dev_->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs_)) ||
        FAILED(dev_->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps_)))
        return false;

    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    dev_->CreateSamplerState(&smp, &sampler_);

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Constants);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev_->CreateBuffer(&bd, nullptr, &cb_);
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    dev_->CreateQuery(&qd, &evt_);
    // Frame-tap shaders (created eagerly: tiny).
    auto tyCode = compile("ps_tap_y", "ps_4_0"), tuvCode = compile("ps_tap_uv", "ps_4_0");
    if (tyCode) dev_->CreatePixelShader(tyCode->GetBufferPointer(), tyCode->GetBufferSize(), nullptr, &psTapY_);
    if (tuvCode) dev_->CreatePixelShader(tuvCode->GetBufferPointer(), tuvCode->GetBufferSize(), nullptr, &psTapUV_);
    auto bgraCode = compile("ps_bgra", "ps_4_0"), tryCode = compile("ps_tap_rgb_y", "ps_4_0"),
         truvCode = compile("ps_tap_rgb_uv", "ps_4_0");
    if (bgraCode) dev_->CreatePixelShader(bgraCode->GetBufferPointer(), bgraCode->GetBufferSize(), nullptr, &psBgra_);
    if (tryCode) dev_->CreatePixelShader(tryCode->GetBufferPointer(), tryCode->GetBufferSize(), nullptr, &psTapRgbY_);
    if (truvCode)
        dev_->CreatePixelShader(truvCode->GetBufferPointer(), truvCode->GetBufferSize(), nullptr, &psTapRgbUV_);
    bd.ByteWidth = 16;
    dev_->CreateBuffer(&bd, nullptr, &tapCb_);
    if (!sampler_ || !cb_) return false;
    return true;
}

// Swap chain + back-buffer render target on dev_ for hwnd_.
bool Renderer::createSwapChain() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    if (width_ && height_ && rc.right > rc.left && rc.bottom > rc.top) {  // not minimized
        width_ = rc.right - rc.left;
        height_ = rc.bottom - rc.top;
    }
    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(dev_.As(&dxgiDev)) || FAILED(dxgiDev->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        log("cannot reach DXGI factory");
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = std::max(width_, 1u);
    sd.Height = std::max(height_, 1u);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = kSwapFlags;
    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = factory->CreateSwapChainForHwnd(dev_.Get(), hwnd_, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr) || FAILED(sc1.As(&swap_))) {
        log("CreateSwapChainForHwnd failed hr=0x%08lx", hr);
        return false;
    }
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    swap_->SetMaximumFrameLatency(1);
    waitable_ = swap_->GetFrameLatencyWaitableObject();
    faultSwallow_ = false;
    return createTargets();
}

bool Renderer::recreateSwapChain() {
    if (!dev_ || !ctx_) return false;
    releaseD2D();
    rtv_.Reset();
    ctx_->ClearState();
    ctx_->Flush();  // flip model: the old swap chain must really be gone before a new one
    if (waitable_) CloseHandle(waitable_);
    waitable_ = nullptr;
    swap_.Reset();
    occluded_ = false;
    if (createSwapChain()) return true;
    rtv_.Reset();
    swap_.Reset();
    return false;
}

void Renderer::releaseDevice() {
    if (ctx_) {
        ctx_->ClearState();
        ctx_->Flush();  // flip model: the old swap chain must really be gone before a new one
    }
    releaseD2D();
    rtv_.Reset();
    if (waitable_) CloseHandle(waitable_);
    waitable_ = nullptr;
    swap_.Reset();
    vs_.Reset();
    ps_.Reset();
    sampler_.Reset();
    cb_.Reset();
    evt_.Reset();
    tapReset();
    for (auto& t : tap_) t = {};
    tapY_.Reset();
    tapUV_.Reset();
    tapYRtv_.Reset();
    tapUVRtv_.Reset();
    tapW_ = tapH_ = 0;
    psTapY_.Reset();
    psTapUV_.Reset();
    psTapRgbY_.Reset();
    psTapRgbUV_.Reset();
    psBgra_.Reset();
    tapCb_.Reset();
    liveReleaseDevice();  // 即時翻譯: the signature texture (else it keeps the lost device alive)
    cur_ = {};
    // attachDevice() uploads the frozen picture again from frozenCpu_.
    if (frozenOn_ && frozen_.valid()) frozenWasUp_ = true;
    frozen_ = {};
    pool_.clear();
    havePicture_ = false;  // a live scene shows black until the decoder delivers again
    ctx_.Reset();
    dev_.Reset();
}

void Renderer::shutdown() {
    releaseDevice();
    // Everything COM-based goes now: the worker uninitialises COM right after.
    formats_.clear();
    heart_.Reset();
    sparkle_.Reset();
    playTri_.Reset();
    round_.Reset();
    toutou_.reset();
    dwrite_.Reset();
    d2d_.Reset();
}

bool Renderer::createTargets() {
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    return SUCCEEDED(dev_->CreateRenderTargetView(back.Get(), nullptr, &rtv_));
}

bool Renderer::resize(UINT w, UINT h) {
    if (w == 0 || h == 0) {  // minimized: keep buffers, skip rendering
        width_ = height_ = 0;
        return true;
    }
    if (w == width_ && h == height_ && rtv_) return true;
    width_ = w;
    height_ = h;
    if (!swap_) return true;  // no device right now: attachDevice() picks the size up
    releaseD2D();
    rtv_.Reset();
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    ctx_->Flush();
    HRESULT hr = swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, kSwapFlags);
    if (FAILED(hr)) {
        log("ResizeBuffers(%u,%u) failed hr=0x%08lx", w, h, hr);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) return false;
    }
    createTargets();
    return true;
}

}  // namespace pm::video
