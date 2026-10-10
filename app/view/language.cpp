// 自在投影 app: view/language.cpp — view（視窗動作、檢視選項）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "help/help.h"
#include "share/share.h"
#include "capture/capture.h"
#include "translate/translate.h"
#include "connect/connect.h"
#include "view/view.h"

namespace pm_app {

// 設定 → 語言 / Language: re-labels everything at once (menus are built on
// open, the toolbar / idle screen / panels are refreshed here); the AirPlay
// and Miracast names are re-advertised only if the display name changed
// (deferred while a phone uses that receiver).
void setLanguage(int pref) {
    if (pref < 0 || pref > 4) return;
    g.settings.language = pref;
    saveSettings();
    const pm::i18n::Lang lang = resolveLanguage(pref);
    const bool changed = lang != pm::i18n::lang();
    pm::i18n::setLang(lang);
    g.log->write("info", std::string("language ") + Settings::languageKey(pref) + " -> " +
                             pm::i18n::langKey(lang) + (changed ? "" : " (unchanged)"));
    if (changed) {
        if (g.liveSource != SrcNone && !g.sourceName.empty()) setPeer(g.liveSource, g.sourceName);
        const std::wstring newName = displayNameFor();
        const bool rename = newName != g.name;
        g.name = newName;
        refreshTitles();
        SetWindowTextW(g.hwnd, currentTitle().c_str());
        trayRetip();
        refreshIdleOptions();
        refreshIdleHints();
        refreshToolbar();
        g.pairPanel.relabel();
        g.aboutPanel.relabel();
        g.updatePanel.relabel();
        g.sharePanel.relabel();
        g.sharePicker.relabel();
        pm::ui::trset::relabel();
        if (g.keysPanel.isOpen()) {
            g.keysPanel.relabel();
            openShortcuts();  // new texts
        }
        if (g.translator && g.settings.translateTo == 0) g.translator->setTarget(translateTarget());  // 翻成 follows the UI
        if (rename) {
            g.log->write("info", "display name -> \"" + toUtf8(newName) + "\"");
            bool deferred = false;
            if (g.sessionActive) {
                g.restartPending = true;
                deferred = true;
            } else {
                restartServer();
            }
            if (g.miracast && g.settings.miracast && !g.miracastUnsupported) {
                if (g_active.load() == SrcMiracast) {
                    g.miracastRenamePending = true;
                    deferred = true;
                } else {
                    miracastApply(false);
                    miracastApply(true);
                }
            }
            if (deferred) {
                showDeferred(fmt(S::NameChangeDeferred, {newName}));
                return;
            }
        }
    }
    const S labels[5] = {S::LangAuto, S::LangZh, S::LangEn, S::LangJa, S::LangKo};
    const S label = labels[pref];
    g.window->showToast(fmt(S::LangToast, {tr(label)}));
}

}  // namespace pm_app
