// 常駐外網 (0.8.2): the 「設定密碼」 window (see remote_password_panel.h).
// Same build as pair_panel.cpp's code form: owned WS_POPUP, Direct2D HWND
// render target in the popup menus' palette, two real EDIT controls
// (ES_PASSWORD) kept clear of Direct2D by WS_CLIPCHILDREN, per-monitor DPI.
#include "remote_password_panel.h"

#include <commctrl.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

#include "pm/i18n.h"
#include "popup_menu.h"
#include "ui_anim.h"

using Microsoft::WRL::ComPtr;
using pm::i18n::S;
using pm::i18n::tr;

namespace pm::ui {
namespace {

constexpr wchar_t kClass[] = L"PhoneMirrorRemotePassword";
constexpr float kW = 400, kH = 448;
constexpr float kPad = 24, kHeadH = 56;
constexpr float kFieldH = 40, kField1Y = 168, kField2Y = 246;
constexpr float kNoteY = 294, kStatusY = 334, kStatusH = 36;
constexpr float kBtnY = 388, kBtnH = 40;
constexpr UINT_PTR kEditSubclass = 1;
constexpr int kMaxLen = 128;

D2D1_COLOR_F rgb(uint32_t c, float a = 1) {
    return {((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, a};
}
D2D1_COLOR_F mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}
float lum(D2D1_COLOR_F c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }
COLORREF toRef(D2D1_COLOR_F c) {
    auto b = [](float v) { return static_cast<BYTE>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
    return RGB(b(c.r), b(c.g), b(c.b));
}
D2D1_RECT_F rc(float x, float y, float w, float h) { return {x, y, x + w, y + h}; }

struct Theme {
    D2D1_COLOR_F top, bottom, fg, dim, accent, field, onAccent, error;
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
    t.field = mix(t.top, c.light ? D2D1_COLOR_F{1, 1, 1, 1} : D2D1_COLOR_F{0, 0, 0, 1}, c.light ? 0.55f : 0.28f);
    t.onAccent = lum(t.accent) > 0.55f ? mix(D2D1_COLOR_F{0.13f, 0.09f, 0.10f, 1}, t.accent, 0.10f) : D2D1_COLOR_F{1, 1, 1, 1};
    t.error = c.light ? D2D1_COLOR_F{0.75f, 0.22f, 0.20f, 1} : D2D1_COLOR_F{1.0f, 0.62f, 0.45f, 1};
    return t;
}

struct Factories {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IDWriteTextFormat> title, body, label, button, status, note;
    int lang = -1;
    bool init() {
        if (!d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return false;
        if (!dw && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                              reinterpret_cast<IUnknown**>(dw.GetAddressOf()))))
            return false;
        if (lang == static_cast<int>(pm::i18n::lang()) && title) return true;
        lang = static_cast<int>(pm::i18n::lang());
        const bool en = !pm::i18n::zh();
        auto fmt = [&](float size, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT a, bool wrap) {
            ComPtr<IDWriteTextFormat> f;
            dw->CreateTextFormat(pm::i18n::uiFont(), nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                 pm::i18n::localeName(), &f);
            if (f) {
                f->SetTextAlignment(a);
                f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
            }
            return f;
        };
        title = fmt(19, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        body = fmt(en ? 13 : 13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        if (body) body->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        label = fmt(12.5f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        button = fmt(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false);
        status = fmt(en ? 13 : 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, true);
        note = fmt(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        return title && body && label && button && status && note;
    }
};
Factories& fx() {
    static Factories f;
    return f;
}

LRESULT CALLBACK panelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* p = reinterpret_cast<RemotePasswordPanel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (p && p->hwnd() == h) return p->handle(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

// Enter = 儲存, Esc = close, Tab = the other box (no dialog manager here).
LRESULT CALLBACK editProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* panel = reinterpret_cast<HWND>(ref);
    if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB)) {
        PostMessageW(panel, WM_APP + 40, wp, reinterpret_cast<LPARAM>(h));
        return 0;
    }
    if (msg == WM_CHAR && (wp == L'\r' || wp == 27 || wp == L'\t')) return 0;
    return DefSubclassProc(h, msg, wp, lp);
}

// Reads a password box and wipes the copy on the stack.
std::wstring readBox(HWND e) {
    wchar_t buf[kMaxLen + 2] = {};
    GetWindowTextW(e, buf, kMaxLen + 1);
    std::wstring v = buf;
    SecureZeroMemory(buf, sizeof(buf));
    return v;
}

}  // namespace

struct RemotePasswordPanel::Impl {
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> brush;
    Theme th = theme();
    HoverAnim anim;
    PanelFade fade;
};

bool RemotePasswordPanel::open(HWND owner, Callbacks cb) {
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
        wc.lpfnWndProc = panelProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    owner_ = owner;
    cb_ = std::move(cb);
    status_.clear();
    hot_ = pressed_ = HitNone;
    impl_ = new Impl;
    RECT orc{};
    const bool ownerVisible = owner && IsWindowVisible(owner) && !IsIconic(owner);
    if (ownerVisible) GetWindowRect(owner, &orc);
    HMONITOR mon = ownerVisible ? MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
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
    hwnd_ = CreateWindowExW(0, kClass, tr(S::RemotePwTitle), WS_POPUP | WS_CLIPCHILDREN | WS_SYSMENU, x, y, w, hgt, owner, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    dpi_ = GetDpiForWindow(hwnd_);
    const DWORD round = 2;
    DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &round, sizeof(round));
    const DWORD es = WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD;
    edit1_ = CreateWindowExW(0, L"EDIT", L"", es, 0, 0, 10, 10, hwnd_, nullptr, nullptr, nullptr);
    edit2_ = CreateWindowExW(0, L"EDIT", L"", es, 0, 0, 10, 10, hwnd_, nullptr, nullptr, nullptr);
    for (HWND e : {edit1_, edit2_}) {
        SendMessageW(e, EM_SETLIMITTEXT, kMaxLen, 0);
        SendMessageW(e, EM_SETPASSWORDCHAR, 0x25CF, 0);  // ●
        SetWindowSubclass(e, editProc, kEditSubclass, reinterpret_cast<DWORD_PTR>(hwnd_));
    }
    SendMessageW(edit1_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tr(S::RemotePwCue1)));
    SendMessageW(edit2_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tr(S::RemotePwCue2)));
    applyEditFonts();
    layout();
    impl_->fade.begin(hwnd_);
    ShowWindow(hwnd_, testOffscreen ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
    if (!testOffscreen) {
        SetForegroundWindow(hwnd_);
        SetFocus(edit1_);
    }
    return true;
}

void RemotePasswordPanel::close() {
    if (!hwnd_) return;
    for (HWND e : {edit1_, edit2_})
        if (e) SetWindowTextW(e, L"");
    prepareCloseFade(hwnd_);
    HWND h = hwnd_;
    hwnd_ = nullptr;
    edit1_ = edit2_ = nullptr;
    closeWithFade(h);
    destroyTarget();
    delete impl_;
    impl_ = nullptr;
    if (editFont_) DeleteObject(editFont_), editFont_ = nullptr;
    if (editBrush_) DeleteObject(editBrush_), editBrush_ = nullptr;
    if (owner_ && IsWindowVisible(owner_) && !testOffscreen) SetForegroundWindow(owner_);
}

void RemotePasswordPanel::destroyTarget() {
    if (!impl_) return;
    impl_->brush.Reset();
    impl_->rt.Reset();
}

void RemotePasswordPanel::retheme() {
    if (!impl_) return;
    impl_->th = theme();
    if (editBrush_) DeleteObject(editBrush_), editBrush_ = nullptr;
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        InvalidateRect(edit1_, nullptr, TRUE);
        InvalidateRect(edit2_, nullptr, TRUE);
    }
}

void RemotePasswordPanel::relabel() {
    if (!hwnd_) return;
    fx().init();
    SetWindowTextW(hwnd_, tr(S::RemotePwTitle));
    SendMessageW(edit1_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tr(S::RemotePwCue1)));
    SendMessageW(edit2_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tr(S::RemotePwCue2)));
    applyEditFonts();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void RemotePasswordPanel::applyEditFonts() {
    if (editFont_) DeleteObject(editFont_);
    LOGFONTW lf{};
    lf.lfHeight = -static_cast<LONG>(std::lround(15 * s()));
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, pm::i18n::uiFont());
    editFont_ = CreateFontIndirectW(&lf);
    SendMessageW(edit1_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
    SendMessageW(edit2_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
}

void RemotePasswordPanel::layout() {
    if (!hwnd_) return;
    const int lineH = static_cast<int>(std::lround(22 * s()));
    auto place = [&](HWND e, float fieldY) {
        const int x = static_cast<int>(std::lround((kPad + 14) * s()));
        const int y = static_cast<int>(std::lround((fieldY + kFieldH / 2) * s())) - lineH / 2;
        const int w = static_cast<int>(std::lround((kW - 2 * kPad - 28) * s()));
        SetWindowPos(e, nullptr, x, y, w, lineH, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    place(edit1_, kField1Y);
    place(edit2_, kField2Y);
    if (impl_ && impl_->rt) {
        RECT r{};
        GetClientRect(hwnd_, &r);
        impl_->rt->Resize(D2D1::SizeU(r.right, r.bottom));
        impl_->rt->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
    }
}

void RemotePasswordPanel::setStatus(const std::wstring& t) {
    status_ = t;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void RemotePasswordPanel::submit() {
    std::wstring a = readBox(edit1_), b = readBox(edit2_);
    if (a.size() < 8) {
        setStatus(tr(S::RemotePwShort));
        SetFocus(edit1_);
    } else if (a != b) {
        setStatus(tr(S::RemotePwMismatch));
        SetWindowTextW(edit2_, L"");
        SetFocus(edit2_);
    } else {
        auto cb = cb_.onSave;
        close();
        if (cb) cb(a);
    }
    std::fill(a.begin(), a.end(), L'\0');
    std::fill(b.begin(), b.end(), L'\0');
}

void RemotePasswordPanel::testFill(const std::wstring& a, const std::wstring& b, bool save) {
    if (!hwnd_) return;
    SetWindowTextW(edit1_, a.c_str());
    SetWindowTextW(edit2_, b.c_str());
    if (save) submit();
}

RemotePasswordPanel::Hit RemotePasswordPanel::hitTest(POINT pt) const {
    const float x = pt.x / s(), y = pt.y / s();
    auto in = [&](D2D1_RECT_F r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
    if (in(rc(kW - 52, 12, 36, 36))) return HitClose;
    const float bw = (kW - 2 * kPad - 12) / 2;
    if (in(rc(kPad, kBtnY, bw, kBtnH))) return HitSecondary;
    if (in(rc(kPad + bw + 12, kBtnY, bw, kBtnH))) return HitPrimary;
    return HitNone;
}

void RemotePasswordPanel::click(Hit h) {
    switch (h) {
    case HitClose:
    case HitSecondary: {
        auto cb = cb_.onCancel;
        close();
        if (cb) cb();
        break;
    }
    case HitPrimary: submit(); break;
    default: break;
    }
}

void RemotePasswordPanel::paint() {
    Impl& im = *impl_;
    Factories& f = fx();
    if (!im.rt) {
        RECT r{};
        GetClientRect(hwnd_, &r);
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
        props.dpiX = props.dpiY = static_cast<float>(dpi_);
        if (FAILED(f.d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(r.right, r.bottom)), &im.rt)))
            return;
        im.rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &im.brush);
        im.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    im.rt->BeginDraw();
    draw(im.rt.Get(), im.brush.Get(), false);
    if (im.rt->EndDraw() == D2DERR_RECREATE_TARGET) destroyTarget();
    im.fade.painted();
}

bool RemotePasswordPanel::renderPng(const std::wstring& path) {
    if (!hwnd_ || !impl_) return false;
    return renderToPng(kW, kH, dpi_, path, [this](ID2D1RenderTarget* rt) {
        ComPtr<ID2D1SolidColorBrush> b;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &b);
        if (b) draw(rt, b.Get(), true);
    });
}

void RemotePasswordPanel::draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* b, bool png) {
    Impl& im = *impl_;
    Factories& f = fx();
    const Theme& t = im.th;
    auto hl = [&](Hit x) { return im.anim.hot(x); };
    D2D1_MATRIX_3X2_F base;
    rt->GetTransform(&base);
    auto pressAt = [&](Hit x, D2D1_RECT_F r) {
        const float k = im.anim.pressScale(x);
        rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k, {(r.left + r.right) / 2, (r.top + r.bottom) / 2}) *
                         *D2D1::Matrix3x2F::ReinterpretBaseType(&base));
    };
    auto fill = [&](D2D1_RECT_F r, D2D1_COLOR_F c, float radius = 0) {
        b->SetColor(c);
        if (radius > 0) rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b);
        else rt->FillRectangle(r, b);
    };
    auto stroke = [&](D2D1_RECT_F r, D2D1_COLOR_F c, float radius, float width = 1) {
        b->SetColor(c);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b, width);
    };
    auto text = [&](const std::wstring& str, IDWriteTextFormat* fmt, D2D1_RECT_F r, D2D1_COLOR_F c) {
        b->SetColor(c);
        const std::wstring w = pm::i18n::keepWords(str);
        rt->DrawTextW(w.c_str(), static_cast<UINT32>(w.size()), fmt, r, b);
    };
    {
        ComPtr<ID2D1GradientStopCollection> stops;
        D2D1_GRADIENT_STOP gs[2] = {{0, t.top}, {1, t.bottom}};
        rt->CreateGradientStopCollection(gs, 2, &stops);
        ComPtr<ID2D1LinearGradientBrush> g;
        if (stops) rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {0, kH}), stops.Get(), &g);
        if (g) rt->FillRectangle(rc(0, 0, kW, kH), g.Get());
        else rt->Clear(t.top);
        fill(rc(0, 0, kW, 3), D2D1_COLOR_F{t.accent.r, t.accent.g, t.accent.b, 0.85f});
        b->SetColor(D2D1_COLOR_F{t.accent.r, t.accent.g, t.accent.b, 0.35f});
        rt->DrawRectangle(rc(0.5f, 0.5f, kW - 1, kH - 1), b, 1);
    }
    text(tr(S::RemotePwTitle), f.title.Get(), rc(kPad, 18, kW - 2 * kPad - 40, 32), t.fg);
    {
        const D2D1_RECT_F cr = rc(kW - 52, 12, 36, 36);
        if (const float h = hl(HitClose); h > 0.003f) fill(cr, D2D1_COLOR_F{t.accent.r, t.accent.g, t.accent.b, 0.22f * h}, 8);
        b->SetColor(animMix(t.dim, t.fg, hl(HitClose)));
        const float cx = kW - 34, cy = 30, d = 5.5f;
        rt->DrawLine({cx - d, cy - d}, {cx + d, cy + d}, b, 1.6f);
        rt->DrawLine({cx - d, cy + d}, {cx + d, cy - d}, b, 1.6f);
    }
    text(tr(S::RemotePwBody), f.body.Get(), rc(kPad, 58, kW - 2 * kPad, 76), t.dim);
    auto field = [&](const wchar_t* lbl, float y, HWND e, S cue) {
        text(lbl, f.label.Get(), rc(kPad, y - 26, kW - 2 * kPad, 22), t.fg);
        fill(rc(kPad, y, kW - 2 * kPad, kFieldH), t.field, 8);
        const bool focus = !png && GetFocus() == e;
        stroke(rc(kPad + 0.5f, y + 0.5f, kW - 2 * kPad - 1, kFieldH - 1),
               focus ? t.accent : D2D1_COLOR_F{t.fg.r, t.fg.g, t.fg.b, 0.16f}, 8, focus ? 1.5f : 1);
        if (png) {  // dots for what is typed, else the cue banner
            const int n = e ? GetWindowTextLengthW(e) : 0;
            const std::wstring s = n ? std::wstring(static_cast<size_t>(n), 0x25CF) : std::wstring(tr(cue));
            text(s, f.label.Get(), rc(kPad + 14, y + 9, kW - 2 * kPad - 28, 22), n ? t.fg : D2D1_COLOR_F{t.fg.r, t.fg.g, t.fg.b, 0.45f});
        }
    };
    field(tr(S::RemotePwField1), kField1Y, edit1_, S::RemotePwCue1);
    field(tr(S::RemotePwField2), kField2Y, edit2_, S::RemotePwCue2);
    text(tr(S::RemotePwNote), f.note.Get(), rc(kPad, kNoteY, kW - 2 * kPad, 36), t.dim);
    if (!status_.empty()) text(status_, f.status.Get(), rc(kPad, kStatusY, kW - 2 * kPad, kStatusH), t.error);
    const float bw = (kW - 2 * kPad - 12) / 2;
    {
        const D2D1_RECT_F r = rc(kPad, kBtnY, bw, kBtnH);
        pressAt(HitSecondary, r);
        fill(r, D2D1_COLOR_F{t.fg.r, t.fg.g, t.fg.b, 0.07f + 0.07f * hl(HitSecondary)}, 9);
        stroke(rc(kPad + 0.5f, kBtnY + 0.5f, bw - 1, kBtnH - 1), D2D1_COLOR_F{t.fg.r, t.fg.g, t.fg.b, 0.2f}, 9);
        text(tr(S::Cancel), f.button.Get(), r, t.fg);
        rt->SetTransform(base);
    }
    {
        const D2D1_RECT_F r = rc(kPad + bw + 12, kBtnY, bw, kBtnH);
        pressAt(HitPrimary, r);
        fill(r, mix(t.accent, t.light ? D2D1_COLOR_F{0, 0, 0, 1} : D2D1_COLOR_F{1, 1, 1, 1}, 0.15f * hl(HitPrimary)), 9);
        text(tr(S::RemotePwSave), f.button.Get(), r, t.onAccent);
        rt->SetTransform(base);
    }
}

