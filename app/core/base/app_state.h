// 自在投影 app: 全 app 狀態 struct App 與全域物件（定義在 core/base/app_state.cpp）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出
#pragma once

#include "core/base/base.h"
#include "core/base/log.h"
#include "core/base/settings.h"
#include "core/base/source_gate.h"

namespace pm_app {

struct App {
    HWND hwnd = nullptr;
    WNDPROC prevProc = nullptr;
    pm::VideoWindow* window = nullptr;
    pm::AudioPlayer* audio = nullptr;
    StatusVideoSink* status = nullptr;
    bool hevc = true;  // HEVC decoder present (else 畫質 is limited to 標準)
    Log* log = nullptr;
    fs::path settingsFile;
    Settings settings;
    bool autostart = false;
    bool quitting = false;
    bool trayOk = false;
    NOTIFYICONDATAW nid{};
    HICON iconSmall = nullptr, iconBig = nullptr;
    UINT taskbarCreatedMsg = 0;
    UINT showMsg = 0;
    UINT devCmdMsg = 0;            // --dev: PhoneMirror.DevCommand (wParam = Command)

    // AirPlay
    std::wstring name;             // display name advertised to phones (AirPlay + Miracast)
    std::wstring nameArg;          // --name "X" (wins over settings / language)
    bool miracastRenamePending = false;  // display name changed during a cast: re-apply after it
    pm::AirPlayServer::Options opts;
    std::unique_ptr<pm::AirPlayServer> server;
    pm::VideoSink* videoSink = nullptr;
    pm::AudioSink* audioSink = nullptr;
    bool sessionActive = false;    // a phone is connecting/connected
    bool restartPending = false;   // PIN / quality option changed during a session
    bool announcePending = false;  // toast "已連線" at the first frame
    std::wstring peerName;
    WPARAM videoState = 0;         // last WM_PM_STATE value

    std::wstring idleTitle, pausedTitle;
    int lastOrientation = 0;  // 0 unknown, 1 portrait, 2 landscape
    long long lastResumeMs = -100000;
    long long resumeFramesIn = 0;  // VideoWindow frames received at resume
    bool dev = false;              // --dev instance

    // 錄影
    pm::Recorder* recorder = nullptr;
    fs::path recordingFile;        // non-empty while recording

    // 自動更新
    bool updateChecking = false;   // a manifest request is in flight
    bool updateManual = false;     // ... started from the menu (report "up to date" / errors)
    bool updateDownloading = false;
    pm::update::Manifest update;   // newer version on offer (update.version empty = none)
    std::string updateNotified;    // version already announced (dialog / balloon) this run
    bool updatePromptReady = false;  // the first check (3 s after start) ran: offers may prompt
    bool updatePending = false;    // prompt deferred (phone live / recording / window hidden)
    bool balloonUpdate = false;    // the last tray balloon was the update one (click = dialog)
    std::string snoozeVersion;     // 稍後提醒: no automatic prompt for it until snoozeUntilMs
    long long snoozeUntilMs = 0;
    pm::update::Manifest localInfo;  // sidecar <installer>.json of the local offer (may be empty)
    unsigned long long localSize = 0;
    pm::ui::UpdatePanel updatePanel;
    fs::path pendingInstaller;     // verified installer to run after the message loop ends
    bool relaunchHidden = false;   // the window was hidden when the update started
    // 本機更新: newer installers dropped into <install>\安裝檔 (see scanLocalUpdates)
    struct LocalSeen {
        unsigned long long size = 0, mtime = 0;
        long long since = 0;       // nowMs() when size / time last changed
        bool checked = false;      // opened exclusively and VERSIONINFO read
        std::string version;       // empty: not a 自在投影 installer
    };
    fs::path localDir;             // <install>\安裝檔 (empty: not watched)
    std::map<std::wstring, LocalSeen> localSeen;  // file name -> last scan
    fs::path localInstaller;       // newest usable installer newer than this app
    std::string localVersion;      // its version (empty = none on offer)
    HANDLE localStop = nullptr;    // ends the watcher thread
    std::thread localWatch;
    std::atomic<bool> localWatchAlive{false};
    long long takeoverAtMs = -100000;  // last 「B」接手投影 (suppresses the 已連線 toast)
    long long recordingStoppedAtMs = -100000;

    // Sources (see "Source arbitration")
    int liveSource = SrcNone;      // source named in the title / toasts (UI copy of the owner)
    std::wstring sourceName;       // its phone name
    bool background = false;       // started with --background

