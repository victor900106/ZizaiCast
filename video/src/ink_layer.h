// 簡報畫筆 (0.8.3): strokes drawn over the live picture -- pen, highlighter,
// arrow -- kept in the phone picture's own space so they follow the
// magnifier, rotation and mirroring, and can be burnt into snapshots and the
// frame tap (recordings).
//
// Coordinates: "U space" = the source picture (decoded, unrotated,
// unmirrored) scaled uniformly so that its short side is 1: u = (x * ax,
// y * ay) for x, y = 0..1 of the picture, ax = max(1, srcW / srcH), ay =
// max(1, srcH / srcW).  A uniform space keeps round caps round and stroke
// widths meaningful (widths are in U units too).  The aspect the strokes were
// drawn on is remembered; a picture with another aspect (the phone turned)
// drops them (checkAspect).
//
// Threading: the UI thread edits (begin / add / end / undo / clear), the
// render thread draws (draw / blendNv12); everything is guarded by one mutex
// and empty() is lock-free, so a layer without strokes costs one atomic load
// per frame.
#pragma once

#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace pm::video {

class InkLayer {
public:
    enum Tool { Pen = 0, Highlighter = 1, Arrow = 2 };
    struct Style {
        int tool = Pen;
        uint32_t rgb = 0xFF3B30;  // 0xRRGGBB
        float width = 0.01f;      // U units
    };

    // ---- editing (UI thread) ----
    // aspect: srcW / srcH of the picture the stroke is drawn on.
    void begin(const Style& st, float ux, float uy, float aspect);
    void add(float ux, float uy);
    // Commits the stroke in progress; false if it was dropped (an arrow
    // shorter than its head) or none was in progress.
    bool end();
    void cancel();
    bool drawing() const;
    bool undo();   // the last committed stroke; false if none
    void clear();  // everything (also a stroke in progress)
    int count() const;
    uint64_t version() const;  // changes with every edit (cache key)
    bool empty() const { return empty_.load(std::memory_order_acquire); }
    float aspect() const;
    // U-space scale of a picture with this aspect.
    static void axes(float aspect, float& ax, float& ay) {
        ax = aspect >= 1 ? aspect : 1.f;
        ay = aspect >= 1 ? 1.f : 1.f / aspect;
    }
    void setCapture(bool inShots, bool inRec) {
        shots_.store(inShots);
        rec_.store(inRec);
    }
    bool inShots() const { return shots_.load(); }
    bool inRec() const { return rec_.load(); }

    // ---- drawing (render thread) ----
    // Every stroke through m (U space -> the target's coordinates); the
    // caller sets the clip.  The target's transform is restored.
    void draw(ID2D1RenderTarget* rt, ID2D1Factory* f, const D2D1_MATRIX_3X2_F& m);
    // Burns the strokes into an NV12 picture of w x h (the decoded picture's
    // own space: what the frame tap delivers), BT.709 video range.  The
    // strokes are rasterized once per change (at most every 33 ms while one
    // is being drawn) and only their bounding box is blended.
    void blendNv12(uint8_t* nv12, int w, int h, int stride, ID2D1Factory* f);
    // The shown picture's aspect: strokes drawn on another aspect are
    // dropped (true: they were).
    bool checkAspect(float aspect);

private:
    struct Stroke {
        Style st;
        std::vector<D2D1_POINT_2F> pts;
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo, head;  // cached (head: arrows)
        size_t geoPts = 0;
        ID2D1Factory* geoFactory = nullptr;
    };
    void updateEmptyLocked() { empty_.store(strokes_.empty() && !cur_, std::memory_order_release); }
    bool buildLocked(Stroke& s, ID2D1Factory* f);
    void drawLocked(ID2D1RenderTarget* rt, ID2D1Factory* f, const D2D1_MATRIX_3X2_F& m);
    bool rasterLocked(int w, int h, ID2D1Factory* f);

    mutable std::mutex m_;
    std::vector<Stroke> strokes_;
    std::unique_ptr<Stroke> cur_;
    float aspect_ = 0;
    uint64_t version_ = 0;  // bumped on every change (raster cache key)
    std::atomic<bool> empty_{true}, shots_{true}, rec_{true};
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> round_;
    ID2D1Factory* roundFactory_ = nullptr;

    // Recording raster (render thread, under m_).
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
    uint64_t rasterVersion_ = ~0ull;
    int rasterW_ = 0, rasterH_ = 0;
    double rasterAt_ = -1e9;
    int bx0_ = 0, by0_ = 0, bx1_ = 0, by1_ = 0;  // bounding box (even-aligned), empty if bx1_ <= bx0_
    std::vector<uint8_t> aY_, pY_;               // full resolution: alpha, premultiplied luma
    std::vector<uint8_t> aC_, pU_, pV_;          // half resolution: alpha, premultiplied Cb / Cr
};

}  // namespace pm::video
