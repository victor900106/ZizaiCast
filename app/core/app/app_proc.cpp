// 自在投影 app: core/app/app_proc.cpp — core/app（視窗程序、命令分派、--dev 測試台）。
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

std::string endSessionWhy(LPARAM lp) {
    std::string s;
    if (lp & ENDSESSION_CLOSEAPP) s += "installer / Restart Manager, ";
    if (lp & ENDSESSION_CRITICAL) s += "critical, ";
    if (lp & ENDSESSION_LOGOFF) s += "logoff";
    else if (!(lp & ENDSESSION_CLOSEAPP)) s += "Windows shutdown / restart";
    while (!s.empty() && (s.back() == ' ' || s.back() == ',')) s.pop_back();
    return s;
}

LRESULT CALLBACK appProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    const LRESULT r = appProcInner(h, msg, wp, lp);
    // Anything that may change the live toolbar (source, recording, fullscreen).
    if (!g.quitting && ((msg >= WM_APP && msg < WM_APP + 32) || msg == WM_SIZE || msg == WM_KEYDOWN ||
                        msg == WM_TIMER || msg == WM_CONTEXTMENU || (msg == g.devCmdMsg && g.devCmdMsg))) {
        syncViewTools();  // picture gone: translation closed, zoom 1×
        refreshToolbar();
        maybeShowDeferredUpdate();  // a prompt held back while mirroring / hidden
    }
    return r;
}

