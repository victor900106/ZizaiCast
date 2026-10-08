// 「關於自在投影 / About Zizai Cast」 window (see about_panel.h, docs/app.md
// "About"). Built like the pairing panel: an owned WS_POPUP drawn with a
// Direct2D HWND render target in the popup menus' palette, Windows 11 rounded
// corners + drop shadow, draggable by its top, per-monitor DPI. Layout in DIPs.
#include "about_panel.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <wincodec.h>
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

constexpr wchar_t kClass[] = L"PhoneMirrorAboutPanel";
constexpr float kW = 420, kH = 520;  // panel size (DIPs; 0.7.0: taller for the longer credits)
constexpr float kPad = 26;
constexpr float kHeadH = 92;         // draggable top (icon + name)
constexpr float kIcon = 52;
constexpr float kTagY = 100, kLicY = 136;
constexpr float kRepoLabelY = 196, kRepoY = 216, kLinkH = 24, kLicLinkY = 250;
constexpr float kCreditsTitleY = 296, kCreditsY = 318, kMascotY = 414;  // credits: up to 4 lines
constexpr float kBtnW = 132, kBtnH = 38, kBtnY = kH - 22 - kBtnH;

D2D1_COLOR_F rgb(uint32_t c, float a = 1) {
    return {((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, a};
}
D2D1_COLOR_F withA(D2D1_COLOR_F c, float a) {
    c.a = a;
    return c;
}
D2D1_RECT_F rc(float x, float y, float w, float h) { return {x, y, x + w, y + h}; }

struct Theme {
    D2D1_COLOR_F top, bottom, fg, dim, accent;
};
Theme theme() {
    const Colors c = currentColors();
    return {rgb(c.cardTop), rgb(c.cardBottom), rgb(c.fg), rgb(c.dim), rgb(c.accent)};
}

struct Factories {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IDWriteTextFormat> title, version, body, note, noteBold, link, button;
    int lang = -1;
    bool init() {
        if (!d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return false;
        if (!dw && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                              reinterpret_cast<IUnknown**>(dw.GetAddressOf()))))
            return false;
        if (!wic) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        if (lang == static_cast<int>(pm::i18n::lang()) && title) return true;
        lang = static_cast<int>(pm::i18n::lang());
        auto fmt = [&](float size, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT a, bool wrap) {
            ComPtr<IDWriteTextFormat> f;
            dw->CreateTextFormat(pm::i18n::uiFont(), nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                 size, pm::i18n::localeName(), &f);
            if (f) {
                f->SetTextAlignment(a);
                f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
            }
            return f;
        };
        title = fmt(21, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        version = fmt(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        body = fmt(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        note = fmt(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        noteBold = fmt(12.5f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        link = fmt(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        button = fmt(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false);
        if (button) button->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        return title && version && body && note && noteBold && link && button;
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

LRESULT CALLBACK aboutProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* p = reinterpret_cast<AboutPanel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (p && p->hwnd() == h) return p->handle(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

// "https://github.com/x/y" -> "github.com/x/y" (shorter link text).
std::wstring shortUrl(const std::wstring& u) {
    const size_t p = u.find(L"://");
    return p == std::wstring::npos ? u : u.substr(p + 3);
}

}  // namespace

struct AboutPanel::Impl {
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1Bitmap> icon;
    Theme th = theme();
};

bool AboutPanel::open(HWND owner, Info info) {
    if (hwnd_) {
        ShowWindow(hwnd_, testOffscreen ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
        if (!testOffscreen) SetForegroundWindow(hwnd_);
        return true;
    }
    if (!fx().init()) return false;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = aboutProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    owner_ = owner;
    info_ = std::move(info);
    hot_ = pressed_ = HitNone;
    impl_ = new Impl;

    // Centre over the owner (or its monitor when hidden), at that monitor's DPI.
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
        if (auto f = reinterpret_cast<Fn>(GetProcAddress(shcore, "GetDpiForMonitor"))) f(mon, 0, &dx, &dy);
    }
    dpi_ = dx;
    const int w = static_cast<int>(std::lround(kW * s())), hgt = static_cast<int>(std::lround(kH * s()));
    int x = (orc.left + orc.right) / 2 - w / 2, y = (orc.top + orc.bottom) / 2 - hgt / 2;
    if (!testOffscreen) {
        x = (std::max)(static_cast<int>(mi.rcWork.left), (std::min)(x, static_cast<int>(mi.rcWork.right) - w));
        y = (std::max)(static_cast<int>(mi.rcWork.top), (std::min)(y, static_cast<int>(mi.rcWork.bottom) - hgt));
    }
    const std::wstring title = pm::i18n::fmt(S::MenuAbout, {tr(S::AppName)});
    hwnd_ = CreateWindowExW(0, kClass, title.c_str(), WS_POPUP | WS_SYSMENU, x, y, w, hgt, owner, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    dpi_ = GetDpiForWindow(hwnd_);
    const DWORD round = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &round, sizeof(round));
    ShowWindow(hwnd_, testOffscreen ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
    if (!testOffscreen) SetForegroundWindow(hwnd_);
    return true;
}

void AboutPanel::close() {
    if (!hwnd_) return;
    HWND h = hwnd_;
    hwnd_ = nullptr;
    DestroyWindow(h);
    destroyTarget();
    delete impl_;
    impl_ = nullptr;
    if (owner_ && IsWindowVisible(owner_) && !testOffscreen) SetForegroundWindow(owner_);
}

void AboutPanel::destroyTarget() {
    if (!impl_) return;
    impl_->icon.Reset();
    impl_->brush.Reset();
    impl_->rt.Reset();
}

void AboutPanel::retheme() {
    if (!impl_) return;
    impl_->th = theme();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void AboutPanel::relabel() {
    if (!hwnd_) return;
    fx().init();
    SetWindowTextW(hwnd_, pm::i18n::fmt(S::MenuAbout, {tr(S::AppName)}).c_str());
    InvalidateRect(hwnd_, nullptr, FALSE);
}

AboutPanel::Hit AboutPanel::hitTest(POINT pt) const {
    const float x = pt.x / s(), y = pt.y / s();
    auto in = [&](D2D1_RECT_F r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
    if (in(rc(kW - 52, 12, 36, 36))) return HitClose;
    if (in(rc(kPad - 4, kRepoY - 2, repoW_ + 8, kLinkH + 4))) return HitRepo;
    if (in(rc(kPad - 4, kLicLinkY - 2, licW_ + 8, kLinkH + 4))) return HitLicenses;
    if (in(rc((kW - kBtnW) / 2, kBtnY, kBtnW, kBtnH))) return HitButton;
    return HitNone;
}

void AboutPanel::click(Hit h) {
    switch (h) {
    case HitClose:
    case HitButton: close(); break;
    case HitRepo:
        if (info_.onOpenUrl) info_.onOpenUrl(info_.repoUrl);
        break;
    case HitLicenses:
        if (info_.onOpenLicenses) info_.onOpenLicenses();
        break;
    default: break;
    }
}

void AboutPanel::paint() {
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
    if (!im.icon) im.icon = iconBitmap(im.rt.Get());
    im.rt->BeginDraw();
    draw(im.rt.Get(), im.brush.Get(), im.icon.Get());
    if (im.rt->EndDraw() == D2DERR_RECREATE_TARGET) destroyTarget();
}

ComPtr<ID2D1Bitmap> AboutPanel::iconBitmap(ID2D1RenderTarget* rt) {
    Factories& f = fx();
    ComPtr<ID2D1Bitmap> bmp;
    if (!info_.icon || !f.wic) return bmp;
    ComPtr<IWICBitmap> wb;
    ComPtr<IWICFormatConverter> conv;
    if (SUCCEEDED(f.wic->CreateBitmapFromHICON(info_.icon, &wb)) && SUCCEEDED(f.wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(wb.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeMedianCut)))
        rt->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp);
    return bmp;
}

// --dev test hook: the panel as it looks now, drawn into a PNG.
bool AboutPanel::renderPng(const std::wstring& path) {
    if (!hwnd_ || !impl_) return false;
    return renderToPng(kW, kH, dpi_, path, [this](ID2D1RenderTarget* rt) {
        ComPtr<ID2D1SolidColorBrush> b;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &b);
        ComPtr<ID2D1Bitmap> icon = iconBitmap(rt);
        if (b) draw(rt, b.Get(), icon.Get());
    });
}

void AboutPanel::draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* b, ID2D1Bitmap* icon) {
    Impl& im = *impl_;
    Factories& f = fx();
    const Theme& t = im.th;
    auto fill = [&](D2D1_RECT_F r, D2D1_COLOR_F c, float radius = 0) {
        b->SetColor(c);
        if (radius > 0) rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b);
        else rt->FillRectangle(r, b);
    };
    auto text = [&](const std::wstring& s, IDWriteTextFormat* fmt, D2D1_RECT_F r, D2D1_COLOR_F c) {
        b->SetColor(c);
        const std::wstring w = pm::i18n::keepWords(s);  // 한국어: wrap between words
        rt->DrawTextW(w.c_str(), static_cast<UINT32>(w.size()), fmt, r, b);
    };
    auto link = [&](const std::wstring& s, float y, float& w, bool hot) {
        w = (std::min)(f.width(f.link.Get(), s), kW - 2 * kPad);
        text(s, f.link.Get(), rc(kPad, y, kW - 2 * kPad, kLinkH), hot ? t.fg : t.accent);
        b->SetColor(withA(t.accent, hot ? 0.9f : 0.45f));
        rt->DrawLine({kPad, y + kLinkH - 2}, {kPad + w, y + kLinkH - 2}, b, 1);
    };

    {
        ComPtr<ID2D1GradientStopCollection> stops;
        D2D1_GRADIENT_STOP gs[2] = {{0, t.top}, {1, t.bottom}};
        rt->CreateGradientStopCollection(gs, 2, &stops);
        ComPtr<ID2D1LinearGradientBrush> g;
        if (stops) rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {0, kH}), stops.Get(), &g);
        if (g) rt->FillRectangle(rc(0, 0, kW, kH), g.Get());
        else rt->Clear(t.top);
        fill(rc(0, 0, kW, 3), withA(t.accent, 0.85f));
        b->SetColor(withA(t.accent, 0.35f));
        rt->DrawRectangle(rc(0.5f, 0.5f, kW - 1, kH - 1), b, 1);
    }
    // Icon, name, version; close ×.
    if (icon) rt->DrawBitmap(icon, rc(kPad, 26, kIcon, kIcon), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    const float nameX = kPad + kIcon + 16;
    text(tr(S::AppName), f.title.Get(), rc(nameX, 28, kW - nameX - 56, 30), t.fg);
    text(pm::i18n::fmt(S::Version, {info_.version}) + L"  ·  GPL-3.0", f.version.Get(), rc(nameX, 60, kW - nameX - 24, 20),
         t.dim);
    {
        const D2D1_RECT_F cr = rc(kW - 52, 12, 36, 36);
        if (hot_ == HitClose) fill(cr, withA(t.accent, 0.22f), 8);
        b->SetColor(hot_ == HitClose ? t.fg : t.dim);
        const float cx = kW - 34, cy = 30, d = 5.5f;
        rt->DrawLine({cx - d, cy - d}, {cx + d, cy + d}, b, 1.6f);
        rt->DrawLine({cx - d, cy + d}, {cx + d, cy - d}, b, 1.6f);
    }
    const float tw = kW - 2 * kPad;
    text(tr(S::AboutTagline), f.body.Get(), rc(kPad, kTagY, tw, 36), t.fg);
    text(tr(S::AboutLicense), f.note.Get(), rc(kPad, kLicY, tw, 54), t.dim);
    text(tr(S::AboutSource), f.noteBold.Get(), rc(kPad, kRepoLabelY, tw, 20), t.dim);
    link(shortUrl(info_.repoUrl), kRepoY, repoW_, hot_ == HitRepo);
    link(tr(S::AboutLicenses), kLicLinkY, licW_, hot_ == HitLicenses);
    text(tr(S::AboutCreditsTitle), f.noteBold.Get(), rc(kPad, kCreditsTitleY, tw, 20), t.dim);
    text(tr(S::AboutCredits), f.note.Get(), rc(kPad, kCreditsY, tw, kMascotY - kCreditsY), t.dim);
    text(tr(S::AboutMascot), f.note.Get(), rc(kPad, kMascotY, tw, 36), t.dim);
    // Close button.
    {
        const D2D1_RECT_F r = rc((kW - kBtnW) / 2, kBtnY, kBtnW, kBtnH);
        fill(r, withA(t.fg, hot_ == HitButton ? 0.14f : 0.07f), 9);
        b->SetColor(withA(t.fg, 0.2f));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(rc(r.left + 0.5f, r.top + 0.5f, kBtnW - 1, kBtnH - 1), 9, 9), b, 1);
        text(tr(S::AboutClose), f.button.Get(), r, t.fg);
    }
}

LRESULT AboutPanel::handle(UINT msg, WPARAM wp, LPARAM lp) {
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
        if (pt.y < kHeadH * s() && hitTest(pt) == HitNone) return HTCAPTION;
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
        if (wp == VK_ESCAPE || wp == VK_RETURN || wp == VK_SPACE) close();
        return 0;
    case WM_CLOSE: close(); return 0;
    case WM_DESTROY:
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace pm::ui
