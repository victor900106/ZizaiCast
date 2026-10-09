// Small themed question / message dialog (0.7): the translation model
// download consent, 「需要加入文字辨識語言」 with 「開啟語言設定」, deleting
// translation models, other notices of pm_translate and 「按 X 時要怎麼做？」
// (two answers + 記住我的選擇 checkbox). Built like the
// 「有新版本」 dialog: owned WS_POPUP, Direct2D in the popup menus' palette,
// rounded corners, draggable by its top, per-monitor DPI. Not modal: the
// answer comes through `done` (exactly once, also when the dialog is closed
// with Esc / × or replaced by another one). UI thread only.
#pragma once
#include <windows.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

struct ID2D1RenderTarget;
struct ID2D1SolidColorBrush;

namespace pm::ui {

class AskPanel {
public:
    struct Info {
        wchar_t glyph = 0xE946;     // Segoe Fluent Icons / MDL2 code point in the accent disc
        std::wstring title;
        std::wstring body;          // wraps; "\n\n" starts a new paragraph
        std::vector<std::pair<std::wstring, std::wstring>> rows;  // label · value (a well under the body)
        std::wstring primary;       // primary button (Enter); required
        std::wstring secondary;     // second button (Esc); empty: none (Esc = close)
        bool danger = false;        // primary in a warm red (delete)
        int secondaryChoice = 0;    // what `done` gets for the secondary button (0: the same as Esc)
        std::wstring check;         // checkbox above the buttons (empty: none), e.g. 記住我的選擇
        bool checked = false;       // its first state; checked() in `done` has the answer
        // 1 = primary, secondaryChoice = secondary, 0 = Esc / × / replaced. The panel is closed already.
        std::function<void(int choice)> done;
    };

    // Shows the dialog centred over `owner` (off the desktop with
    // testOffscreen when the owner is hidden). An open dialog is answered
    // with 0 and replaced.
    bool open(HWND owner, Info info, bool activate = true);
    void close(int choice = 0);  // answers `done` with choice
    // As if clicked (--dev scripts): 1 primary, 2 secondary, 3 the checkbox, else ×.
    void click(int which) { press(which == 1 ? HitPrimary : which == 2 ? HitSecondary : which == 3 ? HitCheck : HitClose); }
    bool isOpen() const { return hwnd_ != nullptr; }
    HWND hwnd() const { return hwnd_; }
    const std::wstring& title() const { return info_.title; }
    bool checked() const { return checked_; }  // the checkbox (Info::check) at the last answer
    void retheme();
    // --dev test hook: draws the open dialog into a PNG (false if not open).
    bool renderPng(const std::wstring& path);
    static inline bool testOffscreen = false;
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

private:
    enum Hit { HitNone = 0, HitClose, HitPrimary, HitSecondary, HitCheck };
    void layout();
    void paint();
    void draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* b);
    Hit hitTest(POINT pt) const;
    void press(Hit hit);  // a button / the checkbox was clicked or chosen with the keyboard
    bool hasCheck() const { return !info_.check.empty(); }
    float primX() const;  // button rectangles (DIPs): side by side, or stacked_ full width
    float primY() const { return btnY_; }
    float secX() const;
    float secY() const { return stacked_ ? btnY_ + kBtnGap + kBtnH_ : btnY_; }
    float btnW(bool primary) const;
    static constexpr float kBtnH_ = 38, kBtnGap = 10;
    void destroyTarget();
    float s() const { return dpi_ / 96.0f; }

    HWND hwnd_ = nullptr, owner_ = nullptr;
    Info info_;
    UINT dpi_ = 96;
    Hit hot_ = HitNone, pressed_ = HitNone;
    int focus_ = 0;  // 0 primary, 1 secondary, 2 checkbox
    bool focusCues_ = false, tracking_ = false;
    bool checked_ = false;
    bool stacked_ = false;  // the buttons do not fit side by side: one above the other, full width
    float h_ = 300, bodyH_ = 0, rowsY_ = 0, rowsH_ = 0, labelW_ = 0, btnY_ = 0, primW_ = 110, secW_ = 100;
    float checkY_ = 0, checkW_ = 0;
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace pm::ui
