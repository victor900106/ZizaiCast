// 即時翻譯 (live translation, 0.7.8): scroll tracking of the overlay.
//
// A picture is reduced to a signature: per row, the mean luma of kBands
// column bands (the renderer draws the shown picture into a 256-column copy
// on the GPU and reads that back; tests build it from a screenshot).
// ScrollTracker matches the signature of the current picture against the
// one the shown boxes were recognised on (the anchor) and finds the vertical
// shift of the part that scrolled; rows identical to the anchor (a header,
// a tab bar, a footer) are left out, boxes on them stay where they are.
// Always against the anchor (no accumulated drift); a picture that is not a
// shifted anchor (another page, a dialog) is reported as not ok.
// Pure CPU, no Windows dependencies; about 1 ms per picture (SSE2).
#pragma once

#include <cstdint>
#include <vector>

namespace pm::video {

struct ScrollSig {
    static constexpr int kBands = 32;
    int rows = 0;
    std::vector<uint8_t> v;  // rows * kBands
    bool valid() const { return rows > 0; }
    const uint8_t* row(int y) const { return v.data() + static_cast<size_t>(y) * kBands; }
};

// bgra: w x h pixels, `stride` bytes per row; w >= kBands (e.g. 256).
ScrollSig makeScrollSig(const uint8_t* bgra, int w, int h, int stride);

class ScrollTracker {
public:
    struct Result {
        bool ok = false;            // a confident answer (false: the picture changed some other way)
        bool moved = false;         // something differs from the anchor
        float dy = 0;               // content shift (0..1 of the height): anchor y -> y + dy
        float top = 0, bottom = 1;  // the area that changed (content units); boxes outside it are static
        float cost = 0;             // match cost of the shift (0 perfect .. 1)
        int changedRows = 0;
    };
    void setAnchor(const ScrollSig& s);
    void clear();
    bool hasAnchor() const { return anchor_.valid(); }
    const Result& update(const ScrollSig& cur);
    const Result& last() const { return last_; }
    // A box of the anchor (content units): true = it moves with the scroll;
    // false = still identical where it was (a header, a footer, a sticky bar).
    bool boxMoves(float x0, float y0, float x1, float y1) const;

private:
    double score(int d, int step, int* overlap) const;
    ScrollSig anchor_, cur_;
    Result last_;
    float predRows_ = 0;        // the previous shift (rows): preferred among look-alikes
    std::vector<int> changed_;  // rows that differ from the anchor at the same place
    int lo_ = 0, hi_ = 0;       // the changed rows' span (the scrolling area)
};

}  // namespace pm::video
