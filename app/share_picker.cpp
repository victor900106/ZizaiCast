// 傳到手機 batch picker (see share_picker.h, docs/share.md). Same
// construction as ask_panel.cpp / share_panel.cpp: owned WS_POPUP, Direct2D
// HWND render target in the popup menus' palette, layout in DIPs.
#include "share_picker.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

#include "pm/i18n.h"
#include "popup_menu.h"

using Microsoft::WRL::ComPtr;
using pm::i18n::S;
using pm::i18n::tr;

namespace pm::ui {
namespace {

constexpr wchar_t kClass[] = L"PhoneMirrorSharePicker";
constexpr int kCols = 4, kMaxVisibleRows = 2;
constexpr float kW = 540, kPad = 24, kGap = 12, kGridY = 92;
constexpr float kCellW = (kW - 2 * kPad - (kCols - 1) * kGap) / kCols;  // 112.5
constexpr float kThumbH = 152, kLabelH = 24, kCellH = kThumbH + kLabelH;
constexpr float kBtnH = 38, kBox = 24;
constexpr UINT kMsgThumb = WM_APP + 1;     // the worker has a thumbnail ready
constexpr UINT kThumbMaxW = 240, kThumbMaxH = 320;
constexpr wchar_t kGlyphCheck = 0xE73E;    // CheckMark
constexpr wchar_t kGlyphPlay = 0xE768;     // Play
constexpr wchar_t kGlyphVideo = 0xE714;    // Video (no thumbnail)
constexpr wchar_t kGlyphPhoto = 0xEB9F;    // Photo (no thumbnail yet)

D2D1_COLOR_F rgb(uint32_t c, float a = 1) {
    return {((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, a};
}
D2D1_COLOR_F withA(D2D1_COLOR_F c, float a) {
    c.a = a;
    return c;
}
D2D1_COLOR_F mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1};
}
float lum(D2D1_COLOR_F c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }
D2D1_RECT_F rc(float x, float y, float w, float h) { return {x, y, x + w, y + h}; }

struct Theme {
    D2D1_COLOR_F top, bottom, fg, dim, accent, onAccent;
    bool light;
};
Theme theme() {
    const Colors c = currentColors();
    Theme t;
    t.top = rgb(c.cardTop);
    t.bottom = rgb(c.cardBottom);
    t.fg = rgb(c.fg);
    t.dim = rgb(c.dim);
    t.accent = rgb(c.accent);
    t.light = c.light;
    t.onAccent = lum(t.accent) > 0.55f ? mix(D2D1_COLOR_F{0.13f, 0.09f, 0.10f, 1}, t.accent, 0.10f)
                                       : D2D1_COLOR_F{1, 1, 1, 1};
    return t;
}

struct Factories {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IDWriteTextFormat> title, sub, label, badge, button, glyph, glyphSmall, glyphBig;
    int lang = -1;
    bool init() {
        if (!d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return false;
        if (!dw && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                              reinterpret_cast<IUnknown**>(dw.GetAddressOf()))))
            return false;
        if (lang == static_cast<int>(pm::i18n::lang()) && title) return true;
        lang = static_cast<int>(pm::i18n::lang());
        auto fmt = [&](float size, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT a, bool wrap, const wchar_t* family = nullptr) {
            ComPtr<IDWriteTextFormat> f;
            dw->CreateTextFormat(family ? family : pm::i18n::uiFont(), nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, size, pm::i18n::localeName(), &f);
            if (f) {
                f->SetTextAlignment(a);
                f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
            }
            return f;
        };
        const auto C = DWRITE_TEXT_ALIGNMENT_CENTER, L = DWRITE_TEXT_ALIGNMENT_LEADING;
        title = fmt(21, DWRITE_FONT_WEIGHT_BOLD, L, false);
        sub = fmt(13, DWRITE_FONT_WEIGHT_NORMAL, L, true);
        label = fmt(12, DWRITE_FONT_WEIGHT_NORMAL, C, false);
        badge = fmt(11, DWRITE_FONT_WEIGHT_SEMI_BOLD, C, false);
        button = fmt(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, C, false);
        const wchar_t* iconFamily = L"Segoe MDL2 Assets";
        ComPtr<IDWriteFontCollection> sys;
        if (SUCCEEDED(dw->GetSystemFontCollection(&sys))) {
            UINT32 idx = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(sys->FindFamilyName(L"Segoe Fluent Icons", &idx, &exists)) && exists)
                iconFamily = L"Segoe Fluent Icons";
        }
        glyph = fmt(13, DWRITE_FONT_WEIGHT_NORMAL, C, false, iconFamily);
        glyphSmall = fmt(10, DWRITE_FONT_WEIGHT_NORMAL, C, false, iconFamily);
        glyphBig = fmt(28, DWRITE_FONT_WEIGHT_NORMAL, C, false, iconFamily);
        return title && sub && label && badge && button && glyph && glyphSmall && glyphBig;
    }
    float width(IDWriteTextFormat* f, const std::wstring& s) {
        ComPtr<IDWriteTextLayout> l;
        if (!f || FAILED(dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, 4096, 64, &l))) return 0;
        DWRITE_TEXT_METRICS m{};
        l->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    }
};
Factories& fx() {
    static Factories f;
    return f;
}

