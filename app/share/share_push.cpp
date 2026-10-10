// 自在投影 app: share/share_push.cpp — share（傳到手機）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "share/share.h"
#include "share/share_internal.h"

namespace pm_app {

// Android over adb: queued, one push at a time on AndroidSource's thread
// (mirroring goes on). A file already waiting is not queued twice.
void queuePush(const std::vector<fs::path>& files) {
    for (const fs::path& f : files)
        if (std::find(g.pushQueue.begin(), g.pushQueue.end(), f) == g.pushQueue.end()) g.pushQueue.push_back(f);
    markSent(files);
    g.log->write("info", "share: " + std::to_string(files.size()) + " file(s) → Android gallery (adb)" +
                             (g.pushing ? ", queued (" + std::to_string(g.pushQueue.size()) + " waiting)" : ""));
    if (g.pushing) {
        if (!g.settings.autoShare)
            g.window->showToast(fmt(S::ShareQueued, {std::to_wstring(g.pushQueue.size())}), 2500);
        return;
    }
    pushNext();
}

void shareFiles(std::vector<fs::path> files) {
    std::error_code ec;
    files.erase(std::remove_if(files.begin(), files.end(),
                               [&](const fs::path& f) { return f.empty() || !fs::exists(f, ec); }),
                files.end());
    if (files.empty()) {
        g.window->showToast(tr(S::ShareNothing));
        return;
    }
    g.shareChip.hide();
    if (androidPushPath()) {
        queuePush(files);
        return;
    }
    g.log->write("info", "share: " + std::to_string(files.size()) + " file(s) → QR page");
    sendViaQr(files, true);
}

// Android auto path: one file at a time (AndroidSource pushes on its own thread).
void pushNext() {
    while (!g.pushQueue.empty()) {
        const fs::path f = g.pushQueue.front();
        g.pushQueue.erase(g.pushQueue.begin());
        if (g.testAndroid && !g.testPush) {  // --test-source android: the fake phone has no adb
            g.log->write("info", "test android: would push " + toUtf8(f.wstring()) + " → " +
                                     pm::AndroidSource::galleryPath(f.filename().wstring()));
            g.window->showToast(fmt(S::ShareTestPushed, {f.filename().wstring()}), 3000);
            continue;
        }
        const HWND h = g.hwnd;
        pm::AndroidSource* src = g.testPush ? g.testPush.get() : g.android;
        const bool started = src->pushToGallery(f.wstring(), [h, f](const pm::AndroidSource::PushResult& r) {
            auto* p = new PushDone{f, r};
            if (!PostMessageW(h, WM_PM_SHARE, SharePushDone, reinterpret_cast<LPARAM>(p))) delete p;
        });
        if (!started) {  // no adb device after all: QR for this and the rest
            g.log->write("warn", "share: pushToGallery refused; QR instead");
            std::vector<fs::path> rest{f};
            rest.insert(rest.end(), g.pushQueue.begin(), g.pushQueue.end());
            g.pushQueue.clear();
            g.pushing = false;
            g.window->showToast(tr(S::SharePushFail), 4000);
            sendViaQr(rest, true);
            return;
        }
        g.pushing = true;
        g.log->write("info", "share: pushing " + toUtf8(f.filename().wstring()) + " (" +
                                 std::to_string(g.pushQueue.size()) + " more waiting)");
        if (!g.settings.autoShare) g.window->showToast(fmt(S::SharePushing, {f.filename().wstring()}), 60000);
        return;
    }
    g.pushing = false;
}

void onPushDone(const PushDone& d) {
    g.pushing = false;
    const auto& r = d.result;
    if (r.ok) {
        g.log->write("info", "share: pushed " + toUtf8(d.file.filename().wstring()) + " → " + r.remotePath +
                                 (r.inGallery ? " (in the gallery)" : " (not in MediaStore yet)"));
        std::string dir = r.remotePath.substr(0, r.remotePath.rfind('/'));
        if (dir.rfind("/sdcard/", 0) == 0) dir = dir.substr(8);  // Pictures/ZizaiCast
        g.window->showToast(r.inGallery ? (g.settings.autoShare ? std::wstring(tr(S::ShareAutoPushed))
                                                                : fmt(S::SharePushedGallery, {d.file.filename().wstring()}))
                                        : fmt(S::SharePushedFolder, {toWide(dir)}),
                            g.settings.autoShare ? 2500 : 4000);
        pushNext();
        return;
    }
    g.log->write("warn", "share: push failed: " + r.error);
    std::vector<fs::path> rest{d.file};
    rest.insert(rest.end(), g.pushQueue.begin(), g.pushQueue.end());
    g.pushQueue.clear();
    g.window->showToast(tr(S::SharePushFail), 4000);
    sendViaQr(rest, true);
}

}  // namespace pm_app
