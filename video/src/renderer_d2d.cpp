// Renderer: Direct2D / DirectWrite objects, brushes, text formats and layouts, background.
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

Renderer::Layout Renderer::layoutFor(bool mascot) const {
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
    if (mascot && toutou_.loaded()) {
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

}  // namespace pm::video
