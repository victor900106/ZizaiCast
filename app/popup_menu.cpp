// Custom-drawn popup menu (see popup_menu.h and docs/app.md "Menus").
//
// Each open level (root + submenus) is a WS_EX_LAYERED | WS_EX_NOACTIVATE |
// WS_EX_TOOLWINDOW | WS_EX_TOPMOST popup. Its pixels (rounded card, drawn
// soft shadow, items) are rendered by a software Direct2D DC render target
// into a premultiplied 32-bit DIB and pushed with UpdateLayeredWindow, so
// the shadow and the anti-aliased corners are real per-pixel alpha (DWM
// does not round layered windows; DWMWA_WINDOW_CORNER_PREFERENCE is set to
// "do not round" so it never clips the drawn shadow).
//
// Input: trackMenu() runs its own message loop (like TrackPopupMenu). The
// root window holds the mouse capture, so clicks anywhere else arrive here and
// dismiss the menu; keyboard messages for the thread (the owner is the
// foreground window) are taken out of the loop before dispatch. A 50 ms poll
// is the backstop when capture / foreground could not be obtained.
//
// Motion (0.7.4; none when Windows' "Animation effects" are off,
// SPI_GETCLIENTAREAANIMATION): each level fades in over 90 ms (the layered
// window's constant alpha), a row's highlight eases in over 100 ms and out
// over 150 ms, a pressed row is lit stronger, and a row reached from the
// keyboard gets a focus ring. One 10 ms timer on the root drives it and
// stops when nothing moves.
#include "popup_menu.h"
#include "pm/i18n.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellscalingapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <memory>

using Microsoft::WRL::ComPtr;

