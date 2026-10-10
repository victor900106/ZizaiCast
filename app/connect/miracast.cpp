// 自在投影 app: connect/miracast.cpp — connect（iPhone / Android / Miracast 連線狀態）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "help/help.h"
#include "capture/capture.h"
#include "connect/connect.h"
#include "connect/connect_internal.h"

namespace pm_app {

// ---- Miracast ----
// start() / stop() on a helper thread (WinRT status query + settings apply
// take up to a few hundred ms); events arrive on thread-pool threads.
void miracastApply(bool on) {
    if (g.testNoNetwork) return;
    if (!g.miracast) return;
    if (g.miracastOp.joinable()) g.miracastOp.join();
    pm::MiracastReceiver* m = g.miracast;
    const HWND h = g.hwnd;
    if (!on) {
        if (g_active.load() == SrcMiracast) {  // stop() ends the cast without onDisconnected
            releaseSource(SrcMiracast);
            sourceEndedIdle();
        }
        g.miracastOp = std::thread([m]() { m->stop(); });
        return;
    }
    pm::VideoWindow* w = g.window;
    const std::wstring name = g.name;
    pm::MiracastReceiver::Events ev;
    ev.onStatus = [h, m](pm::MiracastReceiver::Status s, const std::wstring& detail) {
        if (s == pm::MiracastReceiver::Status::Connecting) {
            // Arbitrate here, before the cast's first picture reaches the window.
            const int a = g_active.load();
            if (a != SrcNone && a != SrcMiracast && !g_takeoverNew.load()) {
                m->disconnect();
                postSource(h, MiraRefused, detail);
                return;
            }
            claimSource(SrcMiracast, true);
            postSource(h, MiraConnecting, detail);
            return;
        }
        postSource(h, MiraStatus, std::to_wstring(static_cast<int>(s)) + detail);
    };
    ev.onConnected = [h](const std::wstring& name) { postSource(h, MiraConnected, name); };
    ev.onDisconnected = [h]() { postSource(h, MiraDisconnected); };
    ev.onPin = [h](const std::wstring& pin) { postSource(h, MiraPin, pin); };
    g.miracastOp = std::thread([m, w, h, name, ev]() mutable {
        const bool ok = m->start(name, w, std::move(ev));
        postSource(h, MiraStarted, ok ? L"1" : L"0");
    });
}

void setMiracastOption(bool on) {
    if (g.miracastUnsupported) {
        g.window->showToast(g.miracastReason.empty() ? std::wstring(tr(S::MiracastCantReceive))
                                                     : moduleText(g.miracastReason));
        return;
    }
    g.settings.miracast = on;
    saveSettings();
    g.miracastStatus = -1;
    miracastApply(on);
    refreshIdleHints();
    g.window->showToast(tr(on ? S::MiracastOn : S::MiracastOff));
}

void onMiracastEvent(SourceEvent kind, const std::wstring& text) {
    using St = pm::MiracastReceiver::Status;
    switch (kind) {
    case MiraStatus: {
        const int s = text.empty() ? 0 : text[0] - L'0';
        const std::wstring detail = text.size() > 1 ? text.substr(1) : L"";
        g.log->write("info", "miracast status " + std::to_string(s) + (detail.empty() ? "" : ": " + toUtf8(detail)));
        g.miracastStatus = s;
        if (s == static_cast<int>(St::Unavailable)) {
            g.miracastUnsupported = true;
            g.miracastReason = detail;
        } else if (s == static_cast<int>(St::Disabled)) {
            g.miracastReason = detail;
        } else {
            g.miracastReason.clear();
        }
        refreshIdleHints();
        // A pending connection (ConnectionPending: the window was claimed and
        // shows 連線中) that went back to listening without a cast or a
        // Disconnected event: free the window once that has lasted a moment.
        if (g_active.load() == SrcMiracast && g.videoState != StateMirroring &&
            (s == static_cast<int>(St::Idle) || s == static_cast<int>(St::Disabled) ||
             s == static_cast<int>(St::Unavailable)))
            SetTimer(g.hwnd, kMiraIdleTimer, 1500, nullptr);
        break;
    }
    case MiraStarted:
        g.log->write(text == L"1" ? "info" : "warn", text == L"1" ? "miracast receiver listening" : "miracast receiver did not start");
        break;
    case MiraRefused: {
        const std::wstring who = text.empty() ? std::wstring(tr(S::AndroidPhone)) : moduleText(text);
        g.log->write("info", "miracast cast from \"" + toUtf8(who) + "\" refused (takeover=keep)");
        g.window->showToast(fmt(S::RefusedMiracast, {g.sourceName, who}));
        break;
    }
    case MiraConnecting: {
        KillTimer(g.hwnd, kMiraIdleTimer);  // the cast is going on after all
        if (g_active.load() != SrcMiracast) break;
        endLostHold();
        g.announcePending = true;
        setPeer(SrcMiracast, text.empty() ? std::wstring(tr(S::AndroidPhone)) : moduleText(text));
        g.window->setConnecting(g.peerName);
        SetWindowTextW(g.hwnd, fmt(S::TitleConnecting, {tr(S::AppName), g.peerName}).c_str());
        bringToFront();
        armConnectWatch();
        break;
    }
    case MiraConnected:
        KillTimer(g.hwnd, kMiraIdleTimer);
        if (g_active.load() != SrcMiracast) {  // lost the window meanwhile
            if (g.miracast) g.miracast->disconnect();
            break;
        }
        if (!text.empty()) setPeer(SrcMiracast, moduleText(text));
        g.announcePending = true;
        SendMessageW(g.hwnd, WM_PM_STATE, StateMirroring, 0);
        break;
    case MiraDisconnected:
        KillTimer(g.hwnd, kMiraIdleTimer);
        if (g_active.load() == SrcMiracast) {
            releaseSource(SrcMiracast);
            g.window->showPin(L"");
            // Ended before its first picture (PIN / negotiation): the pump had
            // nothing to reset, so the 連線中 screen would stay up.
            if (g.videoState != StateMirroring) g.window->onReset();
            sourceEndedIdle();
        }
        if (g.miracastRenamePending) {  // display name changed during the cast
            g.miracastRenamePending = false;
            if (g.settings.miracast && !g.miracastUnsupported) {
                miracastApply(false);
                miracastApply(true);
            }
        }
        break;
    case MiraPin:
        if (!text.empty() && g_active.load() != SrcMiracast && g_active.load() != SrcNone) break;
        g.window->showPin(text);
        if (!text.empty()) {
            KillTimer(g.hwnd, kConnectTimer);  // the user is typing the PIN on the phone
            SetWindowTextW(g.hwnd, fmt(S::TitlePinPhone, {tr(S::AppName)}).c_str());
            bringToFront();
        } else if (connectingNow()) {
            armConnectWatch();
        }
        break;
    default: break;
    }
}

// kMiraIdleTimer: the Miracast receiver has been back to listening (or off)
// for 1.5 s while a pending cast still held the window. Give it back.
void miracastPendingGone() {
    KillTimer(g.hwnd, kMiraIdleTimer);
    if (g_active.load() != SrcMiracast || g.videoState == StateMirroring) return;
    const std::wstring who = g.sourceName.empty() ? std::wstring(tr(S::AndroidPhone)) : g.sourceName;
    g.log->write("warn", "miracast: pending cast from \"" + toUtf8(who) + "\" went back to listening: window freed");
    releaseSource(SrcMiracast);
    if (g.miracast) g.miracast->disconnect();  // no-op without a connection
    g.window->showPin(L"");
    g.window->onReset();  // the 連線中 screen
    g.announcePending = false;
    sourceEndedIdle();
    g.liveSource = SrcNone;
    SetWindowTextW(g.hwnd, g.idleTitle.c_str());
    g.window->showToast(fmt(S::MiraNotCompleted, {who}), 5000);
}

}  // namespace pm_app
