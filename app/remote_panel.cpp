// 遠端操控 panel and banner (see remote_panel.h, docs/remote.md). Built like
// the 傳到手機 panel: owned WS_POPUP, Direct2D HWND render target in the
// popup menus' palette, Windows 11 rounded corners, per-monitor DPI.
#include "remote_panel.h"

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

constexpr wchar_t kPanelClass[] = L"PhoneMirrorRemotePanel";
constexpr wchar_t kBannerClass[] = L"PhoneMirrorRemoteBanner";
constexpr float kW = 440, kH = 600, kPad = 26;  // 0.8.2: wider for a fourth tab (固定網址)
constexpr float kTabY = 62, kTabH = 32;
constexpr float kScanY = 106;
constexpr float kQrCard = 216, kQrY = 152;
constexpr float kUrlY = 378, kCopyY = 398, kLinkH = 22;
constexpr float kStatusY = 432, kNoteY = 466;
constexpr float kBtnH = 38, kBtnY = kH - 22 - kBtnH;
constexpr float kBannerH = 40;

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
    D2D1_COLOR_F top, bottom, fg, dim, accent, onAccent, live;
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
    t.onAccent = lum(t.accent) > 0.55f ? mix(D2D1_COLOR_F{0.13f, 0.09f, 0.10f, 1}, t.accent, 0.10f) : D2D1_COLOR_F{1, 1, 1, 1};
    t.live = D2D1_COLOR_F{0.90f, 0.22f, 0.22f, 1};  // the 「正在被遠端操控」 dot: red in every theme
    return t;
}

