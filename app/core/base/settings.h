// 自在投影 app: settings.ini 設定（讀寫在類別內）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出
#pragma once

#include "core/base/base.h"

namespace pm_app {

// ---------------------------------------------------------------------------
// Settings: %LOCALAPPDATA%\PhoneMirror\settings.ini, UTF-8 "key=value" lines.
// (The phone volume stays in volume.txt, see RememberVolumeAudioSink.)
struct Settings {
    bool requirePin = false;
    bool topmost = false;
    bool trayHintShown = false;
    int closeButton = 0;       // 按 X 時 (×, Alt+F4): 0 ask (每次詢問), 1 tray (縮到右下角), 2 quit (結束程式)
    bool closeHintShown = false;  // 「仍在背景執行」 balloon after the first 縮到右下角 (since 按 X 時)
    bool quickAck = true;      // Options::mirrorQuickAck (experimental low-latency ACKs)
    int audioLatencyMs = -1;   // Options::reportedAudioLatencyMs (-1 = library default)
    int quality = 1;           // 0 standard 1920x1080, 1 high 2560x1440, 2 max 3840x2160
    bool avSync = false;       // VideoWindow::setSyncMode (picture waits for the sound)
    std::string deviceId;      // Options::macAddress; seeded from the core's first choice
    int rotation = 0;          // 畫面: quarter turns clockwise 0..3 (stored as degrees)
    bool mirrored = false;     // 畫面: 左右翻轉
    bool deviceFrame = false;  // 畫面: iPhone 外框
    int theme = 0;             // 主題: 0 sakura 1 mint 2 night 3 milktea
    bool takeoverKeep = false; // 新手機連線時: false 接手 (new), true 保持目前 (keep)
    bool updateUrlSet = false; // update_url present in the file (empty = no checks)
    std::string updateUrl;
    bool miracast = true;      // 接受 Android 投放（Miracast）
    bool androidAuto = true;   // reconnect paired Android phones (wireless debugging) when idle
    int androidPaired = -1;    // an Android phone connected over adb once (1) / never (0): only then look for it; -1 not known (before 0.7.9)
    bool tutorialShown = false;  // 使用教學 opened once at the first start
    bool toolbarHintShown = false;  // 0.7.8: the toolbar introduced itself once (first phone shown)
    bool autoShare = false;      // 截圖／錄影後自動傳到手機 (docs/share.md)
    bool muted = false;          // 靜音 (volume_ui.h); the level itself stays in volume.txt
    // 遠端操控 (0.8.0, docs/remote.md). Off at every start (turned on by hand); these are its options.
    bool remoteViewOnly = false;  // 僅觀看: the phone sees the screen, input is dropped
    bool remoteAudio = true;      // the PC's sound is offered to the phone (it still has to tap 開啟聲音)
    bool remoteMutePc = false;    // mute the PC's speakers while the phone plays the sound
    int remoteIdleMinutes = 30;   // end a session after this long without a touch on the phone (0 = never)
    // 0.8.1 聲音來源: "" automatic (the device that plays), "default" the Windows default, else a device id
    std::string remoteAudioSource;
    std::string remoteToken;      // the phone link's secret part: kept so a Home Screen icon keeps working
    int remotePort = 8090;        // its port (the next free one from 8090 if taken; kept for the same reason)
    // 常駐外網 (0.8.2): remote control + the fixed link (Tailscale Funnel :8443) at every
    // start; logins with the owner's password (only its PBKDF2 hash is kept).
    bool remotePersistent = false;
    std::string remotePwHash;          // "pbkdf2-sha256$<iterations>$<salt>$<key>"
    std::string remoteFixedSecret;     // signs the phones' 30-day logins (a new password = a new one)
    bool remoteFixedNeedApproval = false;  // 外面的新手機也要電腦按允許 (0.8.3.6: kept = remotePolicyFixed != 2, for older versions)
    // 0.8.3.6 連線時要不要允許 (pm::remote::ApprovalPolicy: 0 每次問, 1 記住這台裝置, 2 只用密碼) for the 主帳號 on the
    // fixed address, 臨時帳號, 臨時網址. Safe defaults: 臨時帳號 / 臨時網址 每次問; the 主帳號 as before (只用密碼, or
    // 記住這台裝置 when 外面的新手機也要電腦按允許 was on).
    int remotePolicyFixed = 2;
    int remotePolicyGuest = 0;
    int remotePolicyTemp = 0;
    // 0.8.3: logins ended by 中斷 ("<token id>:<expiry>,..."; the password is needed again) and
    // 遠端斷線後自動鎖定電腦 (from outside: the fixed link / 免安裝外網; on the LAN / Tailscale)
    std::string remoteRevoked;
    // 0.8.3.2: both off by default (0.8.3 / 0.8.3.1: outside on). A saved value is kept as it is.
    bool remoteLockOutside = false;
    bool remoteLockLan = false;
    std::string whatsNewSeen;          // the version whose 新功能 note was shown
    int idleTab = 0;                   // 0.8.2 main window page: 0 手機投影, 1 遠端控制 (the last one shown)
    std::string updatedTo;     // set just before an update installs: 「已更新到 vX」 at the next start
    std::string skipVersion;   // 略過這個版本: never prompted again (the menu item stays)
    int language = 0;          // 語言 / Language: 0 auto (Windows UI language), 1 zh-TW, 2 en, 3 ja, 4 ko
    std::wstring displayName;  // name shown to phones (empty: 自在投影 / Zizai Cast by language)
    int filter = 0;            // 放大鏡 colours: 0 none 1 contrast 2 gray 3 invert 4 yellow (VideoWindow::Filter)
    int translateTo = 0;       // 翻譯 翻成: 0 auto (UI language), 1 繁體中文, 2 English, 3 日本語, 4 한국어
    int translateLayout = 0;   // 翻譯顯示方式: 0 automatic, 1 原位顯示, 2 清單顯示 (0.7.1)
    bool translateDark = false;  // 深色方框 instead of the picture's own colours (0.7.1)
    std::string translateLast;   // source language of the last translation ("ja", "en", ...): its models are
                                 // loaded ahead (ScreenTranslator::prewarm) when a phone connects; empty: never used
    // A/B test (0.7.6): Options::advertiseAudio, settings.ini airplay_advertise_audio
    // 1 also an AirPlay speaker (default), 0 Screen Mirroring only, 2 bit 9 on but no _raop._tcp
    int advertiseAudio = 1;
    // A/B test (0.7.10.1, iOS volume bar when the Google app opens): Options::volumeControlType /
    // audioStatusFlag, settings.ini airplay_volume_control (-1 default, 0..4) and
    // airplay_audio_status (1 default, 0); core/src/pm_volume_hud.h
    int volumeControlType = -1;
    int audioStatusFlag = 1;
    // 0.7.8 UX (p7): 右鍵行為 on an Android picture: false 返回 (default), true 開啟選單;
    // Shift+right click always opens the menu. The 「右鍵＝返回」 hint shows once.
    bool rightClickMenu = false;
    bool rightClickHintShown = false;
    // 0.8.3 簡報畫筆 (app/view/ink.cpp): tool 0 pen / 1 highlighter / 2 arrow,
    // colour index into kInkColors, thickness 0 thin / 1 medium / 2 thick, and
    // whether screenshots / recordings include the drawings.
    int inkTool = 0;
    int inkColor = 0;
    int inkWidth = 1;
    bool inkInShots = true;
    bool inkInRec = true;
    // 0.8.3 Android 操控 (app/connect/android_control.cpp; AndroidSource API of droid-fix).
    bool androidClipSync = true;    // 剪貼簿同步
    bool androidScreenOff = false;  // 操控時關掉手機螢幕
    bool androidStayAwake = false;  // 連線期間不休眠

