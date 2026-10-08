#include "renderer.h"

#include <d3dcompiler.h>
#include <mfapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "log.h"
#include "pm/i18n.h"

namespace pm::video {

double clockMs() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
}

namespace {

constexpr UINT kSwapFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

// ---- UI timing (ms) ----
constexpr double kFadeMs = 250;        // picture fade in / out
constexpr double kPinFadeMs = 160;
constexpr double kToastInMs = 150, kToastHoldMs = 2500, kToastOutMs = 350;
constexpr double kAmbientSettleMs = 1500;  // breathing amplitude eases out
constexpr double kConnectTimeoutMs = 60000;
constexpr double kFastFrameMs = 1000.0 / 60, kSlowFrameMs = 1000.0 / 30;

constexpr double kDimMs = 200;
constexpr float kDimMax = 0.55f;
constexpr double kHoverMs = 160;
constexpr double kReactMs = 1250;      // click reaction (hop, hearts, bubble)
constexpr double kHopMs = 1000;        // squash-and-stretch hop
constexpr double kConnectIntroMs = 900;  // happy hop before the spinner scene
constexpr double kTapFlushMs = 20;     // newest tapped picture delivered at the latest after this
// Live toolbar: shown on mouse movement, hidden kToolHoldMs after the last one.
constexpr double kToolHoldMs = 2000, kToolInMs = 140, kToolOutMs = 260;
constexpr double kTipDelayMs = 450, kTipFadeMs = 120;

constexpr D2D1_COLOR_F rgb(uint32_t c) {
    return {((c >> 16) & 0xff) / 255.f, ((c >> 8) & 0xff) / 255.f, (c & 0xff) / 255.f, 1};
}
// Themes: soft pink, mint, lavender night and milk tea.  The mascot's cloud
// stays white; only its phone screen, rim light, beam and hearts take the
// accent.
//                                bgTop     bgBottom  fg        dim       accent    accent2   ink       card      well
const Renderer::Palette kPalettes[4] = {
    {rgb(0x2A1C1F), rgb(0x181012), rgb(0xFFF4F1), rgb(0xD1B0B0), rgb(0xF5A7A7), rgb(0xE65C54), rgb(0x5A2A2A), rgb(0x332226), rgb(0x452E32)},
    {rgb(0x123230), rgb(0x0A1D1C), rgb(0xF0FCF8), rgb(0xA6CFC4), rgb(0x8FE3C4), rgb(0xF2C6A0), rgb(0x0F3D33), rgb(0x173D39), rgb(0x1F4E49)},
    {rgb(0x1B1D3D), rgb(0x0D0E23), rgb(0xF4F2FF), rgb(0xB3AFD8), rgb(0xBBA9F7), rgb(0x8FA2FF), rgb(0x2B2366), rgb(0x24264D), rgb(0x2F3260)},
    {rgb(0x30231B), rgb(0x1A120D), rgb(0xFFF6EC), rgb(0xD8C3AC), rgb(0xE3B98A), rgb(0xF4DCC0), rgb(0x4D301B), rgb(0x3B2B21), rgb(0x4C392D)},
};
constexpr D2D1_COLOR_F kRecRed = rgb(0xFF4F55);
constexpr D2D1_COLOR_F kDangerRed = rgb(0xFF6B6B);  // 中斷連線 (readable on every theme's card)

constexpr float kPi = 3.14159265f;
using pm::i18n::S;
using pm::i18n::tr;
// Mascot speech bubbles (click reaction), awake / asleep: pm/i18n_strings.inc.
constexpr S kBubbleLines[] = {S::Bubble1, S::Bubble2, S::Bubble3, S::Bubble4, S::Bubble5};
constexpr S kSleepyLines[] = {S::Sleepy1, S::Sleepy2, S::Sleepy3};

float ease(double t) {  // smoothstep on [0,1]
    float x = static_cast<float>(std::clamp(t, 0.0, 1.0));
    return x * x * (3 - 2 * x);
}

const char kShader[] = R"(
cbuffer CB : register(b0) {
    float4 uvRect;   // u0 v0 u1 v1 (visible area in texture coords)
    float4 range;    // yOffset, yScale, cScale, 0
    float4 mat;      // R.cr, G.cb, G.cr, B.cb
    float4 xfU;      // rotation / mirror: texture coord = (dot(xfU.xy, t) + xfU.z,
    float4 xfV;      //                                    dot(xfV.xy, t) + xfV.z)
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
float4 ps_bgra(VSOut i) : SV_Target {
    return float4(texRGB.Sample(smp, i.uv).rgb, 1);
}
float4 ps_main(VSOut i) : SV_Target {
    float  y = (texY.Sample(smp, i.uv) - range.x) * range.y;
    float2 c = (texUV.Sample(smp, i.uv) - 0.5) * range.z;
    float3 rgb = float3(y + mat.x * c.y, y + mat.y * c.x + mat.z * c.y, y + mat.w * c.x);
    return float4(saturate(rgb), 1);
}
)";

struct Constants {
    float uvRect[4];
    float range[4];
    float mat[4];
    float xfU[4];
    float xfV[4];
};

// Screen coords t (0..1, y down) -> picture coords for a clockwise rotation
// of the picture by rot quarter turns, then a horizontal mirror.
void pictureTransform(int rot, bool mirror, float u[4], float v[4]) {
    // Mirror on screen first: tx' = mx * tx + mc.
    const float mx = mirror ? -1.f : 1.f, mc = mirror ? 1.f : 0.f;
    // s = R(t'), coefficients of (tx', ty, 1).
    float a[3], b[3];
    switch (rot & 3) {
    default: a[0] = 1; a[1] = 0; a[2] = 0; b[0] = 0; b[1] = 1; b[2] = 0; break;    // (tx, ty)
    case 1: a[0] = 0; a[1] = 1; a[2] = 0; b[0] = -1; b[1] = 0; b[2] = 1; break;    // (ty, 1-tx)
    case 2: a[0] = -1; a[1] = 0; a[2] = 1; b[0] = 0; b[1] = -1; b[2] = 1; break;   // (1-tx, 1-ty)
    case 3: a[0] = 0; a[1] = -1; a[2] = 1; b[0] = 1; b[1] = 0; b[2] = 0; break;    // (1-ty, tx)
    }
    u[0] = a[0] * mx; u[1] = a[1]; u[2] = a[0] * mc + a[2]; u[3] = 0;
    v[0] = b[0] * mx; v[1] = b[1]; v[2] = b[0] * mc + b[2]; v[3] = 0;
}

}  // namespace

void Renderer::screenToPicture(int rot, bool mirror, float tx, float ty, float& sx, float& sy) {
    float u[4], v[4];
    pictureTransform(rot, mirror, u, v);
    sx = u[0] * tx + u[1] * ty + u[2];
    sy = v[0] * tx + v[1] * ty + v[2];
}

namespace {

ComPtr<ID3DBlob> compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> code, err;
    HRESULT hr = D3DCompile(kShader, sizeof(kShader) - 1, "pm_video", nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr))
        log("shader %s compile failed: %s", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    return code;
}

D2D1_POINT_2F polar(D2D1_POINT_2F c, float r, float deg) {
    float a = deg * kPi / 180.f;
    return {c.x + r * std::cos(a), c.y + r * std::sin(a)};
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
    if (createDeviceObjects()) return true;
    releaseDevice();
    return false;
}

