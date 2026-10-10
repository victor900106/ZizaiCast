// 自在投影 app: share（傳到手機） 對外宣告。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出
#pragma once

#include "core/base/app_state.h"
#include "core/base/base.h"

namespace pm_app {

struct PushDone;  // share_internal.h


// ---- share/share_files.cpp
bool isShareImage(const fs::path& p);
bool isShareVideo(const fs::path& p);
fs::path newestIn(const fs::path& dir, bool video);
fs::path lastShotFile();
fs::path lastRecFile();
fs::path lastShareable();

// ---- share/share_qr.cpp
std::wstring shareFilesLabel(const std::vector<fs::path>& files);
bool androidPushPath();
void copyToClipboard(const std::wstring& text);
bool liveShareOn();
void markSent(const std::vector<fs::path>& files);
std::vector<fs::path> unsentCaptures();
size_t unsentCount();
void openSharePanel();
void addToLive(const std::vector<fs::path>& files, bool showPanel);
bool startShareQr(const std::vector<fs::path>& files, bool live, int ttlSeconds = 0, bool showPanel = true);
void checkShareNetwork(bool force = false);
void sendViaQr(const std::vector<fs::path>& files, bool showPanel);

// ---- share/share_push.cpp
void queuePush(const std::vector<fs::path>& files);
void shareFiles(std::vector<fs::path> files);
void pushNext();
void onPushDone(const PushDone& d);

// ---- share/share_actions.cpp
void onShareEvent(WPARAM kind, LPARAM lp);
std::vector<fs::path> pickShareFiles();
void openSharePicker();
void shareSession();
void setAutoShare(bool on);
void autoSend(const fs::path& f);
void shareCommand(UINT cmd);
void offerShare(const fs::path& file);
void stopSharing();

}  // namespace pm_app
