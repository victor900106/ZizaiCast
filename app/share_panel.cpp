// 傳到手機 panel and chip (see share_panel.h, docs/share.md). Built like the
// pairing / About panels: owned WS_POPUP, Direct2D HWND render target in the
// popup menus' palette, Windows 11 rounded corners, per-monitor DPI.
#include "share_panel.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

#include "pm/i18n.h"
#include "popup_menu.h"

using Microsoft::WRL::ComPtr;
using pm::i18n::S;
using pm::i18n::tr;

namespace pm::ui {
namespace {

constexpr wchar_t kPanelClass[] = L"PhoneMirrorSharePanel";
constexpr wchar_t kChipClass[] = L"PhoneMirrorShareChip";
constexpr float kW = 380, kH = 590, kPad = 26;
constexpr float kQrCard = 236, kQrY = 116;
constexpr float kFilesY = 364, kUrlY = 387, kCopyY = 408, kLinkH = 22;
constexpr float kCountY = 442, kStatusY = 472;
constexpr float kBtnH = 38, kBtnY = kH - 22 - kBtnH;
constexpr float kChipH = 40, kChipX = 34;  // chip height, × area width
constexpr UINT_PTR kTickTimer = 1, kHideTimer = 2;
constexpr wchar_t kGlyphShare = 0xE72D;  // Share

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
    D2D1_COLOR_F top, bottom, fg, dim, accent, onAccent, error;
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
    t.error = c.light ? D2D1_COLOR_F{0.75f, 0.22f, 0.20f, 1} : D2D1_COLOR_F{1.0f, 0.62f, 0.45f, 1};
    return t;
}

struct Factories {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IDWriteTextFormat> title, body, note, noteCenter, files, url, link, count, status, button, chip, glyph;
    int lang = -1;
    bool init() {
        if (!d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return false;
        if (!dw && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                              reinterpret_cast<IUnknown**>(dw.GetAddressOf()))))
            return false;
        if (lang == static_cast<int>(pm::i18n::lang()) && title) return true;
        lang = static_cast<int>(pm::i18n::lang());
        auto fmt = [&](float size, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT a, bool wrap, bool ellipsis = false,
                       const wchar_t* family = nullptr) {
            ComPtr<IDWriteTextFormat> f;
            dw->CreateTextFormat(family ? family : pm::i18n::uiFont(), nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, size, pm::i18n::localeName(), &f);
            if (f) {
                f->SetTextAlignment(a);
                f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
                if (ellipsis) {
                    ComPtr<IDWriteInlineObject> sign;
                    dw->CreateEllipsisTrimmingSign(f.Get(), &sign);
                    DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                    f->SetTrimming(&tr, sign.Get());
                }
            }
            return f;
        };
        const auto C = DWRITE_TEXT_ALIGNMENT_CENTER, L = DWRITE_TEXT_ALIGNMENT_LEADING;
        title = fmt(21, DWRITE_FONT_WEIGHT_BOLD, L, false);
        body = fmt(16, DWRITE_FONT_WEIGHT_SEMI_BOLD, L, false);
        note = fmt(12.5f, DWRITE_FONT_WEIGHT_NORMAL, L, true);
        files = fmt(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, C, false, true);
        url = fmt(11.5f, DWRITE_FONT_WEIGHT_NORMAL, C, false, true, L"Consolas");
        link = fmt(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, C, false);
        count = fmt(16, DWRITE_FONT_WEIGHT_SEMI_BOLD, C, false);
        status = fmt(13, DWRITE_FONT_WEIGHT_NORMAL, C, true);
        button = fmt(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, C, false);
        if (button) button->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        chip = fmt(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, L, false);
        if (chip) chip->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        // Icon font: Segoe Fluent Icons (Windows 11), else Segoe MDL2 Assets.
        const wchar_t* iconFamily = L"Segoe MDL2 Assets";
        ComPtr<IDWriteFontCollection> sys;
        if (SUCCEEDED(dw->GetSystemFontCollection(&sys))) {
            UINT32 idx = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(sys->FindFamilyName(L"Segoe Fluent Icons", &idx, &exists)) && exists)
                iconFamily = L"Segoe Fluent Icons";
        }
        glyph = fmt(16, DWRITE_FONT_WEIGHT_NORMAL, C, false, false, iconFamily);
        if (glyph) glyph->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        return title && body && note && files && url && link && count && status && button && chip && glyph;
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

template <class T>
LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* p = reinterpret_cast<T*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (p && p->hwnd() == h) return p->handle(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

UINT monitorDpi(HMONITOR mon) {
    UINT dx = 96, dy = 96;
    if (HMODULE shcore = GetModuleHandleW(L"shcore.dll")) {
        using Fn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        if (auto f = reinterpret_cast<Fn>(GetProcAddress(shcore, "GetDpiForMonitor"))) f(mon, 0, &dx, &dy);
    }
    return dx;
}

void cardBackground(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* b, const Theme& t, float w, float h, bool bar) {
    ComPtr<ID2D1GradientStopCollection> stops;
    D2D1_GRADIENT_STOP gs[2] = {{0, t.top}, {1, t.bottom}};
    rt->CreateGradientStopCollection(gs, 2, &stops);
    ComPtr<ID2D1LinearGradientBrush> g;
    if (stops) rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {0, h}), stops.Get(), &g);
    if (g) rt->FillRectangle(rc(0, 0, w, h), g.Get());
    else rt->Clear(t.top);
    if (bar) {
        b->SetColor(withA(t.accent, 0.85f));
        rt->FillRectangle(rc(0, 0, w, 3), b);
    }
    b->SetColor(withA(t.accent, 0.35f));
    rt->DrawRectangle(rc(0.5f, 0.5f, w - 1, h - 1), b, 1);
}

}  // namespace

