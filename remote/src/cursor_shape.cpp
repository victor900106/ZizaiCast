#include "cursor_shape.h"

#include <dxgi1_2.h>
#include <objidl.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace pm::remote {

using Microsoft::WRL::ComPtr;

namespace {
bool bitsOf(HBITMAP bmp, int w, int h, std::vector<uint32_t>& out) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    out.assign(size_t(w) * size_t(h), 0);
    HDC dc = GetDC(nullptr);
    const int got = GetDIBits(dc, bmp, 0, UINT(h), out.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    return got == h;
}
}  // namespace

bool cursorShapeOf(HCURSOR hc, CursorShape& s) {
    s = CursorShape{};
    if (!hc) return false;
    ICONINFO ii{};
    if (!GetIconInfo(hc, &ii)) return false;
    bool ok = false;
    BITMAP bm{};
    if (ii.hbmMask && GetObject(ii.hbmMask, sizeof(bm), &bm)) {
        const int w = bm.bmWidth, mh = bm.bmHeight;
        const int h = ii.hbmColor ? mh : mh / 2;
        if (w > 0 && h > 0 && w <= 256 && h <= 256) {
            std::vector<uint32_t> mask, col;
            if (bitsOf(ii.hbmMask, w, mh, mask) && (!ii.hbmColor || bitsOf(ii.hbmColor, w, h, col))) {
                s.w = w, s.h = h, s.hotX = int(ii.xHotspot), s.hotY = int(ii.yHotspot);
                s.color.assign(size_t(w) * size_t(h), 0);
                std::vector<uint32_t> inv(size_t(w) * size_t(h), 0);
                bool anyInv = false, anyAlpha = false;
                if (!col.empty())
                    for (uint32_t p : col) anyAlpha |= (p >> 24) != 0;
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        const size_t i = size_t(y) * size_t(w) + size_t(x);
                        const bool andBit = (mask[i] & 0xFFFFFF) != 0;  // white in the AND mask = keep the screen
                        if (col.empty()) {  // monochrome: AND on top, XOR below
                            const bool xorBit = (mask[size_t(y + h) * size_t(w) + size_t(x)] & 0xFFFFFF) != 0;
                            if (!andBit) s.color[i] = xorBit ? 0xFFFFFFFFu : 0xFF000000u;
                            else if (xorBit) inv[i] = 0xFFFFFFFFu, anyInv = true;
                        } else if (anyAlpha) {  // 32-bit with alpha: the colour is the picture
                            s.color[i] = col[i];
                        } else {  // colour without alpha: AND 0 = paint, AND 1 + colour = XOR
                            if (!andBit) s.color[i] = col[i] | 0xFF000000u;
                            else if (col[i] & 0xFFFFFF) inv[i] = 0xFFFFFFFFu, anyInv = true;
                        }
                    }
                if (anyInv) s.invert = std::move(inv);
                ok = true;
            }
        }
    }
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    return ok;
}

void cursorShapeFromDxgi(const uint8_t* b, int type, int w, int height, int pitch, int hotX, int hotY, CursorShape& s) {
    s = CursorShape{};
    const bool mono = type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
    const int h = mono ? height / 2 : height;
    if (w <= 0 || h <= 0 || w > 512 || h > 512) return;
    s.w = w, s.h = h, s.hotX = hotX, s.hotY = hotY;
    s.color.assign(size_t(w) * size_t(h), 0);
    std::vector<uint32_t> inv(size_t(w) * size_t(h), 0);
    bool anyInv = false;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t& c = s.color[size_t(y) * size_t(w) + size_t(x)];
            uint32_t& v = inv[size_t(y) * size_t(w) + size_t(x)];
            if (mono) {
                const int andBit = (b[size_t(y) * size_t(pitch) + size_t(x / 8)] >> (7 - x % 8)) & 1;
                const int xorBit = (b[size_t(y + h) * size_t(pitch) + size_t(x / 8)] >> (7 - x % 8)) & 1;
                if (!andBit) c = xorBit ? 0xFFFFFFFFu : 0xFF000000u;
                else if (xorBit) v = 0xFFFFFFFFu, anyInv = true;
            } else {
                const uint32_t px = *reinterpret_cast<const uint32_t*>(b + size_t(y) * size_t(pitch) + size_t(x) * 4);
                if (type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) c = px;
                else if ((px >> 24) == 0) c = px | 0xFF000000u;   // masked: replace
                else if (px & 0xFFFFFFu) v = 0xFFFFFFFFu, anyInv = true;  // masked: XOR
            }
        }
    if (anyInv) s.invert = std::move(inv);
}

std::string pngOf(const std::vector<uint32_t>& px, int w, int h) {
    if (w <= 0 || h <= 0 || px.size() < size_t(w) * size_t(h)) return {};
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return {};
    ComPtr<IStream> st;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &st))) return {};
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(st.Get(), WICBitmapEncoderNoCache)))
        return {};
    ComPtr<IWICBitmapFrameEncode> fr;
    if (FAILED(enc->CreateNewFrame(&fr, nullptr)) || FAILED(fr->Initialize(nullptr))) return {};
    fr->SetSize(UINT(w), UINT(h));
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    fr->SetPixelFormat(&fmt);
    if (fmt != GUID_WICPixelFormat32bppBGRA) return {};
    if (FAILED(fr->WritePixels(UINT(h), UINT(w) * 4, UINT(w) * UINT(h) * 4, reinterpret_cast<BYTE*>(const_cast<uint32_t*>(px.data())))) ||
        FAILED(fr->Commit()) || FAILED(enc->Commit()))
        return {};
    HGLOBAL g = nullptr;
    if (FAILED(GetHGlobalFromStream(st.Get(), &g))) return {};
    STATSTG ss{};
    st->Stat(&ss, STATFLAG_NONAME);
    std::string out;
    if (const void* p = GlobalLock(g)) {
        out.assign(static_cast<const char*>(p), size_t(ss.cbSize.QuadPart));
        GlobalUnlock(g);
    }
    return out;
}

std::string base64(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = uint32_t(uint8_t(in[i])) << 16 | uint32_t(uint8_t(in[i + 1])) << 8 | uint8_t(in[i + 2]);
        o += T[v >> 18], o += T[(v >> 12) & 63], o += T[(v >> 6) & 63], o += T[v & 63];
    }
    if (i < in.size()) {
        uint32_t v = uint32_t(uint8_t(in[i])) << 16;
        if (i + 1 < in.size()) v |= uint32_t(uint8_t(in[i + 1])) << 8;
        o += T[v >> 18], o += T[(v >> 12) & 63];
        o += i + 1 < in.size() ? T[(v >> 6) & 63] : '=';
        o += '=';
    }
    return o;
}

uint32_t cursorShapeId(const CursorShape& s) {
    uint32_t h = 2166136261u;
    auto mix = [&](uint32_t v) {
        for (int k = 0; k < 4; ++k) h = (h ^ ((v >> (8 * k)) & 0xFF)) * 16777619u;
    };
    mix(uint32_t(s.w)), mix(uint32_t(s.h)), mix(uint32_t(s.hotX)), mix(uint32_t(s.hotY));
    for (uint32_t p : s.color) mix(p);
    for (uint32_t p : s.invert) mix(p);
    return h ? h : 1;
}

}  // namespace pm::remote
