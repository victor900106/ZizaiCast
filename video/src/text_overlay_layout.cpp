// Text overlay of the on-screen translation (Renderer::drawTextOverlay).
//
// Layout pass (layoutOverlay, redone when the picture, zoom, pan or window
// change): every visible block tries to show its translation in place, the
// way Google Lens does — the block's area painted in the picture's own
// background colour, the translation in its text colour, left-aligned, the
// largest size that fits (lines of even length, 禁則, at least kMinRead
// DIPs; a card may grow wider or downwards).  Blocks of the same print size
// get the same text size.  A card never covers another block, another card,
// a marker or the list panel (hard rule, checked in overlayChecks).  Blocks
// that cannot be shown that way get a numbered marker and their translation
// goes to a list panel (in the free part of the window or of the picture,
// scrollable); when more than 30 % of the blocks do not fit (a label far
// away, camera at 1x) every block is listed (清單顯示), so the picture stays
// visible with numbers.  The 翻譯 menu can force 原位顯示 / 清單顯示 and
// the dark cards of 0.7.0 (low vision).  Hovering / clicking a row
// highlights its block, clicking a marker shows its row, 「放大這一塊」
// magnifies the listed blocks (the UI thread does that with overlayHits()).
// 拆檔 0.7.9：自 text_overlay.cpp 原樣搬出（排版部分；繪製在 text_overlay_draw.cpp）。
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

void Renderer::setTextOverlay(std::vector<TextBox> boxes) {
    // The same blocks again with better translations (progressive display:
    // the LLM's pass, or more chunks of a dense screen): the list keeps its
    // scroll position.
    bool sameBlocks = boxes.size() >= boxes_.size() && !boxes_.empty();
    for (size_t i = 0; sameBlocks && i < boxes_.size(); ++i)
        sameBlocks = boxes[i].original == boxes_[i].original && boxes[i].x0 == boxes_[i].x0 && boxes[i].y0 == boxes_[i].y0;

    const float scroll = ovScroll_;
    boxes_ = std::move(boxes);
    // Marks of pm_translate (translate/src/text_util.h kOverlayRow /
    // kOverlayUncertain) at the start of a translation: a table row, a
    // translation that still failed a check (its last line 「⚠ …」: the
    // checked key facts).  Both are always listed (owner decisions: tables in
    // the numbered list as 「標籤　值」; doubtful text with its key facts
    // highlighted and the original a tap away).
    for (TextBox& b : boxes_) {
        // 0xE002 alone (kOverlayKeep): text kept as written - no card, the list
        // panel is placed off it.
        if (b.text.size() == 1 && b.text[0] == 0xE002) {
            b.kind = 4;
            b.text.clear();
            continue;
        }
        while (!b.text.empty() && (b.text[0] == 0xE000 || b.text[0] == 0xE001)) {
            b.kind |= b.text[0] == 0xE000 ? 1 : 2;
            b.text.erase(0, 1);
        }
        if (b.kind & 2) {
            const size_t f = b.text.rfind(L"\n\x26A0");
            if (f != std::wstring::npos) {
                b.facts = b.text.substr(f + 1);
                b.text.erase(f);
            }
        }
    }
    fit_.clear();
    ovValid_ = false;
    ovHot_ = ovSel_ = -1;
    ovReveal_ = false;
    ovScroll_ = 0;
    if (sameBlocks) ovScroll_ = scroll;  // (the selection is reset with the UI side: VideoWindow::setTextOverlay)
    ovHits_ = {};
}

void Renderer::setTextOverlayOnline(const std::vector<std::wstring>& originals) {
    bool changed = false;
    for (TextBox& b : boxes_) {
        const bool on = std::find(originals.begin(), originals.end(), b.original) != originals.end();
        changed |= on != b.online;
        b.online = on;
    }
    if (changed) ovValid_ = false;  // list rows grow by the badge
}