namespace pm::ui {
namespace {

// ---- palette ------------------------------------------------------------------
// Default = the video window's 櫻花粉 idle screen (warm dark card, blanket
// pink). setPalette() re-derives it from a theme swatch (VideoWindow::themeSwatch).
struct Palette {
    D2D1_COLOR_F cardTop{0.200f, 0.137f, 0.149f, 1};     // #332326
    D2D1_COLOR_F cardBottom{0.165f, 0.110f, 0.122f, 1};  // #2A1C1F
    D2D1_COLOR_F fg{1.00f, 0.955f, 0.945f, 1};           // warm white
    D2D1_COLOR_F dim{0.82f, 0.69f, 0.69f, 1};            // muted pink-grey
    D2D1_COLOR_F accent{0.96f, 0.655f, 0.655f, 1};       // #F5A7A7 blanket pink
    D2D1_COLOR_F shadow{0.04f, 0.015f, 0.02f, 1};
};
Palette g_pal;
constexpr float kDisabledAlpha = 0.38f;

D2D1_COLOR_F rgb(uint32_t c) {
    return {((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, 1};
}
D2D1_COLOR_F mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {  // a..b
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1};
}
float luminance(D2D1_COLOR_F c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

// ---- layout (DIPs) ----------------------------------------------------------
constexpr float kMargin = 16;     // transparent room around the card for the shadow
constexpr float kRadius = 8;      // card corners
constexpr float kPadY = 5;        // card padding above / below the rows
constexpr float kInset = 5;       // highlight inset from the card edge
constexpr float kItemH = 32;
constexpr float kSepH = 9;
constexpr float kCaptionH = 26;
constexpr float kNoteH = 26;
constexpr float kHeaderH = 44;
constexpr float kPadL = 10;       // inside the highlight
constexpr float kCheckW = 20;     // check / radio column
constexpr float kIconW = 28;      // icon column
constexpr float kGapRight = 32;   // text .. right column
constexpr float kArrowW = 14;
constexpr float kPadR = 12;
constexpr float kMinCardW = 188;
constexpr float kTextSize = 14, kSmallSize = 12, kIconSize = 16, kHeaderSize = 15;
constexpr UINT kSubmenuDelayMs = 200;
constexpr UINT_PTR kHoverTimer = 1, kPollTimer = 2, kAnimTimer = 3;
constexpr float kFadeInMs = 90, kHoverInMs = 100, kHoverOutMs = 150;

// Windows' "Animation effects" (Settings > Accessibility > Visual effects).
bool animationsOn() {
    BOOL on = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) on = TRUE;
    return on != FALSE;
}
constexpr wchar_t kClassName[] = L"PhoneMirrorPopupMenu";

D2D1_COLOR_F alpha(D2D1_COLOR_F c, float a) {
    c.a *= a;
    return c;
}

// ---- shared factories / text formats -----------------------------------------
struct Resources {
    bool ready = false;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IDWriteTextFormat> text, bold, right, caption, note, icon, noteIcon, header;
    bool haveIcons = false;
    int lang = -1;  // pm::i18n language the text formats were made for (UI font)

    ComPtr<IDWriteTextFormat> makeFormat(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                                         DWRITE_TEXT_ALIGNMENT align) {
        ComPtr<IDWriteTextFormat> f;
        if (FAILED(dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        size, pm::i18n::localeName(), &f)))
            return nullptr;
        f->SetTextAlignment(align);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return f;
    }

    // UI-language text formats (again after a language switch).
    bool makeTextFormats() {
        lang = static_cast<int>(pm::i18n::lang());
        const wchar_t* ui = pm::i18n::uiFont();
        text = makeFormat(ui, kTextSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
        bold = makeFormat(ui, kTextSize, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
        right = makeFormat(L"Segoe UI", kSmallSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
        caption = makeFormat(ui, kSmallSize, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
        note = makeFormat(ui, kSmallSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
        header = makeFormat(ui, kHeaderSize, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
        return text && bold && right && caption && note && header;
    }

    bool init() {
        if (ready) return lang == static_cast<int>(pm::i18n::lang()) || makeTextFormats();
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(dw.GetAddressOf()))))
            return false;
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // UI thread; harmless if already set
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));

        if (!makeTextFormats()) return false;

        // Icon font: Segoe Fluent Icons (Windows 11), else Segoe MDL2 Assets
        // (Windows 10), else no icons at all.
        ComPtr<IDWriteFontCollection> fonts;
        if (SUCCEEDED(dw->GetSystemFontCollection(&fonts))) {
            for (const wchar_t* fam : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
                UINT32 idx = 0;
                BOOL exists = FALSE;
                if (SUCCEEDED(fonts->FindFamilyName(fam, &idx, &exists)) && exists) {
                    icon = makeFormat(fam, kIconSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
                    noteIcon = makeFormat(fam, kSmallSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
                    haveIcons = icon && noteIcon;
                    break;
                }
            }
        }
        ready = true;
        return true;
    }

    float width(IDWriteTextFormat* f, const std::wstring& s) {
        if (s.empty() || !f) return 0;
        ComPtr<IDWriteTextLayout> l;
        if (FAILED(dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, 4096, 64, &l))) return 0;
        DWRITE_TEXT_METRICS m{};
        l->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    }
};

Resources& res() {
    static Resources r;
    return r;
}

using Kind = MenuItem::Kind;

bool selectable(const MenuItem& it) {
    return (it.kind == Kind::Command || it.kind == Kind::Submenu) && it.enabled;
}

// ---- one menu level -----------------------------------------------------------
struct Row {
    const MenuItem* item;
    float y, h;  // DIPs from the card top
};

struct Level {
    HWND hwnd = nullptr;
    const std::vector<MenuItem>* items = nullptr;
    std::vector<Row> rows;
    UINT dpi = 96;
    float scale = 1;
    int marginPx = 0;
    SIZE cardPx{};  // card size in pixels
    SIZE winPx{};   // window size in pixels (card + shadow margins)
    POINT winPos{}; // window top-left, screen pixels
    float cardW = 0, cardH = 0;  // DIPs (cardPx / scale)
    float textX = 0;             // DIPs from the card's left edge
    bool hasCheck = false, hasIcon = false, hasArrow = false;
    int hot = -1;      // highlighted row
    int openSub = -1;  // row whose submenu is the next level
    int pressed = -1;  // row under a held mouse button
    std::vector<float> hov;  // per row: highlight 0..1 (eases toward hot / openSub)
    float fade = 1;          // window opacity 0..1 (fade-in on open)
    HDC memDC = nullptr;
    HBITMAP dib = nullptr;
    void* bits = nullptr;  // the DIB's premultiplied BGRA pixels
    HGDIOBJ oldBmp = nullptr;
    ComPtr<ID2D1DCRenderTarget> rt;
    ComPtr<ID2D1Bitmap> headerIcon;

    ~Level() {
        rt.Reset();
        headerIcon.Reset();
        if (memDC) {
            SelectObject(memDC, oldBmp);
            DeleteDC(memDC);
        }
        if (dib) DeleteObject(dib);
        if (hwnd) DestroyWindow(hwnd);
    }

    RECT cardRect() const {  // screen pixels
        return {winPos.x + marginPx, winPos.y + marginPx, winPos.x + marginPx + cardPx.cx,
                winPos.y + marginPx + cardPx.cy};
    }
    bool contains(POINT pt) const {
        RECT r = cardRect();
        return PtInRect(&r, pt) != FALSE;
    }
    int rowAt(POINT pt) const {
        const RECT r = cardRect();
        if (!PtInRect(&r, pt)) return -1;
        const float y = (pt.y - r.top) / scale;
        for (size_t i = 0; i < rows.size(); ++i)
            if (y >= rows[i].y && y < rows[i].y + rows[i].h) return static_cast<int>(i);
        return -1;
    }
    int rowTopPx(int row) const { return cardRect().top + static_cast<int>(std::lround(rows[row].y * scale)); }
};

UINT monitorDpi(HMONITOR mon) {
    UINT x = 96, y = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &x, &y))) x = 96;
    return x;
}

// Rows and card size (DIPs) for a level at `dpi`.
void layoutLevel(Level& lv, UINT dpi) {
    Resources& r = res();
    lv.dpi = dpi;
    lv.scale = dpi / 96.0f;
    lv.rows.clear();
    lv.hasCheck = lv.hasIcon = lv.hasArrow = false;
    for (const MenuItem& it : *lv.items) {
        lv.hasCheck |= it.checkable || it.radio;
        lv.hasIcon |= (it.icon != 0 && r.haveIcons) || it.hasSwatch;
        lv.hasArrow |= it.kind == Kind::Submenu;
    }
    lv.textX = kInset + kPadL + (lv.hasCheck ? kCheckW : 0) + (lv.hasIcon ? kIconW : 0);
    float y = kPadY, textW = 0, rightW = 0, otherW = 0;
    for (const MenuItem& it : *lv.items) {
        float h = kItemH;
        switch (it.kind) {
        case Kind::Separator: h = kSepH; break;
        case Kind::Caption:
            h = kCaptionH;
            otherW = (std::max)(otherW, kInset + kPadL + r.width(r.caption.Get(), it.text) + kPadR + kInset);
            break;
        case Kind::Note:
            h = kNoteH;
            otherW = (std::max)(otherW, lv.textX + r.width(r.note.Get(), it.text) + kPadR + kInset);
            break;
        case Kind::Header:
            h = kHeaderH;
            otherW = (std::max)(otherW, kInset + kPadL + 30 + r.width(r.header.Get(), it.text) + kGapRight +
                                            r.width(r.right.Get(), it.right) + kPadR + kInset);
            break;
        default:
            textW = (std::max)(textW, r.width((it.bold ? r.bold : r.text).Get(), it.text));
            rightW = (std::max)(rightW, r.width(r.right.Get(), it.right));
            break;
        }
        lv.rows.push_back({&it, y, h});
        y += h;
    }
    lv.hov.assign(lv.rows.size(), 0.0f);
    float w = lv.textX + textW + (rightW > 0 ? kGapRight + rightW : 0) + (lv.hasArrow ? kGapRight / 2 + kArrowW : 0) +
              kPadR + kInset;
    w = (std::max)({w, otherW, kMinCardW});
    // Whole pixels for the card so its edges stay crisp.
    lv.cardPx = {static_cast<LONG>(std::ceil(w * lv.scale)), static_cast<LONG>(std::ceil((y + kPadY) * lv.scale))};
    lv.cardW = lv.cardPx.cx / lv.scale;
    lv.cardH = lv.cardPx.cy / lv.scale;
    lv.marginPx = static_cast<int>(std::lround(kMargin * lv.scale));
    lv.winPx = {lv.cardPx.cx + 2 * lv.marginPx, lv.cardPx.cy + 2 * lv.marginPx};
}

LRESULT CALLBACK menuProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

void registerClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = menuProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    done = true;
}

