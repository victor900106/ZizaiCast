// 傳到手機 UI (see docs/share.md):
//  * SharePanel — themed window like the pairing panel: 「用手機相機掃描」, the
//    QR code of the share URL on a white card, the URL + 複製連結, a
//    countdown 「9:58 後失效」, a status line (「手機已開啟頁面（IP）」) and
//    停止分享 (closing = stop). Expired: 連結已失效 + 再分享 10 分鐘 / 關閉.
//    It knows nothing about HTTP: main.cpp drives it from pm::share::Server.
//  * ShareChip — a small 「⇪ 傳到手機」 button that appears above the toast
//    after a screenshot / recording (the video window's toasts have no
//    actions), for ~8 s; never takes the focus.
// UI thread only.
#pragma once
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct ID2D1RenderTarget;
struct ID2D1SolidColorBrush;
struct ID2D1Bitmap;

namespace pm::ui {

class SharePanel {
public:
    struct Callbacks {
        std::function<void()> onStop;   // 停止分享 / × / Esc / 關閉 (the panel is already gone)
        std::function<void()> onAgain;  // 再分享 10 分鐘 (after expiry)
        std::function<void()> onCopy;   // 複製連結
        std::function<void()> onHide;   // live: × / Esc only hide the panel (the share goes on); unset → onStop
    };
    bool open(HWND owner, Callbacks cb);  // centred over owner; an open panel is brought forward
    void close();                          // no callback
    bool isOpen() const { return hwnd_ != nullptr; }
    HWND hwnd() const { return hwnd_; }
    // A (new) share: URL, its QR (BGRA, 1 px per module + quiet zone),
    // 「name.png」 / 「3 個檔案」, seconds until it expires.
    void setShare(const std::string& url, const std::vector<uint8_t>& qrBgra, int qrSize, const std::wstring& files,
                  int seconds);
    void setExpired();
    bool expired() const { return expired_; }
    // 自動傳到手機 (live share): 「自動傳送中 · 已傳 N 個」 instead of the
    // countdown, the keep-the-page-open hint, 停止自動傳送.
    void setLive(bool live, int sent);
    bool live() const { return live_; }
    void setStatus(const std::wstring& text);
    void setFiles(const std::wstring& files);  // the name / 「N 個檔案」 line
    void retheme();
    void relabel();
    bool renderPng(const std::wstring& path);  // --dev test hook
    static inline bool testOffscreen = false;
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

private:
    enum Hit { HitNone = 0, HitClose, HitPrimary, HitSecondary, HitCopy };
    void paint();
    void draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* b, ID2D1Bitmap* qr);
    Hit hitTest(POINT pt) const;
    void click(Hit h);
    void destroyTarget();
    std::wstring countdownText() const;
    float s() const { return dpi_ / 96.0f; }

    HWND hwnd_ = nullptr, owner_ = nullptr;
    Callbacks cb_;
    UINT dpi_ = 96;
    std::wstring url_, files_, status_;
    std::vector<uint8_t> qr_;
    int qrSize_ = 0;
    unsigned long long deadline_ = 0;  // GetTickCount64
    bool expired_ = false;
    bool live_ = false;
    int liveSent_ = 0;
    Hit hot_ = HitNone, pressed_ = HitNone;
    bool tracking_ = false;
    float copyW_ = 80;
    struct Impl;
    Impl* impl_ = nullptr;
};

class ShareChip {
public:
    // One clickable part of the chip: glyph (Segoe Fluent Icons) + label.
    struct Action {
        wchar_t glyph = 0;
        std::wstring label;
        std::function<void()> onClick;
    };
    // Shows (or refreshes) the chip over the bottom of `owner`'s client area
    // for `ms`; onClick runs on a click (the chip hides first).
    void show(HWND owner, std::function<void()> onClick, int ms = 8000);
    // 0.7.8: several actions side by side (截圖已儲存 → 開啟資料夾 · 傳到手機),
    // then the ×.
    void show(HWND owner, std::vector<Action> actions, int ms = 8000);
    void hide();
    bool visible() const { return hwnd_ != nullptr; }
    void reposition();  // owner moved / resized
    void retheme();
    bool renderPng(const std::wstring& path);  // --dev test hook
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    HWND hwnd() const { return hwnd_; }

private:
    void paint();
    void draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* b, float w, float h, int hot);
    float widthDip() const;
    float actionW(size_t i) const;  // DIPs of action i
    int hitAt(int xPx) const;       // 1..n an action, n + 1 the ×
    HWND hwnd_ = nullptr, owner_ = nullptr;
    std::vector<Action> actions_;
    UINT dpi_ = 96;
    int hot_ = 0;  // 0 none, 1..n an action, n + 1 ×
    bool tracking_ = false;
    int ms_ = 8000;
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace pm::ui
