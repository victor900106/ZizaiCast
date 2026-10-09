// 「連接 Android」 pairing panel (see pair_panel.h, docs/app.md "Android").
//
// An owned WS_POPUP window drawn with a Direct2D HWND render target in the
// popup menus' palette (pm::ui::currentColors): gradient card, hairline
// accent border, Windows 11 rounded corners + drop shadow. Two real EDIT
// controls (WS_CLIPCHILDREN keeps Direct2D off them) for the code form,
// coloured through WM_CTLCOLOREDIT. Layout is in DIPs; the render target uses
// the window's DPI, so 1 DIP = dpi/96 px. Per-monitor DPI aware.
#include "pair_panel.h"

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

constexpr wchar_t kClass[] = L"PhoneMirrorPairPanel";
constexpr float kW = 380, kH = 516;      // panel size (DIPs)
constexpr float kPad = 24;
constexpr float kHeadH = 56;              // draggable title area
constexpr float kQrCard = 236, kQrY = 124;
constexpr float kStatusY = 372, kStatusH = 40;
constexpr float kLinkY = 414, kLinkH = 22;
constexpr float kBtnY = 452, kBtnH = 40;
constexpr float kFieldH = 40;
constexpr float kHostFieldY = 156, kCodeFieldY = 238;
constexpr UINT_PTR kEditSubclass = 1;

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
    t.onAccent = lum(t.accent) > 0.55f ? mix(D2D1_COLOR_F{0.13f, 0.09f, 0.10f, 1}, t.accent, 0.10f)
                                       : D2D1_COLOR_F{1, 1, 1, 1};
    t.error = c.light ? D2D1_COLOR_F{0.75f, 0.22f, 0.20f, 1} : D2D1_COLOR_F{1.0f, 0.62f, 0.45f, 1};
    return t;
}

struct Factories {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IDWriteTextFormat> title, body, label, button, link, status, note, icon;
    int lang = -1;  // language the text formats were made for (UI font)
    bool init() {
        if (!d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return false;
        if (!dw && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                              reinterpret_cast<IUnknown**>(dw.GetAddressOf()))))
            return false;
        if (lang == static_cast<int>(pm::i18n::lang()) && title) return true;
        lang = static_cast<int>(pm::i18n::lang());
        const bool en = !pm::i18n::zh();  // English / 日本語 / 한국어 run longer than 中文
        auto fmt = [&](float size, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT a, bool wrap,
                       const wchar_t* fam = nullptr) {
            ComPtr<IDWriteTextFormat> f;
            dw->CreateTextFormat(fam ? fam : pm::i18n::uiFont(), nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, size, pm::i18n::localeName(), &f);
            if (f) {
                f->SetTextAlignment(a);
                f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
            }
            return f;
        };
        // English runs longer than the Chinese lines: slightly smaller body /
        // status text so they keep to the same three / two lines.
        title = fmt(19, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        body = fmt(en ? 13 : 13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        if (body) body->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        label = fmt(12.5f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        button = fmt(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false);
        link = fmt(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
        status = fmt(en ? 13 : 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, true);
        note = fmt(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true);
        icon = fmt(11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, L"Segoe Fluent Icons");
        return title && body && label && button && link && status && note;
    }
};
Factories& fx() {
    static Factories f;
    return f;
}

D2D1_RECT_F rc(float x, float y, float w, float h) { return {x, y, x + w, y + h}; }

LRESULT CALLBACK panelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* p = reinterpret_cast<PairPanel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (p && p->hwnd() == h) return p->handle(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

// Enter = 配對, Esc = close, Tab = the other box (no dialog manager here).
LRESULT CALLBACK editProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* panel = reinterpret_cast<HWND>(ref);
    if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB)) {
        PostMessageW(panel, WM_APP + 40, wp, reinterpret_cast<LPARAM>(h));
        return 0;
    }
    if (msg == WM_CHAR && (wp == L'\r' || wp == 27 || wp == L'\t')) return 0;  // no beep
    return DefSubclassProc(h, msg, wp, lp);
}

}  // namespace

struct PairPanel::Impl {
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1Bitmap> qr;
    Theme th = theme();
    HoverAnim anim;  // hover / press levels per Hit
    PanelFade fade;  // open: fade (+ grow) in
    double qrAt = -1e9;  // QR arrived (reveal)
};
constexpr UINT_PTR kQrRevealTimer = 0x7A44;
constexpr double kQrRevealMs = 320;

