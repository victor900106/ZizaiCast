// 即時翻譯 (live translation, 0.7.8): the overlay of live mode.
//
// - Every block in place over its original text, never the list panel or
//   markers (a translation that does not fit is set smaller and clipped to
//   its block).
// - Scroll tracking: each new picture is drawn into a 256-column copy on the
//   GPU (about 1 MB read back), reduced to a signature and matched against
//   the picture the boxes were recognised on (live_scroll_tracker.h).  The
//   cards are drawn shifted by the scroll at once, clipped to the scrolling
//   area (a static header / footer keeps its own cards); cards scrolled off
//   are not drawn.  A picture that is not a shifted anchor (another page)
//   hides the cards (after two such pictures: no flicker on one odd frame);
//   the next translation (ScreenTranslator's live run) replaces them.
// The layout is made once per set of boxes / view; a scroll only moves it.
#include <algorithm>
#include <chrono>
#include <cmath>

#include "live_scroll_tracker.h"
#include "renderer.h"

namespace pm::video {

namespace {

D2D1_RECT_F inflateR(const D2D1_RECT_F& r, float dx, float dy) { return {r.left - dx, r.top - dy, r.right + dx, r.bottom + dy}; }
D2D1_RECT_F uniteR(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return {std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
}
bool overlapsR(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}
D2D1_RECT_F shiftR(const D2D1_RECT_F& r, float dy) { return {r.left, r.top + dy, r.right, r.bottom + dy}; }
float padXL(float lineH) { return std::clamp(lineH * 0.18f, 1.5f, 6.f); }
float padYL(float lineH) { return std::clamp(lineH * 0.12f, 1.f, 4.f); }
D2D1_COLOR_F rgbL(uint32_t c) { return D2D1::ColorF(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f); }
float lumaL(const D2D1_COLOR_F& c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }
constexpr int kSigW = 256, kSigMaxH = 1024;

}  // namespace

struct Renderer::LiveOverlay {
    bool on = false;
    ScrollTracker tracker;
    ScrollSig grabSig, lastSig;
    double sigAt = -1e9;     // lastPictureAt_ of lastSig
    int fails = 0;           // pictures in a row that are not a shifted anchor
    bool hidden = false;
    std::vector<char> moves;  // per box: follows the scroll
    double trackMs = 0;
    // GPU copy for the signature.
    ID3D11Device* texDev = nullptr;
    ComPtr<ID3D11Texture2D> tex, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    UINT texW = 0, texH = 0;
    // Layout (DIPs, unshifted).
    struct Item {
        int box = 0;
        D2D1_RECT_F r{}, card{}, text{};
        float lineH = 0;
        FitCand fc;
        bool clip = false;  // forced in place: the text is clipped to its card
    };
    std::vector<Item> items;
    bool valid = false;
    float key[10] = {};
    bool signature(ID3D11Device* dev, ID3D11DeviceContext* ctx, UINT w, UINT h,
                   const std::function<void(ID3D11RenderTargetView*, const D3D11_VIEWPORT&)>& draw, ScrollSig& out);
    void updateMoves(const std::vector<TextBox>& boxes);
};

Renderer::LiveOverlay& Renderer::live() {
    if (!live_) live_ = std::make_shared<LiveOverlay>();
    return *live_;
}

bool Renderer::overlayLive() const { return live_ && live_->on; }

void Renderer::liveReleaseDevice() {
    if (!live_) return;
    live_->tex.Reset();
    live_->staging.Reset();
    live_->rtv.Reset();
    live_->texDev = nullptr;  // a new device may get the old one's address
    live_->texW = live_->texH = 0;
}



void Renderer::liveNoteGrab() {
    LiveOverlay& L = live();
    int dw = 0, dh = 0;
    displaySize(dw, dh);
    if (dw <= 0 || dh <= 0 || !shown().valid()) return;
    const UINT h = static_cast<UINT>(std::min(dh, kSigMaxH));
    L.signature(dev_.Get(), ctx_.Get(), kSigW, h,
                  [&](ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp) { drawPicture(rtv, vp, shown(), View{}, false); },
                  L.grabSig);
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
}

bool Renderer::LiveOverlay::signature(ID3D11Device* dev, ID3D11DeviceContext* ctx, UINT w, UINT h,
                          const std::function<void(ID3D11RenderTargetView*, const D3D11_VIEWPORT&)>& draw, ScrollSig& out) {
    if (!dev || !ctx) return false;
    if (texDev != dev || texW != w || texH != h || !tex) {
        tex.Reset();
        staging.Reset();
        rtv.Reset();
        texDev = dev;
        texW = w;
        texH = h;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex)) || FAILED(dev->CreateRenderTargetView(tex.Get(), nullptr, &rtv))) {
            tex.Reset();
            return false;
        }
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging))) {
            tex.Reset();
            return false;
        }
    }
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    draw(rtv.Get(), vp);
    ctx->CopyResource(staging.Get(), tex.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    out = makeScrollSig(static_cast<const uint8_t*>(m.pData), static_cast<int>(w), static_cast<int>(h), static_cast<int>(m.RowPitch));
    ctx->Unmap(staging.Get(), 0);
    return out.valid();
}