D2D1_SIZE_F Renderer::onlineBadge(float x, float y, float fs, bool right, bool yellowMode, bool draw) {
    auto l = layout(tr(S::TrOnlineBadge), fs, 400, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    if (!l) return {0, 0};
    const float tw = textW(l.Get()), th = textH(l.Get());
    const float w = std::ceil(tw) + fs * 1.0f, h = th + fs * 0.2f;
    if (!draw) return {w, h};
    const float left = right ? x - w : x;
    l->SetMaxWidth(std::ceil(tw) + 2);
    d2dTarget_->FillRoundedRectangle({{left, y, left + w, y + h}, h / 2, h / 2},
                                     brush(yellowMode ? D2D1::ColorF(1, 0.92f, 0.1f) : pal_.accent, 0.94f));
    d2dTarget_->DrawTextLayout({left + fs * 0.5f, y + fs * 0.1f}, l.Get(),
                               brush(yellowMode ? D2D1::ColorF(0, 0, 0) : D2D1::ColorF(1, 1, 1)));
    return {w, h};
}

bool Renderer::setText(const std::wstring& text, float size, float maxW, DWRITE_FONT_WEIGHT weight, bool force, FitCand& c) {
    c = {};
    c.size = size;
    c.maxW = maxW;
    if (text.empty()) return false;
    // Advances: the text on one line, cluster by cluster.
    auto one = layout(text, size, 100000.f, weight, true);
    if (!one) return false;
    one->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    UINT32 nc = 0;
    one->GetClusterMetrics(nullptr, 0, &nc);
    std::vector<DWRITE_CLUSTER_METRICS> cm(nc);
    if (!nc || FAILED(one->GetClusterMetrics(cm.data(), nc, &nc))) return false;
    const size_t n = text.size();
    std::vector<float> w(n, 0.f);
    std::vector<char> brk(n, 0), clusterStart(n, 0);
    size_t pos = 0;
    for (const auto& m : cm) {
        if (pos < n) {
            w[pos] = m.isNewline ? 0.f : m.width;
            clusterStart[pos] = 1;
        }
        pos += m.length;
    }
    // Break opportunities (a line may start at i).
    for (size_t i = 1; i < n; ++i) {
        if (!clusterStart[i]) continue;
        const wchar_t a = text[i - 1], b = text[i];
        bool ok;
        if (isSpaceCh(b) || b == L'\n' || a == L'\n') ok = false;
        else if (isSpaceCh(a)) ok = true;
        else if (isCjkCh(a) || isCjkCh(b)) ok = true;
        else ok = a == L'/' || a == L'-';  // inside a Latin word / number: only after / or -
        if (noLineStart(b) || noLineEnd(a)) ok = false;
        brk[i] = ok;
    }
    // A Latin word / number after an opening bracket or before punctuation
    // is one unit with it (handled above); 禁則 can make a long unit, fine.
    Breaks br = breakAt(text, w, brk, maxW, force);
    if (br.overflow && !force) return false;
    // Even lines: the narrowest width with the same number of lines.
    const size_t nl = br.lines.size();
    if (nl >= 2) {
        float lo = maxW * 0.3f, hi = maxW;
        for (int it = 0; it < 14 && hi - lo > 0.5f; ++it) {
            const float mid = (lo + hi) / 2;
            Breaks t = breakAt(text, w, brk, mid, false);
            if (!t.overflow && t.lines.size() == nl) hi = mid;
            else lo = mid;
        }
        Breaks t = breakAt(text, w, brk, hi, force);
        if (t.lines.size() == nl && (!t.overflow || force)) br = std::move(t);
    }
    // The text with explicit line breaks.
    std::wstring out;
    for (size_t k = 0; k < br.lines.size(); ++k) {
        if (k) out += L'\n';
        out.append(text, br.lines[k].first, br.lines[k].second - br.lines[k].first);
        if (br.lines[k].second > br.lines[k].first && noLineStart(text[br.lines[k].first]) && k > 0 && br.para[k] == br.para[k - 1])
            ++c.kinsoku;
    }
    // Stubs: a paragraph's last line of <= 2 characters or under a third of its longest line.
    for (size_t k = 0; k < br.lines.size(); ++k) {
        const bool last = k + 1 == br.lines.size() || br.para[k + 1] != br.para[k];
        if (!last || k == 0 || br.para[k - 1] != br.para[k]) continue;
        float longest = 0;
        for (size_t j = 0; j < br.lines.size(); ++j)
            if (br.para[j] == br.para[k]) longest = std::max(longest, lineWidth(w, br.lines[j]));
        int chars = 0;
        for (size_t i = br.lines[k].first; i < br.lines[k].second; ++i) chars += clusterStart[i] && !isSpaceCh(text[i]);
        if (chars <= 2 || lineWidth(w, br.lines[k]) < longest / 3) ++c.shortLast;
    }
    c.text = layout(out, size, maxW + 64, weight, true);
    if (!c.text) return false;
    c.text->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    c.text->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    // One line: no leading needed (it kept stacked one-line cards of an app's
    // list from fitting between their neighbours).
    if (br.lines.size() == 1) c.text->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, size * 1.12f, size * 0.91f);
    else c.text->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, size * 1.22f, size * 0.96f);
    DWRITE_TEXT_METRICS tm{};
    c.text->GetMetrics(&tm);
    c.w = tm.widthIncludingTrailingWhitespace;
    c.h = tm.height;
    c.lines = static_cast<int>(tm.lineCount);
    c.text->SetMaxWidth(std::ceil(c.w) + 1);
    return force || c.w <= maxW + 0.75f;
}

// Ways to set box i's translation at its on-screen size: the largest size
// that fits the box (then a wider card), and as a last resort kMinRead text
// growing downwards.
const std::vector<Renderer::FitCand>& Renderer::fitsFor(size_t box, float bw, float bh, float lineH, float pw) {
    if (fit_.size() != boxes_.size()) fit_.assign(boxes_.size(), {});
    Fit& f = fit_[box];
    const float minRead = std::max(kMinRead, kMinPx * 96.f / static_cast<float>(dpi_));
    if (!f.c.empty() && std::fabs(f.w - bw) < 0.5f && std::fabs(f.h - bh) < 0.5f && f.min == minRead) return f.c;
    f = {};
    f.w = bw;
    f.h = bh;
    f.min = minRead;
    const std::wstring& text = boxes_[box].text;
    if (lineH < kTinyLine) return f.c;  // a label far away: blown up 2x it would cover everything
    const float s0 = std::clamp(lineH * 0.88f, minRead, 64.f), lo = std::max(minRead, s0 * 0.62f);
    const float slack = std::max(2.f, lineH * 0.3f);
    auto weight = [](float size) { return size < 15 ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_MEDIUM; };
    const float narrow = std::max(4.f, bw), wide = std::min(std::max(bw * 1.6f, bw + s0 * 4), pw * 0.94f);
    for (int pass = 0; pass < 2; ++pass) {
        const float w = pass ? wide : narrow;
        if (pass && wide < narrow + 2) break;
        for (float sz = s0; sz >= lo - 0.01f; sz *= 0.93f) {
            FitCand c;
            if (setText(text, sz, w, weight(sz), false, c) && c.h <= bh + slack && !c.shortLast) {
                c.wide = pass == 1;
                f.c.push_back(std::move(c));
                break;
            }
        }
    }
    for (int pass = 0; pass < 2; ++pass) {  // grows downwards
        const float w = pass ? wide : narrow;
        if (pass && wide < narrow + 2) break;
        FitCand c;
        if (setText(text, lo, w, weight(lo), false, c) && !c.shortLast) {
            c.wide = pass == 1;
            f.c.push_back(std::move(c));
        }
    }
    return f.c;
}

