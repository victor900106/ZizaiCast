// 自在投影 app: view/close_flow.cpp — view（視窗動作、檢視選項）。
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

void quitApp() {
    g.quitting = true;
    stopRecording();
    stopSharing();
    g.aboutPanel.close();
    g.updatePanel.close();
    g.askPanel.close();
    g.closePanel.close();
    pm::ui::trset::shutdown();  // cancels a 本機 AI 翻譯 download (the partial file is kept)
    if (g.translator) g.translator->close();
    trayRemove();
    g.window->setLiveToolbar({}, nullptr);
    g.window->close();
}

void hideToTray() {
    if (isFullscreen()) CallWindowProcW(g.prevProc, g.hwnd, WM_KEYDOWN, VK_F11, 0);
    ShowWindow(g.hwnd, SW_HIDE);
    g.log->write("info", "window hidden to the tray (receiving goes on)");
    if (!g.settings.closeHintShown) {  // once: still running bottom-right, click to open, right-click → 結束
        g.settings.closeHintShown = g.settings.trayHintShown = true;
        saveSettings();
        trayBalloon(fmt(S::TrayHintTitle, {tr(S::AppName)}), tr(S::TrayHintText));
    }
}

// × on the caption, Alt+F4, 關閉視窗 on the taskbar (all WM_CLOSE): 按 X 時.
// The tray / menu 結束 always quits (quitApp). Without a tray icon there is
// nowhere to hide: the window closes as before.
void onCloseRequest() {
    if (g.closePanel.isOpen()) {  // asked already: bring the question back up
        if (!g.testOffscreen) SetForegroundWindow(g.closePanel.hwnd());
        return;
    }
    if (!IsWindowVisible(g.hwnd)) return;  // hidden already (stray WM_CLOSE): keep receiving
    if (g.settings.closeButton == 1) return hideToTray();
    if (g.settings.closeButton == 2) {
        g.log->write("info", "close button: quit");
        return quitApp();
    }
    pm::ui::AskPanel::Info q;
    q.glyph = kIcoExit;
    q.title = tr(S::CloseAskTitle);
    q.body = fmt(S::CloseAskBody, {tr(S::AppName)});
    q.primary = tr(S::CloseAskTray);
    q.secondary = tr(S::CloseAskQuit);
    q.secondaryChoice = 2;
    q.check = tr(S::CloseAskRemember);
    q.checked = true;
    q.done = [](int choice) {
        if (g.quitting) return;
        if (choice == 0) {  // Esc / ×: nothing happens
            g.log->write("info", "close question: cancelled");
            return;
        }
        const bool remember = g.closePanel.checked();
        g.log->write("info", std::string("close question: ") + (choice == 1 ? "tray" : "quit") +
                                 (remember ? " (remembered)" : " (this time)"));
        if (remember) setCloseButton(choice == 1 ? 1 : 2, false);
        if (choice == 1) hideToTray();
        else quitApp();
    };
    g.closePanel.open(g.hwnd, std::move(q));
}

}  // namespace pm_app
