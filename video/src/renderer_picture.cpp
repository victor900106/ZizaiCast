// Renderer: decoded pictures: textures, upload, hold / show, drawing the picture, snapshots.
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

bool Renderer::ensureTextures(Picture& p, bool hw, bool tenBit, UINT w, UINT h) {
    if (p.hw == hw && p.tenBit == tenBit && p.w == w && p.h == h && p.ySrv) return true;
    p = {};
    p.hw = hw;
    p.tenBit = tenBit;
    const DXGI_FORMAT fY = tenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    const DXGI_FORMAT fUV = tenBit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    p.w = w;
    p.h = h;
    D3D11_TEXTURE2D_DESC td{};
    td.MipLevels = td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    HRESULT hr;
    if (hw) {
        td.Width = w;
        td.Height = h;
        td.Format = tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
        hr = dev_->CreateTexture2D(&td, nullptr, &p.nv12);
        if (SUCCEEDED(hr)) {
            sv.Format = fY;
            hr = dev_->CreateShaderResourceView(p.nv12.Get(), &sv, &p.ySrv);
        }
        if (SUCCEEDED(hr)) {
            sv.Format = fUV;
            hr = dev_->CreateShaderResourceView(p.nv12.Get(), &sv, &p.uvSrv);
        }
    } else {
        td.Width = w;
        td.Height = h;
        td.Format = fY;
        hr = dev_->CreateTexture2D(&td, nullptr, &p.y);
        if (SUCCEEDED(hr)) hr = dev_->CreateShaderResourceView(p.y.Get(), nullptr, &p.ySrv);
        td.Width = (w + 1) / 2;
        td.Height = (h + 1) / 2;
        td.Format = fUV;
        if (SUCCEEDED(hr)) hr = dev_->CreateTexture2D(&td, nullptr, &p.uv);
        if (SUCCEEDED(hr)) hr = dev_->CreateShaderResourceView(p.uv.Get(), nullptr, &p.uvSrv);
    }
    if (FAILED(hr)) {
        log("texture allocation %ux%u (%s%s) failed hr=0x%08lx", w, h, hw ? "NV12" : "R8/R8G8", tenBit ? ", 10-bit" : "", hr);
        p = {};
        return false;
    }
    return true;
}

bool Renderer::ensureBgra(Picture& p, UINT w, UINT h) {
    if (p.bgra && p.w == w && p.h == h && p.ySrv) return true;
    p = {};
    p.bgra = true;
    p.w = w;
    p.h = h;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = dev_->CreateTexture2D(&td, nullptr, &p.y);
    if (SUCCEEDED(hr)) hr = dev_->CreateShaderResourceView(p.y.Get(), nullptr, &p.ySrv);
    if (FAILED(hr)) {
        log("texture allocation %ux%u (BGRA) failed hr=0x%08lx", w, h, hr);
        p = {};
        return false;
    }
    return true;
}

bool Renderer::uploadBgra(const uint8_t* bgra, UINT w, UINT h, UINT stride) {
    if (!dev_ || !psBgra_ || !ensureBgra(cur_, w, h)) return false;
    // DEFAULT texture + UpdateSubresource: the runtime copies the rows into
    // its upload heap right away (one memcpy), the GPU copy is queued.
    ctx_->UpdateSubresource(cur_.y.Get(), 0, nullptr, bgra, stride, 0);
    VideoFormat f;
    f.codedWidth = w;
    f.codedHeight = h;
    f.crop = {0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    f.fullRange = true;
    f.stride = static_cast<LONG>(stride);
    cur_.fmt = f;
    pictureShown();
    return true;
}

bool Renderer::copyIn(IMFSample* sample, const VideoFormat& fmt, Picture& p) {
    if (!dev_) return false;
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(sample->GetBufferByIndex(0, &buf))) return false;

    ComPtr<IMFDXGIBuffer> dxgiBuf;
    if (SUCCEEDED(buf.As(&dxgiBuf))) {
        ComPtr<ID3D11Texture2D> src;
        UINT sub = 0;
        if (FAILED(dxgiBuf->GetResource(IID_PPV_ARGS(&src))) || FAILED(dxgiBuf->GetSubresourceIndex(&sub)))
            return false;
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        if (!ensureTextures(p, true, sd.Format == DXGI_FORMAT_P010, sd.Width, sd.Height)) return false;
        ctx_->CopySubresourceRegion(p.nv12.Get(), 0, 0, 0, 0, src.Get(), sub, nullptr);
    } else {
        if (!ensureTextures(p, false, fmt.tenBit, fmt.codedWidth, fmt.codedHeight)) return false;
        BYTE* data = nullptr;
        LONG pitch = 0;
        ComPtr<IMF2DBuffer> buf2d;
        bool locked2d = SUCCEEDED(buf.As(&buf2d)) && SUCCEEDED(buf2d->Lock2D(&data, &pitch));
        DWORD maxLen = 0, curLen = 0;
        if (!locked2d) {
            if (FAILED(buf->Lock(&data, &maxLen, &curLen))) return false;
            pitch = std::abs(fmt.stride);
        }
        if (pitch > 0) {
            ctx_->UpdateSubresource(p.y.Get(), 0, nullptr, data, pitch, 0);
            ctx_->UpdateSubresource(p.uv.Get(), 0, nullptr, data + static_cast<size_t>(pitch) * fmt.codedHeight,
                                    pitch, 0);
        }
        if (locked2d) buf2d->Unlock2D();
        else buf->Unlock();
    }
    // Wait for the GPU (DXVA decode + copy) so the caller's latency numbers
    // reflect a picture that is really ready; costs nothing in steady state
    // because the next Present would wait for it anyway.  (After device
    // removal GetData fails instead of returning S_FALSE.)
    if (evt_) {
        ctx_->End(evt_.Get());
        // Bounded: a GPU that never finishes (hang without a TDR) must not
        // freeze the render thread; the watchdog sees no picture then.
        const double t0 = clockMs();
        while (ctx_->GetData(evt_.Get(), nullptr, 0, 0) == S_FALSE) {
            if (clockMs() - t0 > kGpuWaitMaxMs) {
                if (gpuWaitTimeouts_++ % 50 == 0)
                    log("GPU did not finish a picture copy within %.0f ms (%lld times)", kGpuWaitMaxMs, gpuWaitTimeouts_);
                return false;
            }
            SwitchToThread();
        }
    }
    p.fmt = fmt;
    return true;
}

