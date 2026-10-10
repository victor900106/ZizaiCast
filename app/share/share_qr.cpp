// 自在投影 app: share/share_qr.cpp — share（傳到手機）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "share/share.h"
#include "share/share_internal.h"

namespace pm_app {

// Every startShareQr() is a new generation; ShareAccess / ShareExpired carry
// theirs, and those of an earlier share (posted just before 再傳一次 or a
// network rebind) are dropped. UI thread only.
unsigned g_shareGen = 0;

std::wstring shareFilesLabel(const std::vector<fs::path>& files) {
    if (files.empty()) return {};
    if (files.size() == 1) return files[0].filename().wstring();
    return fmt(S::SharePanelMany, {std::to_wstring(files.size())});
}

// An Android phone mirroring over adb (the --test-source android fake counts).
bool androidPushPath() {
    if (!androidLive()) return false;
    if (g.testAndroid) return true;
    return g.android && g.androidOk && g.android->state() == pm::AndroidSource::State::Mirroring;
}

void copyToClipboard(const std::wstring& text) {
    if (g.testOffscreen) {  // never touch the user's clipboard from a test
        g.log->write("info", "share: copy link (test-offscreen: clipboard left alone)");
        return;
    }
    if (!OpenClipboard(g.hwnd)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(mem), text.c_str(), bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
}

bool liveShareOn() { return g.share.live() && !g.share.expired(); }

void markSent(const std::vector<fs::path>& files) {
    for (App::Capture& c : g.captures)
        if (std::find(files.begin(), files.end(), c.file) != files.end()) c.sent = true;
}

std::vector<fs::path> unsentCaptures() {
    std::vector<fs::path> v;
    std::error_code ec;
    for (const App::Capture& c : g.captures)
        if (!c.sent && fs::exists(c.file, ec)) v.push_back(c.file);
    return v;
}
size_t unsentCount() { return unsentCaptures().size(); }

// The QR panel for the running share (opened, or refreshed and brought up).
void openSharePanel() {
    const std::string url = g.share.url();
    if (url.empty()) return;
    std::vector<uint8_t> qr;
    int qrSize = 0;
    pm::share::renderQr(url, 1, 4, qr, qrSize);
    const bool live = g.share.live();
    pm::ui::SharePanel::Callbacks cb;
    cb.onStop = [live]() {
        if (live && g.settings.autoShare) {  // 停止自動傳送: the option goes off with the page
            setAutoShare(false);
            return;
        }
        if (g.share.running()) {
            g.share.stop();
            g.window->showToast(tr(S::ShareStopped));
        }
    };
    cb.onAgain = []() {
        std::vector<fs::path> again;
        for (const std::wstring& f : g.share.files()) again.push_back(f);
        startShareQr(again, g.share.live());
    };
    cb.onCopy = []() {
        copyToClipboard(toWide(g.share.url()));
        g.window->showToast(tr(S::ShareCopied));
    };
    if (live) cb.onHide = []() { g.log->write("info", "share: live panel hidden (still sharing)"); };
    if (!g.sharePanel.open(g.hwnd, std::move(cb))) {
        g.log->write("warn", "share panel could not be created");
        if (!live) g.share.stop();
        return;
    }
    std::vector<fs::path> served;
    for (const std::wstring& f : g.share.files()) served.push_back(f);
    g.sharePanel.setShare(url, qr, qrSize, live && !served.empty() ? served.back().filename().wstring() : shareFilesLabel(served),
                          g.share.secondsLeft());
    g.sharePanel.setLive(live, static_cast<int>(served.size()));
}

// Adds files to the running live page (the phone shows them within a second).
void addToLive(const std::vector<fs::path>& files, bool showPanel) {
    int added = 0;
    for (const fs::path& f : files)
        if (g.share.addFile(f.wstring()) >= 0) ++added;
    markSent(files);
    g.log->write("info", "share: " + std::to_string(added) + " file(s) added to the live page (" +
                             std::to_string(g.share.fileCount()) + " in all)");
    if (showPanel) openSharePanel();
    else if (g.sharePanel.isOpen()) {
        g.sharePanel.setLive(true, g.share.fileCount());
        if (!files.empty()) g.sharePanel.setFiles(files.back().filename().wstring());
    }
    if (added > 0) g.window->showToast(tr(S::ShareLiveAdded), 2500);
}

// ttlSeconds > 0: this lifetime instead of the default (a rebind keeps the
// time left). showPanel false: the QR panel is refreshed only if it is open.
bool startShareQr(const std::vector<fs::path>& files, bool live, int ttlSeconds, bool showPanel) {
    pm::share::Server::Options o;
    if (g.server) o.preferred = g.server->advertisedInterfaces();  // where phones already see us
    o.bindIp = g.shareBindTest;
    o.live = live;
    if (live) o.ttlSeconds = kLiveShareHours * 3600;
    if (g.shareTtlTest > 0) o.ttlSeconds = g.shareTtlTest;
    if (ttlSeconds > 0) o.ttlSeconds = ttlSeconds;
    o.english = pm::i18n::en();
    o.lang = static_cast<int>(pm::i18n::lang());
    o.appName = tr(S::AppName);
    const pm::ui::Colors c = pm::ui::currentColors();
    o.palette = {pm::VideoWindow::themeSwatch(kThemes[g.settings.theme & 3])[0], c.cardTop, c.cardBottom, c.fg, c.dim,
                 c.accent, c.light};
    const HWND h = g.hwnd;
    Log* log = g.log;
    // The running share's threads call these: stop it before they are replaced.
    g.share.stop();
    const unsigned gen = ++g_shareGen;
    g.share.log = [log](const std::string& line) { log->write("share", line); };
    g.share.onAccess = [h, gen](const pm::share::Server::Access& a) {
        auto* p = new ShareAccessMsg{a, gen};
        if (!PostMessageW(h, WM_PM_SHARE, ShareAccess, reinterpret_cast<LPARAM>(p))) delete p;
    };
    g.share.onExpired = [h, gen]() { PostMessageW(h, WM_PM_SHARE, ShareExpired, static_cast<LPARAM>(gen)); };
    std::vector<std::wstring> paths;
    for (const fs::path& f : files) paths.push_back(f.wstring());
    std::string err;
    if (!g.share.start(paths, o, &err)) {
        g.log->write("warn", "share: " + err);
        g.window->showToast(err == "no readable file" && !files.empty()
                                ? fmt(S::ShareFileGone, {files[0].filename().wstring()})
                                : std::wstring(tr(S::ShareStartFail)),
                            4000);
        return false;
    }
    markSent(files);
    if (showPanel || g.sharePanel.isOpen()) openSharePanel();
    SetTimer(g.hwnd, kShareNetTimer, kShareNetCheckMs, nullptr);
    return true;
}

// 傳到手機 after sleep / a network change: the server is bound to one LAN
// address. If that address is gone (other Wi-Fi, new DHCP lease) but the PC is
// on a LAN again, the same files are shared again (same live flag, the time
// left) on the current address and the QR / link refreshed; the old link is
// dead anyway. `force` (--dev test): as if the address were gone.
void checkShareNetwork(bool force) {
    if (!g.share.running()) {
        KillTimer(g.hwnd, kShareNetTimer);
        return;
    }
    if (g.share.expired() || (!g.shareBindTest.empty() && !force)) return;  // tests bind 127.0.0.1 on purpose
    const std::string ip = g.share.ip();
    const std::vector<pm::share::LanInterface> lan = pm::share::lanInterfaces();
    if (!force) {
        for (const auto& l : lan)
            if (l.ip == ip) return;  // still here
        if (lan.empty()) return;     // no network yet (Wi-Fi reconnecting): look again later
    }
    std::vector<fs::path> files;
    for (const std::wstring& f : g.share.files()) files.push_back(f);
    const bool live = g.share.live();
    const int left = std::max(g.share.secondsLeft(), 60);
    std::string now;
    for (const auto& l : lan) now += (now.empty() ? "" : ", ") + l.name + "=" + l.ip;
    g.log->write("info", "share: " + ip + " is no longer a LAN address (now: " + (now.empty() ? "-" : now) +
                             "); sharing " + std::to_string(files.size()) + " file(s) again for " + std::to_string(left) + " s");
    if (startShareQr(files, live, left, false)) {
        g.window->showToast(tr(S::ShareLinkChanged), 6000);
        if (!IsWindowVisible(g.hwnd) && !g.testOffscreen) trayBalloon(tr(S::AppName), tr(S::ShareLinkChanged));
    }
}

// The QR path for `files`: onto the live page when one runs, else a new
// share (live while 自動傳到手機 is on).
void sendViaQr(const std::vector<fs::path>& files, bool showPanel) {
    if (liveShareOn()) {
        addToLive(files, showPanel);
        return;
    }
    startShareQr(files, g.settings.autoShare);
}

}  // namespace pm_app