bool Renderer::createDeviceObjects() {
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
    return createTargets();
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
    cur_ = {};
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

bool Renderer::ensureTextures(Picture& p, bool hw, bool tenBit, UINT w, UINT h) {
    if (p.hw == hw && p.tenBit == tenBit && p.w == w && p.h == h && p.ySrv) return true;
    p = {};
    p.hw = hw;
    p.tenBit = tenBit;
    const DXGI_FORMAT fY = tenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    const DXGI_FORMAT fUV = tenBit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    p.w = w;
    p.h = h;
    D3D11_TEXTURE2D_DESC td{};
    td.MipLevels = td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    HRESULT hr;
    if (hw) {
        td.Width = w;
        td.Height = h;
        td.Format = tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
        hr = dev_->CreateTexture2D(&td, nullptr, &p.nv12);
        if (SUCCEEDED(hr)) {
            sv.Format = fY;
            hr = dev_->CreateShaderResourceView(p.nv12.Get(), &sv, &p.ySrv);
        }
        if (SUCCEEDED(hr)) {
            sv.Format = fUV;
            hr = dev_->CreateShaderResourceView(p.nv12.Get(), &sv, &p.uvSrv);
        }
    } else {
        td.Width = w;
        td.Height = h;
        td.Format = fY;
        hr = dev_->CreateTexture2D(&td, nullptr, &p.y);
        if (SUCCEEDED(hr)) hr = dev_->CreateShaderResourceView(p.y.Get(), nullptr, &p.ySrv);
        td.Width = (w + 1) / 2;
        td.Height = (h + 1) / 2;
        td.Format = fUV;
        if (SUCCEEDED(hr)) hr = dev_->CreateTexture2D(&td, nullptr, &p.uv);
        if (SUCCEEDED(hr)) hr = dev_->CreateShaderResourceView(p.uv.Get(), nullptr, &p.uvSrv);
    }
    if (FAILED(hr)) {
        log("texture allocation %ux%u (%s%s) failed hr=0x%08lx", w, h, hw ? "NV12" : "R8/R8G8", tenBit ? ", 10-bit" : "", hr);
        p = {};
        return false;
    }
    return true;
}

bool Renderer::ensureBgra(Picture& p, UINT w, UINT h) {
    if (p.bgra && p.w == w && p.h == h && p.ySrv) return true;
    p = {};
    p.bgra = true;
    p.w = w;
    p.h = h;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = dev_->CreateTexture2D(&td, nullptr, &p.y);
    if (SUCCEEDED(hr)) hr = dev_->CreateShaderResourceView(p.y.Get(), nullptr, &p.ySrv);
    if (FAILED(hr)) {
        log("texture allocation %ux%u (BGRA) failed hr=0x%08lx", w, h, hr);
        p = {};
        return false;
    }
    return true;
}

bool Renderer::uploadBgra(const uint8_t* bgra, UINT w, UINT h, UINT stride) {
    if (!dev_ || !psBgra_ || !ensureBgra(cur_, w, h)) return false;
    // DEFAULT texture + UpdateSubresource: the runtime copies the rows into
    // its upload heap right away (one memcpy), the GPU copy is queued.
    ctx_->UpdateSubresource(cur_.y.Get(), 0, nullptr, bgra, stride, 0);
    VideoFormat f;
    f.codedWidth = w;
    f.codedHeight = h;
    f.crop = {0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    f.fullRange = true;
    f.stride = static_cast<LONG>(stride);
    cur_.fmt = f;
    pictureShown();
    return true;
}

bool Renderer::copyIn(IMFSample* sample, const VideoFormat& fmt, Picture& p) {
    if (!dev_) return false;
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(sample->GetBufferByIndex(0, &buf))) return false;

    ComPtr<IMFDXGIBuffer> dxgiBuf;
    if (SUCCEEDED(buf.As(&dxgiBuf))) {
        ComPtr<ID3D11Texture2D> src;
        UINT sub = 0;
        if (FAILED(dxgiBuf->GetResource(IID_PPV_ARGS(&src))) || FAILED(dxgiBuf->GetSubresourceIndex(&sub)))
            return false;
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        if (!ensureTextures(p, true, sd.Format == DXGI_FORMAT_P010, sd.Width, sd.Height)) return false;
        ctx_->CopySubresourceRegion(p.nv12.Get(), 0, 0, 0, 0, src.Get(), sub, nullptr);
    } else {
        if (!ensureTextures(p, false, fmt.tenBit, fmt.codedWidth, fmt.codedHeight)) return false;
        BYTE* data = nullptr;
        LONG pitch = 0;
        ComPtr<IMF2DBuffer> buf2d;
        bool locked2d = SUCCEEDED(buf.As(&buf2d)) && SUCCEEDED(buf2d->Lock2D(&data, &pitch));
        DWORD maxLen = 0, curLen = 0;
        if (!locked2d) {
            if (FAILED(buf->Lock(&data, &maxLen, &curLen))) return false;
            pitch = std::abs(fmt.stride);
        }
        if (pitch > 0) {
            ctx_->UpdateSubresource(p.y.Get(), 0, nullptr, data, pitch, 0);
            ctx_->UpdateSubresource(p.uv.Get(), 0, nullptr, data + static_cast<size_t>(pitch) * fmt.codedHeight,
                                    pitch, 0);
        }
        if (locked2d) buf2d->Unlock2D();
        else buf->Unlock();
    }
    // Wait for the GPU (DXVA decode + copy) so the caller's latency numbers
    // reflect a picture that is really ready; costs nothing in steady state
    // because the next Present would wait for it anyway.  (After device
    // removal GetData fails instead of returning S_FALSE.)
    if (evt_) {
        ctx_->End(evt_.Get());
        while (ctx_->GetData(evt_.Get(), nullptr, 0, 0) == S_FALSE) SwitchToThread();
    }
    p.fmt = fmt;
    return true;
}

void Renderer::pictureShown() {
    havePicture_ = true;
    if (dimmed_) setDimmed(false);  // the stream is back: no longer "on hold"
    if (scene_ != Scene::Live) {
        const double now = clockMs();
        // Reconnect during a fade-out: fade in from the idle screen.
        fadeFrom_ = scene_;
        scene_ = Scene::Live;
        fadeInAt_ = now;
        fadeOutAt_ = -1e9;
    }
}

bool Renderer::upload(IMFSample* sample, const VideoFormat& fmt) {
    if (!copyIn(sample, fmt, cur_)) return false;
    pictureShown();
    return true;
}

bool Renderer::hold(IMFSample* sample, const VideoFormat& fmt, Picture& out) {
    out = {};
    if (!pool_.empty()) {
        out = std::move(pool_.back());
        pool_.pop_back();
    }
    if (copyIn(sample, fmt, out)) return true;
    out = {};
    return false;
}

void Renderer::show(Picture&& pic) {
    if (!pic.valid()) return;
    std::swap(cur_, pic);
    recycle(std::move(pic));
    pictureShown();
}

void Renderer::recycle(Picture&& pic) {
    if (!pic.valid() || pool_.size() >= kMaxPool) return;
    // Only keep textures that match the current picture (size / hw-sw kind).
    if (pic.bgra || (cur_.valid() && (pic.w != cur_.w || pic.h != cur_.h || pic.hw != cur_.hw ||
                                      pic.tenBit != cur_.tenBit || cur_.bgra)))
        return;
    pool_.push_back(std::move(pic));
}

void Renderer::reset() {
    const double now = clockMs();
    if (scene_ == Scene::Live && havePicture_ && !paused_) fadeOutAt_ = now;
    scene_ = Scene::Idle;
    paused_ = false;
    sceneAt_ = now;
    deviceName_.clear();
    connectIntro_ = false;
    poke();
}

void Renderer::setConnecting(const std::wstring& name) {
    deviceName_ = name;
    if (scene_ == Scene::Live) return;  // a picture is already up: nothing to announce
    const double now = clockMs();
    if (scene_ == Scene::Idle) {
        // She is happy about it: a hop + hearts, then the spinner scene.
        connectIntro_ = !paused_;
        if (connectIntro_) {
            reactAt_ = now;
            reactBubble_ = false;
        }
    }
    scene_ = Scene::Connecting;
    sceneAt_ = now;
}

void Renderer::setPin(const std::wstring& pin) {
    const double now = clockMs();
    if (!pin.empty()) {
        pin_ = pin;
        if (!pinVisible_) {
            pinVisible_ = true;
            pinAt_ = now;
        }
    } else if (pinVisible_) {
        pinVisible_ = false;
        pinAt_ = now;
    }
}

void Renderer::showToast(const std::wstring& text, double holdMs) {
    toast_ = text;
    toastAt_ = clockMs();
    toastHold_ = holdMs > 0 ? holdMs : kToastHoldMs;
}

// ---------------------------------------------------------------------------
// Live toolbar: a themed pill of icon buttons at the top centre of the
// picture, faded in on mouse movement and out kToolHoldMs after the last one
// (kept while the cursor is on it).  Tooltip under the hovered button.

void Renderer::setToolbar(std::vector<ToolItem> items) {
    toolItems_ = std::move(items);
    if (toolHot_ >= static_cast<int>(toolItems_.size())) toolHot_ = -1;
}

float Renderer::toolbarAlpha(double now) const {
    if (!toolbarAvailable()) return 0;
    const float in = ease((now - toolInAt_) / kToolInMs);
    if (toolInside_ || now < toolUntil_) return in;
    return std::min(in, 1 - ease((now - toolUntil_) / kToolOutMs));
}

bool Renderer::toolbarActivity() {
    if (!toolbarAvailable()) return false;
    const double now = clockMs();
    const float a = toolbarAlpha(now);
    if (a < 1) toolInAt_ = now - kToolInMs * a;  // (re)appear from about the current opacity
    toolUntil_ = std::max(toolUntil_, now + kToolHoldMs);
    return a < 1;  // needs a frame (fade in)
}

void Renderer::toolbarLeave() {
    const double now = clockMs();
    toolInside_ = false;
    toolHot_ = -1;
    if (toolUntil_ > now) toolUntil_ = now;
}

void Renderer::setToolbarHover(int index, bool inside) {
    const double now = clockMs();
    if (index != toolHot_) {
        // Moving between buttons while a tooltip is up: the next one shows at once.
        const bool warm = toolHot_ >= 0 && now - toolHotAt_ >= kTipDelayMs;
        toolHot_ = index;
        toolHotAt_ = warm && index >= 0 ? now - kTipDelayMs - kTipFadeMs : now;
    }
    if (toolInside_ && !inside) toolUntil_ = std::max(toolUntil_, now + kToolHoldMs);  // left it: the usual delay
    toolInside_ = inside;
}

ComPtr<IDWriteTextFormat> Renderer::iconFormat(float size) {
    if (iconFamily_.empty()) {
        iconFamily_ = L"-";
        ComPtr<IDWriteFontCollection> fonts;
        if (dwrite_ && SUCCEEDED(dwrite_->GetSystemFontCollection(&fonts)))
            for (const wchar_t* fam : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
                UINT32 idx = 0;
                BOOL exists = FALSE;
                if (SUCCEEDED(fonts->FindFamilyName(fam, &idx, &exists)) && exists) {
                    iconFamily_ = fam;
                    break;
                }
            }
    }
    if (iconFamily_ == L"-") return nullptr;
    size = std::round(size * 2) / 2;
    if (iconFmt_ && iconFmtSize_ == size) return iconFmt_;
    iconFmt_.Reset();
    if (SUCCEEDED(dwrite_->CreateTextFormat(iconFamily_.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-TW",
                                            &iconFmt_))) {
        iconFmt_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        iconFmt_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        iconFmt_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        iconFmtSize_ = size;
    }
    return iconFmt_;
}

void Renderer::drawToolbar(const D2D1_RECT_F& area, const FrameGeom* frame, double now) {
    toolRects_.clear();
    toolPill_ = {};
    const float a = toolbarAlpha(now);
    if (a <= 0.003f || toolItems_.empty()) return;
    const float s = dpi_ / 96.f;
    const float W = width_ / s;
    const float areaW = area.right - area.left;
    const int n = static_cast<int>(toolItems_.size());
    int groups = 0;
    for (int i = 1; i < n; ++i) groups += toolItems_[i].group ? 1 : 0;
    // Natural size, shrunk (down to 26 DIP buttons) to fit the picture.
    float btn = 36, gap = 2, div = 11, pad = 5;
    const float natural = 2 * pad + n * btn + (n - 1) * gap + groups * div;
    const float avail = std::max(120.f, std::min(areaW, W) - 16);
    if (natural > avail) {
        const float k = std::max(26.f / 36.f, avail / natural);
        btn *= k;
        gap *= k;
        div *= k;
        pad *= k;
    }
    const float pillW = 2 * pad + n * btn + (n - 1) * gap + groups * div, pillH = btn + 2 * pad;
    // Top centre of the picture; below the Dynamic Island when framed (portrait).
    float top = area.top + 10;
    if (frame) {
        const float pw = std::min(frame->screen.right - frame->screen.left, frame->screen.bottom - frame->screen.top);
        top = frame->landscape ? area.top + std::max(10.f, frame->radius * 0.30f / s) : area.top + pw * 0.122f / s + 8;
    }
    float x0 = std::round((area.left + area.right) / 2 - pillW / 2);
    x0 = std::clamp(x0, 4.f, std::max(4.f, W - pillW - 4));
    // Never on top of the REC badge: go below it.
    if (recBadge_.right > recBadge_.left && x0 < recBadge_.right + 6 && top < recBadge_.bottom + 4)
        top = recBadge_.bottom + 6;
    const float y0 = std::round(top - (1 - a) * 6);  // slides down a little while fading in
    const D2D1_RECT_F pill{x0, y0, x0 + pillW, y0 + pillH};
    const float r = pillH / 2;
    // Soft shadow, card, hairline accent border.
    d2dTarget_->FillRoundedRectangle({{pill.left - 1, pill.top + 1, pill.right + 1, pill.bottom + 3}, r + 1, r + 1},
                                     brush(D2D1::ColorF(0, 0, 0), 0.28f * a));
    d2dTarget_->FillRoundedRectangle({pill, r, r}, brush(pal_.card, 0.95f * a));
    d2dTarget_->DrawRoundedRectangle(
        {{pill.left + 0.5f, pill.top + 0.5f, pill.right - 0.5f, pill.bottom - 0.5f}, r - 0.5f, r - 0.5f},
        brush(pal_.accent, 0.38f * a), 1.f);
    auto icons = iconFormat(btn * 0.42f);
    const bool interactive = a > 0.35f;
    float x = pill.left + pad;
    D2D1_RECT_F hotRect{};
    for (int i = 0; i < n; ++i) {
        const ToolItem& it = toolItems_[i];
        if (i > 0 && it.group) {
            const float cx = std::round(x - gap / 2 + div / 2) + 0.5f;
            d2dTarget_->DrawLine({cx, pill.top + pillH * 0.30f}, {cx, pill.bottom - pillH * 0.30f},
                                 brush(pal_.fg, 0.20f * a), 1.f);
            x += div;
        }
        const D2D1_RECT_F b{x, pill.top + pad, x + btn, pill.top + pad + btn};
        const D2D1_ELLIPSE disc{{(b.left + b.right) / 2, (b.top + b.bottom) / 2}, btn / 2, btn / 2};
        const bool hot = i == toolHot_;
        D2D1_COLOR_F ink = it.danger ? kDangerRed : pal_.fg;
        if (it.toggled) {
            d2dTarget_->FillEllipse(disc, brush(kRecRed, (hot ? 0.36f : 0.24f) * a));
            ink = kRecRed;
        } else if (hot && it.danger) {
            d2dTarget_->FillEllipse(disc, brush(kDangerRed, 0.92f * a));
            ink = D2D1::ColorF(1, 1, 1);
        } else if (hot) {
            d2dTarget_->FillEllipse(disc, brush(pal_.accent, 0.26f * a));
        }
        if (icons && it.glyph) {
            const wchar_t g[2] = {it.glyph, 0};
            d2dTarget_->DrawText(g, 1, icons.Get(), b, brush(ink, a));
        }
        if (it.toggled) {  // pulsing dot: recording now
            const float ph = static_cast<float>(std::fmod(now / 1400.0, 1.0));
            const float pulse = 0.5f + 0.5f * std::cos(ph * 2 * kPi);
            d2dTarget_->FillEllipse({{b.right - btn * 0.2f, b.top + btn * 0.2f}, btn * 0.08f, btn * 0.08f},
                                    brush(kRecRed, (0.55f + 0.45f * pulse) * a));
        }
        if (hot) hotRect = b;
        if (interactive)
            toolRects_.push_back({static_cast<LONG>(std::floor(b.left * s)), static_cast<LONG>(std::floor(b.top * s)),
                                  static_cast<LONG>(std::ceil(b.right * s)), static_cast<LONG>(std::ceil(b.bottom * s))});
        x += btn + gap;
    }
    if (interactive)
        toolPill_ = {static_cast<LONG>(std::floor(pill.left * s)), static_cast<LONG>(std::floor(pill.top * s)),
                     static_cast<LONG>(std::ceil(pill.right * s)), static_cast<LONG>(std::ceil(pill.bottom * s))};

    // Tooltip under the hovered button (after a short delay).
    if (toolHot_ < 0 || toolHot_ >= n || toolItems_[toolHot_].tip.empty()) return;
    const float ta = a * ease((now - toolHotAt_ - kTipDelayMs) / kTipFadeMs);
    if (ta <= 0.003f) return;
    const float size = std::clamp(btn * 0.37f, 11.5f, 13.5f);
    auto l = layout(toolItems_[toolHot_].tip, size, W - 16, DWRITE_FONT_WEIGHT_NORMAL, false);
    if (!l) return;
    const float tw = std::min(textW(l.Get()), W - 32), th = textH(l.Get());
    const float padX = size * 0.75f, padY = size * 0.38f;
    const float bw = tw + 2 * padX, bh = th + 2 * padY;
    float bx = std::round((hotRect.left + hotRect.right) / 2 - bw / 2);
    bx = std::clamp(bx, 4.f, std::max(4.f, W - bw - 4));
    const float by = std::round(pill.bottom + 6);
    const D2D1_ROUNDED_RECT tip{{bx, by, bx + bw, by + bh}, 6, 6};
    d2dTarget_->FillRoundedRectangle({{bx, by + 1, bx + bw, by + bh + 2}, 6, 6},
                                     brush(D2D1::ColorF(0, 0, 0), 0.25f * ta));
    d2dTarget_->FillRoundedRectangle(tip, brush(pal_.card, 0.97f * ta));
    d2dTarget_->DrawRoundedRectangle(tip, brush(pal_.accent, 0.30f * ta), 1.f);
    l->SetMaxWidth(std::ceil(tw) + 2.f);
    d2dTarget_->DrawTextLayout({bx + padX - 1.f, by + padY}, l.Get(),
                               brush(toolItems_[toolHot_].danger ? kDangerRed : pal_.fg, ta));
}

double Renderer::nextFrameInMs() const {
    if (width_ == 0 || height_ == 0 || !visible_) return -1;  // minimized / hidden: no animation
    const double now = clockMs();
    constexpr double kTail = 40;  // draw one frame past each animation's end
    // Live toolbar: fading in / out, tooltip appearing, a recording dot
    // pulsing, or (event-driven) the moment it starts to hide.
    bool toolFast = false;
    double toolDue = -1;
    if (toolbarAvailable()) {
        const float ta = toolbarAlpha(now);
        const bool fadingTool = now - toolInAt_ < kToolInMs + kTail ||
                                (!toolInside_ && now >= toolUntil_ && now - toolUntil_ < kToolOutMs + kTail);
        const bool tip = ta > 0 && toolHot_ >= 0 && now - toolHotAt_ < kTipDelayMs + kTipFadeMs + kTail;
        bool recDot = false;
        for (const auto& t : toolItems_) recDot |= t.toggled;
        toolFast = fadingTool || (tip && now - toolHotAt_ >= kTipDelayMs) || (ta > 0 && recDot);
        if (tip && now - toolHotAt_ < kTipDelayMs) toolDue = kTipDelayMs - (now - toolHotAt_);
        if (ta > 0 && !toolInside_ && now < toolUntil_) {
            const double d = toolUntil_ - now;
            toolDue = toolDue < 0 ? d : std::min(toolDue, d);
        }
    }
    bool fast = toolFast || now - fadeInAt_ < kFadeMs + kTail || now - fadeOutAt_ < kFadeMs + kTail ||
                now - pinAt_ < kPinFadeMs + kTail ||
                (!toast_.empty() && now - toastAt_ < toastHold_ + kToastOutMs + kTail) ||
                (scene_ == Scene::Connecting && !paused_) || now - dimAt_ < kDimMs + kTail ||
                now - mascotHotAt_ < kHoverMs + kTail || now - reactAt_ < kReactMs + 250 + kTail;
    // Idle / paused screen: the mascot floats (and blinks) for a while after
    // any activity, then rests on a static frame.
    bool slow = ((scene_ == Scene::Idle || paused_) && now < ambientUntil_ + kTail) || recording_;
    if (!fast && !slow) return toolDue;
    // Fully covered window (Present reported occlusion): probe slowly.
    const double period = occluded_ ? 500 : fast ? kFastFrameMs : kSlowFrameMs;
    const double next = std::max(0.0, period - (now - lastRender_));
    return toolDue >= 0 ? std::min(next, toolDue) : next;
}

void Renderer::drawPicture(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp) {
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    ctx_->RSSetViewports(1, &vp);

    Constants c{};
    c.uvRect[0] = cur_.fmt.crop.left / static_cast<float>(cur_.w);
    c.uvRect[1] = cur_.fmt.crop.top / static_cast<float>(cur_.h);
    c.uvRect[2] = cur_.fmt.crop.right / static_cast<float>(cur_.w);
    c.uvRect[3] = cur_.fmt.crop.bottom / static_cast<float>(cur_.h);
    if (cur_.fmt.fullRange) {
        c.range[0] = 0; c.range[1] = 1; c.range[2] = 1;
    } else {
        c.range[0] = 16.f / 255; c.range[1] = 255.f / 219; c.range[2] = 255.f / 224;
    }
    if (cur_.fmt.bt601) {
        c.mat[0] = 1.402f; c.mat[1] = -0.344136f; c.mat[2] = -0.714136f; c.mat[3] = 1.772f;
    } else {
        c.mat[0] = 1.5748f; c.mat[1] = -0.187324f; c.mat[2] = -0.468124f; c.mat[3] = 1.8556f;
    }
    pictureTransform(rot_, mirror_, c.xfU, c.xfV);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, &c, sizeof(c));
        ctx_->Unmap(cb_.Get(), 0);
    }
    // NV12 / P010: t0 = Y, t1 = UV; BGRA: t2.
    ID3D11ShaderResourceView* srvs[3] = {cur_.bgra ? nullptr : cur_.ySrv.Get(), cur_.uvSrv.Get(),
                                         cur_.bgra ? cur_.ySrv.Get() : nullptr};
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ctx_->PSSetShader(cur_.bgra ? psBgra_.Get() : ps_.Get(), nullptr, 0);
    ctx_->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ctx_->PSSetShaderResources(0, 3, srvs);
    ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    ctx_->Draw(4, 0);
    ID3D11ShaderResourceView* nulls[3] = {};
    ctx_->PSSetShaderResources(0, 3, nulls);
}

