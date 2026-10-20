#include "capture.h"

#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "util.h"

namespace pm::remote {

namespace {

// Full-texture-space quad from SV_VertexID (triangle strip, 4 vertices);
// rect = left, top, right, bottom in clip space.
const char kShader[] = R"(
cbuffer C : register(b0) { float4 rect; };
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VO vs(uint id : SV_VertexID) {
    float2 t = float2((id << 1) & 2, id & 2) * 0.5;
    VO o;
    o.pos = float4(lerp(rect.x, rect.z, t.x), lerp(rect.y, rect.w, t.y), 0, 1);
    o.uv = t;
    return o;
}
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 ps(VO i) : SV_Target { return tex.Sample(smp, i.uv); }
)";

ComPtr<ID3DBlob> compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> code, err;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "remote_pointer", nullptr, nullptr, entry, target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
        logf("capture: shader %s: %s", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return nullptr;
    }
    return code;
}

// Capture-time stamp layout (must match web/index.html readStamp): one row of
// B x B blocks in the stream's top-left corner: white, black, 32 bits of the
// epoch ms (MSB first), 8-bit checksum ((sum of the 4 bytes) & 0xFF) ^ 0x5A.
constexpr int kStampBlocks = 42;

}  // namespace

int stampBlockSize(int h) { return std::max(8, h / 60); }

static void stampBits(uint32_t ms, bool bits[kStampBlocks]) {
    bits[0] = true, bits[1] = false;
    for (int i = 0; i < 32; ++i) bits[2 + i] = (ms >> (31 - i)) & 1;
    const uint8_t ck =
        uint8_t((((ms >> 24) & 0xFF) + ((ms >> 16) & 0xFF) + ((ms >> 8) & 0xFF) + (ms & 0xFF)) & 0xFF) ^ 0x5A;
    for (int i = 0; i < 8; ++i) bits[34 + i] = (ck >> (7 - i)) & 1;
}

// CPU version (readback path when the driver cannot ClearView NV12 planes).
void drawStampCpu(std::vector<uint8_t>& nv12, int w, int h, uint32_t ms) {
    const int B = stampBlockSize(h);
    if (B * kStampBlocks > w) return;
    bool bits[kStampBlocks];
    stampBits(ms, bits);
    for (int y = 0; y < B; ++y) {
        uint8_t* row = &nv12[size_t(y) * size_t(w)];
        for (int i = 0; i < kStampBlocks; ++i) memset(row + i * B, bits[i] ? 235 : 16, size_t(B));
    }
    uint8_t* uv = &nv12[size_t(w) * size_t(h)];
    for (int y = 0; y < B / 2; ++y) memset(uv + size_t(y) * size_t(w), 128, size_t(B * kStampBlocks));
}

std::vector<MonitorDesc> enumMonitors() {
    std::vector<MonitorDesc> v;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(m, &mi)) {
                MonitorDesc d;
                d.device = mi.szDevice;
                d.rect = mi.rcMonitor;
                d.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
                reinterpret_cast<std::vector<MonitorDesc>*>(lp)->push_back(d);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&v));
    std::stable_sort(v.begin(), v.end(), [](const MonitorDesc& a, const MonitorDesc& b) {
        if (a.primary != b.primary) return a.primary;
        if (a.rect.left != b.rect.left) return a.rect.left < b.rect.left;
        return a.rect.top < b.rect.top;
    });
    return v;
}

DesktopCapture::~DesktopCapture() { close(); }

void DesktopCapture::close() {
    pool_.clear();
    vpIn_.Reset(), vp_.Reset(), vpEnum_.Reset(), vctx_.Reset(), vdev_.Reset();
    staging_.Reset(), compRtv_.Reset(), comp_.Reset(), desk_.Reset();
    ptrColor_.Reset(), ptrInvert_.Reset();
    vs_.Reset(), ps_.Reset(), cb_.Reset(), smp_.Reset(), blendAlpha_.Reset(), blendInvert_.Reset(), rs_.Reset();
    dupl_.Reset(), output_.Reset(), mt_.Reset(), ctx1_.Reset(), ctx_.Reset(), dev_.Reset();
    deskW_ = deskH_ = outW_ = outH_ = 0;
    frozen_ = lost_ = false;
    needCompose_ = changed_ = true;
}

