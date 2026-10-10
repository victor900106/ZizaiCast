// 自在投影 app: 共用常數表（模型、主題、濾鏡、選單圖示、資料夾名稱）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出；陣列改成 inline constexpr。
#pragma once

#include "core/base/app_common.h"

namespace pm_app {

// Translation model pairs (translate/src/models.inc) for 管理翻譯模型.
// "ocr": the PaddleOCR text recognition models (models\ocr, 0.7.0).
constexpr int kModelPairCount = 8;
inline constexpr const char* kModelPairs[kModelPairCount] = {"ocr", "ja-en", "ko-en", "zhHans-en", "zhHant-en", "en-zhHant", "en-ja", "en-ko"};

// Install-folder layout names shared with installer/zizai.iss (identifiers,
// the same in every UI language): 程式 (program files), 截圖, 錄影, 安裝檔.
inline constexpr wchar_t kDirProgram[] = L"程式";
inline constexpr wchar_t kDirShots[] = L"截圖";
inline constexpr wchar_t kDirRecordings[] = L"錄影";
inline constexpr wchar_t kDirInstallers[] = L"安裝檔";

// ---- 主題 -------------------------------------------------------------------------
inline constexpr pm::VideoWindow::Theme kThemes[4] = {pm::VideoWindow::Theme::Sakura, pm::VideoWindow::Theme::Mint,
                                               pm::VideoWindow::Theme::Night, pm::VideoWindow::Theme::MilkTea};
inline constexpr S kThemeNames[4] = {S::ThemeSakura, S::ThemeMint, S::ThemeNight, S::ThemeMilkTea};

// ---- 新手機連線時：接手 / 保持目前 --------------------------------------------------
// The policy is an AirPlayServer option, read at start(): restart the server
// now, or after the current session like the PIN / 畫質 options.
// 按 X 時: what × / Alt+F4 does (settings.ini close_button; see onCloseRequest).
inline constexpr S kCloseActNames[3] = {S::CloseActAsk, S::CloseActTray, S::CloseActQuit};

// ---- 放大鏡 / 翻譯 (0.7) ----------------------------------------------------------
// Magnifier, colour filters, freeze (pm_video) and on-screen translation
// (pm_translate, docs/translate.md). Only while a phone picture is shown: the
// menu items are greyed otherwise, the shortcuts do nothing, and when the
// picture ends the translation closes and the zoom goes back to 1× (the colour
// filter is a viewing preference: settings.ini filter=).
inline constexpr pm::VideoWindow::Filter kFilters[5] = {pm::VideoWindow::Filter::None, pm::VideoWindow::Filter::Contrast,
                                                 pm::VideoWindow::Filter::Grayscale, pm::VideoWindow::Filter::Invert,
                                                 pm::VideoWindow::Filter::YellowOnBlack};
inline constexpr S kFilterNames[5] = {S::FilterNone, S::FilterContrast, S::FilterGray, S::FilterInvert, S::FilterYellow};
constexpr int kLiveSeconds = 5;  // 即時翻譯 (change-driven since 0.8: ScreenTranslator::setLive; unused)

// 管理翻譯模型 by language (0.7.7): the user reads FROM a language; the pivot
// pairs behind it (ja-en + en-zhHant for 日文 → 繁體中文) stay out of sight.
inline constexpr pm::translate::Lang kModelSources[5] = {pm::translate::Lang::Ja, pm::translate::Lang::Ko,
                                                   pm::translate::Lang::En, pm::translate::Lang::ZhHans,
                                                   pm::translate::Lang::ZhHant};

// ---- menus (custom-drawn, app/popup_menu.cpp) -----------------------------
// Icon code points (Segoe Fluent Icons / Segoe MDL2 Assets share them).
constexpr wchar_t kIcoShow = 0xE8A7;        // OpenInNewWindow
constexpr wchar_t kIcoFullscreen = 0xE740;  // FullScreen
constexpr wchar_t kIcoPin = 0xE718;         // Pin
constexpr wchar_t kIcoCamera = 0xE722;      // Camera
constexpr wchar_t kIcoFolder = 0xE838;      // FolderOpen
constexpr wchar_t kIcoSettings = 0xE713;    // Setting
constexpr wchar_t kIcoPower = 0xE7E8;       // PowerButton (開機自動啟動)
constexpr wchar_t kIcoLock = 0xE72E;        // Lock (PIN)
constexpr wchar_t kIcoExit = 0xE8BB;        // ChromeClose

constexpr wchar_t kIcoRecord = 0xE7C8;      // Record (開始錄影)
constexpr wchar_t kIcoStop = 0xE71A;        // Stop (停止錄影)
constexpr wchar_t kIcoVideoFolder = 0xE8B7; // Folder (開啟錄影資料夾)
constexpr wchar_t kIcoDisplay = 0xE7F4;     // TVMonitor (畫面)
constexpr wchar_t kIcoRotate = 0xE7AD;      // Rotate
constexpr wchar_t kIcoFlip = 0xE8AB;        // Switch (左右翻轉)
constexpr wchar_t kIcoReset = 0xE777;       // UpdateRestore (還原)
constexpr wchar_t kIcoPhone = 0xE8EA;       // CellPhone (iPhone 外框)
constexpr wchar_t kIcoTheme = 0xE790;       // Color (主題)
constexpr wchar_t kIcoSync = 0xE895;        // Sync (檢查更新)
constexpr wchar_t kIcoUpdate = 0xE74A;      // Up (更新到 vX.Y.Z: menus, toolbar)

constexpr wchar_t kIcoQr = 0xED14;          // QRCode (連接 Android)
constexpr wchar_t kIcoHelp = 0xE897;        // Help (使用教學)
constexpr wchar_t kIcoCast = 0xEC15;        // Cast (接受 Android 投放)
constexpr wchar_t kIcoConnect = 0xE703;     // Connect (自動連線 Android)
constexpr wchar_t kIcoBack = 0xE72B;        // Back
constexpr wchar_t kIcoHome = 0xE80F;        // Home
constexpr wchar_t kIcoRecents = 0xE7C4;     // TaskView (最近使用)
constexpr wchar_t kIcoDisconnect = 0xEA14;  // DisconnectDisplay (中斷連線)
constexpr wchar_t kIcoMore = 0xE712;        // More (toolbar 更多)
constexpr wchar_t kIcoBackToWindow = 0xE73F;  // BackToWindow (toolbar: leave fullscreen)
constexpr wchar_t kIcoLanguage = 0xF2B7;    // LocaleLanguage (語言 / Language)
constexpr wchar_t kIcoKeyboard = 0xE765;    // KeyboardClassic (快速鍵一覽, 0.7.8)
constexpr wchar_t kIcoInfo = 0xE946;        // Info (關於 / About)
constexpr wchar_t kIcoShare = 0xE72D;       // Share (傳到手機: menus, toolbar, chip)
constexpr wchar_t kIcoSharePick = 0xE8E5;   // OpenFile (傳到手機…)
constexpr wchar_t kIcoZoom = 0xE71E;        // Zoom (放大鏡: menu, toolbar)
constexpr wchar_t kIcoZoomIn = 0xE8A3;      // ZoomIn
constexpr wchar_t kIcoZoomOut = 0xE71F;     // ZoomOut
constexpr wchar_t kIcoContrast = 0xE793;    // Contrast (顏色)
constexpr wchar_t kIcoFreeze = 0xE769;      // Pause (凍結畫面)
constexpr wchar_t kIcoTranslate = 0xE8C1;   // Characters (翻譯)
constexpr wchar_t kIcoRegion = 0xE7A8;      // Crop (框選翻譯)
constexpr wchar_t kIcoOriginal = 0xE890;    // View (顯示原文)
constexpr wchar_t kIcoLive = 0xE8EE;        // RepeatAll (連續翻譯)
constexpr wchar_t kIcoModels = 0xE896;      // Download (管理翻譯模型)
constexpr wchar_t kIcoDelete = 0xE74D;      // Delete
constexpr wchar_t kIcoLocalAi = 0xE7F8;     // DeviceLaptopNoPic (本機 AI 翻譯)
constexpr wchar_t kIcoOnline = 0xE774;      // Globe (線上翻譯)

}  // namespace pm_app
