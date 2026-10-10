// 自在投影 app: core/base/paths.cpp — core/base（第 0 層：共用型別、狀態與原語）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"

namespace pm_app {

bool installedLayout(const fs::path& exeDir) { return exeDir.filename() == kDirProgram; }

// <install>\截圖 or <install>\錄影 (the installer keeps the exe in <install>\程式,
// the folders one level up; a dev build uses the exe folder). Falls back to
// Pictures\自在投影 / Videos\自在投影 (Zizai Cast in English) if that is not writable.
fs::path userDir(const wchar_t* leaf, REFKNOWNFOLDERID fallback) {
    std::error_code ec;
    const fs::path exeDir = exePath().parent_path();
    fs::path dir = exeDir / leaf;
    if (installedLayout(exeDir)) dir = exeDir.parent_path() / leaf;
    fs::create_directories(dir, ec);
    if (!ec && fs::is_directory(dir, ec)) {
        // Writable? (an install under Program Files would not be)
        fs::path probe = dir / L".write-test";
        HANDLE f = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            CloseHandle(f);
            return dir;
        }
    }
    fs::path pics = knownFolder(fallback);
    dir = (pics.empty() ? fs::temp_directory_path() : pics) / tr(S::AppName);
    fs::create_directories(dir, ec);
    return dir;
}

fs::path screenshotDir() { return userDir(kDirShots, FOLDERID_Pictures); }
fs::path recordingDir() { return userDir(kDirRecordings, FOLDERID_Videos); }

void openScreenshotDir() {
    ShellExecuteW(g.hwnd, L"open", screenshotDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void openRecordingDir() {
    ShellExecuteW(g.hwnd, L"open", recordingDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace pm_app
