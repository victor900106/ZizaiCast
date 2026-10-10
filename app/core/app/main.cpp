// 自在投影 (PhoneMirror) — receive iPhone Screen Mirroring (AirPlay), Android
// casting (Miracast) and Android wireless-debugging mirroring (scrcpy) in a
// window.
//
// App shell: the AirPlay core and the Android source feed the video window
// and the audio player through the pm::VideoSink / pm::AudioSink interfaces
// (include/pm/media.h); the Miracast receiver hands decoded BGRA pictures to
// the window itself. Only one source shows at a time (see "Source
// arbitration"). On top of that: tray icon, settings, autostart, PIN option,
// screenshots, recording (pm::Recorder), picture rotation / flip / iPhone
// frame, themes, takeover policy, the Android pairing panel (pair_panel.cpp),
// the tutorial and 自動更新 (updater.cpp).
//
// Usage: 自在投影.exe [--background] [--name "Display name"] [--h265] [--debug] [--dev]
//   --background  start hidden in the tray (used by the autostart entry)
//   --dev         developer instance: own mutex, OS-chosen ports, "(測試)" name,
//                 so it can run next to an installed copy
#include "core/base/app_state.h"
#include "core/base/base.h"
#include "help/help.h"
#include "share/share.h"
#include "capture/capture.h"
#include "translate/translate.h"
#include "connect/connect.h"
#include "view/view.h"
#include "update/update.h"
#include "menus/menus.h"
#include "core/app/core_app.h"

namespace pm_app {

HICON loadAppIcon(int cx) {
    HICON icon = nullptr;
    if (FAILED(LoadIconWithScaleDown(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), cx, cx, &icon)))
        icon = LoadIconW(nullptr, IDI_APPLICATION);
    return icon;
}

// A second launch: wake the running copy instead of starting another one.
void activateRunningInstance(bool background, UINT showMsg, Log& log) {
    log.write("info", "another instance is already running");
    if (background) return;
    HWND other = FindWindowW(L"PhoneMirrorVideoWindow", nullptr);
    if (!other) {
        MessageBoxW(nullptr, fmt(S::AlreadyRunning, {tr(S::AppName)}).c_str(), tr(S::AppName), MB_ICONINFORMATION);
        return;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    AllowSetForegroundWindow(pid);
    DWORD_PTR res = 0;
    SendMessageTimeoutW(other, showMsg, 0, 0, SMTO_ABORTIFHUNG, 2000, &res);
    if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
    SetForegroundWindow(other);
}

}  // namespace pm_app