// ---- tracker: the open menu chain and the modal loop --------------------------
class Tracker {
public:
    Tracker(HWND owner, const MenuOptions& o) : owner_(owner), opts_(o) {}

    UINT run(const std::vector<MenuItem>& items, POINT pt, std::chrono::steady_clock::time_point t0, double& openMs);
    // Draws the root level (no window) and saves it as a 32-bit PNG.
    bool renderPng(const std::vector<MenuItem>& items, const std::wstring& path, int hotRow);
    void captureChanged(HWND to) {
        if (done_) return;
        for (auto& lv : levels_)
            if (lv->hwnd == to) return;
        finish(0);
    }

private:
    bool openLevel(const std::vector<MenuItem>& items, POINT anchor, const RECT* parentRow);
    bool prepare(Level& lv);  // DIB, render target, header icon
    void closeFrom(size_t level);
    void openSubmenu(size_t level, int row, bool selectFirst);
    void draw(Level& lv);
    void redraw(size_t level) {
        if (level < levels_.size()) draw(*levels_[level]);
    }
    bool filter(const MSG& msg);
    void onMouseMove(POINT pt);
    void onButtonDown(POINT pt);
    void onButtonUp(POINT pt);
    void onKey(WPARAM vk);
    void onHoverTimer();
    void onPoll();
    void startAnim();
    void onAnimTimer();
    bool settle(Level& lv, float dtMs);  // one animation step; true if anything changed
    void activate(size_t level, int row, bool fromKeyboard);
    void setHot(size_t level, int row);
    void moveHot(size_t level, int dir, bool fromEnd);
    int hitLevel(POINT pt) const {
        for (size_t i = levels_.size(); i-- > 0;)
            if (levels_[i]->contains(pt)) return static_cast<int>(i);
        return -1;
    }
    bool ours(HWND h) const {
        for (auto& lv : levels_)
            if (lv->hwnd == h) return true;
        return false;
    }
    void finish(UINT result) {
        if (done_) return;
        done_ = true;
        result_ = result;
    }

    HWND owner_;
    MenuOptions opts_;
    std::vector<std::unique_ptr<Level>> levels_;
    size_t active_ = 0;  // level with keyboard focus
    bool done_ = false;
    UINT result_ = 0;
    bool wasForeground_ = false;
    int pendLevel_ = -1, pendRow_ = -1;  // submenu hover timer target
    bool anim_ = animationsOn();
    bool animRunning_ = false;
    std::chrono::steady_clock::time_point lastTick_{};
    bool kbFocus_ = false;  // the highlight was moved with the keyboard: focus ring
    POINT lastMouse_{LONG_MIN, LONG_MIN};
};

LRESULT CALLBACK menuProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCHITTEST: return HTCLIENT;
    case WM_CAPTURECHANGED:
        if (auto* t = reinterpret_cast<Tracker*>(GetWindowLongPtrW(h, GWLP_USERDATA)))
            t->captureChanged(reinterpret_cast<HWND>(lp));
        return 0;
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool Tracker::openLevel(const std::vector<MenuItem>& items, POINT anchor, const RECT* parentRow) {
    auto lv = std::make_unique<Level>();
    lv->items = &items;
    const HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    layoutLevel(*lv, monitorDpi(mon));
    const RECT& wa = mi.rcWork;
    const LONG cw = lv->cardPx.cx, ch = lv->cardPx.cy;
    LONG x, y;
    if (!parentRow) {  // at the cursor; flip left / up when it would leave the work area
        x = anchor.x;
        y = anchor.y;
        if (x + cw > wa.right && anchor.x - cw >= wa.left) x = anchor.x - cw;
        if (y + ch > wa.bottom) y = anchor.y - ch;
    } else {  // beside the parent row: right, else left of the parent card
        const LONG gap = static_cast<LONG>(std::lround(2 * lv->scale));
        const RECT& p = *parentRow;  // parent card left/right, row top
        x = p.right + gap;
        if (x + cw > wa.right) x = p.left - gap - cw;
        y = p.top - static_cast<LONG>(std::lround(kPadY * lv->scale));
    }
    x = (std::max)(wa.left, (std::min)(x, wa.right - cw));
    y = (std::max)(wa.top, (std::min)(y, wa.bottom - ch));
    lv->winPos = {x - lv->marginPx, y - lv->marginPx};

    lv->hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kClassName, L"",
                               WS_POPUP, lv->winPos.x, lv->winPos.y, lv->winPx.cx, lv->winPx.cy, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    if (!lv->hwnd) return false;
    SetWindowLongPtrW(lv->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND;  // the corners are drawn
    DwmSetWindowAttribute(lv->hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    if (!prepare(*lv)) return false;

    Level& ref = *lv;
    ref.fade = anim_ ? 0.0f : 1.0f;
    levels_.push_back(std::move(lv));
    draw(ref);
    ShowWindow(ref.hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(ref.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (anim_) startAnim();
    return true;
}

bool Tracker::prepare(Level& level) {
    Level* lv = &level;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = lv->winPx.cx;
    bi.bmiHeader.biHeight = -lv->winPx.cy;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    lv->memDC = CreateCompatibleDC(nullptr);
    lv->dib = CreateDIBSection(lv->memDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!lv->memDC || !lv->dib) return false;
    lv->bits = bits;
    lv->oldBmp = SelectObject(lv->memDC, lv->dib);
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), static_cast<float>(lv->dpi),
        static_cast<float>(lv->dpi));
    if (FAILED(res().d2d->CreateDCRenderTarget(&props, &lv->rt))) return false;
    lv->rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    // Header icon (tray menu): the app icon at 20 DIPs for this DPI.
    if (opts_.headerIconId && res().wic) {
        for (const MenuItem& it : *lv->items) {
            if (it.kind != Kind::Header) continue;
            const int px = static_cast<int>(std::lround(20 * lv->scale));
            HICON icon = nullptr;
            if (SUCCEEDED(LoadIconWithScaleDown(opts_.iconInstance, MAKEINTRESOURCEW(opts_.headerIconId), px, px,
                                                &icon))) {
                ComPtr<IWICBitmap> wb;
                ComPtr<IWICFormatConverter> conv;
                if (SUCCEEDED(res().wic->CreateBitmapFromHICON(icon, &wb)) &&
                    SUCCEEDED(res().wic->CreateFormatConverter(&conv)) &&
                    SUCCEEDED(conv->Initialize(wb.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                               nullptr, 0, WICBitmapPaletteTypeCustom)))
                    lv->rt->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &lv->headerIcon);
                DestroyIcon(icon);
            }
            break;
        }
    }
    return true;
}

bool Tracker::renderPng(const std::vector<MenuItem>& items, const std::wstring& path, int hotRow) {
    Resources& r = res();
    if (!r.wic) return false;
    Level lv;
    lv.items = &items;
    layoutLevel(lv, monitorDpi(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY)));
    if (!prepare(lv)) return false;
    lv.hot = hotRow;
    if (hotRow >= 0 && hotRow < static_cast<int>(lv.hov.size())) lv.hov[hotRow] = opts_.shotHover;
    if (opts_.shotPressed) lv.pressed = hotRow;
    kbFocus_ = opts_.selectFirst;
    draw(lv);  // UpdateLayeredWindow without a window does nothing
    GdiFlush();
    ComPtr<IWICBitmap> bmp;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    const UINT w = lv.winPx.cx, h = lv.winPx.cy;
    if (FAILED(r.wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppPBGRA, w * 4, w * h * 4,
                                             static_cast<BYTE*>(lv.bits), &bmp)) ||
        FAILED(r.wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(r.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize(w, h)))
        return false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(r.wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(bmp.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)) ||
        FAILED(frame->SetPixelFormat(&fmt)) || FAILED(frame->WriteSource(conv.Get(), nullptr)) ||
        FAILED(frame->Commit()) || FAILED(enc->Commit()))
        return false;
    return true;
}

