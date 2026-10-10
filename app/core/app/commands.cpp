// 自在投影 app: core/app/commands.cpp — core/app（視窗程序、命令分派、--dev 測試台）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "help/help.h"
#include "share/share.h"
#include "capture/capture.h"
#include "translate/translate.h"
#include "connect/connect.h"
#include "view/view.h"
#include "update/update.h"
#include "menus/menus.h"
#include "core/app/core_app.h"

namespace pm_app {

void runCommand(UINT cmd) {
    if (g_volumeUi.runCommand(cmd)) return;  // 音量 / 靜音 (pm::ui::VolumeCommand ids)
    switch (cmd) {
    case CmdAndroidPair: openPairPanel(); break;
    case CmdTutorial: openTutorial(nullptr); break;
    case CmdShortcuts: openShortcuts(); break;
    case CmdHowTo: openTutorial(nullptr); break;
    case CmdMiracast: setMiracastOption(!g.settings.miracast); break;
    case CmdAndroidAuto: setAndroidAuto(!g.settings.androidAuto); break;
    case CmdAndroidBack:
    case CmdAndroidHome:
    case CmdAndroidRecents: androidNav(cmd); break;
    case CmdAndroidStop: stopAndroid(); break;
    case CmdDisconnect: disconnectLive(); break;
    case CmdMoreMenu: {
        POINT pt{};
        GetCursorPos(&pt);
        showContextMenu(pt.x, pt.y);
        break;
    }
    case CmdMiracastHelp: openTutorial(L"miracast"); break;
    case CmdLangAuto:
    case CmdLangZh:
    case CmdLangEn: setLanguage(static_cast<int>(cmd - CmdLangAuto)); break;
    case CmdLangJa: setLanguage(3); break;
    case CmdLangKo: setLanguage(4); break;
    case CmdAbout: openAbout(); break;
    case CmdShareLast:
    case CmdShareLastShot:
    case CmdShareLastRec:
    case CmdSharePick:
    case CmdShareStop:
    case CmdShareAuto: shareCommand(cmd); break;
    case CmdShow: bringToFront(); break;
    case CmdAutostart: setAutostartOption(!g.autostart); break;
    case CmdPin: setPinOption(!g.settings.requirePin); break;
    case CmdAudioAdvertAB: toggleAudioAdvertAB(); break;
    case CmdOpenShots: openScreenshotDir(); break;
    case CmdExit: quitApp(); break;  // tray / menu 結束: always quits (按 X 時 is for × / Alt+F4)
    case CmdCloseAsk:
    case CmdCloseTray:
    case CmdCloseQuit: setCloseButton(static_cast<int>(cmd - CmdCloseAsk), true); break;
    case CmdFullscreen:
        if (!IsWindowVisible(g.hwnd)) bringToFront();
        CallWindowProcW(g.prevProc, g.hwnd, WM_KEYDOWN, VK_F11, 0);  // the window's own toggle
        break;
    case CmdTopmost: setTopmost(!g.settings.topmost); break;
    case CmdSnapshot: takeSnapshot(); break;
    case CmdQualityStandard:
    case CmdQualityHigh:
    case CmdQualityMax: setQuality(static_cast<int>(cmd - CmdQualityStandard)); break;
    case CmdAvSync: setAvSync(!g.settings.avSync); break;
    case CmdRecord: toggleRecording(); break;
    case CmdOpenRecordings: openRecordingDir(); break;
    case CmdRotateRight: rotateView(+1); break;
    case CmdRotateLeft: rotateView(-1); break;
    case CmdMirror: toggleMirror(); break;
    case CmdResetView: resetView(); break;
    case CmdDeviceFrame: toggleDeviceFrame(); break;
    case CmdTheme0:
    case CmdTheme1:
    case CmdTheme2:
    case CmdTheme3: setTheme(static_cast<int>(cmd - CmdTheme0)); break;
    case CmdTakeoverNew: setTakeover(false); break;
    case CmdTakeoverKeep: setTakeover(true); break;
    case CmdCheckUpdate: checkForUpdates(true); break;
    case CmdInstallUpdate: installUpdate(); break;
    case CmdUpdateDialog: openUpdateDialog(true); break;
    case CmdZoomIn: zoomBy(+1); break;
    case CmdZoomOut: zoomBy(-1); break;
    case CmdZoomReset: zoomReset(); break;
    case CmdMagCycle: magnifierCycle(); break;
    case CmdFilter0:
    case CmdFilter1:
    case CmdFilter2:
    case CmdFilter3:
    case CmdFilter4: setFilterOption(static_cast<int>(cmd - CmdFilter0)); break;
    case CmdFilterCycle:
        if (needPicture()) setFilterOption((g.settings.filter + 1) % 5);
        break;
    case CmdFreeze: toggleFreeze(); break;
    case CmdTranslateToggle:
    case CmdTranslateScreen:
    case CmdTranslateRegion:
    case CmdTranslateOriginal:
    case CmdTranslateLive:
    case CmdTranslateClose:
    case CmdTrTargetZh:
    case CmdTrTargetEn:
    case CmdTrTargetJa:
    case CmdTrTargetKo:
    case CmdTrLayoutAuto:
    case CmdTrLayoutInPlace:
    case CmdTrLayoutList:
    case CmdTrDarkCards:
        // From the tray with the window hidden: show the picture being translated.
        if (cmd != CmdTrTargetZh && cmd != CmdTrTargetEn && cmd != CmdTrTargetJa && cmd != CmdTrTargetKo &&
            (cmd < CmdTrLayoutAuto || cmd > CmdTrDarkCards) && windowHidden() && viewAvailable())
            bringToFront();
        translateCommand(cmd);
        break;
    case CmdTrModelsFolder: openModelsFolder(); break;
    case CmdTrDeleteAll: askDeleteModels(-1); break;
    case CmdTrLocalAi: pm::ui::trset::openLocalAi(); break;
    case CmdTrOnline: pm::ui::trset::openOnline(); break;
    case CmdRightClickBack:
    case CmdRightClickMenu: setRightClickMenu(cmd == CmdRightClickMenu); break;
    case CmdApplyNow: applyNow(); break;
    default:
        if (cmd >= CmdTrDelete0 && cmd < CmdTrDelete0 + kModelPairCount) askDeleteModels(static_cast<int>(cmd - CmdTrDelete0));
        else if (cmd >= CmdTrDeleteLang0 && cmd < CmdTrDeleteLang0 + 5) {
            const int k = static_cast<int>(cmd - CmdTrDeleteLang0);
            // Files another downloaded language needs too: say so once; if
            // confirmed they go, and that language shows 未下載 (asks to
            // download again when it is next needed).
            const SharedModels sh = sourceSharedModels(k);
            if (!sh.pairs.empty())
                askDeleteModelSet(sh.pairs, modelSourceLabel(k), fmt(S::TrDeleteSharedAsk, {sh.users}));
            else
                askDeleteModelSet(sourceDeletablePairs(k), modelSourceLabel(k), {});
        }
        break;
    }
}

void runMenu(const std::vector<MenuItem>& items, POINT pt, bool keyboard, bool tray) {
    // Tray: the owner must be the foreground window so the menu sees clicks
    // elsewhere and gets the keyboard (it closes when another app activates).
    SetForegroundWindow(g.hwnd);
    pm::ui::MenuOptions o;
    o.selectFirst = keyboard;
    if (tray) {
        o.iconInstance = GetModuleHandleW(nullptr);
        o.headerIconId = IDI_APP;
    }
    static bool logged = false;
    const UINT cmd = pm::ui::trackMenu(g.hwnd, items, pt, o);
    if (!logged) {
        logged = true;
        char buf[64];
        std::snprintf(buf, sizeof buf, "menu opened in %.1f ms", pm::ui::lastOpenMs());
        g.log->write("info", buf);
    }
    PostMessageW(g.hwnd, WM_NULL, 0, 0);
    if (cmd) runCommand(cmd);
}

void showTrayMenu(int x, int y) { runMenu(trayMenuItems(), POINT{x, y}, false, true); }

void showContextMenu(int x, int y) {
    const bool keyboard = x == -1 && y == -1;
    if (keyboard) {  // Shift+F10 / menu key: centre of the window
        RECT r{};
        GetWindowRect(g.hwnd, &r);
        x = (r.left + r.right) / 2;
        y = (r.top + r.bottom) / 2;
    }
    runMenu(contextMenuItems(), POINT{x, y}, keyboard, false);
}

}  // namespace pm_app
