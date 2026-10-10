// 即時翻譯 scroll tracking (see live_scroll_tracker.h).
#include "live_scroll_tracker.h"

#include <emmintrin.h>

#include <algorithm>
#include <cmath>

namespace pm::video {

namespace {

constexpr int kB = ScrollSig::kBands;
// Per row (sum over the 32 bands): below kNoise the row is "the same"
// (video compression noise is ~1-2 levels per band); a row's mismatch counts
// at most kCap (a new line of text scrolled in is as bad as any other).
constexpr int kNoise = 80, kCap = 32 * 20;

inline int sad32(const uint8_t* a, const uint8_t* b) {
    const __m128i a0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a));
    const __m128i a1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + 16));
    const __m128i b0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b));
    const __m128i b1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + 16));
    const __m128i s = _mm_add_epi64(_mm_sad_epu8(a0, b0), _mm_sad_epu8(a1, b1));
    return _mm_cvtsi128_si32(s) + _mm_cvtsi128_si32(_mm_srli_si128(s, 8));
}

}  // namespace

static_assert(ScrollSig::kBands == 32, "sad32 compares 32 bands");

ScrollSig makeScrollSig(const uint8_t* bgra, int w, int h, int stride) {
    ScrollSig s;
    if (!bgra || w < kB || h <= 0) return s;
    s.rows = h;
    s.v.resize(static_cast<size_t>(h) * kB);
    std::vector<int> x0(kB + 1);
    for (int b = 0; b <= kB; ++b) x0[b] = b * w / kB;
    for (int y = 0; y < h; ++y) {
        const uint8_t* p = bgra + static_cast<size_t>(y) * stride;
        uint8_t* o = s.v.data() + static_cast<size_t>(y) * kB;
        for (int b = 0; b < kB; ++b) {
            int acc = 0;
            for (int x = x0[b]; x < x0[b + 1]; ++x) {
                const uint8_t* q = p + x * 4;
                acc += q[0] * 29 + q[1] * 150 + q[2] * 77;
            }
            o[b] = static_cast<uint8_t>(acc / (256 * std::max(1, x0[b + 1] - x0[b])));
        }
    }
    return s;
}

void ScrollTracker::setAnchor(const ScrollSig& s) {
    anchor_ = s;
    predRows_ = 0;
    last_ = {};
    last_.ok = s.valid();
    // The picture shown now may already have moved on from the anchor.
    if (cur_.valid()) {
        const ScrollSig c = cur_;
        update(c);
    }
}

void ScrollTracker::clear() {
    anchor_ = {};
    cur_ = {};
    last_ = {};
    predRows_ = 0;
}

// Mean capped mismatch of the changed rows (every step-th) at shift d, plus
// a penalty for the changed rows with nothing to compare to (outside the
// anchor at that shift): a tiny overlap that happens to match must not win.
double ScrollTracker::score(int d, int step, int* overlap) const {
    long long s = 0;
    int n = 0, all = 0;
    for (size_t i = 0; i < changed_.size(); i += step) {
        ++all;
        const int y = changed_[i], ya = y - d;
        if (ya < lo_ || ya >= hi_) continue;  // (not the anchor's header / footer either)
        s += std::min(sad32(cur_.row(y), anchor_.row(ya)), kCap);
        ++n;
    }
    if (overlap) *overlap = n;
    if (n == 0) return 1e9;
    return static_cast<double>(s) / (static_cast<double>(n) * kCap) + 0.15 * (1.0 - static_cast<double>(n) / all);
}