void Tracker::closeFrom(size_t level) {
    if (level == 0) return;  // the root closes only with the whole menu
    while (levels_.size() > level) levels_.pop_back();
    if (levels_[level - 1]->openSub >= 0) {
        levels_[level - 1]->openSub = -1;
        if (anim_) startAnim();
        else settle(*levels_[level - 1], 1e6f);
        redraw(level - 1);
    }
    active_ = (std::min)(active_, levels_.size() - 1);
}

void Tracker::openSubmenu(size_t level, int row, bool selectFirst) {
    Level& p = *levels_[level];
    if (p.openSub == row && levels_.size() > level + 1) {
        if (selectFirst) {
            active_ = level + 1;
            if (levels_[active_]->hot < 0) moveHot(active_, +1, true);
        }
        return;
    }
    closeFrom(level + 1);
    const MenuItem& it = *p.rows[row].item;
    if (it.kind != Kind::Submenu || !it.enabled || it.sub.empty()) return;
    const RECT card = p.cardRect();
    const RECT anchor{card.left, p.rowTopPx(row), card.right, 0};
    p.openSub = row;
    p.hot = row;
    if (anim_) startAnim();
    else settle(p, 1e6f);
    redraw(level);
    if (!openLevel(it.sub, POINT{card.right, anchor.top}, &anchor)) {
        p.openSub = -1;
        return;
    }
    if (selectFirst) {
        active_ = level + 1;
        moveHot(active_, +1, true);
    }
}

void Tracker::setHot(size_t level, int row) {
    Level& lv = *levels_[level];
    if (lv.hot == row) return;
    lv.hot = row;
    if (lv.pressed != row) lv.pressed = -1;
    if (anim_) {
        startAnim();
        redraw(level);  // the row's text / icon state at once; the wash follows
        return;
    }
    settle(lv, 1e6f);
    redraw(level);
}

// Highlights ease toward their target (hot or the open submenu's row: 1,
// else 0); the level fades in. dtMs huge = jump to the end.
bool Tracker::settle(Level& lv, float dtMs) {
    bool changed = false;
    for (size_t i = 0; i < lv.hov.size(); ++i) {
        const int r = static_cast<int>(i);
        const float target = ((r == lv.hot && selectable(*lv.rows[i].item)) || r == lv.openSub) ? 1.0f : 0.0f;
        float& h = lv.hov[i];
        if (h == target) continue;
        h = target > h ? (std::min)(target, h + dtMs / kHoverInMs) : (std::max)(target, h - dtMs / kHoverOutMs);
        changed = true;
    }
    if (lv.fade < 1) {
        lv.fade = (std::min)(1.0f, lv.fade + dtMs / kFadeInMs);
        changed = true;
    }
    return changed;
}

void Tracker::startAnim() {
    if (animRunning_ || levels_.empty()) return;
    animRunning_ = true;
    lastTick_ = std::chrono::steady_clock::now();
    SetTimer(levels_[0]->hwnd, kAnimTimer, 10, nullptr);
}

void Tracker::onAnimTimer() {
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float, std::milli>(now - lastTick_).count();
    lastTick_ = now;
    bool any = false;
    for (size_t i = 0; i < levels_.size(); ++i)
        if (settle(*levels_[i], dt)) {
            draw(*levels_[i]);
            any = true;
        }
    if (!any) {
        KillTimer(levels_[0]->hwnd, kAnimTimer);
        animRunning_ = false;
    }
}

void Tracker::moveHot(size_t level, int dir, bool fromEnd) {
    Level& lv = *levels_[level];
    const int n = static_cast<int>(lv.rows.size());
    int i = fromEnd || lv.hot < 0 ? (dir > 0 ? -1 : n) : lv.hot;
    for (int step = 0; step < n; ++step) {
        i = (i + dir + n) % n;
        if (selectable(*lv.rows[i].item)) {
            closeFrom(level + 1);
            setHot(level, i);
            return;
        }
    }
}

