// 自在投影 app: connect/android.cpp — connect（iPhone / Android / Miracast 連線狀態）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "help/help.h"
#include "capture/capture.h"
#include "connect/connect.h"
#include "connect/connect_internal.h"

namespace pm_app {

// ---- Android (wireless debugging) ----
// 0.7.8 UX (p7) 右鍵行為: a right click on the Android picture (Shift+right
// click never gets here: the window keeps its menu). 返回: goes to the phone;
// the very first one also shows where the menu is (once, settings.ini
// right_click_hint_shown). 開啟選單: not sent to the phone; the release opens
// the window's menu at the cursor (posted: the window still has the capture).
// True if the event was taken here.
bool androidRightClick(const pm::VideoWindow::PointerEvent& e) {
    using K = pm::VideoWindow::PointerEvent::Kind;
    if (e.button != 1 || (e.kind != K::Down && e.kind != K::Up)) return false;
    if (g.settings.rightClickMenu) {
        // A real release only (not capture lost / picture gone with the button still held).
        if (e.kind == K::Up && GetKeyState(VK_RBUTTON) >= 0) PostMessageW(g.hwnd, WM_PM_TOOL, CmdMoreMenu, 0);
        return true;
    }
    if (e.kind == K::Down && !g.settings.rightClickHintShown) {
        g.settings.rightClickHintShown = true;
        saveSettings();
        g.window->showToast(fmt(S::RightClickHint, {tr(S::MenuSettings), tr(S::MenuRightClick)}), 7000);
    }
    return false;
}

void setRightClickMenu(bool menu) {
    if (menu != g.settings.rightClickMenu) {
        g.settings.rightClickMenu = menu;
        g.settings.rightClickHintShown = true;  // they know now
        saveSettings();
        g.log->write("info", std::string("android right click: ") + (menu ? "menu" : "back"));
        if (g.keysPanel.isOpen()) g.keysPanel.setItems(shortcutItems());  // 快速鍵一覽 follows it
    }
    g.window->showToast(tr(menu ? S::RightClickMenuToast : S::RightClickBackToast));
}

void setAndroidInput(bool on) {
    if (!on) {
        g.window->setPointerHandler(nullptr);
        g.window->setKeyHandler(nullptr);
        return;
    }
    if (g.testAndroid) {  // --test-source: log what would be sent
        g.window->setPointerHandler([](const pm::VideoWindow::PointerEvent& e) {
            if (androidRightClick(e)) return;
            if (e.kind == pm::VideoWindow::PointerEvent::Kind::Down)
                g.log->write("info", "test android: tap " + std::to_string(e.x) + "," + std::to_string(e.y));
        });
        g.window->setKeyHandler([](unsigned vk, bool down, wchar_t) {
            if (down) g.log->write("info", "test android: key " + std::to_string(vk));
        });
        return;
    }
    pm::AndroidSource* a = g.android;
    g.window->setPointerHandler([a](const pm::VideoWindow::PointerEvent& e) {
        if (!androidRightClick(e)) a->sendPointer(e);
    });
    g.window->setKeyHandler([a](unsigned vk, bool down, wchar_t ch) { a->sendKey(vk, down, ch); });
}

// adb connected a phone (after pairing, or a paired phone reconnecting).
void onAndroidConnected(const std::wstring& rawName, bool test) {
    const std::wstring name = rawName.empty() ? std::wstring(tr(S::AndroidPhone)) : rawName;
    const bool userAsked = g.pairPanel.isOpen() || test;
    const int a = g_active.load();
    g.log->write("info", "android connected: \"" + toUtf8(name) + "\"" + (userAsked ? " (pairing)" : " (reconnect)"));
    if (!test && g.settings.androidPaired != 1) {  // from now on auto-reconnect looks for it
        g.settings.androidPaired = 1;
        saveSettings();
    }
    if (a != SrcNone && a != SrcAndroid && (g.settings.takeoverKeep || !userAsked)) {
        // Busy: keep the current phone. A reconnect in the background never
        // takes over; it is tried again when the window is free (stop():
        // Connecting -> Idle, else tryAndroidReconnect never runs again).
        g.android->stop();
        if (userAsked) {
            g.pairPanel.close();
            g.window->showToast(fmt(S::AndroidBusy, {g.sourceName, name}));
        }
        return;
    }
    g.pairPanel.close();
    g.androidUserStopped = false;
    claimSource(SrcAndroid, true);
    endLostHold();
    g.announcePending = true;
    setPeer(SrcAndroid, name);
    g.window->showPin(L"");
    g.window->setConnecting(g.peerName);
    SetWindowTextW(g.hwnd, fmt(S::TitleConnecting, {tr(S::AppName), g.peerName}).c_str());
    bringToFront();
    setAndroidInput(true);
    armConnectWatch();
    if (!test && !g.android->start(g.androidVideo, g.androidAudio)) {
        g.log->write("error", "android: start() refused");
        setAndroidInput(false);
        releaseSource(SrcAndroid);
        g.status->forceIdle();
        g.liveSource = SrcNone;
        g.announcePending = false;
        SetWindowTextW(g.hwnd, g.idleTitle.c_str());
        g.window->showToast(tr(S::AndroidStartFail));
    }
}

// 中斷 Android 連線 (old menu command): same as 中斷連線.
void stopAndroid() { disconnectLive(); }

// The Android phone went away by itself (無線偵錯 turned off: the stream ends;
// Wi-Fi lost: the watchdog notices the silence): straight back to the idle
// screen — no 3 s hold, a reconnect takes longer than that anyway.
void androidGoneUi() {
    KillTimer(g.hwnd, kAndroidWatchTimer);
    KillTimer(g.hwnd, kLostTimer);
    g.window->setDimmed(false);
    SetWindowTextW(g.hwnd, g.idleTitle.c_str());
    g.lastOrientation = 0;
    if (recording())
        stopRecording(fmt(S::AndroidGoneRec, {g.recordingFile.filename().wstring()}));
    else
        g.window->showToast(tr(S::AndroidGone), 4000);
}

// Every second while an Android phone is live. scrcpy repeats the last video
// frame every 100 ms and the audio capture never pauses, so ~6 s without
// either means the phone is gone without the socket noticing (it left the
// Wi-Fi, or went out of range): stop it and go idle. Auto-reconnect stays on.
void androidWatch() {
    if (!androidLive()) {
        KillTimer(g.hwnd, kAndroidWatchTimer);
        g.androidWatchCount = -1;
        return;
    }
    const long long n = g.status->frames.load() + (g.androidAudioPackets ? g.androidAudioPackets->load() : 0);
    const long long now = nowMs();
    if (n != g.androidWatchCount || g.videoState != StateMirroring) {
        g.androidWatchCount = n;
        g.androidWatchAt = now;
        return;
    }
    if (now - g.androidWatchAt < kAndroidStallMs) return;
    g.log->write("warn", "android: no video or audio for " + std::to_string((now - g.androidWatchAt) / 1000) +
                             " s: phone gone (Wi-Fi lost?), stopping");
    releaseSource(SrcAndroid);
    setAndroidInput(false);
    if (g.android) g.android->stop();
    androidGoneUi();
    g.status->forceIdle();
    g.liveSource = SrcNone;
}

void androidNav(UINT cmd) {
    if (!androidLive() || !g.android) return;
    if (cmd == CmdAndroidBack) g.android->pressBack();
    else if (cmd == CmdAndroidHome) g.android->pressHome();
    else g.android->pressAppSwitch();
}

void tryAndroidReconnect() {
    if (!g.androidOk || !g.settings.androidAuto || g.androidUserStopped || g.pairPanel.isOpen()) return;
    // Never paired here: nothing to reconnect (no adb + mDNS search every 45 s;
    // the first pairing goes through the pairing panel).
    if (g.settings.androidPaired != 1) return;
    if (g_active.load() != SrcNone || g.videoState == StateLost) return;
    using S = pm::AndroidSource::State;
    const S s = g.android->state();
    if (s != S::Idle && s != S::Error) return;
    if (g.android->connectKnownDevices()) g.log->write("info", "android: looking for paired phones");
}

void setAndroidAuto(bool on) {
    g.settings.androidAuto = on;
    saveSettings();
    g.window->showToast(tr(on ? S::AndroidAutoOn : S::AndroidAutoOff));
    if (on) tryAndroidReconnect();
}

// Which pm_android state text (Chinese, see the table's ModAnd* entries) `d` is.
bool androidDetailIs(const std::wstring& d, S id) {
    std::wstring zh = tr(id, pm::i18n::Lang::ZhTW);
    if (const size_t ph = zh.find(L"{0}"); ph != std::wstring::npos) zh.resize(ph);  // prefix
    return d.rfind(zh, 0) == 0;
}

// Pairing failed / timed out: what happened and what to do next.
std::wstring pairFailureText(const std::wstring& d, bool codeMode) {
    if (androidDetailIs(d, S::ModAndTimeout)) return tr(S::PairNoAnswer);
    if (androidDetailIs(d, S::ModAndPairFailed)) return tr(codeMode ? S::PairFailCode : S::PairFailQr);
    if (androidDetailIs(d, S::ModAndAdbFail)) return fmt(S::PairAdbFail, {tr(S::AppName)});
    return d.empty() ? std::wstring(tr(S::PairFailRetry)) : moduleText(d);
}

void startQrPairing() {
    std::vector<uint8_t> bgra;
    int size = 0;
    std::wstring text;
    if (g.android->beginQrPairing(bgra, size, text)) {
        g.pairPanel.setQr(bgra, size);
        g.pairPanel.setStatus(tr(S::PairWaiting));
    } else {
        g.pairPanel.setQr({}, 0);
        g.pairPanel.setStatus(tr(S::PairQrFail), true);
    }
}

void openPairPanel() {
    if (g.testNoNetwork && !g.pairPanel.isOpen()) {
        // --dev --test-no-network: the panel alone (screenshots in every language,
        // DevCommand 902) with a dummy code; adb is never started.
        pm::ui::PairPanel::Callbacks cb;
        cb.onClose = []() {};
        cb.onPairCode = [](const std::wstring&, const std::wstring&) { g.pairPanel.setStatus(tr(S::PairPairing)); };
        cb.onQrMode = []() {};
        cb.onHelp = []() { openTutorial(L"adb"); };
        if (!g.pairPanel.open(g.hwnd, std::move(cb))) return;
        constexpr int n = 33;
        std::vector<uint8_t> px(n * n * 4, 255);
        unsigned r = 12345;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                r = r * 1103515245u + 12345u;
                const bool finder = (x < 7 || x >= n - 7) && y < 7 || x < 7 && y >= n - 7;
                const int fx = x < 7 ? x : x - (n - 7), fy = y < 7 ? y : y - (n - 7);
                const bool dark = finder ? (fx == 0 || fx == 6 || fy == 0 || fy == 6 || (fx >= 2 && fx <= 4 && fy >= 2 && fy <= 4))
                                         : ((r >> 16) & 1) != 0;
                if (dark) std::fill_n(px.begin() + (y * n + x) * 4, 3, uint8_t{0});
            }
        g.pairPanel.setQr(px, n);
        g.pairPanel.setStatus(tr(S::PairWaiting));
        g.log->write("info", "test-no-network: pairing panel with a dummy code (no adb)");
        return;
    }
    if (!g.androidOk) {
        g.window->showToast(fmt(S::AndroidToolsMissing, {tr(S::AppName)}));
        return;
    }
    if (g.pairPanel.isOpen()) {
        g.pairPanel.open(g.hwnd, {});
        return;
    }
    pm::ui::PairPanel::Callbacks cb;
    cb.onClose = []() {
        KillTimer(g.hwnd, kPairTimer);
        // Always: a pairing still queued behind a busy adb worker (state not
        // yet WaitingForPairing) must not start later, invisibly.  Once
        // paired, cancelPairing() leaves the connect alone.
        g.android->cancelPairing();
    };
    cb.onPairCode = [](const std::wstring& hostPort, const std::wstring& code) {
        KillTimer(g.hwnd, kPairTimer);
        if (g.android->pairWithCode(hostPort, code)) g.pairPanel.setStatus(tr(S::PairPairing));
        else g.pairPanel.setStatus(tr(S::PairBadFormat), true);
    };
    cb.onQrMode = []() { startQrPairing(); };
    cb.onHelp = []() { openTutorial(L"adb"); };
    if (!g.pairPanel.open(g.hwnd, std::move(cb))) {
        g.window->showToast(tr(S::PairPanelFail));
        return;
    }
    g.androidUserStopped = false;
    startQrPairing();
}