bool Renderer::snapshot(std::vector<uint8_t>& out, UINT& w, UINT& h, bool framed) {
    if (!havePicture_ || !cur_.valid()) return false;
    int dw = 0, dh = 0;
    displaySize(dw, dh);
    if (dw <= 0 || dh <= 0) return false;
    FrameGeom g{};
    float o = 0;
    if (framed) {
        g = frameFor(static_cast<float>(dw), static_cast<float>(dh), 0, 0, 0, 0, false);
        o = std::ceil(g.outset()) + 1;
        g.screen = {o, o, o + dw, o + dh};
    }
    w = static_cast<UINT>(dw + 2 * o);
    h = static_cast<UINT>(dh + 2 * o);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> tex, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &tex)) ||
        FAILED(dev_->CreateRenderTargetView(tex.Get(), nullptr, &rtv)))
        return false;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &staging))) return false;
    const float clear[4] = {0, 0, 0, 0};
    ctx_->ClearRenderTargetView(rtv.Get(), clear);
    D3D11_VIEWPORT vp{o, o, static_cast<float>(dw), static_cast<float>(dh), 0, 1};
    drawPicture(rtv.Get(), vp);  // same shader as on screen: identical colours
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    if (framed && d2d_) {
        ComPtr<IDXGISurface> surf;
        ComPtr<ID2D1RenderTarget> rt;
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        if (FAILED(tex.As(&surf)) || FAILED(d2d_->CreateDxgiSurfaceRenderTarget(surf.Get(), &props, &rt))) return false;
        rt->BeginDraw();
        drawDeviceFrame(rt.Get(), g, 1.f);
        if (FAILED(rt->EndDraw())) return false;
    }
    ctx_->CopyResource(staging.Get(), tex.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    out.resize(static_cast<size_t>(w) * h * 4);
    for (UINT y = 0; y < h; ++y)
        std::memcpy(out.data() + static_cast<size_t>(y) * w * 4, static_cast<const uint8_t*>(m.pData) + y * m.RowPitch,
                    static_cast<size_t>(w) * 4);
    ctx_->Unmap(staging.Get(), 0);
    if (framed) {  // premultiplied -> straight alpha for the PNG
        for (size_t i = 0; i < out.size(); i += 4) {
            const uint8_t a = out[i + 3];
            if (a == 0 || a == 255) continue;
            for (int c = 0; c < 3; ++c) out[i + c] = static_cast<uint8_t>(std::min(255, (out[i + c] * 255 + a / 2) / a));
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 2D UI

bool Renderer::ensureD2D() {
    if (!d2d_ || !dwrite_) return false;
    if (!d2dTarget_) {
        ComPtr<IDXGISurface> surf;
        if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&surf)))) return false;
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        if (FAILED(d2d_->CreateDxgiSurfaceRenderTarget(surf.Get(), &props, &d2dTarget_))) return false;
        d2dTarget_->CreateSolidColorBrush(pal_.fg, &brush_);
        d2dTarget_->CreateSolidColorBrush(pal_.dim, &mutedBrush_);
        d2dTarget_->CreateLayer(nullptr, &layer_);
        d2dTarget_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    d2dTarget_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
    return ensureBrushes();
}

bool Renderer::ensureBrushes() {
    if (!bgBrush_ || !glowBrush_ || !beamBrush_) {  // theme colours
        bgBrush_.Reset();
        glowBrush_.Reset();
        beamBrush_.Reset();
        beamCoreBrush_.Reset();
        screenBrush_.Reset();
        shadowBrush_.Reset();
        phoneGlowBrush_.Reset();
        ComPtr<ID2D1GradientStopCollection> stops;
        const D2D1_GRADIENT_STOP bg[2] = {{0, pal_.bgTop}, {1, pal_.bgBottom}};
        if (SUCCEEDED(d2dTarget_->CreateGradientStopCollection(bg, 2, &stops)))
            d2dTarget_->CreateLinearGradientBrush({}, stops.Get(), &bgBrush_);
        stops.Reset();
        const D2D1_GRADIENT_STOP glow[3] = {{0, {pal_.accent.r, pal_.accent.g, pal_.accent.b, 0.20f}},
                                            {0.45f, {pal_.accent.r, pal_.accent.g, pal_.accent.b, 0.07f}},
                                            {1, {pal_.accent.r, pal_.accent.g, pal_.accent.b, 0}}};
        if (SUCCEEDED(d2dTarget_->CreateGradientStopCollection(glow, 3, &stops)))
            d2dTarget_->CreateRadialGradientBrush({}, stops.Get(), &glowBrush_);
        // Mascot: beam ribbon (accent, fading out along it), its white core,
        // the phone screen (light accent -> accent) and the ground shadow.
        const D2D1_COLOR_F a = pal_.accent;
        auto mixc = [](D2D1_COLOR_F x, D2D1_COLOR_F y, float t) {
            return D2D1_COLOR_F{x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, 1};
        };
        auto linear = [&](const D2D1_GRADIENT_STOP* st, UINT32 n, ComPtr<ID2D1LinearGradientBrush>& out) {
            ComPtr<ID2D1GradientStopCollection> c;
            if (SUCCEEDED(d2dTarget_->CreateGradientStopCollection(st, n, &c)))
                d2dTarget_->CreateLinearGradientBrush({}, c.Get(), &out);
        };
        const D2D1_GRADIENT_STOP beam[3] = {{0, {a.r, a.g, a.b, 0.95f}}, {0.7f, {a.r, a.g, a.b, 0.7f}}, {1, {a.r, a.g, a.b, 0}}};
        linear(beam, 3, beamBrush_);
        const D2D1_GRADIENT_STOP core[3] = {{0, {1, 1, 1, 0.95f}}, {0.55f, {1, 1, 1, 0.5f}}, {1, {1, 1, 1, 0}}};
        linear(core, 3, beamCoreBrush_);
        const D2D1_GRADIENT_STOP scr[2] = {{0, mixc(a, {1, 1, 1, 1}, 0.5f)}, {1, a}};
        linear(scr, 2, screenBrush_);
        stops.Reset();
        const D2D1_GRADIENT_STOP sh[3] = {{0, {0, 0, 0, 0.6f}}, {0.55f, {0, 0, 0, 0.3f}}, {1, {0, 0, 0, 0}}};
        if (SUCCEEDED(d2dTarget_->CreateGradientStopCollection(sh, 3, &stops)))
            d2dTarget_->CreateRadialGradientBrush({}, stops.Get(), &shadowBrush_);
        stops.Reset();
        const D2D1_GRADIENT_STOP pg[3] = {{0, {a.r, a.g, a.b, 0.75f}}, {0.62f, {a.r, a.g, a.b, 0.32f}}, {1, {a.r, a.g, a.b, 0}}};
        if (SUCCEEDED(d2dTarget_->CreateGradientStopCollection(pg, 3, &stops)))
            d2dTarget_->CreateRadialGradientBrush({}, stops.Get(), &phoneGlowBrush_);
    }
    return brush_ && bgBrush_ && glowBrush_ && beamBrush_ && beamCoreBrush_ && screenBrush_ && shadowBrush_ &&
           phoneGlowBrush_;
}

void Renderer::releaseD2D() {
    for (auto& b : layerBmp_) b.Reset();
    layerScale_ = 0;
    beamBrush_.Reset();
    beamCoreBrush_.Reset();
    screenBrush_.Reset();
    shadowBrush_.Reset();
    phoneGlowBrush_.Reset();
    glowBrush_.Reset();
    bgBrush_.Reset();
    brush_.Reset();
    mutedBrush_.Reset();
    layer_.Reset();
    d2dTarget_.Reset();
}

ID2D1SolidColorBrush* Renderer::brush(D2D1_COLOR_F c, float alpha) {
    c.a *= alpha;
    brush_->SetColor(c);
    return brush_.Get();
}

ComPtr<IDWriteTextFormat> Renderer::format(float size, DWRITE_FONT_WEIGHT weight, bool latin) {
    size = std::round(size * 4) / 4;
    const int lang = static_cast<int>(pm::i18n::lang());  // the UI font follows the language
    for (auto& f : formats_)
        if (f.size == size && f.weight == weight && f.latin == latin && f.lang == lang) return f.fmt;
    ComPtr<IDWriteTextFormat> fmt;
    dwrite_->CreateTextFormat(latin ? L"Segoe UI" : pm::i18n::uiFont(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL, size, pm::i18n::localeName(), &fmt);
    if (formats_.size() > 32) formats_.clear();
    if (fmt) formats_.push_back({size, weight, latin, lang, fmt});
    return fmt;
}

ComPtr<IDWriteTextLayout> Renderer::layout(const std::wstring& text, float size, float maxW, DWRITE_FONT_WEIGHT weight,
                                           bool wrap, bool latin) {
    ComPtr<IDWriteTextLayout> l;
    auto fmt = format(size, weight, latin);
    if (!fmt) return l;
    if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt.Get(),
                                         std::max(maxW, 1.f), 4096.f, &l)))
        return l;
    l->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    if (!wrap) {
        l->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        ComPtr<IDWriteInlineObject> sign;
        dwrite_->CreateEllipsisTrimmingSign(fmt.Get(), &sign);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        l->SetTrimming(&trim, sign.Get());
    }
    return l;
}

float Renderer::textW(IDWriteTextLayout* l) {
    DWRITE_TEXT_METRICS m{};
    if (!l || FAILED(l->GetMetrics(&m))) return 0;
    return m.widthIncludingTrailingWhitespace;
}

float Renderer::textH(IDWriteTextLayout* l) {
    DWRITE_TEXT_METRICS m{};
    if (!l || FAILED(l->GetMetrics(&m))) return 0;
    return m.height;
}

Renderer::Layout Renderer::layoutFor() const {
    using namespace toutou;
    Layout L{};
    L.s = dpi_ / 96.f;
    L.W = width_ / L.s;
    L.H = height_ / L.s;
    L.landscape = L.W > L.H * 1.05f;
    // The character in frame units: cloud left .. phone right, cloud top ..
    // ground shadow.  It floats in the lower part (portrait) or left of the
    // text column (landscape), never larger than ~570 DIPs of cloud.
    constexpr float kLeft = 50, kRight = 242, kTop = 46, kBottom = 204, kMaxU = 3.6f;
    constexpr float cw = kRight - kLeft, ch = kBottom - kTop;
    if (toutou_.loaded()) {
        if (L.landscape) {
            L.u = std::min({L.W * 0.36f / cw, L.H * 0.60f / ch, kMaxU});
            const float colW = cw * L.u * 1.22f;  // the mascot's column
            const float textW = std::min(L.W - colW, std::max(L.W * 0.45f, 640.f));
            const float x0 = std::max(0.f, (L.W - colW - textW) / 2);  // ultra-wide: centre the group
            L.anchor = {x0 + (colW - cw * L.u) / 2 + (kPivotX - kLeft) * L.u,
                        L.H * 0.54f + (kPivotY - (kTop + kBottom) / 2) * L.u};
            L.text = {x0 + colW * 0.96f, 0, x0 + colW + textW, L.H};
        } else {
            L.u = std::min({L.W * 0.64f / cw, L.H * 0.30f / ch, kMaxU});
            L.anchor = {L.W / 2 + (kPivotX - (kLeft + kRight) / 2) * L.u,
                        L.H - std::max(L.H * 0.035f, 12.f) - (kBottom - kPivotY) * L.u};
            L.text = {0, 0, L.W, L.anchor.y - (kPivotY - kTop) * L.u};
        }
        const D2D1_POINT_2F a = framePoint(L, kCanvas.x, kCanvas.y);
        L.mascot = {a.x, a.y, a.x + kCanvas.w * L.u, a.y + kCanvas.h * L.u};
    } else {
        L.text = {0, 0, L.W, L.H};
    }
    const float rw = L.text.right - L.text.left, rh = L.text.bottom - L.text.top;
    L.title = std::clamp(std::min(rh / 13, rw / 16), 16.f, 56.f);
    L.hint = std::clamp(L.title * 0.56f, 12.f, 30.f);
    return L;
}

D2D1_POINT_2F Renderer::framePoint(const Layout& L, float x, float y) {
    return {L.anchor.x + (x - toutou::kPivotX) * L.u, L.anchor.y + (y - toutou::kPivotY) * L.u};
}

void Renderer::drawBackground(float opacity) {
    const float s = dpi_ / 96.f;
    const float W = width_ / s, H = height_ / s;
    bgBrush_->SetStartPoint({0, 0});
    bgBrush_->SetEndPoint({0, H});
    bgBrush_->SetOpacity(opacity);
    d2dTarget_->FillRectangle({0, 0, W, H}, bgBrush_.Get());
}

// ---- 投投 Toutou ----------------------------------------------------------
// Layers (ToutouArt) in the art's frame units, composed with one transform per
// frame: pivot (cloud bottom centre) -> anchor, scaled by u and the squash /
// stretch, lifted by the float.  The phone and the beam are drawn here (they
// take the theme accent and animate).

namespace {
constexpr D2D1_COLOR_F kToutouInk = rgb(0x3A2830);
}  // namespace

