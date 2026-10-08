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
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <set>
#include <vector>

#include "pm/i18n.h"
#include "renderer.h"

namespace pm::video {

using pm::i18n::S;
using pm::i18n::tr;

namespace {

// Smallest translation drawn in place (DIPs; PM_OVERLAY_MIN overrides,
// tests).  9 as in 0.7.0: about the print of a label at 2x in a phone-sized
// window (smaller goes to the list, which is 14-17 DIPs).
const float kMinRead = [] {
    const char* e = std::getenv("PM_OVERLAY_MIN");
    return e && atof(e) >= 6 ? static_cast<float>(atof(e)) : 9.f;
}();
// 0.7.2: and at least kMinPx physical pixels.  9 DIPs is 9 px at 100 %
// scaling, where Chinese / Japanese strokes run together; such a block goes
// to the list (14-17 DIPs) instead.  150 % and up: 9 DIPs >= 13.5 px, unchanged.
constexpr float kMinPx = 12.f;
constexpr float kTinyLine = 6.f;   // original lines smaller than this (DIPs): always listed
constexpr float kGap = 4.f;        // clear space between two cards (DIPs)

D2D1_COLOR_F rgb(uint32_t c) {
    return D2D1::ColorF(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f);
}
float luma(const D2D1_COLOR_F& c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }
bool overlaps(const D2D1_RECT_F& a, const D2D1_RECT_F& b, float tol) {
    return std::min(a.right, b.right) - std::max(a.left, b.left) > tol &&
           std::min(a.bottom, b.bottom) - std::max(a.top, b.top) > tol;
}
bool inside(const D2D1_RECT_F& a, const D2D1_RECT_F& outer, float tol) {
    return a.left >= outer.left - tol && a.top >= outer.top - tol && a.right <= outer.right + tol && a.bottom <= outer.bottom + tol;
}
D2D1_RECT_F inflate(const D2D1_RECT_F& r, float dx, float dy) { return {r.left - dx, r.top - dy, r.right + dx, r.bottom + dy}; }
D2D1_RECT_F unite(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return {std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
}
RECT toPx(const D2D1_RECT_F& r, float s) {
    return {std::lround(r.left * s), std::lround(r.top * s), std::lround(r.right * s), std::lround(r.bottom * s)};
}
float padX(float lineH) { return std::clamp(lineH * 0.18f, 1.5f, 6.f); }
float padY(float lineH) { return std::clamp(lineH * 0.12f, 1.f, 4.f); }

// ---- Line breaking ----
bool isSpaceCh(wchar_t c) { return c == L' ' || c == 0x3000 || c == L'\t'; }
// CJK / full-width: a line may break before or after it.
bool isCjkCh(wchar_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x3000 && c <= 0x303F) || (c >= 0x1100 && c <= 0x11FF);
}
// 行頭禁則: closing brackets and punctuation, small kana, ー, iteration marks.
bool noLineStart(wchar_t c) {
    static const wchar_t k[] =
        L"、。，．・：；？！‼⁇⁈⁉゛゜ヽヾゝゞ々〻ー…‥）〕］｝〉》」』】〙〗〟’”｠»ぁぃぅぇぉっゃゅょゎゕゖァィゥェォッャュョヮヵヶ"
        L"ㇰㇱㇲㇳㇴㇵㇶㇷㇸㇹㇺㇻㇼㇽㇾㇿ〜～‐゠–%％‰℃°′″,.:;?!)]}";
    return c && std::wcschr(k, c);
}
// 行末禁則: opening brackets.
bool noLineEnd(wchar_t c) {
    static const wchar_t k[] = L"（〔［｛〈《「『【〘〖〝‘“｟«([{";
    return c && std::wcschr(k, c);
}

struct Breaks {
    std::vector<std::pair<size_t, size_t>> lines;  // [begin, end) per line, trailing spaces trimmed
    std::vector<int> para;                         // paragraph (between '\n') of each line
    bool overflow = false;                         // a word longer than the width
};

// Greedy breaking at width W.  w: advance of each character (0 for the rest
// of a cluster), brk[i]: a line may start at i.
Breaks breakAt(const std::wstring& t, const std::vector<float>& w, const std::vector<char>& brk, float W, bool force) {
    Breaks b;
    const size_t n = t.size();
    int para = 0;
    size_t start = 0;
    auto push = [&](size_t s, size_t e) {
        while (e > s && isSpaceCh(t[e - 1])) --e;
        b.lines.push_back({s, e});
        b.para.push_back(para);
    };
    while (start <= n) {
        while (start < n && isSpaceCh(t[start])) ++start;
        float x = 0;
        size_t lastBrk = SIZE_MAX, i = start, end = n;
        bool newline = false;
        for (; i < n; ++i) {
            if (t[i] == L'\n') {
                end = i;
                newline = true;
                break;
            }
            if (i > start && brk[i]) lastBrk = i;
            x += w[i];
            if (isSpaceCh(t[i]) || x <= W + 0.01f) continue;
            // Too long: back to the last break, else (force) the last cluster, else overflow.
            if (lastBrk != SIZE_MAX) {
                end = lastBrk;
                break;
            }
            if (force && i > start) {
                size_t k = i;
                while (k > start + 1 && w[k] == 0) --k;  // not inside a cluster
                end = k;
                break;
            }
            b.overflow = true;
        }
        if (i >= n && !newline) end = n;
        push(start, end);
        if (newline) {
            ++para;
            start = end + 1;
            if (start > n) break;
            if (start == n) break;
            continue;
        }
        if (end >= n) break;
        start = end;
    }
    return b;
}

float lineWidth(const std::vector<float>& w, std::pair<size_t, size_t> l) {
    float x = 0;
    for (size_t i = l.first; i < l.second; ++i) x += w[i];
    return x;
}

}  // namespace

void Renderer::setTextOverlay(std::vector<TextBox> boxes) {
    boxes_ = std::move(boxes);
    fit_.clear();
    ovValid_ = false;
    ovHot_ = ovSel_ = -1;
    ovReveal_ = false;
    ovScroll_ = 0;
    ovHits_ = {};
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
    c.text->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, size * 1.22f, size * 0.96f);
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
    int notFit = 0, partial = 0;
    for (size_t i = 0; i < n; ++i) {
        OvItem& it = ovItems_[i];
        if (ovMode_ == 2) continue;  // 清單顯示
        const float bw = it.r.right - it.r.left, bh = it.r.bottom - it.r.top;
        const auto& cands = fitsFor(static_cast<size_t>(it.box), bw, bh, it.lineH, pw);
        const float px = padX(it.lineH), py = padY(it.lineH);
        for (size_t k = 0; k < cands.size() && it.cand < 0; ++k) {
            const FitCand& c = cands[k];
            const float tw = c.wide ? c.w : std::max(c.w, bw);
            float tx = it.r.left;
            if (tx + tw > pic.right - px) tx = std::max(pic.left + px, pic.right - px - tw);
            // Centred on the original lines (a larger card grows both ways, into the gaps between rows).
            const float ty = it.r.top + (bh - c.h) / 2;
            const D2D1_RECT_F text{tx, ty, tx + tw, ty + c.h};
            const D2D1_RECT_F card = inflate(unite(it.r, text), px, c.h > bh ? 0.5f : py);
            if (!inside(card, pic, 1.f)) continue;
            // Not far bigger than the original (it would bury the picture).
            if ((card.right - card.left) * (card.bottom - card.top) > 3.5f * std::max(1.f, bw * bh) + 200) continue;
            bool clash = false;
            for (size_t j = 0; j < n && !clash; ++j) clash = j != i && overlaps(card, cores[j], 0.f);
            for (const auto& q : placed) clash = clash || overlaps(inflate(card, half, half), inflate(q, half, half), 0.f);
            if (clash || blocked(card)) continue;
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
    const int whole = static_cast<int>(n) - partial;
    bool listAll = ovMode_ == 2 || (ovMode_ == 0 && notFit * 10 > whole * 3);
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
                const D2D1_RECT_F card = inflate(unite(it.r, text), padX(it.lineH), c.h > bh ? 0.5f : padY(it.lineH));
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
            row.y = y;
            row.h = std::max(badge, row.fc.h) + fs * 0.7f;
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
                return c - (p.bottom - p.top) * 0.001f;  // a tie: the taller one
            };
            const auto& pick = cost(bottom.first) <= cost(top.first) ? bottom : top;
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
        if (ovMode_ == 0 && !listAll && (nl - partial) * 10 > whole * 3) {
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
        d2dTarget_->DrawTextLayout({A.left + pad + badge + fs * 0.55f, top + fs * 0.35f}, row.fc.text.Get(), brush(fg));
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