void Tracker::activate(size_t level, int row, bool fromKeyboard) {
    if (row < 0) return;
    const MenuItem& it = *levels_[level]->rows[row].item;
    if (!selectable(it)) return;
    if (it.kind == Kind::Submenu) {
        KillTimer(levels_[0]->hwnd, kHoverTimer);
        openSubmenu(level, row, fromKeyboard);
        return;
    }
    finish(it.id);
}

void Tracker::onMouseMove(POINT pt) {
    // A real move (not the WM_MOUSEMOVE Windows sends when a window appears
    // under a still cursor) hands the highlight back to the mouse.
    if (pt.x != lastMouse_.x || pt.y != lastMouse_.y) {
        const bool first = lastMouse_.x == LONG_MIN;
        lastMouse_ = pt;
        if (!first && kbFocus_) {
            kbFocus_ = false;
            for (size_t i = 0; i < levels_.size(); ++i) redraw(i);
        }
    }
    const int hit = hitLevel(pt);
    if (hit < 0) {
        // Off the menus: drop the highlight of the deepest level unless it is
        // the parent of an open submenu.
        Level& top = *levels_.back();
        if (top.hot >= 0 && top.hot != top.openSub) setHot(levels_.size() - 1, -1);
        return;
    }
    const size_t level = static_cast<size_t>(hit);
    active_ = level;
    Level& lv = *levels_[level];
    int row = lv.rowAt(pt);
    if (row >= 0 && !selectable(*lv.rows[row].item)) row = -1;
    for (size_t i = 0; i < levels_.size(); ++i) {
        if (i < level && levels_[i]->openSub >= 0) setHot(i, levels_[i]->openSub);  // parents keep their row lit
        if (i > level) setHot(i, levels_[i]->openSub);  // deeper levels lose the hover
    }
    if (row == lv.hot) return;
    setHot(level, row);
    // Open a hovered submenu / close one that is no longer hovered, after a delay.
    const bool opens = row >= 0 && lv.rows[row].item->kind == Kind::Submenu && row != lv.openSub;
    const bool closes = levels_.size() > level + 1 && row != lv.openSub && row >= 0;
    if (opens || closes) {
        pendLevel_ = hit;
        pendRow_ = row;
        SetTimer(levels_[0]->hwnd, kHoverTimer, kSubmenuDelayMs, nullptr);
    }
}

void Tracker::onHoverTimer() {
    KillTimer(levels_[0]->hwnd, kHoverTimer);
    const int level = pendLevel_, row = pendRow_;
    pendLevel_ = pendRow_ = -1;
    if (level < 0 || static_cast<size_t>(level) >= levels_.size()) return;
    Level& lv = *levels_[level];
    if (lv.hot != row || row < 0 || row == lv.openSub) return;  // moved on meanwhile
    closeFrom(level + 1);
    if (lv.rows[row].item->kind == Kind::Submenu) openSubmenu(level, row, false);
}

void Tracker::onButtonDown(POINT pt) {
    const int hit = hitLevel(pt);
    if (hit < 0) {
        finish(0);  // click outside: dismiss (the click is eaten, like native menus)
        return;
    }
    // Pressed look on a command row until the button comes up (it acts then).
    Level& lv = *levels_[hit];
    const int row = lv.rowAt(pt);
    if (row >= 0 && selectable(*lv.rows[row].item) && lv.rows[row].item->kind == Kind::Command) {
        if (lv.hot != row) setHot(static_cast<size_t>(hit), row);
        lv.pressed = row;
        redraw(static_cast<size_t>(hit));
    }
}

void Tracker::onButtonUp(POINT pt) {
    for (size_t i = 0; i < levels_.size(); ++i)
        if (levels_[i]->pressed >= 0) {
            levels_[i]->pressed = -1;
            redraw(i);
        }
    const int hit = hitLevel(pt);
    if (hit < 0) return;
    activate(static_cast<size_t>(hit), levels_[hit]->rowAt(pt), false);
}

void Tracker::onKey(WPARAM vk) {
    if ((vk == VK_DOWN || vk == VK_UP || vk == VK_HOME || vk == VK_END || vk == VK_RIGHT || vk == VK_LEFT) &&
        !kbFocus_) {
        kbFocus_ = true;
        for (size_t i = 0; i < levels_.size(); ++i) redraw(i);
    }
    Level& lv = *levels_[active_];
    switch (vk) {
    case VK_DOWN: moveHot(active_, +1, false); break;
    case VK_UP: moveHot(active_, -1, false); break;
    case VK_HOME: moveHot(active_, +1, true); break;
    case VK_END: moveHot(active_, -1, true); break;
    case VK_RIGHT:
        if (lv.hot >= 0 && lv.rows[lv.hot].item->kind == Kind::Submenu) activate(active_, lv.hot, true);
        break;
    case VK_LEFT:
        if (active_ > 0) closeFrom(active_);
        break;
    case VK_ESCAPE:
        if (active_ > 0) closeFrom(active_);
        else finish(0);
        break;
    case VK_RETURN:
    case VK_SPACE: activate(active_, lv.hot, true); break;
    case VK_MENU:
    case VK_F10:
    case VK_LWIN:
    case VK_RWIN: finish(0); break;
    }
}

void Tracker::onPoll() {
    if (wasForeground_) {
        DWORD tid = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        if (tid != GetCurrentThreadId()) {  // another app was activated
            finish(0);
            return;
        }
    }
    if (GetCapture() == levels_[0]->hwnd) return;  // clicks arrive as messages
    // Backstop without capture: a click anywhere outside the menus dismisses it.
    const bool down = (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) &
                      0x8000;
    POINT pt{};
    GetCursorPos(&pt);
    if (down && hitLevel(pt) < 0) finish(0);
}

