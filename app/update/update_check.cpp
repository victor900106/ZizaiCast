// 自在投影 app: update/update_check.cpp — update（自動更新、本機更新）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "capture/capture.h"
#include "connect/connect.h"
#include "view/view.h"
#include "update/update.h"

namespace pm_app {

// A result of 檢查更新 the user must see: toast, plus a balloon when the
// window is in the tray (the toast is drawn in the hidden window).
void updateResultNote(const std::wstring& text) {
    g.log->write("info", "update check result shown: " + toUtf8(text));
    g.window->showToast(text, 5000);
    if (windowHidden()) trayBalloon(tr(S::AppName), text);
}

void checkForUpdates(bool manual) {
    if (manual) {  // 檢查更新: a newer installer in 安裝檔 answers at once
        scanLocalUpdates();
        if (offerLocal()) {
            g.updateNotified.clear();
            announceUpdate(true);
            return;
        }
    }
    const std::string url = g.settings.effectiveUpdateUrl();
    if (url.empty()) {
        if (manual) updateResultNote(tr(S::UpdDisabled));
        else announceUpdate();  // a local installer may still be on offer
        return;
    }
    if (g.updateChecking) {
        g.updateManual |= manual;
        return;
    }
    g.updateChecking = true;
    g.updateManual = manual;
    if (manual) g.window->showToast(tr(S::UpdChecking));
    g.log->write("info", "update check: " + url);
    const HWND h = g.hwnd;
    std::thread([h, url]() {
        auto* r = new UpdateResult;
        r->ok = pm::update::fetchManifest(toWide(url), r->manifest, r->error);
        if (!PostMessageW(h, WM_PM_UPDATE, UpdChecked, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}
bool updateBusy() { return liveSourceNow() != SrcNone || recording() || g.status->holding(); }

// The 「有新版本」 dialog for the offer (manifest or 安裝檔). `user`: opened by a
// click (menus, toolbar, idle pill, balloon, 檢查更新) -> takes the focus; an
// automatic prompt only does when the app window is in front.
void openUpdateDialog(bool user) {
    const std::string ver = offerVersion();
    if (ver.empty()) {
        if (user) bringToFront();
        return;
    }
    g.updatePending = false;
    const bool local = offerLocal();
    const pm::update::Manifest& m = local ? g.localInfo : g.update;
    pm::ui::UpdatePanel::Info info;
    info.current = toWide(kAppVersion);
    info.version = toWide(ver);
    info.local = local;
    info.date = toWide(m.date);
    info.size = local ? g.localSize : m.size;
    for (const std::string& c : m.changesZh) info.changesZh.push_back(toWide(c));
    for (const std::string& c : m.changesEn) info.changesEn.push_back(toWide(c));
    for (const std::string& c : m.changesJa) info.changesJa.push_back(toWide(c));
    for (const std::string& c : m.changesKo) info.changesKo.push_back(toWide(c));
    info.notes = toWide(m.notes);
    info.icon = g.iconBig;
    info.onInstall = [ver]() {
        g.log->write("info", "update dialog: install v" + ver);
        if (offerVersion() == ver) installUpdate();
    };
    info.onLater = [ver]() {  // no automatic prompt for 24 h (or until the next start)
        g.log->write("info", "update dialog: later v" + ver + " (snoozed 24 h)");
        g.snoozeVersion = ver;
        g.snoozeUntilMs = nowMs() + kUpdateSnoozeMs;
        g.updateNotified.clear();  // a check after the snooze may prompt again
    };
    info.onSkip = [ver]() {
        g.log->write("info", "update dialog: skip v" + ver + " (skip_version written)");
        g.settings.skipVersion = ver;
        saveSettings();
        g.window->showToast(fmt(S::UpdSkipped, {toWide(ver)}), 5000);
    };
    const bool activate = user || GetForegroundWindow() == g.hwnd;
    g.log->write("info", "update dialog: v" + ver + (user ? " (opened by the user)" : " (automatic)") +
                             (local ? ", local installer" : ", manifest") + ", " +
                             std::to_string(m.changesZh.size()) + " zh / " + std::to_string(m.changesEn.size()) +
                             " en changes");
    if (!g.updatePanel.open(g.hwnd, std::move(info), activate)) g.log->write("warn", "update dialog could not be created");
}

// After every message (appProc): a deferred prompt opens once the mirror /
// recording has ended and the window is on screen.
void maybeShowDeferredUpdate() {
    if (!g.updatePending || g.quitting || g.updatePanel.isOpen() || windowHidden() || updateBusy()) return;
    if (offerVersion().empty()) {
        g.updatePending = false;
        return;
    }
    g.log->write("info", "update dialog: deferred prompt now (mirroring ended / window shown)");
    openUpdateDialog(false);
}

// The offer changed or a check finished: the idle pill / menus / toolbar item
// follow the offer (refreshIdleHints, refreshToolbar); then, once per version
// per run, a prompt. `manual` (檢查更新) always shows the dialog. Automatic:
// not for skip_version, not while snoozed (稍後提醒), not before the first
// check (3 s after start); window in front and idle -> the dialog; phone live /
// recording -> toolbar + menu item, toast and balloon, dialog when it ends;
// window in the tray -> balloon 「點這裡看更新內容」 (click = dialog).
void announceUpdate(bool manual) {
    refreshIdleHints();
    const std::string ver = offerVersion();
    if (ver.empty()) {
        g.updatePending = false;
        g.updatePanel.close();
        return;
    }
    if (g.updatePanel.isOpen() && g.updatePanel.version() != toWide(ver)) openUpdateDialog(false);  // newer offer
    if (!manual) {
        if (!g.updatePromptReady || g.updateNotified == ver) return;
        if (ver == g.settings.skipVersion) {
            g.updateNotified = ver;
            g.log->write("info", "update offered: " + ver + " skipped (skip_version), no prompt");
            return;
        }
        if (ver == g.snoozeVersion && nowMs() < g.snoozeUntilMs) {
            g.log->write("info", "update offered: " + ver + " snoozed (later), no prompt");
            return;
        }
    }
    g.updateNotified = ver;
    const std::wstring v = toWide(ver);
    g.log->write("info", "update offered: " + ver + (offerLocal() ? " (local installer)" : " (manifest)") +
                             (manual ? ", manual check" : ""));
    const bool hidden = windowHidden(), busy = updateBusy();
    if (manual || (!hidden && !busy)) {
        openUpdateDialog(manual);
        return;
    }
    g.updatePending = true;
    g.log->write("info", std::string("update dialog deferred: ") + (busy ? "phone live / recording" : "window hidden"));
    if (!hidden) g.window->showToast(fmt(S::UpdAvailable, {v}), 5000);
    trayBalloon(fmt(S::UpdBalloonTitle, {tr(S::AppName), v}), tr(S::UpdBalloonClick), true);
}

void onUpdateChecked(const UpdateResult& r) {
    g.updateChecking = false;
    const bool manual = g.updateManual;
    g.updateManual = false;
    if (!r.ok) {
        g.log->write("warn", std::string("update check failed") + (manual ? " (manual)" : " (automatic, silent)") + ": " +
                                 r.error);
        if (manual) updateResultNote(tr(S::UpdCheckFail));
        else announceUpdate();  // a local installer may still be on offer
        return;
    }
    const int cmp = pm::update::compareVersions(r.manifest.version, kAppVersion);
    g.log->write("info", "update manifest: version " + r.manifest.version + " (running " + kAppVersion + ")" +
                             (cmp > 0 ? " -> update available" : ""));
    if (cmp <= 0) {
        g.update = {};
        if (manual && offerLocal()) {  // nothing newer online, but in 安裝檔
            g.updateNotified.clear();
            announceUpdate(true);
        } else {
            if (manual) updateResultNote(fmt(S::UpdLatest, {toWide(kAppVersion)}));
            announceUpdate();
        }
        return;
    }
    g.update = r.manifest;
    if (manual) g.updateNotified.clear();  // asked for it: say it again
    announceUpdate(manual);
}

// Recording while 更新到 is chosen: a small themed confirm (the custom menu)
// over the window; the recording is saved before the app quits.
bool confirmUpdateWhileRecording(const std::wstring& v) {
    std::vector<MenuItem> items;
    items.push_back(MenuItem::caption(tr(S::ConfirmRecTitle)));
    items.push_back(MenuItem::note(fmt(S::ConfirmRecNote, {tr(S::AppName)})));
    MenuItem ok = MenuItem::command(CmdInstallUpdate, fmt(S::ConfirmRecOk, {v}), kIcoUpdate);
    ok.bold = true;
    items.push_back(std::move(ok));
    items.push_back(MenuItem::command(1, tr(S::Cancel), kIcoExit));
    POINT pt{};
    if (IsWindowVisible(g.hwnd) && !IsIconic(g.hwnd)) {
        RECT r{};
        GetWindowRect(g.hwnd, &r);
        pt = {(r.left + r.right) / 2 - 120, (r.top + r.bottom) / 2 - 60};
    } else {
        GetCursorPos(&pt);
    }
    SetForegroundWindow(g.hwnd);
    pm::ui::MenuOptions o;
    o.selectFirst = true;
    const UINT cmd = pm::ui::trackMenu(g.hwnd, items, pt, o);
    PostMessageW(g.hwnd, WM_NULL, 0, 0);
    g.log->write("info", std::string("update while recording: ") + (cmd == CmdInstallUpdate ? "confirmed" : "cancelled"));
    return cmd == CmdInstallUpdate;
}

}  // namespace pm_app
