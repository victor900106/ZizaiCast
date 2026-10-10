// Renderer: device frame around the picture and the REC badge.
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
    // The combined geometries depend only on the phone's size: kept (per
    // render thread and factory) until it changes, not rebuilt every frame.
    struct RingCache {
        ID2D1Factory* f = nullptr;
        float key[5] = {};
        ComPtr<ID2D1PathGeometry> ring[2];
    };
    thread_local RingCache cache;
    const float key[5] = {pw, ph, R, b, e};
    if (cache.f != d2d_.Get() || std::memcmp(cache.key, key, sizeof key) != 0) {
        cache = {};
        cache.f = d2d_.Get();
        std::memcpy(cache.key, key, sizeof key);
    }
    auto fillRing = [&](int slot, const D2D1_ROUNDED_RECT& a, const D2D1_ROUNDED_RECT& cut, ID2D1Brush* br) {
        ComPtr<ID2D1PathGeometry>& ring = cache.ring[slot];
        if (!ring) {
            ComPtr<ID2D1RoundedRectangleGeometry> ga, gc;
            ComPtr<ID2D1PathGeometry> g;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(d2d_->CreateRoundedRectangleGeometry(a, &ga)) &&
                SUCCEEDED(d2d_->CreateRoundedRectangleGeometry(cut, &gc)) && SUCCEEDED(d2d_->CreatePathGeometry(&g)) &&
                SUCCEEDED(g->Open(&sink)) &&
                SUCCEEDED(ga->CombineWithGeometry(gc.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get())) &&
                SUCCEEDED(sink->Close()))
                ring = g;
        }
        if (ring) rt->FillGeometry(ring.Get(), br);
    };
    const D2D1_ROUNDED_RECT outerR{inflate(scr, b + e), R + b + e, R + b + e};
    const D2D1_ROUNDED_RECT bodyR{inflate(scr, b), R + b, R + b};
    const D2D1_ROUNDED_RECT screenR{scr, R, R};
    fillRing(0, outerR, {inflate(scr, b - 0.5f), R + b - 0.5f, R + b - 0.5f}, metal.Get());  // overlaps the body: no seam
    solid->SetColor(D2D1::ColorF(0x0A0A0C));
    fillRing(1, bodyR, screenR, solid.Get());
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

}  // namespace pm::video
