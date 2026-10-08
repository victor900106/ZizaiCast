// 「有新版本 / A new version」 dialog (自動更新, see docs/app.md "Offer UX"):
// a themed window like the About panel with the app icon, 「目前 vA → 新版
// vB」, release date and download size, a scrollable bullet list 「這次更新了
// 什麼」 and three choices: 立即更新 (primary, Enter), 稍後提醒 (Esc, ×) and
// 略過這個版本. It knows nothing about downloads: main.cpp acts on the
// callbacks. Texts come from pm/i18n_strings.inc. UI thread only.
#pragma once
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

struct ID2D1RenderTarget;
struct ID2D1SolidColorBrush;
struct ID2D1Bitmap;

namespace pm::ui {

class UpdatePanel {
public:
    struct Info {
        std::wstring current, version;            // "0.6.2", "0.7.0"
        std::wstring date;                        // "2026-10-08" (empty: not shown)
        unsigned long long size = 0;              // installer bytes (0: not shown)
        bool local = false;                       // from <install>\安裝檔 (size label, generic text)
        std::vector<std::wstring> changesZh, changesEn;  // bullets per language
        std::vector<std::wstring> changesJa, changesKo;  // optional (日本語 / 한국어 UI; else English)
        std::wstring notes;                       // old one-line summary (fallback)
        HICON icon = nullptr;                     // app icon (large), not owned
        // The panel is already closed when these run.
        std::function<void()> onInstall, onLater, onSkip;
    };

    // Shows the panel centred over `owner` (or its monitor when hidden). An
    // open panel takes the new info. `activate`: take the foreground (a user
    // action); false for an automatic prompt while another app is in front.
    bool open(HWND owner, Info info, bool activate);
    void close();  // no callback
    bool isOpen() const { return hwnd_ != nullptr; }
    HWND hwnd() const { return hwnd_; }
    const std::wstring& version() const { return info_.version; }
    void retheme();
    void relabel();  // language changed (the other bullet list, new height)
    // --dev test hook: draws the open panel into a PNG (false if not open).
    bool renderPng(const std::wstring& path);
    static inline bool testOffscreen = false;
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

private:
    enum Hit { HitNone = 0, HitClose, HitInstall, HitLater, HitSkip, HitList };
    void layout();  // text lines, list / panel height, button widths (DIPs)
    void paint();
    void draw(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* b, ID2D1Bitmap* icon);
    Hit hitTest(POINT pt) const;
    void click(Hit h);
    void scrollBy(float dip);
    void destroyTarget();
    float s() const { return dpi_ / 96.0f; }

    HWND hwnd_ = nullptr, owner_ = nullptr;
    Info info_;
    UINT dpi_ = 96;
    Hit hot_ = HitNone, pressed_ = HitNone;
    int focus_ = 0;           // keyboard: 0 立即更新, 1 稍後提醒, 2 略過 (Tab cycles)
    bool focusCues_ = false;  // Tab was used: draw the focus ring
    bool tracking_ = false;
    // layout() results (DIPs)
    std::vector<std::wstring> items_;  // bullets in the UI language (or one plain paragraph)
    bool plain_ = false;               // items_ is a single paragraph, no bullet
    std::vector<float> itemH_;
    float h_ = 480, listY_ = 0, listH_ = 100, contentH_ = 0, scroll_ = 0;
    float footY_ = 0, btnY_ = 0, installW_ = 120, laterW_ = 110, skipW_ = 100;
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace pm::ui