    // Miracast
    pm::MiracastReceiver* miracast = nullptr;
    std::thread miracastOp;        // start() / stop() run here (WinRT calls take a moment)
    int miracastStatus = -1;       // last pm::MiracastReceiver::Status (-1 unknown yet)
    std::wstring miracastReason;   // unsupportedReason() / Disabled detail (empty = fine)
    bool miracastUnsupported = false;

    // Android (wireless debugging)
    pm::AndroidSource* android = nullptr;
    bool androidOk = false;        // init() found adb.exe + scrcpy-server
    bool androidUserStopped = false;  // 中斷 Android 連線: no auto-reconnect until the next pairing
    pm::VideoSink* androidVideo = nullptr;  // gated sinks handed to AndroidSource::start
    pm::AudioSink* androidAudio = nullptr;
    const std::atomic<long long>* androidAudioPackets = nullptr;  // liveness watchdog
    long long androidWatchCount = -1;  // frames + audio packets at the last progress
    long long androidWatchAt = 0;      // when they last moved (nowMs)
    UINT pairTimeoutMs = 45 * 1000;    // QR shown, no phone yet: say what to check
    std::string toolbarKey;            // last live toolbar handed to the window
    pm::ui::PairPanel pairPanel;
    pm::ui::AboutPanel aboutPanel;
    std::wstring androidState;     // last state text (for the panel)
    bool testAndroid = false;      // --test-source android: fake phone, no adb
    bool testOffscreen = false;    // --dev --test-offscreen: scripted screenshots, no focus changes
    bool demoBranding = false;     // --dev --demo-branding: no 「(測試)」 / 測試版 labels (screenshots)
    bool testNoInstall = false;    // --dev --test-no-install: update verified, installer not run
    bool testNoNetwork = false;    // --dev --test-no-network: no AirPlay / Miracast / adb (no firewall prompt)
    int testWays = -1;             // --dev DevCommand 950: idle cards as if (bit 0) adb ok, (>> 1) MiraWhy; -1 real

    // 傳到手機 (docs/share.md)
    pm::share::Server share;           // QR path: LAN page with the shared files
    pm::ui::SharePanel sharePanel;
    pm::ui::ShareChip shareChip;       // 「傳到手機」 after a screenshot / recording
    pm::ui::ShareChip applyChip;       // 0.7.8 UX (p9): 「現在重新連線套用」 beside a 投影結束後套用 toast
    fs::path lastShot, lastRec;        // taken this run (else the newest file in the folder)
    std::vector<fs::path> pushQueue;   // Android auto path: files still to push
    bool pushing = false;
    struct Capture {
        fs::path file;
        bool video = false;
        bool sent = false;             // pushed / queued to the phone, or on a QR page
        std::time_t at = 0;
    };
    std::vector<Capture> captures;     // screenshots / recordings of this run, oldest first
    pm::ui::SharePicker sharePicker;   // 傳到手機 with several unsent captures
    std::unique_ptr<pm::AndroidSource> testPush;  // --dev PM_SHARE_FAKE_ADB: real pushes through a fake adb.exe
    std::string shareBindTest;         // --dev: PM_SHARE_BIND (e.g. 127.0.0.1)
    int shareTtlTest = 0;              // --dev: PM_SHARE_TTL seconds (expiry tests)

    // 放大鏡 / 翻譯 (0.7)
    std::unique_ptr<pm::translate::ScreenTranslator> translator;
    pm::ui::AskPanel askPanel;         // model download consent, OCR language missing, delete models
    pm::ui::AskPanel closePanel;       // 按 X 時要怎麼做？ (close_button=ask)
    pm::ui::SettingsPanel keysPanel;   // 說明 ▸ 快速鍵一覽 (F1, 0.7.8)
    bool viewToolsOn = false;          // a picture was available at the last check (syncViewTools)
    bool userFrozen = false;           // 凍結 (Ctrl+P / toolbar) is on: closing a translation keeps it
    fs::path volumeFile;               // remembered phone volume (RememberVolumeAudioSink), set at start
    RememberVolumeAudioSink* volume = nullptr;  // its remembered level (saved())
};

extern App g;
extern pm::ui::VolumeControl g_volumeUi;  // 音量 / 靜音: toolbar, Ctrl+M / Ctrl+↑↓, menus (volume_ui.h)
// --dev test feeds: bumped by 中斷連線 so a fake phone stops like a dropped one.
extern std::atomic<int> g_testKick;
extern const char* kAppVersion;

}  // namespace pm_app