int DesktopCapture::busySlots() const {
    int n = 0;
    for (const auto& s : pool_) n += s->busy ? 1 : 0;
    return n;
}

bool DesktopCapture::open(const Options& o) {
    close();
    opt_ = o;
    // The chosen monitor's output (enumMonitors() order), on the adapter that
    // drives it (Desktop Duplication needs the device on that adapter; iGPU +
    // dGPU machines have several). The encoder is later picked on the same
    // adapter (LUID).
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return logf("capture: CreateDXGIFactory1 failed"), false;
    const std::vector<MonitorDesc> mons = enumMonitors();
    monIndex_ = o.monitor >= 0 && o.monitor < int(mons.size()) ? o.monitor : 0;
    const std::wstring want = mons.empty() ? std::wstring() : mons[size_t(monIndex_)].device;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; !output_; ++a) {
        ComPtr<IDXGIAdapter1> ad;
        if (factory->EnumAdapters1(a, &ad) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIOutput> out;
            if (ad->EnumOutputs(i, &out) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC d;
            out->GetDesc(&d);
            MONITORINFO mi{sizeof(mi)};
            if (!d.AttachedToDesktop || !GetMonitorInfoW(d.Monitor, &mi)) continue;
            if (want.empty() ? (mi.dwFlags & MONITORINFOF_PRIMARY) != 0 : want == d.DeviceName) {
                out.As(&output_);
                adapter = ad;
                rect_ = d.DesktopCoordinates;
                break;
            }
        }
    }
    if (!output_) return logf("capture: monitor %d output not found", o.monitor), false;
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    adapter_ = narrow(ad.Description);
    luid_ = ad.AdapterLuid;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 2,
                                   D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
    if (FAILED(hr)) return logf("capture: D3D11CreateDevice failed %s", hresultText(hr).c_str()), false;
    ctx_.As(&ctx1_);
    // The encoder MFT shares this device: serialize our multi-call sequences.
    if (SUCCEEDED(ctx_.As(&mt_))) mt_->SetMultithreadProtected(TRUE);
    if (!createShaders()) return false;
    if (!createDuplication()) return false;
    if (!createPipeline()) return false;
    logf("capture: monitor %d at %ld,%ld, %s, desktop %dx%d -> stream %dx%d, %d pool textures, stamp %s, scene %s",
         monIndex_, rect_.left, rect_.top, adapter_.c_str(), deskW_, deskH_, outW_, outH_, int(pool_.size()), stampGpu_ ? "on GPU (ClearView)" : "CPU fallback",
         opt_.scene == Scene::Static ? "static" : opt_.scene == Scene::Motion ? "motion" : "desktop");
    return true;
}

bool DesktopCapture::createShaders() {
    auto vsb = compile("vs", "vs_4_0"), psb = compile("ps", "ps_4_0");
    if (!vsb || !psb) return false;
    if (FAILED(dev_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_)) ||
        FAILED(dev_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_)))
        return logf("capture: shader creation failed"), false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16, bd.Usage = D3D11_USAGE_DEFAULT, bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev_->CreateBuffer(&bd, nullptr, &cb_);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev_->CreateSamplerState(&sd, &smp_);
    D3D11_BLEND_DESC b{};
    auto& rt = b.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA, rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA, rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE, rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA, rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev_->CreateBlendState(&b, &blendAlpha_);
    // XOR part of monochrome / masked pointers: out = src*(1-dst) + dst*(1-src)
    // = invert where src is white, unchanged where src is black.
    rt.SrcBlend = D3D11_BLEND_INV_DEST_COLOR, rt.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
    rt.SrcBlendAlpha = D3D11_BLEND_ZERO, rt.DestBlendAlpha = D3D11_BLEND_ONE;
    dev_->CreateBlendState(&b, &blendInvert_);
    D3D11_RASTERIZER_DESC r{};
    r.FillMode = D3D11_FILL_SOLID, r.CullMode = D3D11_CULL_NONE;
    dev_->CreateRasterizerState(&r, &rs_);
    return true;
}