void Renderer::pictureShown() {
    havePicture_ = true;
    lastPictureAt_ = clockMs();
    if (dimmed_) setDimmed(false);  // the stream is back: no longer "on hold"
    if (scene_ != Scene::Live) {
        const double now = clockMs();
        // Reconnect during a fade-out: fade in from the idle screen.
        fadeFrom_ = scene_;
        scene_ = Scene::Live;
        fadeInAt_ = now;
        fadeOutAt_ = -1e9;
    }
}

bool Renderer::upload(IMFSample* sample, const VideoFormat& fmt) {
    if (!copyIn(sample, fmt, cur_)) return false;
    pictureShown();
    return true;
}

bool Renderer::hold(IMFSample* sample, const VideoFormat& fmt, Picture& out) {
    out = {};
    if (!pool_.empty()) {
        out = std::move(pool_.back());
        pool_.pop_back();
    }
    if (copyIn(sample, fmt, out)) return true;
    out = {};
    return false;
}

void Renderer::show(Picture&& pic) {
    if (!pic.valid()) return;
    std::swap(cur_, pic);
    recycle(std::move(pic));
    pictureShown();
}

void Renderer::recycle(Picture&& pic) {
    if (!pic.valid() || pool_.size() >= kMaxPool) return;
    // Only keep textures that match the current picture (size / hw-sw kind).
    if (pic.bgra || (cur_.valid() && (pic.w != cur_.w || pic.h != cur_.h || pic.hw != cur_.hw ||
                                      pic.tenBit != cur_.tenBit || cur_.bgra)))
        return;
    pool_.push_back(std::move(pic));
}

void Renderer::reset() {
    const double now = clockMs();
    // The stream ended: nothing to freeze or translate any more (zoom and
    // filter are the user's viewing preference: they stay).
    frozenOn_ = false;
    frozen_ = {};
    frozenCpu_ = {};
    frozenWasUp_ = false;
    boxes_.clear();
    ovHits_ = {};
    ovValid_ = false;
    busy_.clear();
    selecting_ = false;
    if (scene_ == Scene::Live && havePicture_ && !paused_) fadeOutAt_ = now;
    scene_ = Scene::Idle;
    paused_ = false;
    sceneAt_ = now;
    deviceName_.clear();
    connectIntro_ = false;
    poke();
}

void Renderer::drawPicture(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp, const Picture& pic,
                           const View& view, bool mirror) {
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    ctx_->RSSetViewports(1, &vp);

    Constants c{};
    c.uvRect[0] = pic.fmt.crop.left / static_cast<float>(pic.w);
    c.uvRect[1] = pic.fmt.crop.top / static_cast<float>(pic.h);
    c.uvRect[2] = pic.fmt.crop.right / static_cast<float>(pic.w);
    c.uvRect[3] = pic.fmt.crop.bottom / static_cast<float>(pic.h);
    if (pic.fmt.fullRange) {
        c.range[0] = 0; c.range[1] = 1; c.range[2] = 1;
    } else {
        c.range[0] = 16.f / 255; c.range[1] = 255.f / 219; c.range[2] = 255.f / 224;
    }
    if (pic.fmt.bt601) {
        c.mat[0] = 1.402f; c.mat[1] = -0.344136f; c.mat[2] = -0.714136f; c.mat[3] = 1.772f;
    } else {
        c.mat[0] = 1.5748f; c.mat[1] = -0.187324f; c.mat[2] = -0.468124f; c.mat[3] = 1.8556f;
    }
    pictureTransform(rot_, mirror, c.xfU, c.xfV);
    const float inv = 1.f / std::max(1.f, view.zoom);
    c.view[0] = inv;
    c.view[1] = view.cx - 0.5f * inv;
    c.view[2] = view.cy - 0.5f * inv;
    c.view[3] = static_cast<float>(view.filter);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, &c, sizeof(c));
        ctx_->Unmap(cb_.Get(), 0);
    }
    // NV12 / P010: t0 = Y, t1 = UV; BGRA: t2.
    ID3D11ShaderResourceView* srvs[3] = {pic.bgra ? nullptr : pic.ySrv.Get(), pic.uvSrv.Get(),
                                         pic.bgra ? pic.ySrv.Get() : nullptr};
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ctx_->PSSetShader(pic.bgra ? psBgra_.Get() : ps_.Get(), nullptr, 0);
    ctx_->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ctx_->PSSetShaderResources(0, 3, srvs);
    ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    ctx_->Draw(4, 0);
    ID3D11ShaderResourceView* nulls[3] = {};
    ctx_->PSSetShaderResources(0, 3, nulls);
}

