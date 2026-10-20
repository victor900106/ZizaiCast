// 簡報畫筆 (0.8.3): see ink_layer.h.
#include "ink_layer.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace pm::video {

using Microsoft::WRL::ComPtr;

namespace {

constexpr float kHighlighterAlpha = 0.45f;

D2D1_COLOR_F color(uint32_t rgb, float a) {
    return D2D1::ColorF(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}

// Arrow head length / half width for a stroke width (U units).
float headLen(float w) { return w * 4.2f + 0.012f; }

}  // namespace

void InkLayer::begin(const Style& st, float ux, float uy, float aspect) {
    std::lock_guard lk(m_);
    if (!strokes_.empty() && aspect_ > 0 && std::fabs(aspect / aspect_ - 1.f) > 0.03f) strokes_.clear();
    if (strokes_.empty()) aspect_ = aspect;
    cur_ = std::make_unique<Stroke>();
    cur_->st = st;
    cur_->pts.push_back({ux, uy});
    ++version_;
    updateEmptyLocked();
}

void InkLayer::add(float ux, float uy) {
    std::lock_guard lk(m_);
    if (!cur_) return;
    if (cur_->st.tool == Arrow && cur_->pts.size() >= 2) cur_->pts.back() = {ux, uy};  // start + current end only
    else cur_->pts.push_back({ux, uy});
    ++version_;
}

bool InkLayer::end() {
    std::lock_guard lk(m_);
    if (!cur_) return false;
    std::unique_ptr<Stroke> s = std::move(cur_);
    ++version_;
    bool keep = true;
    if (s->st.tool == Arrow) {
        const D2D1_POINT_2F a = s->pts.front(), b = s->pts.back();
        keep = s->pts.size() >= 2 && std::hypot(b.x - a.x, b.y - a.y) >= headLen(s->st.width) * 0.8f;
    }
    if (keep) {
        s->geo.Reset();  // rebuilt from the final points
        s->head.Reset();
        s->geoPts = 0;
        strokes_.push_back(std::move(*s));
    }
    updateEmptyLocked();
    return keep;
}

void InkLayer::cancel() {
    std::lock_guard lk(m_);
    if (!cur_) return;
    cur_.reset();
    ++version_;
    updateEmptyLocked();
}

bool InkLayer::drawing() const {
    std::lock_guard lk(m_);
    return cur_ != nullptr;
}

bool InkLayer::undo() {
    std::lock_guard lk(m_);
    if (cur_) {
        cur_.reset();
        ++version_;
        updateEmptyLocked();
        return true;
    }
    if (strokes_.empty()) return false;
    strokes_.pop_back();
    ++version_;
    updateEmptyLocked();
    return true;
}

void InkLayer::clear() {
    std::lock_guard lk(m_);
    if (strokes_.empty() && !cur_) return;
    strokes_.clear();
    cur_.reset();
    ++version_;
    updateEmptyLocked();
}

int InkLayer::count() const {
    std::lock_guard lk(m_);
    return static_cast<int>(strokes_.size());
}

uint64_t InkLayer::version() const {
    std::lock_guard lk(m_);
    return version_;
}

float InkLayer::aspect() const {
    std::lock_guard lk(m_);
    return aspect_;
}

bool InkLayer::checkAspect(float aspect) {
    if (empty() || aspect <= 0) return false;
    std::lock_guard lk(m_);
    if (aspect_ <= 0 || std::fabs(aspect / aspect_ - 1.f) <= 0.03f) return false;
    if (strokes_.empty() && !cur_) return false;
    strokes_.clear();
    cur_.reset();
    ++version_;
    updateEmptyLocked();
    return true;
}

bool InkLayer::buildLocked(Stroke& s, ID2D1Factory* f) {
    if (s.geo && s.geoPts == s.pts.size() && s.geoFactory == f) return true;
    s.geo.Reset();
    s.head.Reset();
    s.geoFactory = f;
    s.geoPts = s.pts.size();
    if (s.pts.empty()) return false;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(&s.geo)) || FAILED(s.geo->Open(&sink))) {
        s.geo.Reset();
        return false;
    }
    const auto& p = s.pts;
    if (s.st.tool == Arrow) {
        const D2D1_POINT_2F a = p.front(), b = p.back();
        const float len = std::hypot(b.x - a.x, b.y - a.y);
        const float hl = std::min(headLen(s.st.width), len);
        const float dx = len > 0 ? (b.x - a.x) / len : 1.f, dy = len > 0 ? (b.y - a.y) / len : 0.f;
        // The shaft stops inside the head so its round cap never pokes past the tip.
        const D2D1_POINT_2F shaftEnd{b.x - dx * hl * 0.6f, b.y - dy * hl * 0.6f};
        sink->BeginFigure(a, D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine(len > hl * 0.6f ? shaftEnd : D2D1_POINT_2F{a.x + 1e-4f, a.y});
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        ComPtr<ID2D1GeometrySink> hs;
        if (SUCCEEDED(f->CreatePathGeometry(&s.head)) && SUCCEEDED(s.head->Open(&hs))) {
            const D2D1_POINT_2F base{b.x - dx * hl, b.y - dy * hl};
            const float hw = hl * 0.52f;
            hs->BeginFigure(b, D2D1_FIGURE_BEGIN_FILLED);
            hs->AddLine({base.x - dy * hw, base.y + dx * hw});
            hs->AddLine({base.x + dy * hw, base.y - dx * hw});
            hs->EndFigure(D2D1_FIGURE_END_CLOSED);
            hs->Close();
        } else {
            s.head.Reset();
        }
        return true;
    }
    sink->BeginFigure(p[0], D2D1_FIGURE_BEGIN_HOLLOW);
    if (p.size() == 1) {
        sink->AddLine({p[0].x + 1e-4f, p[0].y});  // a dot (the round caps)
    } else if (p.size() == 2) {
        sink->AddLine(p[1]);
    } else {
        // Smooth: quadratic curves through the midpoints, the points as controls.
        for (size_t i = 1; i + 1 < p.size(); ++i) {
            const D2D1_POINT_2F mid{(p[i].x + p[i + 1].x) / 2, (p[i].y + p[i + 1].y) / 2};
            sink->AddQuadraticBezier({p[i], mid});
        }
        sink->AddLine(p.back());
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    return true;
}

void InkLayer::drawLocked(ID2D1RenderTarget* rt, ID2D1Factory* f, const D2D1_MATRIX_3X2_F& m) {
    if (!round_ || roundFactory_ != f) {
        round_.Reset();
        D2D1_STROKE_STYLE_PROPERTIES sp{};
        sp.startCap = sp.endCap = sp.dashCap = D2D1_CAP_STYLE_ROUND;
        sp.lineJoin = D2D1_LINE_JOIN_ROUND;
        f->CreateStrokeStyle(sp, nullptr, 0, &round_);
        roundFactory_ = f;
    }
    D2D1_MATRIX_3X2_F old;
    rt->GetTransform(&old);
    rt->SetTransform(*D2D1::Matrix3x2F::ReinterpretBaseType(&m) * *D2D1::Matrix3x2F::ReinterpretBaseType(&old));
    ComPtr<ID2D1SolidColorBrush> br;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &br);
    auto one = [&](Stroke& s) {
        if (!br || !buildLocked(s, f) || !s.geo) return;
        // A highlighter is one geometry: its own overlaps are not darker.
        br->SetColor(color(s.st.rgb, s.st.tool == Highlighter ? kHighlighterAlpha : 1.f));
        rt->DrawGeometry(s.geo.Get(), br.Get(), s.st.width, round_.Get());
        if (s.head) {
            rt->FillGeometry(s.head.Get(), br.Get());
            rt->DrawGeometry(s.head.Get(), br.Get(), s.st.width * 0.35f, round_.Get());  // soften the head's corners
        }
    };
    for (Stroke& s : strokes_) one(s);
    if (cur_) one(*cur_);
    rt->SetTransform(old);
}