void Renderer::layoutOverlay(const D2D1_RECT_F& pic, float radius) {
    const float s = dpi_ / 96.f;
    const bool frozenBadge = frozenOn_ && frozen_.valid();
    const float key[14] = {pic.left, pic.top, pic.right, pic.bottom, view_.zoom, view_.cx, view_.cy, mirror_ ? 1.f : 0.f,
                           static_cast<float>(width_), static_cast<float>(height_), s, showOriginal_ ? 1.f : 0.f,
                           frozenBadge ? 1.f : 0.f, radius};
    if (ovValid_ && std::equal(std::begin(key), std::end(key), std::begin(ovKey_))) return;
    std::copy(std::begin(key), std::end(key), ovKey_);
    ovValid_ = true;
    ovItems_.clear();
    ovRows_.clear();
    ovListed_.clear();
    ovPanel_ = ovRowsArea_ = ovZoomBtn_ = {};
    ovTitle_ = ovSub_ = ovZoomText_ = nullptr;
    ovHits_ = {};
    // The badges drawn over the picture (畫面已凍結, the zoom level) and the
    // overview: nothing of the overlay goes under them.
    ovBlocked_.clear();
    {
        const Badges b = badgeRects(pic, radius);
        for (const D2D1_RECT_F& r : {b.zoom, b.frozen})
            if (r.right > r.left) ovBlocked_.push_back(inflate(r, 3, 3));
        if (view_.zoom > 1.001f && miniRect_.right > miniRect_.left)
            ovBlocked_.push_back(inflate({miniRect_.left / s, miniRect_.top / s, miniRect_.right / s, miniRect_.bottom / s}, 4, 4));
    }
    auto blocked = [&](const D2D1_RECT_F& r) {
        for (const auto& b : ovBlocked_)
            if (overlaps(r, b, 0.f)) return true;
        return false;
    };
    const float pw = pic.right - pic.left, ph = pic.bottom - pic.top;
    for (size_t i = 0; i < boxes_.size(); ++i) {
        const TextBox& b = boxes_[i];
        if (b.text.empty()) continue;
        const D2D1_POINT_2F p0 = contentToDip(pic, b.x0, b.y0), p1 = contentToDip(pic, b.x1, b.y1);
        const D2D1_RECT_F r{std::min(p0.x, p1.x), std::min(p0.y, p1.y), std::max(p0.x, p1.x), std::max(p0.y, p1.y)};
        if (r.right < pic.left + 2 || r.left > pic.right - 2 || r.bottom < pic.top + 2 || r.top > pic.bottom - 2) continue;
        if (r.right - r.left < 3 || r.bottom - r.top < 3) continue;
        OvItem it;
        it.box = static_cast<int>(i);
        it.r = r;
        it.lineH = (r.bottom - r.top) / static_cast<float>(std::max(1, b.lines));
        ovItems_.push_back(it);
    }
    // Reading order: rows top to bottom (a row = within half a line), then left to right.
    std::stable_sort(ovItems_.begin(), ovItems_.end(), [](const OvItem& a, const OvItem& b) {
        const float tol = 0.5f * std::min(a.lineH, b.lineH);
        if (std::fabs(a.r.top - b.r.top) > tol) return a.r.top < b.r.top;
        return a.r.left < b.r.left;
    });
    const size_t n = ovItems_.size();
    if (!n || showOriginal_) return;
    // Obstacles: every block's own text (a card must not hide another block).
    std::vector<D2D1_RECT_F> cores(n);
    for (size_t i = 0; i < n; ++i) {
        const auto& it = ovItems_[i];
        cores[i] = inflate(it.r, -std::min(1.5f, (it.r.right - it.r.left) * 0.1f), -std::min(it.lineH * 0.22f, 3.f));
    }
    // Cards keep kGap DIPs clear of each other (a hard rule: a block that
    // cannot get that at kMinRead goes to the list, it is never squeezed in).
    const float half = kGap / 2;
    std::vector<D2D1_RECT_F> placed;
    int notFit = 0, partial = 0, forced = 0;
    for (size_t i = 0; i < n; ++i) {
        OvItem& it = ovItems_[i];
        if (ovMode_ == 2) continue;  // 清單顯示
        if (boxes_[it.box].kind) {   // table rows, doubtful text: always listed (not "not fitting")
            ++forced;
            continue;
        }
        const float bw = it.r.right - it.r.left, bh = it.r.bottom - it.r.top;
        const auto& cands = fitsFor(static_cast<size_t>(it.box), bw, bh, it.lineH, pw);
        const float px = padX(it.lineH), py = padY(it.lineH);
        // Lines set close together (a list in an app: 24 px pitch, 4.5 px
        // between glyph bands): the vertical padding shrinks so that two
        // stacked cards keep kGap - else every second line went to the list.
        float gapAbove = 1e9f, gapBelow = 1e9f;
        for (size_t j = 0; j < n; ++j) {
            if (j == i) continue;
            const auto& o = ovItems_[j].r;
            if (std::min(o.right, it.r.right) - std::max(o.left, it.r.left) <= 0) continue;
            if (o.bottom <= it.r.top + 1) gapAbove = std::min(gapAbove, it.r.top - o.bottom);
            if (o.top >= it.r.bottom - 1) gapBelow = std::min(gapBelow, o.top - it.r.bottom);
        }
        const float pyTop = std::clamp((gapAbove - kGap) / 2, 0.f, py), pyBottom = std::clamp((gapBelow - kGap) / 2, 0.f, py);
        for (size_t k = 0; k < cands.size() && it.cand < 0; ++k) {
            const FitCand& c = cands[k];
            const float tw = c.wide ? c.w : std::max(c.w, bw);
            float tx = it.r.left;
            if (tx + tw > pic.right - px) tx = std::max(pic.left + px, pic.right - px - tw);
            // Centred on the original lines (a larger card grows both ways, into the gaps between rows).
            const float ty = it.r.top + (bh - c.h) / 2;
            const D2D1_RECT_F text{tx, ty, tx + tw, ty + c.h};
            D2D1_RECT_F card = inflate(unite(it.r, text), px, 0);
            if (c.h > bh) card.top -= 0.5f, card.bottom += 0.5f;
            else card.top -= pyTop, card.bottom += pyBottom;
            if (!inside(card, pic, 1.f)) continue;
            // Not far bigger than the original (it would bury the picture).
            if ((card.right - card.left) * (card.bottom - card.top) > 3.5f * std::max(1.f, bw * bh) + 200) continue;
            bool clash = false;
            int clashWith = -1;
            for (size_t j = 0; j < n && !clash; ++j)
                if (j != i && overlaps(card, cores[j], 0.f)) clash = true, clashWith = static_cast<int>(j);
            bool clashCard = false;
            for (const auto& q : placed) clashCard = clashCard || overlaps(inflate(card, half, half), inflate(q, half, half), 0.f);
            static const bool dbgWhy = std::getenv("PM_OVERLAY_DEBUG") != nullptr;
            if (dbgWhy && (clash || clashCard || blocked(card)))
                std::fprintf(stderr, "  [overlay]   box %d way %zu (%.0fx%.0f, size %.1f%s): %s %d\n", it.box, k, c.w, c.h, c.size, c.wide ? " wide" : "",
                             clash ? "covers block" : clashCard ? "covers a card" : "blocked", clashWith >= 0 ? ovItems_[clashWith].box : -1);
            if (clash || clashCard || blocked(card)) continue;
            it.cand = static_cast<int>(k);
            it.fc = c;
            it.card = card;
            it.text = text;
        }
        // A block cut by the edge of the view (magnified) is not one that
        // does not fit: it is listed but does not count for the 30 % rule.
        if (it.cand >= 0) placed.push_back(it.card);
        else if (inside(it.r, pic, 1.f)) ++notFit;
        else ++partial;
        static const bool debug = std::getenv("PM_OVERLAY_DEBUG") != nullptr;  // tests: why a block is listed
        if (debug)
            std::fprintf(stderr, "  [overlay] %s line %.1f box %.0fx%.0f, %zu ways%s: box %d\n", it.cand >= 0 ? "in place" : "LISTED",
                         it.lineH, bw, bh, cands.size(), it.cand >= 0 ? (it.fc.wide ? " (wide)" : "") : "",
                         it.box);
    }
    ovHits_.notFitting = notFit;
    // Automatic: more than 30 % do not fit in place -> every block listed
    // (half an overlay is confusing; the picture stays visible with numbers).
    const int whole = static_cast<int>(n) - partial - forced;
    // (Few blocks - a track list with six headings translated: whatever fits
    // stays in place; listing all of them made a panel over the kept text.)
    bool listAll = ovMode_ == 2 || (ovMode_ == 0 && notFit * 10 > whole * 3 && whole > 8);
    if (listAll)
        for (auto& it : ovItems_) it.cand = -1;
    // Same print size, same text size: blocks whose lines are within ~20 %
    // of each other take the smallest size any of them needed (a card only
    // shrinks, so nothing new can overlap).
    {
        std::vector<size_t> on;
        for (size_t i = 0; i < n; ++i)
            if (ovItems_[i].cand >= 0) on.push_back(i);
        std::sort(on.begin(), on.end(), [&](size_t a, size_t b) { return ovItems_[a].lineH < ovItems_[b].lineH; });
        for (size_t g0 = 0; g0 < on.size();) {
            size_t g1 = g0 + 1;
            while (g1 < on.size() && ovItems_[on[g1]].lineH <= ovItems_[on[g0]].lineH * 1.22f) ++g1;
            float size = 1e9f;
            for (size_t k = g0; k < g1; ++k) size = std::min(size, ovItems_[on[k]].fc.size);
            for (size_t k = g0; k < g1; ++k) {
                OvItem& it = ovItems_[on[k]];
                if (it.fc.size <= size + 0.01f) continue;
                FitCand c;
                const auto wt = size < 15 ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_MEDIUM;
                if (!setText(boxes_[it.box].text, size, it.fc.maxW, wt, false, c) || c.h > it.fc.h + 0.5f || c.shortLast) continue;
                c.wide = it.fc.wide;
                const float bw = it.r.right - it.r.left, bh = it.r.bottom - it.r.top;
                const float tw = c.wide ? c.w : std::max(c.w, bw);
                const float ty = it.r.top + (bh - c.h) / 2;
                const D2D1_RECT_F text{it.text.left, ty, it.text.left + tw, ty + c.h};
                D2D1_RECT_F card = inflate(unite(it.r, text), padX(it.lineH), c.h > bh ? 0.5f : 0.f);
                if (c.h <= bh) card.top = std::min(card.top, it.card.top), card.bottom = std::max(card.bottom, it.card.bottom);  // the padding it had
                if (!inside(card, inflate(it.card, 0.01f, 0.01f), 0.f)) continue;
                it.fc = std::move(c);
                it.text = text;
                it.card = card;
            }
            g0 = g1;
        }
    }
    float cc[4] = {1, 1, 0, 0};
    bool canZoom = false;
    // The list panel; in-place cards it would cover join the list (then
    // maybe everything does) and the panel is laid out again.
    for (int round = 0; round < 4; ++round) {
        ovRows_.clear();
        ovListed_.clear();
        ovPanel_ = ovRowsArea_ = ovZoomBtn_ = {};
        ovTitle_ = ovSub_ = ovZoomText_ = nullptr;
        for (size_t i = 0; i < n; ++i) {
            OvItem& it = ovItems_[i];
            it.number = 0;
            if (it.cand < 0) {
                ovListed_.push_back(static_cast<int>(i));
                it.number = static_cast<int>(ovListed_.size());
            }
        }
        if (ovListed_.empty()) break;
        const float fs = std::clamp(std::min(pw, 560.f) / 26.f, 14.f, 17.f);
        ovFs_ = fs;
        ovMarkerD_ = std::clamp(fs * 1.2f, 17.f, 21.f);
        const float winW = width_ / s, pad = fs * 0.7f;
        std::fill(cc, cc + 4, 0.f);
        cc[0] = cc[1] = 1;
        for (int k : ovListed_) {
            const TextBox& b = boxes_[ovItems_[k].box];
            cc[0] = std::min(cc[0], b.x0), cc[1] = std::min(cc[1], b.y0), cc[2] = std::max(cc[2], b.x1), cc[3] = std::max(cc[3], b.y1);
        }
        // Free room beside the picture (wide window), else inside it, at its
        // top or bottom (whichever covers fewer in-place cards).
        D2D1_RECT_F panel{};
        // Beside the picture: right / left (a wide window), below / above
        // (the bars of a landscape picture in a tall window).
        bool side = false, above = false;
        const float winH = height_ / s;
        const float pl = std::max(8.f, pic.left + 8), pr = std::min(winW - 8, pic.right - 8);
        if (winW - pic.right >= 250) panel = {pic.right + 8, pic.top + 8, std::min(winW - 8, pic.right + 8 + 440), pic.bottom - 8}, side = true;
        else if (pic.left >= 250) panel = {std::max(8.f, pic.left - 8 - 440), pic.top + 8, pic.left - 8, pic.bottom - 8}, side = true;
        else if (winH - pic.bottom >= 150) panel = {pl, pic.bottom + 8, pr, winH - 8}, side = true;
        else if (pic.top >= 150) panel = {pl, 8, pr, pic.top - 8}, side = above = true;
        else panel = {pic.left + 8, 0, pic.right - 8, 0};
        const float panelW = panel.right - panel.left;
        // Header: title, (all listed) a hint, the zoom button.
        const float zoomNeed =
            std::clamp(std::min(0.9f / std::max(0.02f, cc[2] - cc[0]), 0.9f / std::max(0.02f, cc[3] - cc[1])), 1.f, kMaxZoom);
        canZoom = zoomNeed >= view_.zoom * 1.3f;
        float headH = pad;
        float zoomW = 0;
        if (canZoom) {
            ovZoomText_ = layout(tr(S::TrZoomBlock), fs * 0.92f, 400, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
            if (ovZoomText_) {
                const float tw = textW(ovZoomText_.Get());
                ovZoomText_->SetMaxWidth(std::ceil(tw) + 2);
                zoomW = tw + fs * 2.6f;
            }
        }
        ovTitle_ = layout(pm::i18n::fmt(S::TrListTitle, {std::to_wstring(ovListed_.size())}), fs,
                          std::max(40.f, panelW - 2 * pad - zoomW - 6), DWRITE_FONT_WEIGHT_BOLD, false);
        if (ovTitle_) ovTitle_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        const float titleH = std::max(ovTitle_ ? textH(ovTitle_.Get()) : fs, canZoom ? fs * 1.9f : 0.f);
        headH += titleH;
        if (listAll && ovMode_ == 0) {
            ovSub_ = layout(tr(S::TrListSmall), fs * 0.8f, std::max(40.f, panelW - 2 * pad), DWRITE_FONT_WEIGHT_NORMAL, true);
            if (ovSub_) {
                ovSub_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                headH += fs * 0.25f + textH(ovSub_.Get());
            }
        }
        headH += pad * 0.6f;
        // Rows (same size, lines of even length, 禁則).
        const float badge = fs * 1.4f, textX = pad + badge + fs * 0.55f, textWMax = std::max(40.f, panelW - textX - pad - 6);
        float y = 0;
        for (size_t k = 0; k < ovListed_.size(); ++k) {
            OvRow row;
            row.item = ovListed_[k];
            if (!setText(boxes_[ovItems_[row.item].box].text, fs, textWMax, DWRITE_FONT_WEIGHT_NORMAL, true, row.fc)) continue;
            row.fc.text->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, fs * 1.35f, fs * 1.05f);
            row.fc.h = textH(row.fc.text.Get());
            float textHgt = row.fc.h;
            const TextBox& tb = boxes_[ovItems_[row.item].box];
            if (tb.kind & 2) {
                // The checked key facts below the translation; the original
                // (shown instead while the row is selected) takes the same room.
                if (!tb.facts.empty() && setText(tb.facts, fs * 0.92f, textWMax, DWRITE_FONT_WEIGHT_SEMI_BOLD, true, row.facts)) {
                    row.facts.text->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, fs * 1.3f, fs * 1.0f);
                    row.facts.h = textH(row.facts.text.Get());
                    row.factsY = row.fc.h + fs * 0.15f;
                    textHgt = row.factsY + row.facts.h;
                }
                if (!tb.original.empty() && setText(L"\x21C4 " + tb.original, fs, textWMax, DWRITE_FONT_WEIGHT_NORMAL, true, row.alt)) {
                    row.alt.text->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, fs * 1.35f, fs * 1.05f);
                    row.alt.h = textH(row.alt.text.Get());
                    textHgt = std::max(textHgt, row.alt.h);
                }
            }
            if (tb.online) {  // the 「線上」 badge under the text
                row.badgeY = textHgt + fs * 0.25f;
                textHgt = row.badgeY + onlineBadge(0, 0, fs * 0.72f, false, false, false).height;
            }
            row.y = y;
            row.h = std::max(badge, textHgt) + fs * 0.7f;
            y += row.h;
            ovRows_.push_back(std::move(row));
        }
        ovRowsH_ = y;
        // The rows shown at first: whole rows only (no row cut at the bottom).
        auto wholeRows = [&](float avail) {
            float hRows = 0;
            for (const auto& r : ovRows_) {
                if (hRows + r.h > avail + 0.5f) break;
                hRows += r.h;
            }
            if (hRows == 0 && !ovRows_.empty()) hRows = std::min(avail, ovRows_[0].h);
            return hRows;
        };
        if (!side) {
            // Inside the picture: at its top or bottom, in the band free of
            // text if that is large enough (camera / app bars), else as
            // small as useful (title + about two rows) on the side that
            // covers fewer blocks; never more than half the picture.
            // Clear of the badges (畫面已凍結 at the top, the zoom level and
            // the overview at the bottom).
            float topY = pic.top + 8, bottomY = pic.bottom - 8;
            for (const auto& b : ovBlocked_) {
                if (b.bottom < pic.top + ph * 0.4f) topY = std::max(topY, b.bottom + 2);
                if (b.top > pic.bottom - ph * 0.4f) bottomY = std::min(bottomY, b.top - 2);
            }
            float textTop = pic.bottom, textBottom = pic.top;
            for (const auto& it : ovItems_) {
                const D2D1_RECT_F& r = it.cand >= 0 ? it.card : it.r;
                textTop = std::min(textTop, r.top);
                textBottom = std::max(textBottom, r.bottom);
            }
            const float bandTop = textTop - 8 - topY, bandBottom = bottomY - (textBottom + 8);
            const float firstRows = ovRows_.empty() ? 0 : ovRows_[0].h + (ovRows_.size() > 1 ? ovRows_[1].h : 0);
            const float minH = std::min(headH + firstRows + pad * 0.5f, ph * 0.3f);
            const float maxH = ph * 0.5f;
            auto panelAt = [&](bool atTop, float band) {
                const float want = std::clamp(band, minH, maxH);
                const float hRows = wholeRows(std::max(0.f, want - headH - pad * 0.5f));
                const float h = headH + hRows + pad * 0.5f;
                return std::make_pair(atTop ? D2D1_RECT_F{panel.left, topY, panel.right, topY + h}
                                            : D2D1_RECT_F{panel.left, bottomY - h, panel.right, bottomY},
                                      hRows);
            };
            const auto top = panelAt(true, bandTop), bottom = panelAt(false, bandBottom);
            auto cost = [&](const D2D1_RECT_F& p) {  // in-place cards covered, then listed blocks covered
                float c = 0;
                for (const auto& it : ovItems_) {
                    if (it.cand >= 0 && overlaps(inflate(it.card, half, half), p, 0.f)) c += 1000;
                    else if (it.cand < 0 && overlaps(it.r, p, 0.f)) c += 1;
                }
                // Text kept as written (a track list's songs 27-40): readable only uncovered.
                for (const TextBox& k : boxes_) {
                    if (k.kind != 4) continue;
                    const D2D1_POINT_2F q0 = contentToDip(pic, k.x0, k.y0), q1 = contentToDip(pic, k.x1, k.y1);
                    if (overlaps(D2D1_RECT_F{std::min(q0.x, q1.x), std::min(q0.y, q1.y), std::max(q0.x, q1.x), std::max(q0.y, q1.y)}, p, 0.f)) c += 1500;  // hidden for good (a covered card only moves to the list: 1000)
                }
                return c - (p.bottom - p.top) * 0.001f;  // a tie: the taller one
            };
            std::pair<D2D1_RECT_F, float> pick = cost(bottom.first) <= cost(top.first) ? bottom : top;
            // Covering text kept as written (a track list) or cards: also try the
            // panel at other heights - over the listed blocks themselves (their
            // originals are in the panel anyway), clear of the badges.
            if (cost(pick.first) >= 1000) {
                const float h = pick.first.bottom - pick.first.top;
                float best = cost(pick.first);
                for (float y = topY; y + h <= bottomY; y += std::max(8.f, h / 8)) {
                    const D2D1_RECT_F c{panel.left, y, panel.right, y + h};
                    bool onBadge = false;
                    for (const auto& b : ovBlocked_) onBadge = onBadge || overlaps(b, c, 0.f);
                    const float k = cost(c);
                    if (!onBadge && k < best - 0.5f) best = k, pick.first = c;
                }
            }
            panel = pick.first;
            ovRowsArea_ = {panel.left, panel.top + headH, panel.right, panel.top + headH + pick.second};
        } else {
            const float hRows = wholeRows(std::max(0.f, panel.bottom - panel.top - headH - pad * 0.5f));
            if (above) panel.top = panel.bottom - (headH + hRows + pad * 0.5f);  // next to the picture
            else panel.bottom = panel.top + headH + hRows + pad * 0.5f;
            ovRowsArea_ = {panel.left, panel.top + headH, panel.right, panel.top + headH + hRows};
        }
        ovPanel_ = panel;
        if (canZoom && ovZoomText_) {
            const float bh = fs * 1.9f;
            ovZoomBtn_ = {panel.right - pad - zoomW, panel.top + pad + (titleH - bh) / 2, panel.right - pad,
                          panel.top + pad + (titleH + bh) / 2};
        }
        // In-place cards under the panel: listed too, and again.
        bool moved = false;
        for (auto& it : ovItems_)
            if (it.cand >= 0 && overlaps(inflate(it.card, half, half), panel, 0.f)) it.cand = -1, moved = true;
        if (!moved) break;
        int nl = 0;
        for (const auto& it : ovItems_) nl += it.cand < 0;
        if (ovMode_ == 0 && !listAll && (nl - partial - forced) * 10 > whole * 3 && whole > 8) {
            listAll = true;
            for (auto& it : ovItems_) it.cand = -1;
        }
    }
    ovHits_.listAll = listAll;
    ovHits_.inPlace = static_cast<int>(n - ovListed_.size());
    ovHits_.listed = static_cast<int>(ovListed_.size());
    // Markers: at the start of each listed block's first line, else along
    // it, beside it, above or below it; clear of cards, the panel and other
    // markers.
    const float d = ovMarkerD_;
    std::vector<D2D1_POINT_2F> marks;
    auto markRect = [&](D2D1_POINT_2F p) { return D2D1_RECT_F{p.x - d / 2, p.y - d / 2, p.x + d / 2, p.y + d / 2}; };
    for (int k : ovListed_) {
        OvItem& it = ovItems_[k];
        const float cy = it.r.top + std::min(it.r.bottom - it.r.top, it.lineH) / 2;
        std::vector<D2D1_POINT_2F> cand{{it.r.left - d * 0.45f, cy}, {it.r.left - d * 1.0f, cy}};
        for (float x = it.r.left + d * 0.55f; x < it.r.right; x += d * 0.55f) cand.push_back({x, cy});
        cand.push_back({it.r.right + d * 0.55f, cy});
        for (float x = it.r.left + d * 0.5f; x < it.r.right; x += d * 0.55f) {
            cand.push_back({x, it.r.top - d * 0.5f});
            cand.push_back({x, it.r.bottom + d * 0.5f});
        }
        for (float dy = d; dy < ph; dy += d * 0.6f) {  // further away, above / below
            cand.push_back({it.r.left + d * 0.5f, it.r.top - dy});
            cand.push_back({it.r.left + d * 0.5f, it.r.bottom + dy});
        }
        D2D1_POINT_2F best = cand[0];
        bool found = false;
        for (const auto& p : cand) {
            if (p.x < pic.left + d / 2 || p.x > pic.right - d / 2 || p.y < pic.top + d / 2 || p.y > pic.bottom - d / 2) continue;
            const D2D1_RECT_F mr = markRect(p);
            bool free = !overlaps(inflate(mr, 1, 1), ovPanel_, 0.f) && !blocked(mr);
            for (const auto& m : marks) free = free && std::hypot(m.x - p.x, m.y - p.y) >= d + 1;
            for (const auto& q : ovItems_) free = free && !(q.cand >= 0 && overlaps(inflate(mr, 1, 1), q.card, 0.f));
            if (free) {
                best = p;
                found = true;
                break;
            }
        }
        if (!found) ++ovHits_.markerClashes;
        best.x = std::clamp(best.x, pic.left + d / 2, pic.right - d / 2);
        best.y = std::clamp(best.y, pic.top + d / 2, pic.bottom - d / 2);
        it.marker = best;
        marks.push_back(best);
    }
    overlayChecks(s);
    static const bool old070 = std::getenv("PM_OVERLAY_070") != nullptr;
    if (old070) legacyChecks(pic);
    if (ovListed_.empty()) return;
    std::copy(cc, cc + 4, ovHits_.zoomTo);
    if (!canZoom) std::fill(std::begin(ovHits_.zoomTo), std::end(ovHits_.zoomTo), 0.f);
}