Renderer::Pose Renderer::mascotPose(const Layout& L, Scene scene, double now) {
    using namespace toutou;
    Pose P;
    const bool paused = paused_ && scene != Scene::Connecting;
    const bool busy = scene == Scene::Connecting && !paused;
    const float env = busy ? 1.f : ease((ambientUntil_ - now) / kAmbientSettleMs);
    const double react = now - reactAt_;
    const bool reacting = react >= 0 && react < kReactMs;
    if (busy) {
        P.face = connectIntro_ && reacting ? ToutouArt::FaceHappy : ToutouArt::FaceConnecting;
    } else if (paused) {
        P.face = reacting ? ToutouArt::FaceSleepy : ToutouArt::FaceAsleep;
    } else if (reacting) {
        P.face = ToutouArt::FaceHappy;
    } else {
        // Blinks every 4-7 s (now and then twice) while the screen animates.
        if (env > 0.5f && now >= nextBlink_) {
            if (nextBlink_ > 0) blinkAt_ = now;
            ++blinkN_;
            nextBlink_ = now + (blinkN_ % 5 == 0 ? 330.0 : 4000.0 + 3000.0 * std::fmod(blinkN_ * 0.618034, 1.0));
        }
        P.face = now - blinkAt_ < 150 ? ToutouArt::FaceBlink : ToutouArt::FaceIdle;
    }
    P.phonePose = P.face == ToutouArt::FaceAsleep ? kAsleep : P.face == ToutouArt::FaceSleepy ? kSleepy : kHold;
    P.lit = !paused;
    P.opacity = paused ? 0.84f : 1.f;
    // Float: a slow sine (slower asleep, quicker while connecting) with a hint
    // of stretch while rising and squash while sinking; settles to rest.
    const double period = busy ? 1700 : paused ? 5200 : 3000;
    const float ph = static_cast<float>(std::fmod(now, period) / period) * 2 * kPi;
    const float amp = busy ? 5.f : paused ? 3.f : 6.f;  // frame units
    P.lift = env * (0.5f - 0.5f * std::cos(ph));
    P.dy = -amp * P.lift * L.u;
    const float st = 0.016f * env * std::sin(ph);
    P.sx = 1 - 0.8f * st;
    P.sy = 1 + st;
    float hx, hy, hdy;
    hopTransform(L, now, hx, hy, hdy);
    P.sx *= hx;
    P.sy *= hy;
    P.dy += hdy;
    const float h = static_cast<float>((now - mascotHotAt_) / kHoverMs);
    const bool hot = mascotHot_ && mascotClickable(now);
    const float grow = 1 + 0.04f * (hot ? ease(h) : (mascotHot_ ? 0.f : 1 - ease(h)));
    P.sx *= grow;
    P.sy *= grow;
    P.m = D2D1::Matrix3x2F::Translation(-kPivotX, -kPivotY) * D2D1::Matrix3x2F::Scale(P.sx * L.u, P.sy * L.u) *
          D2D1::Matrix3x2F::Translation(L.anchor.x, L.anchor.y + P.dy);
    // Beam: fast and full while connecting (it grows out first), a calm pulse
    // every 6 s on the idle screen, none asleep.
    if (busy) {
        const double in = now - sceneAt_ - (connectIntro_ ? kConnectIntroMs * 0.6 : 0.0);
        P.beamT1 = ease(in / 450.0);
        P.beamK = P.beamT1 > 0 ? 1.f : 0.f;
        P.beamFlow = now / 600.0;
    } else if (!paused && !reacting && env > 0.003f) {
        const double p = std::fmod(now + 3500.0, 6000.0) / 1700.0;
        if (p < 1) {
            P.beamT1 = ease(p * 1.8);
            P.beamT0 = ease((p - 0.38) / 0.62);
            P.beamK = 0.8f * env;
            P.beamFlow = now / 2400.0;
        }
    }
    return P;
}

bool Renderer::ensureLayerBitmaps(float pxPerUnit) {
    const float q = toutou_.quantize(pxPerUnit);
    if (q == layerScale_ && layerBmp_[0]) return true;
    auto bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    for (int l = 0; l < ToutouArt::kLayers; ++l) {
        layerBmp_[l].Reset();
        IWICBitmap* src = toutou_.scaled(static_cast<ToutouArt::Layer>(l), pxPerUnit);
        if (!src || FAILED(d2dTarget_->CreateBitmapFromWicBitmap(src, &bp, &layerBmp_[l]))) {
            layerScale_ = 0;
            return false;
        }
    }
    layerScale_ = q;
    return true;
}