bool DesktopCapture::createDuplication() {
    dupl_.Reset();
    HRESULT hr = output_->DuplicateOutput(dev_.Get(), &dupl_);
    if (FAILED(hr)) return logf("capture: DuplicateOutput failed %s", hresultText(hr).c_str()), false;
    DXGI_OUTDUPL_DESC dd;
    dupl_->GetDesc(&dd);
    if (dd.Rotation != DXGI_MODE_ROTATION_IDENTITY && dd.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED)
        logf("capture: note: rotated monitor (%d) is not handled by the spike", int(dd.Rotation));
    const int w = int(dd.ModeDesc.Width), h = int(dd.ModeDesc.Height);
    if (w != deskW_ || h != deskH_) {
        deskW_ = w, deskH_ = h;
        desk_.Reset(), comp_.Reset(), compRtv_.Reset(), vpIn_.Reset(), vp_.Reset(), vpEnum_.Reset();
        pool_.clear();
        staging_.Reset();
    }
    if (!desk_) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(w), td.Height = UINT(h), td.MipLevels = 1, td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM, td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &desk_)) || FAILED(dev_->CreateTexture2D(&td, nullptr, &comp_)) ||
            FAILED(dev_->CreateRenderTargetView(comp_.Get(), nullptr, &compRtv_)))
            return logf("capture: desktop textures failed"), false;
        ComPtr<ID3D11RenderTargetView> rtv;
        if (SUCCEEDED(dev_->CreateRenderTargetView(desk_.Get(), nullptr, &rtv))) {
            const float black[4] = {0, 0, 0, 1};
            ctx_->ClearRenderTargetView(rtv.Get(), black);  // until the first image arrives
        }
        needCompose_ = changed_ = true;
    }
    lost_ = false;
    return true;
}

