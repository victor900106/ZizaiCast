// Renderer: frame tap (CPU copies of presented pictures).
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
// Frame tap: crop + (10 -> 8 bit) on the GPU into R8 / R8G8 targets, copied
// into a ring of staging textures and mapped one picture later (the GPU has
// long finished by then: copyIn() waits for the next picture's decode).

bool Renderer::ensureTap(UINT w, UINT h) {
    if (tapY_ && tapW_ == w && tapH_ == h) return true;
    tapY_.Reset();
    tapUV_.Reset();
    tapYRtv_.Reset();
    tapUVRtv_.Reset();
    tapW_ = tapH_ = 0;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &tapY_)) ||
        FAILED(dev_->CreateRenderTargetView(tapY_.Get(), nullptr, &tapYRtv_)))
        return false;
    td.Width = w / 2;
    td.Height = h / 2;
    td.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &tapUV_)) ||
        FAILED(dev_->CreateRenderTargetView(tapUV_.Get(), nullptr, &tapUVRtv_)))
        return false;
    tapW_ = w;
    tapH_ = h;
    return true;
}

bool Renderer::tapSubmit(const Picture* pic, uint64_t ptsNs) {
    const Picture& p = pic ? *pic : cur_;
    if (!dev_ || !p.valid() || !psTapY_ || !psTapUV_ || !tapCb_ || (p.bgra && (!psTapRgbY_ || !psTapRgbUV_)))
        return false;
    const double t0 = clockMs();
    const UINT w = static_cast<UINT>(p.fmt.visibleWidth()) & ~1u, h = static_cast<UINT>(p.fmt.visibleHeight()) & ~1u;
    if (w < 2 || h < 2) return true;
    if (!ensureTap(w, h)) return false;
    // A free ring slot (the oldest pending one is dropped if none: only if
    // the caller never delivers).
    TapSlot* slot = nullptr;
    for (auto& t : tap_)
        if (!t.pending && (!slot || t.seq < slot->seq)) slot = &t;
    if (!slot) {
        for (auto& t : tap_)
            if (!slot || t.seq < slot->seq) slot = &t;
        slot->pending = false;
    }
    if (!slot->y || slot->w != w || slot->h != h) {
        slot->y.Reset();
        slot->uv.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &slot->y))) return false;
        td.Width = w / 2;
        td.Height = h / 2;
        td.Format = DXGI_FORMAT_R8G8_UNORM;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &slot->uv))) return false;
        slot->w = w;
        slot->h = h;
    }
    const int off[4] = {p.fmt.crop.left, p.fmt.crop.top, p.fmt.crop.left / 2, p.fmt.crop.top / 2};
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx_->Map(tapCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, off, sizeof(off));
        ctx_->Unmap(tapCb_.Get(), 0);
    }
    ID3D11ShaderResourceView* srvs[3] = {p.bgra ? nullptr : p.ySrv.Get(), p.uvSrv.Get(),
                                         p.bgra ? p.ySrv.Get() : nullptr};
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ctx_->PSSetConstantBuffers(1, 1, tapCb_.GetAddressOf());
    ctx_->PSSetShaderResources(0, 3, srvs);
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    ctx_->OMSetRenderTargets(1, tapYRtv_.GetAddressOf(), nullptr);
    ctx_->RSSetViewports(1, &vp);
    ctx_->PSSetShader(p.bgra ? psTapRgbY_.Get() : psTapY_.Get(), nullptr, 0);
    ctx_->Draw(4, 0);
    vp.Width = static_cast<float>(w / 2);
    vp.Height = static_cast<float>(h / 2);
    ctx_->OMSetRenderTargets(1, tapUVRtv_.GetAddressOf(), nullptr);
    ctx_->RSSetViewports(1, &vp);
    ctx_->PSSetShader(p.bgra ? psTapRgbUV_.Get() : psTapUV_.Get(), nullptr, 0);
    ctx_->Draw(4, 0);
    ID3D11ShaderResourceView* nulls[3] = {};
    ctx_->PSSetShaderResources(0, 3, nulls);
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    ctx_->CopyResource(slot->y.Get(), tapY_.Get());
    ctx_->CopyResource(slot->uv.Get(), tapUV_.Get());
    ctx_->Flush();
    slot->pending = true;
    slot->pts = ptsNs;
    slot->at = clockMs();
    slot->seq = ++tapSeq_;
    tapCost_ += slot->at - t0;
    return true;
}

void Renderer::tapDeliver(const TapFn& fn, bool flushAll) {
    while (true) {
        TapSlot* slot = nullptr;
        for (auto& t : tap_)
            if (t.pending && (!slot || t.seq < slot->seq)) slot = &t;
        if (!slot || (!flushAll && slot->seq == tapSeq_)) return;
        slot->pending = false;
        if (!fn || !ctx_) continue;
        const double t0 = clockMs();
        D3D11_MAPPED_SUBRESOURCE my{}, muv{};
        if (FAILED(ctx_->Map(slot->y.Get(), 0, D3D11_MAP_READ, 0, &my))) continue;
        if (FAILED(ctx_->Map(slot->uv.Get(), 0, D3D11_MAP_READ, 0, &muv))) {
            ctx_->Unmap(slot->y.Get(), 0);
            continue;
        }
        const UINT w = slot->w, h = slot->h;
        tapBuf_.resize(static_cast<size_t>(w) * h * 3 / 2);
        uint8_t* dst = tapBuf_.data();
        for (UINT y = 0; y < h; ++y, dst += w)
            std::memcpy(dst, static_cast<const uint8_t*>(my.pData) + static_cast<size_t>(y) * my.RowPitch, w);
        for (UINT y = 0; y < h / 2; ++y, dst += w)
            std::memcpy(dst, static_cast<const uint8_t*>(muv.pData) + static_cast<size_t>(y) * muv.RowPitch, w);
        ctx_->Unmap(slot->uv.Get(), 0);
        ctx_->Unmap(slot->y.Get(), 0);
        tapCost_ += clockMs() - t0;
        fn(tapBuf_.data(), static_cast<int>(w), static_cast<int>(h), static_cast<int>(w), slot->pts);
    }
}

double Renderer::tapFlushInMs() const {
    for (const auto& t : tap_)
        if (t.pending && t.seq == tapSeq_) return std::max(0.0, t.at + kTapFlushMs - clockMs());
    return -1;
}

void Renderer::tapReset() {
    for (auto& t : tap_) t.pending = false;
}

}  // namespace pm::video