// Returns true if the message was consumed by the menu.
bool Tracker::filter(const MSG& msg) {
    const UINT m = msg.message;
    if (m >= WM_KEYFIRST && m <= WM_KEYLAST) {
        if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) onKey(msg.wParam);
        return true;  // nothing reaches the app (F11, Ctrl+T, ...) while the menu is open
    }
    if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) {
        const POINT pt = msg.pt;  // screen position (works with and without capture)
        switch (m) {
        case WM_MOUSEMOVE: onMouseMove(pt); break;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: onButtonDown(pt); break;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP: onButtonUp(pt); break;
        }
        return true;
    }
    if (m >= WM_NCMOUSEMOVE && m <= WM_NCXBUTTONDBLCLK) {
        if (m != WM_NCMOUSEMOVE) finish(0);
        return true;
    }
    if (m == WM_TIMER && ours(msg.hwnd)) {
        if (msg.wParam == kHoverTimer) onHoverTimer();
        else if (msg.wParam == kPollTimer) onPoll();
        else if (msg.wParam == kAnimTimer) onAnimTimer();
        return true;
    }
    return false;
}

UINT Tracker::run(const std::vector<MenuItem>& items, POINT pt, std::chrono::steady_clock::time_point t0,
                  double& openMs) {
    if (!openLevel(items, pt, nullptr)) return 0;
    openMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const HWND root = levels_[0]->hwnd;
    wasForeground_ = GetWindowThreadProcessId(GetForegroundWindow(), nullptr) == GetCurrentThreadId();
    SetCapture(root);
    SetTimer(root, kPollTimer, 50, nullptr);
    if (opts_.selectFirst) {
        kbFocus_ = true;
        moveHot(0, +1, true);
    }
    POINT cur{};
    GetCursorPos(&cur);
    if (hitLevel(cur) >= 0) onMouseMove(cur);  // cursor already over an item

    MSG msg;
    while (!done_) {
        const BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r == 0) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        if (r < 0) break;
        if (filter(msg)) continue;
        if (msg.message == WM_CANCELMODE || (msg.hwnd == owner_ && msg.message == WM_CLOSE)) finish(0);
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    done_ = true;  // WM_CAPTURECHANGED from here on is ours
    KillTimer(root, kPollTimer);
    KillTimer(root, kHoverTimer);
    KillTimer(root, kAnimTimer);
    if (GetCapture() == root) ReleaseCapture();
    while (!levels_.empty()) {
        ShowWindow(levels_.back()->hwnd, SW_HIDE);
        levels_.pop_back();
    }
    return result_;
}