bool PairPanel::open(HWND owner, Callbacks cb) {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
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
    codeMode_ = false;
    qr_.clear();
    qrSize_ = 0;
    status_.clear();
    statusError_ = false;
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

    const std::wstring title = pm::i18n::fmt(S::PairWindowTitle, {tr(S::AppName)});
    hwnd_ = CreateWindowExW(0, kClass, title.c_str(), WS_POPUP | WS_CLIPCHILDREN | WS_SYSMENU, x, y, w, hgt, owner,
                            nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    dpi_ = GetDpiForWindow(hwnd_);
    const DWORD round = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &round, sizeof(round));

    const DWORD es = WS_CHILD | ES_AUTOHSCROLL;
    editHost_ = CreateWindowExW(0, L"EDIT", L"", es, 0, 0, 10, 10, hwnd_, nullptr, nullptr, nullptr);
    editCode_ = CreateWindowExW(0, L"EDIT", L"", es | ES_NUMBER, 0, 0, 10, 10, hwnd_, nullptr, nullptr, nullptr);
    SendMessageW(editHost_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tr(S::PairHostCue)));
    SendMessageW(editCode_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tr(S::PairCodeCue)));
    SendMessageW(editHost_, EM_SETLIMITTEXT, 64, 0);
    SendMessageW(editCode_, EM_SETLIMITTEXT, 6, 0);
    SetWindowSubclass(editHost_, editProc, kEditSubclass, reinterpret_cast<DWORD_PTR>(hwnd_));
    SetWindowSubclass(editCode_, editProc, kEditSubclass, reinterpret_cast<DWORD_PTR>(hwnd_));
    applyEditFonts();
    layout();
    impl_->fade.begin(hwnd_);
    ShowWindow(hwnd_, testOffscreen ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
    if (!testOffscreen) SetForegroundWindow(hwnd_);
    return true;
}

void PairPanel::close() {
    if (!hwnd_) return;
    prepareCloseFade(hwnd_);  // the picture the close fade shows
    HWND h = hwnd_;
    hwnd_ = nullptr;
    editHost_ = editCode_ = nullptr;
    closeWithFade(h);
    destroyTarget();
    delete impl_;
    impl_ = nullptr;
    if (editFont_) DeleteObject(editFont_), editFont_ = nullptr;
    if (editBrush_) DeleteObject(editBrush_), editBrush_ = nullptr;
    // Give the activation back to the main window (not in scripted off-screen tests).
    if (owner_ && IsWindowVisible(owner_) && !testOffscreen) SetForegroundWindow(owner_);
}

void PairPanel::destroyTarget() {
    if (!impl_) return;
    impl_->qr.Reset();
    impl_->brush.Reset();
    impl_->rt.Reset();
}

// A QR bitmap may come pre-scaled (m x m pixels per module). Recover one
// pixel per module, so it can be drawn at a whole multiple for the screen:
// the top-left finder pattern's first dark row is 7 modules wide.
static bool toModuleGrid(const std::vector<uint8_t>& in, int size, std::vector<uint8_t>& out, int& n) {
    auto dark = [&](int x, int y) { return in[(static_cast<size_t>(y) * size + x) * 4 + 1] < 128; };
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            if (!dark(x, y)) continue;
            int run = 0;
            while (x + run < size && dark(x + run, y)) ++run;
            if (run % 7 != 0) return false;
            const int m = run / 7;
            if (m < 1 || size % m != 0) return false;
            n = size / m;
            out.assign(static_cast<size_t>(n) * n * 4, 255);
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) {
                    const uint8_t v = dark(i * m + m / 2, j * m + m / 2) ? 0 : 255;
                    uint8_t* p = &out[(static_cast<size_t>(j) * n + i) * 4];
                    p[0] = p[1] = p[2] = v;
                }
            return true;
        }
    return false;
}