void Renderer::drawLayer(ToutouArt::Layer l, const D2D1_MATRIX_3X2_F& m, float opacity) {
    if (!layerBmp_[l]) return;
    const auto& p = toutou_.placed(l);
    d2dTarget_->SetTransform(m);
    d2dTarget_->DrawBitmap(layerBmp_[l].Get(), {p.x, p.y, p.x + p.w, p.y + p.h}, opacity,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void Renderer::drawMascot(const Layout& L, const Pose& P, double now) {
    using namespace toutou;
    if (!toutou_.loaded() || L.u <= 0 || !ensureLayerBitmaps(L.u * L.s)) return;
    // Ground shadow: stays on the ground, smaller and fainter as it floats up.
    const D2D1_POINT_2F g = framePoint(L, 135, 195);
    const float k = 1 - 0.22f * P.lift;
    const float rx = 64 * L.u * k * P.sx, ry = 10 * L.u * k;
    shadowBrush_->SetCenter(g);
    shadowBrush_->SetRadiusX(rx);
    shadowBrush_->SetRadiusY(ry);
    shadowBrush_->SetOpacity((0.9f - 0.35f * P.lift) * P.opacity);
    d2dTarget_->FillEllipse({g, rx, ry}, shadowBrush_.Get());

    // The phone screen breathes; the rim light on the cloud follows it.
    const float breath = 0.5f - 0.5f * std::cos(static_cast<float>(std::fmod(now, 4200.0) / 4200) * 2 * kPi);
    const float glow = P.lit ? 0.75f + 0.25f * breath : 0.f;
    drawLayer(ToutouArt::Cloud, P.m, P.opacity);
    if (layerBmp_[ToutouArt::Rim]) {
        const auto& r = toutou_.placed(ToutouArt::Rim);
        const D2D1_RECT_F dst{r.x, r.y, r.x + r.w, r.y + r.h};
        d2dTarget_->SetTransform(P.m);
        d2dTarget_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);  // required by FillOpacityMask
        d2dTarget_->FillOpacityMask(layerBmp_[ToutouArt::Rim].Get(),
                                    brush(pal_.accent, (P.lit ? 0.9f * glow : 0.12f) * P.opacity),
                                    D2D1_OPACITY_MASK_CONTENT_GRAPHICS, &dst, nullptr);
        d2dTarget_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }
    drawLayer(P.face, P.m, P.opacity);
    drawPhone(P, glow);
    drawLayer(static_cast<ToutouArt::Layer>(ToutouArt::HandHold + P.phonePose), P.m, P.opacity);
    drawBeam(P, P.beamT0, P.beamT1, P.beamK, P.beamFlow);
    d2dTarget_->SetTransform(D2D1::IdentityMatrix());
}

void Renderer::drawPhone(const Pose& P, float glow) {
    using namespace toutou;
    const Phone& ph = kPhone[P.phonePose];
    const auto m = D2D1::Matrix3x2F::Rotation(ph.rot) * D2D1::Matrix3x2F::Translation(ph.x, ph.y) *
                   D2D1::Matrix3x2F::Rotation(kTilt, {kPivotX, kPivotY}) * P.m;
    d2dTarget_->SetTransform(m);
    const float w = ph.w / 2, h = ph.h / 2;
    if (glow > 0 && phoneGlowBrush_) {  // soft halo in the accent
        const float rx = w + 15, ry = h + 13;
        phoneGlowBrush_->SetCenter({0, 0});
        phoneGlowBrush_->SetRadiusX(rx);
        phoneGlowBrush_->SetRadiusY(ry);
        phoneGlowBrush_->SetOpacity(glow * P.opacity);
        d2dTarget_->FillEllipse({{0, 0}, rx, ry}, phoneGlowBrush_.Get());
    }
    d2dTarget_->FillRoundedRectangle({{-w, -h, w, h}, kPhoneRadius, kPhoneRadius}, brush(kToutouInk, P.opacity));
    const float b = kPhoneBezel, r = kPhoneRadius - b + 0.4f;
    const D2D1_ROUNDED_RECT scr{{-w + b, -h + b, w - b, h - b}, r, r};
    if (P.lit) {
        screenBrush_->SetStartPoint({0, -h});
        screenBrush_->SetEndPoint({0, h});
        screenBrush_->SetOpacity(P.opacity);
        d2dTarget_->FillRoundedRectangle(scr, screenBrush_.Get());
        if (playTri_) {
            d2dTarget_->FillGeometry(playTri_.Get(), brush({1, 1, 1, 1}, 0.95f * P.opacity));
            d2dTarget_->DrawGeometry(playTri_.Get(), brush({1, 1, 1, 1}, 0.95f * P.opacity), 2.2f, round_.Get());
        }
    } else {
        const D2D1_COLOR_F a = pal_.accent, d = rgb(0x2B2226);
        d2dTarget_->FillRoundedRectangle(
            scr, brush({a.r + (d.r - a.r) * 0.62f, a.g + (d.g - a.g) * 0.62f, a.b + (d.b - a.b) * 0.62f, 1}, P.opacity));
    }
    d2dTarget_->FillRoundedRectangle({{-4, -h + 4.4f, 4, -h + 6.4f}, 1, 1}, brush(kToutouInk, 0.55f * P.opacity));
}

void Renderer::drawBeam(const Pose& P, float t0, float t1, float k, double flow) {
    using namespace toutou;
    if (k <= 0.003f || t1 - t0 <= 0.004f || !beamBrush_) return;
    const Phone& ph = kPhone[P.phonePose];
    const float a = ph.rot * kPi / 180;
    // Leaves the phone top along its axis and arcs to the upper right.
    const D2D1_POINT_2F p0{ph.x + std::sin(a) * (ph.h / 2 + 1), ph.y - std::cos(a) * (ph.h / 2 + 1)};
    const D2D1_POINT_2F p1{p0.x + 58, p0.y - 66};
    const float d = std::hypot(p1.x - p0.x, p1.y - p0.y);
    const D2D1_POINT_2F c{p0.x + std::sin(a) * d * 0.55f, p0.y - std::cos(a) * d * 0.55f};
    auto at = [&](float t) {
        const float u = 1 - t;
        return D2D1_POINT_2F{u * u * p0.x + 2 * u * t * c.x + t * t * p1.x, u * u * p0.y + 2 * u * t * c.y + t * t * p1.y};
    };
    auto normal = [&](float t) {  // unit normal of the centre line
        const float dx = 2 * (1 - t) * (c.x - p0.x) + 2 * t * (p1.x - c.x);
        const float dy = 2 * (1 - t) * (c.y - p0.y) + 2 * t * (p1.y - c.y);
        const float l = std::max(std::hypot(dx, dy), 1e-3f);
        return D2D1_POINT_2F{-dy / l, dx / l};
    };
    const float phase = static_cast<float>(std::fmod(flow, 1.0));
    auto width = [&](float t) {  // grows like a projection, breathes with a slow twist
        return (13 + 11 * t) * (0.84f + 0.16f * std::cos(2 * kPi * (t * 1.1f - phase)));
    };
    auto ribbon = [&](float scale, float ta, float tb) {
        ComPtr<ID2D1PathGeometry> g;
        ComPtr<ID2D1GeometrySink> s;
        if (FAILED(d2d_->CreatePathGeometry(&g)) || FAILED(g->Open(&s))) return ComPtr<ID2D1PathGeometry>();
        constexpr int N = 24;
        D2D1_POINT_2F side[N + 1];
        for (int i = 0; i <= N; ++i) {
            const float t = ta + (tb - ta) * i / N;
            const D2D1_POINT_2F p = at(t), n = normal(t);
            const float hw = width(t) * scale * 0.5f;
            side[i] = {p.x - n.x * hw, p.y - n.y * hw};
            if (i == 0) s->BeginFigure({p.x + n.x * hw, p.y + n.y * hw}, D2D1_FIGURE_BEGIN_FILLED);
            else s->AddLine({p.x + n.x * hw, p.y + n.y * hw});
        }
        for (int i = N; i >= 0; --i) s->AddLine(side[i]);
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        s->Close();
        return g;
    };
    d2dTarget_->SetTransform(D2D1::Matrix3x2F::Rotation(kTilt, {kPivotX, kPivotY}) * P.m);
    beamBrush_->SetStartPoint(p0);
    beamBrush_->SetEndPoint(p1);
    beamCoreBrush_->SetStartPoint(p0);
    beamCoreBrush_->SetEndPoint(p1);
    const float op = k * P.opacity;
    struct Band {
        float scale, alpha;
        ID2D1Brush* b;
    };
    const Band bands[] = {{2.4f, 0.10f, beamBrush_.Get()}, {1.6f, 0.20f, beamBrush_.Get()},
                          {1.0f, 1.0f, beamBrush_.Get()}, {0.4f, 1.0f, beamCoreBrush_.Get()}};
    for (const auto& band : bands) {
        if (auto g = ribbon(band.scale, t0, t1)) {
            band.b->SetOpacity(op * band.alpha);
            d2dTarget_->FillGeometry(g.Get(), band.b);
        }
    }
    // A brighter pulse travelling along it while connecting.
    if (k >= 0.99f) {
        const float x = static_cast<float>(std::fmod(flow * 0.55, 1.0));
        const float ta = std::max(t0, x - 0.09f), tb = std::min(t1, x + 0.09f);
        if (tb > ta)
            if (auto g = ribbon(0.8f, ta, tb))
                d2dTarget_->FillGeometry(g.Get(), brush({1, 1, 1, 1}, 0.35f * op * std::sin(kPi * x)));
    }
    // Square pixel sparkles riding along (white, accent, light accent).
    struct Px {
        float t, side, size;
        int c;
    };
    static constexpr Px kPx[] = {{0.16f, 0.9f, 3.4f, 0},  {0.27f, -1.0f, 2.6f, 1}, {0.38f, 1.25f, 4.4f, 2},
                                 {0.50f, -1.35f, 3.2f, 0}, {0.60f, 0.6f, 2.4f, 1},  {0.70f, 1.55f, 5.0f, 2},
                                 {0.82f, -1.2f, 3.6f, 0},  {0.93f, 0.9f, 2.6f, 1}};
    const D2D1_COLOR_F light{(pal_.accent.r + 1) / 2, (pal_.accent.g + 1) / 2, (pal_.accent.b + 1) / 2, 1};
    for (const auto& p : kPx) {
        const float t = static_cast<float>(std::fmod(p.t + flow * 0.25, 1.0));
        if (t < t0 || t > t1) continue;
        const D2D1_POINT_2F q = at(t), n = normal(t);
        const float off = (13 + 11 * t) * 0.5f * p.side;
        const float x = q.x + n.x * off, y = q.y + n.y * off, hs = p.size / 2;
        const float al = std::min(1.f, 1.6f * std::sin(kPi * t)) * op;
        d2dTarget_->FillRectangle({x - hs, y - hs, x + hs, y + hs},
                                  brush(p.c == 0 ? D2D1_COLOR_F{1, 1, 1, 1} : p.c == 1 ? pal_.accent : light, al));
    }
}

// Hearts bursting from the top of the cloud after a click / when a phone
// connects (accent and a lighter accent).
void Renderer::drawMascotFx(const Layout& L, const Pose& P, double now) {
    const double e = now - reactAt_;
    if (e < 0 || e > 1150 || !heart_ || L.u <= 0 || P.face == ToutouArt::FaceSleepy) return;
    const D2D1_POINT_2F o = P.m.TransformPoint({132, 58});
    const float te = static_cast<float>(e / 1150.0);
    constexpr int kHearts = 7;
    for (int i = 0; i < kHearts; ++i) {
        const float ang = (-90.f + (i - 3) * 25.f + 7.f * std::sin(i * 2.3f)) * kPi / 180.f;
        const float dist = L.u * (34 + 11 * (i % 3)) * (1 - (1 - te) * (1 - te) * (1 - te));
        const float rise = L.u * 9 * te * te;
        const float x = o.x + std::cos(ang) * dist * 1.25f, y = o.y + std::sin(ang) * dist - rise;
        const float pop = te < 0.15f ? 1.2f * ease(te / 0.15f) : 1.2f - 0.2f * ease((te - 0.15f) / 0.2f);
        const float size = L.u * (9 + 2.5f * (i % 3)) * pop;
        const float a = te < 0.55f ? 1.f : 1 - ease((te - 0.55f) / 0.45f);
        const float rot = 14 * std::sin(static_cast<float>(e) * 0.012f + i);
        D2D1_COLOR_F c = pal_.accent;
        if (i % 3 == 1) {  // a few lighter ones
            c.r += (1 - c.r) * 0.35f;
            c.g += (1 - c.g) * 0.35f;
            c.b += (1 - c.b) * 0.35f;
        }
        d2dTarget_->SetTransform(D2D1::Matrix3x2F::Scale(size, size) * D2D1::Matrix3x2F::Rotation(rot) *
                                 D2D1::Matrix3x2F::Translation(x, y));
        d2dTarget_->FillGeometry(heart_.Get(), brush(c, a));
    }
    d2dTarget_->SetTransform(D2D1::IdentityMatrix());
}

void Renderer::hopTransform(const Layout& L, double now, float& sx, float& sy, float& dy) const {
    sx = sy = 1;
    dy = 0;
    const double e = now - reactAt_;
    if (e < 0 || e >= kHopMs) return;
    const float t = static_cast<float>(e / kHopMs);
    const float mh = L.mascot.bottom - L.mascot.top;
    const float hop = mh * 0.06f;
    if (t < 0.12f) {  // anticipation: squash
        const float q = ease(t / 0.12f);
        sy = 1 - 0.06f * q;
        sx = 1 + 0.045f * q;
    } else if (t < 0.52f) {  // airborne: stretched, easing back
        const float p = (t - 0.12f) / 0.40f;
        dy = -hop * 4 * p * (1 - p);
        sy = 1 + 0.045f * (1 - p);
        sx = 1 - 0.03f * (1 - p);
    } else if (t < 0.70f) {  // landing squash
        const float w = std::sin(kPi * (t - 0.52f) / 0.18f);
        sy = 1 - 0.07f * w;
        sx = 1 + 0.05f * w;
    } else {  // settle wobble
        const float q = (t - 0.70f) / 0.30f;
        const float w = std::sin(2 * kPi * q) * (1 - q);
        sy = 1 + 0.02f * w;
        sx = 1 - 0.015f * w;
    }
}

bool Renderer::mascotClickable(double now) const {
    if (!toutou_.loaded() || (!pin_.empty() && (pinVisible_ || now - pinAt_ < kPinFadeMs))) return false;
    if (paused_) return true;  // the paused screen
    return scene_ == Scene::Idle && !(havePicture_ && now - fadeOutAt_ < kFadeMs);
}

void Renderer::setMascotHover(bool hot) {
    if (hot == mascotHot_) return;
    const double now = clockMs();
    // Reverse an unfinished ease from where it is.
    const double e = std::clamp((now - mascotHotAt_) / kHoverMs, 0.0, 1.0);
    mascotHotAt_ = now - (1 - e) * kHoverMs;
    mascotHot_ = hot;
}

void Renderer::mascotClicked() {
    const double now = clockMs();
    if (!mascotClickable(now)) return;
    reactAt_ = now;
    reactBubble_ = true;
    ++bubbleLine_;
    poke();
}

// Speech bubble above the cloud (click reaction).
void Renderer::drawBubble(const Layout& L, const Pose& P, double now) {
    if (!reactBubble_) return;
    const double e = now - reactAt_;
    constexpr double kBubbleMs = kReactMs + 250;
    if (e < 0 || e > kBubbleMs || L.u <= 0) return;
    const wchar_t* text;
    if (paused_) text = tr(kSleepyLines[bubbleLine_ % static_cast<int>(std::size(kSleepyLines))]);
    else text = tr(kBubbleLines[bubbleLine_ % static_cast<int>(std::size(kBubbleLines))]);
    const float size = std::clamp(L.hint * 1.05f, 13.f, 26.f);
    auto l = layout(text, size, L.W * 0.6f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    if (!l) return;
    const float tw = textW(l.Get()), th = textH(l.Get());
    const float padX = size * 0.75f, padY = size * 0.42f, tail = size * 0.55f;
    const float bw = tw + 2 * padX, bh = th + 2 * padY;
    // Tail tip just above the big puff, right of its top; the bubble sits
    // above-right of it.
    D2D1_POINT_2F tip = P.m.TransformPoint({150, 46});
    float x0 = tip.x - bw * 0.18f, y0 = tip.y - tail - bh;
    x0 = std::clamp(x0, 8.f, std::max(8.f, L.W - 8 - bw));
    y0 = std::max(y0, 8.f);
    tip.x = std::clamp(tip.x, x0 + bh * 0.5f, x0 + bw - bh * 0.5f);
    const float te = static_cast<float>(e);
    float sc = 1;  // pop with a small overshoot (back ease-out)
    if (te < 220) {
        const float q = te / 220 - 1;
        sc = 0.55f + 0.45f * (1 + 2.70158f * q * q * q + 1.70158f * q * q);
    }
    const float out0 = static_cast<float>(kBubbleMs - 250);
    const float a = te < 120 ? ease(te / 120) : te > out0 ? 1 - ease((te - out0) / 250) : 1.f;
    if (a <= 0.003f) return;
    d2dTarget_->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc, tip));
    ComPtr<ID2D1RoundedRectangleGeometry> box;
    ComPtr<ID2D1PathGeometry> tri, shape;
    ComPtr<ID2D1GeometrySink> sink;
    const D2D1_ROUNDED_RECT rr{{x0, y0, x0 + bw, y0 + bh}, bh / 2, bh / 2};
    if (SUCCEEDED(d2d_->CreateRoundedRectangleGeometry(rr, &box)) && SUCCEEDED(d2d_->CreatePathGeometry(&tri)) &&
        SUCCEEDED(tri->Open(&sink))) {
        const float bx = std::clamp(tip.x + tail * 0.3f, x0 + bh * 0.5f, x0 + bw - bh * 0.5f);
        sink->BeginFigure({bx - tail * 0.55f, y0 + bh - 1}, D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine({tip.x, tip.y});
        sink->AddLine({bx + tail * 0.35f, y0 + bh - 1});
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        sink.Reset();
        if (SUCCEEDED(d2d_->CreatePathGeometry(&shape)) && SUCCEEDED(shape->Open(&sink))) {
            box->CombineWithGeometry(tri.Get(), D2D1_COMBINE_MODE_UNION, nullptr, sink.Get());
            sink->Close();
            d2dTarget_->FillGeometry(shape.Get(), brush(pal_.card, 0.97f * a));
            d2dTarget_->DrawGeometry(shape.Get(), brush(pal_.accent, 0.75f * a), 1.5f, round_.Get());
        }
    }
    l->SetMaxWidth(std::ceil(tw) + 2.f);
    d2dTarget_->DrawTextLayout({x0 + padX - 1.f, y0 + padY}, l.Get(), brush(pal_.fg, a));
    d2dTarget_->SetTransform(D2D1::IdentityMatrix());
}

void Renderer::drawParticles(const Layout& L, double now, float strength) {
    if (strength <= 0.003f || !heart_ || !sparkle_) return;
    const float unit = std::clamp(std::min(L.W, L.H) / 700.f, 0.8f, 1.8f);
    constexpr int kCount = 9;
    for (int i = 0; i < kCount; ++i) {
        const double period = 17000 + 2600 * (i % 5);
        const float seed = static_cast<float>(std::fmod(i * 0.381966, 1.0));
        const float ph = static_cast<float>(std::fmod(now / period + seed, 1.0));
        const float x = L.W * static_cast<float>(std::fmod(0.13 + i * 0.6180339, 1.0)) +
                        16 * unit * std::sin(static_cast<float>(std::fmod(now * 0.0004, 2 * kPi)) + i * 1.7f);
        const float y = L.H * (1.04f - 1.12f * ph);
        const float size = (7 + 3 * (i % 3)) * unit;
        const float a = strength * 0.32f * std::pow(std::sin(kPi * ph), 1.2f);
        if (a <= 0.003f) continue;
        const float angle = 14 * std::sin(static_cast<float>(std::fmod(now * 0.0007, 2 * kPi)) + i);
        d2dTarget_->SetTransform(D2D1::Matrix3x2F::Scale(size, size) * D2D1::Matrix3x2F::Rotation(angle) *
                                 D2D1::Matrix3x2F::Translation(x, y));
        if (i % 3 == 2) d2dTarget_->FillGeometry(sparkle_.Get(), brush(pal_.fg, a * 0.9f));
        else d2dTarget_->FillGeometry(heart_.Get(), brush(i % 4 == 1 ? pal_.accent2 : pal_.accent, a));
    }
    d2dTarget_->SetTransform(D2D1::IdentityMatrix());
}

void Renderer::drawSpinner(D2D1_POINT_2F c, float radius, double now) {
    const float stroke = std::max(radius * 0.24f, 2.f);
    d2dTarget_->DrawEllipse({c, radius, radius}, brush(pal_.fg, 0.10f), stroke);
    const float start = static_cast<float>(std::fmod(now * 0.36, 360.0));  // 1 turn / s
    const float span = 100.f + 30.f * std::sin(static_cast<float>(std::fmod(now * 0.0021, 2 * kPi)));
    ComPtr<ID2D1PathGeometry> g;
    ComPtr<ID2D1GeometrySink> s;
    if (FAILED(d2d_->CreatePathGeometry(&g)) || FAILED(g->Open(&s))) return;
    s->BeginFigure(polar(c, radius, start), D2D1_FIGURE_BEGIN_HOLLOW);
    s->AddArc({polar(c, radius, start + span), {radius, radius}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
               D2D1_ARC_SIZE_SMALL});
    s->EndFigure(D2D1_FIGURE_END_OPEN);
    s->Close();
    d2dTarget_->DrawGeometry(g.Get(), brush(pal_.accent), stroke, round_.Get());
}

// Sleeping: three z rising from the top right of the cloud (accent).
void Renderer::drawZzz(const Layout& L, const Pose& P, double now) {
    if (L.u <= 0) return;
    struct Z {
        float x, y, size, alpha;
        const wchar_t* text;
    };
    static constexpr Z kZ[3] = {{188, 64, 11, 0.55f, L"z"}, {199, 50, 14, 0.72f, L"z"}, {212, 33, 18, 0.9f, L"Z"}};
    const float env = ease((ambientUntil_ - now) / kAmbientSettleMs);
    for (int i = 0; i < 3; ++i) {
        const auto& z = kZ[i];
        const float size = std::max(z.size * L.u, 10.f);
        auto l = layout(z.text, size, size * 2, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, true);
        if (!l) continue;
        // a slow drift up and back while the screen animates
        const float drift = env * 2.5f * std::sin(static_cast<float>(std::fmod(now / 2600.0, 1.0)) * 2 * kPi - i * 0.9f);
        const D2D1_POINT_2F c = framePoint(L, z.x, z.y - drift);
        d2dTarget_->DrawTextLayout({c.x - size / 2, c.y + P.dy - textH(l.Get()) / 2}, l.Get(), brush(pal_.accent, z.alpha));
    }
}

void Renderer::drawOptions(float top, float size, const D2D1_RECT_F& region) {
    const float s = dpi_ / 96.f;
    const float rw = region.right - region.left;
    const float box = std::round(size * 1.15f);
    const float gap = size * 0.6f, padX = size * 0.7f, rowH = std::round(size * 2.3f);
    const float maxLabel = std::max(rw - 32.f - 2 * padX - box - gap, 40.f);
    std::vector<ComPtr<IDWriteTextLayout>> labels;
    float groupW = 0;
    for (auto& o : options_) {
        auto l = layout(o.label, size, maxLabel, DWRITE_FONT_WEIGHT_NORMAL, false);
        if (l) l->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        groupW = std::max(groupW, std::min(textW(l.Get()), maxLabel));
        labels.push_back(l);
    }
    groupW += 2 * padX + box + gap;
    const float left = std::round(region.left + (rw - groupW) / 2);
    for (size_t i = 0; i < options_.size(); ++i) {
        const float y = top + rowH * i;
        D2D1_RECT_F row{left, y, left + groupW, y + rowH};
        const bool hot = hover_ == static_cast<int>(i);
        if (hot) d2dTarget_->FillRoundedRectangle({row, rowH / 2, rowH / 2}, brush(pal_.accent, 0.12f));
        const float bx = left + padX, by = std::round(y + (rowH - box) / 2);
        D2D1_ROUNDED_RECT b{{bx, by, bx + box, by + box}, box * 0.28f, box * 0.28f};
        if (options_[i].checked) {
            d2dTarget_->FillRoundedRectangle(b, brush(pal_.accent));
            ComPtr<ID2D1PathGeometry> g;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(d2d_->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&sink))) {
                sink->BeginFigure({bx + box * 0.26f, by + box * 0.53f}, D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddLine({bx + box * 0.43f, by + box * 0.70f});
                sink->AddLine({bx + box * 0.75f, by + box * 0.34f});
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                d2dTarget_->DrawGeometry(g.Get(), brush(pal_.ink), box * 0.13f, round_.Get());
            }
        } else {
            b.rect = {bx + 0.75f, by + 0.75f, bx + box - 0.75f, by + box - 0.75f};
            d2dTarget_->DrawRoundedRectangle(b, brush(hot ? pal_.accent : pal_.dim, hot ? 1.f : 0.85f), 1.5f);
        }
        if (auto& l = labels[i]) {
            const float tx = bx + box + gap, ty = y + (rowH - textH(l.Get())) / 2;
            d2dTarget_->DrawTextLayout({tx, ty}, l.Get(), brush(hot ? pal_.fg : pal_.dim));
        }
        optionRects_.push_back({static_cast<LONG>(std::floor(row.left * s)), static_cast<LONG>(std::floor(row.top * s)),
                                static_cast<LONG>(std::ceil(row.right * s)),
                                static_cast<LONG>(std::ceil(row.bottom * s))});
    }
}