// ---- drawing ---------------------------------------------------------------------
void Tracker::draw(Level& lv) {
    Resources& r = res();
    ID2D1DCRenderTarget* rt = lv.rt.Get();
    const RECT bind{0, 0, lv.winPx.cx, lv.winPx.cy};
    if (FAILED(rt->BindDC(lv.memDC, &bind))) return;
    const float s = lv.scale;
    auto snap = [s](float v) { return std::round(v * s) / s; };
    const float px = 1 / s;

    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    ComPtr<ID2D1SolidColorBrush> brush;
    rt->CreateSolidColorBrush(g_pal.fg, &brush);
    auto fill = [&](D2D1_COLOR_F c) -> ID2D1Brush* {
        brush->SetColor(c);
        return brush.Get();
    };

    const float m = lv.marginPx / s;
    const D2D1_RECT_F card{m, m, m + lv.cardW, m + lv.cardH};

    // Soft shadow: stacked translucent rounded rects, slightly lower than the card.
    constexpr int kLayers = 12;
    for (int i = kLayers; i >= 1; --i) {
        const float grow = i * 1.1f;
        const D2D1_ROUNDED_RECT rr{{card.left - grow, card.top - grow + 3, card.right + grow, card.bottom + grow + 3},
                                   kRadius + grow, kRadius + grow};
        rt->FillRoundedRectangle(rr, fill(alpha(g_pal.shadow, 0.045f)));
    }
    // Card: warm dark vertical gradient, hairline pink border.
    const D2D1_ROUNDED_RECT cardRR{card, kRadius, kRadius};
    {
        ComPtr<ID2D1GradientStopCollection> stops;
        const D2D1_GRADIENT_STOP gs[2] = {{0, g_pal.cardTop}, {1, g_pal.cardBottom}};
        rt->CreateGradientStopCollection(gs, 2, &stops);
        ComPtr<ID2D1LinearGradientBrush> grad;
        if (stops)
            rt->CreateLinearGradientBrush({{0, card.top}, {0, card.bottom}}, stops.Get(), &grad);
        rt->FillRoundedRectangle(cardRR, grad ? static_cast<ID2D1Brush*>(grad.Get()) : fill(g_pal.cardTop));
    }

    // Everything else is clipped to the card's rounded shape.
    ComPtr<ID2D1RoundedRectangleGeometry> clip;
    r.d2d->CreateRoundedRectangleGeometry(cardRR, &clip);
    ComPtr<ID2D1Layer> layer;
    rt->CreateLayer(&layer);
    if (layer && clip) rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), layer.Get());

    const float left = card.left, right = card.right;
    bool header = false;
    for (size_t i = 0; i < lv.rows.size(); ++i) {
        const Row& row = lv.rows[i];
        const MenuItem& it = *row.item;
        const float top = card.top + row.y, bottom = top + row.h, cy = (top + bottom) / 2;
        switch (it.kind) {
        case Kind::Separator: {
            const float y = snap(cy) + px / 2;
            rt->DrawLine({left + kInset + 6, y}, {right - kInset - 6, y}, fill(alpha(g_pal.fg, 0.10f)), px);
            continue;
        }
        case Kind::Caption: {
            const D2D1_RECT_F tr{left + kInset + kPadL, top + 4, right - kPadR, bottom};
            rt->DrawText(it.text.c_str(), static_cast<UINT32>(it.text.size()), r.caption.Get(), tr,
                         fill(alpha(g_pal.accent, 0.80f)), D2D1_DRAW_TEXT_OPTIONS_NONE);
            continue;
        }
        case Kind::Note: {
            if (r.haveIcons) {  // info glyph in the icon column (or just left of the text)
                const wchar_t info = 0xE946;
                const float ix = left + lv.textX - (lv.hasIcon ? kIconW : 20);
                rt->DrawText(&info, 1, r.noteIcon.Get(), {ix, top, ix + 16, bottom}, fill(alpha(g_pal.dim, 0.75f)));
            }
            rt->DrawText(it.text.c_str(), static_cast<UINT32>(it.text.size()), r.note.Get(),
                         {left + lv.textX, top, right - kPadR, bottom}, fill(alpha(g_pal.dim, 0.85f)));
            continue;
        }
        case Kind::Header: {
            header = true;
            // Faint pink wash behind the app name.
            rt->FillRectangle({left, card.top, right, bottom}, fill(alpha(g_pal.accent, 0.07f)));
            const float ix = snap(left + kInset + kPadL), iy = snap(cy - 10);
            if (lv.headerIcon) rt->DrawBitmap(lv.headerIcon.Get(), {ix, iy, ix + 20, iy + 20});
            rt->DrawText(it.text.c_str(), static_cast<UINT32>(it.text.size()), r.header.Get(),
                         {ix + 30, top, right - kPadR, bottom}, fill(g_pal.accent));
            if (!it.right.empty())
                rt->DrawText(it.right.c_str(), static_cast<UINT32>(it.right.size()), r.right.Get(),
                             {left, top, right - kInset - kPadR, bottom}, fill(alpha(g_pal.dim, 0.7f)));
            continue;
        }
        default: break;
        }

        const bool enabled = it.enabled;
        const float a = enabled ? 1.0f : kDisabledAlpha;
        const bool hot = static_cast<int>(i) == lv.hot && enabled;
        const bool open = static_cast<int>(i) == lv.openSub;
        const bool pressed = hot && static_cast<int>(i) == lv.pressed;
        // Highlight strength 0..1 (animated, see settle), eased: quick start, soft landing.
        const float h0 = i < lv.hov.size() ? lv.hov[i] : ((hot || open) ? 1.0f : 0.0f);
        const float hv = 1 - (1 - h0) * (1 - h0);
        if (hv > 0.004f) {
            const D2D1_ROUNDED_RECT hr{{left + kInset, top + 1, right - kInset, bottom - 1}, 5, 5};
            const float strength = pressed ? 0.36f : (hot || !open) ? 0.22f : 0.14f;
            rt->FillRoundedRectangle(hr, fill(alpha(g_pal.accent, strength * hv)));
            // Small pink bar at the highlight's left edge (shorter while pressed).
            if (hot || !open) {
                const float bh = pressed ? 5.0f : 7.0f;
                const D2D1_ROUNDED_RECT bar{{left + kInset, cy - bh, left + kInset + 3, cy + bh}, 1.5f, 1.5f};
                rt->FillRoundedRectangle(bar, fill(alpha(g_pal.accent, hv)));
            }
        }
        if (hot && kbFocus_) {  // keyboard focus ring
            const D2D1_ROUNDED_RECT ring{
                {left + kInset + 0.75f, top + 1.75f, right - kInset - 0.75f, bottom - 1.75f}, 4.5f, 4.5f};
            rt->DrawRoundedRectangle(ring, fill(alpha(g_pal.accent, 0.85f)), 1.5f);
        }
        float x = left + kInset + kPadL;
        if (lv.hasCheck) {
            const float cx = x + kCheckW / 2 - 2;
            if (it.checkable && it.checked) {
                ComPtr<ID2D1PathGeometry> g;
                r.d2d->CreatePathGeometry(&g);
                ComPtr<ID2D1GeometrySink> sink;
                if (g && SUCCEEDED(g->Open(&sink))) {
                    sink->BeginFigure({cx - 4.5f, cy + 0.2f}, D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddLine({cx - 1.5f, cy + 3.3f});
                    sink->AddLine({cx + 4.5f, cy - 3.6f});
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    sink->Close();
                    ComPtr<ID2D1StrokeStyle> round;
                    r.d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                         D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                                             nullptr, 0, &round);
                    rt->DrawGeometry(g.Get(), fill(alpha(g_pal.accent, a)), 1.8f, round.Get());
                }
            } else if (it.radio) {
                const D2D1_ELLIPSE ring{{cx, cy}, 5, 5};
                if (it.checked) {
                    rt->DrawEllipse(ring, fill(alpha(g_pal.accent, a)), 1.3f);
                    rt->FillEllipse({{cx, cy}, 2.6f, 2.6f}, fill(alpha(g_pal.accent, a)));
                } else {
                    rt->DrawEllipse(ring, fill(alpha(g_pal.dim, 0.45f * a)), 1.1f);
                }
            }
            x += kCheckW;
        }
        if (lv.hasIcon) {
            if (it.icon && r.haveIcons) {
                const D2D1_COLOR_F ic = (it.checkable && it.checked) ? g_pal.accent : hot ? g_pal.fg : g_pal.dim;
                rt->DrawText(&it.icon, 1, r.icon.Get(), {x, top, x + 16, bottom}, fill(alpha(ic, a)));
            } else if (it.hasSwatch) {
                // Theme swatch: an accent-coloured disc with the card colour
                // as a ring and the background as the centre dot.
                const D2D1_POINT_2F c{x + 8, cy};
                rt->FillEllipse({c, 8, 8}, fill(alpha(rgb(it.swatch[2]), a)));
                rt->DrawEllipse({c, 4.6f, 4.6f}, fill(alpha(rgb(it.swatch[1]), a)), 1.6f);
                rt->FillEllipse({c, 3.8f, 3.8f}, fill(alpha(rgb(it.swatch[0]), a)));
                rt->DrawEllipse({c, 8 - px / 2, 8 - px / 2}, fill(alpha(g_pal.fg, 0.28f * a)), px);
            }
            x += kIconW;
        }
        float textRight = right - kInset - kPadR;
        if (it.kind == Kind::Submenu) {
            // Chevron
            const float ax = textRight - 3, ay = cy;
            ComPtr<ID2D1PathGeometry> g;
            r.d2d->CreatePathGeometry(&g);
            ComPtr<ID2D1GeometrySink> sink;
            if (g && SUCCEEDED(g->Open(&sink))) {
                sink->BeginFigure({ax - 3, ay - 5}, D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddLine({ax + 1.5f, ay});
                sink->AddLine({ax - 3, ay + 5});
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                ComPtr<ID2D1StrokeStyle> round;
                r.d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                     D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                                         nullptr, 0, &round);
                rt->DrawGeometry(g.Get(), fill(alpha(hot || open ? g_pal.accent : g_pal.dim, a)), 1.4f, round.Get());
            }
            textRight -= kArrowW;
        }
        if (!it.right.empty())
            rt->DrawText(it.right.c_str(), static_cast<UINT32>(it.right.size()), r.right.Get(),
                         {x, top, textRight, bottom}, fill(alpha(g_pal.dim, (hot ? 0.95f : 0.75f) * a)));
        rt->DrawText(it.text.c_str(), static_cast<UINT32>(it.text.size()), (it.bold ? r.bold : r.text).Get(),
                     {x, top - 0.5f, right - kPadR, bottom}, fill(alpha(g_pal.fg, a)));
    }
    if (header) {  // tiny blanket-pink accent bar along the top edge
        rt->FillRectangle({left, card.top, right, card.top + 2.5f}, fill(alpha(g_pal.accent, 0.9f)));
    }
    if (layer && clip) rt->PopLayer();
    // Hairline border on top (inside the card edge).
    const D2D1_ROUNDED_RECT border{{card.left + px / 2, card.top + px / 2, card.right - px / 2, card.bottom - px / 2},
                                   kRadius - px / 2, kRadius - px / 2};
    rt->DrawRoundedRectangle(border, fill(alpha(g_pal.accent, 0.16f)), px);
    if (rt->EndDraw() == D2DERR_RECREATE_TARGET) return;

    POINT src{0, 0};
    SIZE size = lv.winPx;
    POINT dst = lv.winPos;
    const BYTE opacity = static_cast<BYTE>(std::lround(std::clamp(lv.fade, 0.0f, 1.0f) * 255));
    BLENDFUNCTION bf{AC_SRC_OVER, 0, opacity, AC_SRC_ALPHA};
    UpdateLayeredWindow(lv.hwnd, nullptr, &dst, &size, lv.memDC, &src, 0, &bf, ULW_ALPHA);
}

