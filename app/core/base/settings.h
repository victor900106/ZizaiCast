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
    // 0.7.8 UX (p7): 右鍵行為 on an Android picture: false 返回 (default), true 開啟選單;
    // Shift+right click always opens the menu. The 「右鍵＝返回」 hint shows once.
    bool rightClickMenu = false;
    bool rightClickHintShown = false;

    static constexpr const char* kThemeKeys[4] = {"sakura", "mint", "night", "milktea"};
    static constexpr const char* kFilterKeys[5] = {"none", "contrast", "gray", "invert", "yellow"};
    static constexpr const char* kTranslateKeys[5] = {"auto", "zh-Hant", "en", "ja", "ko"};
    static constexpr const char* kLayoutKeys[3] = {"auto", "inplace", "list"};
    static constexpr const char* kCloseKeys[3] = {"ask", "tray", "quit"};

    // Manifest URL in effect: settings.ini update_url, else the build's default.
    std::string effectiveUpdateUrl() const { return updateUrlSet ? updateUrl : std::string(PM_UPDATE_URL_STR); }

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
        if (auto it = kv.find("android_right_click"); it != kv.end()) s.rightClickMenu = it->second == "menu";
        s.rightClickHintShown = flag("right_click_hint_shown", false);
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
            << "; Android picture right click: back (the phone's Back) or menu (this window's menu); Shift+right click = menu\n"
            << "android_right_click=" << (rightClickMenu ? "menu" : "back") << "\n"
            << "right_click_hint_shown=" << (rightClickHintShown ? 1 : 0) << "\n";
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
