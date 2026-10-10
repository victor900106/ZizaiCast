// 自在投影 app: update/local_updates.cpp — update（自動更新、本機更新）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "capture/capture.h"
#include "connect/connect.h"
#include "view/view.h"
#include "update/update.h"

namespace pm_app {

// <install>\安裝檔 (installed layout: exe in <install>\程式, created if
// missing; a dev build: next to the exe, only if it exists).
fs::path localUpdateDir() {
    const fs::path exeDir = exePath().parent_path();
    std::error_code ec;
    if (installedLayout(exeDir)) {
        const fs::path d = exeDir.parent_path() / kDirInstallers;
        fs::create_directories(d, ec);
        return fs::is_directory(d, ec) ? d : fs::path();
    }
    const fs::path d = exeDir / kDirInstallers;
    return fs::is_directory(d, ec) ? d : fs::path();
}

// 本機更新 scan (startup, every 10 min, after a change in the folder): the
// highest-version 「自在投影-安裝程式-*.exe」 whose VERSIONINFO says 自在投影
// and that is newer than this app. A file still being written (size / time
// changed within 2 s, or not openable exclusively) is skipped and the folder
// scanned again a second later.
void scanLocalUpdates() {
    KillTimer(g.hwnd, kLocalSettleTimer);
    if (g.localDir.empty()) return;
    const long long now = nowMs();
    std::map<std::wstring, App::LocalSeen> seen;
    bool unsettled = false;
    std::string bestV;
    fs::path best;
    WIN32_FIND_DATAW fd{};
    HANDLE f = FindFirstFileW((g.localDir / kInstallerPattern).c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring n = fd.cFileName;
            App::LocalSeen e;
            e.size = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            e.mtime = (static_cast<unsigned long long>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                      fd.ftLastWriteTime.dwLowDateTime;
            auto it = g.localSeen.find(n);
            if (it != g.localSeen.end() && it->second.size == e.size && it->second.mtime == e.mtime) e = it->second;
            else e.since = now;
            if (now - e.since < kLocalSettleMs) {
                unsettled = true;
            } else if (!e.checked) {
                const fs::path p = g.localDir / n;
                HANDLE x = CreateFileW(p.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (x == INVALID_HANDLE_VALUE) {
                    unsettled = true;  // still open for writing (copy in progress)
                } else {
                    CloseHandle(x);
                    e.checked = true;
                    if (!pm::update::installerVersion(p.wstring(), e.version)) e.version.clear();
                    g.log->write("info", "local installer " + toUtf8(n) + ": " +
                                             (e.version.empty() ? std::string("not a ZizaiCast installer")
                                                                : "version " + e.version));
                }
            }
            if (e.checked && !e.version.empty() &&
                (bestV.empty() || pm::update::compareVersions(e.version, bestV) > 0)) {
                bestV = e.version;
                best = g.localDir / n;
            }
            seen[n] = e;
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    g.localSeen.swap(seen);
    if (unsettled) SetTimer(g.hwnd, kLocalSettleTimer, 1000, nullptr);
    if (!bestV.empty() && pm::update::compareVersions(bestV, kAppVersion) <= 0) bestV.clear(), best.clear();
    if (bestV == g.localVersion && best == g.localInstaller) return;
    g.localVersion = bestV;
    g.localInstaller = best;
    // 「這次更新了什麼」 for a local installer: a sidecar <installer>.json (the
    // manifest schema) if it is there and names the same version (or none).
    g.localInfo = {};
    g.localSize = 0;
    if (!best.empty()) {
        std::error_code ec;
        g.localSize = fs::file_size(best, ec);
        if (ec) g.localSize = 0;
        pm::update::Manifest side;
        const fs::path sideFile = best.wstring() + L".json";
        if (pm::update::readManifestFile(sideFile.wstring(), side) && (side.version.empty() || side.version == bestV)) {
            g.localInfo = side;
            g.log->write("info", "local update: notes from " + toUtf8(sideFile.filename().wstring()) + " (" +
                                     std::to_string(side.changesZh.size()) + " zh / " +
                                     std::to_string(side.changesEn.size()) + " en changes)");
        }
    }
    g.log->write("info", bestV.empty() ? std::string("local update: none on offer")
                                       : "local update: " + bestV + " in " + toUtf8(best.wstring()));
    announceUpdate();
}

// Watcher thread: FindFirstChangeNotification on 安裝檔 -> WM_PM_LOCALDIR.
void startLocalWatch() {
    if (g.localDir.empty() || g.localWatchAlive) return;
    if (g.localWatch.joinable()) g.localWatch.join();
    HANDLE ch = FindFirstChangeNotificationW(
        g.localDir.c_str(), FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
    if (ch == INVALID_HANDLE_VALUE) {
        g.log->write("warn", "local update: cannot watch " + toUtf8(g.localDir.wstring()));
        return;
    }
    if (!g.localStop) g.localStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g.localWatchAlive = true;
    const HWND h = g.hwnd;
    g.localWatch = std::thread([ch, h, stop = g.localStop]() {
        HANDLE hs[2] = {stop, ch};
        for (;;) {
            if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
            PostMessageW(h, WM_PM_LOCALDIR, 0, 0);
            if (!FindNextChangeNotification(ch)) break;  // folder deleted
        }
        FindCloseChangeNotification(ch);
        g.localWatchAlive = false;
    });
}

void stopLocalWatch() {
    if (g.localStop) SetEvent(g.localStop);
    if (g.localWatch.joinable()) g.localWatch.join();
    if (g.localStop) CloseHandle(g.localStop);
    g.localStop = nullptr;
}

}  // namespace pm_app