LRESULT appProcInner(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PM_TOOL:
        runCommand(static_cast<UINT>(wp));
        return 0;
    case WM_PM_SHARE:
        onShareEvent(wp, lp);
        return 0;
    case WM_MOVE:
    case WM_SIZE:
        g.shareChip.reposition();  // stays over the window's bottom
        g.applyChip.reposition();
        break;
    case WM_DISPLAYCHANGE:  // a monitor unplugged / resolution changed: still reachable?
        if (IsWindowVisible(h)) keepOnScreen(h);
        break;
    case WM_PM_LOCALDIR:  // settle a moment: a copy fires many changes
        SetTimer(h, kLocalSettleTimer, 500, nullptr);
        return 0;
    case WM_PM_SOURCE:
        onSourceTakeover(static_cast<int>(wp), static_cast<int>(lp));
        return 0;
    case WM_PM_SRCEV: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
        const SourceEvent kind = static_cast<SourceEvent>(wp);
        if (kind >= AndState) onAndroidEvent(kind, text ? *text : std::wstring());
        else onMiracastEvent(kind, text ? *text : std::wstring());
        return 0;
    }
    case WM_PM_STATE: {
        const WPARAM prev = g.videoState;
        g.videoState = wp;
        if (wp == StateLost && g.liveSource == SrcAndroid) {
            // Android: no hold (see androidGoneUi).
            if (androidLive()) releaseSource(SrcAndroid);
            setAndroidInput(false);
            g.videoState = StateIdle;
            g.status->releaseHold();
            g.liveSource = SrcNone;
            androidGoneUi();
            return 0;
        }
        if (wp == StateLost) {
            // The window is free again (the held frame is only for show).
            if (int a = g_active.load(); a == SrcAirPlay || a == SrcAndroid) releaseSource(a);
            if (g.liveSource == SrcAndroid) g.window->setPointerHandler(nullptr), g.window->setKeyHandler(nullptr);
            SetWindowTextW(h, fmt(S::TitleLost, {tr(S::AppName)}).c_str());
            g.window->setDimmed(true);  // the held last frame, until the hold ends
            const std::wstring lostText = tr(g.liveSource == SrcAndroid ? S::LostAndroid : S::LostIphone);
            if (recording())
                stopRecording(fmt(S::LostRec, {g.recordingFile.filename().wstring()}));
            else
                g.window->showToast(lostText);
            SetTimer(h, kLostTimer, kLostHoldMs, nullptr);
            g.lastOrientation = 0;
            return 0;
        }
        if (wp == StateMirroring) {
            KillTimer(h, kLostTimer);  // a new session took over a held frame
            g.window->setDimmed(false);
            const std::wstring app = tr(S::AppName);
            SetWindowTextW(h, (g.peerName.empty() ? fmt(S::TitleMirroring, {app})
                                                  : fmt(S::TitleMirroringName, {app, g.peerName})).c_str());
            if ((g.announcePending || prev == StateIdle || prev == StateLost) && nowMs() - g.takeoverAtMs > 4000) {
                g.announcePending = false;
                if (g.liveSource == SrcAndroid)  // how to get around (right click is 返回, not the menu)
                    g.window->showToast(fmt(g.settings.rightClickMenu ? S::ConnectedAndroidMenu : S::ConnectedAndroid,
                                            {g.sourceName}), 5000);
                else  // iPhone / Miracast: where the toolbar and the menu are (0.7.2)
                    g.window->showToast(g.peerName.empty() ? std::wstring(tr(S::Connected))
                                                           : fmt(S::ConnectedHint, {g.peerName}), 5000);
            }
            prewarmTranslation();
            if (!g.settings.toolbarHintShown) {  // 0.7.8: the very first phone: the toolbar shows itself once
                g.settings.toolbarHintShown = true;
                saveSettings();
                g.window->revealToolbar(7000, tr(S::ToolbarIntro));
            }
            if (g.liveSource == SrcAndroid && androidLive()) {
                g.androidWatchCount = -1;
                SetTimer(h, kAndroidWatchTimer, 1000, nullptr);
            }
        } else if (wp == StatePaused) {
            SetWindowTextW(h, currentTitle().c_str());
        } else {
            if (int a = g_active.load(); a == SrcAirPlay || a == SrcAndroid) releaseSource(a);
            if (g.liveSource == SrcAndroid) g.window->setPointerHandler(nullptr), g.window->setKeyHandler(nullptr);
            if (g_active.load() == SrcNone) g.liveSource = SrcNone;
            SetWindowTextW(h, g.idleTitle.c_str());
            g.lastOrientation = 0;
            if (recording()) stopRecording(fmt(S::MirrorEndedRec, {g.recordingFile.filename().wstring()}));
            if (g.miracastRenamePending && g_active.load() == SrcNone) {  // a cast ended without its event
                g.miracastRenamePending = false;
                if (g.settings.miracast && !g.miracastUnsupported) {
                    miracastApply(false);
                    miracastApply(true);
                }
            }
        }
        return 0;
    }
    case WM_PM_EVENT: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
        onEvent(static_cast<EventKind>(wp), text ? *text : std::wstring());
        return 0;
    }
    case WM_PM_UPDATE: {
        std::unique_ptr<UpdateResult> r(reinterpret_cast<UpdateResult*>(lp));
        if (r && wp == UpdChecked) onUpdateChecked(*r);
        else if (r && wp == UpdDownloaded) onUpdateDownloaded(*r);
        return 0;
    }
    case WM_PM_OPTION:
        if (wp == OptAutostart) setAutostartOption(lp != 0);
        else if (wp == OptPin) setPinOption(lp != 0);
        return 0;
    case WM_PM_TRAY:
        switch (LOWORD(lp)) {
        case NIN_BALLOONUSERCLICK:
            if (g.balloonUpdate && !offerVersion().empty()) {  // 「點這裡看更新內容」
                g.log->write("info", "update balloon clicked");
                openUpdateDialog(true);
                break;
            }
            bringToFront();
            break;
        case WM_LBUTTONDBLCLK:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            bringToFront();
            break;
        case WM_CONTEXTMENU:
            showTrayMenu(static_cast<short>(LOWORD(wp)), static_cast<short>(HIWORD(wp)));
            break;
        }
        return 0;
    case WM_CONTEXTMENU:
        showContextMenu(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)));
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && !isFullscreen() && connectingNow()) {  // Esc cancels 連線中 (like Ctrl+D)
            g.log->write("info", "Esc while connecting: cancelled");
            disconnectLive();
            return 0;
        }
        if (wp == VK_F1 && GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_MENU) >= 0) {  // 快速鍵一覽 (0.7.8)
            openShortcuts();
            return 0;
        }
        if (GetKeyState(VK_CONTROL) < 0 && !(GetKeyState(VK_MENU) < 0)) {
            if (wp == 'S') {
                takeSnapshot();
                return 0;
            }
            if (wp == 'T') {
                setTopmost(!g.settings.topmost);
                return 0;
            }
            const bool shift = GetKeyState(VK_SHIFT) < 0;
            switch (wp) {
            case 'R': toggleRecording(); return 0;
            case VK_RIGHT: rotateView(+1); return 0;
            case VK_LEFT: rotateView(-1); return 0;
            case 'H': toggleMirror(); return 0;
            case '0':
            case VK_NUMPAD0:
                if (shift) zoomReset();  // Ctrl+Shift+0: magnifier back to 1×
                else resetView();
                return 0;
            case 'F': toggleDeviceFrame(); return 0;
            case 'D': disconnectLive(); return 0;
            case VK_UP:    // 音量 one phone step up / down, Ctrl+M 靜音 (volume_ui.h)
            case VK_DOWN:
            case 'M': g_volumeUi.ctrlKey(wp); return 0;
            // 放大鏡 / 翻譯 (handled here so they never reach the phone, and do nothing without a picture)
            case VK_OEM_PLUS:
            case VK_ADD: zoomBy(+1); return 0;
            case VK_OEM_MINUS:
            case VK_SUBTRACT: zoomBy(-1); return 0;
            case 'K': runCommand(CmdFilterCycle); return 0;
            case 'P': toggleFreeze(); return 0;
            case 'L': runCommand(shift ? CmdTranslateRegion : CmdTranslateToggle); return 0;
            case 'O': runCommand(CmdTranslateOriginal); return 0;
            }
        }
        break;
    case WM_TIMER:
        if (wp == kOrientationTimer) {
            followOrientation(h);
            return 0;
        }
        if (wp == kSyncTimer) {
            if (g.settings.avSync) g.window->setSyncMode(true, audioLatencyMs());
            return 0;
        }
        if (wp == kLostTimer) {
            endLostHold();
            if (g.videoState == StateLost) {
                g.videoState = StateIdle;
                SetWindowTextW(h, g.idleTitle.c_str());
            }
            return 0;
        }
        if (wp == kResumeTimer) {
            checkAfterResume();
            return 0;
        }
        if (wp == kRecordTimer) {
            checkRecording();
            return 0;
        }
        if (wp == kFitTimer) {
            KillTimer(h, kFitTimer);
            fitWindowToPicture(h);
            return 0;
        }
        if (wp == kAndroidTimer) {
            tryAndroidReconnect();
            return 0;
        }
        if (wp == kAndroidWatchTimer) {
            androidWatch();
            return 0;
        }
        if (wp == kMiraIdleTimer) {
            miracastPendingGone();
            return 0;
        }
        if (wp == kPinEndTimer) {
            pinSessionEnded();
            return 0;
        }
        if (wp == kConnectTimer) {
            connectWatchTick();
            return 0;
        }
        if (wp == kPairTimer) {
            KillTimer(h, kPairTimer);
            if (g.pairPanel.isOpen() && !g.pairPanel.codeMode() &&
                g.android->state() == pm::AndroidSource::State::WaitingForPairing) {
                g.log->write("info", "pairing: no phone after " + std::to_string(g.pairTimeoutMs / 1000) + " s");
                g.pairPanel.setStatus(tr(S::PairNoAnswer), true);
            }
            return 0;
        }
        if (wp == kLocalSettleTimer) {
            scanLocalUpdates();
            return 0;
        }
        if (wp == kLocalTimer) {
            if (g.localDir.empty() || !g.localWatchAlive) {  // folder (re)created since
                g.localDir = localUpdateDir();
                startLocalWatch();
            }
            scanLocalUpdates();
            return 0;
        }
        if (wp == kShareNetTimer) {
            SetTimer(h, kShareNetTimer, kShareNetCheckMs, nullptr);  // (after resume it fired at 3 s)
            checkShareNetwork();  // kills it once nothing is shared
            return 0;
        }
        if (wp == kUpdateTimer) {
            SetTimer(h, kUpdateTimer, kUpdateIntervalMs, nullptr);  // then every 6 h
            g.updatePromptReady = true;  // offers may prompt from now on
            // 0.7.9: a --dev copy checks by itself only against an update_url set in
            // its settings.ini (the update tests' local server), never GitHub; 檢查更新
            // from the menu still works.
            if (g.dev && !g.settings.updateUrlSet) {
                static bool said = false;
                if (!std::exchange(said, true)) g.log->write("info", "dev: automatic update check off (no update_url in settings.ini)");
                return 0;
            }
            checkForUpdates(false);
            return 0;
        }
        break;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND) g.log->write("info", "system suspending");
        else if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) onResume();
        return TRUE;
    case WM_CLOSE:
        if (!g.quitting && (g.trayOk || g.testOffscreen)) {  // --test-offscreen: no tray icon, same flow
            onCloseRequest();
            return 0;
        }
        break;
    // Windows shutdown / restart / logoff, or an installer's Restart Manager.
    // QUERY only asks (another app may still cancel): nothing changes yet.
    // END with wp TRUE: the session really ends and the process may be
    // killed as soon as this returns, so the recording is finalized and the
    // settings saved right here. wp FALSE: cancelled, carry on.
    case WM_QUERYENDSESSION:
        g.log->write("info", "session ending requested (" + endSessionWhy(lp) + ")" +
                                 (recording() ? "; a recording is running" : ""));
        if (recording()) ShutdownBlockReasonCreate(h, tr(S::RecSavingShutdown));
        return TRUE;
    case WM_ENDSESSION:
        if (!wp) {
            g.log->write("info", "session end cancelled: running on");
            ShutdownBlockReasonDestroy(h);
            return 0;
        }
        g.log->write("info", "session ending (" + endSessionWhy(lp) + "): saving the recording and settings");
        g.quitting = true;
        stopRecording();
        stopSharing();
        saveSettings();
        ShutdownBlockReasonDestroy(h);
        g.log->write("info", "session ending: done");
        if (lp & ENDSESSION_CLOSEAPP) quitApp();  // Restart Manager: it waits for us to exit
        return 0;
    case WM_DESTROY:
        trayRemove();
        break;
    default:
        if (msg == g.taskbarCreatedMsg && g.taskbarCreatedMsg) {  // Explorer restarted
            trayAdd();
            return 0;
        }
        if (msg == g.devCmdMsg && g.devCmdMsg) {  // --dev test scripts: run a menu command
            if (wp == 900 || wp == 901) devMenuShots(static_cast<int>(lp) - 1, wp == 901);  // menu PNGs (lp = hot row + 1)
            else if (wp == 905) {  // 放大鏡 / 翻譯: the open question dialog as ask.png
                const fs::path d = fs::temp_directory_path() / L"pmshots";
                std::error_code ec;
                fs::create_directories(d, ec);
                const bool a = g.askPanel.renderPng((d / L"ask.png").wstring());
                g.log->write("info", std::string("dev ask shot: ") + (a ? "ok (" + toUtf8(g.askPanel.title()) + ")" : "- (no dialog)"));
            } else if (wp == 936) {  // 「線上」 badge test: two made-up blocks (a card, a listed row) over the picture
                std::vector<pm::VideoWindow::TextBox> b(2);
                b[0] = {0.08f, 0.30f, 0.92f, 0.36f, tr(S::TrOnlineTestOkPlain), L"Connected", 1};
                b[0].online = lp != 0;
                b[1] = {0.10f, 0.60f, 0.30f, 0.605f, std::wstring(1, wchar_t(0xE000)) + tr(S::TrOnlineSaved), L"Saved", 1};
                b[1].online = lp != 0;
                g.log->write("info", "dev online badge boxes");
                g.window->setTextOverlay(std::move(b));
            } else if (wp == 941) {  // the open panels as if moved to DPI lp & 0xFFFF (lp & 0x10000: no WM_DPICHANGED)
                pm::ui::trset::devDpiChanged(static_cast<int>(lp & 0xFFFF), (lp & 0x10000) != 0);
            } else if (wp == 942) {  // self-check of the open panels: hit where drawn, window / client / target sizes
                pm::ui::trset::devSelfCheck();
            } else if (wp >= 937 && wp <= 940) {  // the panels on a small screen (DPI / work area tests)
                if (wp == 937) {  // what the open panels show now: llm_view[_lp] / online_view[_lp].png
                    const fs::path d = fs::temp_directory_path() / L"pmshots";
                    std::error_code ec;
                    fs::create_directories(d, ec);
                    pm::ui::trset::devShots(d.wstring(), lp > 0 ? L"_" + std::to_wstring(lp) : L"", true);
                } else if (wp == 938) {  // scroll the 線上翻譯 panel by lp DIPs (wheel)
                    pm::ui::trset::devScroll(1, static_cast<float>(static_cast<int>(lp)));
                } else if (wp == 939) {  // key lp (e.g. 9 = Tab) in the 線上翻譯 panel
                    pm::ui::trset::devKey(1, static_cast<UINT>(lp));
                } else {  // 940: panels opened from now on at DPI (lp & 0xFFFF) on a work area (lp >> 16) px high
                    pm::ui::trset::devScreen(static_cast<int>(lp & 0xFFFF), static_cast<int>((lp >> 16) & 0xFFFF));
                    g.log->write("info", "dev translate settings screen: dpi " + std::to_string(lp & 0xFFFF) + ", work area " +
                                             std::to_string((lp >> 16) & 0xFFFF) + " px");
                }
            } else if (wp >= 930 && wp <= 935) {  // 翻譯 ▸ 本機 AI 翻譯 / 線上翻譯 panels (app/tr_settings.cpp)
                if (wp == 930) {  // the open panels + their question as llm / online / trask[_lp].png
                    const fs::path d = fs::temp_directory_path() / L"pmshots";
                    std::error_code ec;
                    fs::create_directories(d, ec);
                    const int n = pm::ui::trset::devShots(d.wstring(), lp > 0 ? L"_" + std::to_wstring(lp) : L"");
                    g.log->write("info", "dev translate settings shots: " + std::to_string(n));
                } else if (wp == 931 || wp == 932) {  // click item lp in 本機 AI 翻譯 (931) / 線上翻譯 (932)
                    g.log->write("info", "dev translate settings click " + std::to_string(wp) + ": " + std::to_string(lp));
                    pm::ui::trset::devClick(wp == 931 ? 0 : 1, static_cast<int>(lp));
                } else if (wp == 933) {  // answer their question (lp 1 primary, 2 secondary, 0 Esc)
                    pm::ui::trset::devAnswer(static_cast<int>(lp));
                } else if (wp == 934) {  // a made-up key in the 線上翻譯 key box (lp 1: an Azure-style one)
                    pm::ui::trset::devSetKey(lp == 1 ? L"0123456789abcdef0123456789abcdef" : L"00000000-0000-0000-0000-000000000000:fx");
                } else if (wp == 935) {  // 「線上」 badges: mark every shown block (lp 0: none) as translated online
                    std::vector<std::wstring> all;
                    if (lp && g.translator)
                        for (const auto& it : g.translator->lastItems()) all.push_back(it.original);
                    g.log->write("info", "dev online badges: " + std::to_string(all.size()));
                    g.window->setTextOverlayOnline(std::move(all));
                }
            } else if (wp == 906) {  // answer the open question dialog (lp 1 = primary button, 0 = Esc)
                g.log->write("info", std::string("dev ask answer: ") + (lp == 1 ? "primary" : "cancel"));
                g.askPanel.close(lp == 1 ? 1 : 0);
            } else if (wp == 908) {  // 按 X 時要怎麼做？: the open question as closeask.png
                const fs::path d = fs::temp_directory_path() / L"pmshots";
                std::error_code ec;
                fs::create_directories(d, ec);
                const bool a = g.closePanel.renderPng((d / L"closeask.png").wstring());
                const bool m = pm::ui::renderMenuPng(closeItems(), (d / L"menu_close.png").wstring());
                g.log->write("info", std::string("dev close shot: ") + (a ? "ok" : "- (no question)") + (m ? ", menu ok" : ""));
            } else if (wp == 909 && g.closePanel.isOpen()) {  // answer it like a click (lp 0 ×, 1 縮到右下角, 2 結束程式, 3 checkbox)
                g.log->write("info", "dev close answer: " + std::to_string(lp));
                if (lp == 0) SendMessageW(g.closePanel.hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);  // the real Esc path
                else g.closePanel.click(static_cast<int>(lp));
            } else if (wp == 907) devViewMenuShots();
            else if (wp == 912) {  // 說明 ▸ 快速鍵一覽: open it and save it as shortcuts.png (0.7.8)
                openShortcuts();
                const fs::path d = fs::temp_directory_path() / L"pmshots";
                std::error_code ec;
                fs::create_directories(d, ec);
                const bool a = g.keysPanel.renderPng((d / L"shortcuts.png").wstring());
                g.log->write("info", std::string("dev shortcuts shot: ") + (a ? "ok" : "-"));
            }
            else if (wp == 911) {  // menu screenshots: act as a PC without Wi-Fi Direct (Miracast unavailable)
                g.miracastUnsupported = true;
                g.miracastReason = tr(S::ModMiraNoWifi, pm::i18n::Lang::ZhTW);
                refreshIdleHints();
                g.log->write("info", "dev: Miracast marked unavailable (no Wi-Fi Direct)");
            } else if (wp == 950) {  // idle cards screenshots: lp bit 0 = adb ok, lp >> 1 = MiraWhy (0 = on); -1 = real
                g.testWays = static_cast<int>(lp);
                refreshIdleHints();
                g.log->write("info", "dev: idle ways " + std::to_string(g.testWays));
            }
            else if (wp == 902) {  // the open pairing panel / About window as PNGs (pair.png, about.png)
                const fs::path d = fs::temp_directory_path() / L"pmshots";
                std::error_code ec;
                fs::create_directories(d, ec);
                const bool p = g.pairPanel.renderPng((d / L"pair.png").wstring());
                const bool a = g.aboutPanel.renderPng((d / L"about.png").wstring());
                g.log->write("info", std::string("dev panel shots: pair ") + (p ? "ok" : "-") + ", about " + (a ? "ok" : "-"));
            } else if (wp == 904) {  // 自動更新: the open 「有新版本」 dialog as update.png
                const fs::path d = fs::temp_directory_path() / L"pmshots";
                std::error_code ec;
                fs::create_directories(d, ec);
                const bool u = g.updatePanel.renderPng((d / L"update.png").wstring());
                g.log->write("info", std::string("dev update shot: ") + (u ? "ok" : "- (dialog not open)"));
            } else if (wp == 903) {  // 傳到手機: panel + chip PNGs, the share URL for test scripts
                const fs::path d = fs::temp_directory_path() / L"pmshots";
                std::error_code ec;
                fs::create_directories(d, ec);
                const bool p = g.sharePanel.renderPng((d / L"share.png").wstring());
                const bool c = g.shareChip.renderPng((d / L"chip.png").wstring());
                if (g.applyChip.visible()) g.applyChip.renderPng((d / L"applychip.png").wstring());  // p9 現在重新連線套用
                const bool k = g.sharePicker.renderPng((d / L"picker.png").wstring());
                std::ofstream(d / L"share_url.txt", std::ios::trunc) << g.share.url();
                g.log->write("info", std::string("dev share shots: panel ") + (p ? "ok" : "-") + ", chip " + (c ? "ok" : "-") +
                                         ", chip visible " + (g.shareChip.visible() ? "yes" : "no") + ", picker " +
                                         (k ? "ok (" + std::to_string(g.sharePicker.checkedCount()) + " ticked, " +
                                                  std::to_string(g.sharePicker.thumbsPending()) + " thumbnails pending)"
                                            : "-"));
            } else if (wp == 908) {  // 傳到手機 picker: 傳送 N 個
                g.log->write("info", "dev picker: send " + std::to_string(g.sharePicker.checkedCount()));
                g.sharePicker.send();
            } else if (wp == 920) {  // 傳到手機 tests: rebind as if the LAN address had gone (M3)
                checkShareNetwork(true);
            } else if (wp == 910) {  // 傳到手機 tests: the video window with its toast as window.png
                const fs::path d = fs::temp_directory_path() / L"pmshots";
                std::error_code ec;
                fs::create_directories(d, ec);
                g.log->write("info", std::string("dev window shot: ") +
                                         (g.window->saveWindowShot((d / L"window.png").wstring()) ? "ok" : "-"));
            } else if (wp == 909) {  // 傳到手機 picker: tick / untick item lp (-1: 全選 / 全不選)
                if (lp < 0) g.sharePicker.setAll(g.sharePicker.checkedCount() == 0);
                else g.sharePicker.toggle(static_cast<int>(lp));
            }
            else runCommand(static_cast<UINT>(wp));
            return 0;
        }
        if (msg == g.showMsg && g.showMsg) {  // a second launch asks us to show up
            bringToFront();
            return 0;
        }
        break;
    }
    return CallWindowProcW(g.prevProc, h, msg, wp, lp);
}

}  // namespace pm_app