// Which boxes follow the scroll (after a tracker update).
void Renderer::LiveOverlay::updateMoves(const std::vector<TextBox>& boxes) {
    moves.assign(boxes.size(), 0);
    for (size_t i = 0; i < boxes.size(); ++i) {
        const auto& b = boxes[i];
        moves[i] = tracker.boxMoves(std::min(b.x0, b.x1), std::min(b.y0, b.y1), std::max(b.x0, b.x1), std::max(b.y0, b.y1)) ? 1 : 0;
    }
}

void Renderer::liveTrack() {
    if (!live_ || !live_->on) return;
    LiveOverlay& L = *live_;
    if (boxes_.empty() || !L.tracker.hasAnchor()) return;
    if (L.sigAt == lastPictureAt_ && L.lastSig.valid()) return;  // no new picture
    int dw = 0, dh = 0;
    displaySize(dw, dh);
    if (dw <= 0 || dh <= 0 || !shown().valid()) return;
    const auto t0 = std::chrono::steady_clock::now();
    const UINT h = static_cast<UINT>(std::min(dh, kSigMaxH));
    const bool ok = L.signature(dev_.Get(), ctx_.Get(), kSigW, h,
                                  [&](ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp) { drawPicture(rtv, vp, shown(), View{}, false); },
                                  L.lastSig);
    if (rtv_) ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    L.sigAt = lastPictureAt_;
    if (!ok) return;
    const auto& r = L.tracker.update(L.lastSig);
    if (r.ok) {
        L.fails = 0;
        L.hidden = false;
    } else if (++L.fails >= 2) {
        L.hidden = true;
    }
    L.updateMoves(boxes_);
    L.trackMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void Renderer::setOverlayLive(bool on) {
    LiveOverlay& L = live();
    if (L.on == on) return;
    L.on = on;
    L.valid = false;
    L.fails = 0;
    L.hidden = false;
    ovValid_ = false;  // (back to the normal layout when off)
    ovHits_ = {};      // no list panel / markers to hit
    if (on) liveBoxesSet();
}

void Renderer::liveBoxesSet() {
    LiveOverlay& L = live();
    L.valid = false;
    L.fails = 0;
    L.hidden = false;
    if (!L.on) return;
    if (!L.grabSig.valid()) {
        L.tracker.clear();
        return;
    }
    L.tracker.setAnchor(L.grabSig);
    L.sigAt = -1e9;  // the picture shown now: tracked at the next frame drawn
    liveTrack();
    if (!L.lastSig.valid() || L.lastSig.rows != L.grabSig.rows) L.updateMoves(boxes_);
}

void Renderer::drawLiveOverlay(const D2D1_RECT_F& pic, float /*radius*/) {
    if (boxes_.empty() || !live_) return;
    LiveOverlay& L = *live_;
    if (L.hidden) return;
    const float pw = pic.right - pic.left, ph = pic.bottom - pic.top;
    if (pw < 8 || ph < 8) return;
    const float s = dpi_ / 96.f;
    // Layout (once per boxes / view).
    const float key[10] = {pic.left, pic.top, pic.right, pic.bottom, view_.zoom, view_.cx, view_.cy, mirror_ ? 1.f : 0.f, s,
                           static_cast<float>(boxes_.size())};
    if (!L.valid || !std::equal(std::begin(key), std::end(key), std::begin(L.key))) {
        std::copy(std::begin(key), std::end(key), L.key);
        L.valid = true;
        L.items.clear();
        for (size_t i = 0; i < boxes_.size(); ++i) {
            const TextBox& b = boxes_[i];
            if (b.text.empty()) continue;
            const D2D1_POINT_2F p0 = contentToDip(pic, b.x0, b.y0), p1 = contentToDip(pic, b.x1, b.y1);
            const D2D1_RECT_F r{std::min(p0.x, p1.x), std::min(p0.y, p1.y), std::max(p0.x, p1.x), std::max(p0.y, p1.y)};
            if (r.right - r.left < 3 || r.bottom - r.top < 3) continue;
            LiveOverlay::Item it;
            it.box = static_cast<int>(i);
            it.r = r;
            it.lineH = (r.bottom - r.top) / static_cast<float>(std::max(1, b.lines));
            L.items.push_back(std::move(it));
        }
        const size_t n = L.items.size();
        std::vector<D2D1_RECT_F> cores(n);
        for (size_t i = 0; i < n; ++i) {
            const auto& it = L.items[i];
            cores[i] = inflateR(it.r, -std::min(1.5f, (it.r.right - it.r.left) * 0.1f), -std::min(it.lineH * 0.22f, 3.f));
        }
        std::vector<D2D1_RECT_F> placed;
        std::vector<char> done(n, 0);
        auto weight = [](float size) { return size < 15 ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_MEDIUM; };
        // 1) The ways of the normal layout (fitsFor): the first that covers no other block or card.
        for (size_t i = 0; i < n; ++i) {
            auto& it = L.items[i];
            const float bw = it.r.right - it.r.left, bh = it.r.bottom - it.r.top;
            const float px = padXL(it.lineH), py = padYL(it.lineH);
            const auto& cands = fitsFor(static_cast<size_t>(it.box), bw, bh, it.lineH, pw);
            for (const FitCand& c : cands) {
                const float tw = c.wide ? c.w : std::max(c.w, bw);
                float tx = it.r.left;
                if (tx + tw > pic.right - px) tx = std::max(pic.left + px, pic.right - px - tw);
                const float ty = it.r.top + (bh - c.h) / 2;
                const D2D1_RECT_F text{tx, ty, tx + tw, ty + c.h};
                D2D1_RECT_F card = inflateR(uniteR(it.r, text), px, c.h > bh ? 0.5f : py * 0.5f);
                if ((card.right - card.left) * (card.bottom - card.top) > 3.5f * std::max(1.f, bw * bh) + 200) continue;
                bool clash = false;
                for (size_t j = 0; j < n && !clash; ++j) clash = j != i && overlapsR(card, cores[j]);
                for (const auto& q : placed) clash = clash || overlapsR(inflateR(card, 1, 1), q);
                if (clash) continue;
                it.fc = c;
                it.card = card;
                it.text = text;
                placed.push_back(card);
                done[i] = 1;
                break;
            }
        }
        // 2) The rest: in place anyway - smaller text, clipped to the block.
        const float minSize = std::max(6.f, 9.f * 96.f / static_cast<float>(dpi_));
        for (size_t i = 0; i < n; ++i) {
            if (done[i]) continue;
            auto& it = L.items[i];
            const float bw = std::max(4.f, it.r.right - it.r.left), bh = it.r.bottom - it.r.top;
            const float px = padXL(it.lineH), py = padYL(it.lineH);
            float size = std::clamp(it.lineH * 0.85f, minSize, 64.f);
            FitCand c;
            for (;;) {
                FitCand t;
                if (setText(boxes_[it.box].text, size, bw, weight(size), true, t)) c = std::move(t);
                if ((c.text && c.h <= bh + 1) || size <= minSize) break;
                size = std::max(minSize, size * 0.9f);
            }
            if (!c.text) continue;
            const float ty = c.h <= bh ? it.r.top + (bh - c.h) / 2 : it.r.top;
            it.text = {it.r.left, ty, it.r.left + std::max(c.w, bw), ty + c.h};
            it.card = inflateR(it.r, px, py * 0.5f);
            it.card.bottom = std::min(std::max(it.card.bottom, it.text.bottom + py * 0.5f), it.r.top + bh * 1.6f + py);
            it.fc = std::move(c);
            it.clip = true;
        }
        L.items.erase(std::remove_if(L.items.begin(), L.items.end(), [](const LiveOverlay::Item& it) { return !it.fc.text; }),
                      L.items.end());
        ovHits_ = {};
        ovHits_.inPlace = static_cast<int>(L.items.size());
    }
    // The scroll: shift (DIPs) and the scrolling area.
    const ScrollTracker::Result& tr = L.tracker.last();
    const bool moved = tr.ok && tr.moved;
    const float dyDip = moved ? contentToDip(pic, 0, tr.dy).y - contentToDip(pic, 0, 0).y : 0.f;
    D2D1_RECT_F area = pic;
    if (moved) {
        area.top = std::max(pic.top, contentToDip(pic, 0, tr.top).y);
        area.bottom = std::min(pic.bottom, contentToDip(pic, 0, tr.bottom).y);
    }
    const int filter = view_.filter;
    const bool hc = ovDark_ || filter == 1 || filter == 4;
    const D2D1_COLOR_F yellow = D2D1::ColorF(1, 0.92f, 0.1f);
    auto adjust = [&](D2D1_COLOR_F c) {
        if (filter == 2) {
            const float l = lumaL(c);
            c = D2D1::ColorF(l, l, l);
        } else if (filter == 3) {
            c = D2D1::ColorF(1 - c.r, 1 - c.g, 1 - c.b);
        }
        return c;
    };
    d2dTarget_->PushAxisAlignedClip(pic, D2D1_ANTIALIAS_MODE_ALIASED);
    D2D1_MATRIX_3X2_F m0;
    d2dTarget_->GetTransform(&m0);
    for (const auto& it : L.items) {
        const TextBox& b = boxes_[it.box];
        const bool follows = moved && it.box < static_cast<int>(L.moves.size()) && L.moves[it.box];
        const float dy = follows ? dyDip : 0.f;
        const D2D1_RECT_F card = shiftR(it.card, dy);
        if (follows && (card.bottom <= area.top || card.top >= area.bottom)) continue;  // scrolled off
        if (follows) d2dTarget_->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_ALIASED);
        if (dy != 0) d2dTarget_->SetTransform(D2D1::Matrix3x2F::Translation(0, dy) * m0);
        if (showOriginal_) {
            const float rad = std::clamp((it.r.bottom - it.r.top) * 0.18f, 2.f, 8.f);
            d2dTarget_->DrawRoundedRectangle({it.r, rad, rad}, brush(pal_.accent, 0.75f), 1.5f);
        } else {
            const float crad = std::clamp(it.lineH * 0.2f, 2.f, 6.f);
            D2D1_COLOR_F bg, fg;
            if (hc) {
                bg = filter == 4 ? D2D1::ColorF(0, 0, 0) : pal_.card;
                fg = filter == 4 ? yellow : pal_.fg;
                d2dTarget_->FillRoundedRectangle({it.card, crad, crad}, brush(bg, 0.97f));
                d2dTarget_->DrawRoundedRectangle({it.card, crad, crad}, brush(filter == 4 ? yellow : pal_.accent, 0.7f), 1.f);
            } else {
                bg = adjust(b.colors ? rgbL(b.bg) : D2D1::ColorF(0.97f, 0.97f, 0.96f));
                fg = adjust(b.colors ? rgbL(b.fg) : D2D1::ColorF(0.1f, 0.1f, 0.1f));
                if (std::fabs(lumaL(bg) - lumaL(fg)) < 0.45f) fg = lumaL(bg) > 0.5f ? D2D1::ColorF(0.08f, 0.08f, 0.08f) : D2D1::ColorF(1, 1, 1);
                d2dTarget_->FillRoundedRectangle({inflateR(it.card, 1.6f, 1.6f), crad + 1.6f, crad + 1.6f}, brush(bg, 0.35f));
                d2dTarget_->FillRoundedRectangle({inflateR(it.card, 0.8f, 0.8f), crad + 0.8f, crad + 0.8f}, brush(bg, 0.65f));
                d2dTarget_->FillRoundedRectangle({it.card, crad, crad}, brush(bg));
            }
            if (it.clip) d2dTarget_->PushAxisAlignedClip(it.card, D2D1_ANTIALIAS_MODE_ALIASED);
            d2dTarget_->DrawTextLayout({it.text.left, it.text.top}, it.fc.text.Get(), brush(fg));
            if (it.clip) d2dTarget_->PopAxisAlignedClip();
            if (b.online) {
                const float bfs = std::clamp(it.lineH * 0.36f, 8.f, 11.f);
                const D2D1_SIZE_F bs = onlineBadge(0, 0, bfs, true, filter == 4, false);
                onlineBadge(it.card.right + bs.height * 0.25f, it.card.top - bs.height * 0.55f, bfs, true, filter == 4);
            }
        }
        if (dy != 0) d2dTarget_->SetTransform(m0);
        if (follows) d2dTarget_->PopAxisAlignedClip();
    }
    d2dTarget_->PopAxisAlignedClip();
}

}  // namespace pm::video
