// 自在投影 app: 標準與 Win32 include、訊息 ID、命令列舉、Timer ID 與共用型別。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出
#pragma once

#include <windows.h>
#include <commctrl.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pm/airplay_server.h"
#include "pm/android_source.h"
#include "pm/audio_player.h"
#include "pm/miracast_receiver.h"
#include "pm/recorder.h"
#include "pm/share_server.h"
#include "pm/i18n.h"
#include "pm/translate.h"
#include "pm/video_window.h"
#include "about_panel.h"
#include "ask_panel.h"
#include "update_panel.h"
#include "pair_panel.h"
#include "popup_menu.h"
#include "resource.h"
#include "share_panel.h"
#include "settings_panel.h"
#include "share_picker.h"
#include "tr_settings.h"
#include "update/updater.h"
#include "volume_memory.h"
#include "volume_ui.h"

namespace fs = std::filesystem;
using pm::i18n::S;
using I18n = pm::i18n::S;  // where a function has its own S (AndroidSource::State)
using pm::i18n::fmt;
using pm::i18n::tr;

namespace pm_app {

// ---------------------------------------------------------------------------
// UI-thread messages (posted from network/decoder threads).
constexpr UINT WM_PM_STATE = WM_APP + 1;   // wp: 0 idle, 1 mirroring, 2 paused
constexpr UINT WM_PM_TRAY = WM_APP + 2;    // Shell_NotifyIcon callback
constexpr UINT WM_PM_EVENT = WM_APP + 3;   // wp: EventKind, lp: std::wstring* (owned)
constexpr UINT WM_PM_OPTION = WM_APP + 4;  // wp: option id, lp: checked
constexpr UINT WM_PM_UPDATE = WM_APP + 5;  // wp: UpdateKind, lp: UpdateResult* (owned)
constexpr UINT WM_PM_SOURCE = WM_APP + 6;  // wp: new Source, lp: previous Source (a source took the window)
constexpr UINT WM_PM_SRCEV = WM_APP + 7;   // wp: SourceEvent, lp: std::wstring* (owned)
constexpr UINT WM_PM_TOOL = WM_APP + 8;    // wp: Command (live toolbar button)
constexpr UINT WM_PM_LOCALDIR = WM_APP + 9;  // 本機更新: something changed in <install>\安裝檔
constexpr UINT WM_PM_SHARE = WM_APP + 20;   // 傳到手機: wp ShareEvent, lp owned payload (see onShareEvent)
enum EventKind : WPARAM { EvConnecting = 1, EvDisconnected, EvPin, EvTakeover };
// Miracast / Android events (posted from their threads as WM_PM_SRCEV).
enum SourceEvent : WPARAM {
    MiraStatus = 1,     // text = "<status digit><detail>"
    MiraConnecting,     // text = phone name (claimed the window)
    MiraRefused,        // text = phone name (takeover=keep, another source is live)
    MiraConnected,
    MiraDisconnected,
    MiraPin,
    MiraStarted,        // start() returned; text = "1" ok / "0" failed
    AndState,           // text = "<state digit><detail>"
    AndConnected,       // text = phone name
    AndDisconnected,
    AndTestConnected,   // --test-source android: fake phone (no adb)
};

// ---------------------------------------------------------------------------
// Source arbitration: AirPlay (iPhone), Miracast (Android 投放) and Android
// (wireless debugging) share one window, and only one of them shows at a
// time. g_active is the source that owns the picture (SrcNone = free). A
// source claims it when its session starts: a free window is always taken;
// a busy one only with takeover=new (else the newcomer is refused). AirPlay
// and Android video/audio pass through gates that drop everything from a
// source that does not own the window; Miracast draws into the window
// itself, so a takeover from Miracast ends its cast before the newcomer's
// first picture. The previous owner is stopped by the claimer (Miracast:
// disconnect(), Android: stop()) or, for AirPlay, by the UI thread
// (WM_PM_SOURCE → restart of the AirPlay server, which drops the iPhone).
enum Source : int { SrcNone = 0, SrcAirPlay, SrcMiracast, SrcAndroid };

// WM_PM_STATE values.
enum VideoState : WPARAM { StateIdle = 0, StateMirroring = 1, StatePaused = 2, StateLost = 3 };

// RememberVolumeAudioSink / SavedVolumeAudioSink: volume_memory.h

// ---------------------------------------------------------------------------
// Application state (UI thread only).
enum Command : UINT {
    CmdShow = 100,
    CmdAutostart,
    CmdPin,
    CmdOpenShots,
    CmdExit,
    CmdFullscreen,
    CmdTopmost,
    CmdSnapshot,
    CmdQualityStandard,  // + Settings::quality (0..2)
    CmdQualityHigh,
    CmdQualityMax,
    CmdAvSync,
    CmdRecord,           // 開始錄影 / 停止錄影
    CmdOpenRecordings,
    CmdRotateRight,      // 畫面
    CmdRotateLeft,
    CmdMirror,
    CmdResetView,
    CmdDeviceFrame,
    CmdTheme0,           // + Settings::theme (0..3)
    CmdTheme1,
    CmdTheme2,
    CmdTheme3,
    CmdTakeoverNew,
    CmdTakeoverKeep,
    CmdCheckUpdate,
    CmdInstallUpdate,
    CmdAndroidPair,      // 連接 Android（掃 QR）
    CmdTutorial,         // 使用教學
    CmdMiracast,         // 接受 Android 投放（Miracast）
    CmdAndroidAuto,      // 自動連線已配對的 Android
    CmdAndroidBack,      // 返回 / 主畫面 / 最近使用 (while an Android phone is shown)
    CmdAndroidHome,
    CmdAndroidRecents,
    CmdAndroidStop,      // 中斷 Android 連線
    CmdMiracastHelp,     // 如何啟用 (Miracast unavailable: tutorial #miracast)
    CmdDisconnect,       // 中斷連線 (any live source; Ctrl+D, toolbar, tray)
    CmdMoreMenu,         // live toolbar 「更多」: the full context menu
    CmdLangAuto,         // 語言 / Language: 自動 / 繁體中文 / English (+ Settings::language)
    CmdLangZh,
    CmdLangEn,
    CmdAbout,            // 關於自在投影 / About Zizai Cast
    CmdLangJa = 150,     // 語言 / Language: 日本語 (0.7.0; Settings::language 3)
    CmdLangKo,           // 한국어 (Settings::language 4)
    // 傳到手機 (docs/share.md); fixed ids for --dev DevCommand scripts.
    CmdShareLast = 160,  // toolbar: the newest screenshot / recording
    CmdShareLastShot,    // 把最後一張截圖傳到手機
    CmdShareLastRec,     // 把最後一段錄影傳到手機
    CmdSharePick,        // 傳到手機… (file picker, multi-select)
    CmdShareStop,        // stop the QR share (panel 停止分享)
    CmdShareAuto,        // 截圖／錄影後自動傳到手機 (toggle; settings.ini auto_share)
    // 自動更新: the 「有新版本」 dialog (menus, toolbar, idle pill, balloon);
    // CmdInstallUpdate (126) still installs at once (its 立即更新, scripts).
    CmdUpdateDialog = 170,
    // 放大鏡 / 翻譯 (0.7, docs/translate.md *Integration*); fixed ids for DevCommand scripts.
    CmdZoomIn = 180,     // Ctrl+=
    CmdZoomOut,          // Ctrl+-
    CmdZoomReset,        // Ctrl+Shift+0 (還原 1×)
    CmdMagCycle,         // toolbar 放大鏡: 1× → 2× → 4× → 1×
    CmdFilter0,          // + Settings::filter (0..4): 原色 / 加強對比 / 黑白 / 反轉 / 黃字黑底
    CmdFilter1,
    CmdFilter2,
    CmdFilter3,
    CmdFilter4,
    CmdFilterCycle,      // Ctrl+K
    CmdFreeze,           // Ctrl+P, toolbar 凍結
    CmdTranslateToggle,  // Ctrl+L, toolbar 翻譯: translate the screen / close
    CmdTranslateScreen,  // 翻譯整個畫面
    CmdTranslateRegion,  // 框選翻譯 (Ctrl+Shift+L)
    CmdTranslateOriginal,  // 顯示原文 (Ctrl+O)
    CmdTranslateLive,    // 連續翻譯
    CmdTranslateClose,   // 關閉翻譯
    CmdTrTargetZh,       // 翻成：繁體中文
    CmdTrTargetEn,       // 翻成：English
    CmdTrModelsFolder,   // 管理翻譯模型 → 開啟模型資料夾
    CmdTrDeleteAll,      // 管理翻譯模型 → 刪除全部
    CmdTrDelete0,        // + index in kModelPairs (delete one pair, 201..208)
    CmdTrTargetJa = 210, // 翻成：日本語 (0.7.0)
    CmdTrTargetKo,       // 翻成：한국어
    CmdTrLayoutAuto,     // 翻譯顯示方式：自動 (0.7.1)
    CmdTrLayoutInPlace,  // 原位顯示
    CmdTrLayoutList,     // 清單顯示
    CmdTrDarkCards,      // 深色方框 ✓
    // 按 X 時 ▸ 每次詢問 / 縮到右下角 / 結束程式 (+ Settings::closeButton 0..2)
    CmdCloseAsk = 230,
    CmdCloseTray,
    CmdCloseQuit,
    // 翻譯 ▸ 本機 AI 翻譯… / 線上翻譯（選用）… (0.7.4, app/tr_settings.cpp)
    CmdTrLocalAi = 240,
    CmdTrOnline,
    // 設定 ▸ 實驗：只提供螢幕鏡像 (hidden A/B item, 0.7.6; settings.ini airplay_advertise_audio)
    CmdAudioAdvertAB = 250,
    // 管理翻譯模型 ▸ one source language for the current target (+ index in kModelSources, 0.7.7;
    // 260..273 are app/volume_ui.h VolumeCommand)
    CmdTrDeleteLang0 = 290,
    // 說明 ▸ 快速鍵一覽 (F1; 0.7.8)
    CmdShortcuts = 300,
    CmdHowTo,            // 怎麼連線？ (連接手機 ▸ / 說明 ▸; the guide)
    // 0.7.8 UX (p7): 設定 ▸ 右鍵行為 ▸ 返回 / 開啟選單 (Settings::rightClickMenu)
    CmdRightClickBack = 320,
    CmdRightClickMenu,
    // 0.7.8 UX (p9): 現在重新連線套用 (chip, 設定 ▸): end the session so that
    // PIN / 畫面清晰度 / 名稱 changes held for it apply now
    CmdApplyNow = 325,
};
enum IdleOptionId : int { OptAutostart = 1, OptPin = 2 };
constexpr UINT_PTR kOrientationTimer = 1;
constexpr UINT_PTR kSyncTimer = 2;     // every 1 s while 影音同步 is on
constexpr UINT_PTR kLostTimer = 3;     // ends the "connection lost" hold
constexpr UINT_PTR kResumeTimer = 4;   // checks for a stale picture after resume
constexpr UINT_PTR kRecordTimer = 5;   // every 1 s while recording: failure / disk space
constexpr UINT_PTR kUpdateTimer = 6;   // update check: 3 s after start, then every 6 h
constexpr UINT_PTR kFitTimer = 7;      // refit the window after rotate / frame toggle
constexpr UINT_PTR kAndroidTimer = 8;  // reconnect paired Android phones while idle
constexpr UINT_PTR kAndroidWatchTimer = 9;  // every 1 s while an Android phone is live: still sending?
constexpr UINT_PTR kPairTimer = 10;    // the QR has been up a while without a phone
constexpr UINT_PTR kLocalTimer = 11;   // 本機更新: rescan <install>\安裝檔 every 10 min
constexpr UINT_PTR kLocalSettleTimer = 12;  // ... soon after a change / while a file is still being written
constexpr UINT kLocalScanMs = 10 * 60 * 1000;
constexpr UINT_PTR kShareNetTimer = 40;  // 傳到手機 running: is its LAN address still ours? (checkShareNetwork)
constexpr UINT kShareNetCheckMs = 10 * 1000;
constexpr UINT_PTR kMiraIdleTimer = 51;  // a pending Miracast cast went back to listening: free the window
constexpr long long kLocalSettleMs = 2000;  // size + time unchanged this long = fully written
constexpr long long kAndroidStallMs = 6000;  // no video and no audio this long = phone gone
constexpr UINT kAndroidRetryMs = 45 * 1000;
constexpr UINT kLostHoldMs = 3000;
constexpr UINT kResumeCheckMs = 4000;
constexpr UINT kFirstUpdateCheckMs = 3 * 1000;
constexpr UINT kUpdateIntervalMs = 6 * 60 * 60 * 1000;
constexpr long long kUpdateSnoozeMs = 24ll * 60 * 60 * 1000;  // 稍後提醒 (or until the next start)
constexpr unsigned long long kMinFreeBytes = 300ull << 20;  // stop recording below this

enum UpdateKind : WPARAM { UpdChecked = 1, UpdDownloaded };
struct UpdateResult {
    bool ok = false;
    std::string error;
    pm::update::Manifest manifest;
    fs::path file;  // UpdDownloaded
};
using pm::ui::MenuItem;
constexpr UINT_PTR kPinEndTimer = 52;  // PIN hidden and no SETUP followed: the session is over
constexpr UINT_PTR kConnectTimer = 53;  // 連線中 watchdog (connectWatchTick, below disconnectLive)

// Why Android 投放 (Miracast) cannot receive right now, for its idle card
// (MiraOn: listening).  Derived from unsupportedReason() / the Disabled
// detail; an unusable PC wins over the user's own switch.
enum MiraWhy { MiraOn, MiraOff, MiraReboot, MiraFeature, MiraOldWindows, MiraPolicy, MiraNoWifi, MiraOther };

// 連線中 watchdog (any source): a phone that shows 連線中 for 10 s gets a
// hint (Esc / Ctrl+D cancels); still no picture after 30 s, it is dropped
// with a message (the phone can simply try again; Android auto-reconnect
// stays on). A PIN on screen pauses it (the user is typing).
constexpr UINT kConnectHintMs = 10 * 1000, kConnectGiveUpMs = 30 * 1000;

}  // namespace pm_app
