// 自在投影 app: update/update_install.cpp — update（自動更新、本機更新）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "capture/capture.h"
#include "connect/connect.h"
#include "view/view.h"
#include "update/update.h"

namespace pm_app {

// 更新到 vX.Y.Z (menus, idle button, toolbar). Recording: ask first (the
// recording is saved). Local installer: install it now. Manifest: download
// (worker thread) -> verify SHA-256 -> quit -> run the installer after the
// message loop (see runPendingInstaller).
void installUpdate() {
    const std::string ver = offerVersion();
    if (ver.empty() || g.updateDownloading) return;
    if (recording() && !confirmUpdateWhileRecording(toWide(ver))) return;
    if (offerLocal()) {
        installLocalUpdate();
        return;
    }
    g.updateDownloading = true;
    const std::wstring v = toWide(g.update.version);
    g.window->showToast(fmt(S::UpdDownloadingV, {v}));
    g.log->write("info", "downloading update " + g.update.version + " from " + g.update.url);
    const HWND h = g.hwnd;
    const pm::update::Manifest m = g.update;
    const fs::path file = fs::temp_directory_path() / (kUpdateTempPrefix + v + L".exe");
    std::thread([h, m, file]() {
        auto* r = new UpdateResult;
        r->manifest = m;
        r->file = file;
        r->ok = pm::update::download(toWide(m.url), file.wstring(), r->error);
        if (r->ok) {
            const std::string got = pm::update::sha256File(file.wstring());
            if (got != m.sha256) {
                r->ok = false;
                r->error = "sha256 mismatch: got " + (got.empty() ? std::string("(unreadable)") : got);
                DeleteFileW(file.c_str());
            }
        }
        if (!PostMessageW(h, WM_PM_UPDATE, UpdDownloaded, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

// The verified installer stays open with FILE_SHARE_READ only from here until
// it is started (runPendingInstaller): nobody can write, rename or delete it
// meanwhile, and it is hashed again through this handle right before launch.
HANDLE g_installerLock = INVALID_HANDLE_VALUE;
std::string g_installerSha;

void releaseInstallerLock() {
    if (g_installerLock != INVALID_HANDLE_VALUE) CloseHandle(g_installerLock);
    g_installerLock = INVALID_HANDLE_VALUE;
    g_installerSha.clear();
}

// Locks `installer`, checks its SHA-256 (against `expectedSha` when given:
// the manifest's) and its Authenticode signature (unsigned is accepted: the
// installers are not code-signed; a signature that is there must verify).
bool lockAndVerifyInstaller(const fs::path& installer, const std::string& expectedSha) {
    releaseInstallerLock();
    HANDLE h = CreateFileW(installer.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        g.log->write("error", "update: cannot lock the installer (error " + std::to_string(GetLastError()) + ")");
        return false;
    }
    const std::string sha = pm::update::sha256Handle(h);
    if (sha.empty() || (!expectedSha.empty() && sha != expectedSha)) {
        g.log->write("error", "update: installer sha256 " + (sha.empty() ? std::string("(unreadable)") : sha) +
                                  " does not match the manifest's " + expectedSha);
        CloseHandle(h);
        return false;
    }
    std::string detail;
    const pm::update::Signature sig = pm::update::checkSignature(installer.wstring(), h, detail);
    g.log->write(sig == pm::update::Signature::Invalid ? "error" : "info",
                 "update: installer locked, sha256 " + sha + (expectedSha.empty() ? " (local copy)" : " (matches)") +
                     ", " + detail);
    if (sig == pm::update::Signature::Invalid) {
        CloseHandle(h);
        return false;
    }
    g_installerLock = h;
    g_installerSha = sha;
    return true;
}

// Saves any recording, remembers the target version (「已更新到」 toast at
// the next start), quits; the installer starts after the message loop.
void beginInstall(const fs::path& installer, const std::string& version, const std::string& expectedSha) {
    if (!lockAndVerifyInstaller(installer, expectedSha)) {
        g.window->showToast(tr(S::UpdVerifyFail));
        return;
    }
    if (g.testNoInstall) {  // --dev updater test: stop before anything is installed
        g.log->write("info", "test-no-install: would install " + version + " from " + toUtf8(installer.wstring()));
        g.window->showToast(fmt(S::UpdTestReady, {toWide(version)}), 5000);
        releaseInstallerLock();
        return;
    }
    stopRecording();
    g.pendingInstaller = installer;
    g.relaunchHidden = !IsWindowVisible(g.hwnd);
    g.settings.updatedTo = version;
    saveSettings();
    g.window->showToast(fmt(S::UpdInstalling, {toWide(version)}));
    quitApp();
}

// 本機更新: the installer in 安裝檔 is checked again (still there, same
// version) and copied to %TEMP%\自在投影-更新-X.Y.Z.exe (the 安裝檔 copy
// stays unlocked, so it can be replaced meanwhile; the copy is removed at
// the next start like a downloaded one).
void installLocalUpdate() {
    const std::string ver = g.localVersion;
    const fs::path src = g.localInstaller;
    std::string now;
    if (!pm::update::installerVersion(src.wstring(), now) || now != ver) {
        g.log->write("warn", "local installer changed or gone: " + toUtf8(src.wstring()));
        g.window->showToast(tr(S::UpdLocalChanged));
        SetTimer(g.hwnd, kLocalSettleTimer, 200, nullptr);  // rescan
        return;
    }
    const fs::path tmp = fs::temp_directory_path() / (kUpdateTempPrefix + toWide(ver) + L".exe");
    std::error_code ec;
    fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        g.log->write("error", "could not copy " + toUtf8(src.wstring()) + ": " + ec.message());
        g.window->showToast(tr(S::UpdLocalUnreadable));
        return;
    }
    g.log->write("info", "local update " + ver + " from " + toUtf8(src.wstring()) + "; quitting to install");
    beginInstall(tmp, ver);
}

void onUpdateDownloaded(const UpdateResult& r) {
    g.updateDownloading = false;
    if (!r.ok) {
        g.log->write("error", "update download failed: " + r.error);
        g.window->showToast(tr(r.error.rfind("sha256", 0) == 0 ? S::UpdShaFail : S::UpdDownloadFail));
        return;
    }
    g.log->write("info", "update " + r.manifest.version + " verified (sha256 ok): " + toUtf8(r.file.wstring()) +
                             "; quitting to install");
    beginInstall(r.file, r.manifest.version, r.manifest.sha256);
}

// After the message loop: start the verified installer. Installed layout
// (<install>\程式\自在投影.exe): silent update into the same folder; the
// installer waits for this process to exit and starts the app again with
// the same --dev / --background. Anything else (a dev build): the normal
// installer UI.
void runPendingInstaller() {
    if (g.pendingInstaller.empty()) return;
    const fs::path exeDir = exePath().parent_path();
    std::wstring params;
    auto installerLang = []() -> std::wstring {
        switch (pm::i18n::lang()) {
        case pm::i18n::Lang::En: return L"english";
        case pm::i18n::Lang::Ja: return L"japanese";
        case pm::i18n::Lang::Ko: return L"korean";
        default: return L"chinesetrad";
        }
    };
    if (installedLayout(exeDir)) {
        // /LANG: the shortcuts / readme follow the app's UI language (0.6.0+ installers).
        // Scripted off-screen tests: /VERYSILENT (no progress window on the user's screen).
        params = std::wstring(g.testOffscreen ? L"/VERYSILENT" : L"/SILENT") +
                 L" /SUPPRESSMSGBOXES /NORESTART /NOCANCEL /UPDATE /DIR=\"" + exeDir.parent_path().wstring() +
                 L"\" /LANG=" + installerLang();
        std::wstring args;
        if (g.dev) args += L"--dev";
        if (g.relaunchHidden) args += std::wstring(args.empty() ? L"" : L" ") + L"--background";
        if (g.testOffscreen) args += L" --test-offscreen";  // --dev scripted update tests
        if (!args.empty()) params += L" /APPARGS=\"" + args + L"\"";
    }
    // The file has been locked since it was verified; hash it once more anyway.
    const std::string now = pm::update::sha256Handle(g_installerLock);
    if (g_installerLock == INVALID_HANDLE_VALUE || now.empty() || now != g_installerSha) {
        g.log->write("error", "installer not started: changed since it was verified (" + (now.empty() ? "unreadable" : now) +
                                  " vs " + g_installerSha + ")");
        releaseInstallerLock();
        return;
    }
    g.log->write("info", "starting installer " + toUtf8(g.pendingInstaller.wstring()) + " " + toUtf8(params) +
                             " (sha256 re-checked)");
    const HINSTANCE rc = ShellExecuteW(nullptr, L"open", g.pendingInstaller.c_str(),
                                       params.empty() ? nullptr : params.c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(rc) <= 32)
        g.log->write("error", "could not start the installer (" + std::to_string(reinterpret_cast<INT_PTR>(rc)) + ")");
    releaseInstallerLock();
}

}  // namespace pm_app