LRESULT CALLBACK pickerProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* p = reinterpret_cast<SharePicker*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (p && p->hwnd() == h) return p->handle(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

// ---- thumbnails (worker thread) ----
struct Thumb {
    int index = -1;
    UINT w = 0, h = 0;
    std::vector<uint8_t> px;  // premultiplied BGRA, top-down
};

bool wicThumb(IWICImagingFactory* f, const std::wstring& path, Thumb& t) {
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)))
        return false;
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (!w || !h) return false;
    const double k = (std::min)({double(kThumbMaxW) / w, double(kThumbMaxH) / h, 1.0});
    t.w = (std::max)(1u, static_cast<UINT>(std::lround(w * k)));
    t.h = (std::max)(1u, static_cast<UINT>(std::lround(h * k)));
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), t.w, t.h, WICBitmapInterpolationModeFant)) ||
        FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    t.px.resize(size_t(t.w) * t.h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, t.w * 4, static_cast<UINT>(t.px.size()), t.px.data()));
}

// Videos: the shell's thumbnail (Media Foundation's provider: a frame of the MP4).
bool shellThumb(const std::wstring& path, Thumb& t) {
    ComPtr<IShellItemImageFactory> sf;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&sf)))) return false;
    HBITMAP hb = nullptr;
    if (FAILED(sf->GetImage(SIZE{256, 256}, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &hb)) || !hb) return false;
    BITMAP bm{};
    GetObjectW(hb, sizeof(bm), &bm);
    bool ok = false;
    if (bm.bmWidth > 0 && bm.bmHeight > 0) {
        t.w = static_cast<UINT>(bm.bmWidth);
        t.h = static_cast<UINT>(bm.bmHeight);
        t.px.resize(size_t(t.w) * t.h * 4);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = bm.bmWidth;
        bi.bmiHeader.biHeight = -bm.bmHeight;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(nullptr);
        ok = GetDIBits(dc, hb, 0, t.h, t.px.data(), &bi, DIB_RGB_COLORS) == static_cast<int>(t.h);
        ReleaseDC(nullptr, dc);
        for (size_t i = 3; i < t.px.size(); i += 4) t.px[i] = 255;  // video frames are opaque
    }
    DeleteObject(hb);
    return ok;
}

struct ThumbJob {
    std::mutex mu;
    std::vector<Thumb> ready;
    std::atomic<bool> cancel{false};
    std::atomic<int> pending{0};
    HWND hwnd = nullptr;
};

void thumbWorker(std::shared_ptr<ThumbJob> job, std::vector<std::pair<std::wstring, bool>> files) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);  // never in the way of the picture
    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    // Newest first: those are the ones on screen.
    for (int i = static_cast<int>(files.size()) - 1; i >= 0 && !job->cancel; --i) {
        Thumb t;
        t.index = i;
        const bool ok = files[size_t(i)].second ? shellThumb(files[size_t(i)].first, t)
                                                : (wic && wicThumb(wic.Get(), files[size_t(i)].first, t));
        if (!ok) t.w = t.h = 0, t.px.clear();
        {
            std::lock_guard<std::mutex> lk(job->mu);
            job->ready.push_back(std::move(t));
        }
        --job->pending;
        PostMessageW(job->hwnd, kMsgThumb, 0, 0);
    }
    wic.Reset();
    if (SUCCEEDED(co)) CoUninitialize();
}

}  // namespace