struct Factories {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IDWriteTextFormat> title, scan, note, url, link, status, button, tab, banner, bannerBtn, code;
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
        scan = fmt(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, C, true);
        note = fmt(12.5f, DWRITE_FONT_WEIGHT_NORMAL, C, true);
        url = fmt(11.5f, DWRITE_FONT_WEIGHT_NORMAL, C, false, true, L"Consolas");
        link = fmt(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, C, false);
        status = fmt(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, C, true);
        button = fmt(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, C, false);
        tab = fmt(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, C, false, true);
        banner = fmt(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, L, false, true);
        bannerBtn = fmt(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, C, false);
        code = fmt(21, DWRITE_FONT_WEIGHT_BOLD, C, false);  // 免安裝外網: the 通行碼
        for (auto* f : {button.Get(), tab.Get(), banner.Get(), bannerBtn.Get(), code.Get()})
            if (f) f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        return title && scan && note && url && link && status && button && tab && banner && bannerBtn && code;
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

// =============================================================== RemotePanel ==

struct RemotePanel::Impl {
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1Bitmap> qr;
    int qrTab = -1;  // which tab's QR the bitmap holds
    Theme th = theme();
    HoverAnim anim;
    PanelFade fade;
};

bool RemotePanel::open(HWND owner, Callbacks cb) {
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
        wc.lpfnWndProc = proc<RemotePanel>;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kPanelClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    owner_ = owner;
    cb_ = std::move(cb);
    hot_ = pressed_ = HitNone;
    impl_ = new Impl;
    RECT orc{};
    const bool ownerVisible = owner && IsWindowVisible(owner) && !IsIconic(owner);
    if (ownerVisible) GetWindowRect(owner, &orc);
    HMONITOR mon = ownerVisible ? MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
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
    hwnd_ = CreateWindowExW(0, kPanelClass, tr(S::RemotePanelTitle), WS_POPUP | WS_SYSMENU, x, y, w, hgt, owner, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    dpi_ = GetDpiForWindow(hwnd_);
    const DWORD round = 2;
    DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &round, sizeof(round));
    impl_->fade.begin(hwnd_);
    ShowWindow(hwnd_, testOffscreen ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
    if (!testOffscreen) SetForegroundWindow(hwnd_);
    return true;
}

void RemotePanel::close() {
    if (!hwnd_) return;
    prepareCloseFade(hwnd_);
    HWND h = hwnd_;
    hwnd_ = nullptr;
    closeWithFade(h);
    destroyTarget();
    delete impl_;
    impl_ = nullptr;
    if (owner_ && IsWindowVisible(owner_) && !testOffscreen) SetForegroundWindow(owner_);
}

void RemotePanel::destroyTarget() {
    if (!impl_) return;
    impl_->qr.Reset();
    impl_->qrTab = -1;
    impl_->brush.Reset();
    impl_->rt.Reset();
}

void RemotePanel::setUrls(const std::string& lanUrl, const std::vector<uint8_t>& lanQr, int lanQrSize, const std::string& tsUrl,
                          const std::vector<uint8_t>& tsQr, int tsQrSize) {
    auto ok = [](const std::vector<uint8_t>& q, int n) { return n > 0 && q.size() >= static_cast<size_t>(n) * n * 4; };
    lanUrl_ = lanUrl, tsUrl_ = tsUrl;
    lanQr_ = ok(lanQr, lanQrSize) ? lanQr : std::vector<uint8_t>();
    lanQrSize_ = ok(lanQr, lanQrSize) ? lanQrSize : 0;
    tsQr_ = ok(tsQr, tsQrSize) ? tsQr : std::vector<uint8_t>();
    tsQrSize_ = ok(tsQr, tsQrSize) ? tsQrSize : 0;
    if (impl_) impl_->qr.Reset(), impl_->qrTab = -1;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void RemotePanel::setStatus(const std::wstring& text, bool active) {
    status_ = text;
    active_ = active;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void RemotePanel::setTunnel(int state, const std::string& link, const std::vector<uint8_t>& qr, int qrSize, const std::string& code,
                            const std::wstring& err) {
    tunState_ = std::clamp(state, 0, 3);
    tunLink_ = link, tunCode_ = code, tunErr_ = err;
    const bool ok = qrSize > 0 && qr.size() >= static_cast<size_t>(qrSize) * qrSize * 4;
    tunQr_ = ok ? qr : std::vector<uint8_t>();
    tunQrSize_ = ok ? qrSize : 0;
    if (impl_ && tab_ == 2) impl_->qr.Reset(), impl_->qrTab = -1;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void RemotePanel::setFixed(int state, const std::string& url, const std::vector<uint8_t>& qr, int qrSize, const std::wstring& info,
                           const std::wstring& err, bool hasPassword) {
    fixState_ = std::clamp(state, 0, 3);
    fixUrl_ = url, fixInfo_ = info, fixErr_ = err, fixHasPw_ = hasPassword;
    const bool ok = qrSize > 0 && qr.size() >= static_cast<size_t>(qrSize) * qrSize * 4;
    fixQr_ = ok ? qr : std::vector<uint8_t>();
    fixQrSize_ = ok ? qrSize : 0;
    if (impl_ && tab_ == 3) impl_->qr.Reset(), impl_->qrTab = -1;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void RemotePanel::showTab(int tab) {
    tab_ = std::clamp(tab, 0, 3);
    if (impl_) impl_->qr.Reset(), impl_->qrTab = -1;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void RemotePanel::retheme() {
    if (!impl_) return;
    impl_->th = theme();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void RemotePanel::relabel() {
    if (!hwnd_) return;
    fx().init();
    SetWindowTextW(hwnd_, tr(S::RemotePanelTitle));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

RemotePanel::Hit RemotePanel::hitTest(POINT pt) const {
    const float x = pt.x / s(), y = pt.y / s();
    auto in = [&](D2D1_RECT_F r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
    if (in(rc(kW - 52, 12, 36, 36))) return HitClose;
    const float tw = (kW - 2 * kPad) / 4;
    if (in(rc(kPad, kTabY, tw, kTabH))) return HitTabLan;
    if (in(rc(kPad + tw, kTabY, tw, kTabH))) return HitTabTs;
    if (in(rc(kPad + 2 * tw, kTabY, tw, kTabH))) return HitTabTun;
    if (in(rc(kPad + 3 * tw, kTabY, tw, kTabH))) return HitTabFix;
    if (tab_ == 3) {
        const float cardX = (kW - kQrCard) / 2;
        if ((fixState_ == 0 || fixState_ == 3) && in(rc(cardX + 12, kQrY + kQrCard - 54, kQrCard - 24, 40))) return HitFixStart;
        if (fixState_ == 2 && in(rc(kW / 2 - copyW_ / 2 - 6, kCopyY - 2, copyW_ + 12, kLinkH + 4))) return HitCopy;
        if ((fixState_ == 1 || fixState_ == 2) && in(rc(kW * 0.33f - fixStopW_ / 2 - 6, kNoteY + 44, fixStopW_ + 12, kLinkH + 4))) return HitFixStop;
        if (fixState_ == 2 && in(rc(kW * 0.75f - fixPwW_ / 2 - 6, kNoteY + 44, fixPwW_ + 12, kLinkH + 4))) return HitFixPw;
    } else if (tab_ == 2) {
        const float cardX = (kW - kQrCard) / 2;
        if ((tunState_ == 0 || tunState_ == 3) && in(rc(cardX + 12, kQrY + kQrCard - 54, kQrCard - 24, 40))) return HitTunStart;
        if (tunState_ == 2 && in(rc(kW / 2 - copyW_ / 2 - 6, kCopyY + 4, copyW_ + 12, kLinkH + 4))) return HitCopy;
        if ((tunState_ == 1 || tunState_ == 2) && in(rc(kW / 2 - stopW_ / 2 - 6, kNoteY + 44, stopW_ + 12, kLinkH + 4))) return HitTunStop;
    } else {
    const bool haveUrl = tab_ ? !tsUrl_.empty() : !lanUrl_.empty();
    if (haveUrl && in(rc(kW / 2 - copyW_ / 2 - 6, kCopyY - 2, copyW_ + 12, kLinkH + 4))) return HitCopy;
    if (tab_ && tsUrl_.empty() && in(rc(kW / 2 - helpW_ / 2 - 6, kQrY + kQrCard - 40, helpW_ + 12, kLinkH + 8))) return HitTsHelp;
    if (tab_ && !tsUrl_.empty() && in(rc(kW / 2 - helpW_ / 2 - 6, kNoteY + 44, helpW_ + 12, kLinkH + 4))) return HitTsHelp;
    }
    const float bw = (kW - 2 * kPad - 12) / 2;
    if (in(rc(kPad, kBtnY, bw, kBtnH))) return HitSecondary;
    if (in(rc(kPad + bw + 12, kBtnY, bw, kBtnH))) return HitPrimary;
    return HitNone;
}

void RemotePanel::click(Hit h) {
    switch (h) {
    case HitClose:
    case HitPrimary: {  // ×, 完成: hide (remote control stays on)
        auto cb = cb_.onHide;
        close();
        if (cb) cb();
        break;
    }
    case HitSecondary: {  // 關閉遠端操控
        auto cb = cb_.onStop;
        close();
        if (cb) cb();
        break;
    }
    case HitCopy:
        if (cb_.onCopy) cb_.onCopy(tab_ == 3 ? fixUrl_ : tab_ == 2 ? tunLink_ : tab_ ? tsUrl_ : lanUrl_);
        break;
    case HitTabLan: showTab(0); break;
    case HitTabTs: showTab(1); break;
    case HitTabTun: showTab(2); break;
    case HitTabFix: showTab(3); break;
    case HitFixStart:
        if (cb_.onFixedStart) cb_.onFixedStart();
        break;
    case HitFixStop:
        if (cb_.onFixedStop) cb_.onFixedStop();
        break;
    case HitFixPw:
        if (cb_.onFixedPassword) cb_.onFixedPassword();
        break;
    case HitTunStart:
        if (cb_.onTunnelStart) cb_.onTunnelStart();
        break;
    case HitTunStop:
        if (cb_.onTunnelStop) cb_.onTunnelStop();
        break;
    case HitTsHelp:
        if (cb_.onTailscaleHelp) cb_.onTailscaleHelp();
        break;
    default: break;
    }
}

void RemotePanel::paint() {
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
    if (im.qrTab != tab_) {
        im.qr.Reset();
        if (qrSize() > 0) {
            D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
            im.rt->CreateBitmap(D2D1::SizeU(qrSize(), qrSize()), qr().data(), qrSize() * 4, bp, &im.qr);
        }
        im.qrTab = tab_;
    }
    im.rt->BeginDraw();
    if (const float k = im.fade.scale(); k < 1) {
        const D2D1_SIZE_F sz = im.rt->GetSize();
        im.rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k, {sz.width / 2, sz.height / 2}));
    }
    draw(im.rt.Get(), im.brush.Get(), im.qr.Get());
    im.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    if (im.rt->EndDraw() == D2DERR_RECREATE_TARGET) destroyTarget();
    im.fade.painted();
}

bool RemotePanel::renderPng(const std::wstring& path) {
    if (!hwnd_ || !impl_) return false;
    return renderToPng(kW, kH, dpi_, path, [this](ID2D1RenderTarget* rt) {
        ComPtr<ID2D1SolidColorBrush> b;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &b);
        ComPtr<ID2D1Bitmap> q;
        if (qrSize() > 0) {
            D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
            rt->CreateBitmap(D2D1::SizeU(qrSize(), qrSize()), qr().data(), qrSize() * 4, bp, &q);
        }
        if (b) draw(rt, b.Get(), q.Get());
    });
}

void RemotePanel::draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* b, ID2D1Bitmap* qrBitmap) {
    Factories& f = fx();
    const Theme& t = impl_->th;
    Impl& im = *impl_;
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
    auto stroke = [&](D2D1_RECT_F r, D2D1_COLOR_F c, float radius) {
        b->SetColor(c);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b, 1);
    };
    auto text = [&](const std::wstring& str, IDWriteTextFormat* fmt, D2D1_RECT_F r, D2D1_COLOR_F c) {
        b->SetColor(c);
        const std::wstring w = pm::i18n::keepWords(str);
        rt->DrawTextW(w.c_str(), static_cast<UINT32>(w.size()), fmt, r, b);
    };
    auto link = [&](const std::wstring& l, float y, Hit id, float& wOut) {
        wOut = f.width(f.link.Get(), l);
        const float ch = hl(id);
        text(l, f.link.Get(), rc(kPad, y, kW - 2 * kPad, kLinkH), animMix(t.accent, t.fg, ch));
        b->SetColor(withA(t.accent, 0.45f + 0.45f * ch));
        rt->DrawLine({kW / 2 - wOut / 2, y + kLinkH - 2}, {kW / 2 + wOut / 2, y + kLinkH - 2}, b, 1);
    };
    cardBackground(rt, b, t, kW, kH, true);
    text(tr(S::RemotePanelTitle), f.title.Get(), rc(kPad, 18, kW - 2 * kPad - 40, 32), t.fg);
    {
        const D2D1_RECT_F cr = rc(kW - 52, 12, 36, 36);
        if (const float h = hl(HitClose); h > 0.003f) fill(cr, withA(t.accent, 0.22f * h), 8);
        b->SetColor(animMix(t.dim, t.fg, hl(HitClose)));
        const float cx = kW - 34, cy = 30, d = 5.5f;
        rt->DrawLine({cx - d, cy - d}, {cx + d, cy + d}, b, 1.6f);
        rt->DrawLine({cx - d, cy + d}, {cx + d, cy - d}, b, 1.6f);
    }
    // Tabs: 同一個 Wi-Fi | Tailscale | 免安裝外網 | 固定網址 (0.8.2)
    {
        const float tw = (kW - 2 * kPad) / 4;
        stroke(rc(kPad + 0.5f, kTabY + 0.5f, kW - 2 * kPad - 1, kTabH - 1), withA(t.fg, 0.25f), 9);
        static const S kTabLabels[4] = {S::RemoteTabLan, S::RemoteTabTsShort, S::RemoteTabTunnel, S::RemoteTabFixed};
        for (int i = 0; i < 4; ++i) {
            const D2D1_RECT_F r = rc(kPad + i * tw, kTabY, tw, kTabH);
            const Hit id = i == 3 ? HitTabFix : i == 2 ? HitTabTun : i ? HitTabTs : HitTabLan;
            if (tab_ == i) fill(rc(r.left + 2, r.top + 2, tw - 4, kTabH - 4), t.accent, 7);
            else if (const float h = hl(id); h > 0.003f) fill(rc(r.left + 2, r.top + 2, tw - 4, kTabH - 4), withA(t.accent, 0.18f * h), 7);
            text(tr(kTabLabels[i]), f.tab.Get(), rc(r.left + 4, r.top, tw - 8, kTabH), tab_ == i ? t.onAccent : t.fg);
        }
    }
    auto smallButton = [&](D2D1_RECT_F r, const std::wstring& label, Hit id) {
        const float hot = hl(id);
        pressAt(id, r);
        fill(r, mix(t.accent, t.light ? D2D1_COLOR_F{0, 0, 0, 1} : D2D1_COLOR_F{1, 1, 1, 1}, 0.15f * hot), 9);
        text(label, f.button.Get(), r, t.onAccent);
        rt->SetTransform(base);
    };
    auto linkAt = [&](const std::wstring& l, float cx, float y, Hit id, float& wOut) {
        wOut = (std::min)(f.width(f.link.Get(), l), kW / 2 - kPad);
        const float ch = hl(id);
        text(l, f.link.Get(), rc(cx - wOut / 2 - 4, y, wOut + 8, kLinkH), animMix(t.accent, t.fg, ch));
        b->SetColor(withA(t.accent, 0.45f + 0.45f * ch));
        rt->DrawLine({cx - wOut / 2, y + kLinkH - 2}, {cx + wOut / 2, y + kLinkH - 2}, b, 1);
    };
    if (tab_ == 3) {
        // 常駐外網 (0.8.2): the fixed address (Tailscale Funnel :8443) + the owner's password.
        const float cardX = (kW - kQrCard) / 2;
        text(tr(fixState_ == 2 ? S::RemoteScanFixed : S::RemoteFixedTitle), f.scan.Get(), rc(kPad, kScanY, kW - 2 * kPad, 40), t.fg);
        if (fixState_ == 2) {
            fill(rc(cardX, kQrY, kQrCard, kQrCard), D2D1_COLOR_F{1, 1, 1, 1}, 14);
            if (qrBitmap && qrSize() > 0) {
                const float sc = s();
                const int availPx = static_cast<int>(std::floor((kQrCard - 16) * sc));
                const int k = (std::max)(1, availPx / qrSize());
                const float drawPx = static_cast<float>(k * qrSize());
                const float leftPx = std::round((cardX + kQrCard / 2) * sc - drawPx / 2);
                const float topPx = std::round((kQrY + kQrCard / 2) * sc - drawPx / 2);
                rt->DrawBitmap(qrBitmap, rc(leftPx / sc, topPx / sc, drawPx / sc, drawPx / sc), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            }
            text(std::wstring(fixUrl_.begin(), fixUrl_.end()), f.url.Get(), rc(kPad - 8, kUrlY, kW - 2 * kPad + 16, 18), t.dim);
            link(tr(S::RemoteFixedCopy), kCopyY, HitCopy, copyW_);
        } else {
            fill(rc(cardX - 10, kQrY, kQrCard + 20, kQrCard), withA(t.fg, 0.06f), 14);
            if (fixState_ == 1) {
                if (fixInfo_.empty()) {
                    text(tr(S::RemoteFixedStarting), f.scan.Get(), rc(cardX + 6, kQrY + kQrCard / 2 - 20, kQrCard - 12, 40), t.fg);
                } else {  // 0.8.3.1: 等待 Tailscale 啟動…（n 秒後再試）, two lines
                    text(fixInfo_, f.note.Get(), rc(cardX + 6, kQrY + kQrCard / 2 - 30, kQrCard - 12, 60), t.fg);
                }
            } else {
                const std::wstring body = fixState_ == 3 ? pm::i18n::fmt(S::RemoteFixedFailed, {fixErr_}) : std::wstring(tr(S::RemoteFixedIntro));
                text(body, f.note.Get(), rc(cardX + 4, kQrY + 12, kQrCard - 8, kQrCard - 72), fixState_ == 3 ? D2D1_COLOR_F{1.0f, 0.62f, 0.62f, 1} : t.fg);
                smallButton(rc(cardX + 12, kQrY + kQrCard - 54, kQrCard - 24, 40),
                            tr(fixState_ == 3 ? S::RemoteTunnelRetry : fixHasPw_ ? S::RemoteFixedStartNow : S::RemoteFixedStart), HitFixStart);
            }
        }
        if (!status_.empty()) text(status_, f.status.Get(), rc(kPad, kStatusY, kW - 2 * kPad, 40), active_ ? t.accent : t.fg);
        if (fixState_ == 2 && !fixInfo_.empty())
            text(fixInfo_, f.note.Get(), rc(kPad, kNoteY - 4, kW - 2 * kPad, 48), t.light ? D2D1_COLOR_F{0.62f, 0.32f, 0.10f, 1} : D2D1_COLOR_F{1.0f, 0.80f, 0.55f, 1});
        else text(tr(S::RemoteFixedNote), f.note.Get(), rc(kPad, kNoteY - 4, kW - 2 * kPad, 48), t.dim);
        if (fixState_ == 1 || fixState_ == 2) linkAt(tr(S::RemoteFixedStop), kW * 0.33f, kNoteY + 46, HitFixStop, fixStopW_);
        if (fixState_ == 2) linkAt(tr(S::RemoteFixedChangePw), kW * 0.75f, kNoteY + 46, HitFixPw, fixPwW_);
    } else if (tab_ == 2) {
        // 免安裝外網 (0.8.1): cloudflared quick tunnel, one-time link + 通行碼.
        const float cardX = (kW - kQrCard) / 2;
        text(tr(tunState_ == 2 ? S::RemoteScanTunnel : S::RemoteTunnelTitle), f.scan.Get(), rc(kPad, kScanY, kW - 2 * kPad, 40), t.fg);
        if (tunState_ == 2) {
            fill(rc(cardX, kQrY, kQrCard, kQrCard), D2D1_COLOR_F{1, 1, 1, 1}, 14);
            if (qrBitmap && qrSize() > 0) {
                const float sc = s();
                const int availPx = static_cast<int>(std::floor((kQrCard - 16) * sc));
                const int k = (std::max)(1, availPx / qrSize());
                const float drawPx = static_cast<float>(k * qrSize());
                const float leftPx = std::round((cardX + kQrCard / 2) * sc - drawPx / 2);
                const float topPx = std::round((kQrY + kQrCard / 2) * sc - drawPx / 2);
                rt->DrawBitmap(qrBitmap, rc(leftPx / sc, topPx / sc, drawPx / sc, drawPx / sc), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            }
            const std::wstring c = tunCode_.size() == 6 ? std::wstring(tunCode_.begin(), tunCode_.begin() + 3) + L" " +
                                                               std::wstring(tunCode_.begin() + 3, tunCode_.end())
                                                         : std::wstring(tunCode_.begin(), tunCode_.end());
            text(pm::i18n::fmt(S::RemoteTunnelCode, {c}), f.code.Get(), rc(kPad, kUrlY - 6, kW - 2 * kPad, 28), t.accent);
            float cw = 0;
            link(tr(S::RemoteFixedCopy), kCopyY + 6, HitCopy, cw);
            copyW_ = cw;
        } else {
            fill(rc(cardX - 10, kQrY, kQrCard + 20, kQrCard), withA(t.fg, 0.06f), 14);
            if (tunState_ == 1) {
                text(tr(S::RemoteTunnelStarting), f.scan.Get(), rc(cardX + 6, kQrY + kQrCard / 2 - 20, kQrCard - 12, 40), t.fg);
            } else {
                const std::wstring body = tunState_ == 3 ? pm::i18n::fmt(S::RemoteTunnelFailed, {tunErr_}) : std::wstring(tr(S::RemoteTunnelIntro));
                text(body, f.note.Get(), rc(cardX + 4, kQrY + 16, kQrCard - 8, kQrCard - 80), tunState_ == 3 ? D2D1_COLOR_F{1.0f, 0.62f, 0.62f, 1} : t.fg);
                smallButton(rc(cardX + 12, kQrY + kQrCard - 54, kQrCard - 24, 40), tr(tunState_ == 3 ? S::RemoteTunnelRetry : S::RemoteTunnelStart), HitTunStart);
            }
        }
        if (!status_.empty()) text(status_, f.status.Get(), rc(kPad, kStatusY, kW - 2 * kPad, 40), active_ ? t.accent : t.fg);
        text(tr(S::RemoteTunnelNote), f.note.Get(), rc(kPad, kNoteY - 4, kW - 2 * kPad, 48), t.dim);
        if (tunState_ == 1 || tunState_ == 2) link(tr(S::RemoteTunnelStop), kNoteY + 46, HitTunStop, stopW_);
    } else {
    const bool noTs = tab_ == 1 && tsUrl_.empty();
    if (!noTs) text(tr(tab_ ? S::RemoteScanTs : S::RemoteScanLan), f.scan.Get(), rc(kPad, kScanY, kW - 2 * kPad, 40), t.fg);

    const float cardX = (kW - kQrCard) / 2;
    if (noTs) {
        // No Tailscale on this PC: what to do instead of a QR code.
        fill(rc(cardX - 10, kQrY, kQrCard + 20, kQrCard), withA(t.fg, 0.06f), 14);
        text(tr(S::RemoteNoTs), f.note.Get(), rc(cardX + 6, kQrY + 22, kQrCard - 12, kQrCard - 80), t.fg);
        link(tr(S::RemoteTsGuide), kQrY + kQrCard - 36, HitTsHelp, helpW_);
    } else {
        fill(rc(cardX, kQrY, kQrCard, kQrCard), D2D1_COLOR_F{1, 1, 1, 1}, 14);
        if (qrBitmap && qrSize() > 0) {
            const float sc = s();
            const int availPx = static_cast<int>(std::floor((kQrCard - 16) * sc));
            const int k = (std::max)(1, availPx / qrSize());
            const float drawPx = static_cast<float>(k * qrSize());
            const float leftPx = std::round((cardX + kQrCard / 2) * sc - drawPx / 2);
            const float topPx = std::round((kQrY + kQrCard / 2) * sc - drawPx / 2);
            rt->DrawBitmap(qrBitmap, rc(leftPx / sc, topPx / sc, drawPx / sc, drawPx / sc), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        }
        const std::string& u = tab_ ? tsUrl_ : lanUrl_;
        text(std::wstring(u.begin(), u.end()), f.url.Get(), rc(kPad - 8, kUrlY, kW - 2 * kPad + 16, 18), t.dim);
        link(tr(S::RemoteFixedCopy), kCopyY, HitCopy, copyW_);
    }
    if (!status_.empty()) text(status_, f.status.Get(), rc(kPad, kStatusY, kW - 2 * kPad, 40), active_ ? t.accent : t.fg);
    text(tr(tab_ ? S::RemoteNoteTs : S::RemoteSafety), f.note.Get(), rc(kPad, kNoteY, kW - 2 * kPad, 44), t.dim);
    if (tab_ && !noTs) link(tr(S::RemoteTsGuide), kNoteY + 44, HitTsHelp, helpW_);
    }

    auto button = [&](D2D1_RECT_F r, const wchar_t* label, bool primary, Hit id) {
        const float hot = hl(id);
        pressAt(id, r);
        if (primary) {
            D2D1_COLOR_F c = mix(t.accent, t.light ? D2D1_COLOR_F{0, 0, 0, 1} : D2D1_COLOR_F{1, 1, 1, 1}, 0.15f * hot);
            fill(r, c, 9);
            text(label, f.button.Get(), r, t.onAccent);
        } else {
            fill(r, withA(t.fg, 0.07f + 0.07f * hot), 9);
            stroke(rc(r.left + 0.5f, r.top + 0.5f, r.right - r.left - 1, kBtnH - 1), withA(t.fg, 0.2f), 9);
            text(label, f.button.Get(), r, t.fg);
        }
        rt->SetTransform(base);
    };
    const float bw = (kW - 2 * kPad - 12) / 2;
    button(rc(kPad, kBtnY, bw, kBtnH), tr(S::RemoteStop), false, HitSecondary);
    button(rc(kPad + bw + 12, kBtnY, bw, kBtnH), tr(S::RemoteDone), true, HitPrimary);
}

LRESULT RemotePanel::handle(UINT msg, WPARAM wp, LPARAM lp) {
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
        if (impl_ && (impl_->anim.onTimer(h, wp) || impl_->fade.onTimer(h, wp))) return 0;
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
    case WM_LBUTTONDOWN:
        pressed_ = hitTest(POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))});
        if (impl_) impl_->anim.setPressed(h, pressed_);
        return 0;
    case WM_LBUTTONUP: {
        const Hit up = hitTest(POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))});
        const Hit was = pressed_;
        pressed_ = HitNone;
        if (impl_) impl_->anim.setPressed(h, HitNone);
        // 0.8.1: a tap on the phone landed here (owner's log 20:32:30) and turned
        // remote control off. The phone's clicks never reach 關閉遠端操控.
        if ((up == HitSecondary || up == HitTunStop || up == HitFixStop || up == HitFixPw) && fromRemoteInput()) {
            if (cb_.onRemoteRefused) cb_.onRemoteRefused();
            return 0;
        }
        if (up != HitNone && up == was) click(up);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) click(HitClose);
        else if (wp == VK_RIGHT || wp == VK_TAB) showTab((tab_ + 1) % 4);
        else if (wp == VK_LEFT) showTab((tab_ + 3) % 4);
        return 0;
    case WM_CLOSE: click(HitClose); return 0;
    case WM_DESTROY:
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ============================================================== RemoteBanner ==

struct RemoteBanner::Impl {
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> brush;
    Theme th = theme();
};

void RemoteBanner::layout() {
    Factories& f = fx();
    const std::wstring msg = pm::i18n::fmt(viewOnly_ ? S::RemoteBannerView : S::RemoteBanner, {device_});
    textW_ = (std::min)(520.0f, std::ceil(f.width(f.banner.Get(), msg)) + 2);
    viewW_ = std::ceil(f.width(f.bannerBtn.Get(), tr(S::RemoteBannerViewOnly))) + 40;
    stopW_ = std::ceil(f.width(f.bannerBtn.Get(), tr(S::RemoteBannerStop))) + 28;
}

void RemoteBanner::show(const std::wstring& device, bool viewOnly, Callbacks cb) {
    if (!fx().init()) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = proc<RemoteBanner>;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kBannerClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    device_ = device;
    viewOnly_ = viewOnly;
    cb_ = std::move(cb);
    if (!hwnd_) {
        impl_ = new Impl;
        // Topmost, never activated, not in the taskbar / Alt+Tab, no owner (stays when the app is in the tray).
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kBannerClass, tr(S::RemotePanelTitle), WS_POPUP, 0, 0, 10, 10,
                                nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (!hwnd_) {
            delete impl_;
            impl_ = nullptr;
            return;
        }
        const DWORD round = 2;
        DwmSetWindowAttribute(hwnd_, 33, &round, sizeof(round));
    }
    layout();
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const UINT dpi = monitorDpi(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    if (dpi != dpi_ && impl_) impl_->rt.Reset(), impl_->brush.Reset();
    dpi_ = dpi ? dpi : 96;
    const float sc = dpi_ / 96.0f;
    const float w = 16 + 12 + 10 + textW_ + 14 + viewW_ + 8 + stopW_ + 8;
    const int pw = static_cast<int>(std::lround(w * sc)), ph = static_cast<int>(std::lround(kBannerH * sc));
    int x = (mi.rcWork.left + mi.rcWork.right) / 2 - pw / 2, y = mi.rcWork.top + static_cast<int>(std::lround(8 * sc));
    if (testOffscreen) x = GetSystemMetrics(SM_XVIRTUALSCREEN) - pw - 2000, y = GetSystemMetrics(SM_YVIRTUALSCREEN);  // off every monitor
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, pw, ph, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (impl_ && impl_->rt) {
        impl_->rt->Resize(D2D1::SizeU(pw, ph));
        impl_->rt->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void RemoteBanner::setViewOnly(bool on) {
    if (!hwnd_) return;
    show(device_, on, cb_);
}

void RemoteBanner::hide() {
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

void RemoteBanner::retheme() {
    if (!impl_) return;
    impl_->th = theme();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void RemoteBanner::relabel() {
    if (!hwnd_) return;
    fx().init();
    show(device_, viewOnly_, cb_);
}

void RemotePanel::testClickStop(bool fromPhone) {
    if (!hwnd_) return;
    const float bw = (kW - 2 * kPad - 12) / 2;
    const int x = static_cast<int>((kPad + bw / 2) * s()), y = static_cast<int>((kBtnY + kBtnH / 2) * s());
    const LPARAM old = SetMessageExtraInfo(fromPhone ? static_cast<LPARAM>(kRemoteInputTag) : 0);
    const HWND h = hwnd_;
    SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    if (IsWindow(h)) SendMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    SetMessageExtraInfo(old);
}

void RemoteBanner::testClick(int which, bool fromPhone) {
    if (!hwnd_) return;
    const float k = static_cast<float>(dpi_ ? dpi_ : 96) / 96.0f;
    const float v0 = 16 + 12 + 10 + textW_ + 14;
    const float cx = which == 1 ? v0 + viewW_ / 2 : v0 + viewW_ + 8 + stopW_ / 2;
    const int x = static_cast<int>(cx * k), y = static_cast<int>(kBannerH / 2 * k);
    const LPARAM old = SetMessageExtraInfo(fromPhone ? static_cast<LPARAM>(kRemoteInputTag) : 0);
    SendMessageW(hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    SetMessageExtraInfo(old);
}

int RemoteBanner::hitAt(int xPx) const {
    const float x = xPx * 96.0f / static_cast<float>(dpi_ ? dpi_ : 96);
    const float v0 = 16 + 12 + 10 + textW_ + 14;
    if (x >= v0 && x < v0 + viewW_) return 1;
    if (x >= v0 + viewW_ + 8 && x < v0 + viewW_ + 8 + stopW_) return 2;
    return 0;
}

void RemoteBanner::draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* b, float w, float h) {
    Factories& f = fx();
    const Theme& t = impl_ ? impl_->th : theme();
    cardBackground(rt, b, t, w, h, false);
    b->SetColor(t.live);  // ●
    rt->FillEllipse(D2D1::Ellipse({16 + 6, h / 2}, 6, 6), b);
    const std::wstring msg = pm::i18n::fmt(viewOnly_ ? S::RemoteBannerView : S::RemoteBanner, {device_});
    b->SetColor(t.fg);
    rt->DrawTextW(msg.c_str(), static_cast<UINT32>(msg.size()), f.banner.Get(), rc(16 + 12 + 10, 0, textW_, h), b);
    float x = 16 + 12 + 10 + textW_ + 14;
    // 僅觀看 toggle: a small switch + label
    {
        const D2D1_RECT_F r = rc(x, 6, viewW_, h - 12);
        b->SetColor(hot_ == 1 ? withA(t.accent, 0.22f) : withA(t.fg, 0.06f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, 8, 8), b);
        const float sx = x + 8, sy = h / 2 - 7;
        b->SetColor(viewOnly_ ? t.accent : withA(t.fg, 0.3f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(rc(sx, sy, 24, 14), 7, 7), b);
        b->SetColor(D2D1_COLOR_F{1, 1, 1, 1});
        rt->FillEllipse(D2D1::Ellipse({viewOnly_ ? sx + 17 : sx + 7, h / 2}, 5, 5), b);
        const std::wstring l = tr(S::RemoteBannerViewOnly);
        b->SetColor(t.fg);
        rt->DrawTextW(l.c_str(), static_cast<UINT32>(l.size()), f.bannerBtn.Get(), rc(x + 34, 0, viewW_ - 36, h), b);
        x += viewW_ + 8;
    }
    // 中斷（Ctrl+D）: the accent button
    {
        const D2D1_RECT_F r = rc(x, 6, stopW_, h - 12);
        b->SetColor(hot_ == 2 ? mix(t.live, D2D1_COLOR_F{1, 1, 1, 1}, 0.15f) : t.live);
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, 8, 8), b);
        const std::wstring l = tr(S::RemoteBannerStop);
        b->SetColor(D2D1_COLOR_F{1, 1, 1, 1});
        rt->DrawTextW(l.c_str(), static_cast<UINT32>(l.size()), f.bannerBtn.Get(), r, b);
    }
}

void RemoteBanner::paint() {
    Impl& im = *impl_;
    Factories& f = fx();
    RECT r{};
    GetClientRect(hwnd_, &r);
    if (!im.rt) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
        props.dpiX = props.dpiY = static_cast<float>(dpi_);
        if (FAILED(f.d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(r.right, r.bottom)), &im.rt)))
            return;
        im.rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &im.brush);
        im.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    const float sc = dpi_ / 96.0f;
    im.rt->BeginDraw();
    draw(im.rt.Get(), im.brush.Get(), r.right / sc, r.bottom / sc);
    if (im.rt->EndDraw() == D2DERR_RECREATE_TARGET) im.brush.Reset(), im.rt.Reset();
}

bool RemoteBanner::renderPng(const std::wstring& path) {
    if (!fx().init()) return false;
    layout();
    const float w = 16 + 12 + 10 + textW_ + 14 + viewW_ + 8 + stopW_ + 8;
    return renderToPng(w, kBannerH, dpi_, path, [this, w](ID2D1RenderTarget* rt) {
        ComPtr<ID2D1SolidColorBrush> b;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &b);
        if (b) draw(rt, b.Get(), w, kBannerH);
    });
}

LRESULT RemoteBanner::handle(UINT msg, WPARAM wp, LPARAM lp) {
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
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && hot_) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        const int nh = hitAt(static_cast<short>(LOWORD(lp)));
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
        return 0;
    case WM_LBUTTONUP: {
        const int hit = hitAt(static_cast<short>(LOWORD(lp)));
        if (hit && fromRemoteInput()) {  // 中斷 / 僅觀看 are for the PC: the phone could lock itself out
            if (cb_.onRemoteRefused) cb_.onRemoteRefused();
            return 0;
        }
        if (hit == 1 && cb_.onViewOnly) {
            auto cb = cb_.onViewOnly;
            cb(!viewOnly_);
        } else if (hit == 2 && cb_.onStop) {
            auto cb = cb_.onStop;
            cb();
        }
        return 0;
    }
    case WM_DPICHANGED:
        if (impl_) impl_->rt.Reset(), impl_->brush.Reset();
        show(device_, viewOnly_, cb_);
        return 0;
    case WM_DESTROY:
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace pm::ui
