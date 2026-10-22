// 翻譯 ▸ 本機 AI 翻譯… (0.7.4): the settings panel of the optional
// translation engine, on pm::ui::SettingsPanel.
//   本機 AI 翻譯: pm/llm_translate.h (on / off, model, GPU, download with a
//     consent dialog on a worker thread, progress, cancel / continue, remove).
// main.cpp only opens it and forwards theme / language changes.
// UI thread only.
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
    // --dev --test-no-network: the download is a fake (nothing
    // is fetched or sent; PM_TEST_LLM_FAKE=ok|network|verify|disk|unpack and
    // PM_TEST_LLM_FAKE_MS pick the fake download's end and length).
    bool noNetwork = false;
    bool offscreen = false;  // --dev --test-offscreen
};
// Built with pm_translate's optional engines (else every call does nothing).
bool available();
void init(Host host);
// Cancels a running download (waits a moment for it), closes the panels.
// last: the end of wWinMain - also joins every worker still running.
void shutdown(bool last = false);

void openLocalAi();
void retheme();
void relabel();

// Screen translation turned on: local_llm::warmUp() (cheap; nothing when off / not downloaded).
void warmUpLocalAi();
// 鏈 AI 缈昏 on and usable now (installed, enough memory): the menu check mark.
bool localAiOn();

// --dev scripts: PNGs of the open panels and their question dialog
// (llm<suffix>.png, trask<suffix>.png in dir); the number drawn.
// view: only what the windows show now (llm_view; clamped height, scrolled).
int devShots(const std::wstring& dir, const std::wstring& suffix, bool view = false);
void devScroll(int panel, float dips);
void devKey(int panel, UINT vk);  // e.g. VK_TAB: keyboard focus (scrolls into view)
// Panels opened from now on: at this DPI (0 the monitor's), clamped to a work area this high (px, 0 the monitor's).
void devScreen(int dpi, int workAreaPx);
// The open panels as if moved to a monitor at `dpi` (stale: without WM_DPICHANGED).
void devDpiChanged(int dpi, bool stale);
// Logs SettingsPanel::selfCheck() of the open panels (hit where drawn, sizes, DPI).
void devSelfCheck();
void devClick(int panel, int id);  // panel 0 本機 AI 翻譯: as if item `id` was clicked
void devAnswer(int choice);        // the open question dialog: 1 primary, 2 secondary, 0 Esc

}  // namespace pm::ui::trset
