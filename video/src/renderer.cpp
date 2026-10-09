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
// Waiting <-> picture cross-fade with a settle: in, the scene fades over
// 300 ms (standard) while lifting 8 px and the picture grows 98.5 -> 100 %
// over 380 ms (outCubic); out, the picture shrinks to 98.8 % (280 ms), the
// scene fades in over 420 ms (outCubic) and 投投 drops in 10 px (520 ms).
constexpr double kFadeInMs = 380, kSceneOutMs = 300, kFadeOutMs = 420, kDropMs = 520;
constexpr double kPinFadeMs = 160;
constexpr double kToastInMs = 220, kToastHoldMs = 2500, kToastOutMs = 260;  // in: outCubic + 8 px rise
constexpr double kAmbientSettleMs = 1500;  // breathing amplitude eases out
constexpr double kConnectTimeoutMs = 60000;
constexpr double kFastFrameMs = 1000.0 / 60, kSlowFrameMs = 1000.0 / 30;

constexpr double kDimMs = 200;
constexpr float kDimMax = 0.55f;
constexpr double kHoverMs = 160;
constexpr double kReactMs = 1250;      // click reaction (hop, hearts, bubble)
constexpr double kHopMs = 1000;        // squash-and-stretch hop
constexpr double kConnectIntroMs = 1150;  // surprise + happy hop before the spinner scene
constexpr double kSurpriseMs = 260;       // 「found a phone」: surprised face before the hop
constexpr double kTapFlushMs = 20;     // newest tapped picture delivered at the latest after this
// Live toolbar: shown on mouse movement, hidden kToolHoldMs after the last one.
constexpr double kToolHoldMs = 2000, kToolInMs = 160, kToolOutMs = 260;  // in: outCubic
constexpr double kTipDelayMs = 450, kTipFadeMs = 120;
// Magnifier: the big zoom indicator stays this long after a change, then fades.
constexpr double kZoomShowMs = 1200, kZoomFadeMs = 400;
constexpr double kFlashInMs = 40, kFlashOutMs = 200;  // screenshot veil

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
float outCubic(double t) {  // 1 - (1 - t)^3: enters fast, settles softly
    const float x = 1 - static_cast<float>(std::clamp(t, 0.0, 1.0));
    return 1 - x * x * x;
}
float inCubic(double t) {
    const float x = static_cast<float>(std::clamp(t, 0.0, 1.0));
    return x * x * x;
}
// CSS cubic-bezier(x1, y1, x2, y2) at time t (Newton on x).
float bezier(double t, float x1, float y1, float x2, float y2) {
    const float x = static_cast<float>(std::clamp(t, 0.0, 1.0));
    float u = x;
    for (int i = 0; i < 6; ++i) {
        const float v = 1 - u;
        const float fx = 3 * v * v * u * x1 + 3 * v * u * u * x2 + u * u * u - x;
        const float dx = 3 * v * v * x1 + 6 * v * u * (x2 - x1) + 3 * u * u * (1 - x2);
        if (std::fabs(dx) < 1e-5f) break;
        u = std::clamp(u - fx / dx, 0.f, 1.f);
    }
    const float v = 1 - u;
    return 3 * v * v * u * y1 + 3 * v * u * u * y2 + u * u * u;
}
float standard(double t) { return bezier(t, 0.4f, 0, 0.2f, 1); }   // material standard
float softOut(double t) { return bezier(t, 0.22f, 1, 0.36f, 1); }  // gentle ease-out, no overshoot
D2D1_COLOR_F mixc(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}
D2D1_COLOR_F alphaOf(D2D1_COLOR_F c, float a) {
    c.a *= a;
    return c;
}
D2D1_RECT_F inflate(D2D1_RECT_F r, float d) { return {r.left - d, r.top - d, r.right + d, r.bottom + d}; }
// Scale about a point, composed with the target's current transform.
D2D1::Matrix3x2F scaledAbout(const D2D1_MATRIX_3X2_F& base, float k, D2D1_POINT_2F c) {
    return D2D1::Matrix3x2F::Scale(k, k, c) * *D2D1::Matrix3x2F::ReinterpretBaseType(&base);
}

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

struct Constants {
    float uvRect[4];
    float range[4];
    float mat[4];
    float xfU[4];
    float xfV[4];
    float view[4];
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
        // Bounded: a GPU that never finishes (hang without a TDR) must not
        // freeze the render thread; the watchdog sees no picture then.
        const double t0 = clockMs();
        while (ctx_->GetData(evt_.Get(), nullptr, 0, 0) == S_FALSE) {
            if (clockMs() - t0 > kGpuWaitMaxMs) {
                if (gpuWaitTimeouts_++ % 50 == 0)
                    log("GPU did not finish a picture copy within %.0f ms (%lld times)", kGpuWaitMaxMs, gpuWaitTimeouts_);
                return false;
            }
            SwitchToThread();
        }
    }
    p.fmt = fmt;
    return true;
}

void Renderer::pictureShown() {
    havePicture_ = true;
    lastPictureAt_ = clockMs();
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
    // The stream ended: nothing to freeze or translate any more (zoom and
    // filter are the user's viewing preference: they stay).
    frozenOn_ = false;
    frozen_ = {};
    frozenCpu_ = {};
    frozenWasUp_ = false;
    boxes_.clear();
    ovHits_ = {};
    ovValid_ = false;
    busy_.clear();
    selecting_ = false;
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
            // Surprised for a moment (eyes to the phone, sparkles at its top),
            // then the happy hop + hearts.
            surpriseAt_ = now;
            reactAt_ = now + kSurpriseMs;
            reactBubble_ = false;
        }
    }
    scene_ = Scene::Connecting;
    sceneAt_ = now;
}

void Renderer::setOptions(std::vector<Option> options) {
    // Same check boxes, one changed: its tick draws itself (or is undrawn).
    const bool same = options.size() == options_.size() &&
                      std::equal(options.begin(), options.end(), options_.begin(),
                                 [](const Option& a, const Option& b) { return a.label == b.label; });
    optAt_.resize(options.size(), -1e9);
    if (same) {
        const double now = clockMs();
        for (size_t i = 0; i < options.size(); ++i)
            if (options[i].checked != options_[i].checked) optAt_[i] = now;
    } else {
        std::fill(optAt_.begin(), optAt_.end(), -1e9);
    }
    options_ = std::move(options);
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
    // Long enough to read (0.7.2; was 2.5 s or the caller's fixed time, so a
    // two-line message with the next step was gone half read): about 130 ms
    // per CJK / kana / Hangul character and 55 ms per other one, 2.5-8 s; a
    // caller's longer time (a sticky 「正在傳送…」) still wins.
    double read = 1200;
    for (wchar_t c : text) read += c >= 0x2E80 ? 130 : 55;
    read = std::clamp(read, kToastHoldMs, 8000.0);
    toastHold_ = holdMs > 0 ? std::max(holdMs, read) : read;
}

// ---------------------------------------------------------------------------
// Hover / press / focus levels.

float Renderer::fadeLevel(const Fade& f, double now) const {
    const double e = now - f.at;
    if (f.on) return f.from + (1 - f.from) * outCubic(e / f.inMs);
    return f.from * (1 - (f.softOff ? softOut(e / f.outMs) : ease(e / f.outMs)));
}

void Renderer::fadeTo(Fade& f, bool on, double now, double inMs, double outMs, bool softOff) {
    if (on == f.on) return;
    f.from = fadeLevel(f, now);
    f.on = on;
    f.at = now;
    f.inMs = std::max(1.0, ms(inMs));
    f.outMs = std::max(1.0, ms(outMs));
    f.softOff = softOff;
}

void Renderer::hoverTo(int kind, int index) {
    if (kind < 0 || kind >= kUiKinds) return;
    const double now = clockMs();
    for (int i = 0; i < kUiMax; ++i) fadeTo(uiHot_[kind][i], i == index, now, 120, 180);
}

void Renderer::setPressed(int kind, int index) {
    const double now = clockMs();
    for (int k = 0; k < kUiKinds; ++k)
        for (int i = 0; i < kUiMax; ++i) fadeTo(uiPress_[k][i], k == kind && i == index, now, 70, 120, true);
}

void Renderer::setFocus(int kind, int index) {
    const double now = clockMs();
    for (int k = 0; k < kUiKinds; ++k)
        for (int i = 0; i < kUiMax; ++i) fadeTo(uiFocus_[k][i], k == kind && i == index, now, 120, 120);
}

bool Renderer::uiAnimating(double now) const {
    for (const auto* set : {&uiHot_, &uiPress_, &uiFocus_})
        for (const auto& kind : *set)
            for (const Fade& f : kind)
                if (now - f.at < (f.on ? f.inMs : f.outMs) + 40) return true;
    return false;
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
    const float in = outCubic((now - toolInAt_) / ms(kToolInMs));
    if (toolInside_ || now < toolUntil_) return in;
    return std::min(in, 1 - ease((now - toolUntil_) / ms(kToolOutMs)));
}

bool Renderer::toolbarActivity() {
    if (!toolbarAvailable()) return false;
    const double now = clockMs();
    const float a = toolbarAlpha(now);
    if (a < 1) toolInAt_ = now - ms(kToolInMs) * (1 - std::cbrt(1 - a));  // (re)appear from the current opacity
    toolUntil_ = std::max(toolUntil_, now + kToolHoldMs);
    return a < 1;  // needs a frame (fade in)
}