double g_lastOpenMs = 0;
bool g_inMenu = false;

}  // namespace

void setPalette(uint32_t background, uint32_t card, uint32_t accent) {
    Palette p;
    const D2D1_COLOR_F bg = rgb(background), cd = rgb(card), ac = rgb(accent);
    const bool light = luminance(cd) > 0.55f;
    p.cardTop = cd;
    p.cardBottom = mix(cd, bg, light ? 0.35f : 0.45f);
    if (light) p.cardBottom = mix(p.cardBottom, D2D1_COLOR_F{0, 0, 0, 1}, 0.04f);
    else p.cardBottom = mix(p.cardBottom, D2D1_COLOR_F{0, 0, 0, 1}, 0.12f);
    // Text: warm white on dark cards, a dark tint of the accent on light ones.
    p.fg = light ? mix(D2D1_COLOR_F{0.13f, 0.10f, 0.11f, 1}, ac, 0.12f) : mix(D2D1_COLOR_F{1, 0.97f, 0.96f, 1}, ac, 0.06f);
    p.dim = mix(p.fg, ac, 0.30f);
    p.dim = mix(p.dim, cd, 0.18f);
    // On a light card a pale accent would vanish: darken it for strokes / text.
    p.accent = light && luminance(ac) > 0.6f ? mix(ac, D2D1_COLOR_F{0, 0, 0, 1}, 0.25f) : ac;
    p.shadow = mix(D2D1_COLOR_F{0.03f, 0.02f, 0.02f, 1}, bg, 0.08f);
    g_pal = p;
}

Colors currentColors() {
    auto pack = [](D2D1_COLOR_F c) {
        auto b = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
        return (b(c.r) << 16) | (b(c.g) << 8) | b(c.b);
    };
    return {pack(g_pal.cardTop), pack(g_pal.cardBottom), pack(g_pal.fg),     pack(g_pal.dim),
            pack(g_pal.accent),  pack(g_pal.shadow),     luminance(g_pal.cardTop) > 0.55f};
}

void warmUp() {
    res().init();
    registerClass();
}

UINT trackMenu(HWND owner, const std::vector<MenuItem>& items, POINT pt, const MenuOptions& options) {
    if (g_inMenu || items.empty()) return 0;
    const auto t0 = std::chrono::steady_clock::now();
    if (!res().init()) return 0;
    registerClass();
    g_inMenu = true;
    UINT result = 0;
    {
        Tracker t(owner, options);
        result = t.run(items, pt, t0, g_lastOpenMs);
    }
    g_inMenu = false;
    return result;
}

double lastOpenMs() { return g_lastOpenMs; }

bool renderToPng(float widthDip, float heightDip, UINT dpi, const std::wstring& path,
                 const std::function<void(ID2D1RenderTarget*)>& draw) {
    Resources& r = res();
    if (!r.init() || !r.wic) return false;
    const UINT w = static_cast<UINT>(std::lround(widthDip * dpi / 96.0f)), h = static_cast<UINT>(std::lround(heightDip * dpi / 96.0f));
    ComPtr<IWICBitmap> bmp;
    if (FAILED(r.wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) return false;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        static_cast<float>(dpi), static_cast<float>(dpi));
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(r.d2d->CreateWicBitmapRenderTarget(bmp.Get(), props, &rt))) return false;
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    draw(rt.Get());
    if (FAILED(rt->EndDraw())) return false;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(r.wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(r.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr)) || FAILED(frame->WriteSource(bmp.Get(), nullptr)) || FAILED(frame->Commit()) ||
        FAILED(enc->Commit()))
        return false;
    return true;
}

bool renderMenuPng(const std::vector<MenuItem>& items, const std::wstring& path, const MenuOptions& options,
                   int hotRow) {
    if (items.empty() || !res().init()) return false;
    Tracker t(nullptr, options);
    return t.renderPng(items, path, hotRow);
}

}  // namespace pm::ui