// ================================================================ SharePanel ==

struct SharePanel::Impl {
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1Bitmap> qr;
    Theme th = theme();
};

bool SharePanel::open(HWND owner, Callbacks cb) {
    if (hwnd_) {
        cb_ = std::move(cb);
        ShowWindow(hwnd_, testOffscreen ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
        if (!testOffscreen) SetForegroundWindow(hwnd_);
        return true;
    }
    if (!fx().init()) return false;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = proc<SharePanel>;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kPanelClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    owner_ = owner;
    cb_ = std::move(cb);
    hot_ = pressed_ = HitNone;
    expired_ = false;
    status_.clear();
    impl_ = new Impl;
    RECT orc{};
    const bool ownerVisible = owner && IsWindowVisible(owner) && !IsIconic(owner);
    if (ownerVisible) GetWindowRect(owner, &orc);
    HMONITOR mon = ownerVisible ? MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST)
                                : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    if (!ownerVisible) orc = mi.rcWork;
    dpi_ = monitorDpi(mon);
    const int w = static_cast<int>(std::lround(kW * s())), hgt = static_cast<int>(std::lround(kH * s()));
    int x = (orc.left + orc.right) / 2 - w / 2, y = (orc.top + orc.bottom) / 2 - hgt / 2;
    if (!testOffscreen) {
        x = (std::max)(static_cast<int>(mi.rcWork.left), (std::min)(x, static_cast<int>(mi.rcWork.right) - w));
        y = (std::max)(static_cast<int>(mi.rcWork.top), (std::min)(y, static_cast<int>(mi.rcWork.bottom) - hgt));
    }
    hwnd_ = CreateWindowExW(0, kPanelClass, tr(S::SharePanelTitle), WS_POPUP | WS_SYSMENU, x, y, w, hgt, owner, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    dpi_ = GetDpiForWindow(hwnd_);
    const DWORD round = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &round, sizeof(round));
    SetTimer(hwnd_, kTickTimer, 1000, nullptr);
    ShowWindow(hwnd_, testOffscreen ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
    if (!testOffscreen) SetForegroundWindow(hwnd_);
    return true;
}

void SharePanel::close() {
    if (!hwnd_) return;
    HWND h = hwnd_;
    hwnd_ = nullptr;
    KillTimer(h, kTickTimer);
    DestroyWindow(h);
    destroyTarget();
    delete impl_;
    impl_ = nullptr;
    if (owner_ && IsWindowVisible(owner_) && !testOffscreen) SetForegroundWindow(owner_);
}

void SharePanel::destroyTarget() {
    if (!impl_) return;
    impl_->qr.Reset();
    impl_->brush.Reset();
    impl_->rt.Reset();
}

void SharePanel::setShare(const std::string& url, const std::vector<uint8_t>& qrBgra, int qrSize,
                          const std::wstring& files, int seconds) {
    url_.assign(url.begin(), url.end());
    files_ = files;
    if (qrSize > 0 && qrBgra.size() >= static_cast<size_t>(qrSize) * qrSize * 4) {
        qr_ = qrBgra;
        qrSize_ = qrSize;
    } else {
        qr_.clear();
        qrSize_ = 0;
    }
    deadline_ = GetTickCount64() + static_cast<unsigned long long>(seconds) * 1000;
    expired_ = false;
    status_.clear();
    if (impl_) impl_->qr.Reset();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SharePanel::setLive(bool live, int sent) {
    live_ = live;
    liveSent_ = sent;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SharePanel::setExpired() {
    expired_ = true;
    status_.clear();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SharePanel::setFiles(const std::wstring& files) {
    files_ = files;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SharePanel::setStatus(const std::wstring& text) {
    status_ = text;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SharePanel::retheme() {
    if (!impl_) return;
    impl_->th = theme();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SharePanel::relabel() {
    if (!hwnd_) return;
    fx().init();
    SetWindowTextW(hwnd_, tr(S::SharePanelTitle));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::wstring SharePanel::countdownText() const {
    const unsigned long long now = GetTickCount64();
    const long long left = deadline_ > now ? static_cast<long long>((deadline_ - now + 999) / 1000) : 0;
    wchar_t mmss[16];
    swprintf_s(mmss, L"%lld:%02lld", left / 60, left % 60);
    return pm::i18n::fmt(S::SharePanelLeft, {mmss});
}

SharePanel::Hit SharePanel::hitTest(POINT pt) const {
    const float x = pt.x / s(), y = pt.y / s();
    auto in = [&](D2D1_RECT_F r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
    if (in(rc(kW - 52, 12, 36, 36))) return HitClose;
    if (!expired_ && in(rc(kW / 2 - copyW_ / 2 - 6, kCopyY - 2, copyW_ + 12, kLinkH + 4))) return HitCopy;
    if (expired_) {
        const float bw = (kW - 2 * kPad - 12) / 2;
        if (in(rc(kPad, kBtnY, bw, kBtnH))) return HitSecondary;
        if (in(rc(kPad + bw + 12, kBtnY, bw, kBtnH))) return HitPrimary;
    } else if (in(rc(kPad, kBtnY, kW - 2 * kPad, kBtnH))) {
        return HitPrimary;
    }
    return HitNone;
}

void SharePanel::click(Hit h) {
    switch (h) {
    case HitClose:
        if (live_ && !expired_ && cb_.onHide) {  // live: the phone keeps getting new captures
            auto cb = cb_.onHide;
            close();
            cb();
            break;
        }
        [[fallthrough]];
    case HitSecondary: {  // ×, 關閉 (expired)
        auto cb = cb_.onStop;
        close();
        if (cb) cb();
        break;
    }
    case HitPrimary:
        if (expired_) {
            if (cb_.onAgain) cb_.onAgain();
        } else {
            auto cb = cb_.onStop;
            close();
            if (cb) cb();
        }
        break;
    case HitCopy:
        if (cb_.onCopy) cb_.onCopy();
        break;
    default: break;
    }
}

void SharePanel::paint() {
    Impl& im = *impl_;
    Factories& f = fx();
    if (!im.rt) {
        RECT r{};
        GetClientRect(hwnd_, &r);
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
        props.dpiX = props.dpiY = static_cast<float>(dpi_);
        if (FAILED(f.d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(r.right, r.bottom)),
                                                 &im.rt)))
            return;
        im.rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &im.brush);
        im.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    if (!im.qr && qrSize_ > 0) {
        D2D1_BITMAP_PROPERTIES bp =
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
        im.rt->CreateBitmap(D2D1::SizeU(qrSize_, qrSize_), qr_.data(), qrSize_ * 4, bp, &im.qr);
    }
    im.rt->BeginDraw();
    draw(im.rt.Get(), im.brush.Get(), im.qr.Get());
    if (im.rt->EndDraw() == D2DERR_RECREATE_TARGET) destroyTarget();
}

bool SharePanel::renderPng(const std::wstring& path) {
    if (!hwnd_ || !impl_) return false;
    return renderToPng(kW, kH, dpi_, path, [this](ID2D1RenderTarget* rt) {
        ComPtr<ID2D1SolidColorBrush> b;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &b);
        ComPtr<ID2D1Bitmap> qr;
        if (qrSize_ > 0) {
            D2D1_BITMAP_PROPERTIES bp =
                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
            rt->CreateBitmap(D2D1::SizeU(qrSize_, qrSize_), qr_.data(), qrSize_ * 4, bp, &qr);
        }
        if (b) draw(rt, b.Get(), qr.Get());
    });
}

void SharePanel::draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* b, ID2D1Bitmap* qrBitmap) {
    Factories& f = fx();
    const Theme& t = impl_->th;
    auto fill = [&](D2D1_RECT_F r, D2D1_COLOR_F c, float radius = 0) {
        b->SetColor(c);
        if (radius > 0) rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b);
        else rt->FillRectangle(r, b);
    };
    auto stroke = [&](D2D1_RECT_F r, D2D1_COLOR_F c, float radius) {
        b->SetColor(c);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b, 1);
    };
    auto text = [&](const std::wstring& str, IDWriteTextFormat* fmt, D2D1_RECT_F r, D2D1_COLOR_F c) {
        b->SetColor(c);
        const std::wstring w = pm::i18n::keepWords(str);  // 한국어: wrap between words
        rt->DrawTextW(w.c_str(), static_cast<UINT32>(w.size()), fmt, r, b);
    };
    cardBackground(rt, b, t, kW, kH, true);
    text(tr(S::SharePanelTitle), f.title.Get(), rc(kPad, 18, kW - 2 * kPad - 40, 32), t.fg);
    {
        const D2D1_RECT_F cr = rc(kW - 52, 12, 36, 36);
        if (hot_ == HitClose) fill(cr, withA(t.accent, 0.22f), 8);
        b->SetColor(hot_ == HitClose ? t.fg : t.dim);
        const float cx = kW - 34, cy = 30, d = 5.5f;
        rt->DrawLine({cx - d, cy - d}, {cx + d, cy + d}, b, 1.6f);
        rt->DrawLine({cx - d, cy + d}, {cx + d, cy - d}, b, 1.6f);
    }
    text(tr(S::SharePanelScan), f.body.Get(), rc(kPad, 60, kW - 2 * kPad, 24), t.fg);
    text(tr(S::SharePanelHint), f.note.Get(), rc(kPad, 86, kW - 2 * kPad, 20), t.dim);

    // QR on a white card (nearest-neighbour, whole multiple, pixel-aligned).
    const float cardX = (kW - kQrCard) / 2;
    fill(rc(cardX, kQrY, kQrCard, kQrCard), D2D1_COLOR_F{1, 1, 1, 1}, 14);
    if (qrBitmap) {
        const float sc = s();
        const int availPx = static_cast<int>(std::floor((kQrCard - 16) * sc));
        const int k = (std::max)(1, availPx / qrSize_);
        const float drawPx = static_cast<float>(k * qrSize_);
        const float leftPx = std::round((cardX + kQrCard / 2) * sc - drawPx / 2);
        const float topPx = std::round((kQrY + kQrCard / 2) * sc - drawPx / 2);
        rt->DrawBitmap(qrBitmap, rc(leftPx / sc, topPx / sc, drawPx / sc, drawPx / sc), expired_ ? 0.12f : 1.0f,
                       D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    }
    if (expired_) {
        ComPtr<IDWriteTextFormat> big = f.count;
        text(tr(S::SharePanelExpired), big.Get(), rc(cardX, kQrY + kQrCard / 2 - 12, kQrCard, 26),
             D2D1_COLOR_F{0.62f, 0.20f, 0.18f, 1});
    }
    text(files_, f.files.Get(), rc(kPad, kFilesY, kW - 2 * kPad, 20), t.fg);
    text(url_, f.url.Get(), rc(kPad - 8, kUrlY, kW - 2 * kPad + 16, 18), withA(t.dim, expired_ ? 0.5f : 1.0f));
    if (!expired_) {
        const std::wstring l = tr(S::SharePanelCopy);
        copyW_ = f.width(f.link.Get(), l);
        text(l, f.link.Get(), rc(kPad, kCopyY, kW - 2 * kPad, kLinkH), hot_ == HitCopy ? t.fg : t.accent);
        b->SetColor(withA(t.accent, hot_ == HitCopy ? 0.9f : 0.45f));
        rt->DrawLine({kW / 2 - copyW_ / 2, kCopyY + kLinkH - 2}, {kW / 2 + copyW_ / 2, kCopyY + kLinkH - 2}, b, 1);
        text(live_ ? pm::i18n::fmt(S::SharePanelLive, {std::to_wstring(liveSent_)}) : countdownText(), f.count.Get(),
             rc(kPad, kCountY, kW - 2 * kPad, 24), t.accent);
    }
    if (!status_.empty()) text(status_, f.status.Get(), rc(kPad, kStatusY, kW - 2 * kPad, 40), t.fg);
    else if (live_ && !expired_) text(tr(S::SharePanelLiveHint), f.status.Get(), rc(kPad, kStatusY, kW - 2 * kPad, 54), t.dim);

    auto button = [&](D2D1_RECT_F r, const wchar_t* label, bool primary, bool hot) {
        if (primary) {
            D2D1_COLOR_F c = t.accent;
            if (hot) c = mix(c, t.light ? D2D1_COLOR_F{0, 0, 0, 1} : D2D1_COLOR_F{1, 1, 1, 1}, 0.15f);
            fill(r, c, 9);
            text(label, f.button.Get(), r, t.onAccent);
        } else {
            fill(r, withA(t.fg, hot ? 0.14f : 0.07f), 9);
            stroke(rc(r.left + 0.5f, r.top + 0.5f, r.right - r.left - 1, kBtnH - 1), withA(t.fg, 0.2f), 9);
            text(label, f.button.Get(), r, t.fg);
        }
    };
    if (expired_) {
        const float bw = (kW - 2 * kPad - 12) / 2;
        button(rc(kPad, kBtnY, bw, kBtnH), tr(S::AboutClose), false, hot_ == HitSecondary);
        button(rc(kPad + bw + 12, kBtnY, bw, kBtnH), tr(S::SharePanelAgain), true, hot_ == HitPrimary);
    } else {
        button(rc(kPad, kBtnY, kW - 2 * kPad, kBtnH), tr(live_ ? S::SharePanelLiveStop : S::SharePanelStop), false,
               hot_ == HitPrimary);
    }
}

LRESULT SharePanel::handle(UINT msg, WPARAM wp, LPARAM lp) {
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
    case WM_TIMER:
        if (wp == kTickTimer && !expired_) {
            if (GetTickCount64() >= deadline_ && deadline_) expired_ = true;  // the server says so too (onExpired)
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
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
        POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        ScreenToClient(h, &pt);
        if (pt.y < 56 * s() && hitTest(pt) == HitNone) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && hot_ != HitNone) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        const Hit nh = hitTest(POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))});
        if (nh != hot_) {
            hot_ = nh;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hot_ != HitNone) {
            hot_ = HitNone;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        pressed_ = hitTest(POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))});
        return 0;
    case WM_LBUTTONUP: {
        const Hit up = hitTest(POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))});
        const Hit was = pressed_;
        pressed_ = HitNone;
        if (up != HitNone && up == was) click(up);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) click(HitClose);
        return 0;
    case WM_CLOSE: click(HitClose); return 0;
    case WM_DESTROY:
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ================================================================= ShareChip ==