bool DesktopCapture::createPipeline() {
    if (vp_) return true;
    const double sc = std::min({1.0, double(opt_.maxWidth) / deskW_, double(opt_.maxHeight) / deskH_});
    outW_ = std::max(16, int(deskW_ * sc) & ~1);
    outH_ = std::max(16, int(deskH_ * sc) & ~1);
    if (FAILED(dev_.As(&vdev_)) || FAILED(ctx_.As(&vctx_))) return logf("capture: no D3D11 video device"), false;
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = UINT(deskW_), cd.InputHeight = UINT(deskH_);
    cd.OutputWidth = UINT(outW_), cd.OutputHeight = UINT(outH_);
    cd.InputFrameRate = {60, 1}, cd.OutputFrameRate = {60, 1};
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    HRESULT hr = vdev_->CreateVideoProcessorEnumerator(&cd, &vpEnum_);
    if (FAILED(hr)) return logf("capture: CreateVideoProcessorEnumerator %s", hresultText(hr).c_str()), false;
    hr = vdev_->CreateVideoProcessor(vpEnum_.Get(), 0, &vp_);
    if (FAILED(hr)) return logf("capture: CreateVideoProcessor %s", hresultText(hr).c_str()), false;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
    iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    hr = vdev_->CreateVideoProcessorInputView(comp_.Get(), vpEnum_.Get(), &iv, &vpIn_);
    if (FAILED(hr)) return logf("capture: input view %s", hresultText(hr).c_str()), false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = UINT(outW_), td.Height = UINT(outH_), td.MipLevels = 1, td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12, td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    stampGpu_ = ctx1_ != nullptr;
    pool_.clear();
    for (int i = 0; i < std::max(2, opt_.poolSize); ++i) {
        auto s = std::make_unique<Slot>();
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &s->tex))) {
            td.BindFlags = D3D11_BIND_RENDER_TARGET;  // some drivers refuse SRV on NV12
            if (FAILED(dev_->CreateTexture2D(&td, nullptr, &s->tex))) return logf("capture: NV12 texture failed"), false;
        }
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
        ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        hr = vdev_->CreateVideoProcessorOutputView(s->tex.Get(), vpEnum_.Get(), &ov, &s->vpOut);
        if (FAILED(hr)) return logf("capture: output view %s", hresultText(hr).c_str()), false;
        if (stampGpu_) {  // plane views for the stamp: R8 = luma, R8G8 = chroma
            D3D11_RENDER_TARGET_VIEW_DESC rd{};
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            rd.Format = DXGI_FORMAT_R8_UNORM;
            const bool a = SUCCEEDED(dev_->CreateRenderTargetView(s->tex.Get(), &rd, &s->lumaRtv));
            rd.Format = DXGI_FORMAT_R8G8_UNORM;
            const bool b = SUCCEEDED(dev_->CreateRenderTargetView(s->tex.Get(), &rd, &s->chromaRtv));
            if (!a || !b) stampGpu_ = false;
        }
        pool_.push_back(std::move(s));
    }
    td.BindFlags = 0, td.Usage = D3D11_USAGE_STAGING, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &staging_))) return logf("capture: staging texture failed"), false;

    RECT dst{0, 0, outW_, outH_};
    vctx_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &dst);
    vctx_->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE, &dst);
    vctx_->VideoProcessorSetStreamFrameFormat(vp_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vctx_->VideoProcessorSetStreamAutoProcessingMode(vp_.Get(), 0, FALSE);
    ComPtr<ID3D11VideoContext1> vctx1;
    if (SUCCEEDED(vctx_.As(&vctx1))) {
        vctx1->VideoProcessorSetStreamColorSpace1(vp_.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        vctx1->VideoProcessorSetOutputColorSpace1(vp_.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{}, out{};
        in.RGB_Range = 0;
        out.YCbCr_Matrix = 1, out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        vctx_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &in);
        vctx_->VideoProcessorSetOutputColorSpace(vp_.Get(), &out);
    }
    return true;
}

void DesktopCapture::updatePointerShape(const DXGI_OUTDUPL_FRAME_INFO& fi) {
    if (fi.PointerShapeBufferSize == 0) return;
    shapeBuf_.resize(fi.PointerShapeBufferSize);
    DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
    UINT req = 0;
    if (FAILED(dupl_->GetFramePointerShape(UINT(shapeBuf_.size()), shapeBuf_.data(), &req, &si))) return;
    const int w = int(si.Width);
    const bool mono = si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
    const int h = mono ? int(si.Height) / 2 : int(si.Height);
    if (w <= 0 || h <= 0 || w > 512 || h > 512) return;
    std::vector<uint32_t> color(size_t(w) * size_t(h), 0), inv(size_t(w) * size_t(h), 0);
    bool anyInv = false;
    const uint8_t* b = shapeBuf_.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t& c = color[size_t(y) * size_t(w) + size_t(x)];
            uint32_t& v = inv[size_t(y) * size_t(w) + size_t(x)];
            if (mono) {
                const int andBit = (b[size_t(y) * si.Pitch + size_t(x / 8)] >> (7 - x % 8)) & 1;
                const int xorBit = (b[size_t(y + h) * si.Pitch + size_t(x / 8)] >> (7 - x % 8)) & 1;
                if (!andBit) c = xorBit ? 0xFFFFFFFFu : 0xFF000000u;
                else if (xorBit) v = 0xFFFFFFFFu, anyInv = true;
            } else {
                const uint32_t px = *reinterpret_cast<const uint32_t*>(b + size_t(y) * si.Pitch + size_t(x) * 4);
                if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) c = px;
                else if ((px >> 24) == 0) c = px | 0xFF000000u;                       // masked: replace
                else if (px & 0xFFFFFFu) v = 0xFFFFFFFFu, anyInv = true;              // masked: XOR
            }
        }
    auto makeSrv = [&](const std::vector<uint32_t>& px, ComPtr<ID3D11ShaderResourceView>& srv) {
        srv.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(w), td.Height = UINT(h), td.MipLevels = 1, td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM, td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{px.data(), UINT(w * 4), 0};
        ComPtr<ID3D11Texture2D> t;
        if (SUCCEEDED(dev_->CreateTexture2D(&td, &sd, &t))) dev_->CreateShaderResourceView(t.Get(), nullptr, &srv);
    };
    makeSrv(color, ptrColor_);
    if (anyInv) makeSrv(inv, ptrInvert_);
    else ptrInvert_.Reset();
    ptrW_ = w, ptrH_ = h, ptrHotX_ = int(si.HotSpot.x), ptrHotY_ = int(si.HotSpot.y);
}