void onAndroidEvent(SourceEvent kind, const std::wstring& text) {
    using S = pm::AndroidSource::State;
    switch (kind) {
    case AndState: {
        const S s = static_cast<S>(text.empty() ? 0 : text[0] - L'0');
        const std::wstring detail = text.size() > 1 ? text.substr(1) : L"";
        g.log->write("info", "android state " + std::to_string(static_cast<int>(s)) +
                                 (detail.empty() ? "" : ": " + toUtf8(detail)));
        // Start-up failed (or ended) before the first picture: the window
        // showed 連線中 for this phone and owned the source, so nothing else
        // (an iPhone, auto-reconnect) could get in. Give it back, say why.
        if ((s == S::Error || s == S::Idle) && androidLive() && !g.status->mirroring() && !g.status->holding()) {
            g.log->write("warn", "android: start-up ended before the first picture: back to idle");
            KillTimer(g.hwnd, kAndroidWatchTimer);
            releaseSource(SrcAndroid);
            setAndroidInput(false);
            g.status->forceIdle();
            g.liveSource = SrcNone;
            g.announcePending = false;
            SetWindowTextW(g.hwnd, g.idleTitle.c_str());
            // 0.7.9: adb / scrcpy-server internals (often raw English from the
            // server) stay in the log; the toast only says what to do.
            const bool technical = androidDetailIs(detail, I18n::ModAndMirrorFail) ||
                                   androidDetailIs(detail, I18n::ModAndServerFail) ||
                                   androidDetailIs(detail, I18n::ModAndForwardFail) ||
                                   androidDetailIs(detail, I18n::ModAndPushFail);
            if (s == S::Error)
                g.window->showToast(detail.empty() || technical
                                        ? std::wstring(tr(I18n::AndroidStartFailHint))
                                        : fmt(I18n::AndroidStartFailWhy, {moduleText(detail)}),
                                    6000);
            if (!g.pairPanel.isOpen()) break;
        }
        if (!g.pairPanel.isOpen()) {
            if (s == S::Error && androidLive())
                g.window->showToast(detail.empty() ? std::wstring(tr(I18n::AndroidError)) : moduleText(detail));
            break;
        }
        if (s == S::Mirroring) break;
        switch (s) {
        case S::WaitingForPairing:  // steps are on the panel; a failure text stays until the phone answers
            if (!g.pairPanel.statusIsError()) g.pairPanel.setStatus(tr(I18n::PairWaiting));
            // Listening for the phone from now on: no answer by then -> say what to check.
            if (!g.pairPanel.codeMode()) SetTimer(g.hwnd, kPairTimer, g.pairTimeoutMs, nullptr);
            break;
        case S::Pairing:
            KillTimer(g.hwnd, kPairTimer);
            g.pairPanel.setStatus(detail.empty() ? std::wstring(tr(I18n::PairPairing)) : moduleText(detail));
            break;
        case S::Connecting:
            KillTimer(g.hwnd, kPairTimer);
            g.pairPanel.setStatus(detail.empty() ? std::wstring(tr(I18n::PairConnecting)) : moduleText(detail));
            break;
        case S::Error: {
            KillTimer(g.hwnd, kPairTimer);
            const bool code = g.pairPanel.codeMode();
            const std::wstring msg = pairFailureText(detail, code);
            // QR mode: a fresh code, listening again, so 「再掃一次」 works.
            if (!code && !androidDetailIs(detail, I18n::ModAndAdbFail)) startQrPairing();
            g.pairPanel.setStatus(msg, true);
            break;
        }
        default: break;
        }
        break;
    }
    case AndConnected: onAndroidConnected(text, false); break;
    case AndTestConnected: onAndroidConnected(text, true); break;
    case AndDisconnected:
        g.log->write("info", "android disconnected");
        if (!androidLive()) break;
        setAndroidInput(false);
        // The sinks were reset already: a shown picture gets the 連線中斷 hold
        // (StateLost). Nothing shown yet: back to idle here.
        if (!g.status->mirroring() && !g.status->holding()) {
            releaseSource(SrcAndroid);
            g.status->forceIdle();
            androidGoneUi();
        }
        break;
    default: break;
    }
}

}  // namespace pm_app