const ScrollTracker::Result& ScrollTracker::update(const ScrollSig& cur) {
    cur_ = cur;
    Result r;
    if (!anchor_.valid() || cur.rows != anchor_.rows) {
        last_ = r;  // not ok: another picture size (rotated, cropped)
        return last_;
    }
    const int R = cur.rows;
    changed_.clear();
    for (int y = 0; y < R; ++y)
        if (sad32(cur.row(y), anchor_.row(y)) > kNoise) changed_.push_back(y);
    r.changedRows = static_cast<int>(changed_.size());
    if (r.changedRows <= std::max(2, R / 300)) {  // the anchor itself
        r.ok = true;
        predRows_ = 0;
        last_ = r;
        return last_;
    }
    r.moved = true;
    r.top = static_cast<float>(changed_.front()) / R;
    r.bottom = static_cast<float>(changed_.back() + 1) / R;
    lo_ = changed_.front();
    hi_ = changed_.back() + 1;
    const int N = r.changedRows;
    const int minOverlap = std::max(6, std::min(N / 2, R / 40));
    const int maxD = R - minOverlap;
    // Coarse: every other shift, about 160 of the changed rows.
    const int step = std::max(1, N / 160);
    struct Cand {
        int d;
        double s;
    };
    std::vector<Cand> coarse;
    coarse.reserve(static_cast<size_t>(2 * maxD + 1));
    for (int d = -maxD; d <= maxD; d += 2) {
        int n = 0;
        const double s = score(d, step, &n);
        if (n * step >= minOverlap) coarse.push_back({d, s});
    }
    if (coarse.empty()) {
        last_ = r;
        return last_;
    }
    // The best few distinct minima and the predicted shift, refined on every row.
    std::vector<int> seeds{static_cast<int>(std::lround(predRows_))};
    {
        std::vector<Cand> sorted = coarse;
        std::partial_sort(sorted.begin(), sorted.begin() + std::min<size_t>(sorted.size(), 24), sorted.end(),
                          [](const Cand& a, const Cand& b) { return a.s < b.s; });
        for (size_t i = 0; i < std::min<size_t>(sorted.size(), 24) && seeds.size() < 5; ++i) {
            bool near = false;
            for (int sd : seeds) near = near || std::abs(sd - sorted[i].d) <= 6;
            if (!near) seeds.push_back(sorted[i].d);
        }
    }
    const double prior = 0.05 / R;  // per row away from the previous shift (look-alike rows of a list)
    int bestD = 0;
    double best = 1e18, bestRaw = 1e9;
    int bestN = 0;
    for (int sd : seeds)
        for (int d = sd - 3; d <= sd + 3; ++d) {
            if (d < -maxD || d > maxD) continue;
            int n = 0;
            const double s = score(d, 1, &n);
            if (n < minOverlap) continue;
            const double v = s + prior * std::fabs(d - predRows_);
            if (v < best) best = v, bestRaw = s, bestD = d, bestN = n;
        }
    if (best >= 1e17) {
        last_ = r;
        return last_;
    }
    // Sub-row: a parabola through the neighbours.
    double sub = 0;
    {
        int n1 = 0, n2 = 0;
        const double a = score(bestD - 1, 1, &n1), c = score(bestD + 1, 1, &n2);
        const double den = a - 2 * bestRaw + c;
        if (n1 >= minOverlap && n2 >= minOverlap && den > 1e-9) sub = std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
    }
    // Confidence: the overlapping changed rows match (the 0.15 overlap
    // penalty taken out again).
    const double match = bestRaw - 0.15 * (1.0 - static_cast<double>(bestN) / N);
    r.cost = static_cast<float>(match);
    r.ok = match < 0.42;  // (a true shift: <= 0.35 even at half-row phase; another page: >= 0.6)
    if (r.ok) {
        const double d = bestD + sub;
        r.dy = static_cast<float>(d / R);
        predRows_ = static_cast<float>(d);
    }
    last_ = r;
    return last_;
}

bool ScrollTracker::boxMoves(float x0, float y0, float x1, float y1) const {
    if (!anchor_.valid() || !cur_.valid() || cur_.rows != anchor_.rows || !last_.moved) return false;  // (no shift)
    const int R = anchor_.rows;
    const int r0 = std::clamp(static_cast<int>(std::floor(y0 * R)), 0, R - 1);
    const int r1 = std::clamp(static_cast<int>(std::ceil(y1 * R)), r0 + 1, R);
    const int b0 = std::clamp(static_cast<int>(std::floor(x0 * kB)), 0, kB - 1);
    const int b1 = std::clamp(static_cast<int>(std::ceil(x1 * kB)), b0 + 1, kB);
    // Mean difference per band where it was, and where the shift takes it.
    const int d = static_cast<int>(std::lround(last_.dy * R));
    long long same = 0, moved = 0;
    int nMoved = 0;
    for (int y = r0; y < r1; ++y) {
        const uint8_t *a = anchor_.row(y), *c = cur_.row(y);
        for (int b = b0; b < b1; ++b) same += std::abs(a[b] - c[b]);
        if (y + d < 0 || y + d >= R) continue;
        const uint8_t* m = cur_.row(y + d);
        for (int b = b0; b < b1; ++b) moved += std::abs(a[b] - m[b]);
        ++nMoved;
    }
    const double cells = static_cast<double>(r1 - r0) * (b1 - b0);
    const double sameMean = same / cells;
    // Static: still identical where it was (noise level), and clearly a
    // better match there than at the shifted place (a box over plain
    // background matches both: it follows the scroll).
    if (sameMean > 3) return true;
    if (!nMoved || d == 0) return false;
    const double movedMean = moved / (static_cast<double>(nMoved) * (b1 - b0));
    return !(sameMean * 2 + 0.5 < movedMean);
}

}  // namespace pm::video
