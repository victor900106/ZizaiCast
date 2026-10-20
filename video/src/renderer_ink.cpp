// Renderer: 簡報畫筆 (0.8.3) -- the strokes over the live picture and the
// 「畫筆模式」 badge.  The strokes live in ink_layer.* (U space of the source
// picture); here they are mapped through the same rotation / mirror / zoom
// as the picture itself.
#include "renderer.h"

#include <algorithm>
#include <cmath>

#include "ink_layer.h"
#include "renderer_internal.h"

namespace pm::video {

using namespace detail;

D2D1_MATRIX_3X2_F Renderer::inkMatrix(float ax, float ay, int rot, bool mirror, const View& view,
                                      const D2D1_RECT_F& target) {
    float u[4], v[4];
    pictureTransform(rot, mirror, u, v);  // display t -> source s (the vertex shader's mapping)
    const float det = u[0] * v[1] - u[1] * v[0];
    const float zoom = std::max(1.f, view.zoom);
    auto map = [&](float ux, float uy) {
        const float sx = ux / ax, sy = uy / ay;  // U -> source 0..1
        const float dx = sx - u[2], dy = sy - v[2];
        const float tx = (v[1] * dx - u[1] * dy) / det, ty = (-v[0] * dx + u[0] * dy) / det;  // source -> display
        const float vx = (tx - view.cx) * zoom + 0.5f, vy = (ty - view.cy) * zoom + 0.5f;    // display -> viewport
        return D2D1_POINT_2F{target.left + vx * (target.right - target.left), target.top + vy * (target.bottom - target.top)};
    };
    const D2D1_POINT_2F o = map(0, 0), x = map(1, 0), y = map(0, 1);
    return D2D1::Matrix3x2F(x.x - o.x, x.y - o.y, y.x - o.x, y.y - o.y, o.x, o.y);
}

void Renderer::drawInk(const D2D1_RECT_F& pic) {
    if (!ink_ || ink_->empty() || !d2dTarget_) {
        if (inkCache_) inkCache_.Reset(), inkCacheKey_.clear();  // nothing to show: the bitmap goes too
        return;
    }
    int dw = 0, dh = 0;
    displaySize(dw, dh);
    if (dw <= 0 || dh <= 0) return;
    float a = ink_->aspect();
    if (a <= 0) a = (rot_ & 1) ? static_cast<float>(dh) / dw : static_cast<float>(dw) / dh;
    float ax, ay;
    InkLayer::axes(a, ax, ay);
    const float s = dpi_ / 96.f;
    const UINT pw = static_cast<UINT>(std::lround((pic.right - pic.left) * s)),
               ph = static_cast<UINT>(std::lround((pic.bottom - pic.top) * s));
    if (!pw || !ph) return;
    // The bitmap's own coordinates: the picture rectangle at (0, 0).
    const D2D1_RECT_F local{0, 0, pic.right - pic.left, pic.bottom - pic.top};
    const D2D1_MATRIX_3X2_F m = inkMatrix(ax, ay, rot_, mirror_, view_, local);
    const std::vector<float> key{static_cast<float>(ink_->version() & 0xFFFFFF), static_cast<float>(ink_->version() >> 24),
                                 static_cast<float>(pw), static_cast<float>(ph), s, m._11, m._12, m._21, m._22, m._31, m._32};
    if (!inkCache_ || key != inkCacheKey_) {
        D2D1_SIZE_U px{pw, ph};
        D2D1_SIZE_U have{};
        if (inkCache_) {
            ComPtr<ID2D1Bitmap> b;
            if (SUCCEEDED(inkCache_->GetBitmap(&b))) have = b->GetPixelSize();
        }
        if (!inkCache_ || have.width != pw || have.height != ph) {
            inkCache_.Reset();
            if (FAILED(d2dTarget_->CreateCompatibleRenderTarget(D2D1::SizeF(local.right, local.bottom), px, &inkCache_))) {
                inkCache_.Reset();  // fallback: straight onto the target
                d2dTarget_->PushAxisAlignedClip(pic, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                ink_->draw(d2dTarget_.Get(), d2d_.Get(), inkMatrix(ax, ay, rot_, mirror_, view_, pic));
                d2dTarget_->PopAxisAlignedClip();
                return;
            }
        }
        inkCache_->BeginDraw();
        inkCache_->SetTransform(D2D1::IdentityMatrix());
        inkCache_->Clear(D2D1::ColorF(0, 0, 0, 0));
        ink_->draw(inkCache_.Get(), d2d_.Get(), m);
        if (FAILED(inkCache_->EndDraw())) {
            inkCache_.Reset();
            inkCacheKey_.clear();
            return;
        }
        inkCacheKey_ = key;
    }
    ComPtr<ID2D1Bitmap> bmp;
    if (SUCCEEDED(inkCache_->GetBitmap(&bmp))) d2dTarget_->DrawBitmap(bmp.Get(), pic);
}

// 「畫筆模式 · Esc 結束」: a small pill at the bottom left of the picture.
void Renderer::drawInkBadge(const D2D1_RECT_F& pic, float radius) {
    if (inkBadge_.empty() || !d2dTarget_) return;
    const float pw = pic.right - pic.left;
    const float size = std::clamp(std::min(pw, pic.bottom - pic.top) * 0.03f, 11.5f, 14.f);
    const float m = std::max(10.f, radius * 0.35f);
    auto text = layout(inkBadge_, size, std::max(40.f, pw - 2 * m - size * 3), DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    if (!text) return;
    const float tw = textW(text.Get()), th = textH(text.Get());
    text->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);  // (layout() centres in its maximum width)
    text->SetMaxWidth(std::ceil(tw) + 2.f);
    const float padX = size * 0.75f, padY = size * 0.4f, icon = size * 1.25f;
    const float h = th + 2 * padY, w = padX + icon + size * 0.4f + tw + padX;
    // Bottom left (the toolbar and the REC badge are at the top), above the
    // magnifier's zoom badge while zoomed.
    float x = std::round(pic.left + m), y = std::round(pic.bottom - m - h);
    const Badges b = badgeRects(pic, radius);
    if (b.zoom.right > b.zoom.left) y = std::round(b.zoom.top - 8 - h);
    const D2D1_ROUNDED_RECT r{{x, y, x + w, y + h}, h / 2, h / 2};
    d2dTarget_->FillRoundedRectangle({{x, y + 1, x + w, y + h + 2}, h / 2, h / 2}, brush(D2D1::ColorF(0, 0, 0), 0.22f));
    d2dTarget_->FillRoundedRectangle(r, brush(pal_.card, 0.94f));
    d2dTarget_->DrawRoundedRectangle(r, brush(pal_.accent, 0.55f), 1.f);
    if (auto f = iconFormat(icon * 0.86f)) {
        const wchar_t g[2] = {0xE70F, 0};  // Edit (pencil)
        d2dTarget_->DrawText(g, 1, f.Get(), {x + padX, y, x + padX + icon, y + h}, brush(pal_.accent));
    }
    d2dTarget_->DrawTextLayout({x + padX + icon + size * 0.4f, y + padY}, text.Get(), brush(pal_.fg));
}

// 拖檔到視窗 (0.8.3): a veil, a dashed accent frame inside the window and a
// centred card (glyph in an accent disc, the title, a dim sub line).  Static:
// drawn on the frames the drag causes, never animated.
void Renderer::drawDropHint() {
    if (dropTitle_.empty() || !d2dTarget_ || !d2d_) return;
    const float s = dpi_ / 96.f, W = width_ / s, H = height_ / s;
    d2dTarget_->FillRectangle({0, 0, W, H}, brush(D2D1::ColorF(0, 0, 0), 0.45f));
    const float inset = std::clamp(std::min(W, H) * 0.035f, 8.f, 18.f);
    ComPtr<ID2D1StrokeStyle> dash;
    D2D1_STROKE_STYLE_PROPERTIES sp{};
    sp.startCap = sp.endCap = sp.dashCap = D2D1_CAP_STYLE_ROUND;
    sp.lineJoin = D2D1_LINE_JOIN_ROUND;
    sp.dashStyle = D2D1_DASH_STYLE_DASH;
    d2d_->CreateStrokeStyle(sp, nullptr, 0, &dash);
    d2dTarget_->DrawRoundedRectangle({{inset, inset, W - inset, H - inset}, 16, 16}, brush(pal_.accent, 0.95f), 2.5f,
                                     dash.Get());
    const float size = std::clamp(std::min(W, H) * 0.045f, 15.f, 24.f);
    const float maxW = std::max(80.f, std::min(W - 4 * inset, 520.f));
    auto title = layout(dropTitle_, size, maxW - size * 2, DWRITE_FONT_WEIGHT_BOLD, true);
    auto sub = dropSub_.empty() ? ComPtr<IDWriteTextLayout>() : layout(dropSub_, size * 0.62f, maxW - size * 2);
    if (!title) return;
    const float disc = size * 2.6f, gap = size * 0.6f;
    const float tw = textW(title.Get()), th = textH(title.Get());
    const float sw = sub ? textW(sub.Get()) : 0, sh = sub ? textH(sub.Get()) : 0;
    const float cw = std::min(maxW, std::max({tw, sw, disc}) + size * 2);
    const float ch = size + disc + gap + th + (sub ? gap * 0.6f + sh : 0) + size;
    const float cx = std::round((W - cw) / 2), cy = std::round((H - ch) / 2);
    const D2D1_ROUNDED_RECT card{{cx, cy, cx + cw, cy + ch}, 18, 18};
    d2dTarget_->FillRoundedRectangle({{cx, cy + 3, cx + cw, cy + ch + 5}, 18, 18}, brush(D2D1::ColorF(0, 0, 0), 0.3f));
    d2dTarget_->FillRoundedRectangle(card, brush(pal_.card, 0.97f));
    d2dTarget_->DrawRoundedRectangle(card, brush(pal_.accent, 0.5f), 1.f);
    const D2D1_POINT_2F dc{cx + cw / 2, cy + size + disc / 2};
    d2dTarget_->FillEllipse({dc, disc / 2, disc / 2}, brush(pal_.accent, 0.22f));
    d2dTarget_->DrawEllipse({dc, disc / 2 - 0.75f, disc / 2 - 0.75f}, brush(pal_.accent, 0.8f), 1.5f);
    if (auto f = iconFormat(disc * 0.42f)) {
        const wchar_t g[2] = {0xE898, 0};  // Upload (arrow out of a tray)
        d2dTarget_->DrawText(g, 1, f.Get(), {dc.x - disc / 2, dc.y - disc / 2, dc.x + disc / 2, dc.y + disc / 2},
                             brush(pal_.accent));
    }
    float y = cy + size + disc + gap;
    title->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    title->SetMaxWidth(cw - size * 2);
    d2dTarget_->DrawTextLayout({cx + size, y}, title.Get(), brush(pal_.fg));
    y += th + gap * 0.6f;
    if (sub) {
        sub->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        sub->SetMaxWidth(cw - size * 2);
        d2dTarget_->DrawTextLayout({cx + size, y}, sub.Get(), brush(pal_.dim));
    }
}

}  // namespace pm::video
