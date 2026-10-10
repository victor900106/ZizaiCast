// Renderer: magnifier UI (minimap, badges), region selection, busy indicator.
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

}  // namespace pm::video