void InkLayer::draw(ID2D1RenderTarget* rt, ID2D1Factory* f, const D2D1_MATRIX_3X2_F& m) {
    if (empty() || !rt || !f) return;
    std::lock_guard lk(m_);
    drawLocked(rt, f, m);
}

// The strokes at w x h (the decoded picture's pixels) -> alpha / premultiplied
// Y, Cb, Cr over their bounding box.
bool InkLayer::rasterLocked(int w, int h, ID2D1Factory* f) {
    if (!wic_ && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_))))
        return false;
    ComPtr<IWICBitmap> bmp;
    if (FAILED(wic_->CreateBitmap(static_cast<UINT>(w), static_cast<UINT>(h), GUID_WICPixelFormat32bppPBGRA,
                                  WICBitmapCacheOnDemand, &bmp)))
        return false;
    ComPtr<ID2D1RenderTarget> rt;
    const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                    96.f, 96.f);
    if (FAILED(f->CreateWicBitmapRenderTarget(bmp.Get(), props, &rt))) return false;
    float ax, ay;
    axes(aspect_ > 0 ? aspect_ : static_cast<float>(w) / h, ax, ay);
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    drawLocked(rt.Get(), f, D2D1::Matrix3x2F::Scale(w / ax, h / ay));
    if (FAILED(rt->EndDraw())) return false;
    rt.Reset();
    ComPtr<IWICBitmapLock> lock;
    const WICRect all{0, 0, w, h};
    if (FAILED(bmp->Lock(&all, WICBitmapLockRead, &lock))) return false;
    UINT stride = 0, size = 0;
    BYTE* px = nullptr;
    lock->GetStride(&stride);
    if (FAILED(lock->GetDataPointer(&size, &px)) || !px) return false;
    // Bounding box of everything drawn, even-aligned (the chroma grid).
    int x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (int y = 0; y < h; ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(px + static_cast<size_t>(y) * stride);
        int first = -1, last = -1;
        for (int x = 0; x < w; ++x)
            if (row[x] >> 24) {
                if (first < 0) first = x;
                last = x;
            }
        if (first < 0) continue;
        x0 = std::min(x0, first), x1 = std::max(x1, last + 1);
        y0 = std::min(y0, y), y1 = y + 1;
    }
    bx0_ = by0_ = bx1_ = by1_ = 0;
    if (x1 <= x0 || y1 <= y0) return true;  // nothing visible
    bx0_ = x0 & ~1, by0_ = y0 & ~1;
    bx1_ = std::min(w & ~1, (x1 + 1) & ~1), by1_ = std::min(h & ~1, (y1 + 1) & ~1);
    const int bw = bx1_ - bx0_, bh = by1_ - by0_;
    if (bw <= 0 || bh <= 0) {
        bx1_ = bx0_;
        return true;
    }
    aY_.assign(static_cast<size_t>(bw) * bh, 0);
    pY_.assign(aY_.size(), 0);
    aC_.assign(static_cast<size_t>(bw / 2) * (bh / 2), 0);
    pU_.assign(aC_.size(), 0);
    pV_.assign(aC_.size(), 0);
    std::vector<float> su(aC_.size(), 0.f), sv(aC_.size(), 0.f), sa(aC_.size(), 0.f);
    for (int y = by0_; y < by1_; ++y) {
        const uint8_t* row = px + static_cast<size_t>(y) * stride;
        for (int x = bx0_; x < bx1_; ++x) {
            const uint8_t* p = row + x * 4;
            const int a = p[3];
            if (!a) continue;
            // Premultiplied BGRA -> straight RGB 0..1 -> BT.709 video range.
            const float r = std::min(1.f, p[2] / static_cast<float>(a)), g = std::min(1.f, p[1] / static_cast<float>(a)),
                        b = std::min(1.f, p[0] / static_cast<float>(a));
            const float ey = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            const float yv = 16 + 219 * ey, cb = 128 + 224 * (b - ey) / 1.8556f, cr = 128 + 224 * (r - ey) / 1.5748f;
            const size_t i = static_cast<size_t>(y - by0_) * bw + (x - bx0_);
            aY_[i] = static_cast<uint8_t>(a);
            pY_[i] = static_cast<uint8_t>(std::lround(yv * a / 255.f));
            const size_t c = static_cast<size_t>((y - by0_) / 2) * (bw / 2) + (x - bx0_) / 2;
            const float af = a / 255.f;
            sa[c] += af;
            su[c] += cb * af;
            sv[c] += cr * af;
        }
    }
    for (size_t c = 0; c < aC_.size(); ++c) {
        if (sa[c] <= 0) continue;
        aC_[c] = static_cast<uint8_t>(std::lround(std::min(1.f, sa[c] / 4) * 255));
        pU_[c] = static_cast<uint8_t>(std::lround(std::min(255.f, su[c] / 4)));
        pV_[c] = static_cast<uint8_t>(std::lround(std::min(255.f, sv[c] / 4)));
    }
    return true;
}

