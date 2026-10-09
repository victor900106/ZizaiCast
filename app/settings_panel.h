// Small themed settings panel (0.7.4: 翻譯 ▸ 本機 AI 翻譯… / 線上翻譯（選用）…):
// a list of rows the owner declares (text, notes, a switch, radio options,
// buttons, links, a progress bar, edit boxes) and rebuilds with setItems()
// whenever its state changes. Built like PairPanel: owned WS_POPUP, Direct2D
// in the popup menus' palette, rounded corners, draggable by its top,
// per-monitor DPI, hover / press motion (ui_anim.h), real EDIT children for
// typed text. The content scrolls (wheel) when it is taller than the screen;
// footer buttons stay at the bottom. UI thread only.
#pragma once
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

struct ID2D1RenderTarget;
struct ID2D1SolidColorBrush;

namespace pm::ui {

class SettingsPanel {
public:
    struct Button {
        int id = 0;
        std::wstring text;
        bool primary = false, danger = false, enabled = true;
    };
    struct Item {
        enum class Kind {
            Heading,   // bold line
            Text,      // body text (wraps)
            Note,      // smaller, dimmed text
            Status,    // semi-bold line in a tone (ok / warning)
            Code,      // small monospace text in a well (URLs, technical detail; not translated)
            Toggle,    // label (+ sub) and a switch; click: onAction(id)
            Radio,     // one option of a group (the owner keeps one `on`); click: onAction(id)
            Buttons,   // buttons in a row (footer: pinned to the bottom, right-aligned)
            Links,     // underlined links in a row
            Progress,  // bar, value 0..1
            Edit,      // one-line edit box (id), sub = cue banner
            Rule,      // hairline separator
            Space,     // a little room
        };
        Kind kind = Kind::Text;
        int id = 0;
        std::wstring text, sub;
        bool on = false;        // Toggle / Radio
        bool enabled = true;
        int tone = 0;           // Status / Text: 0 normal, 1 ok (accent), 2 warning
        float value = 0;        // Progress
        std::vector<Button> buttons;  // Buttons / Links
        bool password = false;  // Edit: masked
        bool footer = false;    // Buttons: pinned to the bottom
        float indent = 0;       // DIPs from the left padding
    };
    struct Callbacks {
        std::function<void(int id)> onAction;      // a switch / option / button / link
        std::function<void(int id)> onEditChange;  // text typed in edit box `id`
        std::function<void()> onClose;             // × / Esc (the panel is gone already)
    };

    // Shows the panel centred over `owner` (off the desktop with
    // testOffscreen when the owner is hidden). An open panel is brought to
    // the front and gets the new items.
    bool open(HWND owner, const std::wstring& title, wchar_t glyph, std::vector<Item> items, Callbacks cb);
    void close();  // no onClose
    bool isOpen() const { return hwnd_ != nullptr; }
    HWND hwnd() const { return hwnd_; }
    // New content (edit boxes with the same id keep their text and focus).
    void setItems(std::vector<Item> items);
    void setTitle(const std::wstring& title);
    std::wstring editText(int id) const;
    void setEditText(int id, const std::wstring& text);
    // Overwrites the edit box's buffer and empties it (keys).
    void wipeEdit(int id);
    // As if clicked (--dev scripts).
    void click(int id) {
        if (cb_.onAction) cb_.onAction(id);
    }
    void retheme();
    // Language changed: fonts again (the owner sets new items / title).
    void relabel();
    // --dev test hook: the whole content (unscrolled) as a PNG.
    // view: only what the window shows (its clamped height, scrolled).
    bool renderPng(const std::wstring& path, bool view = false);
    // --dev scripts: scroll the content; a key as if pressed in the panel.
    void devScroll(float dips) { scrollBy(dips); }
    void devKey(UINT vk) {
        if (hwnd_) SendMessageW(hwnd_, WM_KEYDOWN, vk, 0);
    }
    float height() const { return h_; }        // window height (DIPs)
    // --dev: as if the window moved to a monitor at `dpi` (testDpi + a
    // WM_DPICHANGED with the scaled rectangle, like Windows sends); stale: the
    // DPI changes without the message (as if it was missed).
    void devDpiChanged(int dpi, bool stale = false);
    // --dev: paints, then checks that every drawn control is hit where it is
    // drawn (centres through the drawing transform and the render target's
    // DPI -> hitAt), and that window, client, render target and layout agree.
    // "ok ..." or "FAIL ..." (one line for the log).
    std::string selfCheck();
    float contentHeight() const { return contentH_; }
    UINT dpi() const { return dpi_; }
    static inline bool testOffscreen = false;
    // --dev: panels open at this DPI (0: the monitor's) and clamp to a work
    // area this many pixels high (0: the monitor's).
    static inline int testDpi = 0, testWorkAreaPx = 0;
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

private:
    struct Hit {
        float l = 0, t = 0, r = 0, b = 0;  // content DIPs (scrolled area) or window DIPs (fixed)
        int id = 0;
        bool enabled = true, fixed = false;
    };
    void layout();       // measure the items, size the window
    void layoutFixedHits();  // footer buttons and the close button, from h_
    void fitClient();    // window DPI / client size -> dpi_, h_, render target
    UINT windowDpi() const { return testDpi > 0 ? static_cast<UINT>(testDpi) : hwnd_ ? GetDpiForWindow(hwnd_) : dpi_; }
    void placeEdits();   // EDIT children at their rows (scrolled)
    void syncEdits();    // create / remove EDIT children for the Edit items
    void paint();
    void draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* b, bool png);
    int hitAt(POINT pt) const;  // index into hits_ + 1, 0 none
    void activate(int hit);
    void scrollBy(float dips);
    void destroyTarget();
    void applyEditFonts();
    float s() const { return dpi_ / 96.0f; }
    float viewTop() const;
    float viewBottom() const;

    HWND hwnd_ = nullptr, owner_ = nullptr;
    std::wstring title_;
    wchar_t glyph_ = 0;
    std::vector<Item> items_;
    Callbacks cb_;
    UINT dpi_ = 96;
    struct EditBox {
        int id = 0;
        HWND h = nullptr;
        bool password = false;
    };
    std::vector<EditBox> edits_;
    HFONT editFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    std::vector<Hit> hits_;
    std::vector<float> itemY_, itemH_;  // per item (content DIPs)
    float contentH_ = 0, footerH_ = 0, h_ = 400, scroll_ = 0;
    int hot_ = 0, pressed_ = 0, focus_ = 0;
    bool inLayout_ = false;  // our own SetWindowPos (WM_SIZE: no fitClient)
    struct Drawn {
        int id = 0;
        POINT px{};  // centre in client pixels, as drawn
    };
    std::vector<Drawn> drawn_;  // the last paint's controls (selfCheck)
    bool focusCues_ = false, tracking_ = false;
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace pm::ui