using namespace pm_app;

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::wstring name;
    bool h265 = false, debug = false, background = false, dev = false, nameGiven = false, noHevc = false;
    TestFeed testFeed;
    int pairTimeoutS = 0;  // --dev --test-pair-timeout S (default 45 s)
    bool testOffscreen = false;
    bool demoBranding = false;  // --dev --demo-branding (README screenshots)
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            std::wstring a = argv[i];
            if (a == L"--name" && i + 1 < argc) name = argv[++i], nameGiven = true;
            else if (a == L"--h265") h265 = true;
            else if (a == L"--debug") debug = true;
            else if (a == L"--background") background = true;
            else if (a == L"--dev") dev = true;
            else if (a == L"--no-hevc") noHevc = true;  // test: act as if HEVC were missing (--dev only)
            else if (a == L"--test-feed" && i + 1 < argc) testFeed.file = argv[++i];  // --dev only
            else if (a == L"--test-seconds" && i + 1 < argc) testFeed.seconds = _wtoi(argv[++i]);
            else if (a == L"--test-takeover" && i + 1 < argc) testFeed.takeoverAt = _wtoi(argv[++i]);
            else if (a == L"--test-source" && i + 1 < argc) {  // --dev only: airplay | android | miracast
                const std::wstring s = argv[++i];
                testFeed.source = s == L"android" ? SrcAndroid : s == L"miracast" ? SrcMiracast : SrcAirPlay;
            } else if (a == L"--test-delay" && i + 1 < argc) testFeed.delayMs = _wtoi(argv[++i]) * 1000;
            else if (a == L"--test-stall" && i + 1 < argc) testFeed.stallAt = _wtoi(argv[++i]);
            else if (a == L"--test-start-fail") testFeed.startFail = true;
            else if (a == L"--test-no-picture") testFeed.noPicture = true;
            else if (a == L"--test-offscreen") testOffscreen = true;
            else if (a == L"--demo-branding") demoBranding = true;  // --dev only: screenshots without test labels
            else if (a == L"--test-no-install") g.testNoInstall = true;  // --dev only (see beginInstall)
            else if (a == L"--test-pair-timeout" && i + 1 < argc) pairTimeoutS = _wtoi(argv[++i]);
            else if (a == L"--test-no-network") g.testNoNetwork = true;  // --dev only (see docs/share.md)
        }
        LocalFree(argv);
    }
    const fs::path dir = dataDir(dev);
    Log log(dir / L"phonemirror.log");
    const UINT showMsg = RegisterWindowMessageW(L"PhoneMirror.ShowWindow");
    // 語言 / Language first: even the "already running" box is in it.
    g.settingsFile = dir / L"settings.ini";
    g.settings = Settings::load(g.settingsFile);
    pm::i18n::setLang(resolveLanguage(g.settings.language));
    g.dev = dev;
    g.demoBranding = dev && demoBranding;  // before displayNameFor(): it drops 「(測試)」
    if (nameGiven) g.nameArg = name;
    name = displayNameFor();

    // AirPlay uses fixed ports, so only one receiver per PC.
    const std::wstring devMutex = dir.filename() == L"PhoneMirror-dev" ? std::wstring(L"Local\\PhoneMirror.Dev")
                                                                       : L"Local\\PhoneMirror.Dev." + dir.filename().wstring();
    HANDLE single = CreateMutexW(nullptr, TRUE, dev ? devMutex.c_str() : L"Local\\PhoneMirror.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        activateRunningInstance(background, showMsg, log);
        return 0;
    }

    log.write("info", "ZizaiCast " PM_APP_VERSION_STR " starting as \"" + toUtf8(name) + "\"" +
                          (background ? " (background)" : "") + (dev ? " (dev)" : "") + ", language " +
                          pm::i18n::langKey(pm::i18n::lang()) + " (" + Settings::languageKey(g.settings.language) + ")");

    g.log = &log;
    g.name = name;
    g.showMsg = showMsg;
    g_takeoverNew = !g.settings.takeoverKeep;
    g.background = background;
    if (dev && pairTimeoutS > 0) g.pairTimeoutMs = static_cast<UINT>(pairTimeoutS) * 1000;
    g.testOffscreen = dev && testOffscreen;
    g.testNoInstall = dev && g.testNoInstall;
    pm::ui::PairPanel::testOffscreen = g.testOffscreen;
    pm::ui::AboutPanel::testOffscreen = g.testOffscreen;
    pm::ui::UpdatePanel::testOffscreen = g.testOffscreen;
    pm::ui::SharePanel::testOffscreen = g.testOffscreen;
    pm::ui::SharePicker::testOffscreen = g.testOffscreen;
    pm::ui::AskPanel::testOffscreen = g.testOffscreen;
    g.testNoNetwork = dev && g.testNoNetwork;
    if (dev) {  // 傳到手機 tests: serve on this address instead of the LAN one (e.g. 127.0.0.1)
        wchar_t bind[64] = {};
        if (GetEnvironmentVariableW(L"PM_SHARE_BIND", bind, 64) > 0 && bind[0]) g.shareBindTest = toUtf8(bind);
        if (GetEnvironmentVariableW(L"PM_SHARE_TTL", bind, 64) > 0 && _wtoi(bind) > 0) g.shareTtlTest = _wtoi(bind);
        // --test-source android: push through this folder's (fake) adb.exe (share/tools/fake_adb.cpp)
        wchar_t fakeAdb[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"PM_SHARE_FAKE_ADB", fakeAdb, MAX_PATH) > 0 && fakeAdb[0]) {
            g.testPush = std::make_unique<pm::AndroidSource>();
            if (g.testPush->init(fakeAdb) && g.testPush->connectKnownDevices()) log.write("info", "share: test pushes via " + toUtf8(fakeAdb));
            else g.testPush.reset();
        }
    }
    if (std::error_code ec; !fs::exists(g.settingsFile, ec)) saveSettings();  // show the keys
    repairAutostart();
    g.autostart = !readAutostart().empty();

    pm::AudioPlayerConfig audioCfg;
    audioCfg.log = [&log](const std::string& line) { log.write("audio", line); };
    pm::AudioPlayer audio(audioCfg);
    if (!audio.start()) log.write("warn", "audio output unavailable; continuing without sound");

    g.audio = &audio;
    refreshTitles();

    // [video] / [video-watchdog] lines (decoder, GPU, Present, the 5 s stream
    // summaries, self-healing actions) go to the log file too.
    pm::VideoWindow::setLogHandler([&log](const char* line) {
        std::string s(line);
        const char* level = "video";
        if (s.rfind("[video-watchdog] ", 0) == 0) {
            level = "video-watchdog";
            s.erase(0, 17);
        } else if (s.rfind("[video] ", 0) == 0) {
            s.erase(0, 8);
        }
        log.write(level, s);
    });
    pm::VideoWindow window;
    if (background) SetEnvironmentVariableW(L"PM_VIDEO_START_HIDDEN", L"1");  // no flash before it hides
    const bool created = window.create(g.idleTitle.c_str(), 540, 960);
    SetEnvironmentVariableW(L"PM_VIDEO_START_HIDDEN", nullptr);  // not inherited by adb / installers
    if (!created) {
        log.write("error", "could not create video window");
        MessageBoxW(nullptr, tr(S::D3DFail), tr(S::AppName), MB_ICONERROR);
        return 1;
    }
    HWND hwnd = reinterpret_cast<HWND>(window.hwnd());
    if (background) ShowWindow(hwnd, SW_HIDE);
    g.hwnd = hwnd;
    g_uiHwnd = hwnd;
    g.window = &window;

    g.iconBig = loadAppIcon(GetSystemMetrics(SM_CXICON));
    g.iconSmall = loadAppIcon(GetSystemMetrics(SM_CXSMICON));
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g.iconBig));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g.iconSmall));

    g.prevProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(appProc)));
    g.taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    // Allow Explorer / a second instance (both may run at another integrity
    // level) to deliver the registered messages.
    ChangeWindowMessageFilterEx(hwnd, g.taskbarCreatedMsg, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, showMsg, MSGFLT_ALLOW, nullptr);
    if (dev) g.devCmdMsg = RegisterWindowMessageW(L"PhoneMirror.DevCommand");
    trayAdd();
    if (!g.trayOk) {
        log.write("warn", "tray icon unavailable; closing the window exits");
        // --test-offscreen has no tray icon on purpose: --background stays hidden there.
        if (background && !g.testOffscreen) ShowWindow(hwnd, SW_SHOWNORMAL);
    }
    if (g.settings.topmost) applyTopmost();
    refreshIdleOptions();
    SetTimer(hwnd, kOrientationTimer, 300, nullptr);
    pm::ui::warmUp();  // D2D/DWrite factories now, so the first menu opens fast
    applyTheme();             // 主題: idle screen + menus
    applyView(false);         // 畫面: rotation, flip, iPhone frame
    setFilterOption(g.settings.filter, false);  // 放大鏡 colours (applied to pictures only)
    {  // 翻譯 ▸ 本機 AI 翻譯 / 線上翻譯 panels (app/tr_settings.cpp)
        pm::ui::trset::Host th;
        th.owner = hwnd;
        th.toast = [](const std::wstring& t) { g.window->showToast(t, 4000); };
        th.log = [](const char* level, const std::string& s) { g.log->write(level, "translate settings: " + s); };
        th.openUrl = [](const std::wstring& url) {
            if (g.testOffscreen) g.log->write("info", "test-offscreen: not opened " + toUtf8(url));
            else if (url.rfind(L"https://", 0) == 0) ShellExecuteW(g.hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        };
        th.onlineChanged = [] { applyOnlineAllowed(); };
        th.noNetwork = g.testNoNetwork;
        th.offscreen = g.testOffscreen;
        pm::ui::trset::init(std::move(th));
    }
    createTranslator();       // 翻譯 (pm_translate; bergamot.dll next to the exe)
    window.setViewHandler([](const pm::VideoWindow::ViewState&) { refreshToolbar(); });  // Ctrl+wheel / drag zoom

    // 錄影
    pm::Recorder recorder;
    recorder.log = [&log](const std::string& line) { log.write("recorder", line); };
    g.recorder = &recorder;

    StatusVideoSink videoSink(window, log);
    g.status = &videoSink;
    const fs::path volumeFile = dir / L"volume.txt";
    g.volumeFile = volumeFile;
    RememberVolumeAudioSink audioSink(audio, volumeFile, -15.0f, g.settings.muted);
    g.volume = &audioSink;
    audioSink.onSharedChange = [](float db) {  // Miracast casts follow the level and the app's 靜音
        if (g_miracast) g_miracast->setVolume(pm::AudioPlayer::airplayDbToGain(db));
    };
    audio.onVolume(audioSink.playDb());  // 靜音 / 0 % from the start
    g_volumeUi.attach(&audioSink, {[](const std::wstring& t) { g.window->showToast(t); }, []() { refreshToolbar(); },
                                   [](bool m) {
                                       g.settings.muted = m;
                                       saveSettings();
                                   },
                                   [&log](const std::string& l) { log.write("info", l); }});
    window.setLiveToolbarSlider([](int, float v, bool done) { g_volumeUi.slide(v, done); },
                                [](int, int notches) { g_volumeUi.wheel(notches); });
    if (audioSink.muted()) log.write("info", "volume: muted (settings.ini mute=1)");
    SavedVolumeAudioSink androidVolume(audio, audioSink, [&log](const std::string& l) { log.write("info", l); });
    // Source arbitration: AirPlay and Android reach the window / player
    // only while they own it.
    GateVideoSink airplayVideo(SrcAirPlay, videoSink), androidVideo(SrcAndroid, videoSink);
    GateAudioSink airplayAudio(SrcAirPlay, audioSink), androidAudio(SrcAndroid, androidVolume);
    g.videoSink = &airplayVideo;
    g.audioSink = &airplayAudio;
    g.androidVideo = &androidVideo;
    g.androidAudio = &androidAudio;
    g.androidAudioPackets = &androidAudio.packets;
    g.opts.h265 = h265;
    g.opts.debugLog = debug;
    g.opts.legacyPorts = !dev;
    g.opts.mirrorQuickAck = g.settings.quickAck;
    g.opts.reportedAudioLatencyMs = g.settings.audioLatencyMs;
    g.hevc = !(dev && noHevc) && hevcDecoderAvailable();
    if (!g.hevc) log.write("warn", "no HEVC decoder (HEVC Video Extensions missing): 畫質 limited to standard 1920x1080 H.264");
    g.opts.keyFile = toUtf8((dir / L"airplay.key").wstring());  // stable identity across restarts
    g.opts.initialVolumeDb = audioSink.saved();

    applySyncMode();
    if (g.testNoNetwork) {
        // --dev --test-no-network: a fresh build path would make Windows Firewall
        // ask about AirPlay's sockets on the user's screen; no receivers at all.
        log.write("info", "test-no-network: AirPlay, Miracast and adb not started");
    } else if (!startServer(10)) {  // a few seconds of retries: the ports may linger (e.g. right after an update)
        // Another receiver (UxPlay, AirServer, …) holds AirPlay's usual ports:
        // any free ones work too (Bonjour tells the phone which).
        bool ok = false;
        if (g.opts.legacyPorts) {
            g.opts.legacyPorts = false;
            ok = startServer(3);
            if (ok) {
                log.write("warn", "AirPlay: the usual ports (7000/7100) are taken: using other ports");
                window.showToast(tr(S::AirPlayOtherPorts), 6000);
            }
        }
        while (!ok) {  // 重試 (after closing the other program) or quit
            log.write("error", "AirPlay server failed to start");
            if (MessageBoxW(IsWindowVisible(hwnd) ? hwnd : nullptr, tr(S::AirPlayStartFail), tr(S::AppName),
                            MB_ICONERROR | MB_RETRYCANCEL) != IDRETRY) {
                trayRemove();
                g.quitting = true;
                window.close();
                window.runMessageLoop();
                return 1;
            }
            ok = startServer(3);
        }
    }

    // 自動更新: first check 3 s after start (also with --background), then every
    // 6 h; offers prompt only from then on (announceUpdate). Installers left in
    // %TEMP% by an earlier update are removed (one still running stays locked).
    SetTimer(hwnd, kUpdateTimer, kFirstUpdateCheckMs, nullptr);
    {
        // Only those names (the file system filters): %TEMP% can hold many
        // thousands of files, not worth listing at every start.
        std::error_code ec;
        const fs::path tmp = fs::temp_directory_path(ec);
        WIN32_FIND_DATAW fd{};
        HANDLE fh = ec ? INVALID_HANDLE_VALUE
                       : FindFirstFileExW((tmp / (std::wstring(kUpdateTempPrefix) + L"*.exe")).c_str(), FindExInfoBasic, &fd,
                                          FindExSearchNameMatch, nullptr, 0);
        if (fh != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring n = fd.cFileName;
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || n.rfind(kUpdateTempPrefix, 0) != 0 ||
                    fs::path(n).extension() != L".exe")
                    continue;  // (a short 8.3 name can match the pattern too)
                std::error_code rm;
                if (fs::remove(tmp / n, rm)) log.write("info", "removed old update installer " + toUtf8(n));
            } while (FindNextFileW(fh, &fd));
            FindClose(fh);
        }
    }

    // Android (wireless debugging): adb.exe + scrcpy-server in 程式\android-tools.
    pm::AndroidSource android;
    android.log = [&log](const std::string& line) { log.write("android", line); };
    android.events.onState = [hwnd](pm::AndroidSource::State s, const std::wstring& detail) {
        postSource(hwnd, AndState, std::to_wstring(static_cast<int>(s)) + detail);
    };
    android.events.onConnected = [hwnd](const std::wstring& name) { postSource(hwnd, AndConnected, name); };
    android.events.onDisconnected = [hwnd]() { postSource(hwnd, AndDisconnected); };
    const fs::path toolsDir = exePath().parent_path() / L"android-tools";
    g.android = &android;
    g_android = &android;
    g.testAndroid = dev && testFeed.source == SrcAndroid && !testFeed.file.empty();
    g.androidOk = !g.testNoNetwork && android.init(toolsDir.wstring());
    if (!g.androidOk && !g.testNoNetwork)
        log.write("warn", "android: tools missing or unusable in " + toUtf8(toolsDir.wstring()));
    if (g.settings.androidPaired < 0) {  // settings from before 0.7.9: has a phone ever connected? (the log says)
        bool seen = false;
        for (const wchar_t* f : {L"phonemirror.log", L"phonemirror.old.log"}) {
            std::ifstream in(dir / f, std::ios::binary);
            for (std::string l; !seen && std::getline(in, l);) seen = l.find("] android connected: ") != std::string::npos;
        }
        g.settings.androidPaired = seen ? 1 : 0;
        saveSettings();
        log.write("info", std::string("android: ") + (seen ? "paired before (log): auto-reconnect on" : "never paired: no reconnect scans until a pairing"));
    }
    if (g.androidOk && !g.testAndroid) {
        tryAndroidReconnect();
        SetTimer(hwnd, kAndroidTimer, kAndroidRetryMs, nullptr);
    }

    // Miracast (Android 投放): listen from the start when on; otherwise only
    // find out whether this PC could (for the greyed menu item).
    pm::MiracastReceiver miracast;
    miracast.log = [&log](const std::string& line) { log.write("miracast", line); };
    g.miracast = &miracast;
    g_miracast = &miracast;
    miracast.setVolume(pm::AudioPlayer::airplayDbToGain(audioSink.sharedDb()));  // and onSharedChange from here on
    if (g.settings.miracast && !(dev && testFeed.source == SrcMiracast) && !g.testNoNetwork) {
        miracastApply(true);
    } else {
        g.miracastOp = std::thread([hwnd]() {
            const std::wstring reason = pm::MiracastReceiver::unsupportedReason();
            if (!reason.empty()) postSource(hwnd, MiraStatus, L"0" + reason);
        });
    }

    // 本機更新: watch <install>\安裝檔, scan now and every 10 min.
    g.localDir = localUpdateDir();
    if (!g.localDir.empty()) log.write("info", "local update folder: " + toUtf8(g.localDir.wstring()));
    startLocalWatch();
    SetTimer(hwnd, kLocalTimer, kLocalScanMs, nullptr);

    // Idle screen: how to connect, the QR button and the tutorial link.
    refreshIdleHints();
    scanLocalUpdates();
    // Just updated (settings.ini updated_to, written before the installer ran).
    if (!g.settings.updatedTo.empty()) {
        const std::string to = g.settings.updatedTo;
        g.settings.updatedTo.clear();
        saveSettings();
        if (pm::update::compareVersions(kAppVersion, to) >= 0) {
            log.write("info", std::string("updated to ") + kAppVersion);
            const std::wstring msg = fmt(S::UpdDone, {toWide(kAppVersion)});
            window.showToast(msg, 5000);
            if (!IsWindowVisible(hwnd)) trayBalloon(msg, fmt(S::UpdDoneBalloon, {tr(S::AppName)}));
        } else {
            log.write("warn", "update to " + to + " did not complete (still " + kAppVersion + ")");
            window.showToast(fmt(S::UpdIncomplete, {toWide(to)}), 5000);
        }
    }
    // First start: open the tutorial once (not from autostart).
    if (!g.settings.tutorialShown && !background) {
        g.settings.tutorialShown = true;
        saveSettings();
        openTutorial(nullptr);
    }

    std::atomic<bool> feedStop{false};
    std::thread feed, feed2;
    if (dev && testFeed.source == SrcMiracast) {
        feed2 = std::thread(runTestMiracast, testFeed, &window, hwnd, &log, &feedStop);
        if (!testFeed.file.empty()) {  // plus a fake iPhone from the start: takeover / refusal test
            TestFeed airplay = testFeed;
            airplay.source = SrcAirPlay;
            airplay.delayMs = 1000;
            airplay.seconds = 0;
            feed = std::thread(runTestFeed, airplay, g.videoSink, hwnd, &log, &feedStop);
        }
    } else if (dev && !testFeed.file.empty()) {
        feed = std::thread(runTestFeed, testFeed, testFeed.source == SrcAndroid ? g.androidVideo : g.videoSink, hwnd,
                           &log, &feedStop);
    }

    int rc = window.runMessageLoop();
    feedStop = true;
    g.askPanel.close();
    window.setViewHandler(nullptr);
    g.translator.reset();  // before the window object goes
    if (feed.joinable()) feed.join();
    if (feed2.joinable()) feed2.join();

    log.write("info", "shutting down");
    trayRemove();
    stopRecording();
    stopSharing();
    g.pairPanel.close();
    g.aboutPanel.close();
    g.updatePanel.close();
    pm::ui::trset::shutdown(true);  // every exit path: its download / key-test threads are joined
    KillTimer(hwnd, kAndroidTimer);
    stopLocalWatch();
    android.stop();
    if (g.miracastOp.joinable()) g.miracastOp.join();
    miracast.stop();
    g.recorder = nullptr;
    if (g.server) g.server->stop();
    g.server.reset();
    g_volumeUi.detach();  // before audioSink goes
    audio.stop();
    if (single) CloseHandle(single);  // the update installer waits for this mutex to go
    runPendingInstaller();
    pm::VideoWindow::setLogHandler(nullptr);
    return rc;
}
