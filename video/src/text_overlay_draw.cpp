// Text overlay of the on-screen translation: drawing (cards, markers, list panel) and
// the legacy checks.  Layout: text_overlay_layout.cpp.
// 拆檔 0.7.9：自 text_overlay.cpp 原樣搬出。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <set>
#include <vector>

#include "pm/i18n.h"
#include "renderer.h"
#include "text_overlay_util.h"

namespace pm::video {

using pm::i18n::S;
using pm::i18n::tr;
using namespace overlay_detail;  // 拆檔 0.7.9: was text_overlay.cpp's anonymous namespace

void Renderer::drawTextOverlay(const D2D1_RECT_F& pic, float radius) {
    if (boxes_.empty()) return;
    const float pw = pic.right - pic.left, ph = pic.bottom - pic.top;
    if (pw < 8 || ph < 8) return;
    layoutOverlay(pic, radius);
    d2dTarget_->PushAxisAlignedClip(pic, D2D1_ANTIALIAS_MODE_ALIASED);
    if (showOriginal_) {  // the picture's own text; the translated areas outlined
        for (const auto& it : ovItems_) {
            const float rad = std::clamp((it.r.bottom - it.r.top) * 0.18f, 2.f, 8.f);
            d2dTarget_->DrawRoundedRectangle({it.r, rad, rad}, brush(pal_.accent, 0.75f), 1.5f);
        }
        d2dTarget_->PopAxisAlignedClip();
        return;
    }
    // Style: the picture's own colours (Google Lens), or high-contrast cards
    // (深色方框, or the 加強對比 / 黃字黑底 filters: low vision).
    const int filter = view_.filter;
    const bool hc = ovDark_ || filter == 1 || filter == 4;
    const D2D1_COLOR_F yellow = D2D1::ColorF(1, 0.92f, 0.1f);
    auto adjust = [&](D2D1_COLOR_F c) {
        if (filter == 2) {
            const float l = luma(c);
            c = D2D1::ColorF(l, l, l);
        } else if (filter == 3) {
            c = D2D1::ColorF(1 - c.r, 1 - c.g, 1 - c.b);
        }
        return c;
    };
    for (const auto& it : ovItems_) {
        if (it.cand < 0) continue;
        const TextBox& b = boxes_[it.box];
        const float crad = std::clamp(it.lineH * 0.2f, 2.f, 6.f);
        D2D1_COLOR_F bg, fg;
        if (hc) {
            bg = filter == 4 ? D2D1::ColorF(0, 0, 0) : pal_.card;
            fg = filter == 4 ? yellow : pal_.fg;
            d2dTarget_->FillRoundedRectangle({it.card, crad, crad}, brush(bg, 0.97f));
            d2dTarget_->DrawRoundedRectangle({it.card, crad, crad}, brush(filter == 4 ? yellow : pal_.accent, 0.7f), 1.f);
        } else {
            bg = adjust(b.colors ? rgb(b.bg) : D2D1::ColorF(0.97f, 0.97f, 0.96f));
            fg = adjust(b.colors ? rgb(b.fg) : D2D1::ColorF(0.1f, 0.1f, 0.1f));
            if (std::fabs(luma(bg) - luma(fg)) < 0.45f) fg = luma(bg) > 0.5f ? D2D1::ColorF(0.08f, 0.08f, 0.08f) : D2D1::ColorF(1, 1, 1);
            // Soft edge: the painted area fades out over ~2 DIPs (inside the kGap between cards).
            d2dTarget_->FillRoundedRectangle({inflate(it.card, 1.6f, 1.6f), crad + 1.6f, crad + 1.6f}, brush(bg, 0.35f));
            d2dTarget_->FillRoundedRectangle({inflate(it.card, 0.8f, 0.8f), crad + 0.8f, crad + 0.8f}, brush(bg, 0.65f));
            d2dTarget_->FillRoundedRectangle({it.card, crad, crad}, brush(bg));
        }
        d2dTarget_->DrawTextLayout({it.text.left, it.text.top}, it.fc.text.Get(), brush(fg));
        // Translated online: a small 「線上」 pill on the card's top-right corner.
        if (b.online) {
            const float bfs = std::clamp(it.lineH * 0.36f, 8.f, 11.f);
            const D2D1_SIZE_F bs = onlineBadge(0, 0, bfs, true, filter == 4, false);
            onlineBadge(it.card.right + bs.height * 0.25f, it.card.top - bs.height * 0.55f, bfs, true, filter == 4);
        }
    }
    if (!ovListed_.empty()) drawOverlayList(pic);
    d2dTarget_->PopAxisAlignedClip();
    // Hit rectangles (client pixels) for the UI thread.
    const float s = dpi_ / 96.f;
    ovHits_.rows.clear();
    ovHits_.markers.clear();
    ovHits_.panel = toPx(ovPanel_, s);
    ovHits_.zoomBtn = toPx(ovZoomBtn_, s);
    for (size_t k = 0; k < ovListed_.size(); ++k) {
        const OvItem& it = ovItems_[ovListed_[k]];
        const float r = ovMarkerD_ / 2 + 2;
        ovHits_.markers.push_back({toPx({it.marker.x - r, it.marker.y - r, it.marker.x + r, it.marker.y + r}, s), static_cast<int>(k)});
    }
    for (const auto& row : ovRows_) {
        const float top = ovRowsArea_.top + row.y - ovScroll_;
        D2D1_RECT_F rr{ovRowsArea_.left, std::max(top, ovRowsArea_.top), ovRowsArea_.right, std::min(top + row.h, ovRowsArea_.bottom)};
        if (rr.bottom > rr.top + 2) ovHits_.rows.push_back({toPx(rr, s), ovItems_[row.item].number - 1});
    }
}

void Renderer::drawOverlayList(const D2D1_RECT_F& pic) {
    const float fs = ovFs_, d = ovMarkerD_, pad = fs * 0.7f;
    const bool yellowMode = view_.filter == 4;
    const D2D1_COLOR_F yellow = D2D1::ColorF(1, 0.92f, 0.1f);
    const D2D1_COLOR_F accent = yellowMode ? yellow : pal_.accent;
    const int focus = ovHot_ >= 0 ? ovHot_ : ovSel_;
    // Focused block: the rest of the picture dimmed, the block outlined.
    if (focus >= 0 && focus < static_cast<int>(ovListed_.size())) {
        const D2D1_RECT_F r = inflate(ovItems_[ovListed_[focus]].r, 3, 3);
        auto veil = brush(D2D1::ColorF(0, 0, 0), 0.42f);
        d2dTarget_->FillRectangle({pic.left, pic.top, pic.right, r.top}, veil);
        d2dTarget_->FillRectangle({pic.left, r.bottom, pic.right, pic.bottom}, veil);
        d2dTarget_->FillRectangle({pic.left, r.top, r.left, r.bottom}, veil);
        d2dTarget_->FillRectangle({r.right, r.top, pic.right, r.bottom}, veil);
    }
    // Listed blocks: a thin outline + the numbered marker.
    for (size_t k = 0; k < ovListed_.size(); ++k) {
        const OvItem& it = ovItems_[ovListed_[k]];
        const bool f = static_cast<int>(k) == focus;
        d2dTarget_->DrawRoundedRectangle({inflate(it.r, 1.5f, 1.5f), 3, 3}, brush(f ? yellow : accent, f ? 1.f : 0.55f), f ? 2.5f : 1.f);
    }
    for (size_t k = 0; k < ovListed_.size(); ++k) {
        const OvItem& it = ovItems_[ovListed_[k]];
        const bool f = static_cast<int>(k) == focus;
        const float rr = (f ? d * 0.62f : d * 0.5f);
        const D2D1_ELLIPSE e{it.marker, rr, rr};
        d2dTarget_->FillEllipse(e, brush(f ? yellow : accent, 0.96f));
        d2dTarget_->DrawEllipse(e, brush(D2D1::ColorF(1, 1, 1), 0.95f), 1.5f);
        if (auto l = layout(std::to_wstring(k + 1), (k + 1 >= 10 ? 0.52f : 0.62f) * rr * 2, rr * 2 + 8, DWRITE_FONT_WEIGHT_BOLD, false, true)) {
            const float tw = textW(l.Get()), th = textH(l.Get());
            l->SetMaxWidth(std::ceil(tw) + 2);
            d2dTarget_->DrawTextLayout({it.marker.x - tw / 2 - 1, it.marker.y - th / 2}, l.Get(),
                                       brush(f || yellowMode ? D2D1::ColorF(0, 0, 0) : D2D1::ColorF(1, 1, 1)));
        }
    }
    // The panel (may be outside the picture: drawn without its clip).
    d2dTarget_->PopAxisAlignedClip();
    const D2D1_RECT_F& P = ovPanel_;
    const D2D1_COLOR_F card = yellowMode ? D2D1::ColorF(0, 0, 0) : pal_.card;
    const D2D1_COLOR_F fg = yellowMode ? yellow : pal_.fg;
    d2dTarget_->FillRoundedRectangle({inflate(P, 3, 3), 15, 15}, brush(D2D1::ColorF(0, 0, 0), 0.18f));
    d2dTarget_->FillRoundedRectangle({P, 12, 12}, brush(card, 0.97f));
    d2dTarget_->DrawRoundedRectangle({P, 12, 12}, brush(accent, 0.6f), 1.5f);
    if (ovTitle_) {
        const float th = textH(ovTitle_.Get());
        const float titleH = std::max(th, ovZoomBtn_.bottom > ovZoomBtn_.top ? fs * 1.9f : 0.f);
        d2dTarget_->DrawTextLayout({P.left + pad, P.top + pad + (titleH - th) / 2}, ovTitle_.Get(), brush(fg));
        if (ovSub_) d2dTarget_->DrawTextLayout({P.left + pad, P.top + pad + titleH + fs * 0.25f}, ovSub_.Get(), brush(fg, 0.75f));
    }
    if (ovZoomBtn_.right > ovZoomBtn_.left && ovZoomText_) {
        const D2D1_RECT_F& z = ovZoomBtn_;
        const float h = z.bottom - z.top;
        d2dTarget_->FillRoundedRectangle({z, h / 2, h / 2}, brush(accent, 0.95f));
        if (auto ic = iconFormat(fs)) {
            const wchar_t g[2] = {0xE71E, 0};  // Zoom
            d2dTarget_->DrawText(g, 1, ic.Get(), {z.left + fs * 0.6f, z.top, z.left + fs * 1.8f, z.bottom},
                                 brush(yellowMode ? D2D1::ColorF(0, 0, 0) : D2D1::ColorF(1, 1, 1)));
        }
        const float th = textH(ovZoomText_.Get());
        d2dTarget_->DrawTextLayout({z.left + fs * 1.85f, z.top + (h - th) / 2}, ovZoomText_.Get(),
                                   brush(yellowMode ? D2D1::ColorF(0, 0, 0) : D2D1::ColorF(1, 1, 1)));
    }
    // Rows (scrolled, clipped).
    const D2D1_RECT_F& A = ovRowsArea_;
    const float viewH = A.bottom - A.top;
    float maxScroll = std::max(0.f, ovRowsH_ - viewH);
    if (ovReveal_ && ovSel_ >= 0) {
        for (const auto& row : ovRows_)
            if (ovItems_[row.item].number - 1 == ovSel_) {
                if (row.y < ovScroll_) ovScroll_ = row.y;
                else if (row.y + row.h > ovScroll_ + viewH) ovScroll_ = row.y + row.h - viewH;
            }
        ovReveal_ = false;
    }
    ovScroll_ = std::clamp(ovScroll_, 0.f, maxScroll);
    d2dTarget_->DrawLine({A.left + pad, A.top - 1}, {A.right - pad, A.top - 1}, brush(fg, 0.2f), 1.f);
    d2dTarget_->PushAxisAlignedClip(A, D2D1_ANTIALIAS_MODE_ALIASED);
    const float badge = fs * 1.4f;
    for (const auto& row : ovRows_) {
        const float top = A.top + row.y - ovScroll_;
        if (top > A.bottom || top + row.h < A.top) continue;
        const int k = ovItems_[row.item].number - 1;
        if (k == ovHot_ || k == ovSel_)
            d2dTarget_->FillRoundedRectangle({{A.left + 4, top + 1, A.right - 4, top + row.h - 1}, 8, 8},
                                             brush(accent, k == ovSel_ ? 0.30f : 0.18f));
        const D2D1_POINT_2F c{A.left + pad + badge / 2, top + fs * 0.35f + badge / 2};
        d2dTarget_->FillEllipse({c, badge / 2, badge / 2}, brush(accent, 0.95f));
        if (auto l = layout(std::to_wstring(k + 1), badge * (k + 1 >= 10 ? 0.5f : 0.6f), badge + 8, DWRITE_FONT_WEIGHT_BOLD, false, true)) {
            const float tw = textW(l.Get()), th = textH(l.Get());
            l->SetMaxWidth(std::ceil(tw) + 2);
            d2dTarget_->DrawTextLayout({c.x - tw / 2 - 1, c.y - th / 2}, l.Get(),
                                       brush(yellowMode ? D2D1::ColorF(0, 0, 0) : D2D1::ColorF(1, 1, 1)));
        }
        const D2D1_POINT_2F at{A.left + pad + badge + fs * 0.55f, top + fs * 0.35f};
        if (row.badgeY >= 0) onlineBadge(at.x, at.y + row.badgeY, fs * 0.72f, false, yellowMode);
        if (row.alt.text || row.facts.text) {
            // Doubtful translation: a warning bar; selected, the original instead.
            const D2D1_COLOR_F warn = yellowMode ? yellow : luma(card) > 0.5f ? D2D1::ColorF(0.78f, 0.36f, 0.f) : D2D1::ColorF(1.f, 0.66f, 0.2f);
            d2dTarget_->FillRoundedRectangle({{A.left + 1, top + fs * 0.3f, A.left + 4, top + row.h - fs * 0.3f}, 1.5f, 1.5f}, brush(warn));
            if (k == ovSel_ && row.alt.text) {
                d2dTarget_->DrawTextLayout(at, row.alt.text.Get(), brush(fg));
                continue;
            }
            d2dTarget_->DrawTextLayout(at, row.fc.text.Get(), brush(fg));
            if (row.facts.text) d2dTarget_->DrawTextLayout({at.x, at.y + row.factsY}, row.facts.text.Get(), brush(warn));
            continue;
        }
        d2dTarget_->DrawTextLayout(at, row.fc.text.Get(), brush(fg));
    }
    d2dTarget_->PopAxisAlignedClip();
    if (maxScroll > 0) {  // scroll bar
        const float bh = std::max(20.f, viewH * viewH / ovRowsH_);
        const float by = A.top + (viewH - bh) * (ovScroll_ / maxScroll);
        d2dTarget_->FillRoundedRectangle({{A.right - 7, by, A.right - 3, by + bh}, 2, 2}, brush(fg, 0.45f));
    }
    d2dTarget_->PushAxisAlignedClip(pic, D2D1_ANTIALIAS_MODE_ALIASED);  // popped by drawTextOverlay
}


// The same checks on the 0.7.0 overlay (PM_OVERLAY_070=1, tests: before /
// after numbers).  A port of 0.7.0's drawTextOverlay geometry: every block a
// dark card at the original's size (shrinking to 70 / 55 %, a card up to
// 1.6x wider, then 9 DIPs growing downwards), drawn in order (a later card
// hides an earlier one), clipped to the picture.
void Renderer::legacyChecks(const D2D1_RECT_F& pic) {
    struct Card {
        D2D1_RECT_F box{};
        D2D1_POINT_2F at{};
        ComPtr<IDWriteTextLayout> text;
        float size = 0;
    };
    std::vector<Card> cards;
    const float pw = pic.right - pic.left;
    for (const TextBox& b : boxes_) {
        if (b.text.empty()) continue;
        const D2D1_POINT_2F p0 = contentToDip(pic, b.x0, b.y0), p1 = contentToDip(pic, b.x1, b.y1);
        const D2D1_RECT_F r{std::min(p0.x, p1.x), std::min(p0.y, p1.y), std::max(p0.x, p1.x), std::max(p0.y, p1.y)};
        if (r.right < pic.left || r.left > pic.right || r.bottom < pic.top || r.top > pic.bottom) continue;
        const float bw = r.right - r.left, bh = r.bottom - r.top;
        if (bw < 3 || bh < 3) continue;
        const float lineH = bh / static_cast<float>(std::max(1, b.lines));
        const float pad = std::clamp(lineH * 0.25f, 2.f, 10.f);
        const float innerH = std::max(4.f, bh + pad * 0.6f);
        constexpr float kMin = 9.f;
        const float size0 = std::clamp(lineH * 0.92f, kMin, 80.f);
        Card c;
        float textW = 0;
        auto tryFit = [&](float size, float maxW) {
            c.text = layout(b.text, size, maxW, DWRITE_FONT_WEIGHT_SEMI_BOLD, true);
            if (!c.text) return false;
            c.text->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            c.text->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, size * 1.2f, size * 0.95f);
            DWRITE_TEXT_METRICS tm{};
            c.text->GetMetrics(&tm);
            c.size = size;
            textW = maxW;
            return tm.height <= innerH + 0.5f && tm.widthIncludingTrailingWhitespace <= maxW + 0.5f;
        };
        bool ok = false;
        const float narrow = std::max(4.f, bw), wide = std::max(narrow, std::min(bw * 1.6f, pw * 0.96f));
        const float floor70 = std::max(kMin, size0 * 0.7f), floor55 = std::max(kMin, size0 * 0.55f);
        for (float sz = size0; !ok && sz >= floor70 - 0.01f; sz *= 0.92f) ok = tryFit(sz, narrow);
        for (float sz = size0 * 0.85f; !ok && wide > narrow + 1 && sz >= floor55 - 0.01f; sz *= 0.92f) ok = tryFit(sz, wide);
        for (float sz = floor55; !ok && sz >= kMin - 0.01f; sz *= 0.9f) ok = tryFit(sz, wide);
        if (!ok) tryFit(kMin, narrow);
        if (!c.text) continue;
        DWRITE_TEXT_METRICS tm{};
        c.text->GetMetrics(&tm);
        float tx0 = r.left, tx1 = r.right;
        if (textW > bw + 0.5f) {
            const float tw = std::max(bw, tm.widthIncludingTrailingWhitespace);
            tx0 = std::clamp((r.left + r.right) / 2 - tw / 2, pic.left + pad, std::max(pic.left + pad, pic.right - pad - tw));
            tx1 = tx0 + tw;
        }
        c.box = {tx0 - pad, r.top - pad * 0.6f, tx1 + pad, r.bottom + pad * 0.6f};
        c.box.bottom = std::max(c.box.bottom, c.box.top + tm.height + pad * 1.2f);
        c.at = {tx0, c.box.top + std::max(pad * 0.6f, (c.box.bottom - c.box.top - tm.height) / 2)};
        cards.push_back(std::move(c));
    }
    int overlapsN = 0, cut = 0, shortLast = 0;
    for (size_t i = 0; i < cards.size(); ++i) {
        const Card& a = cards[i];
        for (size_t j = i + 1; j < cards.size(); ++j)
            if (overlaps(a.box, cards[j].box, 0.f)) ++overlapsN;
        // Characters: outside the card or the picture, or under a later card.
        UINT32 nc = 0, pos = 0;
        a.text->GetClusterMetrics(nullptr, 0, &nc);
        std::vector<DWRITE_CLUSTER_METRICS> cm(nc);
        if (!nc || FAILED(a.text->GetClusterMetrics(cm.data(), nc, &nc))) continue;
        std::vector<float> lineW;
        std::vector<int> lineChars;
        float lastTop = -1e9f;
        for (const auto& m : cm) {
            FLOAT x = 0, y = 0;
            DWRITE_HIT_TEST_METRICS hm{};
            a.text->HitTestTextPosition(pos, FALSE, &x, &y, &hm);
            if (hm.top > lastTop + 0.5f) {
                lineW.push_back(0);
                lineChars.push_back(0);
                lastTop = hm.top;
            }
            if (!m.isWhitespace && !m.isNewline) {
                lineW.back() = hm.left + hm.width;
                ++lineChars.back();
                const float iy = std::max(0.f, (hm.height - a.size) / 2);
                const D2D1_RECT_F g{a.at.x + hm.left + 0.5f, a.at.y + hm.top + iy, a.at.x + hm.left + hm.width - 0.5f,
                                    a.at.y + hm.top + hm.height - iy};
                bool hidden = !inside(g, a.box, 0.f) || !inside(g, pic, 0.f);
                for (size_t j = i + 1; j < cards.size() && !hidden; ++j) hidden = overlaps(g, cards[j].box, 0.f);
                cut += hidden;
            }
            pos += m.length;
        }
        if (lineW.size() >= 2) {
            const float longest = *std::max_element(lineW.begin(), lineW.end());
            if (lineChars.back() <= 2 || lineW.back() < longest / 3) ++shortLast;
        }
    }
    ovHits_.old070Cards = static_cast<int>(cards.size());
    ovHits_.old070Overlaps = overlapsN;
    ovHits_.old070Cut = cut;
    ovHits_.old070ShortLast = shortLast;
}

}  // namespace pm::video
