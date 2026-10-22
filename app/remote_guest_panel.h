// 0.8.3.2 允許遠端控制 ▸ 新增臨時帳號: a small themed popup like 「設定密碼」
// (remote_password_panel): the account name (a real EDIT, filled in with guest-NNNN),
// 有效時間 (用一次 / 1 小時 / 24 小時 / 7 天), 權限 (可操作 / 僅觀看), 允許傳檔 (off), then
// 建立. The app makes the account (only the password's hash is kept) and the panel shows
// 網址 + 帳號 + 密碼 once, with 複製全部. showResult() opens it on that page directly
// (重設密碼). UI thread only. The password is never logged; the panel forgets it on close.
#pragma once
#include <windows.h>

#include <functional>
#include <string>

struct ID2D1RenderTarget;
struct ID2D1SolidColorBrush;

namespace pm::ui {

class RemoteGuestPanel {
public:
    struct Request {
        std::wstring name;
        int life = 0;  // 0 用一次, 1 1 hour, 2 24 hours, 3 7 days
        bool viewOnly = false, files = true;  // 0.8.3.5: 可操作 = 可以傳檔 by default (guest_rights.h)
    };
    struct Result {
        std::wstring url, name, password, summary;  // summary: e.g. 「24 小時 · 可操作 · 不能傳檔」
        bool reset = false;                          // 已重設密碼 (else 已建立)
    };
    struct Callbacks {
        // false: not made (err says why, shown in the panel); true: res is shown.
        std::function<bool(const Request&, Result& res, std::wstring& err)> onCreate;
        std::function<void(const std::wstring& all)> onCopy;  // 複製全部 (the text to put on the clipboard)
        std::function<void()> onClosed;
    };
    bool open(HWND owner, const std::wstring& suggestedName, Callbacks cb);
    bool showResult(HWND owner, const Result& res, Callbacks cb);  // 重設密碼: straight to the result page
    void close();  // no callback
    bool isOpen() const { return hwnd_ != nullptr; }
    HWND hwnd() const { return hwnd_; }
    void retheme();
    void relabel();
    bool renderPng(const std::wstring& path, UINT dpi = 0);  // --dev test hook (0 = the window's DPI)
    // --dev test hooks: as if clicked / typed (no real input).
    void testSet(const std::wstring& name, int life, bool viewOnly, bool files, bool create);
    void testCopy();
    static inline bool testOffscreen = false;
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    static std::wstring copyText(const Result& r);  // what 複製全部 puts on the clipboard

private:
    enum Hit { HitNone = 0, HitClose, HitPrimary, HitSecondary, HitLife0, HitLife1, HitLife2, HitLife3, HitRight0, HitRight1, HitFiles };
    bool create(HWND owner, Callbacks cb);
    void layout();
    void paint();
    void draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, bool png);
    Hit hitTest(POINT pt) const;
    void click(Hit h);
    void submit();
    void destroyTarget();
    void applyEditFont();
    void setStatus(const std::wstring& s, bool good = false);
    float s() const { return dpi_ / 96.0f; }

    HWND hwnd_ = nullptr, owner_ = nullptr, edit_ = nullptr;
    HFONT editFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    Callbacks cb_;
    UINT dpi_ = 96;
    bool result_ = false;  // the result page
    Request req_;
    bool filesPref_ = true;  // 0.8.3.5: the 可以傳檔 box while 可操作 (kept across 僅觀看)
    Result res_;
    std::wstring status_;
    bool statusGood_ = false;
    Hit hot_ = HitNone, pressed_ = HitNone;
    bool tracking_ = false;
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace pm::ui
