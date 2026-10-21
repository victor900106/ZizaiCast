// 遠端操控 UI (see docs/remote.md):
//  * RemotePanel — themed window like 傳到手機: two tabs 「同一個 Wi-Fi」 /
//    「在外面（Tailscale）」, each with its QR code + URL + 複製連結 (no
//    Tailscale on this PC: a hint and 「怎麼設定 Tailscale」 instead of the QR),
//    a status line (等待手機連線… / 「iPhone · Safari 正在操控」), the safety
//    note, 關閉遠端操控 and 完成 (hides the panel; remote control stays on).
//  * RemoteBanner — while a phone is connected: an always-on-top bar at the
//    top of the screen, 「● 正在被遠端操控 · iPhone · Safari」, 僅觀看 (toggle)
//    and 中斷（Ctrl+D）. Never takes the focus; cannot be closed while a phone
//    is connected (it goes when the session ends).
// Both know nothing about the network: app/remote drives them. UI thread only.
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

// dwExtraInfo of every mouse / key event the phone injects (pm::remote::kInjectedInputTag):
// the PC-only controls below (關閉遠端操控, 中斷, 僅觀看) ignore such clicks.
inline constexpr ULONG_PTR kRemoteInputTag = 0x504D5254;
inline bool fromRemoteInput() { return GetMessageExtraInfo() == static_cast<LPARAM>(kRemoteInputTag); }

class RemotePanel {
public:
    struct Callbacks {
        std::function<void()> onStop;              // 關閉遠端操控 (the panel is already gone)
        std::function<void()> onHide;              // 完成 / × / Esc (remote control stays on)
        std::function<void(const std::string&)> onCopy;  // 複製連結 (the URL of the tab shown)
        std::function<void()> onTailscaleHelp;     // 怎麼設定 Tailscale
        std::function<void()> onTunnelStart;       // 免安裝外網 ▸ 取得外網連結 / 再試一次 (0.8.1)
        std::function<void()> onTunnelStop;        // 免安裝外網 ▸ 停止外網連結
        std::function<void()> onRemoteRefused;     // the phone clicked 關閉遠端操控 (ignored)
        std::function<void()> onFixedStart;        // 固定網址 ▸ 設定密碼並開啟 / 開啟常駐外網 / 再試一次 (0.8.2)
        std::function<void()> onFixedStop;         // 固定網址 ▸ 關閉常駐外網
        std::function<void()> onFixedPassword;     // 固定網址 ▸ 改密碼
    };
    bool open(HWND owner, Callbacks cb);  // centred over owner (or the primary screen); an open panel comes forward
    void close();                          // no callback
    bool isOpen() const { return hwnd_ != nullptr; }
    HWND hwnd() const { return hwnd_; }
    // QR codes: BGRA, 1 px per module + quiet zone. tsUrl empty = no Tailscale on this PC.
    void setUrls(const std::string& lanUrl, const std::vector<uint8_t>& lanQr, int lanQrSize, const std::string& tsUrl,
                 const std::vector<uint8_t>& tsQr, int tsQrSize);
    void setStatus(const std::wstring& text, bool active);  // active: shown in the accent colour
    // 免安裝外網 (0.8.1): state 0 off, 1 getting the address, 2 ready (link = the one-time
    // link, its QR, the 通行碼), 3 failed (err).
    void setTunnel(int state, const std::string& link, const std::vector<uint8_t>& qr, int qrSize, const std::string& code,
                   const std::wstring& err);
    // 常駐外網 (0.8.2): state 0 off, 1 starting, 2 ready (url = the fixed address, its QR;
    // info: why a temporary cloudflared address is used instead), 3 failed (err).
    // hasPassword: the off state's button says 開啟 instead of 設定密碼並開啟.
    void setFixed(int state, const std::string& url, const std::vector<uint8_t>& qr, int qrSize, const std::wstring& info,
                  const std::wstring& err, bool hasPassword);
    void showTab(int tab);  // 0 同一個 Wi-Fi, 1 Tailscale, 2 免安裝外網, 3 固定網址
    int tab() const { return tab_; }
    void retheme();
    void relabel();
    bool renderPng(const std::wstring& path);  // --dev test hook
    // --dev test hook: a click on 關閉遠端操控, with the phone's input tag (fromPhone) or a plain one
    // (SetMessageExtraInfo + SendMessage: the same GetMessageExtraInfo() check as a real click).
    void testClickStop(bool fromPhone);
    static inline bool testOffscreen = false;
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

private:
    enum Hit {
        HitNone = 0, HitClose, HitPrimary, HitSecondary, HitCopy, HitTabLan, HitTabTs, HitTsHelp, HitTabTun, HitTunStart, HitTunStop,
        HitTabFix, HitFixStart, HitFixStop, HitFixPw
    };
    void paint();
    void draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* b, ID2D1Bitmap* qr);
    Hit hitTest(POINT pt) const;
    void click(Hit h);
    void destroyTarget();
    float s() const { return dpi_ / 96.0f; }
    const std::vector<uint8_t>& qr() const { return tab_ == 3 ? fixQr_ : tab_ == 2 ? tunQr_ : tab_ ? tsQr_ : lanQr_; }
    int qrSize() const { return tab_ == 3 ? fixQrSize_ : tab_ == 2 ? tunQrSize_ : tab_ ? tsQrSize_ : lanQrSize_; }

    HWND hwnd_ = nullptr, owner_ = nullptr;
    Callbacks cb_;
    UINT dpi_ = 96;
    int tab_ = 0;
    std::string lanUrl_, tsUrl_;
    std::vector<uint8_t> lanQr_, tsQr_;
    int lanQrSize_ = 0, tsQrSize_ = 0;
    std::wstring status_;
    bool active_ = false;
    Hit hot_ = HitNone, pressed_ = HitNone;
    bool tracking_ = false;
    float copyW_ = 80, helpW_ = 120, stopW_ = 120;
    int tunState_ = 0;
    std::string tunLink_, tunCode_;
    std::vector<uint8_t> tunQr_;
    int tunQrSize_ = 0;
    std::wstring tunErr_;
    int fixState_ = 0;
    bool fixHasPw_ = false;
    std::string fixUrl_;
    std::vector<uint8_t> fixQr_;
    int fixQrSize_ = 0;
    std::wstring fixInfo_, fixErr_;
    float fixStopW_ = 120, fixPwW_ = 80;
    struct Impl;
    Impl* impl_ = nullptr;
};

