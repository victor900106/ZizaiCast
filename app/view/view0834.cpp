// 自在投影 app: view/view0834.cpp — 0.8.3.4 畫面組（投影畫面視窗的精修）。
//   B2 截圖提示可以點：the screenshot toast opens the picture.
//   B3 翻譯凍結標示：the translation's freeze has its own badge (not 凍結's).
// (B1 toolbar auto-hide and C3 pen-mode frame / badge live in pm_video:
// vw_view0834.cpp, renderer_view0834.cpp, renderer_ink.cpp.)

#include <fstream>

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "view/ink.h"
#include "view/view.h"

namespace pm_app {

namespace {
bool g_devTrFreeze = false;  // --dev DevCommand 8342: act as if a translation froze the picture
std::wstring g_frozenSent = L"-";
}  // namespace

void devFakeTranslateFreeze0834(bool on) {
    g_devTrFreeze = on;
    refreshFrozenBadge0834();
}

// B3: while a translation holds the picture (and 凍結 was not asked for by
// the user) the badge says so: 「翻譯中 · 畫面已暫停」 with the translate
// glyph; else the usual 「畫面已凍結」.  Sent only when it changes.
void refreshFrozenBadge0834() {
    if (!g.window) return;
    // (C3) The pen mode badge follows a language switch made while it is up.
    static std::wstring inkSent;
    if (inkOn()) {
        const std::wstring ink = tr(S::InkBadge0834);
        if (ink != inkSent) g.window->setInkMode(true, inkSent = ink);
    } else {
        inkSent.clear();
    }
    const bool byTranslation = (translating() || g_devTrFreeze) && !g.userFrozen;
    const std::wstring text = byTranslation ? std::wstring(tr(S::FrozenBadgeTr0834)) : std::wstring();
    if (text == g_frozenSent) return;
    g_frozenSent = text;
    g.window->setFrozenBadge(text, byTranslation ? kIcoTranslate : wchar_t(0));
}

// B2: open a capture with its default app (Photos, a player); gone (moved,
// deleted): its folder.  --dev with PM_TEST_OPEN_OUT: only written to that
// file and logged (scripted tests never open anything).
void openCapture0834(const fs::path& file) {
    std::error_code ec;
    const bool there = fs::exists(file, ec);
    wchar_t out[MAX_PATH] = {};
    if (g.dev && GetEnvironmentVariableW(L"PM_TEST_OPEN_OUT", out, MAX_PATH) && out[0]) {
        std::ofstream(fs::path(out), std::ios::app) << toUtf8((there ? L"open " : L"folder ") + file.wstring()) << "\n";
        g.log->write("info", std::string("dev: capture toast click -> ") + (there ? "open " : "folder ") + toUtf8(file.wstring()));
        return;
    }
    g.log->write("info", std::string("capture toast: ") + (there ? "open the file" : "file gone, open the folder"));
    if (there) ShellExecuteW(g.hwnd, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    else openScreenshotDir();
}

// --dev DevCommand hooks (scripted off-screen tests, tools/shots_0834v.mjs):
//   8342 lp=1  freeze the picture as a translation does (not 凍結), lp=0 back
//   8343       log the frozen badge state
bool devCommand0834View(WPARAM wp, LPARAM lp) {
    if (wp == 8342) {
        g.userFrozen = false;
        g.window->setFrozen(lp != 0);
        devFakeTranslateFreeze0834(lp != 0);
        refreshToolbar();
        g.log->write("info", std::string("dev 0.8.3.4: translation freeze ") + (lp ? "on" : "off"));
        return true;
    }
    if (wp == 8343) {
        g.log->write("info", "dev 0.8.3.4: frozen " + std::string(g.window->viewState().frozen ? "yes" : "no") +
                                 ", badge " + (g_frozenSent.empty() ? std::string("(default)") : toUtf8(g_frozenSent)));
        return true;
    }
    return false;
}

void showShotToast0834(const fs::path& file) {
    g.window->showToast(tr(S::ShotSaved), 8000, tr(S::ShotOpen0834), [file]() { openCapture0834(file); });
}

}  // namespace pm_app