struct ShareChip::Impl {
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> brush;
    Theme th = theme();
};

float ShareChip::widthDip() const {
    return 14 + 18 + 8 + std::ceil(fx().width(fx().chip.Get(), tr(S::ShareChip))) + 14 + kChipX;
}

void ShareChip::show(HWND owner, std::function<void()> onClick, int ms) {
    if (!owner || !IsWindowVisible(owner) || IsIconic(owner) || !fx().init()) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = proc<ShareChip>;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
        wc.lpszClassName = kChipClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    onClick_ = std::move(onClick);
    ms_ = ms;
    if (owner_ != owner) hide();
    owner_ = owner;
    if (!hwnd_) {
        impl_ = new Impl;
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kChipClass, tr(S::ShareChip), WS_POPUP, 0, 0, 10, 10,
                                owner, nullptr, GetModuleHandleW(nullptr), this);
        if (!hwnd_) {
            delete impl_;
            impl_ = nullptr;
            return;
        }
        const DWORD round = 2;
        DwmSetWindowAttribute(hwnd_, 33, &round, sizeof(round));
    } else {
        impl_->th = theme();
    }
    hot_ = 0;
    reposition();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    InvalidateRect(hwnd_, nullptr, FALSE);
    SetTimer(hwnd_, kHideTimer, static_cast<UINT>(ms_), nullptr);
}

