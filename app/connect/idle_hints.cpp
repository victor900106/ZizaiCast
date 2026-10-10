// 自在投影 app: connect/idle_hints.cpp — connect（iPhone / Android / Miracast 連線狀態）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "help/help.h"
#include "capture/capture.h"
#include "connect/connect.h"
#include "connect/connect_internal.h"

namespace pm_app {

void refreshIdleHints() {
    // 0.7.8: 「請選你的手機」 and three cards (iPhone / iPad · Android 只要看 ·
    // Android 用電腦操控), each with one button.  A way that cannot be used
    // right now stays listed, greyed, with the reason and the next step as
    // its button: 開啟 (switched off here), 怎麼安裝 (Wireless Display),
    // 下載安裝檔 (adb tools missing: a reinstall fixes it), else 了解原因; when
    // the PC itself cannot receive casting, the note points to 用電腦操控.
    using IC = pm::VideoWindow::IdleCard;
    using IA = pm::VideoWindow::IdleAction;
    const bool andOk = g.testWays >= 0 ? (g.testWays & 1) != 0 : g.androidOk;
    const MiraWhy why = miracastWhy();
    std::wstring miraNote;
    switch (why) {
    case MiraOn: break;
    case MiraOff: miraNote = tr(S::CardCastOff); break;
    case MiraReboot: miraNote = tr(S::CardCastReboot); break;
    case MiraFeature: miraNote = tr(S::CardCastFeature); break;
    case MiraOldWindows: miraNote = tr(S::CardCastOldWindows); break;
    case MiraPolicy: miraNote = tr(S::CardCastPolicy); break;
    case MiraNoWifi: miraNote = tr(S::CardCastNoWifi); break;
    case MiraOther: miraNote = tr(S::CardCastOther); break;
    }
    // Cannot be fixed with a click or a restart: the other Android way.
    if (andOk && (why == MiraOldWindows || why == MiraPolicy || why == MiraNoWifi || why == MiraOther))
        miraNote += L"\n" + std::wstring(tr(S::CardCastAlt));
    std::vector<IC> cards;
    cards.push_back(IC{tr(S::CardIphoneTitle), fmt(S::CardIphoneBody, {g.name}), L"", false});
    cards.push_back(IC{tr(S::CardCastTitle), fmt(S::CardCastBody, {g.name}), miraNote, why != MiraOn});
    cards.push_back(IC{tr(S::CardAdbTitle), tr(S::CardAdbBody), andOk ? L"" : tr(S::CardAdbMissing), !andOk});
    g.window->setIdleCards(std::move(cards));
    // Actions: each card's button first (the video window draws actions on
    // the idle screen only, never while a phone is live), then 更新 and the
    // tutorial in the row under the cards.
    std::vector<IA> actions;
    actions.push_back({tr(S::CardSteps), true, []() { openTutorial(L"iphone"); }, 0});
    switch (why) {
    case MiraOn: actions.push_back({tr(S::CardSteps), true, []() { openTutorial(L"miracast"); }, 1}); break;
    case MiraOff: {
        const HWND h = g.hwnd;  // posted: the window is inside its own click handling
        actions.push_back({tr(S::CardTurnOn), true, [h]() { PostMessageW(h, WM_PM_TOOL, CmdMiracast, 0); }, 1});
        break;
    }
    case MiraFeature: actions.push_back({tr(S::CardHowInstall), false, []() { openTutorial(L"faq-miracast"); }, 1}); break;
    default: actions.push_back({tr(S::CardWhy), false, []() { openTutorial(L"faq-miracast"); }, 1}); break;
    }
    if (andOk) actions.push_back({tr(S::CardShowQr), true, []() { openPairPanel(); }, 2});
    else actions.push_back({tr(S::CardReinstall), false, []() { openDownloadPage(); }, 2});
    if (const std::string ver = offerVersion(); !ver.empty()) {
        const HWND h = g.hwnd;  // posted: the window is inside its own click handling
        actions.push_back({fmt(S::MenuUpdateTo, {toWide(ver)}), true,
                           [h]() { PostMessageW(h, WM_PM_TOOL, CmdUpdateDialog, 0); }});
    }
    actions.push_back({tr(S::ActHowTo), false, []() { openTutorial(nullptr); }});
    g.window->setIdleActions(std::move(actions));
}

}  // namespace pm_app
