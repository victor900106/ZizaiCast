// Renderer: idle screen (options, scene, idle cards, text blocks, actions).
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
    sceneOpacity_ = opacity;
    // 「請選你的手機」 in a window too small for 投投 and the cards (the
    // title would have to shrink or the cards still not fit, or the text column
    // is under ~240 DIPs): the cards take the whole window, no 投投.
    idleNoMascot_ = false;
    if (scene == Scene::Idle && !paused_ && !cards_.empty() && toutou_.loaded()) {
        const Layout Lm = layoutFor(true);
        const IdlePlan p = planIdleCards(Lm, true);
        // 0.8.2: a page with the big switch / QR codes keeps its check boxes and card
        // text: 投投 makes room as soon as anything would be dropped.
        idleNoMascot_ = !p.fits || p.level >= (richIdle() ? 1 : 3) || Lm.text.right - Lm.text.left < 240;
    }
    const Layout L = layoutFor();
    // The beam keeps off the cards (last frame's; the text is drawn after 投投).
    beamAvoid_ = cardsRect_;
    cardsRect_ = {};
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
        float r = 150 * L.u;
        if (beamAvoid_.right > beamAvoid_.left) {  // ends before the cards
            const float dx = std::max({beamAvoid_.left - gc.x, 0.f, gc.x - beamAvoid_.right});
            const float dy = std::max({beamAvoid_.top - gc.y, 0.f, gc.y - beamAvoid_.bottom});
            r = std::min(r, std::max(std::hypot(dx, dy), r * 0.4f));
        }
        glowBrush_->SetCenter(gc);
        glowBrush_->SetRadiusX(r);
        glowBrush_->SetRadiusY(r);
        glowBrush_->SetOpacity(paused ? 0.25f : busy ? 0.95f : 0.6f + 0.35f * env * breath);
        d2dTarget_->FillEllipse({gc, r, r}, glowBrush_.Get());
    }
    drawParticles(L, now, env);
    drawMascot(L, P, now);
    if (paused && L.u > 0) drawZzz(L, P, now);
    if (L.u > 0) drawMascotFx(L, P, now);

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
    if (L.u > 0) drawBubble(L, P, now);
    if (layered) d2dTarget_->PopLayer();
}

float Renderer::oneLineSize(const std::wstring& text, float size, float maxW) {
    const std::wstring t = keepAll(text);
    ComPtr<IDWriteTextLayout> probe;
    if (auto f = format(size, DWRITE_FONT_WEIGHT_NORMAL);
        f && SUCCEEDED(dwrite_->CreateTextLayout(t.c_str(), static_cast<UINT32>(t.size()), f.Get(), 1e5f, 1e4f, &probe))) {
        DWRITE_TEXT_METRICS m{};
        if (SUCCEEDED(probe->GetMetrics(&m)) && m.widthIncludingTrailingWhitespace > maxW)
            return std::max(size * 0.75f, size * maxW / m.widthIncludingTrailingWhitespace);
    }
    return size;
}

bool Renderer::optionsFit(float size, float regionW) {
    // Same measures as drawOptions.
    const float box = std::round(size * 1.15f), gap = size * 0.6f, padX = size * 0.7f;
    const float maxLabel = regionW - 32.f - 2 * padX - box - gap;
    for (auto& o : options_) {
        auto l = layout(o.label, size, 1e4f, DWRITE_FONT_WEIGHT_NORMAL, false);
        if (!l || textW(l.Get()) > maxLabel) return false;
    }
    return true;
}

