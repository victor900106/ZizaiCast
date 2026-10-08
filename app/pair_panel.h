// 「連接 Android」 pairing panel: a small themed window (same palette as the
// popup menus) with the wireless-debugging QR code, a 「用配對碼」 fallback
// (IP:port + 6-digit code), a status line and 取消. It knows nothing about
// adb: main.cpp drives it from pm::AndroidSource. UI thread only.
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

class PairPanel {
public:
    struct Callbacks {
        std::function<void()> onClose;  // 取消 / × / Esc (the panel is already gone)
        std::function<void(const std::wstring& hostPort, const std::wstring& code)> onPairCode;  // 配對
        std::function<void()> onQrMode;  // 改用 QR (back from the code form)
        std::function<void()> onHelp;    // 「怎麼開啟無線偵錯？」 link
    };

    // Shows the panel centred over `owner` (QR mode, "preparing" until
    // setQr). Returns false if the window could not be created. A panel that
    // is already open is just brought to the front.
    bool open(HWND owner, Callbacks cb);
    void close();  // no onClose callback
    bool isOpen() const { return hwnd_ != nullptr; }
    HWND hwnd() const { return hwnd_; }

    // Square QR bitmap, BGRA, size x size pixels (stride size * 4). Drawn with
    // nearest-neighbour scaling at a whole multiple of its size, on a white
    // card with a quiet zone. Empty = "preparing".
    void setQr(const std::vector<uint8_t>& bgra, int size);
    // Status line under the QR / form; error = drawn in a warning tint.
    void setStatus(const std::wstring& text, bool error = false);
    bool statusIsError() const { return statusError_ && !status_.empty(); }
    bool codeMode() const { return codeMode_; }  // 配對碼 form shown (else the QR)
    // Switches to the code form (also what 「用配對碼」 does).
    void showCodeForm();
    // Pre-fills the IP:port box (e.g. from mDNS discovery) if it is empty.
    void suggestHostPort(const std::wstring& hostPort);
    // Re-reads the palette (theme change while open).
    void retheme();
    // Language changed while open: texts, fonts and the window title again.
    void relabel();
    // --dev test hook: draws the open panel into a PNG (false if not open).
    bool renderPng(const std::wstring& path);

    // --dev --test-offscreen: centred over the owner even off the desktop,
    // shown without activation (scripted screenshots next to a user's session).
    static inline bool testOffscreen = false;

    // For the window procedure.
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

private:
    enum Hit { HitNone = 0, HitClose, HitPrimary, HitSecondary, HitLink };
    void layout();
    void paint();
    void draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, ID2D1Bitmap* qr, bool png);
    Hit hitTest(POINT pt) const;  // client pixels
    void click(Hit h);
    void setMode(bool code);
    void submitCode();
    void destroyTarget();
    void applyEditFonts();
    float s() const { return dpi_ / 96.0f; }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HWND editHost_ = nullptr, editCode_ = nullptr;
    HFONT editFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    Callbacks cb_;
    UINT dpi_ = 96;
    bool codeMode_ = false;
    std::vector<uint8_t> qr_;
    int qrSize_ = 0;
    std::wstring status_;
    bool statusError_ = false;
    Hit hot_ = HitNone, pressed_ = HitNone;
    float linkW_ = 120;  // measured width of the help link (DIPs)
    bool tracking_ = false;
    struct Impl;
    Impl* impl_ = nullptr;  // Direct2D objects
};

}  // namespace pm::ui
