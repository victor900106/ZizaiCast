// Themed settings panel (see settings_panel.h). Same construction as
// pair_panel.cpp: owned WS_POPUP, Direct2D HWND render target in the popup
// menus' palette, Windows 11 rounded corners + shadow, layout in DIPs (1 DIP =
// dpi/96 px), real EDIT children (WS_CLIPCHILDREN keeps Direct2D off them)
// coloured through WM_CTLCOLOREDIT.
#include "settings_panel.h"

#include <commctrl.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <map>

#include "pm/i18n.h"
#include "popup_menu.h"
#include "ui_anim.h"

using Microsoft::WRL::ComPtr;

namespace pm::ui {
namespace {

constexpr wchar_t kClass[] = L"PhoneMirrorSettingsPanel";
constexpr float kW = 468, kPad = 24, kHeadH = 72, kDisc = 36;
constexpr float kBtnH = 36, kEditH = 36, kGap = 8, kSwitchW = 40, kSwitchH = 22;
constexpr UINT_PTR kEditSubclass = 1;
constexpr UINT_PTR kToggleTimer = 0x7A45;
constexpr double kToggleMs = 160;
constexpr UINT kMsgEditKey = WM_APP + 40;

D2D1_COLOR_F rgb(uint32_t c, float a = 1) {
    return {((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, a};
}
D2D1_COLOR_F withA(D2D1_COLOR_F c, float a) {
    c.a = a;
    return c;
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
    D2D1_COLOR_F top, bottom, fg, dim, accent, field, onAccent, warn, danger;
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
    t.field.a = 1;
    t.onAccent = lum(t.accent) > 0.55f ? mix(D2D1_COLOR_F{0.13f, 0.09f, 0.10f, 1}, t.accent, 0.10f)
                                       : D2D1_COLOR_F{1, 1, 1, 1};
    t.onAccent.a = 1;
    t.warn = c.light ? D2D1_COLOR_F{0.75f, 0.22f, 0.20f, 1} : D2D1_COLOR_F{1.0f, 0.62f, 0.45f, 1};
    t.danger = t.light ? rgb(0xC0392B) : rgb(0xE5675A);
    return t;
}

struct Factories {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IDWriteTextFormat> title, heading, body, note, status, label, button, buttonWrap, link, code, field, icon;
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
        const bool zh = pm::i18n::zh();
        title = make(ui, 18, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        heading = make(ui, 14.5f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        body = make(ui, zh ? 13.5f : 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        note = make(ui, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        status = make(ui, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        label = make(ui, 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        button = make(ui, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, false);
        if (button) button->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        buttonWrap = make(ui, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, true);  // a too-long label
        if (buttonWrap) buttonWrap->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        link = make(ui, 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        field = make(ui, 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false);  // edit box text (PNGs)
        code = make(L"Consolas", 11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        if (code) code->SetWordWrapping(DWRITE_WORD_WRAPPING_CHARACTER);  // long URLs
        ComPtr<IDWriteFontCollection> fonts;
        if (!icon && SUCCEEDED(dw->GetSystemFontCollection(&fonts)))
            for (const wchar_t* fam : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
                UINT32 idx = 0;
                BOOL exists = FALSE;
                if (SUCCEEDED(fonts->FindFamilyName(fam, &idx, &exists)) && exists) {
                    icon = make(fam, 17, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
                    if (icon) icon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    break;
                }
            }
        return title && heading && body && note && status && label && button && link && code && field;
    }
    ComPtr<IDWriteTextLayout> layout(IDWriteTextFormat* f, const std::wstring& s0, float w) {
        ComPtr<IDWriteTextLayout> l;
        const std::wstring s = pm::i18n::keepWords(s0);  // 한국어: wrap between words
        if (f) dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, (std::max)(1.0f, w), 4096, &l);
        return l;
    }
    static float height(IDWriteTextLayout* l) {
        DWRITE_TEXT_METRICS m{};
        if (l) l->GetMetrics(&m);
        return m.height;
    }
    float width(IDWriteTextFormat* f, const std::wstring& s) {
        ComPtr<IDWriteTextLayout> l = layout(f, s, 4096);
        DWRITE_TEXT_METRICS m{};
        if (l) l->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
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
    auto* p = reinterpret_cast<SettingsPanel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (p && p->hwnd() == h) return p->handle(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

// Enter / Esc / Tab go to the panel (no dialog manager here).
LRESULT CALLBACK editProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* panel = reinterpret_cast<HWND>(ref);
    if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB)) {
        PostMessageW(panel, kMsgEditKey, wp, reinterpret_cast<LPARAM>(h));
        return 0;
    }
    if (msg == WM_CHAR && (wp == L'\r' || wp == 27 || wp == L'\t')) return 0;  // no beep
    if (msg == WM_MOUSEWHEEL) return SendMessageW(panel, msg, wp, lp);
    return DefSubclassProc(h, msg, wp, lp);
}

}  // namespace

struct SettingsPanel::Impl {
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDWriteTextLayout> title;
    struct Row {
        ComPtr<IDWriteTextLayout> a, b;  // main text, second line
        float ah = 0, bh = 0;
        std::vector<float> x, w;          // Buttons / Links: per button (DIPs, relative)
        std::vector<int> line;            // footer: 0 the bottom row, 1 the row above it
    };
    std::vector<Row> rows;
    Theme th = theme();
    HoverAnim anim;
    PanelFade fade;
    std::map<int, std::pair<bool, double>> toggles;  // id -> (on, changed at)
};

float SettingsPanel::viewTop() const { return kHeadH; }
float SettingsPanel::viewBottom() const { return h_ - footerH_; }

bool SettingsPanel::open(HWND owner, const std::wstring& title, wchar_t glyph, std::vector<Item> items, Callbacks cb) {
    if (hwnd_) {
        cb_ = std::move(cb);
        title_ = title;
        glyph_ = glyph;
        setItems(std::move(items));
        if (!testOffscreen) {
            ShowWindow(hwnd_, SW_SHOWNORMAL);
            SetForegroundWindow(hwnd_);
        }
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
    title_ = title;
    glyph_ = glyph;
    items_ = std::move(items);
    hot_ = pressed_ = focus_ = 0;
    focusCues_ = false;
    scroll_ = 0;
    impl_ = new Impl;
    for (const Item& it : items_)
        if (it.kind == Item::Kind::Toggle) impl_->toggles[it.id] = {it.on, -1e9};

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
    dpi_ = testDpi > 0 ? static_cast<UINT>(testDpi) : dx;
    layout();  // h_ (no window yet: sizes only)
    const int w = static_cast<int>(std::lround(kW * s())), hgt = static_cast<int>(std::lround(h_ * s()));
    int x = (orc.left + orc.right) / 2 - w / 2, y = (orc.top + orc.bottom) / 2 - hgt / 2;
    if (!testOffscreen) {
        x = (std::max)(static_cast<int>(mi.rcWork.left), (std::min)(x, static_cast<int>(mi.rcWork.right) - w));
        y = (std::max)(static_cast<int>(mi.rcWork.top), (std::min)(y, static_cast<int>(mi.rcWork.bottom) - hgt));
    } else if (!ownerVisible) {
        x = -12000;
        y = 200;
    }
    hwnd_ = CreateWindowExW(0, kClass, title_.c_str(), WS_POPUP | WS_CLIPCHILDREN | WS_SYSMENU, x, y, w, hgt, owner,
                            nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    dpi_ = testDpi > 0 ? static_cast<UINT>(testDpi) : GetDpiForWindow(hwnd_);
    const DWORD round = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &round, sizeof(round));
    applyEditFonts();
    syncEdits();
    layout();
    impl_->fade.begin(hwnd_);
    ShowWindow(hwnd_, testOffscreen ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
    if (!testOffscreen) SetForegroundWindow(hwnd_);
    return true;
}

void SettingsPanel::close() {
    if (!hwnd_) return;
    prepareCloseFade(hwnd_);
    HWND h = hwnd_;
    for (EditBox& e : edits_) {  // typed keys do not stay in freed memory
        const int n = GetWindowTextLengthW(e.h);
        if (n > 0) SetWindowTextW(e.h, std::wstring(static_cast<size_t>(n), L' ').c_str());
    }
    hwnd_ = nullptr;
    edits_.clear();
    closeWithFade(h);
    destroyTarget();
    delete impl_;
    impl_ = nullptr;
    if (editFont_) DeleteObject(editFont_), editFont_ = nullptr;
    if (editBrush_) DeleteObject(editBrush_), editBrush_ = nullptr;
    if (owner_ && IsWindowVisible(owner_) && !testOffscreen) SetForegroundWindow(owner_);
}

void SettingsPanel::destroyTarget() {
    if (!impl_) return;
    impl_->brush.Reset();
    impl_->rt.Reset();
}

void SettingsPanel::setItems(std::vector<Item> items) {
    if (!impl_) {
        items_ = std::move(items);
        return;
    }
    const double now = animNowMs();
    for (const Item& it : items)
        if (it.kind == Item::Kind::Toggle) {
            auto f = impl_->toggles.find(it.id);
            if (f == impl_->toggles.end()) impl_->toggles[it.id] = {it.on, -1e9};
            else if (f->second.first != it.on) {
                f->second = {it.on, animationsOn() ? now : -1e9};
                if (hwnd_) SetTimer(hwnd_, kToggleTimer, 16, nullptr);
            }
        }
    const size_t oldHits = hits_.size();
    items_ = std::move(items);
    syncEdits();
    layout();
    if (hits_.size() != oldHits) {  // other rows: no stale hover / focus
        hot_ = pressed_ = 0;
        focus_ = 0;
        impl_->anim.reset();
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsPanel::setTitle(const std::wstring& title) {
    title_ = title;
    if (!impl_) return;
    if (hwnd_) SetWindowTextW(hwnd_, title_.c_str());
    layout();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

std::wstring SettingsPanel::editText(int id) const {
    for (const EditBox& e : edits_)
        if (e.id == id) {
            const int n = GetWindowTextLengthW(e.h);
            std::wstring s(static_cast<size_t>(n) + 1, L'\0');
            GetWindowTextW(e.h, s.data(), n + 1);
            s.resize(static_cast<size_t>(n));
            return s;
        }
    return {};
}

void SettingsPanel::setEditText(int id, const std::wstring& text) {
    for (const EditBox& e : edits_)
        if (e.id == id) SetWindowTextW(e.h, text.c_str());
}

void SettingsPanel::wipeEdit(int id) {
    for (const EditBox& e : edits_)
        if (e.id == id) {
            const int n = GetWindowTextLengthW(e.h);
            if (n > 0) SetWindowTextW(e.h, std::wstring(static_cast<size_t>(n), L' ').c_str());
            SetWindowTextW(e.h, L"");
        }
}

void SettingsPanel::retheme() {
    if (!impl_) return;
    impl_->th = theme();
    if (editBrush_) DeleteObject(editBrush_), editBrush_ = nullptr;
    if (!hwnd_) return;
    InvalidateRect(hwnd_, nullptr, FALSE);
    for (const EditBox& e : edits_) InvalidateRect(e.h, nullptr, TRUE);
}

void SettingsPanel::relabel() {
    if (!hwnd_) return;
    fx().init();
    applyEditFonts();
    layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsPanel::applyEditFonts() {
    if (editFont_) DeleteObject(editFont_);
    LOGFONTW lf{};
    lf.lfHeight = -static_cast<LONG>(std::lround(14 * s()));
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, pm::i18n::uiFont());
    editFont_ = CreateFontIndirectW(&lf);
    for (const EditBox& e : edits_) SendMessageW(e.h, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
}

void SettingsPanel::syncEdits() {
    if (!hwnd_) return;
    // Remove boxes that are gone.
    for (size_t i = 0; i < edits_.size();) {
        const bool keep = std::any_of(items_.begin(), items_.end(), [&](const Item& it) {
            return it.kind == Item::Kind::Edit && it.id == edits_[i].id;
        });
        if (keep) {
            ++i;
            continue;
        }
        const int n = GetWindowTextLengthW(edits_[i].h);
        if (n > 0) SetWindowTextW(edits_[i].h, std::wstring(static_cast<size_t>(n), L' ').c_str());
        DestroyWindow(edits_[i].h);
        edits_.erase(edits_.begin() + static_cast<std::ptrdiff_t>(i));
    }
    for (const Item& it : items_) {
        if (it.kind != Item::Kind::Edit) continue;
        auto e = std::find_if(edits_.begin(), edits_.end(), [&](const EditBox& x) { return x.id == it.id; });
        if (e == edits_.end()) {
            EditBox nb;
            nb.id = it.id;
            nb.h = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd_, nullptr, nullptr, nullptr);
            SendMessageW(nb.h, EM_SETLIMITTEXT, 256, 0);
            SendMessageW(nb.h, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), FALSE);
            SetWindowSubclass(nb.h, editProc, kEditSubclass, reinterpret_cast<DWORD_PTR>(hwnd_));
            nb.password = !it.password;  // set below
            edits_.push_back(nb);
            e = edits_.end() - 1;
        }
        if (e->password != it.password) {
            e->password = it.password;
            SendMessageW(e->h, EM_SETPASSWORDCHAR, it.password ? 0x25CF : 0, 0);
            InvalidateRect(e->h, nullptr, TRUE);
        }
        SendMessageW(e->h, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(it.sub.c_str()));
        EnableWindow(e->h, it.enabled);
    }
}

void SettingsPanel::layout() {
    Factories& f = fx();
    f.init();
    Impl& im = *impl_;
    im.title = f.layout(f.title.Get(), title_, kW - kPad - kDisc - 12 - 56);
    im.rows.assign(items_.size(), {});
    itemY_.assign(items_.size(), 0);
    itemH_.assign(items_.size(), 0);
    hits_.clear();
    float y = 0;
    footerH_ = 20;
    using K = Item::Kind;
    for (size_t i = 0; i < items_.size(); ++i) {
        const Item& it = items_[i];
        Impl::Row& r = im.rows[i];
        const float x0 = kPad + it.indent, w = kW - kPad - x0;
        float h = 0;
        switch (it.kind) {
        case K::Heading:
        case K::Text:
        case K::Note:
        case K::Status: {
            IDWriteTextFormat* fmt = it.kind == K::Heading ? f.heading.Get()
                                     : it.kind == K::Note  ? f.note.Get()
                                     : it.kind == K::Status ? f.status.Get()
                                                           : f.body.Get();
            r.a = f.layout(fmt, it.text, w);
            if (r.a && it.kind != K::Heading)
                r.a->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, (it.kind == K::Note ? 12.5f : 13.5f) * 1.42f,
                                    (it.kind == K::Note ? 12.5f : 13.5f) * 1.12f);
            r.ah = Factories::height(r.a.Get());
            h = r.ah;
            if (it.kind == K::Heading && i > 0) y += 6;
            break;
        }
        case K::KeyRow: {  // label | key cap (right-aligned)
            const float kw = std::ceil(f.width(f.label.Get(), it.sub)) + 16;
            r.b = f.layout(f.label.Get(), it.sub, kw);
            r.bh = Factories::height(r.b.Get());
            r.x.assign(1, kw);
            r.a = f.layout(f.body.Get(), it.text, (std::max)(80.0f, w - kw - 12));
            r.ah = Factories::height(r.a.Get());
            h = (std::max)(r.ah, r.bh + 6) + 2;
            break;
        }
        case K::Code:
            r.a = f.layout(f.code.Get(), it.text, w - 20);
            r.ah = Factories::height(r.a.Get());
            h = r.ah + 16;
            break;
        case K::Toggle:
        case K::Radio: {
            const float tx = it.kind == K::Radio ? x0 + 28 : x0;
            const float tw = it.kind == K::Radio ? kW - kPad - tx : kW - kPad - tx - kSwitchW - 14;
            r.a = f.layout(f.label.Get(), it.text, tw);
            r.ah = Factories::height(r.a.Get());
            if (!it.sub.empty()) {
                r.b = f.layout(f.note.Get(), it.sub, tw);
                if (r.b) r.b->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 12.5f * 1.38f, 12.5f * 1.1f);
                r.bh = Factories::height(r.b.Get());
            }
            h = (std::max)(it.kind == K::Radio ? 24.0f : 28.0f, r.ah + (r.b ? r.bh + 2 : 0)) + 8;
            hits_.push_back({x0 - 8, y, kW - kPad + 8, y + h, it.id, it.enabled, false});
            break;
        }
        case K::Buttons: {
            float x = 0;
            for (const Button& bt : it.buttons) {
                const float bw = (std::max)(92.0f, std::ceil(f.width(f.button.Get(), bt.text)) + 36);
                r.x.push_back(x);
                r.w.push_back(bw);
                x += bw + 10;
            }
            if (it.footer) {
                // Right-aligned (a non-primary danger button on the left).
                float right = kW - kPad;
                for (size_t k = it.buttons.size(); k-- > 0;) {
                    if (it.buttons[k].danger && !it.buttons[k].primary) continue;
                    right -= r.w[k];
                    r.x[k] = right;
                    right -= 10;
                }
                float left = kPad;
                for (size_t k = 0; k < it.buttons.size(); ++k)
                    if (it.buttons[k].danger && !it.buttons[k].primary) {
                        r.x[k] = left;
                        left += r.w[k] + 10;
                    }
                // Too wide for one row (long labels): the left ones go on a row above.
                const bool two = left - 10 > right;
                r.line.assign(it.buttons.size(), 0);
                for (size_t k = 0; k < it.buttons.size(); ++k)
                    if (two && it.buttons[k].danger && !it.buttons[k].primary) r.line[k] = 1;
                footerH_ = kBtnH + 18 + 20 + (two ? kBtnH + 10 : 0);
                h = 0;  // not in the scrolled content
            } else {
                for (float& bx : r.x) bx += x0;
                // A label wider than the panel: the button takes the width and its text wraps.
                h = kBtnH;
                const float avail = kW - kPad - x0;
                for (size_t k = 0; k < it.buttons.size(); ++k)
                    if (r.x[k] + r.w[k] > kW - kPad) {
                        r.w[k] = (std::max)(92.0f, kW - kPad - r.x[k]);
                        ComPtr<IDWriteTextLayout> l = f.layout(f.buttonWrap.Get(), it.buttons[k].text, (std::min)(avail, r.w[k]) - 28);
                        h = (std::max)(h, std::ceil(Factories::height(l.Get())) + 16);
                    }
                for (size_t k = 0; k < it.buttons.size(); ++k)
                    hits_.push_back({r.x[k], y, r.x[k] + r.w[k], y + h, it.buttons[k].id, it.enabled && it.buttons[k].enabled, false});
            }
            break;
        }
        case K::Links: {
            float x = x0;
            for (const Button& bt : it.buttons) {
                const float lw = std::ceil(f.width(f.link.Get(), bt.text));
                r.x.push_back(x);
                r.w.push_back(lw);
                hits_.push_back({x - 4, y - 2, x + lw + 4, y + 22, bt.id, it.enabled && bt.enabled, false});
                x += lw + 20;
            }
            h = 20;
            break;
        }
        case K::Progress: h = 8; break;
        case K::Edit: h = kEditH; break;
        case K::Rule: h = 1; break;
        case K::Space: h = 2; break;
        }
        itemY_[i] = y;
        itemH_[i] = h;
        if (h > 0) y += h + kGap;
    }
    contentH_ = (std::max)(0.0f, y - kGap) + 4;
    // Footer buttons: fixed hits (window DIPs).
    float maxH = 900;
    if (hwnd_ || owner_) {
        HMONITOR mon = MonitorFromWindow(hwnd_ ? hwnd_ : owner_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(mon, &mi)) maxH = (mi.rcWork.bottom - mi.rcWork.top) / s() - 24;
        if (testWorkAreaPx > 0) maxH = testWorkAreaPx / s() - 24;  // --dev: a smaller screen
    }
    h_ = std::ceil((std::min)(kHeadH + contentH_ + footerH_, (std::max)(240.0f, maxH)));
    layoutFixedHits();
    scroll_ = std::clamp(scroll_, 0.0f, (std::max)(0.0f, contentH_ - (viewBottom() - viewTop())));
    if (hwnd_) {
        inLayout_ = true;
        RECT wr{};
        GetWindowRect(hwnd_, &wr);
        const int w = static_cast<int>(std::lround(kW * s())), hgt = static_cast<int>(std::lround(h_ * s()));
        if (wr.right - wr.left != w || wr.bottom - wr.top != hgt) {
            int top = wr.top;
            MONITORINFO mi{sizeof(mi)};
            if (!testOffscreen && GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi) &&
                top + hgt > mi.rcWork.bottom)
                top = (std::max)(static_cast<int>(mi.rcWork.top), static_cast<int>(mi.rcWork.bottom) - hgt);
            SetWindowPos(hwnd_, nullptr, wr.left, top, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        inLayout_ = false;
        if (im.rt) {
            RECT cr{};
            GetClientRect(hwnd_, &cr);
            im.rt->Resize(D2D1::SizeU(cr.right, cr.bottom));
            im.rt->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
        }
        placeEdits();
    }
}

// Footer buttons and × : window DIPs, from the bottom (h_).
void SettingsPanel::layoutFixedHits() {
    hits_.erase(std::remove_if(hits_.begin(), hits_.end(), [](const Hit& x) { return x.fixed; }), hits_.end());
    for (size_t i = 0; i < items_.size() && i < impl_->rows.size(); ++i) {
        const Item& it = items_[i];
        if (it.kind != Item::Kind::Buttons || !it.footer) continue;
        for (size_t k = 0; k < it.buttons.size(); ++k) {
            const float by = h_ - 20 - kBtnH - impl_->rows[i].line[k] * (kBtnH + 10);
            hits_.push_back({impl_->rows[i].x[k], by, impl_->rows[i].x[k] + impl_->rows[i].w[k], by + kBtnH, it.buttons[k].id,
                             it.enabled && it.buttons[k].enabled, true});
        }
    }
    hits_.push_back({kW - 52, 12, kW - 16, 48, -1, true, true});  // × (id -1), always the last hit
}

void SettingsPanel::placeEdits() {
    if (!hwnd_) return;
    const int lineH = static_cast<int>(std::lround(20 * s()));
    for (const EditBox& e : edits_) {
        size_t i = 0;
        while (i < items_.size() && !(items_[i].kind == Item::Kind::Edit && items_[i].id == e.id)) ++i;
        if (i >= items_.size()) continue;
        const float top = viewTop() + itemY_[i] - scroll_;
        const bool visible = top >= viewTop() - 0.5f && top + kEditH <= viewBottom() + 0.5f;
        const float x0 = kPad + items_[i].indent;
        const int x = static_cast<int>(std::lround((x0 + 12) * s()));
        const int y = static_cast<int>(std::lround((top + kEditH / 2) * s())) - lineH / 2;
        const int w = static_cast<int>(std::lround((kW - kPad - x0 - 24) * s()));
        SetWindowPos(e.h, nullptr, x, y, w, lineH, SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(e.h, visible ? SW_SHOWNA : SW_HIDE);
    }
}

void SettingsPanel::scrollBy(float dips) {
    const float maxS = (std::max)(0.0f, contentH_ - (viewBottom() - viewTop()));
    const float ns = std::clamp(scroll_ + dips, 0.0f, maxS);
    if (ns == scroll_) return;
    scroll_ = ns;
    placeEdits();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

int SettingsPanel::hitAt(POINT pt) const {
    const float x = pt.x / s(), y = pt.y / s();
    for (size_t i = 0; i < hits_.size(); ++i) {
        const Hit& h = hits_[i];
        float yy = y;
        if (!h.fixed) {
            if (y < viewTop() || y >= viewBottom()) continue;
            yy = y - viewTop() + scroll_;
        }
        if (x >= h.l && x < h.r && yy >= h.t && yy < h.b) return static_cast<int>(i) + 1;
    }
    return 0;
}

void SettingsPanel::activate(int hit) {
    if (hit <= 0 || hit > static_cast<int>(hits_.size())) return;
    const Hit h = hits_[static_cast<size_t>(hit) - 1];
    if (!h.enabled) return;
    if (h.id == -1) {
        auto cb = cb_.onClose;
        close();
        if (cb) cb();
        return;
    }
    if (cb_.onAction) cb_.onAction(h.id);
}

// One DPI per window: the window's own (GetDpiForWindow). A change that did
// not come with WM_DPICHANGED (or came before the target existed) is caught
// here; the render target always has the window's DPI and client size, and
// the drawn height follows the client (no unpainted band if Windows sized
// the window differently).
void SettingsPanel::fitClient() {
    if (!hwnd_ || !impl_) return;
    const UINT wd = windowDpi();
    if (wd != dpi_) {
        dpi_ = wd;
        applyEditFonts();
        layout();
    }
    RECT cr{};
    GetClientRect(hwnd_, &cr);
    if (cr.bottom > 0 && std::fabs(cr.bottom - h_ * s()) > 1.0f) {
        h_ = cr.bottom / s();
        scroll_ = std::clamp(scroll_, 0.0f, (std::max)(0.0f, contentH_ - (viewBottom() - viewTop())));
        layoutFixedHits();
        placeEdits();
    }
    if (Impl& im = *impl_; im.rt) {
        const D2D1_SIZE_U ps = im.rt->GetPixelSize();
        if (ps.width != static_cast<UINT32>(cr.right) || ps.height != static_cast<UINT32>(cr.bottom))
            im.rt->Resize(D2D1::SizeU(cr.right, cr.bottom));
        float dx = 0, dy = 0;
        im.rt->GetDpi(&dx, &dy);
        if (dx != static_cast<float>(dpi_)) im.rt->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
    }
}

void SettingsPanel::paint() {
    Impl& im = *impl_;
    fitClient();
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
    // draw() starts from the target's transform: always identity here. (It was
    // left at the last frame's open-fade scale, which then compounded frame by
    // frame - 0.97 x 0.97 x ... - so the panel was drawn shrunk towards its
    // centre inside an unpainted frame, with the hit rectangles where the
    // unscaled controls would be: clicks landed below / beside the buttons.)
    im.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    draw(im.rt.Get(), im.brush.Get(), false);
    im.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    if (im.rt->EndDraw() == D2DERR_RECREATE_TARGET) destroyTarget();
    im.fade.painted();
}

bool SettingsPanel::renderPng(const std::wstring& path, bool view) {
    if (!hwnd_ || !impl_) return false;
    if (view)  // what the window shows now (its height, scrolled)
        return renderToPng(kW, h_, dpi_, path, [this](ID2D1RenderTarget* rt) {
            ComPtr<ID2D1SolidColorBrush> b;
            rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &b);
            if (b) draw(rt, b.Get(), true);
        });
    const float fullH = std::ceil(kHeadH + contentH_ + footerH_);
    const float saveH = h_, saveScroll = scroll_;
    // The whole content, unscrolled (the footer at the full height).
    h_ = fullH;
    scroll_ = 0;
    const bool ok = renderToPng(kW, fullH, dpi_, path, [this](ID2D1RenderTarget* rt) {
        ComPtr<ID2D1SolidColorBrush> b;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &b);
        if (b) draw(rt, b.Get(), true);
    });
    h_ = saveH;
    scroll_ = saveScroll;
    return ok;
}

void SettingsPanel::devDpiChanged(int dpi, bool stale) {
    if (!hwnd_) return;
    const UINT old = dpi_;
    testDpi = dpi;
    if (stale) {  // no message: the next paint must notice
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    RECT wr{};
    GetWindowRect(hwnd_, &wr);
    const double k = static_cast<double>(dpi) / old;
    RECT r{wr.left, wr.top, wr.left + static_cast<LONG>(std::lround((wr.right - wr.left) * k)),
           wr.top + static_cast<LONG>(std::lround((wr.bottom - wr.top) * k))};
    SendMessageW(hwnd_, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&r));
}

std::string SettingsPanel::selfCheck() {
    if (!hwnd_ || !impl_) return "FAIL not open";
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);  // paint now: fitClient + drawn_
    RECT wr{}, cr{};
    GetWindowRect(hwnd_, &wr);
    GetClientRect(hwnd_, &cr);
    std::string bad;
    auto fail = [&](const std::string& s) { bad += (bad.empty() ? "" : "; ") + s; };
    const int wantW = static_cast<int>(std::lround(kW * s())), wantH = static_cast<int>(std::lround(h_ * s()));
    if (windowDpi() != dpi_) fail("window dpi " + std::to_string(windowDpi()) + " != layout dpi " + std::to_string(dpi_));
    if (cr.right != wantW || std::abs(cr.bottom - wantH) > 1)
        fail("client " + std::to_string(cr.right) + "x" + std::to_string(cr.bottom) + " != layout " + std::to_string(wantW) + "x" +
             std::to_string(wantH));
    if (impl_->rt) {
        float dx = 0, dy = 0;
        impl_->rt->GetDpi(&dx, &dy);
        const D2D1_SIZE_U ps = impl_->rt->GetPixelSize();
        if (dx != static_cast<float>(dpi_)) fail("target dpi " + std::to_string(static_cast<int>(dx)));
        if (ps.width != static_cast<UINT32>(cr.right) || ps.height != static_cast<UINT32>(cr.bottom))
            fail("target " + std::to_string(ps.width) + "x" + std::to_string(ps.height));
    } else {
        fail("no render target");
    }
    int ok = 0, wrong = 0;
    for (const Drawn& d : drawn_) {
        const int hit = hitAt(d.px);
        if (hit > 0 && hits_[static_cast<size_t>(hit) - 1].id == d.id) {
            ++ok;
        } else {
            ++wrong;
            if (wrong <= 3)
                fail("id " + std::to_string(d.id) + " drawn at " + std::to_string(d.px.x) + "," + std::to_string(d.px.y) + " hits " +
                     (hit > 0 ? "id " + std::to_string(hits_[static_cast<size_t>(hit) - 1].id) : std::string("nothing")));
        }
    }
    if (drawn_.empty()) fail("nothing drawn");
    return (bad.empty() ? "ok" : "FAIL") + std::string(" dpi ") + std::to_string(dpi_) + ", window " +
           std::to_string(wr.right - wr.left) + "x" + std::to_string(wr.bottom - wr.top) + ", client " + std::to_string(cr.right) +
           "x" + std::to_string(cr.bottom) + ", scroll " + std::to_string(static_cast<int>(scroll_)) + ", controls " +
           std::to_string(ok) + "/" + std::to_string(ok + wrong) + " hit where drawn" + (bad.empty() ? "" : ": " + bad);
}

void SettingsPanel::draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* b, bool png) {
    Impl& im = *impl_;
    Factories& f = fx();
    const Theme& t = im.th;
    using K = Item::Kind;
    D2D1_MATRIX_3X2_F base;
    rt->GetTransform(&base);
    auto fill = [&](D2D1_RECT_F r, D2D1_COLOR_F c, float radius = 0) {
        b->SetColor(c);
        if (radius > 0) rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b);
        else rt->FillRectangle(r, b);
    };
    auto stroke = [&](D2D1_RECT_F r, D2D1_COLOR_F c, float radius, float w = 1) {
        b->SetColor(c);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b, w);
    };
    auto text = [&](const std::wstring& s, IDWriteTextFormat* fmt, D2D1_RECT_F r, D2D1_COLOR_F c) {
        b->SetColor(c);
        rt->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, r, b);
    };
    auto layoutAt = [&](IDWriteTextLayout* l, float x, float y, D2D1_COLOR_F c) {
        if (!l) return;
        b->SetColor(c);
        rt->DrawTextLayout({x, y}, l, b);
    };
    // Hover / press level and focus ring of the hit with this id (index + 1).
    auto hitIndex = [&](int id, bool fixed) {
        for (size_t i = 0; i < hits_.size(); ++i)
            if (hits_[i].id == id && hits_[i].fixed == fixed) return static_cast<int>(i) + 1;
        return 0;
    };
    auto hl = [&](int key) { return key > 0 && key < HoverAnim::kMax ? im.anim.hot(key) : 0.f; };
    auto pressAt = [&](int key, D2D1_RECT_F r, const D2D1_MATRIX_3X2_F& m) {
        const float k = key > 0 && key < HoverAnim::kMax ? im.anim.pressScale(key) : 1.f;
        rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k, {(r.left + r.right) / 2, (r.top + r.bottom) / 2}) *
                         *D2D1::Matrix3x2F::ReinterpretBaseType(&m));
    };
    auto focusRing = [&](int key, D2D1_RECT_F r, float radius) {
        if (focusCues_ && key == focus_ && key > 0)
            stroke({r.left - 3, r.top - 3, r.right + 3, r.bottom + 3}, withA(t.fg, 0.85f), radius + 3, 1.5f);
    };
    // selfCheck: where each control is drawn (client px, through the transform and the target's DPI).
    if (!png) drawn_.clear();
    float rtDpiX = 96, rtDpiY = 96;
    rt->GetDpi(&rtDpiX, &rtDpiY);
    auto note = [&](int id, D2D1_RECT_F r, const D2D1_MATRIX_3X2_F& m, bool fixed) {
        if (png) return;
        const D2D1_POINT_2F c = D2D1::Matrix3x2F::ReinterpretBaseType(&m)->TransformPoint({(r.left + r.right) / 2, (r.top + r.bottom) / 2});
        if (!fixed && (c.y < viewTop() + 1 || c.y > viewBottom() - 1)) return;  // scrolled out (clipped)
        drawn_.push_back({id, POINT{static_cast<LONG>(std::floor(c.x * rtDpiX / 96)), static_cast<LONG>(std::floor(c.y * rtDpiY / 96))}});
    };
    auto button = [&](const Button& bt, D2D1_RECT_F r, bool enabled, int key, const D2D1_MATRIX_3X2_F& m) {
        note(bt.id, r, m, &m == &base);
        const bool tall = r.bottom - r.top > kBtnH + 0.5f;
        IDWriteTextFormat* bf = tall ? f.buttonWrap.Get() : f.button.Get();
        const D2D1_RECT_F tr14 = tall ? D2D1_RECT_F{r.left + 14, r.top, r.right - 14, r.bottom} : r;
        const float hot = enabled ? hl(key) : 0;
        const float a = enabled ? 1.f : 0.42f;
        pressAt(key, r, m);
        if (bt.primary) {
            D2D1_COLOR_F c = bt.danger ? t.danger : t.accent;
            c = mix(c, t.light ? D2D1_COLOR_F{0, 0, 0, 1} : D2D1_COLOR_F{1, 1, 1, 1}, 0.15f * hot);
            fill(r, withA(c, a), 9);
            text(bt.text, bf, tr14, withA(bt.danger ? D2D1_COLOR_F{1, 1, 1, 1} : t.onAccent, a));
        } else {
            fill(r, withA(t.fg, (0.07f + 0.07f * hot) * a), 9);
            stroke({r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f},
                   withA(bt.danger ? t.danger : t.fg, (bt.danger ? 0.55f : 0.2f) * a), 9);
            text(bt.text, bf, tr14, withA(bt.danger ? t.danger : t.fg, a));
        }
        rt->SetTransform(m);
        focusRing(key, r, 9);
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
    }
    if (const float k = png ? 1.f : im.fade.scale(); k < 1)  // opening: grows from 97 %
        base = D2D1::Matrix3x2F::Scale(k, k, {kW / 2, h_ / 2}) * *D2D1::Matrix3x2F::ReinterpretBaseType(&base);
    rt->SetTransform(base);

    // Content (scrolled, clipped to the view).
    const float vt = viewTop(), vb = viewBottom();
    rt->PushAxisAlignedClip(rc(0, vt, kW, vb - vt), D2D1_ANTIALIAS_MODE_ALIASED);
    D2D1_MATRIX_3X2_F cm = D2D1::Matrix3x2F::Translation(0, vt - scroll_) * *D2D1::Matrix3x2F::ReinterpretBaseType(&base);
    rt->SetTransform(cm);
    for (size_t i = 0; i < items_.size(); ++i) {
        const Item& it = items_[i];
        const Impl::Row& r = im.rows[i];
        const float y = itemY_[i], h = itemH_[i];
        if (h <= 0 || y - scroll_ > vb - vt + 4 || y + h - scroll_ < -40) continue;
        const float x0 = kPad + it.indent;
        const float a = it.enabled ? 1.f : 0.42f;
        switch (it.kind) {
        case K::Heading: layoutAt(r.a.Get(), x0, y, withA(t.fg, a)); break;
        case K::Text: layoutAt(r.a.Get(), x0, y, withA(it.tone == 2 ? t.warn : it.tone == 1 ? t.accent : t.fg, a)); break;
        case K::Note: layoutAt(r.a.Get(), x0, y, withA(t.dim, a)); break;
        case K::Status:
            layoutAt(r.a.Get(), x0, y, withA(it.tone == 2 ? t.warn : it.tone == 1 ? t.accent : t.fg, a));
            break;
        case K::KeyRow: {
            layoutAt(r.a.Get(), x0, y + (h - r.ah) / 2, withA(t.fg, a));
            const float kw = r.x.empty() ? 0 : r.x[0], kx = kW - kPad - kw, kh = r.bh + 6, ky = y + (h - kh) / 2;
            fill(rc(kx, ky, kw, kh), withA(t.fg, t.light ? 0.06f : 0.08f), 6);
            stroke({kx + 0.5f, ky + 0.5f, kx + kw - 0.5f, ky + kh - 0.5f}, withA(t.fg, 0.18f), 6);
            layoutAt(r.b.Get(), kx + 8, ky + 3, withA(t.accent, a));
            break;
        }
        case K::Code:
            fill(rc(x0, y, kW - kPad - x0, h), withA(t.fg, t.light ? 0.05f : 0.07f), 8);
            layoutAt(r.a.Get(), x0 + 10, y + 8, withA(t.dim, a));
            break;
        case K::Toggle: {
            const int key = hitIndex(it.id, false);
            const float hot = it.enabled ? hl(key) : 0;
            if (hot > 0.003f) fill(rc(x0 - 8, y, kW - kPad - x0 + 16, h), withA(t.accent, 0.10f * hot), 8);
            layoutAt(r.a.Get(), x0, y + 4, withA(t.fg, a));
            if (r.b) layoutAt(r.b.Get(), x0, y + 4 + r.ah + 2, withA(t.dim, a));
            // The switch: knob slides (160 ms) when the state changes.
            const auto tg = im.toggles.count(it.id) ? im.toggles.at(it.id) : std::pair<bool, double>{it.on, -1e9};
            const float p = png ? 1.f : animOutCubic((animNowMs() - tg.second) / kToggleMs);
            const float pos = it.on ? p : 1 - p;
            const float sx = kW - kPad - kSwitchW, sy = y + 4 + (std::max)(0.0f, (r.ah - kSwitchH) / 2);
            const D2D1_RECT_F sw = rc(sx, sy, kSwitchW, kSwitchH);
            pressAt(key, sw, cm);
            const D2D1_COLOR_F onC = mix(t.accent, t.light ? D2D1_COLOR_F{0, 0, 0, 1} : D2D1_COLOR_F{1, 1, 1, 1}, 0.12f * hot);
            if (pos > 0.01f) fill(sw, withA(onC, a * pos), kSwitchH / 2);
            if (pos < 0.99f) {
                fill(sw, withA(t.fg, 0.06f * (1 - pos) * a), kSwitchH / 2);
                stroke(rc(sx + 0.75f, sy + 0.75f, kSwitchW - 1.5f, kSwitchH - 1.5f), withA(t.fg, 0.55f * (1 - pos) * a),
                       kSwitchH / 2 - 0.75f, 1.5f);
            }
            const float kr = 6.5f + 1.0f * pos;
            const float kx = sx + 11 + (kSwitchW - 22) * pos;
            b->SetColor(withA(mix(t.fg, t.onAccent, pos), a));
            rt->FillEllipse(D2D1::Ellipse({kx, sy + kSwitchH / 2}, kr, kr), b);
            rt->SetTransform(cm);
            focusRing(key, rc(x0 - 8, y, kW - kPad - x0 + 16, h), 8);
            note(it.id, rc(x0 - 8, y, kW - kPad - x0 + 16, h), cm, false);
            break;
        }
        case K::Radio: {
            const int key = hitIndex(it.id, false);
            const float hot = it.enabled ? hl(key) : 0;
            if (hot > 0.003f) fill(rc(x0 - 8, y, kW - kPad - x0 + 16, h), withA(t.accent, 0.10f * hot), 8);
            const D2D1_POINT_2F c{x0 + 9, y + 4 + (std::max)(9.0f, r.ah / 2)};
            b->SetColor(withA(it.on ? t.accent : t.fg, (it.on ? 1.f : 0.55f) * a));
            rt->DrawEllipse(D2D1::Ellipse(c, 8.25f, 8.25f), b, 1.5f);
            if (it.on) rt->FillEllipse(D2D1::Ellipse(c, 4.5f, 4.5f), b);
            layoutAt(r.a.Get(), x0 + 28, y + 4, withA(t.fg, a));
            if (r.b) layoutAt(r.b.Get(), x0 + 28, y + 4 + r.ah + 2, withA(t.dim, a));
            focusRing(key, rc(x0 - 8, y, kW - kPad - x0 + 16, h), 8);
            note(it.id, rc(x0 - 8, y, kW - kPad - x0 + 16, h), cm, false);
            break;
        }
        case K::Buttons:
            if (it.footer) break;
            for (size_t k = 0; k < it.buttons.size(); ++k)
                button(it.buttons[k], rc(r.x[k], y, r.w[k], h), it.enabled && it.buttons[k].enabled,
                       hitIndex(it.buttons[k].id, false), cm);
            break;
        case K::Links:
            for (size_t k = 0; k < it.buttons.size(); ++k) {
                const bool en = it.enabled && it.buttons[k].enabled;
                const int key = hitIndex(it.buttons[k].id, false);
                const float lh = en ? hl(key) : 0;
                const D2D1_COLOR_F c = withA(mix(t.accent, t.fg, lh), en ? 1.f : 0.42f);
                text(it.buttons[k].text, f.link.Get(), rc(r.x[k], y, r.w[k] + 4, 20), c);
                b->SetColor(withA(t.accent, (0.45f + 0.45f * lh) * (en ? 1.f : 0.42f)));
                rt->DrawLine({r.x[k], y + 18.5f}, {r.x[k] + r.w[k], y + 18.5f}, b, 1);
                focusRing(key, rc(r.x[k] - 2, y - 1, r.w[k] + 4, 21), 4);
                note(it.buttons[k].id, rc(r.x[k], y, r.w[k], 20), cm, false);
            }
            break;
        case K::Progress: {
            const float w = kW - kPad - x0;
            fill(rc(x0, y, w, 8), withA(t.fg, 0.12f), 4);
            const float v = std::clamp(it.value, 0.0f, 1.0f);
            if (v > 0) fill(rc(x0, y, (std::max)(8.0f, w * v), 8), t.accent, 4);
            break;
        }
        case K::Edit: {
            const D2D1_RECT_F fr = rc(x0, y, kW - kPad - x0, kEditH);
            fill(fr, withA(t.field, it.enabled ? 1.f : 0.5f), 8);
            HWND eh = nullptr;
            for (const EditBox& e : edits_)
                if (e.id == it.id) eh = e.h;
            const bool focus = eh && GetFocus() == eh;
            stroke(rc(x0 + 0.5f, y + 0.5f, kW - kPad - x0 - 1, kEditH - 1), focus ? t.accent : withA(t.fg, 0.16f), 8,
                   focus ? 1.5f : 1);
            if (png && eh) {  // the EDIT control's content (or its cue banner)
                const int n = GetWindowTextLengthW(eh);
                std::wstring s;
                if (n > 0) {
                    s.assign(static_cast<size_t>(n) + 1, L'\0');
                    GetWindowTextW(eh, s.data(), n + 1);
                    s.resize(static_cast<size_t>(n));
                    if (it.password) s.assign(s.size(), 0x25CF);
                }
                const D2D1_RECT_F tr = rc(x0 + 12, y + 7, kW - kPad - x0 - 24, 22);
                rt->PushAxisAlignedClip(tr, D2D1_ANTIALIAS_MODE_ALIASED);
                text(s.empty() ? it.sub : s, f.field.Get(), tr, s.empty() ? withA(t.fg, 0.45f) : t.fg);
                rt->PopAxisAlignedClip();
            }
            break;
        }
        case K::Rule: fill(rc(x0, y, kW - kPad - x0, 1), withA(t.fg, 0.12f)); break;
        case K::Space: break;
        }
    }
    rt->SetTransform(base);
    rt->PopAxisAlignedClip();
    // Scroll bar; a hairline where content runs under the footer / header.
    const float viewH = vb - vt;
    if (contentH_ > viewH + 0.5f) {
        const float bh = (std::max)(24.0f, viewH * viewH / contentH_);
        const float by = vt + (viewH - bh) * (scroll_ / (contentH_ - viewH));
        fill(rc(kW - 9, by, 4, bh), withA(t.fg, 0.35f), 2);
        if (scroll_ > 0.5f) fill(rc(0, vt - 1, kW, 1), withA(t.fg, 0.12f));
        if (scroll_ < contentH_ - viewH - 0.5f) fill(rc(0, vb, kW, 1), withA(t.fg, 0.12f));
    }

    // Header: accent bar, glyph disc, title, ×.
    fill(rc(0, 0, kW, 3), withA(t.accent, 0.85f));
    b->SetColor(withA(t.accent, 0.18f));
    rt->FillEllipse(D2D1::Ellipse({kPad + kDisc / 2, 18 + kDisc / 2}, kDisc / 2, kDisc / 2), b);
    if (f.icon && glyph_) {
        const wchar_t g[2] = {glyph_, 0};
        text(g, f.icon.Get(), rc(kPad, 18, kDisc, kDisc), t.accent);
    }
    if (im.title) {
        const float th = Factories::height(im.title.Get());
        layoutAt(im.title.Get(), kPad + kDisc + 12, 18 + (std::max)(0.0f, (kDisc - th) / 2), t.fg);
    }
    {
        const int key = hitIndex(-1, true);
        const D2D1_RECT_F cr = rc(kW - 52, 12, 36, 36);
        const float h = hl(key);
        if (h > 0.003f) fill(cr, withA(t.accent, 0.22f * h), 8);
        b->SetColor(mix(t.dim, t.fg, h));
        const float cx = kW - 34, cy = 30, d = 5.5f;
        rt->DrawLine({cx - d, cy - d}, {cx + d, cy + d}, b, 1.6f);
        rt->DrawLine({cx - d, cy + d}, {cx + d, cy - d}, b, 1.6f);
        focusRing(key, cr, 8);
        note(-1, cr, base, true);
    }
    // Footer buttons.
    for (size_t i = 0; i < items_.size(); ++i) {
        const Item& it = items_[i];
        if (it.kind != K::Buttons || !it.footer) continue;
        for (size_t k = 0; k < it.buttons.size(); ++k) {
            const float by = h_ - 20 - kBtnH - im.rows[i].line[k] * (kBtnH + 10);
            button(it.buttons[k], rc(im.rows[i].x[k], by, im.rows[i].w[k], kBtnH), it.enabled && it.buttons[k].enabled,
                   hitIndex(it.buttons[k].id, true), base);
        }
    }
    // Border last (over everything).
    rt->SetTransform(base);
    b->SetColor(withA(t.accent, 0.35f));
    rt->DrawRectangle(rc(0.5f, 0.5f, kW - 1, h_ - 1), b, 1);
}

LRESULT SettingsPanel::handle(UINT msg, WPARAM wp, LPARAM lp) {
    HWND h = hwnd_;
    // Keyboard order: the hits (content in order, then footer, then ×) and the edit boxes at their rows.
    auto order = [&] {
        std::vector<int> o;  // > 0 hit index + 1, < 0 -(edit index + 1)
        std::vector<std::pair<float, int>> c;
        for (size_t i = 0; i < hits_.size(); ++i)
            if (hits_[i].enabled) c.push_back({hits_[i].fixed ? 1e6f + hits_[i].l + (hits_[i].id == -1 ? 1e5f : 0) : hits_[i].t * 1000 + hits_[i].l, static_cast<int>(i) + 1});
        for (size_t e = 0; e < edits_.size(); ++e)
            for (size_t i = 0; i < items_.size(); ++i)
                if (items_[i].kind == Item::Kind::Edit && items_[i].id == edits_[e].id && items_[i].enabled)
                    c.push_back({itemY_[i] * 1000, -static_cast<int>(e) - 1});
        std::stable_sort(c.begin(), c.end(), [](auto& a, auto& b) { return a.first < b.first; });
        for (auto& p : c) o.push_back(p.second);
        return o;
    };
    auto ensureVisible = [&](int hit) {
        if (hit <= 0 || hit > static_cast<int>(hits_.size()) || hits_[hit - 1].fixed) return;
        const Hit& x = hits_[hit - 1];
        const float viewH = viewBottom() - viewTop();
        if (x.t < scroll_) scrollBy(x.t - scroll_ - 4);
        else if (x.b > scroll_ + viewH) scrollBy(x.b - scroll_ - viewH + 4);
    };
    auto moveFocus = [&](bool back, HWND fromEdit) {
        const std::vector<int> o = order();
        if (o.empty()) return;
        int cur = -1;
        for (size_t i = 0; i < o.size(); ++i) {
            if (fromEdit && o[i] < 0 && edits_[static_cast<size_t>(-o[i] - 1)].h == fromEdit) cur = static_cast<int>(i);
            if (!fromEdit && o[i] == focus_) cur = static_cast<int>(i);
        }
        const int n = static_cast<int>(o.size());
        const int next = cur < 0 ? (back ? n - 1 : 0) : (cur + (back ? n - 1 : 1)) % n;
        const int v = o[static_cast<size_t>(next)];
        focusCues_ = true;
        if (v < 0) {
            focus_ = 0;
            const size_t e = static_cast<size_t>(-v - 1);
            for (size_t i = 0; i < items_.size(); ++i)
                if (items_[i].kind == Item::Kind::Edit && items_[i].id == edits_[e].id) {
                    const float viewH = viewBottom() - viewTop();
                    if (itemY_[i] < scroll_) scrollBy(itemY_[i] - scroll_ - 4);
                    else if (itemY_[i] + kEditH > scroll_ + viewH) scrollBy(itemY_[i] + kEditH - scroll_ - viewH + 4);
                }
            SetFocus(edits_[e].h);
        } else {
            focus_ = v;
            SetFocus(h);
            ensureVisible(v);
        }
        InvalidateRect(h, nullptr, FALSE);
    };
    auto defaultButton = [&] {  // Enter in an edit box: the primary footer button
        for (const Item& it : items_)
            if (it.kind == Item::Kind::Buttons && it.footer)
                for (const Button& bt : it.buttons)
                    if (bt.primary && bt.enabled && it.enabled) return bt.id;
        return 0;
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
    case WM_SIZE:
        if (!inLayout_) fitClient();
        return 0;
    case WM_DPICHANGED: {
        // Suggested rectangle first (its position keeps the window under the
        // mouse while dragged), then our own size at the new DPI; layout()
        // also sets the render target's DPI and rebuilds the hit rectangles.
        dpi_ = testDpi > 0 ? static_cast<UINT>(testDpi) : HIWORD(wp);
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        inLayout_ = true;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        inLayout_ = false;
        applyEditFonts();
        if (impl_) layout();
        fitClient();
        InvalidateRect(h, nullptr, TRUE);
        return 0;
    }
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &pt);
        if (pt.y < 56 * s() && hitAt(pt) == 0) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && hot_ > 0 && hits_[static_cast<size_t>(hot_) - 1].enabled) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        int nh = hitAt(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (nh > 0 && !hits_[static_cast<size_t>(nh) - 1].enabled) nh = 0;
        if (nh != hot_) {
            hot_ = nh;
            if (impl_) impl_->anim.setHot(h, nh < HoverAnim::kMax ? nh : 0);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hot_) {
            hot_ = 0;
            if (impl_) impl_->anim.setHot(h, 0);
        }
        if (pressed_ && impl_) impl_->anim.setPressed(h, 0);
        return 0;
    case WM_MOUSEWHEEL:
        scrollBy(-GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * 48);
        return 0;
    case WM_TIMER:
        if (impl_ && (impl_->anim.onTimer(h, wp) || impl_->fade.onTimer(h, wp))) return 0;
        if (wp == kToggleTimer) {
            InvalidateRect(h, nullptr, FALSE);
            bool running = false;
            if (impl_)
                for (const auto& kv : impl_->toggles) running |= animNowMs() - kv.second.second < kToggleMs + 20;
            if (!running) KillTimer(h, kToggleTimer);
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
        pressed_ = hitAt(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (pressed_ > 0 && !hits_[static_cast<size_t>(pressed_) - 1].enabled) pressed_ = 0;
        if (impl_) impl_->anim.setPressed(h, pressed_ < HoverAnim::kMax ? pressed_ : 0);
        if (!pressed_) SetFocus(h);
        if (focusCues_) {
            focusCues_ = false;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        const int up = hitAt(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        const int was = pressed_;
        pressed_ = 0;
        if (impl_) impl_->anim.setPressed(h, 0);
        if (up && up == was) activate(up);  // may close the panel (and delete impl_)
        return 0;
    }
    case WM_KEYDOWN:
        switch (wp) {
        case VK_ESCAPE: activate(static_cast<int>(hits_.size())); return 0;  // × is the last hit
        case VK_TAB: moveFocus(GetKeyState(VK_SHIFT) < 0, nullptr); return 0;
        case VK_DOWN: moveFocus(false, nullptr); return 0;
        case VK_UP: moveFocus(true, nullptr); return 0;
        case VK_SPACE:
        case VK_RETURN:
            if (focus_) activate(focus_);
            else if (wp == VK_RETURN && cb_.onAction && defaultButton()) cb_.onAction(defaultButton());
            return 0;
        case VK_PRIOR: scrollBy(-(viewBottom() - viewTop()) * 0.8f); return 0;
        case VK_NEXT: scrollBy((viewBottom() - viewTop()) * 0.8f); return 0;
        }
        return 0;
    case kMsgEditKey:
        if (wp == VK_ESCAPE) activate(static_cast<int>(hits_.size()));
        else if (wp == VK_TAB) moveFocus(GetKeyState(VK_SHIFT) < 0, reinterpret_cast<HWND>(lp));
        else if (wp == VK_RETURN && cb_.onAction && defaultButton()) cb_.onAction(defaultButton());
        return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) InvalidateRect(h, nullptr, FALSE);
        if (HIWORD(wp) == EN_CHANGE && cb_.onEditChange)
            for (const EditBox& e : edits_)
                if (e.h == reinterpret_cast<HWND>(lp)) cb_.onEditChange(e.id);
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {  // (a disabled edit box asks with WM_CTLCOLORSTATIC)
        const Theme& t = impl_ ? impl_->th : theme();
        if (!editBrush_) editBrush_ = CreateSolidBrush(toRef(t.field));
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, toRef(IsWindowEnabled(reinterpret_cast<HWND>(lp)) ? t.fg : t.dim));
        SetBkColor(dc, toRef(t.field));
        return reinterpret_cast<LRESULT>(editBrush_);
    }
    case WM_CLOSE: activate(static_cast<int>(hits_.size())); return 0;
    case WM_DESTROY: SetWindowLongPtrW(h, GWLP_USERDATA, 0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace pm::ui