bool DesktopCapture::poll(int timeoutMs, bool& changed) {
    changed = false;
    if (lost_ || !dupl_) {
        // Secure desktop (UAC), lock screen, mode change: retry quietly.
        if (!createDuplication()) {
            Sleep(DWORD(std::max(timeoutMs, 1)));
            return true;
        }
        if (!createPipeline()) return false;
        logf("capture: duplication re-created (%dx%d)", deskW_, deskH_);
    }
    DXGI_OUTDUPL_FRAME_INFO fi{};
    ComPtr<IDXGIResource> res;
    // AcquireNextFrame(timeout) keeps the device lock while it waits, which
    // stalls the encoder MFT sharing this device (measured: encode 1.5 ->
    // 18 ms). So: zero-timeout polls, sleeping 1 ms in between, unlocked.
    HRESULT hr;
    const int64_t until = nowUs() + int64_t(std::max(timeoutMs, 0)) * 1000;
    for (;;) {
        hr = dupl_->AcquireNextFrame(0, &fi, &res);
        if (hr != DXGI_ERROR_WAIT_TIMEOUT || nowUs() >= until) break;
        Sleep(1);
    }
    auto pending = [&] { return opt_.scene == Scene::Motion || (changed_ && (!frozen_ || frameNo_ == 0)); };
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        changed = pending();
        return true;
    }
    if (FAILED(hr)) {
        logf("capture: AcquireNextFrame %s, re-creating", hresultText(hr).c_str());
        dupl_.Reset();
        lost_ = true;
        return true;
    }
    if (!frozen_) {
        if (fi.LastPresentTime.QuadPart != 0) {
            ComPtr<ID3D11Texture2D> tex;
            if (SUCCEEDED(res.As(&tex))) {
                ctx_->CopyResource(desk_.Get(), tex.Get());
                needCompose_ = changed_ = true;
                // Test scenes freeze the first real image.
                if (opt_.scene != Scene::Desktop) frozen_ = true;
            }
        }
        if (opt_.drawCursor && opt_.scene == Scene::Desktop && fi.LastMouseUpdateTime.QuadPart != 0) {
            const bool vis = fi.PointerPosition.Visible != 0;
            const int x = fi.PointerPosition.Position.x, y = fi.PointerPosition.Position.y;
            if (vis != ptrVisible_ || (vis && (x != ptrX_ || y != ptrY_)) || fi.PointerShapeBufferSize) {
                ptrVisible_ = vis, ptrX_ = x, ptrY_ = y;
                needCompose_ = changed_ = true;
            }
            updatePointerShape(fi);
        }
    }
    dupl_->ReleaseFrame();
    changed = pending();
    return true;
}