void Renderer::toolbarLeave() {
    const double now = clockMs();
    toolInside_ = false;
    toolHot_ = -1;
    hoverTo(UiTool, -1);
    if (toolUntil_ > now) toolUntil_ = now;
}

void Renderer::setToolbarHover(int index, bool inside) {
    const double now = clockMs();
    hoverTo(UiTool, index);
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
    // Buttons shown: all, or (window too narrow for every one even at the
    // smallest size) without the optional ones, from the right (0.7.2: an
    // Android phone's 14 buttons ran off a phone-sized window; those
    // commands are also in 更多).  A hidden group start passes its divider on.
    // A slider (volume) is kSliderUnits buttons wide and goes only after every
    // optional button.
    constexpr float kSliderUnits = 72.f / 36.f;
    auto units = [&](int i) { return toolItems_[i].slider >= 0 ? kSliderUnits : 1.f; };
    std::vector<char> vis(n, 1), grp(n, 0);
    float shownU = 0;  // shown width in buttons
    auto count = [&](int& shown, int& groups) {
        shown = groups = 0;
        shownU = 0;
        bool pending = false;
        for (int i = 0; i < n; ++i) {
            pending |= toolItems_[i].group;
            grp[i] = 0;
            if (!vis[i]) continue;
            if (shown > 0 && pending) grp[i] = 1, ++groups;
            pending = false;
            ++shown;
            shownU += units(i);
        }
    };
    int shownN = 0, groups = 0;
    count(shownN, groups);
    auto widthAt = [&](float k) { return k * (10 + shownU * 36 + std::max(0, shownN - 1) * 2 + groups * 11); };
    // An optional button right before a slider (its speaker) goes last of all.
    auto beforeSlider = [&](int i) { return i + 1 < n && toolItems_[i + 1].slider >= 0; };
    for (int i = n - 1; i >= 0 && widthAt(26.f / 36.f) > W - 8; --i)
        if (toolItems_[i].optional && toolItems_[i].slider < 0 && !beforeSlider(i)) vis[i] = 0, count(shownN, groups);
    for (int i = n - 1; i >= 0 && widthAt(26.f / 36.f) > W - 8; --i)
        if (toolItems_[i].slider >= 0) vis[i] = 0, count(shownN, groups);
    for (int i = n - 1; i >= 0 && widthAt(26.f / 36.f) > W - 8; --i)
        if (toolItems_[i].optional && beforeSlider(i)) vis[i] = 0, count(shownN, groups);
    // Natural size, shrunk (down to 26 DIP buttons) to fit the picture.
    float btn = 36, gap = 2, div = 11, pad = 5;
    const float natural = 2 * pad + shownU * btn + std::max(0, shownN - 1) * gap + groups * div;
    const float avail = std::max(120.f, std::min(areaW, W) - 16);
    if (natural > avail) {
        const float k = std::max(26.f / 36.f, avail / natural);
        btn *= k;
        gap *= k;
        div *= k;
        pad *= k;
    }
    const float pillW = 2 * pad + shownU * btn + std::max(0, shownN - 1) * gap + groups * div, pillH = btn + 2 * pad;
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
    const float y0 = std::round(top - (reduced_ ? 0 : (1 - a) * 6));  // slides down a little while fading in
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
        if (!vis[i]) {  // keeps toolRects_ index == item index
            if (interactive) toolRects_.push_back({});
            continue;
        }
        if (grp[i]) {
            const float cx = std::round(x - gap / 2 + div / 2) + 0.5f;
            d2dTarget_->DrawLine({cx, pill.top + pillH * 0.30f}, {cx, pill.bottom - pillH * 0.30f},
                                 brush(pal_.fg, 0.20f * a), 1.f);
            x += div;
        }
        if (it.slider >= 0) {
            // Track (inset kToolSliderInset x height on each side), filled up to
            // the knob in the accent (greyed when toggled = muted), knob.
            const float sw = btn * kSliderUnits;
            const D2D1_RECT_F b{x, pill.top + pad, x + sw, pill.top + pad + btn};
            const bool hot = i == toolHot_;
            const float hl = uiLevel(uiHot_, UiTool, i, now);  // the knob grows a little on hover
            const float inset = btn * kToolSliderInset, cy = (b.top + b.bottom) / 2;
            const float x0t = b.left + inset, x1t = b.right - inset;
            const float v = std::clamp(it.slider, 0.f, 1.f), kx = x0t + (x1t - x0t) * v;
            const float th = std::max(3.f, btn * 0.11f);
            const D2D1_COLOR_F fill = it.toggled ? pal_.fg : pal_.accent;
            const float fillA = it.toggled ? 0.35f : 0.95f;
            d2dTarget_->FillRoundedRectangle({{x0t, cy - th / 2, x1t, cy + th / 2}, th / 2, th / 2},
                                             brush(pal_.fg, 0.20f * a));
            if (kx > x0t + 0.5f)
                d2dTarget_->FillRoundedRectangle({{x0t, cy - th / 2, kx, cy + th / 2}, th / 2, th / 2},
                                                 brush(fill, fillA * a));
            const float kr = btn * (0.17f + 0.026f * hl);
            d2dTarget_->FillEllipse({{kx, cy}, kr, kr}, brush(fill, (it.toggled ? 0.6f : 1.f) * a));
            d2dTarget_->DrawEllipse({{kx, cy}, kr - 0.5f, kr - 0.5f}, brush(pal_.card, 0.9f * a), 1.f);
            if (hot) hotRect = b;
            if (interactive)
                toolRects_.push_back({static_cast<LONG>(std::floor(b.left * s)), static_cast<LONG>(std::floor(b.top * s)),
                                      static_cast<LONG>(std::ceil(b.right * s)), static_cast<LONG>(std::ceil(b.bottom * s))});
            x += sw + gap;
            continue;
        }
        const D2D1_RECT_F b{x, pill.top + pad, x + btn, pill.top + pad + btn};
        const D2D1_ELLIPSE disc{{(b.left + b.right) / 2, (b.top + b.bottom) / 2}, btn / 2, btn / 2};
        const bool hot = i == toolHot_;
        // Hover eases the wash in (120 ms) and out (180 ms); a press shrinks the button to 92 %.
        const float hl = uiLevel(uiHot_, UiTool, i, now), pl = reduced_ ? 0.f : uiLevel(uiPress_, UiTool, i, now);
        D2D1_MATRIX_3X2_F base;
        d2dTarget_->GetTransform(&base);
        if (pl > 0.003f) d2dTarget_->SetTransform(scaledAbout(base, 1 - 0.08f * pl, disc.point));
        D2D1_COLOR_F ink = it.danger ? kDangerRed : pal_.fg;
        if (it.toggled && it.recording) {
            d2dTarget_->FillEllipse(disc, brush(kRecRed, (0.24f + 0.12f * hl) * a));
            ink = kRecRed;
        } else if (it.toggled) {  // 放大鏡 / 翻譯 / 凍結 on: the theme's accent, not REC red
            d2dTarget_->FillEllipse(disc, brush(pal_.accent, (0.30f + 0.12f * hl) * a));
            d2dTarget_->DrawEllipse({disc.point, disc.radiusX - 0.75f, disc.radiusY - 0.75f}, brush(pal_.accent, 0.9f * a),
                                    1.5f);
        } else if (it.danger) {
            if (hl > 0.003f) d2dTarget_->FillEllipse(disc, brush(kDangerRed, 0.92f * hl * a));
            ink = mixc(kDangerRed, D2D1::ColorF(1, 1, 1), hl);
        } else if (hl > 0.003f) {
            d2dTarget_->FillEllipse(disc, brush(pal_.accent, 0.26f * hl * a));
        }
        if (icons && it.glyph) {
            const wchar_t g[2] = {it.glyph, 0};
            d2dTarget_->DrawText(g, 1, icons.Get(), b, brush(ink, a));
        }
        if (it.toggled && it.recording) {  // pulsing dot: recording now
            const float pulse = recPulse(now);
            d2dTarget_->FillEllipse({{b.right - btn * 0.2f, b.top + btn * 0.2f}, btn * 0.08f, btn * 0.08f},
                                    brush(kRecRed, (0.55f + 0.45f * pulse) * a));
        }
        d2dTarget_->SetTransform(base);
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
        const bool fadingTool = now - toolInAt_ < ms(kToolInMs) + kTail ||
                                (!toolInside_ && now >= toolUntil_ && now - toolUntil_ < ms(kToolOutMs) + kTail);
        const bool tip = ta > 0 && toolHot_ >= 0 && now - toolHotAt_ < kTipDelayMs + kTipFadeMs + kTail;
        toolFast = fadingTool || (tip && now - toolHotAt_ >= kTipDelayMs);
        if (tip && now - toolHotAt_ < kTipDelayMs) toolDue = kTipDelayMs - (now - toolHotAt_);
        if (ta > 0 && !toolInside_ && now < toolUntil_) {
            const double d = toolUntil_ - now;
            toolDue = toolDue < 0 ? d : std::min(toolDue, d);
        }
    }
    bool fast = toolFast || now - fadeInAt_ < ms(kFadeInMs) + kTail || now - fadeOutAt_ < ms(kDropMs) + kTail ||
                now - pinAt_ < ms(kPinFadeMs) + kTail ||
                (!toast_.empty() && now - toastAt_ < toastHold_ + ms(kToastOutMs) + kTail) ||
                (scene_ == Scene::Connecting && !paused_) || now - dimAt_ < ms(kDimMs) + kTail ||
                (!reduced_ && now - mascotHotAt_ < kHoverMs + kTail) ||
                (now - reactAt_ < kReactMs + 250 + kTail && (!reduced_ || reactBubble_)) ||
                uiAnimating(now) || !busy_.empty() || now - flashAt_ < kFlashInMs + kFlashOutMs + kTail || (now - zoomAt_ >= kZoomShowMs && now - zoomAt_ < kZoomShowMs + ms(kZoomFadeMs) + kTail);
    for (double at : optAt_) fast = fast || now - at < 70 + 220 + kTail;  // tick drawing
    if (!fast && now - zoomAt_ < kZoomShowMs) {
        // Event-driven until the big zoom indicator starts to fade.
        const double d = kZoomShowMs - (now - zoomAt_);
        toolDue = toolDue < 0 ? d : std::min(toolDue, d);
    }
    // Recording: the REC timer ticks once a second (its dot pulses only on
    // frames drawn anyway: a static phone screen costs 1 frame/s, not 30-60).
    if (recording_) {
        const double into = std::fmod(now - recAt_, 1000.0);  // ms into the timer's current second
        // (Waits up to 120 ms for a picture that redraws it anyway.)
        const double d = lastRender_ < now - into ? std::max(0.0, 120 - into) : 1000 - into;
        toolDue = toolDue < 0 ? d : std::min(toolDue, d);
    }
    // Idle / paused screen: the mascot floats (and blinks) for a while after
    // any activity, then rests on a static frame (at once with reduced motion).
    bool slow = !reduced_ && (scene_ == Scene::Idle || paused_) && now < ambientUntil_ + kTail;
    if (!fast && !slow) return toolDue;
    // Fully covered window (Present reported occlusion): probe slowly.
    const double period = occluded_ ? 500 : fast ? kFastFrameMs : kSlowFrameMs;
    const double next = std::max(0.0, period - (now - lastRender_));
    return toolDue >= 0 ? std::min(next, toolDue) : next;
}