void ShareChip::hide() {
    if (!hwnd_) return;
    HWND h = hwnd_;
    hwnd_ = nullptr;
    DestroyWindow(h);
    if (impl_) {
        impl_->brush.Reset();
        impl_->rt.Reset();
        delete impl_;
        impl_ = nullptr;
    }
}

// Centred above the bottom toast of the video window (same geometry as
// video/src/renderer.cpp drawToast: size ≈ min(H/24, W/15)·0.56, pill ≈ 2.6·size,
// bottom gap max(20, 5 %)).
void ShareChip::reposition() {
    if (!hwnd_ || !owner_) return;
    if (!IsWindowVisible(owner_) || IsIconic(owner_)) {
        hide();
        return;
    }
    RECT cr{};
    GetClientRect(owner_, &cr);
    POINT tl{0, 0};
    ClientToScreen(owner_, &tl);
    const UINT dpi = GetDpiForWindow(owner_);
    if (dpi != dpi_ && impl_) {
        impl_->rt.Reset();
        impl_->brush.Reset();
    }
    dpi_ = dpi ? dpi : 96;
    const float sc = dpi_ / 96.0f;
    const float W = cr.right / sc, H = cr.bottom / sc;
    const float size = std::clamp((std::min)(H / 24, W / 15) * 0.56f, 12.f, 24.f);
    const float toastH = size * 1.33f + 1.3f * size;
    const float bottom = H - (std::max)(20.f, H * 0.05f) - toastH - 12;
    const float w = widthDip();
    const int px = static_cast<int>(std::lround(w * sc)), ph = static_cast<int>(std::lround(kChipH * sc));
    const int x = tl.x + static_cast<int>(std::lround((W - w) / 2 * sc));
    const int y = tl.y + static_cast<int>(std::lround((bottom - kChipH) * sc));
    SetWindowPos(hwnd_, nullptr, x, y, px, ph, SWP_NOZORDER | SWP_NOACTIVATE);
    if (impl_ && impl_->rt) {
        impl_->rt->Resize(D2D1::SizeU(px, ph));
        impl_->rt->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
    }
}

