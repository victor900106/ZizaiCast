// 自在投影 app: connect/sources.cpp — connect（iPhone / Android / Miracast 連線狀態）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "help/help.h"
#include "capture/capture.h"
#include "connect/connect.h"
#include "connect/connect_internal.h"

namespace pm_app {

// ---- sources: arbitration (UI side), Miracast, Android -------------------------
// Back to the idle screen state (title, recording, …) as if the stream had
// ended; the window itself is reset by the caller / source.
void sourceEndedIdle() { SendMessageW(g.hwnd, WM_PM_STATE, StateIdle, 0); }

// A source took the window from another one (claimSource, any thread). The
// old owner is already stopped unless it was AirPlay.
void onSourceTakeover(int now, int prev) {
    if (prev == SrcNone || prev == now) return;
    g.log->write("info", std::string("source ") + toUtf8(sourceLabel(now)) + " took the window from " +
                             toUtf8(sourceLabel(prev)));
    if (recording()) stopRecording(fmt(S::SourceSwitchedRec, {g.recordingFile.filename().wstring()}));
    KillTimer(g.hwnd, kLostTimer);
    g.status->switchSource();
    g.window->setDimmed(false);
    g.lastOrientation = 0;
    if (prev == SrcAndroid) {
        g.window->setPointerHandler(nullptr);
        g.window->setKeyHandler(nullptr);
    }
    if (prev == SrcAirPlay) {  // drop the iPhone (the core has no "disconnect client")
        g.sessionActive = false;
        g.window->showPin(L"");
        restartServer();
    }
}

// Idle-screen hint lines: Miracast 投放 is usable right now?
bool miracastListening() {
    using St = pm::MiracastReceiver::Status;
    if (!g.miracast || !g.settings.miracast || g.miracastUnsupported) return false;
    const int s = g.miracastStatus;
    return s < 0 || s == static_cast<int>(St::Idle) || s == static_cast<int>(St::Connecting) ||
           s == static_cast<int>(St::Connected);
}
MiraWhy miracastWhy() {
    if (g.testWays >= 0) return static_cast<MiraWhy>(std::clamp(g.testWays >> 1, 0, static_cast<int>(MiraOther)));
    if (miracastListening()) return MiraOn;
    if (g.miracastUnsupported) {
        // Which unsupportedReason() (the module's Chinese text, see the
        // table's ModMira* entries) it is.
        const std::wstring& r = g.miracastReason;
        auto is = [&](S id) { return r == tr(id, pm::i18n::Lang::ZhTW); };
        if (is(S::ModMiraReboot)) return MiraReboot;
        if (is(S::ModMiraNeedFeature)) return MiraFeature;
        if (is(S::ModMiraOldWindows)) return MiraOldWindows;
        if (is(S::ModMiraPolicy)) return MiraPolicy;
        if (is(S::ModMiraNoWifi)) return MiraNoWifi;
        return MiraOther;
    }
    if (!g.settings.miracast) return MiraOff;
    return MiraOther;
}

}  // namespace pm_app
