// 「有新版本 / A new version」 dialog (see update_panel.h, docs/app.md
// "Offer UX"). Built like the About panel: an owned WS_POPUP drawn with a
// Direct2D HWND render target in the popup menus' palette, Windows 11 rounded
// corners + drop shadow, draggable by its top, per-monitor DPI. Layout in
// DIPs; the height follows the length of the change list (scrolls past ~250).
#include "update_panel.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

#include "pm/i18n.h"
#include "popup_menu.h"
#include "ui_anim.h"

using Microsoft::WRL::ComPtr;
using pm::i18n::fmt;
using pm::i18n::S;
using pm::i18n::tr;

namespace pm::ui {
namespace {

constexpr wchar_t kClass[] = L"PhoneMirrorUpdatePanel";
constexpr float kW = 460, kPad = 26;
constexpr float kHeadH = 92;  // draggable top (icon + title + versions)
constexpr float kIcon = 48;
constexpr float kListPadY = 12, kBulletX = 14, kTextX = 30, kListPadR = 18, kItemGap = 7;
constexpr float kListMin = 56, kListMax = 250;
constexpr float kBtnH = 38;

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
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IDWriteTextFormat> title, versions, meta, label, item, foot, link, button;
    int lang = -1;
    bool init() {
        if (!d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return false;
        if (!dw && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                              reinterpret_cast<IUnknown**>(dw.GetAddressOf()))))
            return false;
        if (!wic) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        if (lang == static_cast<int>(pm::i18n::lang()) && title) return true;
        lang = static_cast<int>(pm::i18n::lang());
        auto make = [&](float size, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT a, bool wrap) {
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
        title = make(19, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        versions = make(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        meta = make(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        label = make(12.5f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        item = make(13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        foot = make(12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        link = make(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        button = make(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false);
        if (button) button->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (link) link->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        return title && versions && meta && label && item && foot && link && button;
    }
    ComPtr<IDWriteTextLayout> layout(IDWriteTextFormat* f, const std::wstring& s0, float w) {
        ComPtr<IDWriteTextLayout> l;
        const std::wstring s = pm::i18n::keepWords(s0);  // 한국어: wrap between words (0.7.2)
        if (f) dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, w, 4096, &l);
        return l;
    }
    float width(IDWriteTextFormat* f, const std::wstring& s) {
        ComPtr<IDWriteTextLayout> l = layout(f, s, 4096);
        DWRITE_TEXT_METRICS m{};
        if (l) l->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    }
    float height(IDWriteTextLayout* l) {
        DWRITE_TEXT_METRICS m{};
        if (l) l->GetMetrics(&m);
        return m.height;
    }
};
Factories& fx() {
    static Factories f;
    return f;
}

LRESULT CALLBACK updateProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* p = reinterpret_cast<UpdatePanel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (p && p->hwnd() == h) return p->handle(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

// 13569777 -> "13.6 MB" (KB below 1 MB).
std::wstring sizeText(unsigned long long b) {
    wchar_t buf[32];
    if (b >= 1024 * 1024) swprintf_s(buf, L"%.1f MB", b / (1024.0 * 1024.0));
    else swprintf_s(buf, L"%llu KB", (b + 1023) / 1024);
    return buf;
}

}  // namespace

struct UpdatePanel::Impl {
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1Bitmap> icon;
    std::vector<ComPtr<IDWriteTextLayout>> items;  // one per items_ entry (list width)
    ComPtr<IDWriteTextLayout> foot;
    Theme th = theme();
    HoverAnim anim;  // hover / press levels per Hit
    PanelFade fade;  // open: fade (+ grow) in
};

static ComPtr<ID2D1Bitmap> iconBitmapFor(ID2D1RenderTarget* rt, HICON icon) {
    Factories& f = fx();
    ComPtr<ID2D1Bitmap> bmp;
    if (!icon || !f.wic) return bmp;
    ComPtr<IWICBitmap> wb;
    ComPtr<IWICFormatConverter> conv;
    if (SUCCEEDED(f.wic->CreateBitmapFromHICON(icon, &wb)) && SUCCEEDED(f.wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(wb.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeMedianCut)))
        rt->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp);
    return bmp;
}

void UpdatePanel::layout() {
    Factories& f = fx();
    f.init();
    // The change list in the UI language (日本語 / 한국어: else English); else
    // the old one-line notes; else the other list; else a generic sentence.
    const pm::i18n::Lang ui = pm::i18n::lang();
    const bool zh = ui == pm::i18n::Lang::ZhTW;
    const std::vector<std::wstring>& own = ui == pm::i18n::Lang::Ja   ? info_.changesJa
                                           : ui == pm::i18n::Lang::Ko ? info_.changesKo
                                           : zh                       ? info_.changesZh
                                                                      : info_.changesEn;
    const std::vector<std::wstring>& mine = own.empty() && !zh ? info_.changesEn : own;
    const std::vector<std::wstring>& other = zh ? info_.changesEn : info_.changesZh;
    plain_ = false;
    if (!mine.empty()) items_ = mine;
    else if (!info_.notes.empty()) items_ = {info_.notes}, plain_ = true;
    else if (!other.empty()) items_ = other;
    else items_ = {tr(info_.local ? S::UpdDlgLocal : S::UpdDlgGeneric)}, plain_ = true;

    const float listW = kW - 2 * kPad;
    const float textW = plain_ ? listW - kBulletX - kListPadR : listW - kTextX - kListPadR;
    impl_->items.clear();
    itemH_.clear();
    contentH_ = 0;
    for (size_t i = 0; i < items_.size(); ++i) {
        impl_->items.push_back(f.layout(f.item.Get(), items_[i], textW));
        const float h = f.height(impl_->items.back().Get());
        itemH_.push_back(h);
        contentH_ += h + (i ? kItemGap : 0);
    }
    const bool meta = !info_.date.empty() || info_.size > 0;
    const float labelY = meta ? 126 : 100;
    listY_ = labelY + 24;
    listH_ = std::clamp(contentH_ + 2 * kListPadY, kListMin, kListMax);
    scroll_ = std::clamp(scroll_, 0.0f, (std::max)(0.0f, contentH_ + 2 * kListPadY - listH_));
    footY_ = listY_ + listH_ + 12;
    impl_->foot = f.layout(f.foot.Get(), fmt(S::UpdDlgFoot, {tr(S::AppName)}), kW - 2 * kPad);
    btnY_ = footY_ + f.height(impl_->foot.Get()) + 16;
    h_ = std::ceil(btnY_ + kBtnH + 22);
    installW_ = (std::max)(112.0f, f.width(f.button.Get(), tr(S::UpdDlgInstall)) + 36);
    laterW_ = (std::max)(100.0f, f.width(f.button.Get(), tr(S::UpdDlgLater)) + 32);
    skipW_ = f.width(f.link.Get(), tr(S::UpdDlgSkip));
}

bool UpdatePanel::open(HWND owner, Info info, bool activate) {
    if (!fx().init()) return false;
    const bool wasOpen = hwnd_ != nullptr;
    info_ = std::move(info);
    scroll_ = 0;
    focus_ = 0;
    if (wasOpen) {  // new offer while open: new content, same place
        layout();
        RECT r{};
        GetWindowRect(hwnd_, &r);
        SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, static_cast<int>(std::lround(h_ * s())),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd_, nullptr, FALSE);
        ShowWindow(hwnd_, activate && !testOffscreen ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
        if (activate && !testOffscreen) SetForegroundWindow(hwnd_);
        return true;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = updateProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    owner_ = owner;
    hot_ = pressed_ = HitNone;
    focusCues_ = false;
    impl_ = new Impl;
    layout();

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
        if (auto fn = reinterpret_cast<Fn>(GetProcAddress(shcore, "GetDpiForMonitor"))) fn(mon, 0, &dx, &dy);
    }
    dpi_ = dx;
    const int w = static_cast<int>(std::lround(kW * s())), hgt = static_cast<int>(std::lround(h_ * s()));
    int x = (orc.left + orc.right) / 2 - w / 2, y = (orc.top + orc.bottom) / 2 - hgt / 2;
    if (!testOffscreen) {
        x = (std::max)(static_cast<int>(mi.rcWork.left), (std::min)(x, static_cast<int>(mi.rcWork.right) - w));
        y = (std::max)(static_cast<int>(mi.rcWork.top), (std::min)(y, static_cast<int>(mi.rcWork.bottom) - hgt));
    } else if (!ownerVisible) {  // scripted test with the app in the "tray": stay off the desktop too
        x = -12000;
        y = 200;
    }
    const std::wstring title = fmt(S::UpdDlgTitle, {tr(S::AppName)});
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
    const bool act = activate && !testOffscreen;
    impl_->fade.begin(hwnd_);
    ShowWindow(hwnd_, act ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
    if (act) SetForegroundWindow(hwnd_);
    return true;
}

void UpdatePanel::close() {
    if (!hwnd_) return;
    prepareCloseFade(hwnd_);  // the picture the close fade shows
    HWND h = hwnd_;
    const bool hadFocus = GetForegroundWindow() == h;
    hwnd_ = nullptr;
    closeWithFade(h);
    destroyTarget();
    delete impl_;
    impl_ = nullptr;
    if (hadFocus && owner_ && IsWindowVisible(owner_) && !testOffscreen) SetForegroundWindow(owner_);
}

void UpdatePanel::destroyTarget() {
    if (!impl_) return;
    impl_->icon.Reset();
    impl_->brush.Reset();
    impl_->rt.Reset();
}

void UpdatePanel::retheme() {
    if (!impl_) return;
    impl_->th = theme();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void UpdatePanel::relabel() {
    if (!hwnd_) return;
    fx().init();
    layout();
    SetWindowTextW(hwnd_, fmt(S::UpdDlgTitle, {tr(S::AppName)}).c_str());
    RECT r{};
    GetWindowRect(hwnd_, &r);
    SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, static_cast<int>(std::lround(h_ * s())),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

UpdatePanel::Hit UpdatePanel::hitTest(POINT pt) const {
    const float x = pt.x / s(), y = pt.y / s();
    auto in = [&](D2D1_RECT_F r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
    if (in(rc(kW - 52, 12, 36, 36))) return HitClose;
    if (in(rc(kW - kPad - installW_, btnY_, installW_, kBtnH))) return HitInstall;
    if (in(rc(kW - kPad - installW_ - 10 - laterW_, btnY_, laterW_, kBtnH))) return HitLater;
    if (in(rc(kPad - 6, btnY_, skipW_ + 12, kBtnH))) return HitSkip;
    return HitNone;
}

void UpdatePanel::click(Hit h) {
    std::function<void()> cb;
    switch (h) {
    case HitInstall: cb = info_.onInstall; break;
    case HitClose:
    case HitLater: cb = info_.onLater; break;
    case HitSkip: cb = info_.onSkip; break;
    default: return;
    }
    close();
    if (cb) cb();
}

void UpdatePanel::scrollBy(float dip) {
    const float maxS = (std::max)(0.0f, contentH_ + 2 * kListPadY - listH_);
    const float n = std::clamp(scroll_ + dip, 0.0f, maxS);
    if (n != scroll_) {
        scroll_ = n;
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void UpdatePanel::paint() {
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
    if (!im.icon) im.icon = iconBitmapFor(im.rt.Get(), info_.icon);
    im.rt->BeginDraw();
    if (const float k = im.fade.scale(); k < 1) {  // opening: grows from 97 % about the centre
        const D2D1_SIZE_F sz = im.rt->GetSize();
        im.rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k, {sz.width / 2, sz.height / 2}));
    }
    draw(im.rt.Get(), im.brush.Get(), im.icon.Get());
    im.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    if (im.rt->EndDraw() == D2DERR_RECREATE_TARGET) destroyTarget();
    im.fade.painted();
}

bool UpdatePanel::renderPng(const std::wstring& path) {
    if (!hwnd_ || !impl_) return false;
    return renderToPng(kW, h_, dpi_, path, [this](ID2D1RenderTarget* rt) {
        ComPtr<ID2D1SolidColorBrush> b;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &b);
        ComPtr<ID2D1Bitmap> icon = iconBitmapFor(rt, info_.icon);
        if (b) draw(rt, b.Get(), icon.Get());
    });
}

void UpdatePanel::draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* b, ID2D1Bitmap* icon) {
    Impl& im = *impl_;
    Factories& f = fx();
    const Theme& t = im.th;
    auto hl = [&](Hit x) { return im.anim.hot(x); };
    D2D1_MATRIX_3X2_F base;
    rt->GetTransform(&base);
    auto pressAt = [&](Hit x, D2D1_RECT_F r) {  // a pressed button shrinks to 96 %
        const float k = im.anim.pressScale(x);
        rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k, {(r.left + r.right) / 2, (r.top + r.bottom) / 2}) *
                         *D2D1::Matrix3x2F::ReinterpretBaseType(&base));
    };
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
        rt->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmtx, r, b);
    };

    // Card: vertical gradient, accent bar along the top, hairline border.
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
    // Icon, 「自在投影有新版本」, 「目前 v0.6.2 → 新版 v0.7.0」; close ×.
    if (icon) rt->DrawBitmap(icon, rc(kPad, 26, kIcon, kIcon), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    const float tx = kPad + kIcon + 14;
    text(fmt(S::UpdDlgTitle, {tr(S::AppName)}), f.title.Get(), rc(tx, 25, kW - tx - 56, 28), t.fg);
    text(fmt(S::UpdDlgVersions, {info_.current, info_.version}), f.versions.Get(), rc(tx, 55, kW - tx - kPad, 22),
         t.accent);
    {
        const D2D1_RECT_F cr = rc(kW - 52, 12, 36, 36);
        if (const float h = hl(HitClose); h > 0.003f) fill(cr, withA(t.accent, 0.22f * h), 8);
        b->SetColor(animMix(t.dim, t.fg, hl(HitClose)));
        const float cx = kW - 34, cy = 30, d = 5.5f;
        rt->DrawLine({cx - d, cy - d}, {cx + d, cy + d}, b, 1.6f);
        rt->DrawLine({cx - d, cy + d}, {cx + d, cy - d}, b, 1.6f);
    }
    // 發布日期 · 下載大小
    {
        std::wstring meta;
        if (!info_.date.empty()) meta = fmt(S::UpdDlgDate, {info_.date});
        if (info_.size > 0) {
            if (!meta.empty()) meta += L"   ·   ";
            meta += fmt(info_.local ? S::UpdDlgSizeLocal : S::UpdDlgSize, {sizeText(info_.size)});
        }
        if (!meta.empty()) {
            b->SetColor(withA(t.fg, 0.10f));
            rt->DrawLine({kPad, 90}, {kW - kPad, 90}, b, 1);
            text(meta, f.meta.Get(), rc(kPad, 98, kW - 2 * kPad, 20), t.dim);
        }
    }
    // 這次更新了什麼: a rounded well with the bullets, clipped and scrolled.
    text(tr(S::UpdDlgChanges), f.label.Get(), rc(kPad, listY_ - 24, kW - 2 * kPad, 20), t.dim);
    const D2D1_RECT_F well = rc(kPad, listY_, kW - 2 * kPad, listH_);
    fill(well, withA(t.fg, t.light ? 0.05f : 0.06f), 10);
    stroke(rc(well.left + 0.5f, well.top + 0.5f, kW - 2 * kPad - 1, listH_ - 1), withA(t.fg, 0.10f), 10);
    rt->PushAxisAlignedClip(rc(kPad + 1, listY_ + 1, kW - 2 * kPad - 2, listH_ - 2), D2D1_ANTIALIAS_MODE_ALIASED);
    {
        float y = listY_ + kListPadY - scroll_;
        for (size_t i = 0; i < items_.size() && i < im.items.size(); ++i) {
            if (y + itemH_[i] > listY_ && y < listY_ + listH_) {
                const float x = kPad + (plain_ ? kBulletX : kTextX);
                if (!plain_) {  // bullet dot centred on the first line
                    b->SetColor(t.accent);
                    rt->FillEllipse(D2D1::Ellipse({kPad + kBulletX + 4, y + 10}, 3, 3), b);
                }
                b->SetColor(t.fg);
                if (im.items[i]) rt->DrawTextLayout({x, y}, im.items[i].Get(), b);
            }
            y += itemH_[i] + kItemGap;
        }
    }
    rt->PopAxisAlignedClip();
    if (const float total = contentH_ + 2 * kListPadY; total > listH_ + 0.5f) {  // scroll thumb
        const float trackY = listY_ + 6, trackH = listH_ - 12;
        const float th = (std::max)(24.0f, trackH * listH_ / total);
        const float ty = trackY + (trackH - th) * (scroll_ / (total - listH_));
        fill(rc(kW - kPad - 8, ty, 4, th), withA(t.fg, 0.30f), 2);
    }
    // Footnote: the app closes and comes back.
    if (im.foot) {
        b->SetColor(t.dim);
        rt->DrawTextLayout({kPad, footY_}, im.foot.Get(), b);
    }
    // Buttons: 略過這個版本 (link, left) · 稍後提醒 · 立即更新 (primary).
    auto button = [&](D2D1_RECT_F r, const std::wstring& label, bool primary, Hit id, bool focused) {
        const float w = r.right - r.left, hot = hl(id);
        pressAt(id, r);
        if (primary) {
            D2D1_COLOR_F c = t.accent;
            c = mix(c, t.light ? D2D1_COLOR_F{0, 0, 0, 1} : D2D1_COLOR_F{1, 1, 1, 1}, 0.15f * hot);
            fill(r, c, 9);
            text(label, f.button.Get(), r, t.onAccent);
        } else {
            fill(r, withA(t.fg, 0.07f + 0.07f * hot), 9);
            stroke(rc(r.left + 0.5f, r.top + 0.5f, w - 1, kBtnH - 1), withA(t.fg, 0.2f), 9);
            text(label, f.button.Get(), r, t.fg);
        }
        rt->SetTransform(base);
        if (focused) stroke(rc(r.left - 3, r.top - 3, w + 6, kBtnH + 6), withA(t.fg, 0.85f), 11, 1.5f);
    };
    const float ix = kW - kPad - installW_, lx = ix - 10 - laterW_;
    button(rc(ix, btnY_, installW_, kBtnH), tr(S::UpdDlgInstall), true, HitInstall, focusCues_ && focus_ == 0);
    button(rc(lx, btnY_, laterW_, kBtnH), tr(S::UpdDlgLater), false, HitLater, focusCues_ && focus_ == 1);
    {
        const float hot = hl(HitSkip);
        const D2D1_RECT_F r = rc(kPad, btnY_, skipW_, kBtnH);
        text(tr(S::UpdDlgSkip), f.link.Get(), r, animMix(t.dim, t.fg, hot));
        b->SetColor(withA(animMix(t.dim, t.fg, hot), 0.4f + 0.4f * hot));
        const float uy = btnY_ + kBtnH / 2 + 10;
        rt->DrawLine({kPad, uy}, {kPad + skipW_, uy}, b, 1);
        if (focusCues_ && focus_ == 2) stroke(rc(kPad - 6, btnY_ + 3, skipW_ + 12, kBtnH - 6), withA(t.fg, 0.85f), 7, 1.5f);
    }
}

LRESULT UpdatePanel::handle(UINT msg, WPARAM wp, LPARAM lp) {
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
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
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
        const Hit nh = hitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (nh != hot_) {
            hot_ = nh;
            if (impl_) impl_->anim.setHot(h, nh);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hot_ != HitNone) {
            hot_ = HitNone;
            if (impl_) impl_->anim.setHot(h, HitNone);
        }
        if (pressed_ != HitNone && impl_) impl_->anim.setPressed(h, HitNone);
        return 0;
    case WM_MOUSEWHEEL:
        scrollBy(-GET_WHEEL_DELTA_WPARAM(wp) / static_cast<float>(WHEEL_DELTA) * 48);
        return 0;
    case WM_TIMER:
        if (impl_ && (impl_->anim.onTimer(h, wp) || impl_->fade.onTimer(h, wp))) return 0;
        break;
    case WM_LBUTTONDOWN:
        pressed_ = hitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (impl_) impl_->anim.setPressed(h, pressed_);
        return 0;
    case WM_LBUTTONUP: {
        const Hit up = hitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        const Hit was = pressed_;
        pressed_ = HitNone;
        if (impl_) impl_->anim.setPressed(h, HitNone);
        if (up != HitNone && up == was) click(up);
        return 0;
    }
    case WM_KEYDOWN:
        switch (wp) {
        case VK_ESCAPE: click(HitLater); return 0;
        case VK_RETURN:
        case VK_SPACE: click(focus_ == 1 ? HitLater : focus_ == 2 ? HitSkip : HitInstall); return 0;
        case VK_TAB:
            focus_ = (focus_ + (GetKeyState(VK_SHIFT) < 0 ? 2 : 1)) % 3;
            focusCues_ = true;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case VK_LEFT:
        case VK_RIGHT:
            focus_ = (focus_ + (wp == VK_LEFT ? 1 : 2)) % 3;  // left = towards 略過
            focusCues_ = true;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case VK_UP: scrollBy(-32); return 0;
        case VK_DOWN: scrollBy(32); return 0;
        case VK_PRIOR: scrollBy(-(listH_ - 24)); return 0;
        case VK_NEXT: scrollBy(listH_ - 24); return 0;
        case VK_HOME: scrollBy(-1e6f); return 0;
        case VK_END: scrollBy(1e6f); return 0;
        }
        return 0;
    case WM_CLOSE: click(HitLater); return 0;
    case WM_DESTROY:
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace pm::ui
