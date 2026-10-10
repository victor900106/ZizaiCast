// Helpers of the translation text overlay (layout and drawing): minimum sizes, line
// breaking with 禁則, rectangle and colour helpers.
// 拆檔 0.7.9：自 text_overlay.cpp 的匿名 namespace 原樣搬出，改放 pm::video::overlay_detail
// （不放 detail：rgb / inflate 與 renderer_internal.h 同名不同定義，同一 namespace 會違反 ODR）。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <set>
#include <vector>

#include "pm/i18n.h"
#include "renderer.h"

namespace pm::video::overlay_detail {

// Smallest translation drawn in place (DIPs; PM_OVERLAY_MIN overrides,
// tests).  9 as in 0.7.0: about the print of a label at 2x in a phone-sized
// window (smaller goes to the list, which is 14-17 DIPs).
inline const float kMinRead = [] {
    const char* e = std::getenv("PM_OVERLAY_MIN");
    return e && atof(e) >= 6 ? static_cast<float>(atof(e)) : 9.f;
}();
// 0.7.2: and at least kMinPx physical pixels.  9 DIPs is 9 px at 100 %
// scaling, where Chinese / Japanese strokes run together; such a block goes
// to the list (14-17 DIPs) instead.  150 % and up: 9 DIPs >= 13.5 px, unchanged.
inline constexpr float kMinPx = 12.f;
inline constexpr float kTinyLine = 6.f;   // original lines smaller than this (DIPs): always listed
inline constexpr float kGap = 4.f;        // clear space between two cards (DIPs)

inline D2D1_COLOR_F rgb(uint32_t c) {
    return D2D1::ColorF(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f);
}
inline float luma(const D2D1_COLOR_F& c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }
inline bool overlaps(const D2D1_RECT_F& a, const D2D1_RECT_F& b, float tol) {
    return std::min(a.right, b.right) - std::max(a.left, b.left) > tol &&
           std::min(a.bottom, b.bottom) - std::max(a.top, b.top) > tol;
}
inline bool inside(const D2D1_RECT_F& a, const D2D1_RECT_F& outer, float tol) {
    return a.left >= outer.left - tol && a.top >= outer.top - tol && a.right <= outer.right + tol && a.bottom <= outer.bottom + tol;
}
inline D2D1_RECT_F inflate(const D2D1_RECT_F& r, float dx, float dy) { return {r.left - dx, r.top - dy, r.right + dx, r.bottom + dy}; }
inline D2D1_RECT_F unite(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return {std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
}
inline RECT toPx(const D2D1_RECT_F& r, float s) {
    return {std::lround(r.left * s), std::lround(r.top * s), std::lround(r.right * s), std::lround(r.bottom * s)};
}
inline float padX(float lineH) { return std::clamp(lineH * 0.18f, 1.5f, 6.f); }
inline float padY(float lineH) { return std::clamp(lineH * 0.12f, 1.f, 4.f); }

// ---- Line breaking ----
inline bool isSpaceCh(wchar_t c) { return c == L' ' || c == 0x3000 || c == L'\t'; }
// CJK / full-width: a line may break before or after it.
inline bool isCjkCh(wchar_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x3000 && c <= 0x303F) || (c >= 0x1100 && c <= 0x11FF);
}
// 行頭禁則: closing brackets and punctuation, small kana, ー, iteration marks.
inline bool noLineStart(wchar_t c) {
    static const wchar_t k[] =
        L"、。，．・：；？！‼⁇⁈⁉゛゜ヽヾゝゞ々〻ー…‥）〕］｝〉》」』】〙〗〟’”｠»ぁぃぅぇぉっゃゅょゎゕゖァィゥェォッャュョヮヵヶ"
        L"ㇰㇱㇲㇳㇴㇵㇶㇷㇸㇹㇺㇻㇼㇽㇾㇿ〜～‐゠–%％‰℃°′″,.:;?!)]}";
    return c && std::wcschr(k, c);
}
// 行末禁則: opening brackets.
inline bool noLineEnd(wchar_t c) {
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
inline Breaks breakAt(const std::wstring& t, const std::vector<float>& w, const std::vector<char>& brk, float W, bool force) {
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

inline float lineWidth(const std::vector<float>& w, std::pair<size_t, size_t> l) {
    float x = 0;
    for (size_t i = l.first; i < l.second; ++i) x += w[i];
    return x;
}

}  // namespace pm::video::overlay_detail