class RemoteBanner {
public:
    struct Callbacks {
        std::function<void()> onStop;           // 中斷
        std::function<void(bool)> onViewOnly;   // 僅觀看 toggled
        std::function<void()> onRemoteRefused;  // the phone clicked 中斷 / 僅觀看 (ignored)
    };
    // Shows (or updates) the bar at the top centre of the primary screen's work area.
    void show(const std::wstring& device, bool viewOnly, Callbacks cb);
    void setViewOnly(bool on);
    void hide();
    bool visible() const { return hwnd_ != nullptr; }
    void retheme();
    void relabel();
    bool renderPng(const std::wstring& path);  // --dev test hook
    void testDevice(const std::wstring& device, UINT dpi) { device_ = device, dpi_ = dpi; }  // --dev screenshots (not shown)
    // --dev test hook: a click on 1 僅觀看 / 2 中斷, with the phone's input tag (fromPhone) or a plain one.
    void testClick(int which, bool fromPhone);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    HWND hwnd() const { return hwnd_; }
    // 0.8.3.1: --dev --test-offscreen: the bar goes left of every monitor (the remote tests had
    // it at the top of the real screen).
    static inline bool testOffscreen = false;

private:
    void paint();
    void layout();
    void draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* b, float w, float h);
    int hitAt(int xPx) const;  // 1 僅觀看, 2 中斷, 0 none
    float textW_ = 200, viewW_ = 80, stopW_ = 100;
    HWND hwnd_ = nullptr;
    Callbacks cb_;
    std::wstring device_;
    bool viewOnly_ = false;
    UINT dpi_ = 96;
    int hot_ = 0;
    bool tracking_ = false;
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace pm::ui