void PairPanel::setQr(const std::vector<uint8_t>& bgra, int size) {
    const bool had = qrSize_ > 0;
    if (size <= 0 || bgra.size() < static_cast<size_t>(size) * size * 4) {
        qr_.clear();
        qrSize_ = 0;
    } else if (int n = 0; toModuleGrid(bgra, size, qr_, n)) {
        qrSize_ = n;
    } else {
        qr_ = bgra;
        qrSize_ = size;
    }
    if (impl_) impl_->qr.Reset();
    if (impl_ && hwnd_ && !had && qrSize_ > 0 && animationsOn()) {  // first QR: revealed from the centre
        impl_->qrAt = animNowMs();
        SetTimer(hwnd_, kQrRevealTimer, 16, nullptr);
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void PairPanel::setStatus(const std::wstring& text, bool error) {
    status_ = text;
    statusError_ = error;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void PairPanel::showCodeForm() { setMode(true); }

void PairPanel::suggestHostPort(const std::wstring& hostPort) {
    if (!editHost_ || GetWindowTextLengthW(editHost_) > 0) return;
    SetWindowTextW(editHost_, hostPort.c_str());
}

void PairPanel::retheme() {
    if (!impl_) return;
    impl_->th = theme();
    if (editBrush_) DeleteObject(editBrush_), editBrush_ = nullptr;
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        InvalidateRect(editHost_, nullptr, TRUE);
        InvalidateRect(editCode_, nullptr, TRUE);
    }
}

void PairPanel::relabel() {
    if (!hwnd_) return;
    fx().init();  // text formats in the new language's font
    SetWindowTextW(hwnd_, pm::i18n::fmt(S::PairWindowTitle, {tr(S::AppName)}).c_str());
    SendMessageW(editHost_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tr(S::PairHostCue)));
    SendMessageW(editCode_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tr(S::PairCodeCue)));
    applyEditFonts();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PairPanel::applyEditFonts() {
    if (editFont_) DeleteObject(editFont_);
    LOGFONTW lf{};
    lf.lfHeight = -static_cast<LONG>(std::lround(15 * s()));
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, pm::i18n::uiFont());
    editFont_ = CreateFontIndirectW(&lf);
    SendMessageW(editHost_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
    SendMessageW(editCode_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
}

void PairPanel::layout() {
    if (!hwnd_) return;
    // Edit boxes inside the drawn fields (14 DIP side padding, vertically centred).
    const int lineH = static_cast<int>(std::lround(22 * s()));
    auto place = [&](HWND e, float fieldY) {
        const int x = static_cast<int>(std::lround((kPad + 14) * s()));
        const int y = static_cast<int>(std::lround((fieldY + kFieldH / 2) * s())) - lineH / 2;
        const int w = static_cast<int>(std::lround((kW - 2 * kPad - 28) * s()));
        SetWindowPos(e, nullptr, x, y, w, lineH, SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(e, codeMode_ ? SW_SHOW : SW_HIDE);
    };
    place(editHost_, kHostFieldY);
    place(editCode_, kCodeFieldY);
    if (impl_ && impl_->rt) {
        RECT r{};
        GetClientRect(hwnd_, &r);
        impl_->rt->Resize(D2D1::SizeU(r.right, r.bottom));
        impl_->rt->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
    }
}

void PairPanel::setMode(bool code) {
    if (!hwnd_ || code == codeMode_) return;
    codeMode_ = code;
    layout();
    if (code) {
        SetFocus(GetWindowTextLengthW(editHost_) > 0 ? editCode_ : editHost_);
    } else {
        SetFocus(hwnd_);
        if (cb_.onQrMode) cb_.onQrMode();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PairPanel::submitCode() {
    wchar_t host[80] = {}, code[16] = {};
    GetWindowTextW(editHost_, host, 80);
    GetWindowTextW(editCode_, code, 16);
    std::wstring hp = host, c = code;
    auto trim = [](std::wstring& v) {
        while (!v.empty() && iswspace(v.back())) v.pop_back();
        while (!v.empty() && iswspace(v.front())) v.erase(v.begin());
    };
    trim(hp);
    trim(c);
    const auto colon = hp.rfind(L':');
    if (hp.empty() || colon == std::wstring::npos || colon == 0 || colon + 1 == hp.size()) {
        setStatus(tr(S::PairEnterHost), true);
        SetFocus(editHost_);
        return;
    }
    if (c.size() != 6) {
        setStatus(tr(S::PairCodeDigits), true);
        SetFocus(editCode_);
        return;
    }
    if (cb_.onPairCode) cb_.onPairCode(hp, c);
}

PairPanel::Hit PairPanel::hitTest(POINT pt) const {
    const float x = pt.x / s(), y = pt.y / s();
    auto in = [&](D2D1_RECT_F r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
    if (in(rc(kW - 52, 12, 36, 36))) return HitClose;
    const float bw = (kW - 2 * kPad - 12) / 2;
    if (in(rc(kPad, kBtnY, bw, kBtnH))) return HitSecondary;
    if (in(rc(kPad + bw + 12, kBtnY, bw, kBtnH))) return HitPrimary;
    if (in(rc(kW / 2 - linkW_ / 2 - 6, kLinkY, linkW_ + 12, kLinkH))) return HitLink;
    return HitNone;
}

void PairPanel::click(Hit h) {
    switch (h) {
    case HitClose: {
        auto cb = cb_.onClose;
        close();
        if (cb) cb();
        break;
    }
    case HitSecondary:  // QR mode: 用配對碼 · code mode: 改用 QR
        setMode(!codeMode_);
        break;
    case HitPrimary:  // QR mode: 取消 · code mode: 配對
        if (codeMode_) submitCode();
        else click(HitClose);
        break;
    case HitLink:
        if (cb_.onHelp) cb_.onHelp();
        break;
    default: break;
    }
}

void PairPanel::paint() {
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
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
        im.rt->CreateBitmap(D2D1::SizeU(qrSize_, qrSize_), qr_.data(), qrSize_ * 4, bp, &im.qr);
    }
    im.rt->BeginDraw();
    draw(im.rt.Get(), im.brush.Get(), im.qr.Get(), false);
    if (im.rt->EndDraw() == D2DERR_RECREATE_TARGET) destroyTarget();
    im.fade.painted();
}

// --dev test hook: the panel as it looks now, drawn into a PNG (the edit
// boxes' text / cue banners drawn in place of the real EDIT controls).
bool PairPanel::renderPng(const std::wstring& path) {
    if (!hwnd_ || !impl_) return false;
    return renderToPng(kW, kH, dpi_, path, [this](ID2D1RenderTarget* rt) {
        ComPtr<ID2D1SolidColorBrush> b;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &b);
        ComPtr<ID2D1Bitmap> qr;
        if (qrSize_ > 0) {
            D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
            rt->CreateBitmap(D2D1::SizeU(qrSize_, qrSize_), qr_.data(), qrSize_ * 4, bp, &qr);
        }
        if (b) draw(rt, b.Get(), qr.Get(), true);
    });
}

void PairPanel::draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* b, ID2D1Bitmap* qrBitmap, bool png) {
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
    auto stroke = [&](D2D1_RECT_F r, D2D1_COLOR_F c, float radius, float width = 1) {
        b->SetColor(c);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b, width);
    };
    auto text = [&](const std::wstring& s, IDWriteTextFormat* fmt, D2D1_RECT_F r, D2D1_COLOR_F c) {
        b->SetColor(c);
        const std::wstring w = pm::i18n::keepWords(s);  // 한국어: wrap between words
        rt->DrawTextW(w.c_str(), static_cast<UINT32>(w.size()), fmt, r, b);
    };

    // Card: vertical gradient + hairline accent border.
    {
        ComPtr<ID2D1GradientStopCollection> stops;
        D2D1_GRADIENT_STOP gs[2] = {{0, t.top}, {1, t.bottom}};
        rt->CreateGradientStopCollection(gs, 2, &stops);
        ComPtr<ID2D1LinearGradientBrush> g;
        if (stops)
            rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {0, kH}), stops.Get(), &g);
        if (g) rt->FillRectangle(rc(0, 0, kW, kH), g.Get());
        else rt->Clear(t.top);
        // Thin accent bar along the top (like the tray menu).
        fill(rc(0, 0, kW, 3), D2D1_COLOR_F{t.accent.r, t.accent.g, t.accent.b, 0.85f});
        b->SetColor(D2D1_COLOR_F{t.accent.r, t.accent.g, t.accent.b, 0.35f});
        rt->DrawRectangle(rc(0.5f, 0.5f, kW - 1, kH - 1), b, 1);
    }
    // Title + close button.
    text(tr(S::PairTitle), f.title.Get(), rc(kPad, 18, kW - 2 * kPad - 40, 32), t.fg);
    {
        const D2D1_RECT_F cr = rc(kW - 52, 12, 36, 36);
        if (const float h = hl(HitClose); h > 0.003f) fill(cr, D2D1_COLOR_F{t.accent.r, t.accent.g, t.accent.b, 0.22f * h}, 8);
        b->SetColor(animMix(t.dim, t.fg, hl(HitClose)));
        const float cx = kW - 34, cy = 30, d = 5.5f;
        rt->DrawLine({cx - d, cy - d}, {cx + d, cy + d}, b, 1.6f);
        rt->DrawLine({cx - d, cy + d}, {cx + d, cy - d}, b, 1.6f);
    }

    if (!codeMode_) {
        text(tr(S::PairStepsQr), f.body.Get(), rc(kPad, 58, kW - 2 * kPad, 64), t.dim);
        // White card with the QR (quiet zone of at least 4 modules).
        const float cardX = (kW - kQrCard) / 2;
        fill(rc(cardX, kQrY, kQrCard, kQrCard), D2D1_COLOR_F{1, 1, 1, 1}, 14);
        if (qrBitmap) {
            const float sc = s();
            // The bitmap has its own quiet zone; keep 10 DIP of card around it.
            const int availPx = static_cast<int>(std::floor((kQrCard - 20) * sc));
            int k = availPx / qrSize_;  // whole pixels per bitmap pixel
            float drawPx = static_cast<float>(k * qrSize_);
            auto interp = D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
            if (k < 1) {  // big pre-scaled bitmap: fit it, smoothly
                drawPx = static_cast<float>(availPx);
                interp = D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
            }
            // Pixel-aligned placement so every module has the same width.
            const float leftPx = std::round((cardX + kQrCard / 2) * sc - drawPx / 2);
            const float topPx = std::round((kQrY + kQrCard / 2) * sc - drawPx / 2);
            const D2D1_RECT_F qr = rc(leftPx / sc, topPx / sc, drawPx / sc, drawPx / sc);
            const double qe = animNowMs() - im.qrAt;
            ComPtr<ID2D1EllipseGeometry> mask;
            ComPtr<ID2D1Layer> layer;
            if (!png && qe >= 0 && qe < kQrRevealMs) {
                // Reveal: a circle growing from the centre (soft-out) while it
                // fades in (outCubic); a phone can lock on before it ends.
                const float t = static_cast<float>(qe / kQrRevealMs);
                const float r = (drawPx / sc) * 0.72f * animSoftOut(t);
                f.d2d->CreateEllipseGeometry(D2D1::Ellipse({(qr.left + qr.right) / 2, (qr.top + qr.bottom) / 2}, r, r), &mask);
                rt->CreateLayer(&layer);
                if (mask && layer)
                    rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), mask.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                        D2D1::IdentityMatrix(), animOutCubic(t)),
                                  layer.Get());
            }
            rt->DrawBitmap(qrBitmap, qr, 1, interp);
            if (mask && layer) rt->PopLayer();
        } else {
            text(tr(S::PairPreparingQr), f.note.Get(), rc(cardX, kQrY, kQrCard, kQrCard),
                 D2D1_COLOR_F{0.35f, 0.30f, 0.31f, 1});
        }
    } else {
        text(tr(S::PairStepsCode), f.body.Get(), rc(kPad, 58, kW - 2 * kPad, 64), t.dim);
        auto field = [&](const wchar_t* lbl, float y, HWND e) {
            text(lbl, f.label.Get(), rc(kPad, y - 26, kW - 2 * kPad, 22), t.fg);
            fill(rc(kPad, y, kW - 2 * kPad, kFieldH), t.field, 8);
            const bool focus = GetFocus() == e;
            stroke(rc(kPad + 0.5f, y + 0.5f, kW - 2 * kPad - 1, kFieldH - 1),
                   focus ? t.accent : D2D1_COLOR_F{t.fg.r, t.fg.g, t.fg.b, 0.16f}, 8, focus ? 1.5f : 1);
            if (png) {  // the EDIT control's content (or its cue banner)
                wchar_t buf[80] = {};
                GetWindowTextW(e, buf, 80);
                const bool cue = !buf[0];
                const std::wstring s = cue ? tr(e == editHost_ ? S::PairHostCue : S::PairCodeCue) : std::wstring(buf);
                text(s, f.label.Get(), rc(kPad + 14, y + 9, kW - 2 * kPad - 28, 22),
                     cue ? D2D1_COLOR_F{t.fg.r, t.fg.g, t.fg.b, 0.45f} : t.fg);
            }
        };
        field(tr(S::PairFieldHost), kHostFieldY, editHost_);
        field(tr(S::PairFieldCode), kCodeFieldY, editCode_);
        text(tr(S::PairAutoNote), f.note.Get(), rc(kPad, 296, kW - 2 * kPad, 40), t.dim);
    }

    // Status line.
    if (!status_.empty())
        text(status_, f.status.Get(), rc(kPad, kStatusY, kW - 2 * kPad, kStatusH), statusError_ ? t.error : t.accent);
    // Help link.
    {
        const std::wstring l = tr(S::PairHelpLink);
        ComPtr<IDWriteTextLayout> m;  // measured: the underline and the hit area follow the text
        if (SUCCEEDED(f.dw->CreateTextLayout(l.c_str(), static_cast<UINT32>(l.size()), f.link.Get(), kW, kLinkH, &m))) {
            DWRITE_TEXT_METRICS tm{};
            m->GetMetrics(&tm);
            linkW_ = (std::min)(tm.widthIncludingTrailingWhitespace, kW - 2 * kPad);
        }
        const float lh = hl(HitLink);
        text(l, f.link.Get(), rc(kPad, kLinkY, kW - 2 * kPad, kLinkH), animMix(t.accent, t.fg, lh));
        b->SetColor(D2D1_COLOR_F{t.accent.r, t.accent.g, t.accent.b, 0.45f + 0.45f * lh});
        rt->DrawLine({kW / 2 - linkW_ / 2, kLinkY + kLinkH - 1}, {kW / 2 + linkW_ / 2, kLinkY + kLinkH - 1}, b, 1);
    }
    // Buttons: [secondary] [primary].
    const float bw = (kW - 2 * kPad - 12) / 2;
    {
        const D2D1_RECT_F r = rc(kPad, kBtnY, bw, kBtnH);
        pressAt(HitSecondary, r);
        fill(r, D2D1_COLOR_F{t.fg.r, t.fg.g, t.fg.b, 0.07f + 0.07f * hl(HitSecondary)}, 9);
        stroke(rc(kPad + 0.5f, kBtnY + 0.5f, bw - 1, kBtnH - 1), D2D1_COLOR_F{t.fg.r, t.fg.g, t.fg.b, 0.2f}, 9);
        text(tr(codeMode_ ? S::PairUseQr : S::PairUseCode), f.button.Get(), r, t.fg);
        rt->SetTransform(base);
    }
    {
        const D2D1_RECT_F r = rc(kPad + bw + 12, kBtnY, bw, kBtnH);
        const float ph = hl(HitPrimary);
        pressAt(HitPrimary, r);
        if (codeMode_) {
            D2D1_COLOR_F c = t.accent;
            c = mix(c, t.light ? D2D1_COLOR_F{0, 0, 0, 1} : D2D1_COLOR_F{1, 1, 1, 1}, 0.15f * ph);
            fill(r, c, 9);
            text(tr(S::PairButton), f.button.Get(), r, t.onAccent);
        } else {
            fill(r, D2D1_COLOR_F{t.fg.r, t.fg.g, t.fg.b, 0.07f + 0.07f * ph}, 9);
            stroke(rc(r.left + 0.5f, kBtnY + 0.5f, bw - 1, kBtnH - 1), D2D1_COLOR_F{t.fg.r, t.fg.g, t.fg.b, 0.2f}, 9);
            text(tr(S::Cancel), f.button.Get(), r, t.fg);
        }
        rt->SetTransform(base);
    }
}

LRESULT PairPanel::handle(UINT msg, WPARAM wp, LPARAM lp) {
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
        if (wp == kQrRevealTimer) {
            InvalidateRect(h, nullptr, FALSE);
            if (!impl_ || animNowMs() - impl_->qrAt > kQrRevealMs + 20) KillTimer(h, kQrRevealTimer);
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
        pressed_ = hitTest(POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))});
        if (impl_) impl_->anim.setPressed(h, pressed_);
        if (pressed_ == HitNone) SetFocus(h);
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
        else if (wp == VK_RETURN && codeMode_) submitCode();
        return 0;
    case WM_APP + 40:  // key from an edit box (editProc)
        if (wp == VK_ESCAPE) click(HitClose);
        else if (wp == VK_RETURN) submitCode();
        else if (wp == VK_TAB) SetFocus(reinterpret_cast<HWND>(lp) == editHost_ ? editCode_ : editHost_);
        return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) InvalidateRect(h, nullptr, FALSE);
        if (HIWORD(wp) == EN_CHANGE && statusError_) setStatus(L"");
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