Renderer::IdlePlan Renderer::planIdleCards(const Layout& L0, bool full) {
    IdlePlan p;
    // 0.8.2: page tabs at the top (drawTabs) take their band off the region.
    Layout L = L0;
    L.text.top += drawTabs(L0, false, false);
    const float rw = L.text.right - L.text.left, rh = L.text.bottom - L.text.top;
    const float margin = std::max(16.f, rw * 0.06f), textMax = rw - 2 * margin;
    if (cards_.empty() || textMax <= 20) return p;
    const std::wstring t1 = tr(S::IdleChooseTitle);
    const float title1 = oneLineSize(t1, L.title, textMax);
    // The rows under the cards, as drawTextBlock lays them out.
    const float rowH = std::round(L.hint * 2.3f), linkRowH = std::round(L.hint * 2.2f);
    const float actGap = std::round(L.hint * 0.7f), actRowGap = std::round(L.hint * 0.45f), gapOpts = L.hint * 1.5f;
    std::vector<ActionLayout> acts;
    std::vector<float> actRowW;
    const int actRows = full ? layoutActions(acts, actRowW, L.hint * 0.95f, linkRowH, rw - margin, actGap) : 0;
    const bool optsOk = full && !options_.empty() && optionsFit(L.hint, rw);
    const float room = rh - margin * 1.5f;
    // 0.8.2: cards with QR codes / several buttons (遠端控制) keep a readable size:
    // 投投 makes room instead (idleNoMascot_).
    const float cardMin = std::max(L.hint * (richIdle() ? 0.86f : 0.62f), 12.f * 0.78f);
    const float swH = drawSwitch(0, L, textMax, false, false);  // 0.8.2: the big switch in place of the title
    struct Step {
        int mode;
        bool opts;
        float titleK;  // 0: no title
    };
    static constexpr Step kSteps[] = {{0, true, 1}, {0, false, 1}, {1, false, 1}, {1, false, 0.7f}, {2, false, 0.7f}, {2, false, 0}};
    constexpr int kN = static_cast<int>(sizeof kSteps / sizeof kSteps[0]);
    for (int k = 0; k < kN; ++k) {
        const Step& st = kSteps[k];
        if (st.opts && !optsOk) continue;
        IdlePlan q;
        q.level = k;
        q.mode = st.mode;
        q.opts = st.opts && optsOk;
        q.title = st.titleK > 0;
        q.titleSize = q.title ? std::max(title1 * st.titleK, std::min(title1, L.hint * 1.25f)) : title1;
        float fixed = 0;
        if (swH > 0) fixed += swH;
        else if (q.title)
            if (auto l = layout(keepAll(t1), q.titleSize, textMax, DWRITE_FONT_WEIGHT_NORMAL)) fixed += textH(l.Get());
        if (q.opts) fixed += gapOpts + rowH * options_.size();
        if (actRows) fixed += (q.opts ? L.hint * 0.5f : L.hint * 1.2f) + linkRowH * actRows + actRowGap * (actRows - 1);
        q.gapCards = swH > 0 ? std::round(L.hint * 0.9f) : q.title ? q.titleSize * 0.6f : 0.f;
        const float avail = room - fixed - q.gapCards;
        q.cardSize = L.hint;
        q.cardsH = drawCards(0, 0, rw - margin, q.cardSize, false, false, q.mode);
        // (a few steps: rounding and re-wrapping keep it from scaling exactly)
        for (int i = 0; i < 5 && q.cardsH > avail && avail > 0 && q.cardSize > cardMin; ++i) {
            q.cardSize = std::max(cardMin, q.cardSize * avail / q.cardsH * 0.98f);
            q.cardsH = drawCards(0, 0, rw - margin, q.cardSize, false, false, q.mode);
        }
        // Last resort (under the smallest window anyway): smaller still, to 8 DIPs.
        if (k == kN - 1)
            for (int i = 0; i < 5 && q.cardsH > avail && avail > 0 && q.cardSize > 8.f; ++i) {
                q.cardSize = std::max(8.f, q.cardSize * avail / q.cardsH * 0.98f);
                q.cardsH = drawCards(0, 0, rw - margin, q.cardSize, false, false, q.mode);
            }
        q.block = fixed + q.gapCards + q.cardsH;
        q.fits = q.cardsH > 0 && q.block <= room;
        p = q;
        if (q.fits) return p;
    }
    p.level = -1;
    return p;
}