void Renderer::drawPicture(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp, const Picture& pic,
                           const View& view, bool mirror) {
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    ctx_->RSSetViewports(1, &vp);

    Constants c{};
    c.uvRect[0] = pic.fmt.crop.left / static_cast<float>(pic.w);
    c.uvRect[1] = pic.fmt.crop.top / static_cast<float>(pic.h);
    c.uvRect[2] = pic.fmt.crop.right / static_cast<float>(pic.w);
    c.uvRect[3] = pic.fmt.crop.bottom / static_cast<float>(pic.h);
    if (pic.fmt.fullRange) {
        c.range[0] = 0; c.range[1] = 1; c.range[2] = 1;
    } else {
        c.range[0] = 16.f / 255; c.range[1] = 255.f / 219; c.range[2] = 255.f / 224;
    }
    if (pic.fmt.bt601) {
        c.mat[0] = 1.402f; c.mat[1] = -0.344136f; c.mat[2] = -0.714136f; c.mat[3] = 1.772f;
    } else {
        c.mat[0] = 1.5748f; c.mat[1] = -0.187324f; c.mat[2] = -0.468124f; c.mat[3] = 1.8556f;
    }
    pictureTransform(rot_, mirror, c.xfU, c.xfV);
    const float inv = 1.f / std::max(1.f, view.zoom);
    c.view[0] = inv;
    c.view[1] = view.cx - 0.5f * inv;
    c.view[2] = view.cy - 0.5f * inv;
    c.view[3] = static_cast<float>(view.filter);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, &c, sizeof(c));
        ctx_->Unmap(cb_.Get(), 0);
    }
    // NV12 / P010: t0 = Y, t1 = UV; BGRA: t2.
    ID3D11ShaderResourceView* srvs[3] = {pic.bgra ? nullptr : pic.ySrv.Get(), pic.uvSrv.Get(),
                                         pic.bgra ? pic.ySrv.Get() : nullptr};
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ctx_->PSSetShader(pic.bgra ? psBgra_.Get() : ps_.Get(), nullptr, 0);
    ctx_->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ctx_->PSSetShaderResources(0, 3, srvs);
    ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    ctx_->Draw(4, 0);
    ID3D11ShaderResourceView* nulls[3] = {};
    ctx_->PSSetShaderResources(0, 3, nulls);
}

bool Renderer::snapshot(std::vector<uint8_t>& out, UINT& w, UINT& h, bool framed) {
    return renderPicture(out, w, h, framed, mirror_);
}

bool Renderer::grab(std::vector<uint8_t>& out, UINT& w, UINT& h, bool mirror) {
    return renderPicture(out, w, h, false, mirror);
}