void Renderer::drawScene(Scene scene, float opacity, double now) {
    if (opacity <= 0.003f) return;
    const bool layered = opacity < 0.997f && layer_;
    if (layered)
        d2dTarget_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                    D2D1::IdentityMatrix(), opacity),
                              layer_.Get());
    const Layout L = layoutFor();
    sceneOpacity_ = opacity;
    const bool paused = paused_ && scene != Scene::Connecting;
    const bool busy = scene == Scene::Connecting && !paused;
    const float env = paused ? 0.f : busy ? 1.f : ease((ambientUntil_ - now) / kAmbientSettleMs);
    const Pose P = mascotPose(L, scene, now);

    // Behind 投投: a soft accent glow around the phone, floating hearts.
    if (L.u > 0) {
        const auto& ph = toutou::kPhone[P.phonePose];
        const D2D1_POINT_2F gc =
            (D2D1::Matrix3x2F::Rotation(toutou::kTilt, {toutou::kPivotX, toutou::kPivotY}) * P.m).TransformPoint({ph.x, ph.y});
        const float breath = 0.5f - 0.5f * std::cos(static_cast<float>(std::fmod(now, 4200.0) / 4200) * 2 * kPi);
        const float r = 150 * L.u;
        glowBrush_->SetCenter(gc);
        glowBrush_->SetRadiusX(r);
        glowBrush_->SetRadiusY(r);
        glowBrush_->SetOpacity(paused ? 0.25f : busy ? 0.95f : 0.6f + 0.35f * env * breath);
        d2dTarget_->FillEllipse({gc, r, r}, glowBrush_.Get());
    }
    drawParticles(L, now, env);
    drawMascot(L, P, now);
    if (paused) drawZzz(L, P, now);
    drawMascotFx(L, P, now);

    // Text block.  A phone that starts connecting first gets the happy hop
    // while the idle text gives way to the spinner scene.
    const double intro = now - sceneAt_;
    if (busy && connectIntro_ && intro < kConnectIntroMs) {
        const float t = static_cast<float>(intro);
        drawTextBlock(Scene::Idle, L, 1 - ease((t - 450) / 250), now);
        drawTextBlock(Scene::Connecting, L, ease((t - 600) / 300), now);
    } else {
        drawTextBlock(scene, L, 1, now);
    }
    drawBubble(L, P, now);
    if (layered) d2dTarget_->PopLayer();
}

void Renderer::drawTextBlock(Scene scene, const Layout& L, float opacity, double now) {
    if (opacity <= 0.003f) return;
    const bool layered = opacity < 0.997f && layer_;
    if (layered)
        d2dTarget_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                    D2D1::IdentityMatrix(), opacity),
                              layer_.Get());
    const bool paused = paused_ && scene != Scene::Connecting;
    const bool busy = scene == Scene::Connecting && !paused;
    // Text block, centred in the free region.
    const float rw = L.text.right - L.text.left, rh = L.text.bottom - L.text.top;
    const float margin = std::max(16.f, rw * 0.06f);
    const float textMax = rw - 2 * margin;
    std::wstring t1, t2;
    if (paused) {
        t1 = tr(S::VidPausedTitle);
        t2 = tr(S::VidPausedHint);
    } else if (scene == Scene::Connecting) {
        t1 = tr(S::VidConnecting);  // + animated dots; the device name gets its own line
        if (!deviceName_.empty()) t2 = pm::i18n::fmt(S::VidDeviceQuoted, {deviceName_});
    } else if (hints_.empty()) {
        t1 = tr(S::VidWaitIphone);
        t2 = tr(S::VidWaitIphoneHint);
    } else {
        t1 = tr(S::VidWaitPhone);  // the hints name the platforms
    }
    const bool idle = scene == Scene::Idle && !paused;
    // Long titles (device names) shrink up to 25% to stay on one line, then wrap.
    float titleSize = L.title;
    ComPtr<IDWriteTextLayout> probe;
    if (auto f = format(titleSize, DWRITE_FONT_WEIGHT_NORMAL);
        f && SUCCEEDED(dwrite_->CreateTextLayout(t1.c_str(), static_cast<UINT32>(t1.size()), f.Get(), 1e5f, 1e4f,
                                                 &probe))) {
        DWRITE_TEXT_METRICS m{};
        if (SUCCEEDED(probe->GetMetrics(&m)) && m.widthIncludingTrailingWhitespace > textMax)
            titleSize = std::max(titleSize * 0.75f, titleSize * textMax / m.widthIncludingTrailingWhitespace);
    }
    auto l1 = layout(t1, titleSize, textMax, DWRITE_FONT_WEIGHT_NORMAL);
    // Device name: one line, trimmed with an ellipsis if it is very long.
    auto l2 = t2.empty()  ? nullptr
              : busy      ? layout(t2, std::max(L.hint, L.title * 0.72f), textMax, DWRITE_FONT_WEIGHT_NORMAL, false)
                          : layout(t2, L.hint, textMax);
    // Custom idle hints (setIdleHints): up to three lines, each wrapping.
    // A tab starts a muted suffix (smaller, fainter), e.g. 「（重新開機後可用）」.
    std::vector<ComPtr<IDWriteTextLayout>> hints;
    if (idle)
        for (size_t i = 0; i < hints_.size() && i < 3; ++i) {
            std::wstring text = hints_[i];
            // The tab becomes a space (English "Zizai Cast (after a restart)"
            // needs one; the full-width brackets in Chinese look fine either way)
            // or the line break when the suffix has to wrap.
            size_t tab = text.find(L'\t');
            if (tab != std::wstring::npos) text[tab] = L' ';
            const bool muted = tab != std::wstring::npos && tab + 1 < text.size() && mutedBrush_;
            auto make = [&]() {
                auto l = layout(text, L.hint, textMax);
                if (l && muted) {
                    const DWRITE_TEXT_RANGE r{static_cast<UINT32>(tab), static_cast<UINT32>(text.size() - tab)};
                    l->SetFontSize(std::round(L.hint * 0.86f * 4) / 4, r);
                    l->SetDrawingEffect(mutedBrush_.Get(), r);
                }
                return l;
            };
            auto l = make();
            if (!l) continue;
            // Too long for one line: the suffix moves to its own line as a
            // whole instead of breaking inside it.
            DWRITE_TEXT_METRICS m{};
            if (muted && tab > 0 && SUCCEEDED(l->GetMetrics(&m)) && m.lineCount > 1) {
                text[tab] = L'\n';
                if (!(l = make())) continue;
            }
            hints.push_back(l);
        }
    if (mutedBrush_) mutedBrush_->SetColor({pal_.dim.r, pal_.dim.g, pal_.dim.b, pal_.dim.a * 0.62f});
    const float hintGap = L.hint * 0.45f;
    const float dotsW = busy ? titleSize * 0.95f : 0;  // fixed slot: the title does not jitter
    // Check boxes and the help link only on a fully shown idle screen (they
    // are clickable).
    const bool full = opacity * sceneOpacity_ >= 0.997f;
    const bool showOpts = idle && !options_.empty() && full;
    // Actions (setIdleActions): pill buttons and links in centred rows.
    const float rowH = std::round(L.hint * 2.3f), linkRowH = std::round(L.hint * 2.2f);
    const float actGap = std::round(L.hint * 0.7f), actRowGap = std::round(L.hint * 0.45f);
    std::vector<ActionLayout> acts;
    std::vector<float> actRowW;
    const int actRows = idle && full ? layoutActions(acts, actRowW, L.hint * 0.95f, linkRowH, rw - margin, actGap) : 0;
    const float spin = busy ? L.title * 1.1f : 0, gapSpin = L.title * 0.8f;
    const float gap2 = L.title * 0.5f, gapOpts = L.hint * 1.5f;
    const float gapLink = showOpts ? L.hint * 0.5f : L.hint * 1.2f;
    float block = textH(l1.Get());
    if (busy) block += spin + gapSpin;
    if (l2) block += gap2 + textH(l2.Get());
    for (size_t i = 0; i < hints.size(); ++i) block += (i ? hintGap : gap2) + textH(hints[i].Get());
    if (showOpts) block += gapOpts + rowH * options_.size();
    if (actRows) block += gapLink + linkRowH * actRows + actRowGap * (actRows - 1);
    float y = std::round(L.text.top + std::max(margin * 0.75f, (rh - block) * (L.landscape ? 0.5f : 0.55f)));
    const float x = L.text.left + margin;
    if (busy) {
        drawSpinner({L.text.left + rw / 2, y + spin / 2}, spin / 2, now);
        y += spin + gapSpin;
    }
    if (l1) {
        const float dx = -dotsW / 2;
        d2dTarget_->DrawTextLayout({x + dx, y}, l1.Get(), brush(paused ? pal_.dim : pal_.fg));
        if (busy) {
            const float r = std::max(titleSize * 0.065f, 1.5f), step = titleSize * 0.28f;
            const float x0 = L.text.left + rw / 2 + dx + textW(l1.Get()) / 2 + titleSize * 0.22f + r;
            DWRITE_LINE_METRICS lm{};
            UINT32 lines = 0;
            l1->GetLineMetrics(&lm, 1, &lines);
            const float cy = y + (lines ? lm.baseline : textH(l1.Get()) * 0.8f) - r;
            for (int i = 0; i < 3; ++i) {
                const float ph = static_cast<float>(std::fmod(now / 1200.0 - i * 0.18, 1.0));
                const float a = 0.35f + 0.65f * std::pow(std::max(0.f, std::sin(ph * 2 * kPi)), 2.f);
                d2dTarget_->FillEllipse({{x0 + i * step, cy}, r, r}, brush(pal_.fg, a));
            }
        }
        y += textH(l1.Get());
    }
    if (l2) {
        y += gap2;
        d2dTarget_->DrawTextLayout({x, y}, l2.Get(), busy ? brush(pal_.accent) : brush(pal_.dim, paused ? 0.8f : 1.f));
        y += textH(l2.Get());
    }
    for (size_t i = 0; i < hints.size(); ++i) {
        y += i ? hintGap : gap2;
        d2dTarget_->DrawTextLayout({x, y}, hints[i].Get(), brush(pal_.dim));
        y += textH(hints[i].Get());
    }
    if (showOpts) {
        drawOptions(y + gapOpts, L.hint, L.text);
        y += gapOpts + rowH * options_.size();
    }
    if (actRows) drawActions(acts, actRowW, y + gapLink, linkRowH, actRowGap, actGap, L.text);
    if (layered) d2dTarget_->PopLayer();
}