void DesktopCapture::compose() {
    ctx_->CopyResource(comp_.Get(), desk_.Get());
    if (!opt_.drawCursor || !ptrVisible_ || !ptrColor_ || opt_.scene != Scene::Desktop) return;
    const float l = float(ptrX_) / deskW_ * 2 - 1, t = 1 - float(ptrY_) / deskH_ * 2;
    const float r = float(ptrX_ + ptrW_) / deskW_ * 2 - 1, bt = 1 - float(ptrY_ + ptrH_) / deskH_ * 2;
    const float rect[4] = {l, t, r, bt};
    (void)ptrHotX_, (void)ptrHotY_;  // DXGI positions are already the shape's top-left corner
    ctx_->UpdateSubresource(cb_.Get(), 0, nullptr, rect, 0, 0);
    D3D11_VIEWPORT vp{0, 0, float(deskW_), float(deskH_), 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetState(rs_.Get());
    ctx_->OMSetRenderTargets(1, compRtv_.GetAddressOf(), nullptr);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ctx_->PSSetSamplers(0, 1, smp_.GetAddressOf());
    const float bf[4] = {0, 0, 0, 0};
    ctx_->OMSetBlendState(blendAlpha_.Get(), bf, 0xFFFFFFFF);
    ctx_->PSSetShaderResources(0, 1, ptrColor_.GetAddressOf());
    ctx_->Draw(4, 0);
    if (ptrInvert_) {
        ctx_->OMSetBlendState(blendInvert_.Get(), bf, 0xFFFFFFFF);
        ctx_->PSSetShaderResources(0, 1, ptrInvert_.GetAddressOf());
        ctx_->Draw(4, 0);
    }
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx_->PSSetShaderResources(0, 1, &nullSrv);
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
}

int DesktopCapture::render(bool stamp, uint32_t stampMs) {
    if (!vp_) return -1;
    int slot = -1;
    for (size_t i = 0; i < pool_.size(); ++i) {
        bool expected = false;
        if (pool_[i]->busy.compare_exchange_strong(expected, true)) {
            slot = int(i);
            break;
        }
    }
    if (slot < 0) return -1;
    Slot& s = *pool_[size_t(slot)];
    if (mt_) mt_->Enter();
    if (needCompose_) {
        compose();
        needCompose_ = false;
    }
    // Source rect: whole desktop, or a panning 80% window in the Motion scene.
    RECT src{0, 0, deskW_, deskH_};
    if (opt_.scene == Scene::Motion) {
        const double ph = 0.5 + 0.5 * std::sin(double(frameNo_) * 0.05);
        const int ww = deskW_ * 8 / 10 & ~1, wh = deskH_ * 8 / 10 & ~1;
        const int x = int((deskW_ - ww) * ph) & ~1, y = int((deskH_ - wh) * (1 - ph)) & ~1;
        src = {x, y, x + ww, y + wh};
    }
    vctx_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &src);
    D3D11_VIDEO_PROCESSOR_STREAM st{};
    st.Enable = TRUE;
    st.pInputSurface = vpIn_.Get();
    HRESULT hr = vctx_->VideoProcessorBlt(vp_.Get(), s.vpOut.Get(), 0, 1, &st);
    if (SUCCEEDED(hr) && stamp && stampGpu_) {
        const int B = stampBlockSize(outH_);
        if (B * kStampBlocks <= outW_) {
            bool bits[kStampBlocks];
            stampBits(stampMs, bits);
            D3D11_RECT white[kStampBlocks], black[kStampBlocks];
            UINT nw = 0, nb = 0;
            for (int i = 0; i < kStampBlocks; ++i) (bits[i] ? white[nw++] : black[nb++]) = {i * B, 0, (i + 1) * B, B};
            const float cw[4] = {235 / 255.f, 0, 0, 0}, ck[4] = {16 / 255.f, 0, 0, 0}, cc[4] = {128 / 255.f, 128 / 255.f, 0, 0};
            ctx1_->ClearView(s.lumaRtv.Get(), cw, white, nw);
            ctx1_->ClearView(s.lumaRtv.Get(), ck, black, nb);
            D3D11_RECT c{0, 0, B * kStampBlocks / 2, B / 2};
            ctx1_->ClearView(s.chromaRtv.Get(), cc, &c, 1);
        }
    }
    ctx_->Flush();
    if (mt_) mt_->Leave();
    if (FAILED(hr)) {
        logf("capture: VideoProcessorBlt %s", hresultText(hr).c_str());
        s.busy = false;
        return -1;
    }
    ++frameNo_;
    changed_ = false;
    return slot;
}

bool DesktopCapture::readback(int slot, std::vector<uint8_t>& out) {
    if (mt_) mt_->Enter();
    ctx_->CopyResource(staging_.Get(), pool_[size_t(slot)]->tex.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) {
        if (mt_) mt_->Leave();
        return logf("capture: Map %s", hresultText(hr).c_str()), false;
    }
    const size_t w = size_t(outW_), h = size_t(outH_);
    out.resize(w * h * 3 / 2);
    const auto* src = static_cast<const uint8_t*>(m.pData);
    for (size_t y = 0; y < h; ++y) memcpy(&out[y * w], src + y * m.RowPitch, w);
    const uint8_t* uv = src + size_t(m.RowPitch) * h;
    for (size_t y = 0; y < h / 2; ++y) memcpy(&out[w * h + y * w], uv + y * m.RowPitch, w);
    ctx_->Unmap(staging_.Get(), 0);
    if (mt_) mt_->Leave();
    return true;
}

}  // namespace pm::remote
