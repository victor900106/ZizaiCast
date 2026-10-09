// 翻譯 ▸ 本機 AI 翻譯… and 翻譯 ▸ 線上翻譯（選用）… (0.7.4): the settings
// panels of the optional translation engines, on pm::ui::SettingsPanel.
//   本機 AI 翻譯: pm/llm_translate.h (on / off, model, GPU, download with a
//     consent dialog on a worker thread, progress, cancel / continue, remove).
//   線上翻譯: pm/online_translate.h, launch/_work/tr_arch/ONLINE_UI_SPEC.md
//     (on / off, provider, mode, key, Azure region, 測試連線, consent per
//     provider, privacy note, how to get a key, back-off retry).
// main.cpp only opens them, forwards theme / language changes and asks
// for the online toast after a translation. UI thread only.
#pragma once
#include <windows.h>

#include <functional>
#include <string>

namespace pm::ui::trset {

struct Host {
    HWND owner = nullptr;  // main window: the panels are centred over it
    std::function<void(const std::wstring& text)> toast;
    std::function<void(const char* level, const std::string& text)> log;
    std::function<void(const std::wstring& url)> openUrl;
    std::function<void()> onlineChanged;  // online settings saved / removed (setOnlineAllowed, menus)
    // --dev --test-no-network: the download and the key test are fakes (nothing
    // is fetched or sent; PM_TEST_LLM_FAKE=ok|network|verify|disk|unpack and
    // PM_TEST_LLM_FAKE_MS pick the fake download's end and length).
    bool noNetwork = false;
    bool offscreen = false;  // --dev --test-offscreen
};
// Built with pm_translate's optional engines (else every call does nothing).
bool available();
void init(Host host);
// Cancels a running download (waits a moment for it), closes the panels.
void shutdown();

void openLocalAi();
void openOnline();
void retheme();
void relabel();

// Screen translation turned on: local_llm::warmUp() (cheap; nothing when off / not downloaded).
void warmUpLocalAi();
// 鏈 AI 缈昏 on and usable now (installed, enough memory): the menu check mark.
bool localAiOn();
// The online switch as saved (menu check mark, ScreenTranslator::setOnlineAllowed).
bool onlineEnabled();
// After a translation: the toast for a new online failure (「線上翻譯額度已用完…」), "" if none.
std::wstring onlineToast();

// --dev scripts: PNGs of the open panels and their question dialog
// (llm<suffix>.png, online<suffix>.png, trask<suffix>.png in dir); the number drawn.
// view: only what the windows show now (llm_view / online_view; clamped height, scrolled).
int devShots(const std::wstring& dir, const std::wstring& suffix, bool view = false);
void devScroll(int panel, float dips);
void devKey(int panel, UINT vk);  // e.g. VK_TAB: keyboard focus (scrolls into view)
// Panels opened from now on: at this DPI (0 the monitor's), clamped to a work area this high (px, 0 the monitor's).
void devScreen(int dpi, int workAreaPx);
// The open panels as if moved to a monitor at `dpi` (stale: without WM_DPICHANGED).
void devDpiChanged(int dpi, bool stale);
// Logs SettingsPanel::selfCheck() of the open panels (hit where drawn, sizes, DPI).
void devSelfCheck();
void devClick(int panel, int id);  // panel 0 本機 AI 翻譯, 1 線上翻譯: as if item `id` was clicked
void devAnswer(int choice);        // the open question dialog: 1 primary, 2 secondary, 0 Esc
void devSetKey(const std::wstring& key);  // types into the online panel's key box

}  // namespace pm::ui::trset
