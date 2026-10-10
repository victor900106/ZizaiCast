// 自在投影 app: translate（放大鏡、翻譯的 UI 接線） 對外宣告。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出
#pragma once

#include "core/base/app_state.h"
#include "core/base/base.h"

namespace pm_app {

// A downloaded language whose files are all shared (英文 → 繁體中文 while 日文
// also needs en-zhHant): its own pairs on disk, and the other downloaded
// languages that would lose them, as 「日文」 / 「日文、韓文」. Empty when
// deleting the language frees files of its own (no question then).
struct SharedModels {
    std::vector<int> pairs;
    std::wstring users;
};


// ---- translate/magnifier.cpp
void syncViewTools();
void returnToLive(const char* why);
bool needPicture();
void zoomBy(int steps);
void zoomReset();
void magnifierCycle();
void setFilterOption(int f, bool toast = true);
void toggleFreeze();

// ---- translate/translate_actions.cpp
pm::translate::Lang translateLastSource();
void prewarmTranslation();
void translateCommand(UINT cmd);

// ---- translate/models.cpp
fs::path modelPairDir(int i);
unsigned long long dirBytes(const fs::path& d, bool fresh = false);
std::wstring mbText(unsigned long long b);
std::wstring pairLabel(int i);
void deleteModels(std::vector<int> pairs);
void askDeleteModels(int pair);
int modelPairIndex(const std::string& pair);
std::vector<int> sourcePairs(pm::translate::Lang src, bool* supported = nullptr);
bool sourceDownloaded(pm::translate::Lang src);
std::vector<int> sourceDeletablePairs(int source);
std::wstring modelSourceLabel(int source);
SharedModels sourceSharedModels(int source);
void askDeleteModelSet(std::vector<int> pairs, const std::wstring& label, const std::wstring& body = {});
void openModelsFolder();

// ---- translate/translator_host.cpp
void translatorAskDownload(pm::translate::Lang src, pm::translate::Lang tgt, double mb, std::function<void(bool)> answer);
void translatorNotify(const std::wstring& title, const std::wstring& text, bool important);
void closeEmptyTranslation(int tries);
void applyOnlineAllowed();
void createTranslator();

}  // namespace pm_app
