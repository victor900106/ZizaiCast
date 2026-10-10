// 自在投影 app: connect/session.cpp — connect（iPhone / Android / Miracast 連線狀態）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "help/help.h"
#include "capture/capture.h"
#include "connect/connect.h"
#include "connect/connect_internal.h"

namespace pm_app {

// 中斷連線 (toolbar, Ctrl+D, menus, tray) for whichever phone is live:
//   AirPlay  — the core has no per-client disconnect: the picture goes idle
//              first (so the server's reset is not a "lost" hold), then the
//              AirPlay server restarts, which drops the iPhone (as for refusals).
//   Miracast — disconnect() ends the cast.
//   Android  — stop(); no auto-reconnect until the next pairing.
void disconnectLive() {
    const int src = liveSourceNow();
    if (src == SrcNone) {
        g.window->showToast(tr(S::NoLivePhone));
        return;
    }
    const std::wstring who = liveName();
    g.log->write("info", "user disconnected " + toUtf8(who) + " (" + toUtf8(sourceLabel(src)) + ")");
    ++g_testKick;
    const std::wstring msg = fmt(S::DisconnectedName, {who});
    if (recording()) stopRecording(fmt(S::RecSavedSuffix, {msg}));
    else g.window->showToast(msg);
    KillTimer(g.hwnd, kLostTimer);
    KillTimer(g.hwnd, kAndroidWatchTimer);
    g.window->setDimmed(false);
    switch (src) {
    case SrcAndroid:
        g.androidUserStopped = true;
        releaseSource(SrcAndroid);  // its sink resets are dropped from here on
        setAndroidInput(false);
        if (g.android) g.android->stop();
        g.status->forceIdle();
        break;
    case SrcMiracast:
        releaseSource(SrcMiracast);
        if (g.miracast) g.miracast->disconnect();
        g.window->showPin(L"");
        g.window->onReset();
        sourceEndedIdle();
        break;
    case SrcAirPlay:
        releaseSource(SrcAirPlay);
        g.status->forceIdle();  // mirroring off: the restart's reset is not a "loss"
        g.sessionActive = false;
        g.announcePending = false;
        g.window->showPin(L"");
        restartServer();
        break;
    }
    g.liveSource = SrcNone;
    g.lastOrientation = 0;
    SetWindowTextW(g.hwnd, g.idleTitle.c_str());
}

// 0.7.8 UX (p9): something waits for the session to end — an AirPlay restart
// (PIN / 畫面清晰度 / 第二支手機連上時 / 名稱) or the Miracast name during a cast.
bool applyPendingNow() {
    return (g.restartPending && g.sessionActive) || (g.miracastRenamePending && liveSourceNow() == SrcMiracast);
}

// A name kept on one line in a wrapped toast (「自在投 / 影」): word joiners
// (U+2060, invisible) between its characters; DirectWrite still breaks a
// name wider than the line.
std::wstring unbrokenName(const std::wstring& s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); ++i) {
        o += s[i];
        if (i + 1 < s.size() && !IS_HIGH_SURROGATE(s[i])) o += L'⁠';
    }
    return o;
}

// 現在重新連線套用: a clean end of the session, as 中斷連線 does — AirPlay:
// the server restarts with the new settings / name (re-advertised; the
// iPhone picks 螢幕鏡像 again); Miracast: the cast ends and MiraDisconnected
// re-registers the new name. An iPhone still connecting / at the PIN: the
// server restart alone. Then a toast says how to come back (unless the
// 錄影已儲存 toast of a stopped recording is showing).
void applyNow() {
    g.applyChip.hide();
    if (!applyPendingNow()) {  // the session ended meanwhile: already applied
        g.window->showToast(tr(S::ApplyNowDone));
        return;
    }
    const int src = liveSourceNow();
    const bool wasRecording = recording();
    g.log->write("info", "apply now: ending the " + toUtf8(sourceLabel(src)) + " session to apply held settings");
    if (src == SrcAirPlay || src == SrcMiracast) {
        disconnectLive();
    } else if (src == SrcNone) {
        g_pinOnlySession = false;  // logic 0.7.9: the PIN-only session ends here, not by kPinEndTimer
        KillTimer(g.hwnd, kPinEndTimer);
        g.status->forceIdle();
        g.sessionActive = false;
        g.announcePending = false;
        g.window->showPin(L"");
        restartServer();
        SetWindowTextW(g.hwnd, g.idleTitle.c_str());
    } else {
        return;  // another source is live: nothing of this kind is held for it
    }
    if (!wasRecording)
        g.window->showToast(fmt(src == SrcMiracast ? S::ApplyNowCast : S::ApplyNowIphone, {unbrokenName(g.name)}), 6000);
}
int g_connectStage = 0;  // 1: the hint is next, 2: giving up is next
long long g_connectAudio0 = 0;  // AirPlay audio packets when it started

long long airplayAudioPackets() {
    const auto* a = dynamic_cast<const GateAudioSink*>(g.audioSink);
    return a ? a->packets.load() : 0;
}

bool connectingNow() {
    const int src = liveSourceNow();
    if (src == SrcNone || g.videoState == StateMirroring || g.videoState == StatePaused || g.status->mirroring())
        return false;
    if (src == SrcAirPlay && g_connectStage && airplayAudioPackets() != g_connectAudio0)
        return false;  // sound is playing: an audio-only AirPlay session (music), not a stuck one
    return !(src == SrcAirPlay && g_pinOnlySession);  // only a PIN so far
}

void armConnectWatch() {
    g_connectAudio0 = airplayAudioPackets();
    g_connectStage = 1;
    SetTimer(g.hwnd, kConnectTimer, kConnectHintMs, nullptr);
}

void connectWatchTick() {
    if (!connectingNow()) {
        KillTimer(g.hwnd, kConnectTimer);
        g_connectStage = 0;
        return;
    }
    const std::wstring who = liveName(true);
    if (g_connectStage == 1) {
        g_connectStage = 2;
        SetTimer(g.hwnd, kConnectTimer, kConnectGiveUpMs - kConnectHintMs, nullptr);
        g.window->showToast(fmt(S::ConnectSlow, {who}), 8000);
        return;
    }
    KillTimer(g.hwnd, kConnectTimer);
    g_connectStage = 0;
    const int src = liveSourceNow();
    g.log->write("warn", "connecting to " + toUtf8(who) + " (" + toUtf8(sourceLabel(src)) + ") for " +
                             std::to_string(kConnectGiveUpMs / 1000) + " s without a picture: cancelled");
    disconnectLive();
    if (src == SrcAndroid) g.androidUserStopped = false;  // not the user's choice: auto-reconnect may try again
    g.window->showToast(fmt(S::ConnectTimeout, {who}), 6000);
}

}  // namespace pm_app