void ShareChip::retheme() {
    if (!impl_) return;
    impl_->th = theme();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void ShareChip::draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* b, float w, float h, bool hotMain, bool hotClose) {
    Factories& f = fx();
    const Theme& t = impl_ ? impl_->th : theme();
    cardBackground(rt, b, t, w, h, false);
    const float mainW = w - kChipX;
    if (hotMain) {
        b->SetColor(withA(t.accent, 0.22f));
        rt->FillRectangle(rc(1, 1, mainW - 1, h - 2), b);
    }
    if (hotClose) {
        b->SetColor(withA(t.accent, 0.22f));
        rt->FillRectangle(rc(mainW, 1, kChipX - 1, h - 2), b);
    }
    const wchar_t g[2] = {kGlyphShare, 0};
    b->SetColor(t.accent);
    rt->DrawTextW(g, 1, f.glyph.Get(), rc(12, 0, 22, h), b);
    const std::wstring label = tr(S::ShareChip);
    b->SetColor(t.fg);
    rt->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), f.chip.Get(), rc(40, 0, mainW - 40, h), b);
    b->SetColor(withA(t.fg, 0.18f));
    rt->DrawLine({mainW, 9}, {mainW, h - 9}, b, 1);
    b->SetColor(hotClose ? t.fg : t.dim);
    const float cx = mainW + kChipX / 2, cy = h / 2, d = 4.5f;
    rt->DrawLine({cx - d, cy - d}, {cx + d, cy + d}, b, 1.4f);
    rt->DrawLine({cx - d, cy + d}, {cx + d, cy - d}, b, 1.4f);
}

