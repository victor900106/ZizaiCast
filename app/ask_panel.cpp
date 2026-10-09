// Themed question / message dialog (see ask_panel.h). Same construction as
// update_panel.cpp: owned WS_POPUP, Direct2D HWND render target in the popup
// menus' palette, Windows 11 rounded corners + shadow, layout in DIPs.
#include "ask_panel.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

#include "pm/i18n.h"
#include "popup_menu.h"
#include "ui_anim.h"

using Microsoft::WRL::ComPtr;

namespace pm::ui {
namespace {

constexpr wchar_t kClass[] = L"PhoneMirrorAskPanel";
constexpr float kW = 452, kPad = 26, kHeadH = 76, kDisc = 40, kBtnH = 38;
constexpr float kRowPadX = 14, kRowPadY = 10, kRowGap = 6;
constexpr float kCheckH = 22, kCheckBox = 18;  // checkbox row (Info::check)

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
    D2D1_COLOR_F top, bottom, fg, dim, accent, onAccent, danger;
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
    t.danger = t.light ? rgb(0xC0392B) : rgb(0xE5675A);
    return t;
}

struct Factories {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IDWriteTextFormat> title, body, label, value, button, icon;
    int lang = -1;
    bool init() {
        if (!d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return false;
        if (!dw && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                              reinterpret_cast<IUnknown**>(dw.GetAddressOf()))))
            return false;
        if (lang == static_cast<int>(pm::i18n::lang()) && title) return true;
        lang = static_cast<int>(pm::i18n::lang());
        auto make = [&](const wchar_t* family, float size, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT a, bool wrap) {
            ComPtr<IDWriteTextFormat> f;
            dw->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                 pm::i18n::localeName(), &f);
            if (f) {
                f->SetTextAlignment(a);
                f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
            }
            return f;
        };
        const wchar_t* ui = pm::i18n::uiFont();
        title = make(ui, 18, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        body = make(ui, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        label = make(ui, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        value = make(ui, 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        button = make(ui, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false);
        if (button) button->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        // Icon font: Segoe Fluent Icons (Windows 11), else Segoe MDL2 Assets.
        ComPtr<IDWriteFontCollection> fonts;
        if (!icon && SUCCEEDED(dw->GetSystemFontCollection(&fonts)))
            for (const wchar_t* fam : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
                UINT32 idx = 0;
                BOOL exists = FALSE;
                if (SUCCEEDED(fonts->FindFamilyName(fam, &idx, &exists)) && exists) {
                    icon = make(fam, 19, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
                    if (icon) icon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    break;
                }
            }
        return title && body && label && value && button;
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

LRESULT CALLBACK askProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* p = reinterpret_cast<AskPanel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (p && p->hwnd() == h) return p->handle(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

struct AskPanel::Impl {
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDWriteTextLayout> title, body;
    std::vector<ComPtr<IDWriteTextLayout>> values;
    std::vector<float> rowH;
    float titleH = 24;
    Theme th = theme();
    HoverAnim anim;  // hover / press levels per Hit
    PanelFade fade;  // open: fade (+ grow) in
};

void AskPanel::layout() {
    Factories& f = fx();
    f.init();
    Impl& im = *impl_;
    const float tx = kPad + kDisc + 14;
    im.title = f.layout(f.title.Get(), info_.title, kW - tx - 56);
    im.titleH = (std::max)(24.0f, f.height(im.title.Get()));
    const float headH = (std::max)(kHeadH, 26 + im.titleH + 18);
    im.body = f.layout(f.body.Get(), info_.body, kW - 2 * kPad);
    if (im.body) im.body->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 13.5f * 1.5f, 13.5f * 1.2f);
    bodyH_ = info_.body.empty() ? 0 : f.height(im.body.Get());
    labelW_ = 0;
    for (const auto& r : info_.rows) labelW_ = (std::max)(labelW_, f.width(f.label.Get(), r.first));
    labelW_ = std::ceil(labelW_) + 16;
    im.values.clear();
    im.rowH.clear();
    rowsH_ = 0;
    const float valueW = kW - 2 * kPad - 2 * kRowPadX - labelW_;
    for (size_t i = 0; i < info_.rows.size(); ++i) {
        im.values.push_back(f.layout(f.value.Get(), info_.rows[i].second, valueW));
        const float h = (std::max)(18.0f, f.height(im.values.back().Get()));
        im.rowH.push_back(h);
        rowsH_ += h + (i ? kRowGap : 0);
    }
    if (!info_.rows.empty()) rowsH_ += 2 * kRowPadY;
    rowsY_ = headH + bodyH_ + (bodyH_ > 0 ? 16 : 0);
    btnY_ = rowsY_ + rowsH_ + (rowsH_ > 0 ? 20 : 6);
    checkW_ = 0;
    if (hasCheck()) {  // [✓] 記住我的選擇 between the body / rows and the buttons
        checkY_ = btnY_ - (rowsH_ > 0 ? 6 : 0);
        checkW_ = std::ceil(kCheckBox + 10 + f.width(f.body.Get(), info_.check));
        btnY_ = checkY_ + kCheckH + 18;
    }
    primW_ = (std::max)(104.0f, f.width(f.button.Get(), info_.primary) + 40);
    secW_ = info_.secondary.empty() ? 0 : (std::max)(92.0f, f.width(f.button.Get(), info_.secondary) + 36);
    stacked_ = secW_ > 0 && primW_ + kBtnGap + secW_ > kW - 2 * kPad;
    h_ = std::ceil(btnY_ + kBtnH + (stacked_ ? kBtnGap + kBtnH : 0) + 22);
}

float AskPanel::btnW(bool primary) const { return stacked_ ? kW - 2 * kPad : primary ? primW_ : secW_; }
float AskPanel::primX() const { return kW - kPad - btnW(true); }
float AskPanel::secX() const { return stacked_ ? kPad : kW - kPad - primW_ - kBtnGap - secW_; }

bool AskPanel::open(HWND owner, Info info, bool activate) {
    if (!fx().init()) return false;
    if (hwnd_) close(0);  // the previous question is answered "no"
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = askProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    info_ = std::move(info);
    owner_ = owner;
    hot_ = pressed_ = HitNone;
    focus_ = 0;
    focusCues_ = false;
    checked_ = info_.checked;
    impl_ = new Impl;
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
    hwnd_ = CreateWindowExW(0, kClass, info_.title.c_str(), WS_POPUP | WS_SYSMENU, x, y, w, hgt, owner, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    dpi_ = GetDpiForWindow(hwnd_);
    if (dpi_ != dx) {  // created on another monitor than measured: resize at its DPI
        SetWindowPos(hwnd_, nullptr, 0, 0, static_cast<int>(std::lround(kW * s())), static_cast<int>(std::lround(h_ * s())),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    const DWORD round = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &round, sizeof(round));
    const bool act = activate && !testOffscreen;
    impl_->fade.begin(hwnd_);
    ShowWindow(hwnd_, act ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
    if (act) SetForegroundWindow(hwnd_);
    return true;
}

void AskPanel::close(int choice) {
    if (!hwnd_) return;
    prepareCloseFade(hwnd_);  // the picture the close fade shows
    HWND h = hwnd_;
    const bool hadFocus = GetForegroundWindow() == h;
    std::function<void(int)> done = std::move(info_.done);
    info_.done = nullptr;
    hwnd_ = nullptr;
    closeWithFade(h);
    destroyTarget();
    delete impl_;
    impl_ = nullptr;
    if (hadFocus && owner_ && IsWindowVisible(owner_) && !testOffscreen) SetForegroundWindow(owner_);
    if (done) done(choice);
}

void AskPanel::destroyTarget() {
    if (!impl_) return;
    impl_->brush.Reset();
    impl_->rt.Reset();
}

void AskPanel::retheme() {
    if (!impl_) return;
    impl_->th = theme();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

AskPanel::Hit AskPanel::hitTest(POINT pt) const {
    const float x = pt.x / s(), y = pt.y / s();
    auto in = [&](D2D1_RECT_F r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
    if (in(rc(kW - 52, 12, 36, 36))) return HitClose;
    if (in(rc(primX(), primY(), btnW(true), kBtnH))) return HitPrimary;
    if (secW_ > 0 && in(rc(secX(), secY(), btnW(false), kBtnH))) return HitSecondary;
    if (hasCheck() && in(rc(kPad - 6, checkY_ - 5, checkW_ + 12, kCheckH + 10))) return HitCheck;
    return HitNone;
}

void AskPanel::press(Hit hit) {
    switch (hit) {
    case HitPrimary: close(1); break;
    case HitSecondary: close(info_.secondaryChoice); break;
    case HitClose: close(0); break;
    case HitCheck:
        checked_ = !checked_;
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    default: break;
    }
}

void AskPanel::paint() {
    Impl& im = *impl_;
    // One DPI per window (GetDpiForWindow): a change without WM_DPICHANGED is
    // caught here - window size, render target DPI / size follow it.
    if (const UINT wd = GetDpiForWindow(hwnd_); wd && wd != dpi_) {
        dpi_ = wd;
        SetWindowPos(hwnd_, nullptr, 0, 0, static_cast<int>(std::lround(kW * s())), static_cast<int>(std::lround(h_ * s())),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (im.rt) {
        RECT cr{};
        GetClientRect(hwnd_, &cr);
        const D2D1_SIZE_U ps = im.rt->GetPixelSize();
        if (ps.width != static_cast<UINT32>(cr.right) || ps.height != static_cast<UINT32>(cr.bottom))
            im.rt->Resize(D2D1::SizeU(cr.right, cr.bottom));
        float dx = 0, dy = 0;
        im.rt->GetDpi(&dx, &dy);
        if (dx != static_cast<float>(dpi_)) im.rt->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
    }
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
    if (const float k = im.fade.scale(); k < 1) {  // opening: grows from 97 % about the centre
        const D2D1_SIZE_F sz = im.rt->GetSize();
        im.rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k, {sz.width / 2, sz.height / 2}));
    }
    draw(im.rt.Get(), im.brush.Get());
    im.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    if (im.rt->EndDraw() == D2DERR_RECREATE_TARGET) destroyTarget();
    im.fade.painted();
}

bool AskPanel::renderPng(const std::wstring& path) {
    if (!hwnd_ || !impl_) return false;
    return renderToPng(kW, h_, dpi_, path, [this](ID2D1RenderTarget* rt) {
        ComPtr<ID2D1SolidColorBrush> b;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &b);
        if (b) draw(rt, b.Get());
    });
}

void AskPanel::draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* b) {
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
    // Card: gradient, accent bar, hairline border.
    {
        ComPtr<ID2D1GradientStopCollection> stops;
        D2D1_GRADIENT_STOP gs[2] = {{0, t.top}, {1, t.bottom}};
        rt->CreateGradientStopCollection(gs, 2, &stops);
        ComPtr<ID2D1LinearGradientBrush> g;
        if (stops) rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {0, h_}), stops.Get(), &g);
        if (g) rt->FillRectangle(rc(0, 0, kW, h_), g.Get());
        else rt->Clear(t.top);
        fill(rc(0, 0, kW, 3), withA(info_.danger ? t.danger : t.accent, 0.85f));
        b->SetColor(withA(t.accent, 0.35f));
        rt->DrawRectangle(rc(0.5f, 0.5f, kW - 1, h_ - 1), b, 1);
    }
    // Glyph in an accent disc, title, ×.
    const D2D1_COLOR_F mark = info_.danger ? t.danger : t.accent;
    b->SetColor(withA(mark, 0.18f));
    rt->FillEllipse(D2D1::Ellipse({kPad + kDisc / 2, 26 + kDisc / 2}, kDisc / 2, kDisc / 2), b);
    if (f.icon && info_.glyph) {
        const wchar_t g[2] = {info_.glyph, 0};
        text(g, f.icon.Get(), rc(kPad, 26, kDisc, kDisc), mark);
    }
    const float tx = kPad + kDisc + 14;
    b->SetColor(t.fg);
    if (im.title) rt->DrawTextLayout({tx, 26 + (im.titleH < 30 ? (kDisc - im.titleH) / 2 : 0)}, im.title.Get(), b);
    {
        const D2D1_RECT_F cr = rc(kW - 52, 12, 36, 36);
        const float h = hl(HitClose);
        if (h > 0.003f) fill(cr, withA(t.accent, 0.22f * h), 8);
        b->SetColor(mix(t.dim, t.fg, h));
        const float cx = kW - 34, cy = 30, d = 5.5f;
        rt->DrawLine({cx - d, cy - d}, {cx + d, cy + d}, b, 1.6f);
        rt->DrawLine({cx - d, cy + d}, {cx + d, cy - d}, b, 1.6f);
    }
    // Body.
    if (im.body && bodyH_ > 0) {
        b->SetColor(t.fg);
        rt->DrawTextLayout({kPad, rowsY_ - bodyH_ - 16}, im.body.Get(), b);
    }
    // Rows: label · value in a rounded well.
    if (!info_.rows.empty()) {
        const D2D1_RECT_F well = rc(kPad, rowsY_, kW - 2 * kPad, rowsH_);
        fill(well, withA(t.fg, t.light ? 0.05f : 0.06f), 10);
        stroke(rc(well.left + 0.5f, well.top + 0.5f, kW - 2 * kPad - 1, rowsH_ - 1), withA(t.fg, 0.10f), 10);
        float y = rowsY_ + kRowPadY;
        for (size_t i = 0; i < info_.rows.size() && i < im.values.size(); ++i) {
            text(info_.rows[i].first, f.label.Get(), rc(kPad + kRowPadX, y + 1, labelW_, 20), t.dim);
            b->SetColor(t.fg);
            if (im.values[i]) rt->DrawTextLayout({kPad + kRowPadX + labelW_, y}, im.values[i].Get(), b);
            y += im.rowH[i] + kRowGap;
        }
    }
    // Buttons: [secondary] [primary].
    auto button = [&](D2D1_RECT_F r, const std::wstring& label, bool primary, Hit id, bool focused) {
        const float w = r.right - r.left, hot = hl(id);
        pressAt(id, r);
        if (primary) {
            D2D1_COLOR_F c = info_.danger ? t.danger : t.accent;
            c = mix(c, t.light ? D2D1_COLOR_F{0, 0, 0, 1} : D2D1_COLOR_F{1, 1, 1, 1}, 0.15f * hot);
            fill(r, c, 9);
            text(label, f.button.Get(), r, info_.danger ? D2D1_COLOR_F{1, 1, 1, 1} : t.onAccent);
        } else {
            fill(r, withA(t.fg, 0.07f + 0.07f * hot), 9);
            stroke(rc(r.left + 0.5f, r.top + 0.5f, w - 1, kBtnH - 1), withA(t.fg, 0.2f), 9);
            text(label, f.button.Get(), r, t.fg);
        }
        rt->SetTransform(base);
        if (focused) stroke(rc(r.left - 3, r.top - 3, w + 6, kBtnH + 6), withA(t.fg, 0.85f), 11, 1.5f);
    };
    button(rc(primX(), primY(), btnW(true), kBtnH), info_.primary, true, HitPrimary, focusCues_ && focus_ == 0);
    if (secW_ > 0)
        button(rc(secX(), secY(), btnW(false), kBtnH), info_.secondary, false, HitSecondary,
               focusCues_ && focus_ == 1);
    // Checkbox: rounded box (accent with a check mark when on) + label.
    if (hasCheck()) {
        const float by = checkY_ + (kCheckH - kCheckBox) / 2;
        const D2D1_RECT_F box = rc(kPad, by, kCheckBox, kCheckBox);
        const float hc = hl(HitCheck);
        pressAt(HitCheck, box);
        if (checked_) {
            D2D1_COLOR_F c = t.accent;
            c = mix(c, t.light ? D2D1_COLOR_F{0, 0, 0, 1} : D2D1_COLOR_F{1, 1, 1, 1}, 0.15f * hc);
            fill(box, c, 4.5f);
            b->SetColor(t.onAccent);
            rt->DrawLine({kPad + 4.2f, by + 9.4f}, {kPad + 7.6f, by + 12.8f}, b, 2.0f);
            rt->DrawLine({kPad + 7.6f, by + 12.8f}, {kPad + 14.0f, by + 5.6f}, b, 2.0f);
        } else {
            fill(box, withA(t.fg, 0.05f + 0.07f * hc), 4.5f);
            stroke(rc(kPad + 0.75f, by + 0.75f, kCheckBox - 1.5f, kCheckBox - 1.5f), withA(t.fg, 0.55f), 4, 1.5f);
        }
        rt->SetTransform(base);
        text(info_.check, f.body.Get(), rc(kPad + kCheckBox + 10, checkY_ + 1, checkW_, kCheckH), t.fg);
        if (focusCues_ && focus_ == 2)
            stroke(rc(kPad - 5, checkY_ - 4, checkW_ + 10, kCheckH + 8), withA(t.fg, 0.85f), 7, 1.5f);
    }
}

LRESULT AskPanel::handle(UINT msg, WPARAM wp, LPARAM lp) {
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
        if (up != HitNone && up == was) press(up);
        return 0;
    }
    case WM_KEYDOWN:
        switch (wp) {
        case VK_ESCAPE: close(0); return 0;
        case VK_SPACE:
            if (focus_ == 2) {
                press(HitCheck);
                return 0;
            }
            [[fallthrough]];
        case VK_RETURN: press(focus_ == 1 ? HitSecondary : HitPrimary); return 0;  // Enter on the checkbox: primary
        case VK_TAB: {  // primary → secondary → checkbox (Shift+Tab backwards)
            const int n = 3, step = GetKeyState(VK_SHIFT) < 0 ? n - 1 : 1;
            do focus_ = (focus_ + step) % n;
            while ((focus_ == 1 && secW_ <= 0) || (focus_ == 2 && !hasCheck()));
            focusCues_ = true;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
            if (secW_ > 0) focus_ = focus_ == 0 ? 1 : 0;
            focusCues_ = true;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        return 0;
    case WM_CLOSE: close(0); return 0;
    case WM_DESTROY:
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace pm::ui
