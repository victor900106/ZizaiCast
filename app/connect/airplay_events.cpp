// 自在投影 app: connect/airplay_events.cpp — connect（iPhone / Android / Miracast 連線狀態）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "help/help.h"
#include "capture/capture.h"
#include "connect/connect.h"
#include "connect/connect_internal.h"

namespace pm_app {

// ---- events from the AirPlay core -----------------------------------------
// The "connection lost" hold (StateLost): last frame + toast for ~3 s.
void endLostHold() {
    KillTimer(g.hwnd, kLostTimer);
    g.status->releaseHold();
    g.window->setDimmed(false);
}

// After sleep the phone is normally gone, but the core may only notice after
// its feedback timeout. Re-announce on the (possibly new) network and, if no
// new video arrives shortly, drop the frozen picture for the idle screen.
void onResume() {
    const long long now = nowMs();
    if (now - g.lastResumeMs < 5000) return;  // RESUMEAUTOMATIC + RESUMESUSPEND
    g.lastResumeMs = now;
    g.log->write("info", "resumed from sleep: refreshing network");
    if (g.server) {
        g.server->refreshNetwork();
        std::string ifs;
        for (const std::string& i : g.server->advertisedInterfaces()) ifs += (ifs.empty() ? "" : ", ") + i;
        g.log->write("info", "advertised interfaces: " + (ifs.empty() ? std::string("(none yet)") : ifs));
    }
    if (g.share.running()) SetTimer(g.hwnd, kShareNetTimer, 3000, nullptr);  // the network settles first
    if (g.status->mirroring() || g.status->holding()) {
        g.resumeFramesIn = g.window->stats().framesIn;
        SetTimer(g.hwnd, kResumeTimer, kResumeCheckMs, nullptr);
    }
}

void checkAfterResume() {
    KillTimer(g.hwnd, kResumeTimer);
    if (g.status->holding()) {
        endLostHold();
    } else if (g.status->mirroring() && g.videoState != StatePaused &&
               g.window->stats().framesIn == g.resumeFramesIn) {
        g.log->write("info", "no video since resume: back to the idle screen");
        g.status->forceIdle();
        g.window->showToast(tr(S::LostIphone));
    }
}

// The phone named in the title / toasts: 「Galaxy S24（Android）」.
void setPeer(int source, const std::wstring& name) {
    g.liveSource = source;
    g.sourceName = name;
    g.peerName = fmt(S::PeerFmt, {name, sourceLabel(source)});
}

// Miracast / Android owns the window (not AirPlay, not free).
bool otherSourceLive() {
    const int a = g_active.load();
    return a == SrcMiracast || a == SrcAndroid;
}

// An iPhone wants to mirror while a Miracast / Android phone is shown:
// takeover=keep refuses it (restarting the AirPlay server drops the
// connection), takeover=new hands it the window now. True = refused.
bool refuseAirPlay(const std::wstring& who) {
    if (!otherSourceLive()) return false;
    if (!g.settings.takeoverKeep) {
        claimSource(SrcAirPlay, true);
        return false;
    }
    static long long lastToast = -100000;
    g.log->write("info", "AirPlay client \"" + toUtf8(who) + "\" refused: " + toUtf8(sourceLabel(g_active.load())) +
                             " is live (takeover=keep)");
    if (nowMs() - lastToast > 5000) {
        lastToast = nowMs();
        g.window->showToast(fmt(S::RefusedIphone, {g.sourceName, who}));
    }
    g.sessionActive = false;
    restartServer();
    return true;
}

// The AirPlay session so far is only a PIN on screen (no SETUP yet). The core
// sends no disconnect for it: a PIN cancelled on the iPhone or timed out only
// hides the PIN, which also happens right before a paired phone's SETUP.
bool g_pinOnlySession = false;

void pinSessionEnded() {
    KillTimer(g.hwnd, kPinEndTimer);
    if (!std::exchange(g_pinOnlySession, false) || !g.sessionActive || g_active.load() != SrcNone) return;
    g.log->write("info", "AirPlay: PIN cancelled or timed out without a connection: session over");
    g.sessionActive = false;
    g.announcePending = false;
    SetWindowTextW(g.hwnd, g.idleTitle.c_str());
    if (g.restartPending) restartServer();  // settings changed while the PIN was up
}

void onEvent(EventKind kind, const std::wstring& text) {
    switch (kind) {
    case EvConnecting: {
        const std::wstring who = text.empty() ? L"iPhone" : text;
        g_pinOnlySession = false;
        KillTimer(g.hwnd, kPinEndTimer);
        if (refuseAirPlay(who)) break;
        endLostHold();
        g.sessionActive = true;
        g.window->showPin(L"");
        g.announcePending = true;
        setPeer(SrcAirPlay, who);
        g.window->setConnecting(g.peerName);
        SetWindowTextW(g.hwnd, fmt(S::TitleConnecting, {tr(S::AppName), g.peerName}).c_str());
        bringToFront();
        armConnectWatch();
        break;
    }
    case EvPin:
        if (!text.empty() && refuseAirPlay(L"iPhone")) break;
        g.window->showPin(text);
        if (!text.empty()) {
            KillTimer(g.hwnd, kPinEndTimer);
            if (!g.sessionActive) g_pinOnlySession = true;
            g.sessionActive = true;  // the PIN request comes before onClientConnecting
            SetWindowTextW(g.hwnd, fmt(S::TitlePinIphone, {tr(S::AppName)}).c_str());
            bringToFront();
        } else if (g_pinOnlySession) {
            SetTimer(g.hwnd, kPinEndTimer, 3000, nullptr);  // paired: SETUP (EvConnecting) comes within a moment
        }
        break;
    case EvTakeover: {
        // Another iPhone took over the receiver (takeover=new). The core has
        // already reset the sinks, which looks like an unexpected loss to
        // StatusVideoSink: end that hold (no dimmed frame, no 連線中斷) at once.
        // A recording of the previous phone is finished (new picture size).
        const bool savedJustNow = nowMs() - g.recordingStoppedAtMs < 1500;
        endLostHold();
        if (g.videoState == StateLost) g.videoState = StateIdle;
        setPeer(SrcAirPlay, text.empty() ? L"iPhone" : text);
        g.lastOrientation = 0;
        const std::wstring msg = fmt(S::TookOver, {g.sourceName});
        g.takeoverAtMs = nowMs();
        if (recording()) stopRecording(fmt(S::RecSavedSuffix, {msg}));
        else g.window->showToast(savedJustNow ? fmt(S::RecSavedSuffix, {msg}) : msg);
        SetWindowTextW(g.hwnd, fmt(S::TitleMirroringName, {tr(S::AppName), g.peerName}).c_str());
        g.announcePending = false;
        bringToFront();
        break;
    }
    case EvDisconnected:
        if (otherSourceLive()) {  // a refused / replaced iPhone: the picture is someone else's
            g.sessionActive = false;
            if (g.restartPending) restartServer();
            break;
        }
        g.status->lostWithoutReset();  // no-op unless still mirroring
        // A hold just started: its StateLost handler saves the recording (with
        // the 連線中斷 toast). Otherwise save it here.
        if (recording() && !g.status->holding())
            stopRecording(fmt(S::MirrorEndedRec, {g.recordingFile.filename().wstring()}));
        g.sessionActive = false;
        g.announcePending = false;
        g.window->showPin(L"");
        SetWindowTextW(g.hwnd, g.idleTitle.c_str());
        g.lastOrientation = 0;
        if (g.restartPending) restartServer();
        break;
    }
}

}  // namespace pm_app