void ShareChip::paint() {
    Impl& im = *impl_;
    Factories& f = fx();
    RECT r{};
    GetClientRect(hwnd_, &r);
    if (!im.rt) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
        props.dpiX = props.dpiY = static_cast<float>(dpi_);
        if (FAILED(f.d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(r.right, r.bottom)),
                                                 &im.rt)))
            return;
        im.rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &im.brush);
        im.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    const float sc = dpi_ / 96.0f;
    im.rt->BeginDraw();
    draw(im.rt.Get(), im.brush.Get(), r.right / sc, r.bottom / sc, hot_ == 1, hot_ == 2);
    if (im.rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        im.brush.Reset();
        im.rt.Reset();
    }
}

bool ShareChip::renderPng(const std::wstring& path) {
    if (!fx().init()) return false;
    const float w = widthDip();
    return renderToPng(w, kChipH, dpi_, path, [this, w](ID2D1RenderTarget* rt) {
        ComPtr<ID2D1SolidColorBrush> b;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &b);
        if (b) draw(rt, b.Get(), w, kChipH, true, false);
    });
}

LRESULT ShareChip::handle(UINT msg, WPARAM wp, LPARAM lp) {
    HWND h = hwnd_;
    auto hitAt = [&](LPARAM l) {
        RECT r{};
        GetClientRect(h, &r);
        const int x = static_cast<short>(LOWORD(l));
        return x >= r.right - static_cast<int>(kChipX * dpi_ / 96.0f) ? 2 : 1;
    };
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        if (impl_) paint();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_TIMER:
        if (wp == kHideTimer) hide();
        return 0;
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
            KillTimer(h, kHideTimer);  // stays while the cursor is on it
        }
        const int nh = hitAt(lp);
        if (nh != hot_) {
            hot_ = nh;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        hot_ = 0;
        InvalidateRect(h, nullptr, FALSE);
        SetTimer(h, kHideTimer, 2500, nullptr);
        return 0;
    case WM_LBUTTONUP: {
        const int hit = hitAt(lp);
        auto cb = onClick_;
        hide();
        if (hit == 1 && cb) cb();
        return 0;
    }
    case WM_DESTROY:
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace pm::ui
