// 「關於自在投影 / About Zizai Cast」: a small themed window (same look as the
// pairing panel) with the app icon, version, licence (GPL-3.0), a link to the
// GitHub repository, a link that opens the licences folder, and the credits
// of the open-source parts. Texts come from pm/i18n_strings.inc. UI thread only.
#pragma once
#include <windows.h>

#include <wrl/client.h>

#include <functional>
#include <string>

struct ID2D1RenderTarget;
struct ID2D1SolidColorBrush;
struct ID2D1Bitmap;

namespace pm::ui {

class AboutPanel {
public:
    struct Info {
        std::wstring version;                        // e.g. L"0.6.0" (or the dev-build label)
        std::wstring repoUrl;                        // https://github.com/…
        HICON icon = nullptr;                        // app icon (large), not owned
        std::function<void()> onOpenLicenses;        // 「開啟授權資訊資料夾」
        std::function<void(const std::wstring&)> onOpenUrl;
    };

    // Shows the panel centred over `owner` (or brings an open one forward).
    bool open(HWND owner, Info info);
    void close();
    bool isOpen() const { return hwnd_ != nullptr; }
    HWND hwnd() const { return hwnd_; }
    void retheme();  // palette changed
    void relabel();  // language changed
    // --dev test hook: draws the open panel into a PNG (false if not open).
    bool renderPng(const std::wstring& path);

    // --dev --test-offscreen: centred over the owner even off the desktop,
    // shown without activation (scripted screenshots).
    static inline bool testOffscreen = false;

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

private:
    enum Hit { HitNone = 0, HitClose, HitRepo, HitLicenses, HitButton };
    void paint();
    void draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, ID2D1Bitmap* icon);
    Microsoft::WRL::ComPtr<ID2D1Bitmap> iconBitmap(ID2D1RenderTarget* rt);
    Hit hitTest(POINT pt) const;  // client pixels
    void click(Hit h);
    void destroyTarget();
    float s() const { return dpi_ / 96.0f; }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Info info_;
    UINT dpi_ = 96;
    Hit hot_ = HitNone, pressed_ = HitNone;
    bool tracking_ = false;
    float repoW_ = 200, licW_ = 160;  // measured link widths (DIPs)
    struct Impl;
    Impl* impl_ = nullptr;  // Direct2D objects
};

}  // namespace pm::ui
