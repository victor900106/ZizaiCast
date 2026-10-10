// Renderer: live toolbar (state, reveal note, drawing).
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

// One-time introduction (revealToolbar): once the toolbar is available on a
// live picture it fades in by itself and stays revealMs_, the callout with it.
void Renderer::startReveal(double now) {
    if (revealMs_ <= 0 || !toolbarAvailable()) return;
    const float a = toolbarAlpha(now);
    if (a < 1) toolInAt_ = now - ms(kToolInMs) * (1 - std::cbrt(1 - a));
    toolUntil_ = std::max(toolUntil_, now + revealMs_);
    revealUntil_ = now + revealMs_;
    revealMs_ = 0;
}

// The callout under the pill: a small accent-bordered card with an arrow up
// to the pill; fades with the toolbar and at revealUntil_.
void Renderer::drawRevealNote(const D2D1_RECT_F& pill, float a, double now) {
    if (revealNote_.empty() || now >= revealUntil_ + ms(kToolOutMs)) return;
    const float na = a * (now < revealUntil_ ? 1.f : 1 - ease((now - revealUntil_) / ms(kToolOutMs)));
    if (na <= 0.003f) return;
    const float W = width_ / (dpi_ / 96.f);
    const float size = 13.5f;
    auto l = layout(revealNote_, size, std::min(W - 40, 420.f), DWRITE_FONT_WEIGHT_NORMAL, true);
    if (!l) return;
    const float tw = textW(l.Get()), th = textH(l.Get());
    const float padX = 14, padY = 9, arrow = 7;
    const float bw = tw + 2 * padX, bh = th + 2 * padY;
    const float cx = (pill.left + pill.right) / 2;
    float bx = std::round(cx - bw / 2);
    bx = std::clamp(bx, 6.f, std::max(6.f, W - bw - 6));
    const float by = std::round(pill.bottom + arrow + 6);
    const D2D1_ROUNDED_RECT card{{bx, by, bx + bw, by + bh}, 10, 10};
    d2dTarget_->FillRoundedRectangle({{bx, by + 1, bx + bw, by + bh + 3}, 10, 10}, brush(D2D1::ColorF(0, 0, 0), 0.28f * na));
    // Arrow: a small triangle from the card's top edge up towards the pill.
    ComPtr<ID2D1PathGeometry> tri;
    if (d2d_ && SUCCEEDED(d2d_->CreatePathGeometry(&tri))) {
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(tri->Open(&sink))) {
            sink->BeginFigure({cx - arrow, by + 1}, D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine({cx, by - arrow});
            sink->AddLine({cx + arrow, by + 1});
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            d2dTarget_->FillGeometry(tri.Get(), brush(pal_.accent, 0.95f * na));
        }
    }
    d2dTarget_->FillRoundedRectangle(card, brush(pal_.card, 0.97f * na));
    d2dTarget_->DrawRoundedRectangle({{bx + 0.75f, by + 0.75f, bx + bw - 0.75f, by + bh - 0.75f}, 9.25f, 9.25f},
                                     brush(pal_.accent, 0.95f * na), 1.5f);
    l->SetMaxWidth(std::ceil(tw) + 2.f);
    d2dTarget_->DrawTextLayout({bx + padX - 1.f, by + padY}, l.Get(), brush(pal_.fg, na));
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
    // 0.7.8: the most used buttons (截圖 錄影 翻譯 放大) get a small caption under
    // the icon (all of them or none).  A button is as wide as its caption needs
    // (two CJK characters fit in the button itself); the pill grows by one
    // caption line and may reach past a narrow picture, never past the window.
    // 0.7.9: too wide (English 「Translate」 in a 540 px window at 150 %)?
    // Once more with a smaller caption and less air around it before none.
    float capSize = std::clamp(11.f * btn / 36.f, 9.5f, 11.f);
    std::vector<ComPtr<IDWriteTextLayout>> labs(n);
    std::vector<float> labW(n, 0.f);  // extra width beyond btn
    float labelExtra = 0;
    bool labelled = false;
    const float pillW0 = 2 * pad + shownU * btn + std::max(0, shownN - 1) * gap + groups * div;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const float air = attempt == 0 ? 8.f : 4.f;
        if (attempt == 1) capSize = 9.f;
        labs.assign(n, nullptr);
        labW.assign(n, 0.f);
        labelExtra = 0;
        labelled = false;
        for (int i = 0; i < n; ++i) {
            const ToolItem& it = toolItems_[i];
            if (!vis[i] || it.label.empty() || it.slider >= 0) continue;
            if (!(labs[i] = layout(it.label, capSize, 200.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false))) continue;
            labW[i] = std::max(0.f, std::ceil(textW(labs[i].Get())) + air - btn);
            labelExtra += labW[i];
            labelled = true;
        }
        if (!labelled || pillW0 + labelExtra <= std::max(W - 8, pillW0)) break;
    }
    if (labelled && pillW0 + labelExtra > std::max(W - 8, pillW0)) {  // (W - 8: the pill's widest)
        labs.assign(n, nullptr);
        labW.assign(n, 0.f);
        labelExtra = 0;
        labelled = false;
    }
    const float capH = labelled ? std::round(capSize * 1.3f - btn * 0.10f) : 0.f;  // the caption overlaps the icon's margin
    const float pillW = pillW0 + labelExtra, pillH = btn + 2 * pad + capH;
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
        // A labelled button: icon with the caption under it, a rounded tile
        // instead of the disc.  Icons stay on one row at the top of the pill.
        const D2D1_RECT_F ib{x + labW[i] / 2, pill.top + pad, x + labW[i] / 2 + btn, pill.top + pad + btn};  // icon square
        const bool lab = labs[i] != nullptr;
        const D2D1_RECT_F b = lab ? D2D1_RECT_F{x, ib.top, x + btn + labW[i], ib.bottom + capH} : ib;
        const D2D1_ELLIPSE disc{{(ib.left + ib.right) / 2, (ib.top + ib.bottom) / 2}, btn / 2, btn / 2};
        const float cr = btn * 0.32f;
        const D2D1_ROUNDED_RECT cap{b, cr, cr};
        auto fillShape = [&](ID2D1Brush* br) {
            if (lab) d2dTarget_->FillRoundedRectangle(cap, br);
            else d2dTarget_->FillEllipse(disc, br);
        };
        const bool hot = i == toolHot_;
        // Hover eases the wash in (120 ms) and out (180 ms); a press shrinks the button to 92 %.
        const float hl = uiLevel(uiHot_, UiTool, i, now), pl = reduced_ ? 0.f : uiLevel(uiPress_, UiTool, i, now);
        D2D1_MATRIX_3X2_F base;
        d2dTarget_->GetTransform(&base);
        if (pl > 0.003f)
            d2dTarget_->SetTransform(scaledAbout(base, 1 - 0.08f * pl, {(b.left + b.right) / 2, (b.top + b.bottom) / 2}));
        D2D1_COLOR_F ink = it.danger ? kDangerRed : pal_.fg;
        if (it.toggled && it.recording) {
            fillShape(brush(kRecRed, (0.24f + 0.12f * hl) * a));
            ink = kRecRed;
        } else if (it.toggled) {  // 放大鏡 / 翻譯 / 凍結 on: the theme's accent, not REC red
            fillShape(brush(pal_.accent, (0.30f + 0.12f * hl) * a));
            if (lab)
                d2dTarget_->DrawRoundedRectangle({inflate(b, -0.75f), cr - 0.75f, cr - 0.75f},
                                                 brush(pal_.accent, 0.9f * a), 1.5f);
            else
                d2dTarget_->DrawEllipse({disc.point, disc.radiusX - 0.75f, disc.radiusY - 0.75f},
                                        brush(pal_.accent, 0.9f * a), 1.5f);
        } else if (it.danger) {
            if (hl > 0.003f) fillShape(brush(kDangerRed, 0.92f * hl * a));
            ink = mixc(kDangerRed, D2D1::ColorF(1, 1, 1), hl);
        } else if (hl > 0.003f) {
            fillShape(brush(pal_.accent, 0.26f * hl * a));
        }
        if (icons && it.glyph) {
            const wchar_t g[2] = {it.glyph, 0};
            d2dTarget_->DrawText(g, 1, icons.Get(), ib, brush(ink, a));
        }
        if (lab) {  // centred under the icon (the layout centres it in the button's width)
            IDWriteTextLayout* tl = labs[i].Get();
            tl->SetMaxWidth(b.right - b.left + 2.f);
            d2dTarget_->DrawTextLayout({b.left - 1.f, std::round(ib.bottom - btn * 0.10f)}, tl,
                                       brush(it.toggled || it.danger ? ink : alphaOf(pal_.fg, 0.92f), a));
        }
        if (it.toggled && it.recording) {  // pulsing dot: recording now
            const float pulse = recPulse(now);
            d2dTarget_->FillEllipse({{ib.right - btn * 0.2f, ib.top + btn * 0.2f}, btn * 0.08f, btn * 0.08f},
                                    brush(kRecRed, (0.55f + 0.45f * pulse) * a));
        }
        d2dTarget_->SetTransform(base);
        if (hot) hotRect = b;
        if (interactive)
            toolRects_.push_back({static_cast<LONG>(std::floor(b.left * s)), static_cast<LONG>(std::floor(b.top * s)),
                                  static_cast<LONG>(std::ceil(b.right * s)), static_cast<LONG>(std::ceil(b.bottom * s))});
        x += btn + labW[i] + gap;
    }
    if (interactive)
        toolPill_ = {static_cast<LONG>(std::floor(pill.left * s)), static_cast<LONG>(std::floor(pill.top * s)),
                     static_cast<LONG>(std::ceil(pill.right * s)), static_cast<LONG>(std::ceil(pill.bottom * s))};

    if (toolHot_ < 0) drawRevealNote(pill, a, now);  // (a hovered button's tooltip goes there instead)
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

}  // namespace pm::video