struct SharePicker::Impl {
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> brush;
    std::vector<Thumb> thumbs;  // by item; w == 0: none (yet)
    std::vector<bool> tried;    // the worker is done with it
    std::vector<ComPtr<ID2D1Bitmap>> bitmaps;  // on rt
    std::vector<ComPtr<ID2D1Bitmap>> offBitmaps;  // renderPng's target
    std::shared_ptr<ThumbJob> job;
    std::thread worker;
    Theme th = theme();
};

SharePicker::~SharePicker() { close(); }

void SharePicker::layout() {
    Factories& f = fx();
    f.init();
    rows_ = (std::max)(1, (static_cast<int>(items_.size()) + kCols - 1) / kCols);
    const int vis = (std::min)(rows_, kMaxVisibleRows);
    contentH_ = rows_ * kCellH + (rows_ - 1) * kGap;
    // More than two rows: show a sliver of the third, so it is clear the grid scrolls.
    gridH_ = vis * kCellH + (vis - 1) * kGap + (rows_ > kMaxVisibleRows ? 40.0f : 0.0f);
    footY_ = kGridY + gridH_ + 20;
    h_ = std::ceil(footY_ + kBtnH + 22);
    allW_ = (std::max)(84.0f, std::ceil((std::max)(f.width(f.button.Get(), tr(S::SharePickerAll)),
                                                   f.width(f.button.Get(), tr(S::SharePickerNone)))) + 32);
    cancelW_ = (std::max)(84.0f, std::ceil(f.width(f.button.Get(), tr(S::Cancel))) + 32);
    sendW_ = (std::max)(116.0f, std::ceil(f.width(f.button.Get(), pm::i18n::fmt(S::SharePickerSend, {L"88"}))) + 40);
    scroll_ = std::clamp(scroll_, 0.0f, maxScroll());
}

float SharePicker::maxScroll() const { return (std::max)(0.0f, contentH_ - gridH_); }