void Renderer::drawTextBlock(Scene scene, const Layout& L0, float opacity, double now) {
    if (opacity <= 0.003f) return;
    const bool layered = opacity < 0.997f && layer_;
    if (layered)
        d2dTarget_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                    D2D1::IdentityMatrix(), opacity),
                              layer_.Get());
    const bool paused = paused_ && scene != Scene::Connecting;
    const bool busy = scene == Scene::Connecting && !paused;
    // 0.8.2: the idle screen's page tabs (手機投影 | 遠端控制) at the top.
    const bool tabsOn = scene == Scene::Idle && !paused && !cards_.empty();
    const float tabsUsed = tabsOn ? drawTabs(L0, false, false) : 0.f;
    Layout L = L0;
    L.text.top += tabsUsed;
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
    } else if (!cards_.empty()) {
        t1 = tr(S::IdleChooseTitle);  // 0.7.8: 「請選你的手機」 over the cards
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
    float titleSize = oneLineSize(t1, L.title, textMax);
    // Check boxes and the help link only on a fully shown idle screen (they
    // are clickable).
    const bool full = opacity * sceneOpacity_ >= 0.997f;
    if (tabsUsed > 0) drawTabs(L0, true, full);
    // 0.7.8 「請選你的手機」: what fits (planIdleCards).
    const bool cardsOn = idle && !cards_.empty();
    IdlePlan plan;
    if (cardsOn) {
        plan = planIdleCards(L0, full);
        titleSize = plan.titleSize;
    }
    const float swH = cardsOn ? drawSwitch(0, L, textMax, false, false) : 0.f;  // 0.8.2: in place of the title
    auto l1 = (cardsOn && !plan.title) || swH > 0 ? nullptr : layout(keepAll(t1), titleSize, textMax, DWRITE_FONT_WEIGHT_NORMAL);
    // Device name: one line, trimmed with an ellipsis if it is very long.
    auto l2 = t2.empty()  ? nullptr
              : busy      ? layout(t2, std::max(L.hint, L.title * 0.72f), textMax, DWRITE_FONT_WEIGHT_NORMAL, false)
                          : layout(t2, L.hint, textMax);
    // Custom idle hints (setIdleHints): up to three lines, each wrapping.
    // A tab starts a muted suffix (smaller, fainter), e.g. 「（重新開機後可用）」.
    std::vector<ComPtr<IDWriteTextLayout>> hints;
    if (idle && cards_.empty())
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
    const bool showOpts = idle && !options_.empty() && full && (!cardsOn || plan.opts);
    // Actions (setIdleActions): pill buttons and links in centred rows.
    const float rowH = std::round(L.hint * 2.3f), linkRowH = std::round(L.hint * 2.2f);
    const float actGap = std::round(L.hint * 0.7f), actRowGap = std::round(L.hint * 0.45f);
    std::vector<ActionLayout> acts;
    std::vector<float> actRowW;
    const int actRows = idle && full ? layoutActions(acts, actRowW, L.hint * 0.95f, linkRowH, rw - margin, actGap) : 0;
    const float spin = busy ? L.title * 1.1f : 0, gapSpin = L.title * 0.8f;
    const float gap2 = L.title * 0.5f, gapOpts = L.hint * 1.5f;
    const float gapLink = showOpts ? L.hint * 0.5f : L.hint * 1.2f;
    float block = textH(l1.Get()) + swH;
    if (busy) block += spin + gapSpin;
    if (l2) block += gap2 + textH(l2.Get());
    for (size_t i = 0; i < hints.size(); ++i) block += (i ? hintGap : gap2) + textH(hints[i].Get());
    if (showOpts) block += gapOpts + rowH * options_.size();
    if (actRows) block += gapLink + linkRowH * actRows + actRowGap * (actRows - 1);
    // 0.7.8 cards, as planned (planIdleCards measures the same way).
    const float cardsH = cardsOn ? plan.cardsH : 0.f, gapCards = cardsOn ? plan.gapCards : 0.f;
    if (cardsOn) block += gapCards + cardsH;
    float y = std::round(L.text.top + std::max(margin * 0.75f, (rh - block) * (L.landscape ? 0.5f : 0.55f)));
    const float x = L.text.left + margin;
    if (opacity * sceneOpacity_ >= 0.5f) textBlock_ = {x, y - L.hint * 0.5f, L.text.right - margin, y + block + L.hint * 0.5f};
    if (busy) {
        drawSpinner({L.text.left + rw / 2, y + spin / 2}, spin / 2, now);
        y += spin + gapSpin;
    }
    if (swH > 0) {
        drawSwitch(y, L, textMax, true, full);
        y += swH;
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
    if (cardsH > 0) {
        y += gapCards;
        drawCards(L.text.left + margin / 2, y, rw - margin, plan.cardSize, true, full, plan.mode);
        y += cardsH;
        // The hearts fade over the cards too (they reach past the text column).
        if (opacity * sceneOpacity_ >= 0.5f && cardsRect_.right > cardsRect_.left) {
            textBlock_.left = std::min(textBlock_.left, cardsRect_.left);
            textBlock_.right = std::max(textBlock_.right, cardsRect_.right);
        }
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
        if (a.label.empty() || (a.card >= 0 && a.card < static_cast<int>(cards_.size()))) continue;  // in its card
        if (a.kind == 1 || a.kind == 2) continue;  // 0.8.2: drawn by drawTabs / drawSwitch
        const float padX = std::round(rowH * (a.primary ? 0.62f : 0.42f));
        auto l = layout(keepAll(a.label), size, std::max(maxW - 2 * padX, 20.f), DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
        if (!l) continue;
        if (!a.primary) l->SetUnderline(TRUE, {0, ~0u});
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
    if (actionRects_.size() < actions_.size()) actionRects_.resize(actions_.size());  // (the cards' buttons may be in)
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

// A check box (box + tick), as drawOptions draws it settled.
void Renderer::drawCheck(float bx, float by, float box, bool on, float hl) {
    D2D1_ROUNDED_RECT b{{bx, by, bx + box, by + box}, box * 0.28f, box * 0.28f};
    if (!on) {
        const D2D1_ROUNDED_RECT o{{bx + 0.75f, by + 0.75f, bx + box - 0.75f, by + box - 0.75f}, b.radiusX, b.radiusY};
        d2dTarget_->DrawRoundedRectangle(o, brush(mixc(alphaOf(pal_.dim, 0.85f), pal_.accent, hl)), 1.5f);
        return;
    }
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
}

// 0.8.2: the idle screen's page tabs (Action kind 1): a segmented pill at the top
// of the text region, the selected page filled with the accent.  Returns the
// height used (0: no tabs).
float Renderer::drawTabs(const Layout& L, bool draw, bool clickable) {
    std::vector<int> idx;
    for (size_t i = 0; i < actions_.size(); ++i)
        if (actions_[i].kind == 1 && !actions_[i].label.empty()) idx.push_back(static_cast<int>(i));
    if (idx.size() < 2) return 0;
    const float rw = L.text.right - L.text.left;
    const float margin = std::max(16.f, rw * 0.06f);
    const float size = std::round(L.hint * 1.02f * 4) / 4;
    const float h = std::round(size * 2.55f), top = std::round(L.text.top + std::max(12.f, margin * 0.6f));
    const float used = top - L.text.top + h + std::round(L.hint * 0.9f);
    if (!draw) return used;
    const double now = clockMs();
    const float s = dpi_ / 96.f;
    const float w = std::floor(std::min(rw - margin, size * 26)), x0 = std::round(L.text.left + (rw - w) / 2);
    const float segW = w / idx.size();
    const D2D1_RECT_F all{x0, top, x0 + w, top + h};
    d2dTarget_->FillRoundedRectangle({all, h / 2, h / 2}, brush(pal_.card, 0.55f));
    d2dTarget_->DrawRoundedRectangle({inflate(all, -0.5f), h / 2 - 0.5f, h / 2 - 0.5f}, brush(pal_.accent, 0.45f), 1.f);
    if (clickable && actionRects_.size() < actions_.size()) actionRects_.resize(actions_.size());
    for (size_t k = 0; k < idx.size(); ++k) {
        const Action& a = actions_[idx[k]];
        const D2D1_RECT_F r{std::round(x0 + segW * k) + 3, top + 3, std::round(x0 + segW * (k + 1)) - 3, top + h - 3};
        const float hl = uiLevel(uiHot_, UiAction, idx[k], now), fl = uiLevel(uiFocus_, UiAction, idx[k], now);
        const float rr = (r.bottom - r.top) / 2;
        if (a.on) d2dTarget_->FillRoundedRectangle({r, rr, rr}, brush(pal_.accent));
        else if (hl > 0.003f) d2dTarget_->FillRoundedRectangle({r, rr, rr}, brush(pal_.accent, 0.16f * hl));
        auto l = layout(keepAll(a.label), size, std::max(r.right - r.left - 12, 20.f), a.on ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
        if (l) {
            const float tw = std::min(textW(l.Get()), r.right - r.left - 12), th = textH(l.Get());
            l->SetMaxWidth(std::ceil(tw) + 2.f);
            d2dTarget_->DrawTextLayout({std::round((r.left + r.right) / 2 - tw / 2) - 1.f, std::round((r.top + r.bottom - th) / 2)}, l.Get(),
                                       brush(a.on ? pal_.ink : mixc(pal_.fg, pal_.accent, hl * 0.5f)));
        }
        if (fl > 0.003f) d2dTarget_->DrawRoundedRectangle({inflate(r, 2), rr + 2, rr + 2}, brush(pal_.fg, 0.9f * fl), 2.f);
        if (clickable)
            actionRects_[idx[k]] = {static_cast<LONG>(std::floor(r.left * s)), static_cast<LONG>(std::floor(r.top * s)),
                                    static_cast<LONG>(std::ceil(r.right * s)), static_cast<LONG>(std::ceil(r.bottom * s))};
    }
    return used;
}

// 0.8.2: the big on / off switch (Action kind 2) in place of the title: a track
// with its knob, the label (bold) and its status line (Action::sub) beside it.
// Returns its height; draws it centred at y when draw.
float Renderer::drawSwitch(float y, const Layout& L, float maxW, bool draw, bool clickable) {
    int i = -1;
    for (size_t k = 0; k < actions_.size(); ++k)
        if (actions_[k].kind == 2 && !actions_[k].label.empty()) {
            i = static_cast<int>(k);
            break;
        }
    if (i < 0) return 0;
    const Action& a = actions_[i];
    const float size = L.hint;
    const float trackW = std::round(size * 3.1f), trackH = std::round(size * 1.75f), gap = std::round(size * 0.8f);
    const float textMaxW = std::max(maxW - trackW - gap - size, 60.f);
    auto t = layout(keepAll(a.label), std::round(size * 1.3f * 4) / 4, textMaxW, DWRITE_FONT_WEIGHT_BOLD, true);
    auto sub = a.sub.empty() ? nullptr : layout(keepAll(a.sub), std::round(size * 0.92f * 4) / 4, textMaxW, DWRITE_FONT_WEIGHT_NORMAL, true);
    for (auto* l : {t.Get(), sub.Get()})
        if (l) l->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    const float tW = std::max(t ? textW(t.Get()) : 0.f, sub ? textW(sub.Get()) : 0.f);
    const float tH = (t ? textH(t.Get()) : 0.f) + (sub ? size * 0.15f + textH(sub.Get()) : 0.f);
    const float padY = std::round(size * 0.45f), padX = std::round(size * 0.7f);
    const float h = std::round(std::max(trackH, tH) + 2 * padY);
    if (!draw) return h;
    const double now = clockMs();
    const float s = dpi_ / 96.f;
    const float w = std::round(trackW + gap + tW + 2 * padX);
    const float x0 = std::round(L.text.left + ((L.text.right - L.text.left) - w) / 2);
    const D2D1_RECT_F row{x0, y, x0 + w, y + h};
    const float hl = uiLevel(uiHot_, UiAction, i, now), fl = uiLevel(uiFocus_, UiAction, i, now);
    const float pl = reduced_ ? 0.f : uiLevel(uiPress_, UiAction, i, now);
    D2D1_MATRIX_3X2_F base;
    d2dTarget_->GetTransform(&base);
    if (pl > 0.003f) d2dTarget_->SetTransform(scaledAbout(base, 1 - 0.03f * pl, {(row.left + row.right) / 2, (row.top + row.bottom) / 2}));
    d2dTarget_->FillRoundedRectangle({row, size * 0.9f, size * 0.9f}, brush(pal_.card, 0.45f + 0.2f * hl));
    const float tx = x0 + padX, ty = std::round(y + (h - trackH) / 2);
    const D2D1_RECT_F track{tx, ty, tx + trackW, ty + trackH};
    if (a.on) d2dTarget_->FillRoundedRectangle({track, trackH / 2, trackH / 2}, brush(pal_.accent));
    else {
        d2dTarget_->FillRoundedRectangle({track, trackH / 2, trackH / 2}, brush(pal_.fg, 0.10f + 0.06f * hl));
        d2dTarget_->DrawRoundedRectangle({inflate(track, -0.75f), trackH / 2 - 0.75f, trackH / 2 - 0.75f}, brush(pal_.dim, 0.8f), 1.5f);
    }
    const float kr = trackH / 2 - std::round(size * 0.22f);
    const float kx = a.on ? track.right - trackH / 2 : track.left + trackH / 2;
    d2dTarget_->FillEllipse({{kx, (track.top + track.bottom) / 2}, kr, kr}, brush(a.on ? pal_.ink : pal_.dim, a.on ? 1.f : 0.9f));
    float ly = std::round(y + (h - tH) / 2);
    const float lx = tx + trackW + gap;
    if (t) {
        d2dTarget_->DrawTextLayout({lx, ly}, t.Get(), brush(pal_.fg));
        ly += textH(t.Get()) + size * 0.15f;
    }
    if (sub) d2dTarget_->DrawTextLayout({lx, ly}, sub.Get(), brush(a.on ? pal_.accent : pal_.dim));
    d2dTarget_->SetTransform(base);
    if (fl > 0.003f) d2dTarget_->DrawRoundedRectangle({inflate(row, 3), size * 0.9f + 3, size * 0.9f + 3}, brush(pal_.fg, 0.9f * fl), 2.f);
    if (clickable) {
        if (actionRects_.size() < actions_.size()) actionRects_.resize(actions_.size());
        actionRects_[i] = {static_cast<LONG>(std::floor(row.left * s)), static_cast<LONG>(std::floor(row.top * s)),
                           static_cast<LONG>(std::ceil(row.right * s)), static_cast<LONG>(std::ceil(row.bottom * s))};
    }
    return h;
}

// 0.7.8 idle cards (setCards): rounded tiles in the theme's card colour with
// a hairline accent border; title, one line of how-to, a muted note; the
// button (same pill / link as the action row) right of the text when there
// is room, else under it.  Three side by side in a wide region (equal
// heights, buttons at the bottom), else stacked.  Muted: faded, grey border.
// 0.8.2: a card may have several actions (buttons, links, check boxes: Action
// kind 3) in rows under its text, and a QR code at its right (Card::qr); such
// cards are always stacked.
float Renderer::drawCards(float x0, float top, float maxW, float size, bool draw, bool clickable, int mode) {
    const int nc = static_cast<int>(std::min<size_t>(cards_.size(), 4));
    if (nc == 0 || maxW <= 40) return 0;
    const double now = clockMs();
    const float s = dpi_ / 96.f;
    auto q = [](float v) { return std::round(v * 4) / 4; };
    const float pad = std::round(size * 0.8f), gap = std::round(size * 0.55f);
    const float titleSize = q(size * 1.0f), bodySize = q(size * 0.84f), noteSize = q(size * 0.76f),
                btnSize = q(size * 0.84f);
    const float btnH = std::round(btnSize * 2.3f);
    std::vector<std::vector<int>> items(nc);
    for (size_t i = 0; i < actions_.size(); ++i) {
        const int c = actions_[i].card;
        if (c >= 0 && c < nc && !actions_[i].label.empty() && (actions_[i].kind == 0 || actions_[i].kind == 3))
            items[c].push_back(static_cast<int>(i));
    }
    bool rich = false;  // a QR code or several actions somewhere: stacked
    for (int i = 0; i < nc; ++i) rich = rich || cards_[i].qrSize > 0 || items[i].size() > 1 || (items[i].size() == 1 && actions_[items[i][0]].kind == 3);
    const int cols = rich ? 1
                     : nc <= 3 ? (maxW >= size * 36 ? nc : 1)  // three side by side
                     : maxW >= size * 48 ? 4 : maxW >= size * 24 ? 2 : 1;
    const bool row = cols > 1;  // cards in a grid row share its height
    const float cardW =
        std::floor(row ? std::min((maxW - (cols - 1) * gap) / cols, size * 17) : std::min(maxW, size * 30));
    const float inner = cardW - 2 * pad;
    if (inner < 40) return 0;
    struct Item {
        int index = -1;
        ComPtr<IDWriteTextLayout> l;
        float w = 0, padX = 0;
        int row = 0;
    };
    struct Lay {
        ComPtr<IDWriteTextLayout> t, b, n, k;
        float bw = 0, h = 0, tw = 0, inset = 0;  // inset: a link's text lines up with the card text
        bool side = false;
        int one = -1;            // the single button (the 0.7.8 card)
        std::vector<Item> its;   // several (0.8.2)
        int rows = 0;
        float qr = 0;            // QR side (DIPs), 0: none
    };
    std::vector<Lay> cs(nc);
    auto lead = [](IDWriteTextLayout* l) {
        if (l) l->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    };
    // A wrapped line: as narrow as keeps its line count, so the last line
    // is not one lone character (「選「自在投 / 影」」).
    auto balance = [](IDWriteTextLayout* l, float w) {
        DWRITE_TEXT_METRICS m{};
        if (!l || FAILED(l->GetMetrics(&m)) || m.lineCount < 2) return;
        float lo = w * 0.5f, hi = w;
        for (int k = 0; k < 7; ++k) {
            const float mid = (lo + hi) / 2;
            l->SetMaxWidth(mid);
            DWRITE_TEXT_METRICS mm{};
            if (SUCCEEDED(l->GetMetrics(&mm)) && mm.lineCount <= m.lineCount) hi = mid;
            else lo = mid;
        }
        l->SetMaxWidth(std::ceil(hi));
    };
    const float box = std::round(btnSize * 1.15f), boxGap = std::round(btnSize * 0.5f);
    const float itemGap = std::round(size * 0.5f), itemRowGap = std::round(size * 0.35f);
    float total = 0;
    std::vector<float> gridH((nc + cols - 1) / cols, 0.f);  // each grid row's height
    for (int i = 0; i < nc; ++i) {
        Lay& c = cs[i];
        const Card& cd = cards_[i];
        c.qr = cd.qrSize > 0 ? std::round(std::min(size * 7.2f, inner * 0.42f)) : 0.f;
        const float colW = c.qr > 0 ? inner - c.qr - pad : inner;  // the text column
        const bool single = items[i].size() == 1 && actions_[items[i][0]].kind == 0 && c.qr == 0;
        if (single) {
            c.one = items[i][0];
            const Action& a = actions_[c.one];
            const float padX = std::round(btnH * (a.primary ? 0.62f : 0.42f));
            if ((c.k = layout(keepAll(a.label), btnSize, std::max(inner - 2 * padX, 20.f), DWRITE_FONT_WEIGHT_SEMI_BOLD, false))) {
                if (!a.primary) c.k->SetUnderline(TRUE, {0, ~0u});
                c.bw = std::round(std::min(textW(c.k.Get()), inner - 2 * padX) + 2 * padX);
                if (!a.primary) c.inset = padX;
            }
        } else {
            for (int idx : items[i]) {
                const Action& a = actions_[idx];
                Item it;
                it.index = idx;
                if (a.kind == 3) {  // check box + label
                    it.padX = std::round(btnH * 0.3f);
                    it.l = layout(keepAll(a.label), btnSize, std::max(colW - box - boxGap - 2 * it.padX, 20.f), DWRITE_FONT_WEIGHT_NORMAL, true);
                    lead(it.l.Get());
                    if (it.l) it.w = std::round(std::min(textW(it.l.Get()), colW - box - boxGap - 2 * it.padX) + box + boxGap + 2 * it.padX);
                } else {
                    it.padX = std::round(btnH * (a.primary ? 0.62f : 0.42f));
                    it.l = layout(keepAll(a.label), btnSize, std::max(colW - 2 * it.padX, 20.f), DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
                    if (it.l && !a.primary) it.l->SetUnderline(TRUE, {0, ~0u});
                    if (it.l) it.w = std::round(std::min(textW(it.l.Get()), colW - 2 * it.padX) + 2 * it.padX);
                }
                if (it.l) c.its.push_back(std::move(it));
            }
            // Greedy rows within the text column.
            float x = 0;
            c.rows = c.its.empty() ? 0 : 1;
            for (auto& it : c.its) {
                if (x > 0 && x + itemGap + it.w > colW + 0.5f) ++c.rows, x = 0;
                x += (x > 0 ? itemGap : 0) + it.w;
                it.row = c.rows - 1;
            }
        }
        // Button beside the text (stacked cards) while the text keeps ~9 characters a line.
        c.side = !row && c.k && inner - c.bw - pad >= size * 9;
        c.tw = c.side ? inner - c.bw - pad : colW;
        c.t = layout(keepAll(cd.title), titleSize, c.tw, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        lead(c.t.Get());
        if (!cd.body.empty() && mode == 0) {
            lead((c.b = layout(keepAll(cd.body), bodySize, c.tw)).Get());
            balance(c.b.Get(), c.tw);
        }
        if (!cd.note.empty() && (mode < 2 || c.qr > 0)) {  // (a QR card's note is its address: kept)
            lead((c.n = layout(keepAll(cd.note), noteSize, c.tw)).Get());
            balance(c.n.Get(), c.tw);
        }
        float th = textH(c.t.Get());
        if (c.b) th += size * 0.22f + textH(c.b.Get());
        if (c.n) th += size * 0.12f + textH(c.n.Get());
        if (c.rows) th += size * 0.6f + c.rows * btnH + (c.rows - 1) * itemRowGap;
        c.h = std::round(2 * pad + (c.side ? std::max(th, btnH) : std::max(th + (c.k ? size * 0.6f + btnH : 0.f), c.qr)));
        gridH[i / cols] = std::max(gridH[i / cols], c.h);
        total += c.h + (i ? gap : 0.f);
    }
    if (row) {
        total = 0;
        for (size_t k = 0; k < gridH.size(); ++k) total += gridH[k] + (k ? gap : 0.f);
    }
    if (!draw) return total;

    if (clickable && actionRects_.size() < actions_.size()) actionRects_.resize(actions_.size());
    D2D1_MATRIX_3X2_F base;
    d2dTarget_->GetTransform(&base);
    const float groupW = row ? cols * cardW + (cols - 1) * gap : cardW;
    const float cx0 = std::round(x0 + (maxW - groupW) / 2);
    float cx = cx0, cy = std::round(top);
    cardsRect_ = {cx, cy, cx + groupW, cy + total};  // (the beam and the hearts keep off it)
    const float rad = std::round(size * 0.75f);
    auto setRect = [&](int idx, const D2D1_RECT_F& br) {
        if (clickable)
            actionRects_[idx] = {static_cast<LONG>(std::floor(br.left * s)), static_cast<LONG>(std::floor(br.top * s)),
                                 static_cast<LONG>(std::ceil(br.right * s)), static_cast<LONG>(std::ceil(br.bottom * s))};
    };
    // A pill / link button, as drawActions: hover 120 / 180 ms, press 96 %, focus ring.
    auto button = [&](int idx, IDWriteTextLayout* l, const D2D1_RECT_F& br) {
        const bool primary = actions_[idx].primary;
        const float hl = uiLevel(uiHot_, UiAction, idx, now);
        const float pl = reduced_ ? 0.f : uiLevel(uiPress_, UiAction, idx, now);
        const float fl = uiLevel(uiFocus_, UiAction, idx, now);
        if (pl > 0.003f)
            d2dTarget_->SetTransform(scaledAbout(base, 1 - 0.04f * pl, {(br.left + br.right) / 2, (br.top + br.bottom) / 2}));
        const float w = br.right - br.left, tw = textW(l), th = textH(l);
        l->SetMaxWidth(std::ceil(std::min(tw, w)) + 2.f);
        const D2D1_POINT_2F at{std::round((br.left + br.right) / 2 - std::min(tw, w) / 2) - 1.f, std::round(br.top + (btnH - th) / 2)};
        const D2D1_ROUNDED_RECT pill{br, btnH / 2, btnH / 2};
        if (primary) {
            const D2D1_ROUNDED_RECT edge{inflate(br, -0.75f), btnH / 2 - 0.75f, btnH / 2 - 0.75f};
            d2dTarget_->FillRoundedRectangle(pill, brush(pal_.accent, 0.10f + 0.90f * hl));
            d2dTarget_->DrawRoundedRectangle(edge, brush(pal_.accent, 0.85f + 0.15f * hl), 1.5f);
            d2dTarget_->DrawTextLayout(at, l, brush(mixc(pal_.accent, pal_.ink, hl)));
        } else {
            if (hl > 0.003f) d2dTarget_->FillRoundedRectangle(pill, brush(pal_.accent, 0.14f * hl));
            d2dTarget_->DrawTextLayout(at, l, brush(mixc(alphaOf(pal_.accent, 0.92f), pal_.fg, hl)));
        }
        d2dTarget_->SetTransform(base);
        if (fl > 0.003f) d2dTarget_->DrawRoundedRectangle({inflate(br, 3), btnH / 2 + 3, btnH / 2 + 3}, brush(pal_.fg, 0.9f * fl), 2.f);
        setRect(idx, br);
    };
    for (int i = 0; i < nc; ++i) {
        const Lay& c = cs[i];
        const Card& cd = cards_[i];
        const float h = row ? gridH[i / cols] : c.h;
        const D2D1_RECT_F r{cx, cy, cx + cardW, cy + h};
        const float fade = cd.muted ? 0.55f : 1.f;
        d2dTarget_->FillRoundedRectangle({r, rad, rad}, brush(pal_.card, cd.muted ? 0.38f : 0.62f));
        d2dTarget_->DrawRoundedRectangle({inflate(r, -0.5f), rad - 0.5f, rad - 0.5f},
                                         brush(cd.muted ? pal_.fg : pal_.accent, cd.muted ? 0.12f : 0.40f), 1.f);
        const float tx = r.left + pad;
        float ty = r.top + pad;
        if (c.side) {  // text centred against the button
            float th = textH(c.t.Get());
            if (c.b) th += size * 0.22f + textH(c.b.Get());
            if (c.n) th += size * 0.12f + textH(c.n.Get());
            ty = std::round(r.top + (h - th) / 2);
        }
        if (c.t) {
            d2dTarget_->DrawTextLayout({tx, ty}, c.t.Get(), brush(pal_.fg, fade));
            ty += textH(c.t.Get());
        }
        if (c.b) {
            ty += size * 0.22f;
            d2dTarget_->DrawTextLayout({tx, ty}, c.b.Get(), brush(pal_.dim, fade));
            ty += textH(c.b.Get());
        }
        if (c.n) {
            ty += size * 0.12f;
            d2dTarget_->DrawTextLayout({tx, ty}, c.n.Get(), brush(cd.qrSize > 0 ? pal_.fg : pal_.dim, cd.qrSize > 0 ? 0.85f : 0.75f));
            ty += textH(c.n.Get());
        }
        if (c.qr > 0 && cd.qr.size() >= static_cast<size_t>(cd.qrSize) * cd.qrSize * 4) {
            // White tile with the code at a whole number of pixels per module.
            const float qx = r.right - pad - c.qr, qy = std::round(r.top + std::max(pad, (h - c.qr) / 2));
            d2dTarget_->FillRoundedRectangle({{qx, qy, qx + c.qr, qy + c.qr}, size * 0.5f, size * 0.5f}, brush({1, 1, 1, 1}));
            ComPtr<ID2D1Bitmap> bmp;
            const D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
            if (SUCCEEDED(d2dTarget_->CreateBitmap(D2D1::SizeU(cd.qrSize, cd.qrSize), cd.qr.data(), cd.qrSize * 4, bp, &bmp))) {
                const int availPx = static_cast<int>(std::floor((c.qr - size * 0.5f) * s));
                const int k = std::max(1, availPx / cd.qrSize);
                const float drawPx = static_cast<float>(k * cd.qrSize);
                const float lp = std::round((qx + c.qr / 2) * s - drawPx / 2), tp = std::round((qy + c.qr / 2) * s - drawPx / 2);
                d2dTarget_->DrawBitmap(bmp.Get(), {lp / s, tp / s, (lp + drawPx) / s, (tp + drawPx) / s}, 1.f,
                                       D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            }
        }
        if (c.rows) {  // 0.8.2: the card's actions, in rows under its text
            float iy = ty + size * 0.6f;
            int rowNow = 0;
            float ix = tx - (!c.its.empty() && actions_[c.its[0].index].kind == 0 && !actions_[c.its[0].index].primary ? c.its[0].padX : 0.f);
            const float ix0 = ix;
            bool first = true;
            for (const auto& it : c.its) {
                if (it.row != rowNow) iy += btnH + itemRowGap, rowNow = it.row, ix = ix0, first = true;
                if (!first) ix += itemGap;
                first = false;
                const D2D1_RECT_F br{ix, iy, ix + it.w, iy + btnH};
                ix += it.w;
                const Action& a = actions_[it.index];
                if (a.kind == 3) {
                    const float hl = uiLevel(uiHot_, UiAction, it.index, now), fl = uiLevel(uiFocus_, UiAction, it.index, now);
                    if (hl > 0.003f) d2dTarget_->FillRoundedRectangle({br, btnH / 2, btnH / 2}, brush(pal_.accent, 0.12f * hl));
                    drawCheck(br.left + it.padX, std::round(br.top + (btnH - box) / 2), box, a.on, hl);
                    const float th = textH(it.l.Get());
                    d2dTarget_->DrawTextLayout({br.left + it.padX + box + boxGap, std::round(br.top + (btnH - th) / 2)}, it.l.Get(),
                                               brush(mixc(pal_.fg, pal_.accent, hl * 0.4f), fade));
                    if (fl > 0.003f) d2dTarget_->DrawRoundedRectangle({inflate(br, 3), btnH / 2 + 3, btnH / 2 + 3}, brush(pal_.fg, 0.9f * fl), 2.f);
                    setRect(it.index, br);
                } else {
                    button(it.index, it.l.Get(), br);
                }
            }
        }
        if (c.k) {
            const D2D1_RECT_F br = c.side ? D2D1_RECT_F{r.right - pad - c.bw, std::round((r.top + r.bottom - btnH) / 2),
                                                         r.right - pad, std::round((r.top + r.bottom - btnH) / 2) + btnH}
                                          : D2D1_RECT_F{tx - c.inset, r.bottom - pad - btnH, tx - c.inset + c.bw, r.bottom - pad};
            button(c.one, c.k.Get(), br);
        }
        if (row && (i + 1) % cols != 0) cx += cardW + gap;
        else cx = cx0, cy += h + gap;
    }
    return total;
}

}  // namespace pm::video
