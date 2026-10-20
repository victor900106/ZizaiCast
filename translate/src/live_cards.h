// 即時翻譯: a live run's new cards against the ones already on screen (header-only, so that
// pm_translate_test can check it without models or pictures).
#pragma once

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <string>
#include <vector>

#include "pm/video_window.h"

namespace pm::translate {

struct LiveCardsResult {
    int kept = 0, added = 0, gone = 0;
    int shifted = 0;    // kept cards that moved with a scroll (their place is the new block's)
    float scrollDy = 0; // the scroll shift found (normalised picture height; + = content moved down)
    bool same = false;  // nothing differs (same cards, same places): the shown list stays as it is
};

namespace live_cards_detail {

inline std::wstring squeeze(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s)
        if (!std::iswspace(c)) o += c;
    return o;
}
inline std::wstring digitsOf(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s)
        if (c >= L'0' && c <= L'9') o += c;
    return o;
}
// The same text for a card: equal, or (long enough) differing by a few letters - OCR reads a scrolled
// picture a little differently every time.  The digits must be the same (12 min is not 13 min).
inline bool sameText(const std::wstring& a, const std::wstring& b) {
    if (a == b) return true;
    const std::wstring x = squeeze(a), y = squeeze(b);
    if (x == y) return true;
    const size_t n = std::max(x.size(), y.size());
    if (std::min(x.size(), y.size()) < 5 || n > 300) return false;
    const size_t k = std::max<size_t>(1, n / 8);
    if ((x.size() > y.size() ? x.size() - y.size() : y.size() - x.size()) > k) return false;
    if (digitsOf(x) != digitsOf(y)) return false;
    std::vector<size_t> p(y.size() + 1), c(y.size() + 1);
    for (size_t j = 0; j <= y.size(); ++j) p[j] = j;
    for (size_t i = 1; i <= x.size(); ++i) {
        c[0] = i;
        size_t rowMin = c[0];
        for (size_t j = 1; j <= y.size(); ++j) {
            c[j] = std::min({p[j] + 1, c[j - 1] + 1, p[j - 1] + (x[i - 1] == y[j - 1] ? 0u : 1u)});
            rowMin = std::min(rowMin, c[j]);
        }
        if (rowMin > k) return false;
        std::swap(p, c);
    }
    return p[y.size()] <= k;
}

}  // namespace live_cards_detail

// A block with the same recognised text at the same place - or at the place a scroll moved it to - becomes
// the old card (its translation, colours and layout; no flash).  The scroll shifts are found from the
// text itself: every pair of equal texts votes for (new y - old y); the shifts that many pairs agree on
// (the page, and 0 for what stays: a header, a fixed bar) are tried for every block.  `bx` is changed in
// place: a kept block becomes the old card, moved onto the block's place when the content scrolled.
inline LiveCardsResult stabilizeLiveCards(std::vector<VideoWindow::TextBox>& bx, const std::vector<VideoWindow::TextBox>& shown) {
    using namespace live_cards_detail;
    using Box = VideoWindow::TextBox;
    LiveCardsResult r;
    // 1) The scroll shifts.
    struct Vote {
        float dy, w;
    };
    std::vector<Vote> votes;
    std::vector<std::vector<size_t>> cand(bx.size());
    for (size_t k = 0; k < bx.size(); ++k) {
        const Box& b = bx[k];
        for (size_t i = 0; i < shown.size(); ++i) {
            const Box& s = shown[i];
            if (std::fabs(s.x0 - b.x0) > 0.02f || std::fabs(s.x1 - b.x1) > 0.03f) continue;
            if (std::fabs((s.y1 - s.y0) - (b.y1 - b.y0)) > std::max(0.012f, (b.y1 - b.y0) * 0.4f)) continue;
            if (!sameText(s.original, b.original)) continue;
            cand[k].push_back(i);
        }
        for (size_t i : cand[k])
            votes.push_back({((b.y0 + b.y1) - (shown[i].y0 + shown[i].y1)) * 0.5f, 1.0f / static_cast<float>(cand[k].size())});
    }
    std::sort(votes.begin(), votes.end(), [](const Vote& a, const Vote& b) { return a.dy < b.dy; });
    std::vector<float> shifts{0.0f};
    {
        std::vector<char> taken(votes.size(), 0);
        float bestW = 0;
        for (int round = 0; round < 3; ++round) {
            size_t bi = votes.size();
            float bw = 0, bsum = 0;
            for (size_t lo = 0, hi = 0; lo < votes.size(); ++lo) {  // the window [dy - 0.004, dy + 0.004] with the most weight
                float w = 0, sum = 0;
                for (hi = lo; hi < votes.size() && votes[hi].dy - votes[lo].dy <= 0.008f; ++hi)
                    if (!taken[hi]) w += votes[hi].w, sum += votes[hi].w * votes[hi].dy;
                if (w > bw) bw = w, bsum = sum, bi = lo;
            }
            if (bi == votes.size() || bw < (round == 0 ? 0.99f : 1.5f)) break;
            const float dy = bsum / bw;
            for (size_t j = 0; j < votes.size(); ++j)
                if (std::fabs(votes[j].dy - dy) <= 0.01f) taken[j] = 1;
            if (round == 0) bestW = bw, r.scrollDy = dy;
            if (std::fabs(dy) < 0.002f) continue;  // (0 is always tried)
            if (round > 0 && bw < 0.25f * bestW) break;
            shifts.push_back(dy);
        }
    }
    // 2) Match.
    std::vector<char> used(shown.size(), 0);
    bool anyShifted = false;
    for (size_t k = 0; k < bx.size(); ++k) {
        Box& b = bx[k];
        const float tolX = 0.012f, hB = b.y1 - b.y0;
        int bestI = -1;
        float bestDy = 0, bestErr = 1e9f;
        for (size_t i : cand[k]) {
            const Box& s = shown[i];
            if (used[i]) continue;
            for (float dy : shifts) {
                const float slack = dy == 0.0f ? 0.0f : 0.003f;
                const float tolY = std::clamp(hB * 0.35f, 0.003f, 0.012f) + slack;
                const float ey0 = std::fabs(s.y0 + dy - b.y0), ey1 = std::fabs(s.y1 + dy - b.y1);
                if (std::fabs(s.x0 - b.x0) > tolX || std::fabs(s.x1 - b.x1) > tolX || ey0 > tolY || ey1 > tolY) continue;
                const float err = std::fabs(s.x0 - b.x0) + std::fabs(s.x1 - b.x1) + ey0 + ey1 + (s.original == b.original ? 0.0f : 0.002f);
                if (err < bestErr) bestErr = err, bestI = static_cast<int>(i), bestDy = dy;
            }
        }
        if (bestI < 0) continue;
        const Box& s = shown[static_cast<size_t>(bestI)];
        used[static_cast<size_t>(bestI)] = 1;
        if (bestDy == 0.0f && s.original == b.original) {
            b = s;  // as before: the old card where it is
        } else {
            Box moved = s;  // the old card (translation, colours, layout) on the block's place
            moved.x0 = b.x0, moved.x1 = b.x1, moved.y0 = b.y0, moved.y1 = b.y1;
            b = moved;
            anyShifted = true;
            ++r.shifted;
        }
        ++r.kept;
    }
    r.added = static_cast<int>(bx.size()) - r.kept;
    r.gone = static_cast<int>(shown.size()) - r.kept;
    r.same = r.added == 0 && r.gone == 0 && !anyShifted;
    return r;
}

}  // namespace pm::translate