int Renderer::layoutActions(std::vector<ActionLayout>& out, std::vector<float>& rowW, float size, float rowH,
                            float maxW, float gap) {
    for (auto& a : actions_) {
        if (a.label.empty()) continue;
        const float padX = std::round(rowH * (a.primary ? 0.62f : 0.42f));
        auto l = layout(a.label, size, std::max(maxW - 2 * padX, 20.f), DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
        if (!l) continue;
        if (!a.primary) l->SetUnderline(TRUE, {0, static_cast<UINT32>(a.label.size())});
        const float tw = std::min(textW(l.Get()), maxW - 2 * padX);
        out.push_back({l, std::round(tw + 2 * padX), a.primary, 0, static_cast<int>(&a - actions_.data())});
    }
    // Greedy rows: an action that does not fit next to the previous one
    // starts a new row.
    int row = 0;
    float x = 0;
    for (auto& a : out) {
        if (x > 0 && x + gap + a.w > maxW) {
            rowW.push_back(x);
            ++row;
            x = 0;
        }
        x += (x > 0 ? gap : 0) + a.w;
        a.row = row;
    }
    if (!out.empty()) rowW.push_back(x);
    return static_cast<int>(rowW.size());
}

// Pill button (primary): accent outline and text on a faint accent wash,
// filled with the accent on hover.  Link: underlined accent text, a soft
// pill on hover.
void Renderer::drawActions(const std::vector<ActionLayout>& acts, const std::vector<float>& rowW, float top,
                           float rowH, float rowGap, float gap, const D2D1_RECT_F& region) {
    const float s = dpi_ / 96.f;
    const float cx = (region.left + region.right) / 2;
    actionRects_.assign(actions_.size(), RECT{});
    int row = -1;
    float x = 0, y = top;
    for (size_t i = 0; i < acts.size(); ++i) {
        const auto& a = acts[i];
        if (a.row != row) {
            if (row >= 0) y += rowH + rowGap;
            row = a.row;
            x = std::round(cx - rowW[row] / 2);
        } else {
            x += gap;
        }
        const D2D1_RECT_F r{x, y, x + a.w, y + rowH};
        x += a.w;
        const bool hot = actionHot_ == a.index;
        IDWriteTextLayout* l = a.text.Get();
        const float tw = textW(l), th = textH(l);
        l->SetMaxWidth(std::ceil(std::min(tw, a.w)) + 2.f);  // exactly-measured width would trigger the ellipsis
        const D2D1_POINT_2F at{std::round((r.left + r.right) / 2 - std::min(tw, a.w) / 2) - 1.f,
                               std::round(r.top + (rowH - th) / 2)};
        const D2D1_ROUNDED_RECT pill{r, rowH / 2, rowH / 2};
        if (a.primary) {
            const D2D1_ROUNDED_RECT edge{{r.left + 0.75f, r.top + 0.75f, r.right - 0.75f, r.bottom - 0.75f},
                                         rowH / 2 - 0.75f, rowH / 2 - 0.75f};
            d2dTarget_->FillRoundedRectangle(pill, hot ? brush(pal_.accent) : brush(pal_.accent, 0.10f));
            d2dTarget_->DrawRoundedRectangle(edge, brush(pal_.accent, hot ? 1.f : 0.85f), 1.5f);
            d2dTarget_->DrawTextLayout(at, l, hot ? brush(pal_.ink) : brush(pal_.accent));
        } else {
            if (hot) d2dTarget_->FillRoundedRectangle(pill, brush(pal_.accent, 0.14f));
            d2dTarget_->DrawTextLayout(at, l, hot ? brush(pal_.fg) : brush(pal_.accent, 0.92f));
        }
        actionRects_[a.index] = {static_cast<LONG>(std::floor(r.left * s)), static_cast<LONG>(std::floor(r.top * s)),
                                static_cast<LONG>(std::ceil(r.right * s)), static_cast<LONG>(std::ceil(r.bottom * s))};
    }
}

void Renderer::drawPin(double now) {
    if (pin_.empty()) return;
    const float t = ease((now - pinAt_) / kPinFadeMs);
    const float a = pinVisible_ ? t : 1 - t;
    if (a <= 0.003f) return;
    const float s = dpi_ / 96.f;
    const float W = width_ / s, H = height_ / s;
    const D2D1_COLOR_F veil{pal_.bgBottom.r * 0.4f, pal_.bgBottom.g * 0.4f, pal_.bgBottom.b * 0.4f, 1};
    d2dTarget_->FillRectangle({0, 0, W, H}, brush(veil, 0.62f * a));

    const size_t n = std::min<size_t>(pin_.size(), 8);
    const float nf = static_cast<float>(n);
    float d = std::clamp(std::min(W * 0.15f, H * 0.11f), 36.f, 120.f);
    float gap = d * 0.22f, pad = d * 0.5f;
    const float maxCard = W - 24;
    if (nf * d + (nf - 1) * gap + 2 * pad > maxCard) {
        d = std::max((maxCard - 2 * pad) / (nf + (nf - 1) * 0.22f), 20.f);
        gap = d * 0.22f;
    }
    const float labelSize = std::clamp(d * 0.27f, 13.f, 28.f);
    auto label = layout(tr(S::VidPinLabel), labelSize, maxCard - 2 * pad);
    const float digitsW = nf * d + (nf - 1) * gap;
    const float cardW = std::min(std::max(digitsW, textW(label.Get())) + 2 * pad, maxCard);
    const float labelH = textH(label.Get());
    const float heart = labelSize * 0.9f;
    const float cardH = pad * 0.8f + heart + labelSize * 0.5f + labelH + d * 0.4f + d + pad;
    const float x0 = std::round((W - cardW) / 2), y0 = std::round((H - cardH) / 2);
    const float lift = pinVisible_ ? (1 - t) * 8 : 0;  // slight rise while fading in
    const D2D1_RECT_F card{x0, y0 + lift, x0 + cardW, y0 + cardH + lift};
    d2dTarget_->FillRoundedRectangle({card, 18, 18}, brush(pal_.card, a));
    d2dTarget_->DrawRoundedRectangle({card, 18, 18}, brush(pal_.accent, 0.28f * a), 1.25f);
    float y = card.top + pad * 0.8f;
    if (heart_) {
        d2dTarget_->SetTransform(D2D1::Matrix3x2F::Scale(heart, heart) *
                                 D2D1::Matrix3x2F::Translation(W / 2, y + heart / 2));
        d2dTarget_->FillGeometry(heart_.Get(), brush(pal_.accent, a));
        d2dTarget_->SetTransform(D2D1::IdentityMatrix());
    }
    y += heart + labelSize * 0.5f;
    if (label) {
        label->SetMaxWidth(cardW - 2 * pad);
        d2dTarget_->DrawTextLayout({card.left + pad, y}, label.Get(), brush(pal_.dim, a));
    }
    y += labelH + d * 0.4f;
    float x = std::round((W - digitsW) / 2);
    for (size_t i = 0; i < n; ++i, x += d + gap) {
        D2D1_ROUNDED_RECT cell{{x, y, x + d, y + d}, d * 0.22f, d * 0.22f};
        d2dTarget_->FillRoundedRectangle(cell, brush(pal_.well, a));
        d2dTarget_->DrawRoundedRectangle(cell, brush(pal_.accent, 0.55f * a), 1.5f);
        auto digit = layout(std::wstring(1, pin_[i]), d * 0.6f, d, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, true);
        if (!digit) continue;
        digit->SetMaxHeight(d);
        digit->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        d2dTarget_->DrawTextLayout({x, y}, digit.Get(), brush(pal_.fg, a));
    }
}

void Renderer::drawToast(double now) {
    if (toast_.empty()) return;
    const double e = now - toastAt_;
    float a;
    if (e < kToastInMs) a = ease(e / kToastInMs);
    else if (e < toastHold_) a = 1;
    else a = 1 - ease((e - toastHold_) / kToastOutMs);
    if (a <= 0.003f) return;
    const float s = dpi_ / 96.f;
    const float W = width_ / s, H = height_ / s;
    const float size = std::clamp(std::min(H / 24, W / 15) * 0.56f, 12.f, 24.f);
    const float padX = size * 1.2f, padY = size * 0.65f;
    const float maxW = W - 32 - 2 * padX;
    auto l = layout(toast_, size, maxW, DWRITE_FONT_WEIGHT_NORMAL, false);
    if (!l) return;
    // (Measured untrimmed: the trimmed layout never reports more than maxW.)
    if (auto full = layout(toast_, size, 100000.f, DWRITE_FONT_WEIGHT_NORMAL, false); full && textW(full.Get()) > maxW) {
        // Too long for one line: wrap (after the first 「。」 / ". " if there is one).
        std::wstring t = toast_;
        if (const size_t dot = t.find(L'。'); dot != std::wstring::npos && dot + 1 < t.size()) t.insert(dot + 1, L"\n");
        else if (const size_t en = t.find(L". "); en != std::wstring::npos && en > 8) t[en + 1] = L'\n';
        if (auto w = layout(t, size, maxW, DWRITE_FONT_WEIGHT_NORMAL, true)) l = w;
    }
    const float tw = std::min(textW(l.Get()), maxW), th = textH(l.Get());
    const float pillW = tw + 2 * padX, pillH = th + 2 * padY;
    const float x0 = std::round((W - pillW) / 2);
    const float y0 = std::round(H - std::max(20.f, H * 0.05f) - pillH + (1 - a) * 6);
    const float rad = std::min(pillH / 2, size * 1.25f);  // one line: a pill; more: a rounded card
    const D2D1_ROUNDED_RECT pill{{x0, y0, x0 + pillW, y0 + pillH}, rad, rad};
    d2dTarget_->FillRoundedRectangle(pill, brush(pal_.card, 0.96f * a));
    d2dTarget_->DrawRoundedRectangle(pill, brush(pal_.accent, 0.35f * a), 1.25f);
    l->SetMaxWidth(std::ceil(tw) + 2.f);  // slack: exactly-measured width triggers the ellipsis
    d2dTarget_->DrawTextLayout({x0 + padX - 1.f, y0 + padY}, l.Get(), brush(pal_.fg, a));
}

bool Renderer::render() {
    if (!swap_) return false;  // device lost and not re-created yet
    if (width_ == 0 || height_ == 0 || !rtv_) return true;
    const double now = clockMs();
    lastRender_ = now;
    if (scene_ == Scene::Connecting && !pinVisible_ && now - sceneAt_ > kConnectTimeoutMs) {
        scene_ = Scene::Idle;  // nothing arrived: don't spin forever
        sceneAt_ = now;
        connectIntro_ = false;
        poke();
    }
    const bool fadingIn = scene_ == Scene::Live && now - fadeInAt_ < kFadeMs;
    const bool fadingOut = scene_ != Scene::Live && havePicture_ && now - fadeOutAt_ < kFadeMs;
    const bool video = havePicture_ && !paused_ && (scene_ == Scene::Live || fadingOut);
    // Device re-created while live: black until the decoder delivers again
    // (the re-fed key frame, a few ms later).
    const bool blank = scene_ == Scene::Live && !havePicture_ && !paused_;
    if (!video && dimmed_) {  // the dim belongs to the picture only
        dimmed_ = false;
        dimFrom_ = 0;
        dimAt_ = -1e9;
    }
    FrameGeom fg{};
    const bool framed = video && frame_;
    if (video || blank) {
        // Device frame: the letterbox takes the theme's background colour.
        const D2D1_COLOR_F& b = pal_.bgBottom;
        const float bg[4] = {framed ? b.r : 0.f, framed ? b.g : 0.f, framed ? b.b : 0.f, 1};
        ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
        ctx_->ClearRenderTargetView(rtv_.Get(), bg);
    }
    D3D11_VIEWPORT vp{};
    picRect_ = {};
    if (video) {
        pictureViewport(vp, framed ? &fg : nullptr);
        if (vp.Width > 0 && vp.Height > 0) drawPicture(rtv_.Get(), vp);
        if (scene_ == Scene::Live && vp.Width > 0 && vp.Height > 0)
            picRect_ = {static_cast<LONG>(vp.TopLeftX), static_cast<LONG>(vp.TopLeftY),
                        static_cast<LONG>(vp.TopLeftX + vp.Width), static_cast<LONG>(vp.TopLeftY + vp.Height)};
    }
    optionRects_.clear();
    actionRects_.clear();
    const float dim = video ? dimLevel(now) : 0.f;
    const bool overlays = (!pin_.empty() && (pinVisible_ || now - pinAt_ < kPinFadeMs)) ||
                          (!toast_.empty() && now - toastAt_ < toastHold_ + kToastOutMs);
    const float toolA = toolbarAlpha(now);
    const bool need2D = (!video && !blank) || fadingIn || fadingOut || overlays || framed || dim > 0.003f ||
                        recording_ || toolA > 0.003f;
    toolRects_.clear();
    toolPill_ = {};
    recBadge_ = {};
    bool clickable = false;
    if (need2D && ensureD2D()) {
        const float s = dpi_ / 96.f;
        d2dTarget_->BeginDraw();
        d2dTarget_->SetTransform(D2D1::IdentityMatrix());
        if (blank) {
        } else if (!video) {
            drawBackground(1);
            drawScene(scene_, 1, now);
            clickable = mascotClickable(now);
        } else {
            if (framed) drawDeviceFrame(d2dTarget_.Get(), fg, 1 / s);
            if (dim > 0.003f) d2dTarget_->FillRectangle({0, 0, width_ / s, height_ / s}, brush(D2D1::ColorF(0, 0, 0), dim));
            if (fadingIn) {
                const float a = 1 - ease((now - fadeInAt_) / kFadeMs);
                drawBackground(a);
                drawScene(fadeFrom_, a, now);
            } else if (fadingOut) {
                const float a = ease((now - fadeOutAt_) / kFadeMs);
                drawBackground(a);
                drawScene(Scene::Idle, a, now);
            }
        }
        if (recording_) {
            if (video && vp.Width > 0) {
                const D2D1_RECT_F r{vp.TopLeftX / s, vp.TopLeftY / s, (vp.TopLeftX + vp.Width) / s,
                                    (vp.TopLeftY + vp.Height) / s};
                drawRecBadge(r, framed ? fg.radius / s : 0.f, now);
            } else {
                drawRecBadge({0, 0, width_ / s, height_ / s}, 0.f, now);
            }
        }
        if (toolA > 0.003f && scene_ == Scene::Live) {
            // Over the picture; without one (paused, re-created device) over the window.
            if (video && vp.Width > 0)
                drawToolbar({vp.TopLeftX / s, vp.TopLeftY / s, (vp.TopLeftX + vp.Width) / s, (vp.TopLeftY + vp.Height) / s},
                            framed ? &fg : nullptr, now);
            else
                drawToolbar({0, 0, width_ / s, height_ / s}, nullptr, now);
        }
        drawPin(now);
        drawToast(now);
        if (d2dTarget_->EndDraw() == D2DERR_RECREATE_TARGET) releaseD2D();
    }
    // Mascot hit area (client pixels, at rest) for hover / click.
    mascotRect_ = {};
    if (clickable) {
        const Layout L = layoutFor();
        mascotRect_ = {static_cast<LONG>(L.mascot.left * L.s), static_cast<LONG>(L.mascot.top * L.s),
                       static_cast<LONG>(L.mascot.right * L.s), static_cast<LONG>(L.mascot.bottom * L.s)};
    }
    if (capture_) copyBackBuffer();
    HRESULT hr = swap_->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        log("device lost on Present (hr=0x%08lx, reason=0x%08lx)", hr, dev_->GetDeviceRemovedReason());
        return false;
    }
    occluded_ = hr == DXGI_STATUS_OCCLUDED;
    return true;
}

bool Renderer::renderCapture(std::vector<uint8_t>& bgrx, UINT& w, UINT& h) {
    if (!swap_ || !width_ || !height_) return false;
    capture_ = &bgrx;
    captureW_ = captureH_ = 0;
    const bool ok = render();
    capture_ = nullptr;
    w = captureW_;
    h = captureH_;
    return ok && w && h;
}

void Renderer::copyBackBuffer() {
    ComPtr<ID3D11Texture2D> back, staging;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    D3D11_TEXTURE2D_DESC td{};
    back->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &staging))) return;
    ctx_->CopyResource(staging.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
    capture_->resize(static_cast<size_t>(td.Width) * td.Height * 4);
    for (UINT y = 0; y < td.Height; ++y)
        std::memcpy(capture_->data() + static_cast<size_t>(y) * td.Width * 4,
                    static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, td.Width * 4);
    ctx_->Unmap(staging.Get(), 0);
    captureW_ = td.Width;
    captureH_ = td.Height;
}

// ---------------------------------------------------------------------------
// Presentation: theme, dim, rotation / mirror, device frame, REC badge

const Renderer::Palette& Renderer::palette(int theme) { return kPalettes[std::clamp(theme, 0, 3)]; }

void Renderer::setTheme(int theme) {
    theme = std::clamp(theme, 0, 3);
    if (theme == theme_) return;
    theme_ = theme;
    pal_ = kPalettes[theme];
    bgBrush_.Reset();  // gradients carry the colours: rebuilt by ensureBrushes()
    glowBrush_.Reset();
    beamBrush_.Reset();
}

float Renderer::dimLevel(double now) const {
    const float t = ease((now - dimAt_) / kDimMs);
    const float target = dimmed_ ? kDimMax : 0.f;
    return dimFrom_ + (target - dimFrom_) * t;
}

void Renderer::setDimmed(bool d) {
    if (d == dimmed_) return;
    const double now = clockMs();
    dimFrom_ = dimLevel(now);
    dimmed_ = d;
    dimAt_ = now;
}

void Renderer::setTransform(int rot, bool mirror) {
    rot_ = rot & 3;
    mirror_ = mirror;
}

void Renderer::setRecording(bool on) {
    if (on == recording_) return;
    recording_ = on;
    if (on) recAt_ = clockMs();
}

void Renderer::displaySize(int& w, int& h) const {
    w = h = 0;
    if (!cur_.valid()) return;
    w = cur_.fmt.visibleWidth();
    h = cur_.fmt.visibleHeight();
    if (rot_ & 1) std::swap(w, h);
}

// Proportions of an iPhone 15-class phone relative to the screen's short side.
Renderer::FrameGeom Renderer::frameFor(float dw, float dh, float x0, float y0, float W, float H, bool fit) {
    constexpr float kBezel = 0.042f, kEdge = 0.012f, kRadius = 0.135f;
    const float mn = std::min(dw, dh);
    const float out = (kBezel + kEdge * 2.2f) * mn;  // outset at scale 1
    float sc = 1;
    if (fit) {
        const float m = std::max(6.f, 0.025f * std::min(W, H));
        sc = std::max(0.01f, std::min((W - 2 * m) / (dw + 2 * out), (H - 2 * m) / (dh + 2 * out)));
    }
    FrameGeom g{};
    const float sw = std::max(1.f, std::round(dw * sc)), sh = std::max(1.f, std::round(dh * sc));
    const float q = std::min(sw, sh);
    g.bezel = kBezel * q;
    g.edge = std::max(1.5f, kEdge * q);
    g.radius = kRadius * q;
    g.landscape = sw > sh;
    const float left = fit ? std::floor(x0 + (W - sw) / 2) : x0;
    const float top = fit ? std::floor(y0 + (H - sh) / 2) : y0;
    g.screen = {left, top, left + sw, top + sh};
    return g;
}

void Renderer::pictureViewport(D3D11_VIEWPORT& vp, FrameGeom* frame) const {
    vp = {};
    vp.MaxDepth = 1;
    int dw = 0, dh = 0;
    displaySize(dw, dh);
    if (dw <= 0 || dh <= 0) return;
    if (frame) {
        *frame = frameFor(static_cast<float>(dw), static_cast<float>(dh), 0, 0, static_cast<float>(width_),
                          static_cast<float>(height_), true);
        vp.TopLeftX = frame->screen.left;
        vp.TopLeftY = frame->screen.top;
        vp.Width = frame->screen.right - frame->screen.left;
        vp.Height = frame->screen.bottom - frame->screen.top;
        return;
    }
    // Letterbox: fit the visible picture into the client area.
    const float scale = std::min(width_ / static_cast<float>(dw), height_ / static_cast<float>(dh));
    vp.Width = std::round(dw * scale);
    vp.Height = std::round(dh * scale);
    vp.TopLeftX = std::floor((width_ - vp.Width) / 2);
    vp.TopLeftY = std::floor((height_ - vp.Height) / 2);
}