bool SharePicker::open(HWND owner, std::vector<Item> items, std::function<void(std::vector<std::wstring>)> onSend) {
    if (!fx().init()) return false;
    close();
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = pickerProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    items_ = std::move(items);
    onSend_ = std::move(onSend);
    owner_ = owner;
    hot_ = pressed_ = HitAt{};
    scroll_ = 0;
    impl_ = new Impl;
    impl_->thumbs.resize(items_.size());
    impl_->tried.assign(items_.size(), false);
    impl_->bitmaps.resize(items_.size());
    layout();

    RECT orc{};
    const bool ownerVisible = owner && IsWindowVisible(owner) && !IsIconic(owner);
    if (ownerVisible) GetWindowRect(owner, &orc);
    HMONITOR mon = ownerVisible ? MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST)
                                : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    if (!ownerVisible) orc = mi.rcWork;
    UINT dx = 96, dy = 96;
    if (HMODULE shcore = GetModuleHandleW(L"shcore.dll")) {
        using Fn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        if (auto fn = reinterpret_cast<Fn>(GetProcAddress(shcore, "GetDpiForMonitor"))) fn(mon, 0, &dx, &dy);
    }
    dpi_ = dx;
    const int w = static_cast<int>(std::lround(kW * s())), hgt = static_cast<int>(std::lround(h_ * s()));
    int x = (orc.left + orc.right) / 2 - w / 2, y = (orc.top + orc.bottom) / 2 - hgt / 2;
    if (!testOffscreen) {
        x = (std::max)(static_cast<int>(mi.rcWork.left), (std::min)(x, static_cast<int>(mi.rcWork.right) - w));
        y = (std::max)(static_cast<int>(mi.rcWork.top), (std::min)(y, static_cast<int>(mi.rcWork.bottom) - hgt));
    } else if (!ownerVisible) {
        x = -12000;
        y = 200;
    }
    hwnd_ = CreateWindowExW(0, kClass, tr(S::SharePanelTitle), WS_POPUP | WS_SYSMENU, x, y, w, hgt, owner, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    dpi_ = GetDpiForWindow(hwnd_);
    if (dpi_ != dx)
        SetWindowPos(hwnd_, nullptr, 0, 0, static_cast<int>(std::lround(kW * s())), static_cast<int>(std::lround(h_ * s())),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    const DWORD round = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &round, sizeof(round));

    auto job = std::make_shared<ThumbJob>();
    job->hwnd = hwnd_;
    job->pending = static_cast<int>(items_.size());
    std::vector<std::pair<std::wstring, bool>> files;
    for (const Item& it : items_) files.emplace_back(it.path, it.video);
    impl_->job = job;
    impl_->worker = std::thread(thumbWorker, job, std::move(files));

    ShowWindow(hwnd_, testOffscreen ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
    if (!testOffscreen) SetForegroundWindow(hwnd_);
    return true;
}

void SharePicker::close() {
    if (!impl_) return;
    if (impl_->job) impl_->job->cancel = true;
    HWND h = hwnd_;
    hwnd_ = nullptr;
    if (h) DestroyWindow(h);
    if (impl_->worker.joinable()) impl_->worker.join();  // at most the thumbnail in progress
    destroyTarget();
    delete impl_;
    impl_ = nullptr;
    if (h && owner_ && IsWindowVisible(owner_) && !testOffscreen) SetForegroundWindow(owner_);
}

void SharePicker::destroyTarget() {
    if (!impl_) return;
    for (auto& b : impl_->bitmaps) b.Reset();
    impl_->brush.Reset();
    impl_->rt.Reset();
}

void SharePicker::retheme() {
    if (!impl_) return;
    impl_->th = theme();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SharePicker::relabel() {
    if (!hwnd_) return;
    layout();
    SetWindowTextW(hwnd_, tr(S::SharePanelTitle));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SharePicker::takeThumbs() {
    if (!impl_ || !impl_->job) return;
    std::vector<Thumb> got;
    {
        std::lock_guard<std::mutex> lk(impl_->job->mu);
        got.swap(impl_->job->ready);
    }
    for (Thumb& t : got) {
        if (t.index < 0 || t.index >= static_cast<int>(items_.size())) continue;
        impl_->tried[size_t(t.index)] = true;
        impl_->thumbs[size_t(t.index)] = std::move(t);
        impl_->bitmaps[size_t(t.index)].Reset();
    }
    if (!got.empty() && hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

int SharePicker::thumbsPending() const { return impl_ && impl_->job ? impl_->job->pending.load() : 0; }

int SharePicker::checkedCount() const {
    int n = 0;
    for (const Item& it : items_) n += it.checked ? 1 : 0;
    return n;
}

void SharePicker::toggle(int i) {
    if (i < 0 || i >= static_cast<int>(items_.size())) return;
    items_[size_t(i)].checked = !items_[size_t(i)].checked;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SharePicker::setAll(bool on) {
    for (Item& it : items_) it.checked = on;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SharePicker::send() {
    std::vector<std::wstring> out;
    for (const Item& it : items_)
        if (it.checked) out.push_back(it.path);
    if (out.empty()) return;
    auto cb = std::move(onSend_);
    onSend_ = nullptr;
    close();
    if (cb) cb(std::move(out));
}

SharePicker::HitAt SharePicker::hitTest(POINT pt) const {
    const float x = pt.x / s(), y = pt.y / s();
    auto in = [&](D2D1_RECT_F r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
    if (in(rc(kW - 52, 12, 36, 36))) return {HitClose, -1};
    if (in(rc(kPad, footY_, allW_, kBtnH))) return {HitAll, -1};
    if (in(rc(kW - kPad - sendW_, footY_, sendW_, kBtnH))) return {HitSend, -1};
    if (in(rc(kW - kPad - sendW_ - 10 - cancelW_, footY_, cancelW_, kBtnH))) return {HitCancel, -1};
    if (in(rc(kPad, kGridY, kW - 2 * kPad, gridH_))) {
        const float gy = y - kGridY + scroll_, gx = x - kPad;
        const int col = static_cast<int>(gx / (kCellW + kGap)), row = static_cast<int>(gy / (kCellH + kGap));
        const float cx = gx - col * (kCellW + kGap), cy = gy - row * (kCellH + kGap);
        const int i = row * kCols + col;
        if (col >= 0 && col < kCols && cx < kCellW && cy < kCellH && i >= 0 && i < static_cast<int>(items_.size()))
            return {HitCell, i};
    }
    return {};
}

void SharePicker::click(HitAt h) {
    switch (h.hit) {
    case HitClose:
    case HitCancel: close(); break;
    case HitAll: setAll(checkedCount() < static_cast<int>(items_.size())); break;
    case HitSend: send(); break;
    case HitCell: toggle(h.cell); break;
    default: break;
    }
}

void SharePicker::paint() {
    Impl& im = *impl_;
    if (!im.rt) {
        RECT r{};
        GetClientRect(hwnd_, &r);
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
        props.dpiX = props.dpiY = static_cast<float>(dpi_);
        if (FAILED(fx().d2d->CreateHwndRenderTarget(
                props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(r.right, r.bottom)), &im.rt)))
            return;
        im.rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &im.brush);
        im.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    im.rt->BeginDraw();
    draw(im.rt.Get(), im.brush.Get(), false);
    if (im.rt->EndDraw() == D2DERR_RECREATE_TARGET) destroyTarget();
}

bool SharePicker::renderPng(const std::wstring& path) {
    if (!hwnd_ || !impl_) return false;
    takeThumbs();
    const bool ok = renderToPng(kW, h_, dpi_, path, [this](ID2D1RenderTarget* rt) {
        ComPtr<ID2D1SolidColorBrush> b;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &b);
        impl_->offBitmaps.assign(items_.size(), nullptr);
        if (b) draw(rt, b.Get(), true);
        impl_->offBitmaps.clear();
    });
    return ok;
}

void SharePicker::draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* b, bool offscreen) {
    Impl& im = *impl_;
    Factories& f = fx();
    const Theme& t = im.th;
    auto fill = [&](D2D1_RECT_F r, D2D1_COLOR_F c, float radius = 0) {
        b->SetColor(c);
        if (radius > 0) rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b);
        else rt->FillRectangle(r, b);
    };
    auto stroke = [&](D2D1_RECT_F r, D2D1_COLOR_F c, float radius, float w = 1) {
        b->SetColor(c);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b, w);
    };
    auto text = [&](const std::wstring& s, IDWriteTextFormat* fmtx, D2D1_RECT_F r, D2D1_COLOR_F c) {
        b->SetColor(c);
        const std::wstring w = pm::i18n::keepWords(s);  // 한국어: wrap between words
        rt->DrawTextW(w.c_str(), static_cast<UINT32>(w.size()), fmtx, r, b);
    };
    // Card: gradient, accent bar, hairline border.
    {
        ComPtr<ID2D1GradientStopCollection> stops;
        D2D1_GRADIENT_STOP gs[2] = {{0, t.top}, {1, t.bottom}};
        rt->CreateGradientStopCollection(gs, 2, &stops);
        ComPtr<ID2D1LinearGradientBrush> g;
        if (stops) rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {0, h_}), stops.Get(), &g);
        if (g) rt->FillRectangle(rc(0, 0, kW, h_), g.Get());
        else rt->Clear(t.top);
        fill(rc(0, 0, kW, 3), withA(t.accent, 0.85f));
        b->SetColor(withA(t.accent, 0.35f));
        rt->DrawRectangle(rc(0.5f, 0.5f, kW - 1, h_ - 1), b, 1);
    }
    text(tr(S::SharePanelTitle), f.title.Get(), rc(kPad, 16, kW - 2 * kPad - 40, 34), t.fg);
    text(tr(S::SharePickerSub), f.sub.Get(), rc(kPad, 52, kW - 2 * kPad - 20, 30), t.dim);
    {
        const D2D1_RECT_F cr = rc(kW - 52, 12, 36, 36);
        if (hot_.hit == HitClose) fill(cr, withA(t.accent, 0.22f), 8);
        b->SetColor(hot_.hit == HitClose ? t.fg : t.dim);
        const float cx = kW - 34, cy = 30, d = 5.5f;
        rt->DrawLine({cx - d, cy - d}, {cx + d, cy + d}, b, 1.6f);
        rt->DrawLine({cx - d, cy + d}, {cx + d, cy - d}, b, 1.6f);
    }

    // Grid (clipped, scrolled).
    rt->PushAxisAlignedClip(rc(0, kGridY - 6, kW, gridH_ + 12), D2D1_ANTIALIAS_MODE_ALIASED);
    auto& bitmaps = offscreen ? im.offBitmaps : im.bitmaps;
    const D2D1_COLOR_F white{1, 1, 1, 1}, shade{0, 0, 0, 0.55f};
    for (size_t i = 0; i < items_.size(); ++i) {
        const Item& it = items_[i];
        const int row = static_cast<int>(i) / kCols, col = static_cast<int>(i) % kCols;
        const float x = kPad + col * (kCellW + kGap), y = kGridY + row * (kCellH + kGap) - scroll_;
        if (y + kCellH < kGridY - 6 || y > kGridY + gridH_ + 6) continue;
        const bool hot = hot_.hit == HitCell && hot_.cell == static_cast<int>(i);
        const D2D1_RECT_F box = rc(x, y, kCellW, kThumbH);
        fill(box, t.light ? withA(t.fg, 0.10f) : D2D1_COLOR_F{0, 0, 0, 0.30f}, 10);
        // Thumbnail, fitted.
        const Thumb& th = im.thumbs[i];
        if (th.w > 0) {
            if (!bitmaps[i]) {
                D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
                rt->CreateBitmap(D2D1::SizeU(th.w, th.h), th.px.data(), th.w * 4, bp, &bitmaps[i]);
            }
            if (bitmaps[i]) {
                const float aw = kCellW - 10, ah = kThumbH - 10;
                const float k = (std::min)(aw / th.w, ah / th.h);
                const float dw = th.w * k, dh = th.h * k;
                const D2D1_RECT_F dst = rc(x + (kCellW - dw) / 2, y + (kThumbH - dh) / 2, dw, dh);
                rt->DrawBitmap(bitmaps[i].Get(), dst, it.checked ? 1.0f : 0.45f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
        } else {  // not made (yet / at all): the kind's glyph
            const wchar_t g[2] = {it.video ? kGlyphVideo : kGlyphPhoto, 0};
            text(g, f.glyphBig.Get(), box, withA(t.dim, im.tried[i] ? 0.8f : 0.4f));
        }
        if (it.video) {  // ▶ badge, bottom left
            const D2D1_RECT_F pill = rc(x + 8, y + kThumbH - 30, 26, 22);
            fill(pill, shade, 6);
            const wchar_t g[2] = {kGlyphPlay, 0};
            text(g, f.glyphSmall.Get(), pill, white);
        }
        if (it.sent) {  // 已傳, top left
            const std::wstring s = tr(S::SharePickerSent);
            const float w = std::ceil(f.width(f.badge.Get(), s)) + 14;
            const D2D1_RECT_F pill = rc(x + 7, y + 7, w, 20);
            fill(pill, shade, 10);
            text(s, f.badge.Get(), pill, white);
        }
        // Check box, top right.
        const D2D1_RECT_F cb = rc(x + kCellW - kBox - 7, y + 7, kBox, kBox);
        if (it.checked) {
            fill(cb, t.accent, kBox / 2);
            const wchar_t g[2] = {kGlyphCheck, 0};
            text(g, f.glyph.Get(), cb, t.onAccent);
        } else {
            fill(cb, D2D1_COLOR_F{0, 0, 0, 0.35f}, kBox / 2);
            stroke(rc(cb.left + 1, cb.top + 1, kBox - 2, kBox - 2), withA(white, 0.9f), kBox / 2 - 1, 1.6f);
        }
        if (it.checked) stroke(rc(x + 0.75f, y + 0.75f, kCellW - 1.5f, kThumbH - 1.5f), t.accent, 10, 1.5f);
        else if (hot) stroke(rc(x + 0.5f, y + 0.5f, kCellW - 1, kThumbH - 1), withA(t.accent, 0.7f), 10);
        if (hot && it.checked) stroke(rc(x - 1.5f, y - 1.5f, kCellW + 3, kThumbH + 3), withA(t.accent, 0.5f), 11, 1);
        text(it.label, f.label.Get(), rc(x, y + kThumbH + 2, kCellW, kLabelH - 2), it.checked ? t.fg : t.dim);
    }
    rt->PopAxisAlignedClip();
    // Scroll hint: a thin bar on the right.
    if (maxScroll() > 0) {
        const float trackH = gridH_, barH = (std::max)(30.0f, trackH * gridH_ / contentH_);
        const float by = kGridY + (trackH - barH) * (scroll_ / maxScroll());
        fill(rc(kW - kPad / 2 - 2, kGridY, 4, trackH), withA(t.fg, 0.08f), 2);
        fill(rc(kW - kPad / 2 - 2, by, 4, barH), withA(t.accent, 0.7f), 2);
    }

    // Footer: [全選]            [取消] [傳送 N 個]
    auto button = [&](D2D1_RECT_F r, const std::wstring& label, bool primary, bool hot, bool enabled) {
        const float w = r.right - r.left;
        if (primary) {
            D2D1_COLOR_F c = t.accent;
            if (hot && enabled) c = mix(c, t.light ? D2D1_COLOR_F{0, 0, 0, 1} : D2D1_COLOR_F{1, 1, 1, 1}, 0.15f);
            fill(r, withA(c, enabled ? 1.0f : 0.4f), 9);
            text(label, f.button.Get(), r, withA(t.onAccent, enabled ? 1.0f : 0.7f));
        } else {
            fill(r, withA(t.fg, hot ? 0.14f : 0.07f), 9);
            stroke(rc(r.left + 0.5f, r.top + 0.5f, w - 1, kBtnH - 1), withA(t.fg, 0.2f), 9);
            text(label, f.button.Get(), r, t.fg);
        }
    };
    const int n = checkedCount();
    const bool all = n == static_cast<int>(items_.size());
    button(rc(kPad, footY_, allW_, kBtnH), tr(all ? S::SharePickerNone : S::SharePickerAll), false, hot_.hit == HitAll, true);
    button(rc(kW - kPad - sendW_ - 10 - cancelW_, footY_, cancelW_, kBtnH), tr(S::Cancel), false, hot_.hit == HitCancel,
           true);
    button(rc(kW - kPad - sendW_, footY_, sendW_, kBtnH), pm::i18n::fmt(S::SharePickerSend, {std::to_wstring(n)}), true,
           hot_.hit == HitSend, n > 0);
}

LRESULT SharePicker::handle(UINT msg, WPARAM wp, LPARAM lp) {
    HWND h = hwnd_;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        if (impl_) paint();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case kMsgThumb: takeThumbs(); return 0;
    case WM_SIZE:
        if (impl_ && impl_->rt) {
            RECT r{};
            GetClientRect(h, &r);
            impl_->rt->Resize(D2D1::SizeU(r.right, r.bottom));
            impl_->rt->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
        }
        return 0;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &pt);
        if (pt.y < (kGridY - 8) * s() && hitTest(pt).hit == HitNone) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && hot_.hit != HitNone) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        const HitAt nh = hitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (!(nh == hot_)) {
            hot_ = nh;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hot_.hit != HitNone) {
            hot_ = HitAt{};
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        const float before = scroll_;
        scroll_ = std::clamp(scroll_ - GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * 60.0f, 0.0f, maxScroll());
        if (scroll_ != before) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h, &pt);
            hot_ = hitTest(pt);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        pressed_ = hitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        return 0;
    case WM_LBUTTONUP: {
        const HitAt up = hitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        const HitAt was = pressed_;
        pressed_ = HitAt{};
        if (up.hit != HitNone && up == was) click(up);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) close();
        else if (wp == VK_RETURN) send();
        else if (wp == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) setAll(checkedCount() < static_cast<int>(items_.size()));
        return 0;
    case WM_CLOSE: close(); return 0;
    case WM_DESTROY:
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace pm::ui