bool Renderer::snapshot(std::vector<uint8_t>& out, UINT& w, UINT& h, bool framed) {
    return renderPicture(out, w, h, framed, mirror_);
}

bool Renderer::grab(std::vector<uint8_t>& out, UINT& w, UINT& h, bool mirror) {
    const bool ok = renderPicture(out, w, h, false, mirror);
    if (ok) liveNoteGrab();  // 即時翻譯: boxes recognised on this picture follow its scrolling (live_overlay.cpp)
    return ok;
}

// The shown picture, unzoomed and unfiltered (snapshots, OCR).
bool Renderer::renderPicture(std::vector<uint8_t>& out, UINT& w, UINT& h, bool framed, bool mirror) {
    if (!(havePicture_ || (frozenOn_ && frozen_.valid())) || !shown().valid()) return false;
    int dw = 0, dh = 0;
    displaySize(dw, dh);
    if (dw <= 0 || dh <= 0) return false;
    FrameGeom g{};
    float o = 0;
    if (framed) {
        g = frameFor(static_cast<float>(dw), static_cast<float>(dh), 0, 0, 0, 0, false);
        o = std::ceil(g.outset()) + 1;
        g.screen = {o, o, o + dw, o + dh};
    }
    w = static_cast<UINT>(dw + 2 * o);
    h = static_cast<UINT>(dh + 2 * o);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> tex, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &tex)) ||
        FAILED(dev_->CreateRenderTargetView(tex.Get(), nullptr, &rtv)))
        return false;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &staging))) return false;
    const float clear[4] = {0, 0, 0, 0};
    ctx_->ClearRenderTargetView(rtv.Get(), clear);
    D3D11_VIEWPORT vp{o, o, static_cast<float>(dw), static_cast<float>(dh), 0, 1};
    drawPicture(rtv.Get(), vp, shown(), View{}, mirror);  // same shader as on screen: identical colours
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    if (framed && d2d_) {
        ComPtr<IDXGISurface> surf;
        ComPtr<ID2D1RenderTarget> rt;
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        if (FAILED(tex.As(&surf)) || FAILED(d2d_->CreateDxgiSurfaceRenderTarget(surf.Get(), &props, &rt))) return false;
        rt->BeginDraw();
        drawDeviceFrame(rt.Get(), g, 1.f);
        if (FAILED(rt->EndDraw())) return false;
    }
    ctx_->CopyResource(staging.Get(), tex.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    out.resize(static_cast<size_t>(w) * h * 4);
    for (UINT y = 0; y < h; ++y)
        std::memcpy(out.data() + static_cast<size_t>(y) * w * 4, static_cast<const uint8_t*>(m.pData) + y * m.RowPitch,
                    static_cast<size_t>(w) * 4);
    ctx_->Unmap(staging.Get(), 0);
    if (framed) {  // premultiplied -> straight alpha for the PNG
        for (size_t i = 0; i < out.size(); i += 4) {
            const uint8_t a = out[i + 3];
            if (a == 0 || a == 255) continue;
            for (int c = 0; c < 3; ++c) out[i + c] = static_cast<uint8_t>(std::min(255, (out[i + c] * 255 + a / 2) / a));
        }
    }
    return true;
}

bool Renderer::renderCapture(std::vector<uint8_t>& bgrx, UINT& w, UINT& h) {
    if (!swap_ || !width_ || !height_) return false;
    capture_ = &bgrx;
    captureW_ = captureH_ = 0;
    const bool ok = render();
    capture_ = nullptr;
    w = captureW_;
    h = captureH_;
    return ok && w && h;
}

void Renderer::copyBackBuffer() {
    ComPtr<ID3D11Texture2D> back, staging;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    D3D11_TEXTURE2D_DESC td{};
    back->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &staging))) return;
    ctx_->CopyResource(staging.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
    capture_->resize(static_cast<size_t>(td.Width) * td.Height * 4);
    for (UINT y = 0; y < td.Height; ++y)
        std::memcpy(capture_->data() + static_cast<size_t>(y) * td.Width * 4,
                    static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, td.Width * 4);
    ctx_->Unmap(staging.Get(), 0);
    captureW_ = td.Width;
    captureH_ = td.Height;
}

}  // namespace pm::video