    static constexpr const char* kThemeKeys[4] = {"sakura", "mint", "night", "milktea"};
    static constexpr const char* kFilterKeys[5] = {"none", "contrast", "gray", "invert", "yellow"};
    static constexpr const char* kTranslateKeys[5] = {"auto", "zh-Hant", "en", "ja", "ko"};
    static constexpr const char* kLayoutKeys[3] = {"auto", "inplace", "list"};
    static constexpr const char* kCloseKeys[3] = {"ask", "tray", "quit"};

    // Manifest URL in effect: settings.ini update_url, else the build's default.
    std::string effectiveUpdateUrl() const { return updateUrlSet ? updateUrl : std::string(PM_UPDATE_URL_STR); }

    static const char* policyKey(int p) { return p == 1 ? "remember" : p == 2 ? "password" : "ask"; }  // 0.8.3.6

    static Settings load(const fs::path& file) {
        Settings s;
        std::ifstream in(file);
        std::string line;
        std::map<std::string, std::string> kv;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto eq = line.find('=');
            if (line.empty() || line[0] == ';' || line[0] == '[' || eq == std::string::npos) continue;
            kv[line.substr(0, eq)] = line.substr(eq + 1);
        }
        auto flag = [&](const char* k, bool def) {
            auto it = kv.find(k);
            return it == kv.end() ? def : it->second == "1";
        };
        s.requirePin = flag("require_pin", false);
        s.topmost = flag("topmost", false);
        s.trayHintShown = flag("tray_hint_shown", false);
        if (auto it = kv.find("close_button"); it != kv.end())
            for (int c = 0; c < 3; ++c)
                if (it->second == kCloseKeys[c]) s.closeButton = c;
        s.closeHintShown = flag("close_hint_shown", false);
        s.quickAck = flag("quick_ack", true);
        // 影音同步 is disabled (picture fell behind the sound, docs/app.md): the
        // stored value is ignored and the option is not offered in any menu.
        s.avSync = false;
        if (auto it = kv.find("device_id"); it != kv.end() && validMac(it->second)) s.deviceId = it->second;
        if (auto it = kv.find("quality"); it != kv.end()) {
            if (it->second == "standard") s.quality = 0;
            else if (it->second == "max") s.quality = 2;
            else s.quality = 1;  // "high" or anything unknown
        }
        if (auto it = kv.find("audio_latency_ms"); it != kv.end()) {
            try {
                s.audioLatencyMs = std::stoi(it->second);
            } catch (...) {
            }
        }
        if (auto it = kv.find("rotation"); it != kv.end()) {
            const std::string& v = it->second;
            s.rotation = v == "90" ? 1 : v == "180" ? 2 : v == "270" ? 3 : 0;
        }
        s.mirrored = flag("mirror", false);
        s.deviceFrame = flag("device_frame", false);
        if (auto it = kv.find("theme"); it != kv.end())
            for (int t = 0; t < 4; ++t)
                if (it->second == kThemeKeys[t]) s.theme = t;
        if (auto it = kv.find("takeover"); it != kv.end()) s.takeoverKeep = it->second == "keep";
        s.miracast = flag("miracast", true);
        s.androidAuto = flag("android_auto", true);
        if (auto it = kv.find("android_paired"); it != kv.end()) s.androidPaired = it->second == "1" ? 1 : 0;
        s.tutorialShown = flag("tutorial_shown", false);
        s.toolbarHintShown = flag("toolbar_hint_shown", false);
        s.autoShare = flag("auto_share", false);
        s.muted = flag("mute", false);
        s.remoteViewOnly = flag("remote_view_only", false);
        s.remoteAudio = flag("remote_audio", true);
        s.remoteMutePc = flag("remote_mute_pc", false);
        if (auto it = kv.find("remote_idle_minutes"); it != kv.end())
            s.remoteIdleMinutes = (std::max)(0, (std::min)(24 * 60, atoi(it->second.c_str())));
        if (auto it = kv.find("remote_audio_source"); it != kv.end() && it->second.size() < 512)
            s.remoteAudioSource = it->second == "auto" ? std::string() : it->second;
        if (auto it = kv.find("remote_token"); it != kv.end() && it->second.size() == 32) s.remoteToken = it->second;
        if (auto it = kv.find("remote_port"); it != kv.end()) {
            const int p = atoi(it->second.c_str());
            if (p >= 1024 && p <= 65535) s.remotePort = p;
        }
        s.remotePersistent = flag("remote_persistent", false);
        if (auto it = kv.find("remote_pw_hash"); it != kv.end() && it->second.rfind("pbkdf2-sha256$", 0) == 0 && it->second.size() < 256)
            s.remotePwHash = it->second;
        if (auto it = kv.find("remote_fixed_secret"); it != kv.end() && it->second.size() == 64) s.remoteFixedSecret = it->second;
        s.remoteFixedNeedApproval = flag("remote_fixed_approval", false);
        {  // 0.8.3.6: ask | remember | password; a file from before has none (the 主帳號 follows remote_fixed_approval)
            auto policy = [&](const char* key, int def) {
                const auto it = kv.find(key);
                if (it == kv.end()) return def;
                return it->second == "ask" ? 0 : it->second == "remember" ? 1 : it->second == "password" ? 2 : def;
            };
            s.remotePolicyFixed = policy("remote_policy_fixed", s.remoteFixedNeedApproval ? 1 : 2);
            s.remotePolicyGuest = policy("remote_policy_guest", 0);
            s.remotePolicyTemp = policy("remote_policy_temp", 0);
            s.remoteFixedNeedApproval = s.remotePolicyFixed != 2;
        }
        if (auto it = kv.find("remote_revoked"); it != kv.end() && it->second.size() < 8192) s.remoteRevoked = it->second;
        s.remoteLockOutside = flag("remote_lock_outside", false);  // 0.8.3.2: default off
        s.remoteLockLan = flag("remote_lock_lan", false);
        if (s.remotePwHash.empty() || s.remoteFixedSecret.empty()) s.remotePersistent = false;  // no password: never on
        if (auto it = kv.find("whats_new_seen"); it != kv.end() && it->second.size() < 32) s.whatsNewSeen = it->second;
        if (auto it = kv.find("idle_tab"); it != kv.end()) s.idleTab = it->second == "remote" ? 1 : 0;
        if (auto it = kv.find("updated_to"); it != kv.end()) s.updatedTo = it->second;
        if (auto it = kv.find("skip_version"); it != kv.end()) s.skipVersion = it->second;
        if (auto it = kv.find("language"); it != kv.end())
            s.language = it->second == "zh-TW" ? 1 : it->second == "en" ? 2 : it->second == "ja" ? 3 : it->second == "ko" ? 4 : 0;
        if (auto it = kv.find("filter"); it != kv.end())
            for (int f = 0; f < 5; ++f)
                if (it->second == kFilterKeys[f]) s.filter = f;
        if (auto it = kv.find("translate_to"); it != kv.end())
            for (int t = 0; t < 5; ++t)
                if (it->second == kTranslateKeys[t]) s.translateTo = t;
        if (auto it = kv.find("translate_layout"); it != kv.end())
            for (int t = 0; t < 3; ++t)
                if (it->second == kLayoutKeys[t]) s.translateLayout = t;
        if (auto it = kv.find("translate_cards"); it != kv.end()) s.translateDark = it->second == "dark";
        if (auto it = kv.find("translate_last"); it != kv.end()) s.translateLast = it->second;
        if (auto it = kv.find("airplay_advertise_audio"); it != kv.end())
            s.advertiseAudio = it->second == "0" ? 0 : it->second == "2" ? 2 : 1;
        if (auto it = kv.find("airplay_volume_control"); it != kv.end()) {
            const std::string& v = it->second;
            s.volumeControlType = v.size() == 1 && v[0] >= '0' && v[0] <= '4' ? v[0] - '0' : -1;
        }
        if (auto it = kv.find("airplay_audio_status"); it != kv.end()) s.audioStatusFlag = it->second == "0" ? 0 : 1;
        if (auto it = kv.find("android_right_click"); it != kv.end()) s.rightClickMenu = it->second == "menu";
        s.rightClickHintShown = flag("right_click_hint_shown", false);
        auto ranged = [&](const char* k, int def, int n) {
            auto it = kv.find(k);
            const int v = it == kv.end() ? def : atoi(it->second.c_str());
            return v >= 0 && v < n ? v : def;
        };
        s.inkTool = ranged("ink_tool", 0, 3);
        s.inkColor = ranged("ink_color", 0, 6);
        s.inkWidth = ranged("ink_width", 1, 3);
        s.inkInShots = flag("ink_in_screenshots", true);
        s.inkInRec = flag("ink_in_recordings", true);
        s.androidClipSync = flag("android_clipboard_sync", true);
        s.androidScreenOff = flag("android_screen_off", false);
        s.androidStayAwake = flag("android_stay_awake", false);
        if (auto it = kv.find("display_name"); it != kv.end()) {
            std::string v = it->second;
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
            s.displayName = toWide(v);
        }
        if (auto it = kv.find("update_url"); it != kv.end()) {
            s.updateUrlSet = true;
            s.updateUrl = it->second;
            while (!s.updateUrl.empty() && s.updateUrl.back() == ' ') s.updateUrl.pop_back();
            // Earlier defaults (the pre-0.5.3 placeholder, the repo's old name) were
            // never a user choice: use the build's default.
            if (s.updateUrl == "https://zizai-update.example.com/zizai/update.json" ||
                s.updateUrl == "https://github.com/victor900106/zizai-touying/releases/latest/download/update.json") {
                s.updateUrlSet = false;
                s.updateUrl.clear();
            }
        }
        return s;
    }
    // Atomic: written to settings.ini.tmp, flushed, then moved over
    // settings.ini (a crash / power cut mid-write never leaves a cut-off file,
    // which would load as all defaults: PIN off, new device_id, …).
    void save(const fs::path& file) const {
        const fs::path tmp = fs::path(file) += L".tmp";
        std::ofstream out(tmp, std::ios::trunc);
        out << "[settings]\n"
            << "require_pin=" << (requirePin ? 1 : 0) << "\n"
            << "topmost=" << (topmost ? 1 : 0) << "\n"
            << "tray_hint_shown=" << (trayHintShown ? 1 : 0) << "\n"
            << "; window close button (×, Alt+F4): ask (every time), tray (minimize to the tray, keep receiving), quit\n"
            << "close_button=" << kCloseKeys[closeButton % 3] << "\n"
            << "close_hint_shown=" << (closeHintShown ? 1 : 0) << "\n"
            << "; experimental low-latency TCP ACKs for the mirror stream (1 on, 0 off)\n"
            << "quick_ack=" << (quickAck ? 1 : 0) << "\n"
            << "; audio latency reported to the phone in ms (-1 = default 250)\n"
            << "audio_latency_ms=" << audioLatencyMs << "\n"
            << "; display size offered to the phone: standard 1920x1080, high 2560x1440, max 3840x2160\n"
            << "quality=" << qualityKey(quality) << "\n"
            << "; A/V sync: disabled since 0.3.1 (made the picture lag the sound), always 0\n"
            << "av_sync=" << (avSync ? 1 : 0) << "\n"
            << "; AirPlay identity (MAC-style deviceid); keep it so paired iPhones recognise this PC\n"
            << "device_id=" << deviceId << "\n"
            << "; picture: rotation 0/90/180/270 (clockwise), mirror (left-right flip), device_frame (iPhone frame)\n"
            << "rotation=" << rotation * 90 << "\n"
            << "mirror=" << (mirrored ? 1 : 0) << "\n"
            << "device_frame=" << (deviceFrame ? 1 : 0) << "\n"
            << "; theme: sakura (櫻花粉), mint (薄荷綠), night (夜空藍), milktea (奶茶)\n"
            << "theme=" << kThemeKeys[theme & 3] << "\n"
            << "; another iPhone starts mirroring: new = it takes over, keep = the current one stays\n"
            << "takeover=" << (takeoverKeep ? "keep" : "new") << "\n"
            << "; Android: accept Miracast casting (1/0); reconnect paired phones over wireless debugging (1/0)\n"
            << "miracast=" << (miracast ? 1 : 0) << "\n"
            << "android_auto=" << (androidAuto ? 1 : 0) << "\n"
            << (androidPaired < 0 ? std::string() : "android_paired=" + std::to_string(androidPaired) + "\n")
            << "tutorial_shown=" << (tutorialShown ? 1 : 0) << "\n"
            << "toolbar_hint_shown=" << (toolbarHintShown ? 1 : 0) << "\n"
            << "; send every new screenshot / recording to the phone (Android over adb: its gallery; else a live QR page)\n"
            << "auto_share=" << (autoShare ? 1 : 0) << "\n"
            << "; 靜音: PC sound off for every phone (1/0); the level is kept in volume.txt\n"
            << "mute=" << (muted ? 1 : 0) << "\n"
            << "; 遠端操控 (remote control from a phone browser; always off at start): view only (1/0), send the PC sound (1/0),\n"
            << ";   mute the PC while the phone plays it (1/0), end a session after N minutes without a touch (0 = never)\n"
            << "remote_view_only=" << (remoteViewOnly ? 1 : 0) << "\n"
            << "remote_audio=" << (remoteAudio ? 1 : 0) << "\n"
            << "remote_mute_pc=" << (remoteMutePc ? 1 : 0) << "\n"
            << "remote_idle_minutes=" << remoteIdleMinutes << "\n"
            << "; the PC sound for the phone from: auto (the playback device that plays), default (the Windows default), or a device id\n"
            << "remote_audio_source=" << (remoteAudioSource.empty() ? std::string("auto") : remoteAudioSource) << "\n"
            << "; the phone link (secret token + port), kept so the phone's Home Screen icon keeps working; delete both for a new link\n"
            << (remoteToken.empty() ? std::string() : "remote_token=" + remoteToken + "\n")
            << "remote_port=" << remotePort << "\n"
            << "; 常駐外網: remote control + a fixed address (Tailscale Funnel port 8443) at every start (1/0); the password's\n"
            << ";   PBKDF2 hash (never the password), the login-signing secret, new phones from outside also need 允許 (1/0)\n"
            << "remote_persistent=" << (remotePersistent ? 1 : 0) << "\n"
            << (remotePwHash.empty() ? std::string() : "remote_pw_hash=" + remotePwHash + "\n")
            << (remoteFixedSecret.empty() ? std::string() : "remote_fixed_secret=" + remoteFixedSecret + "\n")
            << "remote_fixed_approval=" << (remotePolicyFixed != 2 ? 1 : 0) << "\n"
            << "; 0.8.3.6 when someone connects from outside: ask (every time) | remember (Allow can tick Remember) |\n"
            << ";   password (the right password / link + passcode is enough): your account on the fixed address, guest\n"
            << ";   accounts, the temporary address (LAN / Tailscale always ask, with Remember)\n"
            << "remote_policy_fixed=" << policyKey(remotePolicyFixed) << "\n"
            << "remote_policy_guest=" << policyKey(remotePolicyGuest) << "\n"
            << "remote_policy_temp=" << policyKey(remotePolicyTemp) << "\n"
            << "; 0.8.3: fixed-link logins ended by 中斷 (token id:expiry, no secrets); lock the PC when a remote session ends:\n"
            << ";   from outside (fixed link / no-app link) and on the LAN / Tailscale (1/0)\n"
            << (remoteRevoked.empty() ? std::string() : "remote_revoked=" + remoteRevoked + "\n")
            << "remote_lock_outside=" << (remoteLockOutside ? 1 : 0) << "\n"
            << "remote_lock_lan=" << (remoteLockLan ? 1 : 0) << "\n"
            << (whatsNewSeen.empty() ? std::string() : "whats_new_seen=" + whatsNewSeen + "\n")
            << "; main window page: cast (手機投影) or remote (遠端控制)\n"
            << "idle_tab=" << (idleTab == 1 ? "remote" : "cast") << "\n"
            << "; UI language: auto (Windows display language: zh-* -> zh-TW, ja-* -> ja, ko-* -> ko, else en), zh-TW, en, ja, ko\n"
            << "language=" << languageKey(language) << "\n"
            << "; magnifier colours: none, contrast, gray, invert, yellow (yellow text on black)\n"
            << "filter=" << kFilterKeys[filter % 5] << "\n"
            << "; on-screen translation target: auto (UI language), zh-Hant, en, ja, ko\n"
            << "translate_to=" << kTranslateKeys[translateTo % 5] << "\n"
            << "; translations shown: auto (in place, a numbered list when over 30 % do not fit), inplace, list\n"
            << "translate_layout=" << kLayoutKeys[translateLayout % 3] << "\n"
            << "; translation boxes: lens (the picture's own colours), dark (dark boxes, easier to read)\n"
            << "translate_cards=" << (translateDark ? "dark" : "lens") << "\n"
            << "; source language of the last translation (its models are loaded ahead when a phone connects; empty: none)\n"
            << "translate_last=" << translateLast << "\n"
            << "; A/B test: 1 = also offered as an AirPlay speaker (default), 0 = Screen Mirroring only\n"
            << ";   (no audio-only AirPlay: features bit 9 off, no _raop._tcp), 2 = bit 9 on but no _raop._tcp\n"
            << "airplay_advertise_audio=" << advertiseAudio << "\n"
            << "; A/B test (iOS volume bar pops up while mirroring), applies at the next start:\n"
            << ";   airplay_volume_control: -1 = as before (default); 0..4 = tell the iPhone this volume control type\n"
            << ";   (0 = guess: fixed volume, the phone's volume buttons stay on the phone; 1 = up/down only; 3 = slider)\n"
            << ";   airplay_audio_status: 1 = as before (default); 0 = do not claim an audio output (may lose the sound)\n"
            << "airplay_volume_control=" << volumeControlType << "\n"
            << "airplay_audio_status=" << audioStatusFlag << "\n"
            << "; Android picture right click: back (the phone's Back) or menu (this window's menu); Shift+right click = menu\n"
            << "android_right_click=" << (rightClickMenu ? "menu" : "back") << "\n"
            << "right_click_hint_shown=" << (rightClickHintShown ? 1 : 0) << "\n"
            << "; 畫筆 (presentation pen): tool 0 pen / 1 highlighter / 2 arrow, colour 0..5 (red yellow blue green white black),\n"
            << ";   thickness 0..2, drawings in screenshots / recordings (1/0)\n"
            << "ink_tool=" << inkTool << "\n"
            << "ink_color=" << inkColor << "\n"
            << "ink_width=" << inkWidth << "\n"
            << "ink_in_screenshots=" << (inkInShots ? 1 : 0) << "\n"
            << "ink_in_recordings=" << (inkInRec ? 1 : 0) << "\n"
            << "; Android control: clipboard sync, phone screen off while controlling, keep the phone awake (1/0)\n"
            << "android_clipboard_sync=" << (androidClipSync ? 1 : 0) << "\n"
            << "android_screen_off=" << (androidScreenOff ? 1 : 0) << "\n"
            << "android_stay_awake=" << (androidStayAwake ? 1 : 0) << "\n";
        if (!displayName.empty())
            out << "; name shown to phones (empty / missing: 自在投影 in zh-TW, Zizai Cast in English)\n"
                << "display_name=" << toUtf8(displayName) << "\n";
        else
            out << "; display_name=<name shown to phones> overrides 自在投影 / Zizai Cast\n";
        if (!updatedTo.empty())
            out << "; an update to this version was just started (toast at the next start, then removed)\n"
                << "updated_to=" << updatedTo << "\n";
        if (!skipVersion.empty())
            out << "; skip_version: no update prompt for this version (略過這個版本; the menu still offers it)\n"
                << "skip_version=" << skipVersion << "\n";
        if (updateUrlSet)
            out << "; update manifest URL (empty = never check for updates)\n"
                << "update_url=" << updateUrl << "\n";
        else
            out << "; update_url=<manifest URL> overrides the default " << PM_UPDATE_URL_STR
                << " (update_url= with nothing turns update checks off)\n";
        out.flush();
        const bool written = static_cast<bool>(out);
        out.close();
        if (written) {  // on disk before the rename, so the rename never exposes an empty file
            HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) FlushFileBuffers(f), CloseHandle(f);
        }
        if (!written || !MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            DeleteFileW(tmp.c_str());  // the old settings.ini stays as it was
    }
    static bool validMac(const std::string& m) {
        if (m.size() != 17) return false;
        for (size_t i = 0; i < m.size(); ++i) {
            if (i % 3 == 2 ? m[i] != ':' : !isxdigit(static_cast<unsigned char>(m[i]))) return false;
        }
        return true;
    }
    static const char* qualityKey(int q) { return q == 0 ? "standard" : q == 2 ? "max" : "high"; }
    static const char* languageKey(int l) {
        return l == 1 ? "zh-TW" : l == 2 ? "en" : l == 3 ? "ja" : l == 4 ? "ko" : "auto";
    }
};

}  // namespace pm_app