// The shown picture, unzoomed and unfiltered (snapshots, OCR).
bool Renderer::renderPicture(std::vector<uint8_t>& out, UINT& w, UINT& h, bool framed, bool mirror) {
    if (!(havePicture_ || (frozenOn_ && frozen_.valid())) || !shown().valid()) return false;
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
    drawPicture(rtv.Get(), vp, shown(), View{}, mirror);  // same shader as on screen: identical colours
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
    if (sceneDy_ != 0) {  // (sizes above from the unshifted layout)
        L.anchor.y += sceneDy_;
        L.mascot.top += sceneDy_;
        L.mascot.bottom += sceneDy_;
        L.text.top += sceneDy_;
        L.text.bottom += sceneDy_;
    }
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
    const float env = reduced_ ? 0.f : busy ? 1.f : ease((ambientUntil_ - now) / kAmbientSettleMs);
    const double react = now - reactAt_;
    const bool reacting = react >= 0 && react < kReactMs;
    const double surprise = now - surpriseAt_;
    if (busy && connectIntro_ && surprise >= 0 && surprise < kSurpriseMs) {
        P.face = ToutouArt::FaceSurprised;
    } else if (busy) {
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
    if (busy && connectIntro_ && !reduced_ && surprise >= 0 && surprise < 850) {
        // A small lift while surprised; the face turns to the phone (160 ms
        // in, held, 350 ms back).
        if (surprise < kSurpriseMs) P.dy -= 3 * L.u * std::sin(kPi * static_cast<float>(surprise / kSurpriseMs));
        const float k = surprise < 160 ? outCubic(surprise / 160) : surprise < 500 ? 1.f : 1 - ease((surprise - 500) / 350);
        P.faceX = 2.5f * k;
        P.faceY = -1.f * k;
    }
    if (!reduced_ && scene_ != Scene::Live && now - fadeOutAt_ < kDropMs)  // back from a picture: drops in 10 px
        P.dy -= 10 * (1 - outCubic((now - fadeOutAt_) / kDropMs));
    float hx, hy, hdy;
    hopTransform(L, now, hx, hy, hdy);
    P.sx *= hx;
    P.sy *= hy;
    P.dy += hdy;
    const float h = static_cast<float>((now - mascotHotAt_) / kHoverMs);
    const bool hot = mascotHot_ && mascotClickable(now);
    const float grow = reduced_ ? 1.f : 1 + 0.04f * (hot ? ease(h) : (mascotHot_ ? 0.f : 1 - ease(h)));
    P.sx *= grow;
    P.sy *= grow;
    P.m = D2D1::Matrix3x2F::Translation(-kPivotX, -kPivotY) * D2D1::Matrix3x2F::Scale(P.sx * L.u, P.sy * L.u) *
          D2D1::Matrix3x2F::Translation(L.anchor.x, L.anchor.y + P.dy);
    // Beam: fast and full while connecting (it grows out first), a calm pulse
    // every 6 s on the idle screen, none asleep.
    if (busy && reduced_) {  // a still beam
        P.beamT1 = 1;
        P.beamK = 1;
    } else if (busy) {
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
    const float breath =
        reduced_ ? 1.f : 0.5f - 0.5f * std::cos(static_cast<float>(std::fmod(now, 4200.0) / 4200) * 2 * kPi);
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
    drawLayer(P.face, P.faceX || P.faceY ? D2D1::Matrix3x2F::Translation(P.faceX, P.faceY) * P.m : P.m, P.opacity);
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

// 「Found a phone」: five small sparkles from the phone's top (white and the
// accent), 180-780 ms after the surprise.
void Renderer::drawSparkles(const Pose& P, double now) {
    const double e = now - surpriseAt_ - 180;
    if (reduced_ || e < 0 || e > 600 || !sparkle_ || scene_ != Scene::Connecting) return;
    const float t = static_cast<float>(e / 600);
    const D2D1_POINT_2F o = P.m.TransformPoint({224, 96});
    const float u = P.m._11;  // DIPs per frame unit (incl. squash)
    const float grow = t < 0.3f ? outCubic(t / 0.3f) : 1 - inCubic((t - 0.3f) / 0.7f);
    for (int i = 0; i < 5; ++i) {
        const float ang = (-160.f + i * 32.f) * kPi / 180.f;
        const float dist = u * (7 + 13 * outCubic(t));
        const float size = u * (4.5f + 1.5f * (i % 2)) * grow;
        if (size <= 0.05f) continue;
        d2dTarget_->SetTransform(D2D1::Matrix3x2F::Scale(size, size) * D2D1::Matrix3x2F::Rotation(45 * t + i * 9) *
                                 D2D1::Matrix3x2F::Translation(o.x + std::cos(ang) * dist, o.y + std::sin(ang) * dist));
        d2dTarget_->FillGeometry(sparkle_.Get(), brush(i % 2 ? pal_.accent : D2D1_COLOR_F{1, 1, 1, 1}, 0.95f));
    }
    d2dTarget_->SetTransform(D2D1::IdentityMatrix());
}

// Hearts bursting from the top of the cloud after a click / when a phone
// connects (accent and a lighter accent).
void Renderer::drawMascotFx(const Layout& L, const Pose& P, double now) {
    drawSparkles(P, now);
    const double e = now - reactAt_;
    if (reduced_ || e < 0 || e > 1150 || !heart_ || L.u <= 0 || P.face == ToutouArt::FaceSleepy) return;
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
    if (reduced_ || e < 0 || e >= kHopMs) return;
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
    if (!toutou_.loaded() || (!pin_.empty() && (pinVisible_ || now - pinAt_ < ms(kPinFadeMs)))) return false;
    if (paused_) return true;  // the paused screen
    return scene_ == Scene::Idle && !(havePicture_ && now - fadeOutAt_ < ms(kFadeOutMs));
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
    if (te < 220 && !reduced_) {
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
        float a = strength * 0.32f * std::pow(std::sin(kPi * ph), 1.2f);
        // Nearly gone over the text block (0.7.2: a heart or sparkle drawn
        // across 「Android」 or the 怎麼連線？ link read as a stray glyph).
        if (textBlock_.right > textBlock_.left) {
            const float dx = std::max({textBlock_.left - x, 0.f, x - textBlock_.right});
            const float dy = std::max({textBlock_.top - y, 0.f, y - textBlock_.bottom});
            a *= std::clamp(std::sqrt(dx * dx + dy * dy) / (24 * unit), 0.1f, 1.f);
        }
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
    const float env = reduced_ ? 0.f : ease((ambientUntil_ - now) / kAmbientSettleMs);
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
    const double now = clockMs();
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
    D2D1_MATRIX_3X2_F base;
    d2dTarget_->GetTransform(&base);
    for (size_t i = 0; i < options_.size(); ++i) {
        const int ii = static_cast<int>(i);
        const float y = top + rowH * i;
        D2D1_RECT_F row{left, y, left + groupW, y + rowH};
        // Hover: the row wash, box outline and label ease in / out; a press
        // shrinks the row to 96 %; keyboard focus draws a ring.
        const float hl = uiLevel(uiHot_, UiOption, ii, now);
        const float pl = reduced_ ? 0.f : uiLevel(uiPress_, UiOption, ii, now);
        const float fl = uiLevel(uiFocus_, UiOption, ii, now);
        if (pl > 0.003f)
            d2dTarget_->SetTransform(scaledAbout(base, 1 - 0.04f * pl, {(row.left + row.right) / 2, (row.top + row.bottom) / 2}));
        if (hl > 0.003f) d2dTarget_->FillRoundedRectangle({row, rowH / 2, rowH / 2}, brush(pal_.accent, 0.12f * hl));
        const float bx = left + padX, by = std::round(y + (rowH - box) / 2);
        D2D1_ROUNDED_RECT b{{bx, by, bx + box, by + box}, box * 0.28f, box * 0.28f};
        // Tick: checking fills the box (140 ms) and draws the tick along its
        // stroke (220 ms after 70 ms); unchecking undraws it (120 ms).
        const double ce = i < optAt_.size() ? now - optAt_[i] : 1e9;
        const bool on = options_[i].checked;
        float fillA = 1, tickT = 1;
        if (reduced_) {
            fillA = tickT = on ? ease(ce / 120) : 1 - ease(ce / 120);
        } else if (on) {
            fillA = outCubic(ce / 140);
            tickT = softOut((ce - 70) / 220);
        } else {
            fillA = tickT = 1 - inCubic(ce / 120);
        }
        if (!on || fillA < 1) {
            const D2D1_ROUNDED_RECT o{{bx + 0.75f, by + 0.75f, bx + box - 0.75f, by + box - 0.75f}, b.radiusX, b.radiusY};
            d2dTarget_->DrawRoundedRectangle(o, brush(mixc(alphaOf(pal_.dim, 0.85f), pal_.accent, hl), 1 - fillA), 1.5f);
        }
        if (fillA > 0.003f) d2dTarget_->FillRoundedRectangle(b, brush(pal_.accent, fillA));
        if (tickT > 0.003f) {
            const D2D1_POINT_2F p0{bx + box * 0.26f, by + box * 0.53f}, p1{bx + box * 0.43f, by + box * 0.70f},
                p2{bx + box * 0.75f, by + box * 0.34f};
            const float l1 = std::hypot(p1.x - p0.x, p1.y - p0.y), l2 = std::hypot(p2.x - p1.x, p2.y - p1.y);
            const float len = (l1 + l2) * tickT;
            ComPtr<ID2D1PathGeometry> g;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(d2d_->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&sink))) {
                sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_HOLLOW);
                if (len <= l1) {
                    const float k = len / l1;
                    sink->AddLine({p0.x + (p1.x - p0.x) * k, p0.y + (p1.y - p0.y) * k});
                } else {
                    const float k = (len - l1) / l2;
                    sink->AddLine(p1);
                    sink->AddLine({p1.x + (p2.x - p1.x) * k, p1.y + (p2.y - p1.y) * k});
                }
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                d2dTarget_->DrawGeometry(g.Get(), brush(pal_.ink), box * 0.13f, round_.Get());
            }
        }
        if (auto& l = labels[i]) {
            const float tx = bx + box + gap, ty = y + (rowH - textH(l.Get())) / 2;
            d2dTarget_->DrawTextLayout({tx, ty}, l.Get(), brush(mixc(pal_.dim, pal_.fg, hl)));
        }
        d2dTarget_->SetTransform(base);
        if (fl > 0.003f)
            d2dTarget_->DrawRoundedRectangle({inflate(row, 3), rowH / 2 + 3, rowH / 2 + 3}, brush(pal_.fg, 0.9f * fl), 2.f);
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
    const float env = paused || reduced_ ? 0.f : busy ? 1.f : ease((ambientUntil_ - now) / kAmbientSettleMs);
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
    if (busy && connectIntro_ && reduced_ && intro < 120) {  // a plain cross-fade
        const float t = ease(intro / 120);
        drawTextBlock(Scene::Idle, L, 1 - t, now);
        drawTextBlock(Scene::Connecting, L, t, now);
    } else if (busy && connectIntro_ && !reduced_ && intro < kConnectIntroMs) {
        const float t = static_cast<float>(intro);
        drawTextBlock(Scene::Idle, L, 1 - ease((t - 700) / 250), now);
        drawTextBlock(Scene::Connecting, L, ease((t - 850) / 300), now);
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
    // Idle: the title's 「…」 becomes three dots that breathe (staggered, 1.8 s)
    // while the screen animates; the text keeps no ellipsis glyph of its own.
    bool idleDots = false;
    if (idle) {
        if (t1.size() >= 1 && t1.back() == L'\u2026') t1.pop_back(), idleDots = true;
        else if (t1.size() >= 3 && t1.compare(t1.size() - 3, 3, L"...") == 0) t1.resize(t1.size() - 3), idleDots = true;
    }
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
    const float dotsW = busy || idleDots ? titleSize * 0.95f : 0;  // fixed slot: the title does not jitter
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
    if (opacity * sceneOpacity_ >= 0.5f) textBlock_ = {x, y - L.hint * 0.5f, L.text.right - margin, y + block + L.hint * 0.5f};
    if (busy) {
        drawSpinner({L.text.left + rw / 2, y + spin / 2}, spin / 2, now);
        y += spin + gapSpin;
    }
    if (l1) {
        const float dx = -dotsW / 2;
        d2dTarget_->DrawTextLayout({x + dx, y}, l1.Get(), brush(paused ? pal_.dim : pal_.fg));
        if (busy || idleDots) {
            const float r = std::max(titleSize * 0.065f, 1.5f), step = titleSize * 0.28f;
            const float env = idleDots && !reduced_ ? ease((ambientUntil_ - now) / kAmbientSettleMs) : 0.f;
            const float x0 = L.text.left + rw / 2 + dx + textW(l1.Get()) / 2 + titleSize * (busy ? 0.22f : 0.12f) + r;
            DWRITE_LINE_METRICS lm{};
            UINT32 lines = 0;
            l1->GetLineMetrics(&lm, 1, &lines);
            const float cy = y + (lines ? lm.baseline : textH(l1.Get()) * 0.8f) - r;
            for (int i = 0; i < 3; ++i) {
                float a;
                if (busy) {
                    const float ph = static_cast<float>(std::fmod(now / 1200.0 - i * 0.18, 1.0));
                    a = 0.35f + 0.65f * std::pow(std::max(0.f, std::sin(ph * 2 * kPi)), 2.f);
                } else {  // idle: a slow breath, 0.45 <-> 1, still once the screen rests
                    const float ph = static_cast<float>(std::fmod(now / 1800.0 - i * 0.16, 1.0));
                    a = 1 - env * 0.55f * (0.5f + 0.5f * std::cos(ph * 2 * kPi));
                }
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
    const double now = clockMs();
    const float s = dpi_ / 96.f;
    const float cx = (region.left + region.right) / 2;
    actionRects_.assign(actions_.size(), RECT{});
    D2D1_MATRIX_3X2_F base;
    d2dTarget_->GetTransform(&base);
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
        // Hover eases in (120 ms) / out (180 ms), a press shrinks it to 96 %,
        // keyboard focus draws a ring.
        const float hl = uiLevel(uiHot_, UiAction, a.index, now);
        const float pl = reduced_ ? 0.f : uiLevel(uiPress_, UiAction, a.index, now);
        const float fl = uiLevel(uiFocus_, UiAction, a.index, now);
        if (pl > 0.003f) d2dTarget_->SetTransform(scaledAbout(base, 1 - 0.04f * pl, {(r.left + r.right) / 2, (r.top + r.bottom) / 2}));
        IDWriteTextLayout* l = a.text.Get();
        const float tw = textW(l), th = textH(l);
        l->SetMaxWidth(std::ceil(std::min(tw, a.w)) + 2.f);  // exactly-measured width would trigger the ellipsis
        const D2D1_POINT_2F at{std::round((r.left + r.right) / 2 - std::min(tw, a.w) / 2) - 1.f,
                               std::round(r.top + (rowH - th) / 2)};
        const D2D1_ROUNDED_RECT pill{r, rowH / 2, rowH / 2};
        if (a.primary) {
            const D2D1_ROUNDED_RECT edge{{r.left + 0.75f, r.top + 0.75f, r.right - 0.75f, r.bottom - 0.75f},
                                         rowH / 2 - 0.75f, rowH / 2 - 0.75f};
            d2dTarget_->FillRoundedRectangle(pill, brush(pal_.accent, 0.10f + 0.90f * hl));
            d2dTarget_->DrawRoundedRectangle(edge, brush(pal_.accent, 0.85f + 0.15f * hl), 1.5f);
            d2dTarget_->DrawTextLayout(at, l, brush(mixc(pal_.accent, pal_.ink, hl)));
        } else {
            if (hl > 0.003f) d2dTarget_->FillRoundedRectangle(pill, brush(pal_.accent, 0.14f * hl));
            d2dTarget_->DrawTextLayout(at, l, brush(mixc(alphaOf(pal_.accent, 0.92f), pal_.fg, hl)));
        }
        d2dTarget_->SetTransform(base);
        if (fl > 0.003f)
            d2dTarget_->DrawRoundedRectangle({inflate(r, 3), rowH / 2 + 3, rowH / 2 + 3}, brush(pal_.fg, 0.9f * fl), 2.f);
        actionRects_[a.index] = {static_cast<LONG>(std::floor(r.left * s)), static_cast<LONG>(std::floor(r.top * s)),
                                static_cast<LONG>(std::ceil(r.right * s)), static_cast<LONG>(std::ceil(r.bottom * s))};
    }
}

void Renderer::drawPin(double now) {
    if (pin_.empty()) return;
    const float t = ease((now - pinAt_) / ms(kPinFadeMs));
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
    const float lift = pinVisible_ && !reduced_ ? (1 - t) * 8 : 0;  // slight rise while fading in
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
    // In: 220 ms outCubic, rising 8 px and growing from 98 %; out: 260 ms
    // smoothstep fade in place (reduced motion: 120 ms fades only).
    float a, rise = 0, scale = 1;
    if (e < ms(kToastInMs)) {
        a = reduced_ ? ease(e / ms(kToastInMs)) : outCubic(e / kToastInMs);
        if (!reduced_) rise = (1 - a) * 8, scale = 0.98f + 0.02f * a;
    } else if (e < toastHold_) {
        a = 1;
    } else {
        a = 1 - ease((e - toastHold_) / ms(kToastOutMs));
    }
    if (a <= 0.003f) return;
    const float s = dpi_ / 96.f;
    const float W = width_ / s, H = height_ / s;
    const float size = std::clamp(std::min(H / 24, W / 15) * 0.56f, 12.f, 24.f);
    const float padX = size * 1.2f, padY = size * 0.65f;
    const float maxW = W - 32 - 2 * padX;
    auto l = layout(pm::i18n::keepWords(toast_), size, maxW, DWRITE_FONT_WEIGHT_NORMAL, false);
    if (!l) return;
    // (Measured untrimmed: the trimmed layout never reports more than maxW.)
    if (auto full = layout(toast_, size, 100000.f, DWRITE_FONT_WEIGHT_NORMAL, false); full && textW(full.Get()) > maxW) {
        // Too long for one line: wrap (after the first 「。」 / ". " if there is one).
        std::wstring t = toast_;
        bool split = true;
        if (const size_t dot = t.find(L'。'); dot != std::wstring::npos && dot + 1 < t.size()) t.insert(dot + 1, L"\n");
        else if (const size_t en = t.find(L". "); en != std::wstring::npos && en > 8) t[en + 1] = L'\n';
        else split = false;
        // No sentence break: lines of even length instead of a full first
        // line and a stub such as 「Wi-Fi）」 (0.7.2).
        float wrapW = maxW;
        if (!split) {
            const float fw = textW(full.Get());
            wrapW = std::min(maxW, fw / std::ceil(fw / maxW) + size * 1.5f);
        }
        if (auto w = layout(pm::i18n::keepWords(t), size, wrapW, DWRITE_FONT_WEIGHT_NORMAL, true)) l = w;
    }
    const float tw = std::min(textW(l.Get()), maxW), th = textH(l.Get());
    const float pillW = tw + 2 * padX, pillH = th + 2 * padY;
    const float x0 = std::round((W - pillW) / 2);
    float y0 = std::round(H - std::max(20.f, H * 0.05f) - pillH + rise);
    // Above the translation list when that panel fills the bottom (0.7.2:
    // the toast was drawn across its rows).
    if (!boxes_.empty() && !showOriginal_ && ovPanel_.bottom > ovPanel_.top && y0 + pillH > ovPanel_.top &&
        x0 < ovPanel_.right && x0 + pillW > ovPanel_.left && ovPanel_.top - pillH - 10 > H * 0.12f)
        y0 = std::round(ovPanel_.top - pillH - 10 + rise);
    const float rad = std::min(pillH / 2, size * 1.25f);  // one line: a pill; more: a rounded card
    const D2D1_ROUNDED_RECT pill{{x0, y0, x0 + pillW, y0 + pillH}, rad, rad};
    D2D1_MATRIX_3X2_F base;
    d2dTarget_->GetTransform(&base);
    if (scale < 1) d2dTarget_->SetTransform(scaledAbout(base, scale, {x0 + pillW / 2, y0 + pillH / 2}));
    d2dTarget_->FillRoundedRectangle(pill, brush(pal_.card, 0.96f * a));
    d2dTarget_->DrawRoundedRectangle(pill, brush(pal_.accent, 0.35f * a), 1.25f);
    l->SetMaxWidth(std::ceil(tw) + 2.f);  // slack: exactly-measured width triggers the ellipsis
    d2dTarget_->DrawTextLayout({x0 + padX - 1.f, y0 + padY}, l.Get(), brush(pal_.fg, a));
    d2dTarget_->SetTransform(base);
}

bool Renderer::render() {
    presented_ = false;
    presentHr_ = S_OK;
    if (!swap_) return false;  // device lost and not re-created yet
    if (width_ == 0 || height_ == 0) return true;
    if (!rtv_) {  // a failed ResizeBuffers / GetBuffer: nothing can be drawn (the watchdog re-creates the chain)
        presentHr_ = E_HANDLE;
        return true;
    }
    const double now = clockMs();
    lastRender_ = now;
    if (scene_ == Scene::Connecting && !pinVisible_ && now - sceneAt_ > kConnectTimeoutMs) {
        scene_ = Scene::Idle;  // nothing arrived: don't spin forever
        sceneAt_ = now;
        connectIntro_ = false;
        poke();
    }
    const bool fadingIn = scene_ == Scene::Live && now - fadeInAt_ < ms(kFadeInMs);
    const bool fadingOut = scene_ != Scene::Live && havePicture_ && now - fadeOutAt_ < ms(kFadeOutMs);
    // A frozen picture stays up while the phone screen is off.
    if (frozenOn_ && !frozen_.valid() && cur_.valid() && havePicture_ && copyPicture(cur_, frozen_)) keepFrozenCopy();
    const bool frozenUp = frozenOn_ && frozen_.valid();
    // (A frozen picture restored after a device loss shows before the
    // decoder has delivered again.)
    const bool video = (havePicture_ || frozenUp) && (!paused_ || frozenUp) && (scene_ == Scene::Live || fadingOut);
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
        if (vp.Width > 0 && vp.Height > 0) {
            // Settle: the picture grows in from 98.5 % / shrinks to 98.8 % while
            // cross-fading (not inside a device frame: the bezel stays put).
            D3D11_VIEWPORT pv = vp;
            float k = 1;
            if (!reduced_ && !framed && fadingIn) k = 0.985f + 0.015f * outCubic((now - fadeInAt_) / kFadeInMs);
            if (!reduced_ && !framed && fadingOut) k = 1 - 0.012f * standard((now - fadeOutAt_) / 280);
            if (k < 1) {
                pv.TopLeftX += pv.Width * (1 - k) / 2;
                pv.TopLeftY += pv.Height * (1 - k) / 2;
                pv.Width *= k;
                pv.Height *= k;
            }
            drawPicture(rtv_.Get(), pv);
        }
        if (scene_ == Scene::Live && vp.Width > 0 && vp.Height > 0)
            picRect_ = {static_cast<LONG>(vp.TopLeftX), static_cast<LONG>(vp.TopLeftY),
                        static_cast<LONG>(vp.TopLeftX + vp.Width), static_cast<LONG>(vp.TopLeftY + vp.Height)};
        // Magnifier overview: the whole picture, small, bottom right.
        miniRect_ = {};
        if (view_.zoom > 1.001f && vp.Width > 0 && vp.Height > 0) {
            miniRect_ = minimapRect(vp, framed ? fg.radius : 0.f);
            const D3D11_VIEWPORT mv{miniRect_.left, miniRect_.top, miniRect_.right - miniRect_.left,
                                    miniRect_.bottom - miniRect_.top, 0, 1};
            View whole;
            whole.filter = view_.filter;
            drawPicture(rtv_.Get(), mv, shown(), whole, mirror_);
        }
    }
    optionRects_.clear();
    actionRects_.clear();
    const float dim = video ? dimLevel(now) : 0.f;
    const bool overlays = (!pin_.empty() && (pinVisible_ || now - pinAt_ < ms(kPinFadeMs))) ||
                          (!toast_.empty() && now - toastAt_ < toastHold_ + ms(kToastOutMs));
    const float toolA = toolbarAlpha(now);
    const bool magUi = video && (view_.zoom > 1.001f || now - zoomAt_ < kZoomShowMs + ms(kZoomFadeMs) || frozenUp ||
                                 !boxes_.empty() || selecting_);
    const bool flashing = video && now - flashAt_ < kFlashInMs + kFlashOutMs;
    const bool need2D = (!video && !blank) || fadingIn || fadingOut || overlays || framed || dim > 0.003f || flashing ||
                        recording_ || toolA > 0.003f || magUi || !busy_.empty();
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
                const double e = now - fadeInAt_;
                const float a = reduced_ ? 1 - ease(e / ms(kFadeInMs)) : 1 - standard(e / kSceneOutMs);
                drawBackground(a);
                sceneDy_ = reduced_ ? 0.f : -8 * outCubic(e / kSceneOutMs);
                drawScene(fadeFrom_, a, now);
                sceneDy_ = 0;
            } else if (fadingOut) {
                const double e = now - fadeOutAt_;
                const float a = reduced_ ? ease(e / ms(kFadeOutMs)) : outCubic(e / kFadeOutMs);
                drawBackground(a);
                drawScene(Scene::Idle, a, now);
            }
        }
        if (flashing && vp.Width > 0) {  // screenshot: a white veil over the picture only
            const double fe = now - flashAt_;
            const float fa = fe < kFlashInMs ? 0.28f * outCubic(fe / kFlashInMs)
                                             : 0.28f * (1 - standard((fe - kFlashInMs) / kFlashOutMs));
            d2dTarget_->FillRectangle({vp.TopLeftX / s, vp.TopLeftY / s, (vp.TopLeftX + vp.Width) / s,
                                       (vp.TopLeftY + vp.Height) / s},
                                      brush(D2D1::ColorF(1, 1, 1), fa));
        }
        if (video && vp.Width > 0 && !fadingIn && !fadingOut) {
            const D2D1_RECT_F pr{vp.TopLeftX / s, vp.TopLeftY / s, (vp.TopLeftX + vp.Width) / s,
                                 (vp.TopLeftY + vp.Height) / s};
            drawTextOverlay(pr, framed ? fg.radius / s : 0.f);
            drawMagnifierUi(pr, framed ? fg.radius / s : 0.f, now);
            if (selecting_) drawSelection(pr, framed ? fg.radius / s : 0.f);
        }
        if (!busy_.empty()) {
            if (video && vp.Width > 0)
                drawBusy({vp.TopLeftX / s, vp.TopLeftY / s, (vp.TopLeftX + vp.Width) / s, (vp.TopLeftY + vp.Height) / s}, now);
            else
                drawBusy({0, 0, width_ / s, height_ / s}, now);
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
    if (faultSwallow_) {  // fault injection: a swap chain that no longer presents
        presentHr_ = S_FALSE;
        return true;
    }
    HRESULT hr = swap_->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        log("device lost on Present (hr=0x%08lx, reason=0x%08lx)", hr, dev_->GetDeviceRemovedReason());
        return false;
    }
    if (SUCCEEDED(hr) && faultOccludeUntil_ > now) hr = DXGI_STATUS_OCCLUDED;  // fault injection
    presentHr_ = hr;
    presented_ = hr == S_OK || hr == DXGI_STATUS_OCCLUDED;
    if (FAILED(hr) && hr != presentErrLogged_) {
        presentErrLogged_ = hr;
        log("Present failed hr=0x%08lx", hr);
    } else if (SUCCEEDED(hr)) {
        presentErrLogged_ = S_OK;
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
    const float t = ease((now - dimAt_) / ms(kDimMs));
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
    const Picture& p = shown();
    if (!p.valid()) return;
    w = p.fmt.visibleWidth();
    h = p.fmt.visibleHeight();
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

// REC dot: a 1.4 s pulse while pictures flow (frames are drawn anyway);
// steady on a still phone screen (1 frame/s for the timer) and with reduced
// motion.
float Renderer::recPulse(double now) const {
    if (reduced_ || now - lastPictureAt_ > 250) return 1;
    const float ph = static_cast<float>(std::fmod((now - recAt_) / 1400.0, 1.0));
    return 0.5f + 0.5f * std::cos(ph * 2 * kPi);
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
    const float pulse = recPulse(now);
    const D2D1_POINT_2F c{x0 + padX + dot, y0 + pillH / 2};
    d2dTarget_->FillEllipse({c, dot * (1.5f + 0.5f * (1 - pulse)), dot * (1.5f + 0.5f * (1 - pulse))},
                            brush(kRecRed, 0.22f * pulse));
    d2dTarget_->FillEllipse({c, dot, dot}, brush(kRecRed, 0.55f + 0.45f * pulse));
    l->SetMaxWidth(std::ceil(tw) + 2.f);
    d2dTarget_->DrawTextLayout({c.x + dot + gap - 1.f, y0 + padY}, l.Get(), brush(pal_.fg));
}

// ---------------------------------------------------------------------------
// Magnifier, low-vision filters, freeze, translated-text overlay

Renderer::View Renderer::clampView(View v) {
    if (!(v.zoom >= 1)) v.zoom = 1;  // also NaN
    v.zoom = std::min(v.zoom, kMaxZoom);
    const float half = 0.5f / v.zoom;
    v.cx = std::isfinite(v.cx) ? std::clamp(v.cx, half, 1 - half) : 0.5f;
    v.cy = std::isfinite(v.cy) ? std::clamp(v.cy, half, 1 - half) : 0.5f;
    v.filter = std::clamp(v.filter, 0, 4);
    return v;
}

void Renderer::setView(const View& v) {
    const View n = clampView(v);
    if (std::fabs(n.zoom - view_.zoom) > 1e-4f) zoomAt_ = clockMs();
    view_ = n;
}

bool Renderer::copyPicture(const Picture& from, Picture& to) {
    if (!dev_ || !from.valid()) return false;
    const bool ok = from.bgra ? ensureBgra(to, from.w, from.h) : ensureTextures(to, from.hw, from.tenBit, from.w, from.h);
    if (!ok) return false;
    if (from.hw) {
        ctx_->CopyResource(to.nv12.Get(), from.nv12.Get());
    } else {
        ctx_->CopyResource(to.y.Get(), from.y.Get());
        if (!from.bgra) ctx_->CopyResource(to.uv.Get(), from.uv.Get());
    }
    to.fmt = from.fmt;
    return true;
}

void Renderer::setFrozen(bool on) {
    if (on == frozenOn_) return;
    frozenOn_ = on;
    frozen_ = {};
    frozenCpu_ = {};
    frozenWasUp_ = false;
    if (on && cur_.valid() && havePicture_ && copyPicture(cur_, frozen_)) keepFrozenCopy();  // else: from the next picture
}

void Renderer::keepFrozenCopy() {
    const double t0 = clockMs();
    if (!readBackPicture(frozen_, frozenCpu_)) {
        frozenCpu_ = {};
        log("freeze: could not keep a CPU copy of the frozen picture (a device loss ends the freeze)");
        return;
    }
    log("freeze: CPU copy of the frozen picture kept (%ux%u, %.1f MB, %.1f ms)", frozenCpu_.w, frozenCpu_.h,
        (frozenCpu_.y.size() + frozenCpu_.uv.size()) / 1048576.0, clockMs() - t0);
}

bool Renderer::readBackPicture(const Picture& p, CpuPicture& c) {
    c = {};
    if (!p.valid() || !dev_ || !ctx_) return false;
    auto stage = [&](ID3D11Texture2D* t, ComPtr<ID3D11Texture2D>& st) {
        if (!t) return false;
        D3D11_TEXTURE2D_DESC td{};
        t->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &st))) return false;
        ctx_->CopyResource(st.Get(), t);
        return true;
    };
    auto rows = [](const D3D11_MAPPED_SUBRESOURCE& m, size_t first, UINT n, UINT rowBytes, std::vector<uint8_t>& out) {
        out.resize(static_cast<size_t>(n) * rowBytes);
        const auto* src = static_cast<const uint8_t*>(m.pData) + first * m.RowPitch;
        for (UINT r = 0; r < n; ++r)
            std::memcpy(out.data() + static_cast<size_t>(r) * rowBytes, src + static_cast<size_t>(r) * m.RowPitch, rowBytes);
    };
    const UINT bpp = p.tenBit ? 2 : 1;
    c.bgra = p.bgra;
    c.tenBit = p.tenBit;
    c.w = p.w;
    c.h = p.h;
    c.fmt = p.fmt;
    c.yRow = p.bgra ? p.w * 4 : p.w * bpp;
    c.uvH = (p.h + 1) / 2;
    c.uvRow = (p.w + 1) / 2 * 2 * bpp;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (p.hw) {  // NV12 / P010: the UV plane follows the Y plane (RowPitch x Height)
        ComPtr<ID3D11Texture2D> st;
        if (!stage(p.nv12.Get(), st) || FAILED(ctx_->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        rows(m, 0, p.h, c.yRow, c.y);
        rows(m, p.h, c.uvH, c.uvRow, c.uv);
        ctx_->Unmap(st.Get(), 0);
        return true;
    }
    ComPtr<ID3D11Texture2D> sy, suv;
    if (!stage(p.y.Get(), sy) || (!p.bgra && !stage(p.uv.Get(), suv))) return false;
    if (FAILED(ctx_->Map(sy.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    rows(m, 0, p.h, c.yRow, c.y);
    ctx_->Unmap(sy.Get(), 0);
    if (p.bgra) return true;
    if (FAILED(ctx_->Map(suv.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    rows(m, 0, c.uvH, c.uvRow, c.uv);
    ctx_->Unmap(suv.Get(), 0);
    return true;
}

bool Renderer::restorePicture(const CpuPicture& c, Picture& p) {
    p = {};
    if (!c.w || !c.h || !dev_ || !ctx_) return false;
    if (c.bgra) {
        if (!ensureBgra(p, c.w, c.h)) return false;
        ctx_->UpdateSubresource(p.y.Get(), 0, nullptr, c.y.data(), c.yRow, 0);
    } else {
        if (!ensureTextures(p, false, c.tenBit, c.w, c.h)) return false;
        ctx_->UpdateSubresource(p.y.Get(), 0, nullptr, c.y.data(), c.yRow, 0);
        ctx_->UpdateSubresource(p.uv.Get(), 0, nullptr, c.uv.data(), c.uvRow, 0);
    }
    p.fmt = c.fmt;
    return true;
}

void Renderer::restoreFrozen() {
    if (!std::exchange(frozenWasUp_, false) || !frozenOn_) return;
    if (restorePicture(frozenCpu_, frozen_)) {
        log("freeze: frozen picture restored on the new device");
        return;
    }
    // Re-freezing would show another picture under the old translation:
    // end the freeze and drop the boxes instead (the window tells the app).
    log("freeze: the frozen picture could not be restored after the device loss; freeze ended, text overlay cleared");
    frozenOn_ = false;
    frozen_ = {};
    frozenCpu_ = {};
    boxes_.clear();
    ovHits_ = {};
    ovValid_ = false;
    frozenDropped_ = true;
}

void Renderer::setBusy(const std::wstring& label) {
    if (!label.empty() && busy_.empty()) busyAt_ = clockMs();
    busy_ = label;
}

void Renderer::setSelection(bool active, float x0, float y0, float x1, float y1) {
    selecting_ = active;
    sel_[0] = x0;
    sel_[1] = y0;
    sel_[2] = x1;
    sel_[3] = y1;
}

D2D1_POINT_2F Renderer::contentToDip(const D2D1_RECT_F& pic, float dx, float dy) const {
    const float tx = mirror_ ? 1 - dx : dx;
    const float vx = (tx - view_.cx) * view_.zoom + 0.5f, vy = (dy - view_.cy) * view_.zoom + 0.5f;
    return {pic.left + vx * (pic.right - pic.left), pic.top + vy * (pic.bottom - pic.top)};
}

D2D1_RECT_F Renderer::minimapRect(const D3D11_VIEWPORT& vp, float radiusPx) const {
    // About 30 % of the picture's short side (70..220 DIP), inside the rounded corner.
    const float s = dpi_ / 96.f;
    const float aspect = vp.Width / std::max(1.f, vp.Height);
    float h = std::clamp(std::min(vp.Width, vp.Height) * 0.30f, 70 * s, 220 * s);
    float w = h * aspect;
    if (w > vp.Width * 0.28f) {
        w = vp.Width * 0.28f;
        h = w / aspect;
    }
    const float m = std::max(10 * s, radiusPx * 0.35f);
    const float x1 = std::floor(vp.TopLeftX + vp.Width - m), y1 = std::floor(vp.TopLeftY + vp.Height - m);
    return {std::round(x1 - w), std::round(y1 - h), x1, y1};
}

// drawTextOverlay: text_overlay.cpp

// Where drawMagnifierUi puts the zoom badge (bottom left, zoomed) and the
// 畫面已凍結 badge (top right, frozen); empty rects when not shown.  The
// translation overlay keeps clear of them.
Renderer::Badges Renderer::badgeRects(const D2D1_RECT_F& pic, float radius) {
    Badges b;
    const float pw = pic.right - pic.left, ph = pic.bottom - pic.top;
    const float inset = std::max(10.f, radius * 0.40f);
    if (view_.zoom > 1.001f) {
        wchar_t num[16];
        swprintf_s(num, std::fabs(view_.zoom - std::round(view_.zoom)) > 0.04f ? L"%.1f" : L"%.0f", view_.zoom);
        const float size = std::clamp(std::min(pw, ph) / 16.f, 16.f, 30.f);
        if (auto l = layout(std::wstring(num) + L"×", size, 400, DWRITE_FONT_WEIGHT_BOLD, false, true)) {
            const float tw = textW(l.Get()), th = textH(l.Get());
            const float icon = size * 1.05f, padX = size * 0.55f, padY = size * 0.28f, gap = size * 0.3f;
            const float w = padX + icon + gap + tw + padX, h = th + 2 * padY;
            const float x0 = std::round(pic.left + inset), y0 = std::round(pic.bottom - inset - h);
            b.zoom = {x0, y0, x0 + w, y0 + h};
        }
    }
    if (frozenOn_ && frozen_.valid()) {
        const float size = std::clamp(std::min(pw, ph) / 22.f, 13.f, 24.f);
        if (auto l = layout(tr(S::FrozenBadge), size, std::max(40.f, pw * 0.6f), DWRITE_FONT_WEIGHT_BOLD, false)) {
            const float tw = textW(l.Get()), th = textH(l.Get());
            const float icon = size, padX = size * 0.6f, padY = size * 0.3f, gap = size * 0.35f;
            const float w = padX + icon + gap + tw + padX, h = th + 2 * padY;
            const float x1 = std::round(pic.right - inset);
            float y0 = std::round(pic.top + std::max(size * 0.7f, radius * 0.30f));
            // Device frame, portrait: below the Dynamic Island if they would touch.
            if (radius > 0 && ph > pw && x1 - w < pic.left + pw * 0.67f) y0 = std::round(pic.top + pw * 0.122f + 6);
            b.frozen = {x1 - w, y0, x1, y0 + h};
        }
    }
    return b;
}

void Renderer::drawMagnifierUi(const D2D1_RECT_F& pic, float radius, double now) {
    const float s = dpi_ / 96.f;
    const float pw = pic.right - pic.left, ph = pic.bottom - pic.top;
    const D2D1_COLOR_F hi = D2D1::ColorF(1, 0.92f, 0.1f);  // high-contrast yellow
    wchar_t num[16];
    swprintf_s(num, std::fabs(view_.zoom - std::round(view_.zoom)) > 0.04f ? L"%.1f" : L"%.0f", view_.zoom);
    // Overview: the whole picture (drawn by D3D) with the magnified part outlined.
    if (view_.zoom > 1.001f && miniRect_.right > miniRect_.left) {
        const D2D1_RECT_F m{miniRect_.left / s, miniRect_.top / s, miniRect_.right / s, miniRect_.bottom / s};
        d2dTarget_->DrawRectangle({m.left - 1.5f, m.top - 1.5f, m.right + 1.5f, m.bottom + 1.5f}, brush(pal_.card, 0.95f),
                                  3.f);
        const float mw = m.right - m.left, mh = m.bottom - m.top, half = 0.5f / view_.zoom;
        const D2D1_RECT_F v{m.left + (view_.cx - half) * mw, m.top + (view_.cy - half) * mh,
                            m.left + (view_.cx + half) * mw, m.top + (view_.cy + half) * mh};
        d2dTarget_->DrawRectangle(v, brush(D2D1::ColorF(0, 0, 0), 0.7f), 4.f);
        d2dTarget_->DrawRectangle(v, brush(hi), 2.f);
    }
    // Persistent zoom badge (bottom left): magnifier glyph + "2.5x".
    if (view_.zoom > 1.001f) {
        const float size = std::clamp(std::min(pw, ph) / 16.f, 16.f, 30.f);
        if (auto l = layout(std::wstring(num) + L"×", size, 400, DWRITE_FONT_WEIGHT_BOLD, false, true)) {
            const float tw = textW(l.Get());
            const float icon = size * 1.05f, padX = size * 0.55f, padY = size * 0.28f, gap = size * 0.3f;
            const D2D1_RECT_F zr = badgeRects(pic, radius).zoom;
            const float h = zr.bottom - zr.top, w = zr.right - zr.left;
            const float x0 = zr.left, y0 = zr.top;
            const D2D1_ROUNDED_RECT pill{{x0, y0, x0 + w, y0 + h}, h / 2, h / 2};
            d2dTarget_->FillRoundedRectangle(pill, brush(D2D1::ColorF(0, 0, 0), 0.78f));
            d2dTarget_->DrawRoundedRectangle(pill, brush(hi, 0.9f), 2.f);
            if (auto ic = iconFormat(icon)) {
                const wchar_t g[2] = {0xE71E, 0};  // Zoom
                d2dTarget_->DrawText(g, 1, ic.Get(), {x0 + padX, y0, x0 + padX + icon, y0 + h}, brush(hi));
            }
            l->SetMaxWidth(std::ceil(tw) + 2.f);
            d2dTarget_->DrawTextLayout({x0 + padX + icon + gap, y0 + padY}, l.Get(), brush(D2D1::ColorF(1, 1, 1)));
        }
    }
    // Big indicator right after a change: "放大 2.5x" in the upper third.
    const double e = now - zoomAt_;
    if (e < kZoomShowMs + ms(kZoomFadeMs)) {
        const float a = e < kZoomShowMs ? 1.f : 1.f - ease((e - kZoomShowMs) / ms(kZoomFadeMs));
        const float size = std::clamp(std::min(pw, ph) / 7.f, 26.f, 72.f);
        const std::wstring t = view_.zoom > 1.001f ? pm::i18n::fmt(S::MagZoomFmt, {num}) : tr(S::MagZoomOff);
        if (auto l = layout(t, size, std::max(40.f, pw - 16), DWRITE_FONT_WEIGHT_BOLD, false); l && a > 0.003f) {
            const float tw = std::min(textW(l.Get()), pw - 16), th = textH(l.Get());
            const float padX = size * 0.6f, padY = size * 0.25f;
            const float w = tw + 2 * padX, h = th + 2 * padY;
            const float x0 = std::round(pic.left + (pw - w) / 2), y0 = std::round(pic.top + ph * 0.30f - h / 2);
            const D2D1_ROUNDED_RECT pill{{x0, y0, x0 + w, y0 + h}, size * 0.45f, size * 0.45f};
            d2dTarget_->FillRoundedRectangle(pill, brush(D2D1::ColorF(0, 0, 0), 0.80f * a));
            d2dTarget_->DrawRoundedRectangle(pill, brush(hi, a), 3.f);
            l->SetMaxWidth(std::ceil(tw) + 2.f);
            d2dTarget_->DrawTextLayout({x0 + padX - 1.f, y0 + padY}, l.Get(), brush(D2D1::ColorF(1, 1, 1), a));
        }
    }
    // Frozen badge (top right): pause glyph + "畫面已凍結".
    if (frozenOn_ && frozen_.valid()) {
        const float size = std::clamp(std::min(pw, ph) / 22.f, 13.f, 24.f);
        if (auto l = layout(tr(S::FrozenBadge), size, std::max(40.f, pw * 0.6f), DWRITE_FONT_WEIGHT_BOLD, false)) {
            const float tw = textW(l.Get());
            const float icon = size, padX = size * 0.6f, padY = size * 0.3f, gap = size * 0.35f;
            const D2D1_RECT_F fr = badgeRects(pic, radius).frozen;
            const float w = fr.right - fr.left, h = fr.bottom - fr.top;
            const float x1 = fr.right, y0 = fr.top;
            const D2D1_ROUNDED_RECT pill{{x1 - w, y0, x1, y0 + h}, h / 2, h / 2};
            d2dTarget_->FillRoundedRectangle(pill, brush(pal_.card, 0.92f));
            d2dTarget_->DrawRoundedRectangle(pill, brush(pal_.accent, 0.9f), 2.f);
            if (auto ic = iconFormat(icon)) {
                const wchar_t g[2] = {0xE769, 0};  // Pause
                d2dTarget_->DrawText(g, 1, ic.Get(), {x1 - w + padX, y0, x1 - w + padX + icon, y0 + h},
                                     brush(pal_.accent));
            }
            l->SetMaxWidth(std::ceil(tw) + 2.f);
            d2dTarget_->DrawTextLayout({x1 - w + padX + icon + gap, y0 + padY}, l.Get(), brush(pal_.fg));
        }
    }
}

void Renderer::drawSelection(const D2D1_RECT_F& pic, float radius) {
    auto veil = brush(D2D1::ColorF(0, 0, 0), 0.45f);
    const float pw = pic.right - pic.left, ph = pic.bottom - pic.top;
    if (sel_[0] >= 0) {
        const D2D1_RECT_F r{pic.left + std::min(sel_[0], sel_[2]) * pw, pic.top + std::min(sel_[1], sel_[3]) * ph,
                            pic.left + std::max(sel_[0], sel_[2]) * pw, pic.top + std::max(sel_[1], sel_[3]) * ph};
        d2dTarget_->FillRectangle({pic.left, pic.top, pic.right, r.top}, veil);
        d2dTarget_->FillRectangle({pic.left, r.bottom, pic.right, pic.bottom}, veil);
        d2dTarget_->FillRectangle({pic.left, r.top, r.left, r.bottom}, veil);
        d2dTarget_->FillRectangle({r.right, r.top, pic.right, r.bottom}, veil);
        d2dTarget_->DrawRectangle(r, brush(D2D1::ColorF(0, 0, 0), 0.8f), 4.f);
        d2dTarget_->DrawRectangle(r, brush(D2D1::ColorF(1, 0.92f, 0.1f)), 2.f);
    } else {
        d2dTarget_->FillRectangle(pic, brush(D2D1::ColorF(0, 0, 0), 0.25f));
    }
    const float size = std::clamp(std::min(pw, ph) / 26.f, 13.f, 22.f);
    if (auto l = layout(tr(S::SelectHint), size, std::max(40.f, pw - 24), DWRITE_FONT_WEIGHT_SEMI_BOLD, true)) {
        const float tw = std::min(textW(l.Get()), pw - 24), th = textH(l.Get());
        const float padX = size * 0.8f, padY = size * 0.45f, w = tw + 2 * padX, h = th + 2 * padY;
        const float x0 = std::round(pic.left + (pw - w) / 2);
        float y0 = std::round(pic.top + std::max(12.f, ph * 0.04f));
        // Below the 畫面已凍結 badge (top right) instead of over it (0.7.2).
        const D2D1_RECT_F fb = badgeRects(pic, radius).frozen;
        if (fb.right > fb.left && x0 + w > fb.left - 6 && y0 < fb.bottom + 6) y0 = std::round(fb.bottom + 8);
        const float rr = std::min(h / 2, size * 1.2f);
        const D2D1_ROUNDED_RECT pill{{x0, y0, x0 + w, y0 + h}, rr, rr};
        d2dTarget_->FillRoundedRectangle(pill, brush(pal_.card, 0.95f));
        d2dTarget_->DrawRoundedRectangle(pill, brush(pal_.accent, 0.6f), 1.25f);
        l->SetMaxWidth(std::ceil(tw) + 2.f);
        d2dTarget_->DrawTextLayout({x0 + padX - 1.f, y0 + padY}, l.Get(), brush(pal_.fg));
    }
}

void Renderer::drawBusy(const D2D1_RECT_F& pic, double now) {
    const float pw = pic.right - pic.left, ph = pic.bottom - pic.top;
    const float a = ease((now - busyAt_) / 150.0);
    const float size = std::clamp(std::min(pw, ph) / 20.f, 14.f, 26.f);
    auto l = layout(pm::i18n::keepWords(busy_), size, std::max(40.f, pw - 40), DWRITE_FONT_WEIGHT_SEMI_BOLD, true);
    if (!l) return;
    const float tw = std::min(textW(l.Get()), pw - 40), th = textH(l.Get());
    const float spin = size * 1.1f, padX = size * 0.9f, padY = size * 0.6f, gap = size * 0.6f;
    const float w = padX + spin + gap + tw + padX, h = std::max(th, spin) + 2 * padY;
    const float x0 = std::round(pic.left + (pw - w) / 2), y0 = std::round(pic.top + (ph - h) / 2);
    const float rr = std::min(h / 2, size * 1.3f);
    const D2D1_ROUNDED_RECT pill{{x0, y0, x0 + w, y0 + h}, rr, rr};
    d2dTarget_->FillRoundedRectangle(pill, brush(pal_.card, 0.95f * a));
    d2dTarget_->DrawRoundedRectangle(pill, brush(pal_.accent, 0.5f * a), 1.25f);
    drawSpinner({x0 + padX + spin / 2, y0 + h / 2}, spin / 2, now);
    l->SetMaxWidth(std::ceil(tw) + 2.f);
    d2dTarget_->DrawTextLayout({x0 + padX + spin + gap - 1.f, y0 + (h - th) / 2}, l.Get(), brush(pal_.fg, a));
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
