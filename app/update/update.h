// 自在投影 app: update（自動更新、本機更新） 對外宣告。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出
#pragma once

#include "core/base/app_state.h"
#include "core/base/base.h"

namespace pm_app {

// File names shared with the installer / the local-update folder (identifiers,
// not UI text): 自在投影-安裝程式-X.Y.Z.exe, %TEMP%\自在投影-更新-X.Y.Z.exe.
const wchar_t kInstallerPattern[] = L"自在投影-安裝程式-*.exe";
const wchar_t kUpdateTempPrefix[] = L"自在投影-更新-";


// ---- update/update_check.cpp
void updateResultNote(const std::wstring& text);
void checkForUpdates(bool manual);
bool updateBusy();
void openUpdateDialog(bool user);
void maybeShowDeferredUpdate();
void announceUpdate(bool manual = false);
void onUpdateChecked(const UpdateResult& r);
bool confirmUpdateWhileRecording(const std::wstring& v);

// ---- update/update_install.cpp
void installUpdate();
void releaseInstallerLock();
bool lockAndVerifyInstaller(const fs::path& installer, const std::string& expectedSha);
void beginInstall(const fs::path& installer, const std::string& version, const std::string& expectedSha = {});
void installLocalUpdate();
void onUpdateDownloaded(const UpdateResult& r);
void runPendingInstaller();

// ---- update/local_updates.cpp
fs::path localUpdateDir();
void scanLocalUpdates();
void startLocalWatch();
void stopLocalWatch();

}  // namespace pm_app
