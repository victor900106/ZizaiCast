// 自在投影 app: view/options.cpp — view（視窗動作、檢視選項）。
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

// ---- options (tray menu, context menu and idle screen stay in sync) -------
void refreshIdleOptions() {
    std::vector<pm::VideoWindow::IdleOption> opts;
    opts.push_back({OptAutostart, tr(S::OptAutostart), g.autostart});
    opts.push_back({OptPin, tr(S::OptPin), g.settings.requirePin});
    const HWND h = g.hwnd;
    // The callback runs inside the window's click handling: only post, so
    // that setIdleOptions() is never re-entered from its own callback.
    g.window->setIdleOptions(std::move(opts), [h](int id, bool checked) {
        PostMessageW(h, WM_PM_OPTION, static_cast<WPARAM>(id), checked ? 1 : 0);
    });
}

void setAutostartOption(bool on) {
    if (setAutostart(on)) {
        g.autostart = on;
        g.window->showToast(tr(on ? S::AutostartOn : S::AutostartOff));
    } else {
        g.window->showToast(tr(S::AutostartFail));
    }
    g.autostart = !readAutostart().empty();
    refreshIdleOptions();
}

void setPinOption(bool on) {
    if (on != g.settings.requirePin) {
        g.settings.requirePin = on;
        saveSettings();
        if (g.sessionActive) {
            g.restartPending = true;
            showDeferred(tr(S::PinDeferred));
        } else {
            restartServer();
            g.window->showToast(tr(on ? S::PinOn : S::PinOff));
        }
    }
    refreshIdleOptions();
}

// 設定 ▸ 實驗：只提供螢幕鏡像 (A/B test, 0.7.6). Hidden: the row shows only with
// Shift held while the menu opens, in --dev, or while the test is on (so it can
// be turned back off). Fixed strings (zh-TW + English): it is not a product option.
bool audioAdvertItemVisible() {
    return g.dev || g.settings.advertiseAudio != 1 || GetKeyState(VK_SHIFT) < 0;
}

void toggleAudioAdvertAB() {
    g.settings.advertiseAudio = g.settings.advertiseAudio == 1 ? 0 : 1;
    saveSettings();
    g.log->write(g.settings.advertiseAudio == 1 ? "info" : "warn",
                 "[audio-advert] airplay_advertise_audio set to " + std::to_string(g.settings.advertiseAudio) +
                     (g.settings.advertiseAudio == 1 ? " (also an AirPlay speaker, default)"
                                                      : " (A/B test: Screen Mirroring only, no audio-only AirPlay)") +
                     (g.sessionActive ? "; applies when this mirroring session ends" : "; restarting the AirPlay server"));
    const bool on = g.settings.advertiseAudio != 1;
    if (g.sessionActive) {
        g.restartPending = true;
        g.window->showToast(on ? L"實驗已開啟：投影結束後套用 (A/B test on after this session)"
                               : L"實驗已關閉：投影結束後套用 (A/B test off after this session)");
    } else {
        restartServer();
        g.window->showToast(on ? L"實驗已開啟：只提供螢幕鏡像 (A/B test on)"
                               : L"實驗已關閉：恢復預設 (A/B test off)");
    }
}

const wchar_t* qualityLabel(int q) {
    return tr(q == 0 ? S::QualityStdLabel : q == 2 ? S::QualityMaxLabel : S::QualityHighLabel);
}

void setQuality(int q) {
    if (q < 0 || q > 2 || q == g.settings.quality) return;
    if (q > 0 && !g.hevc) {
        g.window->showToast(tr(S::NeedHevc));
        return;
    }
    g.settings.quality = q;
    saveSettings();
    if (g.sessionActive) {
        g.restartPending = true;
        showDeferred(tr(S::QualityDeferred));
    } else {
        restartServer();
        g.window->showToast(fmt(S::QualityToast, {qualityLabel(q)}));
    }
}


// Audio path latency the picture should wait for: jitter buffer + device buffer.
int audioLatencyMs() {
    if (!g.audio) return 0;
    const pm::AudioStats st = g.audio->stats();
    return static_cast<int>(st.bufferMs + st.deviceBufferMs + 0.5);
}

void applySyncMode() {
    if (g.settings.avSync) {
        g.window->setSyncMode(true, audioLatencyMs());
        SetTimer(g.hwnd, kSyncTimer, 1000, nullptr);
    } else {
        KillTimer(g.hwnd, kSyncTimer);
        g.window->setSyncMode(false, 0);
    }
}

void setAvSync(bool on) {
    if (on == g.settings.avSync) return;
    g.settings.avSync = on;
    saveSettings();
    g.log->write("info", std::string("av sync ") + (on ? "on" : "off"));
    applySyncMode();
    g.window->showToast(tr(on ? S::AvSyncOn : S::AvSyncOff));
}

void setTopmost(bool on) {
    g.settings.topmost = on;
    saveSettings();
    applyTopmost();
    g.window->showToast(tr(on ? S::TopmostOn : S::TopmostOff));
}
void setCloseButton(int c, bool toast) {
    if (c < 0 || c > 2) return;
    const bool changed = c != g.settings.closeButton;
    g.settings.closeButton = c;
    saveSettings();
    g.log->write("info", std::string("close button: ") + Settings::kCloseKeys[c] + (changed ? "" : " (unchanged)"));
    if (toast) g.window->showToast(fmt(S::CloseActToast, {tr(kCloseActNames[c])}));
}

void setTakeover(bool keep) {
    if (keep == g.settings.takeoverKeep) return;
    g.settings.takeoverKeep = keep;
    g_takeoverNew = !keep;  // Miracast / Android arbitration: at once
    saveSettings();
    const wchar_t* what = tr(keep ? S::TakeoverKeepToast : S::TakeoverNewToast);
    if (g.sessionActive) {
        g.restartPending = true;
        showDeferred(fmt(S::AppliesAfter, {what}));
    } else {
        restartServer();
        g.window->showToast(what);
    }
}

}  // namespace pm_app
