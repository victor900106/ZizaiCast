// 自在投影 app: connect/android_control.cpp — Android 操控 (0.8.3), see android_control.h.

#include "connect/android_control.h"

#include "connect/connect.h"
#include "core/base/app_state.h"
#include "core/base/base.h"
#include "menus/menus.h"

namespace pm_app {

namespace {

constexpr wchar_t kIcoClipboard = 0xE77F;  // Paste
constexpr wchar_t kIcoScreenOff = 0xE7E8;  // PowerButton
constexpr wchar_t kIcoAwake = 0xE706;      // Brightness
constexpr wchar_t kIcoUsb = 0xE88E;        // USB

std::thread g_usbThread;
std::atomic<bool> g_usbBusy{false};
bool g_usbWatch = false;  // UI thread: a USB attempt is running, its states are shown as toasts

// The AndroidSource to talk to: the real one (also for the --test-source
// android fake phone, whose calls the stub / droid-fix code just log).
pm::AndroidSource* source() { return g.android; }

void toastOption(S name, bool on) {
    std::wstring t = fmt(on ? S::AndOptOn : S::AndOptOff, {tr(name)});
    if (!androidLive()) t += tr(S::AndOptNext);
    g.window->showToast(t, 2500);
}

}  // namespace

void androidControlApply(const char* why) {
    pm::AndroidSource* a = source();
    if (!a) return;
    g.log->write("info", std::string("android control (") + why + "): clipboard sync " + (g.settings.androidClipSync ? "on" : "off") +
                             ", screen " + (g.settings.androidScreenOff ? "off" : "on") + ", stay awake " +
                             (g.settings.androidStayAwake ? "on" : "off"));
    a->setClipboardSync(g.settings.androidClipSync);
    a->setStayAwake(g.settings.androidStayAwake);
    if (g.settings.androidScreenOff) a->setPhoneScreenOn(false);
}

void androidControlCommand(UINT cmd) {
    pm::AndroidSource* a = source();
    const bool live = androidLive();
    switch (cmd) {
    case CmdAndClipSync:
        g.settings.androidClipSync = !g.settings.androidClipSync;
        saveSettings();
        if (live && a) a->setClipboardSync(g.settings.androidClipSync);
        toastOption(S::AndClipSync, g.settings.androidClipSync);
        break;
    case CmdAndScreenOff:
        g.settings.androidScreenOff = !g.settings.androidScreenOff;
        saveSettings();
        if (live && a) a->setPhoneScreenOn(!g.settings.androidScreenOff);
        toastOption(S::AndScreenOff, g.settings.androidScreenOff);
        break;
    case CmdAndStayAwake:
        g.settings.androidStayAwake = !g.settings.androidStayAwake;
        saveSettings();
        if (live && a) a->setStayAwake(g.settings.androidStayAwake);
        toastOption(S::AndStayAwake, g.settings.androidStayAwake);
        break;
    case CmdAndUsb: {
        if (!a || (!g.androidOk && !g.testAndroid)) {
            g.window->showToast(tr(S::NoteNoTools), 3000);
            return;
        }
        if (g_usbBusy.exchange(true)) return;  // one at a time
        if (g_usbThread.joinable()) g_usbThread.join();
        g.log->write("info", "android: connect over USB");
        g.window->showToast(tr(S::AndUsbTrying), 8000);
        // connectUsb() may run adb (devices / forward): off the UI thread.  A
        // phone that comes up arrives as AndroidSource::Events::onConnected.
        pm::VideoWindow* w = g.window;
        g_usbThread = std::thread([a, w] {
            // false: adb not running (the --test-source fake phone) or a pairing / connect is
            // still running; true: started, its progress and result come as states.
            const bool ok = a->connectUsb();
            w->post([ok] {
                g_usbBusy = false;
                g_usbWatch = ok;
                const bool busy = !ok && g.androidOk;
                g.log->write(ok ? "info" : "warn", std::string("android: USB connect ") +
                                                       (ok ? "started" : busy ? "refused: a pairing / connect is running" : "not available"));
                if (!ok) g.window->showToast(tr(busy ? S::AndUsbBusy : S::AndUsbNone), 5000);
            });
        });
        break;
    }
    }
}

void androidUsbState(pm::AndroidSource::State s, const std::wstring& detail) {
    using St = pm::AndroidSource::State;
    if (!g_usbWatch) return;
    if (s == St::Mirroring || (s == St::Connecting && androidLive())) {  // the phone is here
        g_usbWatch = false;
        return;
    }
    const bool end = s == St::Idle || s == St::Error;
    if (end) g_usbWatch = false;
    if (!detail.empty() && !g.pairPanel.isOpen()) g.window->showToast(moduleText(detail), end ? 6000 : 30000);
}

void appendAndroidControlItems(std::vector<MenuItem>& v) {
    v.push_back(MenuItem::separator());
    v.push_back(MenuItem::caption(tr(S::AndCtrlCaption)));
    v.push_back(checkItem(CmdAndClipSync, tr(S::AndClipSync), kIcoClipboard, g.settings.androidClipSync));
    v.push_back(checkItem(CmdAndScreenOff, tr(S::AndScreenOff), kIcoScreenOff, g.settings.androidScreenOff));
    v.push_back(checkItem(CmdAndStayAwake, tr(S::AndStayAwake), kIcoAwake, g.settings.androidStayAwake));
    MenuItem usb = MenuItem::command(CmdAndUsb, tr(S::AndUsb), kIcoUsb);
    usb.enabled = (g.androidOk || g.testAndroid) && !g_usbBusy.load();
    if (!g.androidOk && !g.testAndroid) usb.right = tr(S::NoteNoTools);
    v.push_back(std::move(usb));
    v.push_back(MenuItem::note(tr(S::AndUsbNote)));
}

void androidControlShutdown() {
    if (g_usbThread.joinable()) g_usbThread.join();
}

}  // namespace pm_app