void InkLayer::blendNv12(uint8_t* nv12, int w, int h, int stride, ID2D1Factory* f) {
    if (empty() || !nv12 || w < 2 || h < 2 || !f) return;
    std::lock_guard lk(m_);
    const double now = static_cast<double>(GetTickCount64());
    if (rasterVersion_ != version_ || rasterW_ != w || rasterH_ != h) {
        // While a stroke is being drawn every mouse move changes it: at most ~30 rasters a second.
        if (!cur_ || now - rasterAt_ >= 33 || rasterW_ != w || rasterH_ != h) {
            rasterAt_ = now;
            rasterW_ = w, rasterH_ = h;
            rasterVersion_ = version_;
            if (!rasterLocked(w, h, f)) bx0_ = bx1_ = 0;
        }
    }
    if (bx1_ <= bx0_ || by1_ <= by0_ || bx1_ > w || by1_ > h) return;
    const int bw = bx1_ - bx0_;
    for (int y = by0_; y < by1_; ++y) {
        uint8_t* row = nv12 + static_cast<size_t>(y) * stride;
        const uint8_t* a = aY_.data() + static_cast<size_t>(y - by0_) * bw;
        const uint8_t* p = pY_.data() + static_cast<size_t>(y - by0_) * bw;
        for (int x = 0; x < bw; ++x) {
            const int al = a[x];
            if (!al) continue;
            uint8_t& v = row[bx0_ + x];
            v = static_cast<uint8_t>(std::min(255, (v * (255 - al) + 127) / 255 + p[x]));
        }
    }
    uint8_t* uv = nv12 + static_cast<size_t>(h) * stride;
    const int cw = bw / 2;
    for (int cy = by0_ / 2; cy < by1_ / 2; ++cy) {
        uint8_t* row = uv + static_cast<size_t>(cy) * stride;
        const size_t base = static_cast<size_t>(cy - by0_ / 2) * cw;
        for (int cx = 0; cx < cw; ++cx) {
            const int al = aC_[base + cx];
            if (!al) continue;
            uint8_t* q = row + (bx0_ / 2 + cx) * 2;
            q[0] = static_cast<uint8_t>(std::min(255, (q[0] * (255 - al) + 127) / 255 + pU_[base + cx]));
            q[1] = static_cast<uint8_t>(std::min(255, (q[1] * (255 - al) + 127) / 255 + pV_[base + cx]));
        }
    }
}

}  // namespace pm::video
