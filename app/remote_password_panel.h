// 常駐外網 (0.8.2): the 「設定密碼」 window. Built like the 「連接 Android」
// code form (pair_panel): a small themed popup with two password boxes
// (ES_PASSWORD), a status line, 取消 and 儲存並開啟. It only checks the
// rules (at least 8 characters, both the same) and hands the password over:
// app/remote hashes it (PBKDF2) and keeps only the hash. UI thread only.
#pragma once
#include <windows.h>

#include <functional>
#include <string>

struct ID2D1RenderTarget;
struct ID2D1SolidColorBrush;

namespace pm::ui {

class RemotePasswordPanel {
public:
    struct Callbacks {
        std::function<void(const std::wstring& password)> onSave;  // the panel is already gone
        std::function<void()> onCancel;                            // 取消 / × / Esc
    };
    bool open(HWND owner, Callbacks cb);
    void close();  // no callback
    bool isOpen() const { return hwnd_ != nullptr; }
    HWND hwnd() const { return hwnd_; }
    void retheme();
    void relabel();
    bool renderPng(const std::wstring& path);  // --dev test hook (dots for the typed characters)
    // --dev test hook: types into the boxes and presses 儲存 (no real keyboard).
    void testFill(const std::wstring& a, const std::wstring& b, bool save);
    static inline bool testOffscreen = false;
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

private:
    enum Hit { HitNone = 0, HitClose, HitPrimary, HitSecondary };
    void layout();
    void paint();
    void draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, bool png);
    Hit hitTest(POINT pt) const;
    void click(Hit h);
    void submit();
    void destroyTarget();
    void applyEditFonts();
    void setStatus(const std::wstring& s);
    float s() const { return dpi_ / 96.0f; }

    HWND hwnd_ = nullptr, owner_ = nullptr;
    HWND edit1_ = nullptr, edit2_ = nullptr;
    HFONT editFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    Callbacks cb_;
    UINT dpi_ = 96;
    std::wstring status_;
    Hit hot_ = HitNone, pressed_ = HitNone;
    bool tracking_ = false;
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace pm::ui