// Bezel, outer edge, side buttons and Dynamic Island around g.screen (pixel
// coordinates; pxToDip maps them to the target's DIPs).  Drawn in the local
// frame of an upright phone, rotated a quarter turn counter-clockwise for a
// landscape picture (island on the left).
void Renderer::drawDeviceFrame(ID2D1RenderTarget* rt, const FrameGeom& g, float pxToDip) {
    if (!d2d_) return;
    const float sw = g.screen.right - g.screen.left, sh = g.screen.bottom - g.screen.top;
    const float pw = std::min(sw, sh), ph = std::max(sw, sh);
    const float cx = (g.screen.left + g.screen.right) / 2, cy = (g.screen.top + g.screen.bottom) / 2;
    D2D1_MATRIX_3X2_F saved;
    rt->GetTransform(&saved);
    rt->SetTransform((g.landscape ? D2D1::Matrix3x2F::Rotation(-90) : D2D1::Matrix3x2F::Identity()) *
                     D2D1::Matrix3x2F::Translation(cx, cy) * D2D1::Matrix3x2F::Scale(pxToDip, pxToDip));
    const float R = g.radius, b = g.bezel, e = g.edge;
    const D2D1_RECT_F scr{-pw / 2, -ph / 2, pw / 2, ph / 2};
    auto inflate = [](D2D1_RECT_F r, float d) { return D2D1_RECT_F{r.left - d, r.top - d, r.right + d, r.bottom + d}; };

    // Titanium edge colour, faintly tinted with the theme accent.
    auto mix = [](D2D1_COLOR_F a, D2D1_COLOR_F c, float t) {
        return D2D1_COLOR_F{a.r + (c.r - a.r) * t, a.g + (c.g - a.g) * t, a.b + (c.b - a.b) * t, 1};
    };
    const D2D1_COLOR_F light = mix(D2D1::ColorF(0xA9ACB3), pal_.accent, 0.18f);
    const D2D1_COLOR_F dark = mix(D2D1::ColorF(0x45474D), pal_.accent, 0.10f);
    ComPtr<ID2D1GradientStopCollection> stops;
    ComPtr<ID2D1LinearGradientBrush> metal;
    const D2D1_GRADIENT_STOP gs[4] = {{0, light}, {0.35f, dark}, {0.65f, dark}, {1, light}};
    rt->CreateGradientStopCollection(gs, 4, &stops);
    if (stops)
        rt->CreateLinearGradientBrush({{-pw / 2 - b - e, -ph / 2 - b - e}, {pw / 2 + b + e, ph / 2 + b + e}}, stops.Get(),
                                      &metal);
    ComPtr<ID2D1SolidColorBrush> solid;
    rt->CreateSolidColorBrush(D2D1::ColorF(0x0A0A0C), &solid);
    if (!metal || !solid) {
        rt->SetTransform(saved);
        return;
    }

    // Side buttons (behind the edge): action + volume on the left, power right.
    const float bx = pw / 2 + b + e;  // outer edge of the frame
    auto button = [&](float side, float y, float len) {
        const float x0 = side < 0 ? -bx - e * 1.1f : bx - e * 0.6f;
        const D2D1_ROUNDED_RECT r{{x0, -ph / 2 + y * ph, x0 + e * 1.7f, -ph / 2 + (y + len) * ph}, e * 0.8f, e * 0.8f};
        rt->FillRoundedRectangle(r, metal.Get());
    };
    button(-1, 0.165f, 0.040f);
    button(-1, 0.235f, 0.068f);
    button(-1, 0.318f, 0.068f);
    button(1, 0.255f, 0.105f);

    // Rings: metal edge (outer minus body), black body (body minus the
    // rounded screen: rounds the picture's corners), then a highlight.
    auto fillRing = [&](const D2D1_ROUNDED_RECT& a, const D2D1_ROUNDED_RECT& cut, ID2D1Brush* br) {
        ComPtr<ID2D1RoundedRectangleGeometry> ga, gc;
        ComPtr<ID2D1PathGeometry> ring;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(d2d_->CreateRoundedRectangleGeometry(a, &ga)) &&
            SUCCEEDED(d2d_->CreateRoundedRectangleGeometry(cut, &gc)) && SUCCEEDED(d2d_->CreatePathGeometry(&ring)) &&
            SUCCEEDED(ring->Open(&sink))) {
            ga->CombineWithGeometry(gc.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get());
            sink->Close();
            rt->FillGeometry(ring.Get(), br);
        }
    };
    const D2D1_ROUNDED_RECT outerR{inflate(scr, b + e), R + b + e, R + b + e};
    const D2D1_ROUNDED_RECT bodyR{inflate(scr, b), R + b, R + b};
    const D2D1_ROUNDED_RECT screenR{scr, R, R};
    fillRing(outerR, {inflate(scr, b - 0.5f), R + b - 0.5f, R + b - 0.5f}, metal.Get());  // overlaps the body: no seam
    solid->SetColor(D2D1::ColorF(0x0A0A0C));
    fillRing(bodyR, screenR, solid.Get());
    solid->SetColor(D2D1::ColorF(1, 1, 1, 0.22f));
    rt->DrawRoundedRectangle({inflate(scr, b + e - 0.5f), R + b + e - 0.5f, R + b + e - 0.5f}, solid.Get(), 1.f);
    // Dynamic Island with a hint of the camera lens.
    const float iw = pw * 0.31f, ih = pw * 0.092f, it = -ph / 2 + pw * 0.030f;
    solid->SetColor(D2D1::ColorF(0x000000));
    rt->FillRoundedRectangle({{-iw / 2, it, iw / 2, it + ih}, ih / 2, ih / 2}, solid.Get());
    const D2D1_POINT_2F lens{iw / 2 - ih / 2, it + ih / 2};
    solid->SetColor(D2D1::ColorF(0x10162A));
    rt->FillEllipse({lens, ih * 0.27f, ih * 0.27f}, solid.Get());
    solid->SetColor(D2D1::ColorF(0x3A4A78, 0.55f));
    rt->FillEllipse({{lens.x - ih * 0.07f, lens.y - ih * 0.07f}, ih * 0.07f, ih * 0.07f}, solid.Get());
    rt->SetTransform(saved);
}

void Renderer::drawRecBadge(const D2D1_RECT_F& r, float radius, double now) {
    const float w = r.right - r.left, h = r.bottom - r.top;
    const float size = std::clamp(std::min(w, h) / 26.f, 12.f, 20.f);
    const long long secs = static_cast<long long>((now - recAt_) / 1000);
    wchar_t text[32];
    if (secs >= 3600)
        swprintf_s(text, L"%lld:%02lld:%02lld", secs / 3600, secs / 60 % 60, secs % 60);
    else
        swprintf_s(text, L"%02lld:%02lld", secs / 60, secs % 60);
    auto l = layout(text, size, 400, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, true);
    if (!l) return;
    const float tw = textW(l.Get()), th = textH(l.Get());
    const float dot = size * 0.34f, padX = size * 0.62f, padY = size * 0.30f, gap = size * 0.42f;
    const float pillW = padX + 2 * dot + gap + tw + padX, pillH = th + 2 * padY;
    // Inside the rounded screen corner when framed.
    const float inset = std::max(size * 0.7f, radius * 0.42f);
    const float x0 = std::round(r.left + inset), y0 = std::round(r.top + std::max(size * 0.7f, radius * 0.30f));
    const D2D1_ROUNDED_RECT pill{{x0, y0, x0 + pillW, y0 + pillH}, pillH / 2, pillH / 2};
    recBadge_ = pill.rect;
    d2dTarget_->FillRoundedRectangle(pill, brush(pal_.card, 0.86f));
    d2dTarget_->DrawRoundedRectangle(pill, brush(pal_.accent, 0.40f), 1.f);
    const float ph = static_cast<float>(std::fmod((now - recAt_) / 1400.0, 1.0));
    const float pulse = 0.5f + 0.5f * std::cos(ph * 2 * kPi);
    const D2D1_POINT_2F c{x0 + padX + dot, y0 + pillH / 2};
    d2dTarget_->FillEllipse({c, dot * (1.5f + 0.5f * (1 - pulse)), dot * (1.5f + 0.5f * (1 - pulse))},
                            brush(kRecRed, 0.22f * pulse));
    d2dTarget_->FillEllipse({c, dot, dot}, brush(kRecRed, 0.55f + 0.45f * pulse));
    l->SetMaxWidth(std::ceil(tw) + 2.f);
    d2dTarget_->DrawTextLayout({c.x + dot + gap - 1.f, y0 + padY}, l.Get(), brush(pal_.fg));
}

// ---------------------------------------------------------------------------
// Frame tap: crop + (10 -> 8 bit) on the GPU into R8 / R8G8 targets, copied
// into a ring of staging textures and mapped one picture later (the GPU has
// long finished by then: copyIn() waits for the next picture's decode).

bool Renderer::ensureTap(UINT w, UINT h) {
    if (tapY_ && tapW_ == w && tapH_ == h) return true;
    tapY_.Reset();
    tapUV_.Reset();
    tapYRtv_.Reset();
    tapUVRtv_.Reset();
    tapW_ = tapH_ = 0;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &tapY_)) ||
        FAILED(dev_->CreateRenderTargetView(tapY_.Get(), nullptr, &tapYRtv_)))
        return false;
    td.Width = w / 2;
    td.Height = h / 2;
    td.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &tapUV_)) ||
        FAILED(dev_->CreateRenderTargetView(tapUV_.Get(), nullptr, &tapUVRtv_)))
        return false;
    tapW_ = w;
    tapH_ = h;
    return true;
}

bool Renderer::tapSubmit(const Picture* pic, uint64_t ptsNs) {
    const Picture& p = pic ? *pic : cur_;
    if (!dev_ || !p.valid() || !psTapY_ || !psTapUV_ || !tapCb_ || (p.bgra && (!psTapRgbY_ || !psTapRgbUV_)))
        return false;
    const double t0 = clockMs();
    const UINT w = static_cast<UINT>(p.fmt.visibleWidth()) & ~1u, h = static_cast<UINT>(p.fmt.visibleHeight()) & ~1u;
    if (w < 2 || h < 2) return true;
    if (!ensureTap(w, h)) return false;
    // A free ring slot (the oldest pending one is dropped if none: only if
    // the caller never delivers).
    TapSlot* slot = nullptr;
    for (auto& t : tap_)
        if (!t.pending && (!slot || t.seq < slot->seq)) slot = &t;
    if (!slot) {
        for (auto& t : tap_)
            if (!slot || t.seq < slot->seq) slot = &t;
        slot->pending = false;
    }
    if (!slot->y || slot->w != w || slot->h != h) {
        slot->y.Reset();
        slot->uv.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &slot->y))) return false;
        td.Width = w / 2;
        td.Height = h / 2;
        td.Format = DXGI_FORMAT_R8G8_UNORM;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &slot->uv))) return false;
        slot->w = w;
        slot->h = h;
    }
    const int off[4] = {p.fmt.crop.left, p.fmt.crop.top, p.fmt.crop.left / 2, p.fmt.crop.top / 2};
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx_->Map(tapCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, off, sizeof(off));
        ctx_->Unmap(tapCb_.Get(), 0);
    }
    ID3D11ShaderResourceView* srvs[3] = {p.bgra ? nullptr : p.ySrv.Get(), p.uvSrv.Get(),
                                         p.bgra ? p.ySrv.Get() : nullptr};
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ctx_->PSSetConstantBuffers(1, 1, tapCb_.GetAddressOf());
    ctx_->PSSetShaderResources(0, 3, srvs);
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    ctx_->OMSetRenderTargets(1, tapYRtv_.GetAddressOf(), nullptr);
    ctx_->RSSetViewports(1, &vp);
    ctx_->PSSetShader(p.bgra ? psTapRgbY_.Get() : psTapY_.Get(), nullptr, 0);
    ctx_->Draw(4, 0);
    vp.Width = static_cast<float>(w / 2);
    vp.Height = static_cast<float>(h / 2);
    ctx_->OMSetRenderTargets(1, tapUVRtv_.GetAddressOf(), nullptr);
    ctx_->RSSetViewports(1, &vp);
    ctx_->PSSetShader(p.bgra ? psTapRgbUV_.Get() : psTapUV_.Get(), nullptr, 0);
    ctx_->Draw(4, 0);
    ID3D11ShaderResourceView* nulls[3] = {};
    ctx_->PSSetShaderResources(0, 3, nulls);
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    ctx_->CopyResource(slot->y.Get(), tapY_.Get());
    ctx_->CopyResource(slot->uv.Get(), tapUV_.Get());
    ctx_->Flush();
    slot->pending = true;
    slot->pts = ptsNs;
    slot->at = clockMs();
    slot->seq = ++tapSeq_;
    tapCost_ += slot->at - t0;
    return true;
}

void Renderer::tapDeliver(const TapFn& fn, bool flushAll) {
    while (true) {
        TapSlot* slot = nullptr;
        for (auto& t : tap_)
            if (t.pending && (!slot || t.seq < slot->seq)) slot = &t;
        if (!slot || (!flushAll && slot->seq == tapSeq_)) return;
        slot->pending = false;
        if (!fn || !ctx_) continue;
        const double t0 = clockMs();
        D3D11_MAPPED_SUBRESOURCE my{}, muv{};
        if (FAILED(ctx_->Map(slot->y.Get(), 0, D3D11_MAP_READ, 0, &my))) continue;
        if (FAILED(ctx_->Map(slot->uv.Get(), 0, D3D11_MAP_READ, 0, &muv))) {
            ctx_->Unmap(slot->y.Get(), 0);
            continue;
        }
        const UINT w = slot->w, h = slot->h;
        tapBuf_.resize(static_cast<size_t>(w) * h * 3 / 2);
        uint8_t* dst = tapBuf_.data();
        for (UINT y = 0; y < h; ++y, dst += w)
            std::memcpy(dst, static_cast<const uint8_t*>(my.pData) + static_cast<size_t>(y) * my.RowPitch, w);
        for (UINT y = 0; y < h / 2; ++y, dst += w)
            std::memcpy(dst, static_cast<const uint8_t*>(muv.pData) + static_cast<size_t>(y) * muv.RowPitch, w);
        ctx_->Unmap(slot->uv.Get(), 0);
        ctx_->Unmap(slot->y.Get(), 0);
        tapCost_ += clockMs() - t0;
        fn(tapBuf_.data(), static_cast<int>(w), static_cast<int>(h), static_cast<int>(w), slot->pts);
    }
}

double Renderer::tapFlushInMs() const {
    for (const auto& t : tap_)
        if (t.pending && t.seq == tapSeq_) return std::max(0.0, t.at + kTapFlushMs - clockMs());
    return -1;
}

void Renderer::tapReset() {
    for (auto& t : tap_) t.pending = false;
}

}  // namespace pm::video
