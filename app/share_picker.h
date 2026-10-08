// 傳到手機 batch picker (docs/share.md): this session's screenshots and
// recordings as thumbnails with check boxes (the unsent ones ticked), 全選 /
// 全不選, 取消 and 「傳送 N 個」. Built like the other themed panels (owned
// WS_POPUP, Direct2D in the menus' palette, rounded corners, per-monitor DPI,
// draggable by its top). Thumbnails are made on a worker thread (WIC for
// pictures, the shell's thumbnail for videos), so opening it never waits on
// a decoder. Not modal. UI thread only.
#pragma once
#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ID2D1RenderTarget;
struct ID2D1SolidColorBrush;

namespace pm::ui {

class SharePicker {
public:
    struct Item {
        std::wstring path;
        bool video = false;
        bool sent = false;     // 「已傳」 badge
        bool checked = false;  // pre-ticked (the unsent ones)
        std::wstring label;    // under the thumbnail, e.g. 「14:03:12」
    };
    // onSend gets the ticked paths (in the items' order) after the picker has closed.
    bool open(HWND owner, std::vector<Item> items, std::function<void(std::vector<std::wstring>)> onSend);
    void close();  // cancel
    bool isOpen() const { return hwnd_ != nullptr; }
    HWND hwnd() const { return hwnd_; }
    void retheme();
    void relabel();
    // --dev test hooks
    bool renderPng(const std::wstring& path);
    int thumbsPending() const;  // thumbnails still being made
    void toggle(int index);
    void setAll(bool on);
    void send();                // as if 傳送 N 個 was clicked
    int checkedCount() const;
    static inline bool testOffscreen = false;
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    ~SharePicker();

private:
    enum Hit { HitNone = 0, HitClose, HitAll, HitCancel, HitSend, HitCell };
    struct HitAt {
        Hit hit = HitNone;
        int cell = -1;
        bool operator==(const HitAt& o) const { return hit == o.hit && cell == o.cell; }
    };
    void layout();
    void paint();
    void draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* b, bool offscreen);
    HitAt hitTest(POINT pt) const;
    void click(HitAt h);
    void destroyTarget();
    void takeThumbs();
    float maxScroll() const;
    float s() const { return dpi_ / 96.0f; }

    HWND hwnd_ = nullptr, owner_ = nullptr;
    std::vector<Item> items_;
    std::function<void(std::vector<std::wstring>)> onSend_;
    UINT dpi_ = 96;
    HitAt hot_, pressed_;
    bool tracking_ = false;
    float h_ = 400, gridH_ = 0, footY_ = 0, scroll_ = 0, contentH_ = 0;
    float allW_ = 90, cancelW_ = 90, sendW_ = 120;
    int rows_ = 1;
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace pm::ui
