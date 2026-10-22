// 0.8.3.2 允許遠端控制 ▸ 新增臨時帳號 (see remote_guest_panel.h). Same build as the
// 「設定密碼」 window (remote_password_panel.cpp): owned WS_POPUP, Direct2D HWND render
// target in the popup menus' palette, one real EDIT (the account name) kept clear of
// Direct2D by WS_CLIPCHILDREN, per-monitor DPI. Two pages: the form, then the result
// (網址 / 帳號 / 密碼 once, 複製全部); 重設密碼 opens the result page directly.
#include "remote_guest_panel.h"

#include <commctrl.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

#include "pm/i18n.h"
#include "guest_rights.h"
#include "popup_menu.h"
#include "ui_anim.h"

using Microsoft::WRL::ComPtr;
using pm::i18n::S;
using pm::i18n::tr;

namespace pm::ui {
namespace {

constexpr wchar_t kClass[] = L"PhoneMirrorRemoteGuest";
constexpr float kW = 440, kH = 500;
constexpr float kPad = 24, kHeadH = 56;
// the form
constexpr float kBodyY = 56, kNameLblY = 108, kNameY = 132, kFieldH = 40;
constexpr float kLifeLblY = 186, kLifeY = 210, kChipH = 36;
constexpr float kRightLblY = 260, kRightY = 284;
constexpr float kFilesY = 334, kFilesH = 34;
constexpr float kStatusY = 380, kStatusH = 40;
constexpr float kBtnY = 436, kBtnH = 40;
// the result
constexpr float kUrlLblY = 62, kUrlY = 84, kUrlH = 46;
constexpr float kNameRLblY = 138, kNameRY = 160, kPwLblY = 212, kPwY = 234, kSumY = 290, kNoteY = 320, kNoteH = 58;
constexpr UINT_PTR kEditSubclass = 1;
constexpr int kMaxName = 24;

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
    D2D1_COLOR_F top, bottom, fg, dim, accent, field, onAccent, error, good;
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
    t.good = c.light ? D2D1_COLOR_F{0.16f, 0.52f, 0.30f, 1} : D2D1_COLOR_F{0.55f, 0.88f, 0.62f, 1};
    return t;
}

struct Factories {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IDWriteTextFormat> title, body, label, button, status, chip, value, url, pw;
    int lang = -1;
    bool init() {
        if (!d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return false;
        if (!dw && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                              reinterpret_cast<IUnknown**>(dw.GetAddressOf()))))
            return false;
        if (lang == static_cast<int>(pm::i18n::lang()) && title) return true;
        lang = static_cast<int>(pm::i18n::lang());
        const bool en = !pm::i18n::zh();
        auto fmt = [&](const wchar_t* face, float size, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT a, bool wrap) {
            ComPtr<IDWriteTextFormat> f;
            dw->CreateTextFormat(face, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, pm::i18n::localeName(), &f);
            if (f) {
                f->SetTextAlignment(a);
                f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
            }
            return f;
        };
        const wchar_t* ui = pm::i18n::uiFont();
        title = fmt(ui, 19, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        body = fmt(ui, en ? 13 : 13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        if (body) body->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        label = fmt(ui, 12.5f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        button = fmt(ui, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false);
        status = fmt(ui, en ? 13 : 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, true);
        chip = fmt(ui, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false);
        value = fmt(L"Consolas", 20, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        url = fmt(L"Consolas", 13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        if (url) url->SetWordWrapping(DWRITE_WORD_WRAPPING_CHARACTER);
        pw = fmt(L"Consolas", 30, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        return title && body && label && button && status && chip && value && url && pw;
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
    auto* p = reinterpret_cast<RemoteGuestPanel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (p && p->hwnd() == h) return p->handle(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

// Enter = 建立, Esc = close (no dialog manager here).
LRESULT CALLBACK editProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* panel = reinterpret_cast<HWND>(ref);
    if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE)) {
        PostMessageW(panel, WM_APP + 40, wp, 0);
        return 0;
    }
    if (msg == WM_CHAR && (wp == L'\r' || wp == 27)) return 0;
    return DefSubclassProc(h, msg, wp, lp);
}

const S kLife[4] = {S::RemoteGuestLifeOnce, S::RemoteGuestLife1h, S::RemoteGuestLife24h, S::RemoteGuestLife7d};

}  // namespace

struct RemoteGuestPanel::Impl {
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> brush;
    Theme th = theme();
    HoverAnim anim;
    PanelFade fade;
};

std::wstring RemoteGuestPanel::copyText(const Result& r) {
    return pm::i18n::fmt(S::RemoteGuestCopyText, {tr(S::AppName), r.url, r.name, r.password, r.summary});
}

bool RemoteGuestPanel::create(HWND owner, Callbacks cb) {
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
    if (testOffscreen) x = -12000 - w, y = 200;  // never on a monitor in the tests
    else {
        x = (std::max)(static_cast<int>(mi.rcWork.left), (std::min)(x, static_cast<int>(mi.rcWork.right) - w));
        y = (std::max)(static_cast<int>(mi.rcWork.top), (std::min)(y, static_cast<int>(mi.rcWork.bottom) - hgt));
    }
    hwnd_ = CreateWindowExW(testOffscreen ? WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE : 0, kClass, tr(S::RemoteGuestNewTitle),
                            WS_POPUP | WS_CLIPCHILDREN | WS_SYSMENU, x, y, w, hgt, owner, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    dpi_ = GetDpiForWindow(hwnd_);
    const DWORD round = 2;
    DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &round, sizeof(round));
    edit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd_, nullptr, nullptr, nullptr);
    SendMessageW(edit_, EM_SETLIMITTEXT, kMaxName, 0);
    SetWindowSubclass(edit_, editProc, kEditSubclass, reinterpret_cast<DWORD_PTR>(hwnd_));
    applyEditFont();
    layout();
    impl_->fade.begin(hwnd_);
    ShowWindow(hwnd_, testOffscreen ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
    if (!testOffscreen) SetForegroundWindow(hwnd_);
    return true;
}

bool RemoteGuestPanel::open(HWND owner, const std::wstring& suggestedName, Callbacks cb) {
    close();
    req_ = Request{};
    req_.name = suggestedName;
    req_.life = 2;  // 24 小時
    filesPref_ = kGuestFilesDefault, req_.files = guestFilesEffective(req_.viewOnly, filesPref_);  // 0.8.3.5
    result_ = false;
    if (!create(owner, std::move(cb))) return false;
    SetWindowTextW(edit_, suggestedName.c_str());
    ShowWindow(edit_, SW_SHOW);
    if (!testOffscreen) {
        SetFocus(edit_);
        SendMessageW(edit_, EM_SETSEL, 0, -1);
    }
    return true;
}

bool RemoteGuestPanel::showResult(HWND owner, const Result& res, Callbacks cb) {
    close();
    res_ = res;
    result_ = true;
    if (!create(owner, std::move(cb))) return false;
    SetWindowTextW(hwnd_, tr(res.reset ? S::RemoteGuestResetTitle : S::RemoteGuestMadeTitle));
    return true;
}

void RemoteGuestPanel::close() {
    if (!hwnd_) return;
    if (edit_) SetWindowTextW(edit_, L"");
    std::fill(res_.password.begin(), res_.password.end(), L'\0');
    res_ = Result{};
    prepareCloseFade(hwnd_);
    HWND h = hwnd_;
    hwnd_ = nullptr;
    edit_ = nullptr;
    closeWithFade(h);
    destroyTarget();
    delete impl_;
    impl_ = nullptr;
    if (editFont_) DeleteObject(editFont_), editFont_ = nullptr;
    if (editBrush_) DeleteObject(editBrush_), editBrush_ = nullptr;
    if (owner_ && IsWindowVisible(owner_) && !testOffscreen) SetForegroundWindow(owner_);
}

void RemoteGuestPanel::destroyTarget() {
    if (!impl_) return;
    impl_->brush.Reset();
    impl_->rt.Reset();
}

void RemoteGuestPanel::retheme() {
    if (!impl_) return;
    impl_->th = theme();
    if (editBrush_) DeleteObject(editBrush_), editBrush_ = nullptr;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE), InvalidateRect(edit_, nullptr, TRUE);
}

void RemoteGuestPanel::relabel() {
    if (!hwnd_) return;
    fx().init();
    applyEditFont();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void RemoteGuestPanel::applyEditFont() {
    if (editFont_) DeleteObject(editFont_);
    LOGFONTW lf{};
    lf.lfHeight = -static_cast<LONG>(std::lround(16 * s()));
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Consolas");
    editFont_ = CreateFontIndirectW(&lf);
    if (edit_) SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
}

void RemoteGuestPanel::layout() {
    if (!hwnd_) return;
    const int lineH = static_cast<int>(std::lround(22 * s()));
    if (edit_) {
        const int x = static_cast<int>(std::lround((kPad + 14) * s()));
        const int y = static_cast<int>(std::lround((kNameY + kFieldH / 2) * s())) - lineH / 2;
        const int w = static_cast<int>(std::lround((kW - 2 * kPad - 28) * s()));
        SetWindowPos(edit_, nullptr, x, y, w, lineH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (impl_ && impl_->rt) {
        RECT r{};
        GetClientRect(hwnd_, &r);
        impl_->rt->Resize(D2D1::SizeU(r.right, r.bottom));
        impl_->rt->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
    }
}

void RemoteGuestPanel::setStatus(const std::wstring& t, bool good) {
    status_ = t, statusGood_ = good;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void RemoteGuestPanel::submit() {
    if (result_ || !edit_) return;
    wchar_t buf[kMaxName + 2] = {};
    GetWindowTextW(edit_, buf, kMaxName + 1);
    req_.name = buf;
    req_.files = guestFilesEffective(req_.viewOnly, req_.files);  // 僅觀看 never carries 傳檔
    std::wstring err;
    Result res;
    if (!cb_.onCreate || !cb_.onCreate(req_, res, err)) {
        setStatus(err);
        if (!testOffscreen) SetFocus(edit_);
        return;
    }
    res_ = std::move(res);
    std::fill(res.password.begin(), res.password.end(), L'\0');
    result_ = true;
    status_.clear();
    ShowWindow(edit_, SW_HIDE);
    SetWindowTextW(hwnd_, tr(res_.reset ? S::RemoteGuestResetTitle : S::RemoteGuestMadeTitle));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void RemoteGuestPanel::testSet(const std::wstring& name, int life, bool viewOnly, bool files, bool doCreate) {
    if (!hwnd_ || result_) return;
    if (!name.empty()) SetWindowTextW(edit_, name.c_str());
    req_.life = std::clamp(life, 0, 3), req_.viewOnly = viewOnly, filesPref_ = files;
    req_.files = guestFilesEffective(viewOnly, filesPref_);
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (doCreate) submit();
}

void RemoteGuestPanel::testCopy() { click(HitPrimary); }

RemoteGuestPanel::Hit RemoteGuestPanel::hitTest(POINT pt) const {
    const float x = pt.x / s(), y = pt.y / s();
    auto in = [&](D2D1_RECT_F r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
    if (in(rc(kW - 52, 12, 36, 36))) return HitClose;
    const float bw = (kW - 2 * kPad - 12) / 2;
    if (in(rc(kPad, kBtnY, bw, kBtnH))) return HitSecondary;
    if (in(rc(kPad + bw + 12, kBtnY, bw, kBtnH))) return HitPrimary;
    if (!result_) {
        const float cw4 = (kW - 2 * kPad - 3 * 8) / 4, cw2 = (kW - 2 * kPad - 8) / 2;
        for (int i = 0; i < 4; ++i)
            if (in(rc(kPad + i * (cw4 + 8), kLifeY, cw4, kChipH))) return static_cast<Hit>(HitLife0 + i);
        for (int i = 0; i < 2; ++i)
            if (in(rc(kPad + i * (cw2 + 8), kRightY, cw2, kChipH))) return static_cast<Hit>(HitRight0 + i);
        if (in(rc(kPad, kFilesY, kW - 2 * kPad, kFilesH))) return HitFiles;
    }
    return HitNone;
}

void RemoteGuestPanel::click(Hit h) {
    switch (h) {
    case HitClose:
    case HitSecondary: {
        auto cb = cb_.onClosed;
        close();
        if (cb) cb();
        break;
    }
    case HitPrimary:
        if (!result_) submit();
        else {
            if (cb_.onCopy) cb_.onCopy(copyText(res_));
            setStatus(tr(S::RemoteGuestCopied), true);
        }
        break;
    case HitLife0:
    case HitLife1:
    case HitLife2:
    case HitLife3: req_.life = h - HitLife0, InvalidateRect(hwnd_, nullptr, FALSE); break;
    case HitRight0:
    case HitRight1:
        req_.viewOnly = h == HitRight1, req_.files = guestFilesEffective(req_.viewOnly, filesPref_);  // 0.8.3.5
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    case HitFiles:
        if (req_.viewOnly) break;  // 僅觀看: no files (the row says so)
        filesPref_ = !filesPref_, req_.files = filesPref_, InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    default: break;
    }
}

void RemoteGuestPanel::paint() {
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

bool RemoteGuestPanel::renderPng(const std::wstring& path, UINT dpi) {
    if (!hwnd_ || !impl_) return false;
    return renderToPng(kW, kH, dpi ? dpi : dpi_, path, [this](ID2D1RenderTarget* rt) {
        ComPtr<ID2D1SolidColorBrush> b;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &b);
        if (b) draw(rt, b.Get(), true);
    });
}

void RemoteGuestPanel::draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* b, bool png) {
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
    auto text = [&](const std::wstring& str, IDWriteTextFormat* fmt, D2D1_RECT_F r, D2D1_COLOR_F c, bool keep = true) {
        b->SetColor(c);
        const std::wstring w = keep ? pm::i18n::keepWords(str) : str;
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
    const std::wstring title = !result_ ? tr(S::RemoteGuestNewTitle) : tr(res_.reset ? S::RemoteGuestResetTitle : S::RemoteGuestMadeTitle);
    text(title, f.title.Get(), rc(kPad, 18, kW - 2 * kPad - 40, 32), t.fg);
    {
        const D2D1_RECT_F cr = rc(kW - 52, 12, 36, 36);
        if (const float h = hl(HitClose); h > 0.003f) fill(cr, D2D1_COLOR_F{t.accent.r, t.accent.g, t.accent.b, 0.22f * h}, 8);
        b->SetColor(animMix(t.dim, t.fg, hl(HitClose)));
        const float cx = kW - 34, cy = 30, d = 5.5f;
        rt->DrawLine({cx - d, cy - d}, {cx + d, cy + d}, b, 1.6f);
        rt->DrawLine({cx - d, cy + d}, {cx + d, cy - d}, b, 1.6f);
    }
    auto chip = [&](Hit h, D2D1_RECT_F r, const std::wstring& label, bool on) {
        const float k = hl(h);
        fill(r, on ? D2D1_COLOR_F{t.accent.r, t.accent.g, t.accent.b, 0.92f}
                   : D2D1_COLOR_F{t.fg.r, t.fg.g, t.fg.b, 0.06f + 0.08f * k}, 9);
        if (!on) stroke(rc(r.left + 0.5f, r.top + 0.5f, r.right - r.left - 1, r.bottom - r.top - 1), D2D1_COLOR_F{t.fg.r, t.fg.g, t.fg.b, 0.2f}, 9);
        text(label, f.chip.Get(), r, on ? t.onAccent : t.fg);
    };
    if (!result_) {
        text(tr(S::RemoteGuestNewBody), f.body.Get(), rc(kPad, kBodyY, kW - 2 * kPad, 48), t.dim);
        text(tr(S::RemoteGuestName), f.label.Get(), rc(kPad, kNameLblY, kW - 2 * kPad, 22), t.fg);
        fill(rc(kPad, kNameY, kW - 2 * kPad, kFieldH), t.field, 8);
        const bool focus = !png && GetFocus() == edit_;
        stroke(rc(kPad + 0.5f, kNameY + 0.5f, kW - 2 * kPad - 1, kFieldH - 1), focus ? t.accent : D2D1_COLOR_F{t.fg.r, t.fg.g, t.fg.b, 0.16f}, 8,
               focus ? 1.5f : 1);
        if (png && edit_) {  // what is typed (the EDIT itself is not in a PNG)
            wchar_t buf[kMaxName + 2] = {};
            GetWindowTextW(edit_, buf, kMaxName + 1);
            text(buf, f.value.Get(), rc(kPad + 14, kNameY + 6, kW - 2 * kPad - 28, 28), t.fg, false);
        }
        text(tr(S::RemoteGuestLife), f.label.Get(), rc(kPad, kLifeLblY, kW - 2 * kPad, 22), t.fg);
        const float cw4 = (kW - 2 * kPad - 3 * 8) / 4, cw2 = (kW - 2 * kPad - 8) / 2;
        for (int i = 0; i < 4; ++i)
            chip(static_cast<Hit>(HitLife0 + i), rc(kPad + i * (cw4 + 8), kLifeY, cw4, kChipH), tr(kLife[i]), req_.life == i);
        text(tr(S::RemoteGuestRights), f.label.Get(), rc(kPad, kRightLblY, kW - 2 * kPad, 22), t.fg);
        chip(HitRight0, rc(kPad, kRightY, cw2, kChipH), tr(S::RemoteGuestControl), !req_.viewOnly);
        chip(HitRight1, rc(kPad + cw2 + 8, kRightY, cw2, kChipH), tr(S::RemoteGuestView), req_.viewOnly);
        {  // 可以傳檔: a check box row right under 權限 (its own switch, not part of 僅觀看; greyed in 僅觀看)
            const D2D1_RECT_F r = rc(kPad, kFilesY, kW - 2 * kPad, kFilesH);
            const bool off = req_.viewOnly;
            const float k = off ? 0.0f : hl(HitFiles);
            fill(r, D2D1_COLOR_F{t.fg.r, t.fg.g, t.fg.b, off ? 0.03f : 0.05f}, 8);
            if (k > 0.003f) fill(r, D2D1_COLOR_F{t.accent.r, t.accent.g, t.accent.b, 0.12f * k}, 8);
            stroke(rc(r.left + 0.5f, r.top + 0.5f, r.right - r.left - 1, r.bottom - r.top - 1),
                   req_.files ? D2D1_COLOR_F{t.accent.r, t.accent.g, t.accent.b, 0.7f} : D2D1_COLOR_F{t.fg.r, t.fg.g, t.fg.b, off ? 0.1f : 0.2f}, 8);
            const D2D1_RECT_F box = rc(kPad + 8, kFilesY + (kFilesH - 18) / 2, 18, 18);
            if (req_.files) {
                fill(box, t.accent, 4);
                b->SetColor(t.onAccent);
                rt->DrawLine({box.left + 4, box.top + 9.5f}, {box.left + 7.5f, box.top + 13}, b, 2);
                rt->DrawLine({box.left + 7.5f, box.top + 13}, {box.left + 14, box.top + 5}, b, 2);
            } else {
                stroke(box, D2D1_COLOR_F{t.fg.r, t.fg.g, t.fg.b, off ? 0.25f : 0.55f}, 4, 1.4f);
            }
            text(tr(off ? S::RemoteGuestFilesViewOff0835 : S::RemoteGuestFilesCan0835), f.body.Get(),
                 rc(kPad + 36, kFilesY + 7, kW - 2 * kPad - 44, 22), off ? t.dim : t.fg);
        }
    } else {
        text(tr(S::RemoteGuestUrl), f.label.Get(), rc(kPad, kUrlLblY, kW - 2 * kPad, 22), t.dim);
        text(res_.url, f.url.Get(), rc(kPad, kUrlY, kW - 2 * kPad, kUrlH), t.fg, false);
        text(tr(S::RemoteGuestName), f.label.Get(), rc(kPad, kNameRLblY, kW - 2 * kPad, 22), t.dim);
        text(res_.name, f.value.Get(), rc(kPad, kNameRY, kW - 2 * kPad, 34), t.fg, false);
        text(tr(S::RemoteGuestPw), f.label.Get(), rc(kPad, kPwLblY, kW - 2 * kPad, 22), t.dim);
        fill(rc(kPad, kPwY, kW - 2 * kPad, 48), t.field, 8);
        std::wstring spaced;  // 「ABC 234」: easier to read out
        for (size_t i = 0; i < res_.password.size(); ++i) spaced += (i == 3 ? L" " : L""), spaced += res_.password[i];
        text(spaced, f.pw.Get(), rc(kPad + 16, kPwY + 2, kW - 2 * kPad - 32, 44), t.accent, false);
        text(res_.summary, f.body.Get(), rc(kPad, kSumY, kW - 2 * kPad, 22), t.fg);
        text(tr(S::RemoteGuestOnceNote), f.body.Get(), rc(kPad, kNoteY, kW - 2 * kPad, kNoteH), t.dim);
    }
    if (!status_.empty()) text(status_, f.status.Get(), rc(kPad, kStatusY, kW - 2 * kPad, kStatusH), statusGood_ ? t.good : t.error);
    const float bw = (kW - 2 * kPad - 12) / 2;
    {
        const D2D1_RECT_F r = rc(kPad, kBtnY, bw, kBtnH);
        pressAt(HitSecondary, r);
        fill(r, D2D1_COLOR_F{t.fg.r, t.fg.g, t.fg.b, 0.07f + 0.07f * hl(HitSecondary)}, 9);
        stroke(rc(kPad + 0.5f, kBtnY + 0.5f, bw - 1, kBtnH - 1), D2D1_COLOR_F{t.fg.r, t.fg.g, t.fg.b, 0.2f}, 9);
        text(tr(result_ ? S::RemoteGuestDone : S::Cancel), f.button.Get(), r, t.fg);
        rt->SetTransform(base);
    }
    {
        const D2D1_RECT_F r = rc(kPad + bw + 12, kBtnY, bw, kBtnH);
        pressAt(HitPrimary, r);
        fill(r, mix(t.accent, t.light ? D2D1_COLOR_F{0, 0, 0, 1} : D2D1_COLOR_F{1, 1, 1, 1}, 0.15f * hl(HitPrimary)), 9);
        text(tr(result_ ? S::RemoteGuestCopyAll : S::RemoteGuestCreate), f.button.Get(), r, t.onAccent);
        rt->SetTransform(base);
    }
}

LRESULT RemoteGuestPanel::handle(UINT msg, WPARAM wp, LPARAM lp) {
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
        applyEditFont();
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
        else if (wp == VK_RETURN && !result_) submit();
        return 0;
    case WM_APP + 40:
        if (wp == VK_ESCAPE) click(HitClose);
        else if (wp == VK_RETURN) submit();
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
