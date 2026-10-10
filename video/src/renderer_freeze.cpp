// Renderer: freeze / view (zoom, pan) and the frozen picture copy.
// 拆檔 0.7.9：自 renderer.cpp 原樣搬出。
#include "renderer.h"

#include <d3dcompiler.h>
#include <mfapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "log.h"
#include "pm/i18n.h"
#include "renderer_internal.h"

namespace pm::video {

using namespace detail;

// ---------------------------------------------------------------------------
// Magnifier, low-vision filters, freeze, translated-text overlay

Renderer::View Renderer::clampView(View v) {
    if (!(v.zoom >= 1)) v.zoom = 1;  // also NaN
    v.zoom = std::min(v.zoom, kMaxZoom);
    const float half = 0.5f / v.zoom;
    v.cx = std::isfinite(v.cx) ? std::clamp(v.cx, half, 1 - half) : 0.5f;
    v.cy = std::isfinite(v.cy) ? std::clamp(v.cy, half, 1 - half) : 0.5f;
    v.filter = std::clamp(v.filter, 0, 4);
    return v;
}

void Renderer::setView(const View& v) {
    const View n = clampView(v);
    if (std::fabs(n.zoom - view_.zoom) > 1e-4f) zoomAt_ = clockMs();
    view_ = n;
}

bool Renderer::copyPicture(const Picture& from, Picture& to) {
    if (!dev_ || !from.valid()) return false;
    const bool ok = from.bgra ? ensureBgra(to, from.w, from.h) : ensureTextures(to, from.hw, from.tenBit, from.w, from.h);
    if (!ok) return false;
    if (from.hw) {
        ctx_->CopyResource(to.nv12.Get(), from.nv12.Get());
    } else {
        ctx_->CopyResource(to.y.Get(), from.y.Get());
        if (!from.bgra) ctx_->CopyResource(to.uv.Get(), from.uv.Get());
    }
    to.fmt = from.fmt;
    return true;
}

void Renderer::setFrozen(bool on) {
    if (on == frozenOn_) return;
    frozenOn_ = on;
    frozen_ = {};
    frozenCpu_ = {};
    frozenWasUp_ = false;
    if (on && cur_.valid() && havePicture_ && copyPicture(cur_, frozen_)) keepFrozenCopy();  // else: from the next picture
}

void Renderer::keepFrozenCopy() {
    const double t0 = clockMs();
    if (!readBackPicture(frozen_, frozenCpu_)) {
        frozenCpu_ = {};
        log("freeze: could not keep a CPU copy of the frozen picture (a device loss ends the freeze)");
        return;
    }
    log("freeze: CPU copy of the frozen picture kept (%ux%u, %.1f MB, %.1f ms)", frozenCpu_.w, frozenCpu_.h,
        (frozenCpu_.y.size() + frozenCpu_.uv.size()) / 1048576.0, clockMs() - t0);
}

bool Renderer::readBackPicture(const Picture& p, CpuPicture& c) {
    c = {};
    if (!p.valid() || !dev_ || !ctx_) return false;
    auto stage = [&](ID3D11Texture2D* t, ComPtr<ID3D11Texture2D>& st) {
        if (!t) return false;
        D3D11_TEXTURE2D_DESC td{};
        t->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &st))) return false;
        ctx_->CopyResource(st.Get(), t);
        return true;
    };
    auto rows = [](const D3D11_MAPPED_SUBRESOURCE& m, size_t first, UINT n, UINT rowBytes, std::vector<uint8_t>& out) {
        out.resize(static_cast<size_t>(n) * rowBytes);
        const auto* src = static_cast<const uint8_t*>(m.pData) + first * m.RowPitch;
        for (UINT r = 0; r < n; ++r)
            std::memcpy(out.data() + static_cast<size_t>(r) * rowBytes, src + static_cast<size_t>(r) * m.RowPitch, rowBytes);
    };
    const UINT bpp = p.tenBit ? 2 : 1;
    c.bgra = p.bgra;
    c.tenBit = p.tenBit;
    c.w = p.w;
    c.h = p.h;
    c.fmt = p.fmt;
    c.yRow = p.bgra ? p.w * 4 : p.w * bpp;
    c.uvH = (p.h + 1) / 2;
    c.uvRow = (p.w + 1) / 2 * 2 * bpp;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (p.hw) {  // NV12 / P010: the UV plane follows the Y plane (RowPitch x Height)
        ComPtr<ID3D11Texture2D> st;
        if (!stage(p.nv12.Get(), st) || FAILED(ctx_->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        rows(m, 0, p.h, c.yRow, c.y);
        rows(m, p.h, c.uvH, c.uvRow, c.uv);
        ctx_->Unmap(st.Get(), 0);
        return true;
    }
    ComPtr<ID3D11Texture2D> sy, suv;
    if (!stage(p.y.Get(), sy) || (!p.bgra && !stage(p.uv.Get(), suv))) return false;
    if (FAILED(ctx_->Map(sy.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    rows(m, 0, p.h, c.yRow, c.y);
    ctx_->Unmap(sy.Get(), 0);
    if (p.bgra) return true;
    if (FAILED(ctx_->Map(suv.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    rows(m, 0, c.uvH, c.uvRow, c.uv);
    ctx_->Unmap(suv.Get(), 0);
    return true;
}

bool Renderer::restorePicture(const CpuPicture& c, Picture& p) {
    p = {};
    if (!c.w || !c.h || !dev_ || !ctx_) return false;
    if (c.bgra) {
        if (!ensureBgra(p, c.w, c.h)) return false;
        ctx_->UpdateSubresource(p.y.Get(), 0, nullptr, c.y.data(), c.yRow, 0);
    } else {
        if (!ensureTextures(p, false, c.tenBit, c.w, c.h)) return false;
        ctx_->UpdateSubresource(p.y.Get(), 0, nullptr, c.y.data(), c.yRow, 0);
        ctx_->UpdateSubresource(p.uv.Get(), 0, nullptr, c.uv.data(), c.uvRow, 0);
    }
    p.fmt = c.fmt;
    return true;
}

void Renderer::restoreFrozen() {
    if (!std::exchange(frozenWasUp_, false) || !frozenOn_) return;
    if (restorePicture(frozenCpu_, frozen_)) {
        log("freeze: frozen picture restored on the new device");
        return;
    }
    // Re-freezing would show another picture under the old translation:
    // end the freeze and drop the boxes instead (the window tells the app).
    log("freeze: the frozen picture could not be restored after the device loss; freeze ended, text overlay cleared");
    frozenOn_ = false;
    frozen_ = {};
    frozenCpu_ = {};
    boxes_.clear();
    ovHits_ = {};
    ovValid_ = false;
    frozenDropped_ = true;
}

}  // namespace pm::video