LRESULT RemotePasswordPanel::handle(UINT msg, WPARAM wp, LPARAM lp) {
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
    case WM_SIZE: layout(); return 0;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        applyEditFonts();
        layout();
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
        pressed_ = hitTest(POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))});
        if (impl_) impl_->anim.setPressed(h, pressed_);
        return 0;
    case WM_LBUTTONUP: {
        const Hit up = hitTest(POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))});
        const Hit was = pressed_;
        pressed_ = HitNone;
        if (impl_) impl_->anim.setPressed(h, HitNone);
        if (up != HitNone && up == was) click(up);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) click(HitClose);
        else if (wp == VK_RETURN) submit();
        return 0;
    case WM_APP + 40:
        if (wp == VK_ESCAPE) click(HitClose);
        else if (wp == VK_RETURN) submit();
        else if (wp == VK_TAB) SetFocus(reinterpret_cast<HWND>(lp) == edit1_ ? edit2_ : edit1_);
        return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) InvalidateRect(h, nullptr, FALSE);
        if (HIWORD(wp) == EN_CHANGE && !status_.empty()) setStatus(L"");
        return 0;
    case WM_CTLCOLOREDIT: {
        const Theme& t = impl_ ? impl_->th : theme();
        if (!editBrush_) editBrush_ = CreateSolidBrush(toRef(t.field));
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, toRef(t.fg));
        SetBkColor(dc, toRef(t.field));
        return reinterpret_cast<LRESULT>(editBrush_);
    }
    case WM_CLOSE: click(HitClose); return 0;
    case WM_DESTROY:
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace pm::ui