// Checks of the layout (tests, logs): overlapping pairs of drawn things,
// cards closer than kGap, characters cut or hidden, 禁則, stub last lines,
// the smallest text and the number of text sizes.
void Renderer::overlayChecks(float s) {
    const size_t n = ovItems_.size();
    const float half = kGap / 2, d = ovMarkerD_;
    auto markRect = [&](const OvItem& it) {
        return D2D1_RECT_F{it.marker.x - d / 2, it.marker.y - d / 2, it.marker.x + d / 2, it.marker.y + d / 2};
    };
    const D2D1_RECT_F pic{ovKey_[0], ovKey_[1], ovKey_[2], ovKey_[3]};
    const bool panel = !ovListed_.empty();
    std::set<int> sizes;
    float minFont = 1e9f;
    for (size_t i = 0; i < n; ++i) {
        const OvItem& a = ovItems_[i];
        if (a.cand < 0) continue;
        const FitCand& c = a.fc;
        sizes.insert(static_cast<int>(std::lround(c.size * 4)));
        minFont = std::min(minFont, c.size);
        ovHits_.kinsoku += c.kinsoku;
        ovHits_.shortLast += c.shortLast;
        if (panel && overlaps(a.card, ovPanel_, 0.f)) ++ovHits_.overlaps;
        for (size_t j = i + 1; j < n; ++j) {
            const OvItem& b = ovItems_[j];
            if (b.cand < 0) continue;
            if (overlaps(a.card, b.card, 0.f)) ++ovHits_.overlaps;
            else if (overlaps(inflate(a.card, half - 0.05f, half - 0.05f), inflate(b.card, half - 0.05f, half - 0.05f), 0.f))
                ++ovHits_.tooClose;
        }
        for (int k : ovListed_)
            if (overlaps(markRect(ovItems_[k]), a.card, 0.f)) ++ovHits_.overlaps;
        // Every character: inside its card and the picture, under nothing else.
        UINT32 len = 0, nc = 0;
        c.text->GetClusterMetrics(nullptr, 0, &nc);
        std::vector<DWRITE_CLUSTER_METRICS> cm(nc);
        if (nc && SUCCEEDED(c.text->GetClusterMetrics(cm.data(), nc, &nc))) {
            for (const auto& m : cm) {
                if (!m.isWhitespace && !m.isNewline) {
                    FLOAT x = 0, y = 0;
                    DWRITE_HIT_TEST_METRICS hm{};
                    c.text->HitTestTextPosition(len, FALSE, &x, &y, &hm);
                    // The character's em box (the hit-test box is the whole line pitch).
                    const float iy = std::max(0.f, (hm.height - c.size) / 2);
                    const D2D1_RECT_F g{a.text.left + hm.left + 0.5f, a.text.top + hm.top + iy, a.text.left + hm.left + hm.width - 0.5f,
                                        a.text.top + hm.top + hm.height - iy};
                    int why = !inside(g, a.card, 0.f) ? 1 : !inside(g, pic, 0.f) ? 2 : 0;
                    for (size_t j = 0; j < n && !why; ++j)
                        if (j != i && ovItems_[j].cand >= 0 && overlaps(g, ovItems_[j].card, 0.f)) why = 3;
                    for (int k : ovListed_)
                        if (!why && overlaps(g, markRect(ovItems_[k]), 0.f)) why = 4;
                    if (!why && panel && overlaps(g, ovPanel_, 0.f)) why = 5;
                    const bool cut = why != 0;
                    ovHits_.truncated += cut;
                    static const bool debug = std::getenv("PM_OVERLAY_DEBUG") != nullptr;
                    if (debug && cut)
                        std::fprintf(stderr, "  [cut %d] box %d glyph %.1f,%.1f-%.1f,%.1f card %.1f,%.1f-%.1f,%.1f pic %.1f,%.1f-%.1f,%.1f\n", why, a.box,
                                     g.left, g.top, g.right, g.bottom, a.card.left, a.card.top, a.card.right, a.card.bottom, pic.left,
                                     pic.top, pic.right, pic.bottom);
                }
                len += m.length;
            }
        }
    }
    // Markers: apart from each other and from the panel.
    for (size_t a = 0; a < ovListed_.size(); ++a) {
        const D2D1_RECT_F ma = markRect(ovItems_[ovListed_[a]]);
        if (overlaps(ma, ovPanel_, 0.f)) ++ovHits_.overlaps;
        for (size_t b = a + 1; b < ovListed_.size(); ++b) {  // round markers
            const auto p = ovItems_[ovListed_[a]].marker, q = ovItems_[ovListed_[b]].marker;
            if (std::hypot(p.x - q.x, p.y - q.y) < d) ++ovHits_.overlaps;
        }
    }
    // List rows: within the panel's width; whole rows shown at first.
    for (const auto& row : ovRows_) {
        minFont = std::min(minFont, ovFs_);
        if (row.fc.w > row.fc.maxW + 0.75f) ovHits_.truncated += 1;  // a character runs out of the panel
        for (const FitCand* x : {&row.facts, &row.alt})
            if (x->text && x->w > x->maxW + 0.75f) ovHits_.truncated += 1;
        ovHits_.kinsoku += row.fc.kinsoku;
        ovHits_.shortLast += row.fc.shortLast;
        const float top = ovRowsArea_.top + row.y;
        if (top < ovRowsArea_.bottom - 0.5f && top + row.h > ovRowsArea_.bottom + 0.5f) {
            // A row cut by the bottom of the list (not at rest: wholeRows).
            ovHits_.truncated += 1;
        }
    }
    // Nothing under the badges / the overview.
    for (const auto& b : ovBlocked_) {
        for (const auto& it : ovItems_)
            if (it.cand >= 0 && overlaps(it.card, b, 0.f)) ++ovHits_.overlaps;
        for (int k : ovListed_)
            if (overlaps(markRect(ovItems_[k]), b, 0.f)) ++ovHits_.overlaps;
        if (panel && overlaps(ovPanel_, b, 0.f)) ++ovHits_.overlaps;
    }
    ovHits_.fontSizes = static_cast<int>(sizes.size());
    ovHits_.minFont = minFont > 1e8f ? 0 : minFont * s;  // pixels
}

}  // namespace pm::video
