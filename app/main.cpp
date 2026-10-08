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
#include <windows.h>
#include <commctrl.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include <algorithm>
#include <array>
#include <atomic>
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
#include "share_picker.h"
#include "updater.h"

namespace fs = std::filesystem;
using pm::i18n::S;
using I18n = pm::i18n::S;  // where a function has its own S (AndroidSource::State)
using pm::i18n::fmt;
using pm::i18n::tr;

namespace {

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

fs::path knownFolder(REFKNOWNFOLDERID id) {
    PWSTR base = nullptr;
    fs::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &base))) dir = base;
    CoTaskMemFree(base);
    return dir;
}

fs::path dataDir(bool dev) {
    const wchar_t* leaf = dev ? L"PhoneMirror-dev" : L"PhoneMirror";
    fs::path dir = knownFolder(FOLDERID_LocalAppData);
    dir = dir.empty() ? fs::temp_directory_path() / leaf : dir / leaf;
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

fs::path exePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

class Log {
public:
    explicit Log(const fs::path& file) : out_((rotate(file), file), std::ios::app) {}
    // Over 8 MB at start: kept as phonemirror.old.log (the video module logs
    // a summary line every 5 s while mirroring).
    static void rotate(const fs::path& file) {
        std::error_code ec;
        const auto size = fs::file_size(file, ec);
        if (ec || size < (8u << 20)) return;
        fs::path old = file;
        old.replace_extension(L".old.log");
        fs::rename(file, old, ec);
    }
    void write(const char* level, const std::string& msg) {
        std::lock_guard<std::mutex> lock(mu_);
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        char ts[32];
        std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
        std::string line = std::string(ts) + " [" + level + "] " + msg + "\n";
        out_ << line;
        out_.flush();
        OutputDebugStringA(line.c_str());
    }

private:
    std::mutex mu_;
    std::ofstream out_;
};

// ---------------------------------------------------------------------------
// Settings: %LOCALAPPDATA%\PhoneMirror\settings.ini, UTF-8 "key=value" lines.
// (The phone volume stays in volume.txt, see RememberVolumeAudioSink.)
struct Settings {
    bool requirePin = false;
    bool topmost = false;
    bool trayHintShown = false;
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
    bool tutorialShown = false;  // 使用教學 opened once at the first start
    bool autoShare = false;      // 截圖／錄影後自動傳到手機 (docs/share.md)
    std::string updatedTo;     // set just before an update installs: 「已更新到 vX」 at the next start
    std::string skipVersion;   // 略過這個版本: never prompted again (the menu item stays)
    int language = 0;          // 語言 / Language: 0 auto (Windows UI language), 1 zh-TW, 2 en, 3 ja, 4 ko
    std::wstring displayName;  // name shown to phones (empty: 自在投影 / Zizai Cast by language)
    int filter = 0;            // 放大鏡 colours: 0 none 1 contrast 2 gray 3 invert 4 yellow (VideoWindow::Filter)
    int translateTo = 0;       // 翻譯 翻成: 0 auto (UI language), 1 繁體中文, 2 English, 3 日本語, 4 한국어
    int translateLayout = 0;   // 翻譯顯示方式: 0 automatic, 1 原位顯示, 2 清單顯示 (0.7.1)
    bool translateDark = false;  // 深色方框 instead of the picture's own colours (0.7.1)

    static constexpr const char* kThemeKeys[4] = {"sakura", "mint", "night", "milktea"};
    static constexpr const char* kFilterKeys[5] = {"none", "contrast", "gray", "invert", "yellow"};
    static constexpr const char* kTranslateKeys[5] = {"auto", "zh-Hant", "en", "ja", "ko"};
    static constexpr const char* kLayoutKeys[3] = {"auto", "inplace", "list"};

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
        s.tutorialShown = flag("tutorial_shown", false);
        s.autoShare = flag("auto_share", false);
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
    void save(const fs::path& file) const {
        std::ofstream out(file, std::ios::trunc);
        out << "[settings]\n"
            << "require_pin=" << (requirePin ? 1 : 0) << "\n"
            << "topmost=" << (topmost ? 1 : 0) << "\n"
            << "tray_hint_shown=" << (trayHintShown ? 1 : 0) << "\n"
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
            << "tutorial_shown=" << (tutorialShown ? 1 : 0) << "\n"
            << "; send every new screenshot / recording to the phone (Android over adb: its gallery; else a live QR page)\n"
            << "auto_share=" << (autoShare ? 1 : 0) << "\n"
            << "; UI language: auto (Windows display language: zh-* -> zh-TW, ja-* -> ja, ko-* -> ko, else en), zh-TW, en, ja, ko\n"
            << "language=" << languageKey(language) << "\n"
            << "; magnifier colours: none, contrast, gray, invert, yellow (yellow text on black)\n"
            << "filter=" << kFilterKeys[filter % 5] << "\n"
            << "; on-screen translation target: auto (UI language), zh-Hant, en, ja, ko\n"
            << "translate_to=" << kTranslateKeys[translateTo % 5] << "\n"
            << "; translations shown: auto (in place, a numbered list when over 30 % do not fit), inplace, list\n"
            << "translate_layout=" << kLayoutKeys[translateLayout % 3] << "\n"
            << "; translation boxes: lens (the picture's own colours), dark (dark boxes, easier to read)\n"
            << "translate_cards=" << (translateDark ? "dark" : "lens") << "\n";
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

// 語言 / Language: auto follows the Windows display language (zh-* -> 繁體中文,
// anything else -> English).
pm::i18n::Lang resolveLanguage(int pref) {
    switch (pref) {
    case 1: return pm::i18n::Lang::ZhTW;
    case 2: return pm::i18n::Lang::En;
    case 3: return pm::i18n::Lang::Ja;
    case 4: return pm::i18n::Lang::Ko;
    }
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_CHINESE: return pm::i18n::Lang::ZhTW;
    case LANG_JAPANESE: return pm::i18n::Lang::Ja;
    case LANG_KOREAN: return pm::i18n::Lang::Ko;
    default: return pm::i18n::Lang::En;
    }
}

// Texts from pm_miracast / pm_android (always Chinese) in the UI language:
// the table's Mod* entries hold the module's exact text, "{0}" standing for
// a variable part (device name, adb output …), which is kept as is.
std::wstring moduleText(const std::wstring& s) {
    if (pm::i18n::zh() || s.empty()) return s;
    for (int i = static_cast<int>(S::ModMiraOldWindows); i <= static_cast<int>(S::ModAndStopped); ++i) {
        const std::wstring zh = tr(static_cast<S>(i), pm::i18n::Lang::ZhTW);
        const size_t ph = zh.find(L"{0}");
        if (ph == std::wstring::npos) {
            if (s == zh) return tr(static_cast<S>(i));
            continue;
        }
        const std::wstring pre = zh.substr(0, ph), post = zh.substr(ph + 3);
        if (s.size() >= pre.size() + post.size() && s.compare(0, pre.size(), pre) == 0 &&
            s.compare(s.size() - post.size(), post.size(), post) == 0)
            return fmt(static_cast<S>(i), {s.substr(pre.size(), s.size() - pre.size() - post.size())});
    }
    return s;
}

// High/Highest stream H.265: is any HEVC decoder MFT (hardware or the
// "HEVC Video Extensions" software one) installed?
bool hevcDecoderAvailable() {
    const bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_HEVC};
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    const UINT32 flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE |
                         MFT_ENUM_FLAG_SORTANDFILTER;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &in, nullptr, &acts, &n))) n = 0;
    for (UINT32 i = 0; i < n; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    if (mf) MFShutdown();
    return n > 0;
}

// Autostart: HKCU\...\Run\自在投影 = "<exe>" --background. The registry is the
// source of truth (the installer's optional task writes the same value). The
// value name is an identifier shared with the installer, not a UI string.
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"自在投影";

std::wstring autostartCommand() { return L"\"" + exePath().wstring() + L"\" --background"; }

std::wstring readAutostart() {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS)
        return {};
    std::wstring v(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, v.data(), &bytes) != ERROR_SUCCESS)
        return {};
    v.resize(wcslen(v.c_str()));
    return v;
}

bool setAutostart(bool on) {
    if (!on) {
        LSTATUS st = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
        return st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND;
    }
    const std::wstring cmd = autostartCommand();
    return RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, cmd.c_str(),
                           (DWORD)((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

// The entry points at an exe that no longer exists (folder moved): repoint it
// at this exe. An entry for another existing copy is left alone.
void repairAutostart() {
    std::wstring v = readAutostart();
    if (v.empty()) return;
    std::wstring path = v;
    if (!path.empty() && path[0] == L'"') {
        auto end = path.find(L'"', 1);
        path = path.substr(1, end == std::wstring::npos ? std::wstring::npos : end - 1);
    }
    std::error_code ec;
    if (!fs::exists(path, ec)) setAutostart(true);
}

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

void postText(HWND h, EventKind kind, std::wstring text) {
    if (!h) return;
    auto* p = new std::wstring(std::move(text));
    if (!PostMessageW(h, WM_PM_EVENT, kind, reinterpret_cast<LPARAM>(p))) delete p;
}

void postSource(HWND h, SourceEvent kind, std::wstring text = {}) {
    if (!h) return;
    auto* p = new std::wstring(std::move(text));
    if (!PostMessageW(h, WM_PM_SRCEV, kind, reinterpret_cast<LPARAM>(p))) delete p;
}

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
std::atomic<int> g_active{SrcNone};
std::atomic<bool> g_takeoverNew{true};       // Settings::takeoverKeep == false
std::atomic<HWND> g_uiHwnd{nullptr};
pm::MiracastReceiver* g_miracast = nullptr;  // set in wWinMain, lives until exit
pm::AndroidSource* g_android = nullptr;

const wchar_t* sourceLabel(int s) {
    return s == SrcAirPlay ? L"AirPlay" : s == SrcMiracast ? L"Miracast" : s == SrcAndroid ? L"Android" : L"";
}

// Any thread. Takes the window for `me`: free → yes; held by another source
// → only if `force`. Stops a Miracast / Android owner right here (before the
// newcomer's first picture) and tells the UI thread.
bool claimSource(int me, bool force) {
    int cur = g_active.load();
    for (;;) {
        if (cur == me) return true;
        if (cur != SrcNone && !force) return false;
        if (g_active.compare_exchange_weak(cur, me)) break;
    }
    if (cur == SrcMiracast && g_miracast) g_miracast->disconnect();  // ends its cast (window->onReset)
    if (cur == SrcAndroid && g_android) g_android->stop();           // its resets are gated off now
    if (HWND h = g_uiHwnd.load()) PostMessageW(h, WM_PM_SOURCE, static_cast<WPARAM>(me), static_cast<LPARAM>(cur));
    return true;
}

// Gives the window back if `s` still owns it.
void releaseSource(int s) {
    int cur = s;
    g_active.compare_exchange_strong(cur, SrcNone);
}

// Video from AirPlay / Android: forwarded only while that source owns the
// window. A new stream (onCodec) claims a free window, or a busy one with
// takeover=new. The codec is replayed when a blocked stream gets the window.
class GateVideoSink final : public pm::VideoSink {
public:
    GateVideoSink(int me, pm::VideoSink& inner) : me_(me), inner_(inner) {}
    void onCodec(pm::VideoCodec c) override {
        codec_ = c;
        haveCodec_ = true;
        if (claimSource(me_, g_takeoverNew.load())) {
            synced_ = true;
            inner_.onCodec(c);
        } else {
            synced_ = false;
        }
    }
    void onFrame(const uint8_t* d, size_t n, uint64_t t) override {
        if (!pass()) return;
        inner_.onFrame(d, n, t);
    }
    void onSourceSize(int w, int h) override {
        if (pass()) inner_.onSourceSize(w, h);
    }
    void onReset() override {
        if (g_active.load() == me_) inner_.onReset();
        synced_ = false;
    }
    void onPaused(bool p) override {
        if (g_active.load() == me_) inner_.onPaused(p);
    }

private:
    bool pass() {
        if (g_active.load() != me_ && !(g_active.load() == SrcNone && claimSource(me_, false))) {
            synced_ = false;
            return false;
        }
        if (!synced_ && haveCodec_) inner_.onCodec(codec_);  // got the window mid-stream
        synced_ = true;
        return true;
    }
    const int me_;
    pm::VideoSink& inner_;
    std::atomic<bool> synced_{false}, haveCodec_{false};
    pm::VideoCodec codec_ = pm::VideoCodec::H264;
};

// Audio from AirPlay / Android: played while that source owns the window (or
// nobody does — AirPlay audio may start before its video). The last format is
// replayed when a blocked stream becomes audible.
class GateAudioSink final : public pm::AudioSink {
public:
    GateAudioSink(int me, pm::AudioSink& inner) : me_(me), inner_(inner) {}
    void onFormat(pm::AudioCodec c, int rate, int ch, int spf) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            fmt_ = {c, rate, ch, spf};
            haveFmt_ = true;
        }
        if (open()) {
            inner_.onFormat(c, rate, ch, spf);
            synced_ = true;
        } else {
            synced_ = false;
        }
    }
    void onPacket(const uint8_t* d, size_t n, uint64_t t) override {
        packets.fetch_add(1, std::memory_order_relaxed);
        if (!open()) {
            synced_ = false;
            return;
        }
        if (!synced_) {
            Fmt f;
            bool have;
            {
                std::lock_guard<std::mutex> lock(mu_);
                f = fmt_;
                have = haveFmt_;
            }
            if (!have) return;
            inner_.onFormat(f.c, f.rate, f.ch, f.spf);
            synced_ = true;
        }
        inner_.onPacket(d, n, t);
    }
    void onVolume(float db) override {
        if (open()) inner_.onVolume(db);
    }
    void onFlush() override {
        if (open()) inner_.onFlush();
    }
    std::atomic<long long> packets{0};  // every packet seen (liveness of the source)

private:
    bool open() const {
        const int a = g_active.load();
        return a == me_ || a == SrcNone;
    }
    struct Fmt {
        pm::AudioCodec c = pm::AudioCodec::AAC_ELD;
        int rate = 44100, ch = 2, spf = 480;
    };
    const int me_;
    pm::AudioSink& inner_;
    std::mutex mu_;
    Fmt fmt_;
    bool haveFmt_ = false;
    std::atomic<bool> synced_{false};
};

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// WM_PM_STATE values.
enum VideoState : WPARAM { StateIdle = 0, StateMirroring = 1, StatePaused = 2, StateLost = 3 };

// Forwards to the video window and tracks the mirroring state for the UI.
// An unexpected end of a session (anything but the phone's own "stop
// mirroring", which the core logs as "video_reset: RTP shutdown" right
// before onReset) does not drop to the idle screen at once: the last frame is
// held (StateLost) until the UI calls releaseHold() ~3 s later, or until the
// next session's video arrives.
class StatusVideoSink final : public pm::VideoSink {
public:
    StatusVideoSink(pm::VideoWindow& win, Log& log) : win_(win), log_(log) {}

    // Fed from the core's log callback (same thread, just before onReset).
    void noteCoreLog(const std::string& msg) {
        if (msg.rfind("video_reset: RTP shutdown", 0) == 0) cleanStopAt_ = nowMs();
    }

    void onCodec(pm::VideoCodec codec) override {
        releaseHold();
        log_.write("info", codec == pm::VideoCodec::H265 ? "video codec H.265" : "video codec H.264");
        win_.onCodec(codec);
    }
    void onFrame(const uint8_t* data, size_t len, uint64_t ntp) override {
        frames.fetch_add(1, std::memory_order_relaxed);
        if (holding_) releaseHold();
        if (!mirroring_.exchange(true)) {
            log_.write("info", "mirroring started");
            post(StateMirroring);
        } else if (paused_.exchange(false)) {  // pictures again without video_resume: not paused any more
            log_.write("info", "frames after video_pause: resumed");
            post(StateMirroring);
            win_.onPaused(false);
        }
        win_.onFrame(data, len, ntp);
    }
    void onSourceSize(int w, int h) override {
        releaseHold();
        log_.write("info", "source size " + std::to_string(w) + "x" + std::to_string(h));
        win_.onSourceSize(w, h);
    }
    void onReset() override {
        std::lock_guard<std::mutex> lock(mu_);
        if (holding_) return;  // keep showing the last frame; the UI ends the hold
        const bool wasPaused = paused_.exchange(false);
        if (mirroring_.exchange(false)) {
            const bool clean = nowMs() - cleanStopAt_.load() < 2000;
            log_.write("info", clean ? "mirroring stopped" : "mirroring lost (unexpected reset)");
            if (!clean && !wasPaused) {
                holding_ = true;
                post(StateLost);
                return;
            }
            post(StateIdle);
        }
        win_.onReset();
    }
    void onPaused(bool paused) override {
        paused_ = paused;
        if (mirroring_) post(paused ? StatePaused : StateMirroring);
        win_.onPaused(paused);
    }

    // UI thread: the phone went away without onReset (connection dropped).
    void lostWithoutReset() {
        std::lock_guard<std::mutex> lock(mu_);
        if (holding_ || !mirroring_.exchange(false)) return;
        log_.write("info", "mirroring lost (client disconnected)");
        if (paused_.exchange(false)) {
            post(StateIdle);
            win_.onReset();
            return;
        }
        holding_ = true;
        post(StateLost);
    }
    // Ends a hold: the window returns to the idle screen. Returns true if a
    // hold was active.
    bool releaseHold() {
        std::lock_guard<std::mutex> lock(mu_);
        if (!holding_) return false;
        holding_ = false;
        win_.onReset();
        return true;
    }
    // UI thread: drop a stale picture (e.g. after resume from sleep).
    void forceIdle() {
        std::lock_guard<std::mutex> lock(mu_);
        holding_ = false;
        paused_ = false;
        if (mirroring_.exchange(false)) log_.write("info", "mirroring dropped (stale after resume, or stopped by the user)");
        win_.onReset();
        post(StateIdle);
    }
    // UI thread: another source took the window. Forget the old stream's
    // state without touching the window (the newcomer's picture replaces
    // it), so the next stream's first frame counts as "mirroring started".
    void switchSource() {
        std::lock_guard<std::mutex> lock(mu_);
        holding_ = false;
        paused_ = false;
        mirroring_ = false;
    }
    bool mirroring() const { return mirroring_; }
    bool holding() const { return holding_; }
    std::atomic<long long> frames{0};  // every frame passed on (liveness watchdog)

private:
    void post(WPARAM state) {
        if (HWND h = reinterpret_cast<HWND>(win_.hwnd())) PostMessageW(h, WM_PM_STATE, state, 0);
    }
    pm::VideoWindow& win_;
    Log& log_;
    std::mutex mu_;
    std::atomic<bool> mirroring_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> holding_{false};
    std::atomic<long long> cleanStopAt_{-100000};
};

// Forwards to the real player and remembers the phone volume so the next
// connection starts where the user left it instead of at full volume.
class RememberVolumeAudioSink final : public pm::AudioSink {
public:
    RememberVolumeAudioSink(pm::AudioSink& inner, fs::path file) : inner_(inner), file_(std::move(file)) {}
    static float load(const fs::path& file, float fallback) {
        std::ifstream in(file);
        float db = fallback;
        if (in >> db && db <= 0.0f && db >= -30.0f) return db;
        return fallback;
    }
    void onFormat(pm::AudioCodec c, int rate, int ch, int spf) override { inner_.onFormat(c, rate, ch, spf); }
    void onPacket(const uint8_t* d, size_t n, uint64_t t) override { inner_.onPacket(d, n, t); }
    void onFlush() override { inner_.onFlush(); }
    void onVolume(float db) override {
        inner_.onVolume(db);
        if (db >= -30.0f && db <= 0.0f) {  // mute (-144) is not carried over
            std::ofstream out(file_, std::ios::trunc);
            out << db;
        }
    }

private:
    pm::AudioSink& inner_;
    fs::path file_;
};

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
};
// Translation model pairs (translate/src/models.inc) for 管理翻譯模型.
// "ocr": the PaddleOCR text recognition models (models\ocr, 0.7.0).
constexpr int kModelPairCount = 8;
constexpr const char* kModelPairs[kModelPairCount] = {"ocr", "ja-en", "ko-en", "zhHans-en", "zhHant-en", "en-zhHant", "en-ja", "en-ko"};
enum IdleOptionId : int { OptAutostart = 1, OptPin = 2 };

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

    // 傳到手機 (docs/share.md)
    pm::share::Server share;           // QR path: LAN page with the shared files
    pm::ui::SharePanel sharePanel;
    pm::ui::ShareChip shareChip;       // 「傳到手機」 after a screenshot / recording
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
    bool viewToolsOn = false;          // a picture was available at the last check (syncViewTools)
};
App g;
void refreshToolbar();
std::wstring currentTitle();
// While the phone has paused the stream: the title says whether a frozen picture is shown.
void refreshPausedTitle();
// --dev test feeds: bumped by 中斷連線 so a fake phone stops like a dropped one.
std::atomic<int> g_testKick{0};
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

void saveSettings() { g.settings.save(g.settingsFile); }

// ---- AirPlay server lifetime ---------------------------------------------
// Without an HEVC decoder only 標準 (H.264) works; the stored choice is kept
// so it applies once the decoder is installed.
int effectiveQuality() { return g.hevc ? g.settings.quality : 0; }

bool startServer(int attempts) {
    auto server = std::make_unique<pm::AirPlayServer>();
    Log* log = g.log;
    StatusVideoSink* status = g.status;
    server->setLogCallback([log, status](pm::AirPlayServer::LogLevel level, const std::string& msg) {
        static const char* names[] = {"error", "warn", "info", "debug"};
        log->write(names[static_cast<int>(level)], msg);
        if (status) status->noteCoreLog(msg);
    });
    pm::AirPlayServer::Options opts = g.opts;
    opts.requirePin = g.settings.requirePin;
    // 畫質: display size offered to the phone (High/Highest also turn H.265 on).
    using QP = pm::AirPlayServer::QualityPreset;
    pm::AirPlayServer::applyQualityPreset(
        opts, effectiveQuality() == 0 ? QP::Standard : effectiveQuality() == 2 ? QP::Highest : QP::High);
    opts.h265 = opts.h265 || g.opts.h265;  // --h265 forces HEVC at any size
    // Stable identity: VPN/virtual adapters must not change the deviceid.
    if (!g.settings.deviceId.empty()) opts.macAddress = g.settings.deviceId;
    // 新手機連線時：接手 / 保持目前
    using Takeover = decltype(opts.takeoverPolicy);
    opts.takeoverPolicy = g.settings.takeoverKeep ? Takeover::KeepCurrent : Takeover::NewReplacesOld;
    server->setOptions(opts);
    const HWND h = g.hwnd;
    pm::AirPlayServer::Events ev;
    ev.onClientConnecting = [h, log](const std::string& device, const std::string& model) {
        log->write("info", "client connecting: " + device + " (" + model + ")");
        postText(h, EvConnecting, toWide(device.empty() ? model : device));
    };
    ev.onClientDisconnected = [h, log]() {
        log->write("info", "client disconnected");
        postText(h, EvDisconnected, {});
    };
    ev.onPin = [h](const std::string& pin) { postText(h, EvPin, toWide(pin)); };
    ev.onTakeover = [h, log](const std::string& oldName, const std::string& newName) {
        log->write("info", "takeover: \"" + newName + "\" replaced \"" + oldName + "\"");
        postText(h, EvTakeover, toWide(newName));
    };
    server->setEvents(std::move(ev));
    // Ports may linger briefly after a restart; retry for a few seconds.
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (server->start(toUtf8(g.name), g.videoSink, g.audioSink)) {
            g.log->write("info", "ready on port " + std::to_string(server->port()) + ", device id " +
                                     server->deviceId() + ", display " + std::to_string(opts.width) + "x" +
                                     std::to_string(opts.height) + "@" + std::to_string(opts.maxFps) +
                                     (opts.h265 ? ", H.265" : ", H.264") + (opts.requirePin ? ", PIN required" : ""));
            if (g.settings.deviceId.empty() && Settings::validMac(server->deviceId())) {
                g.settings.deviceId = server->deviceId();  // first run: keep the core's choice
                saveSettings();
                g.log->write("info", "device_id stored: " + g.settings.deviceId);
            }
            std::string ifs;
            for (const std::string& i : server->advertisedInterfaces()) ifs += (ifs.empty() ? "" : ", ") + i;
            g.log->write("info", "advertised interfaces: " + (ifs.empty() ? std::string("(none yet)") : ifs));
            g.server = std::move(server);
            return true;
        }
        server->stop();
        if (attempt + 1 < attempts) Sleep(400);
    }
    return false;
}

void restartServer() {
    g.restartPending = false;
    if (g.testNoNetwork) return;  // --dev --test-no-network: never listen (e.g. after a language switch)
    g.log->write("info", "restarting AirPlay server (requirePin=" + std::to_string(g.settings.requirePin) +
                             ", quality=" + Settings::qualityKey(g.settings.quality) +
                             ", takeover=" + (g.settings.takeoverKeep ? "keep" : "new") + ")");
    if (g.server) g.server->stop();
    g.server.reset();
    const bool ok = startServer(10);
    if (!ok) {
        g.log->write("error", "AirPlay server failed to restart");
        g.window->showToast(tr(S::AirPlayRestartFail));
    }
}

// ---- window helpers --------------------------------------------------------
bool isFullscreen() { return !(GetWindowLongW(g.hwnd, GWL_STYLE) & WS_CAPTION); }

void bringToFront() {
    HWND h = g.hwnd;
    if (g.testOffscreen) {  // --dev --test-offscreen: never take the foreground / z-order
        if (!IsWindowVisible(h)) ShowWindow(h, SW_SHOWNOACTIVATE);
        return;
    }
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    else if (!IsWindowVisible(h)) ShowWindow(h, SW_SHOW);
    // The foreground lock may refuse SetForegroundWindow from a background
    // process; a brief topmost flip still raises the window above others.
    if (!g.settings.topmost) {
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    SetForegroundWindow(h);
}

void applyTopmost() {
    SetWindowPos(g.hwnd, g.settings.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Shape the picture wants (after 畫面 rotation and with the iPhone frame), from
// the video window. False before the first picture.
bool desiredAspect(int& w, int& h) {
    w = h = 0;
    g.window->desiredClientAspect(w, h);
    return w > 0 && h > 0;
}

// Moves/sizes the window to a client area of cw x ch around its centre,
// kept on the monitor's work area.
void setClientSizeCentred(HWND h, int cw, int ch) {
    const LONG style = GetWindowLongW(h, GWL_STYLE);
    RECT win{};
    GetWindowRect(h, &win);
    RECT want{0, 0, cw, ch};
    AdjustWindowRectExForDpi(&want, style, FALSE, GetWindowLongW(h, GWL_EXSTYLE), GetDpiForWindow(h));
    const int w = want.right - want.left, hgt = want.bottom - want.top;
    int x = (win.left + win.right) / 2 - w / 2, y = (win.top + win.bottom) / 2 - hgt / 2;
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
        const RECT& wa = mi.rcWork;
        x = (std::max)(static_cast<int>(wa.left), (std::min)(x, static_cast<int>(wa.right) - w));
        y = (std::max)(static_cast<int>(wa.top), (std::min)(y, static_cast<int>(wa.bottom) - hgt));
    }
    SetWindowPos(h, nullptr, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
}

bool windowResizable(HWND h) {
    return (GetWindowLongW(h, GWL_STYLE) & WS_CAPTION) && !IsZoomed(h) && !IsIconic(h);
}

// When the picture turns (the phone rotates, or 畫面 rotation), turn the
// window too: swap the client width/height around the window centre.
// Skipped while fullscreen (no caption) or maximized.
void followOrientation(HWND h) {
    int aw = 0, ah = 0;
    if (!desiredAspect(aw, ah)) return;
    const int orientation = aw > ah ? 2 : 1;
    if (orientation == g.lastOrientation) return;
    const bool first = g.lastOrientation == 0;
    g.lastOrientation = orientation;
    if (!windowResizable(h)) return;
    RECT client{};
    GetClientRect(h, &client);
    const int cw = client.right, ch = client.bottom;
    if (cw <= 0 || ch <= 0 || ((cw > ch) == (orientation == 2))) return;  // already matches
    if (first && orientation == 1) return;  // default window is already portrait
    setClientSizeCentred(h, ch, cw);
}

// After a 畫面 change by the user (rotate / flip / reset / iPhone frame): fit
// the window to the picture exactly, keeping its long side, so there are no
// bars around it.
void fitWindowToPicture(HWND h) {
    int aw = 0, ah = 0;
    if (!desiredAspect(aw, ah)) return;
    g.lastOrientation = aw > ah ? 2 : 1;
    if (!windowResizable(h)) return;
    RECT client{};
    GetClientRect(h, &client);
    const int longSide = (std::max)(client.right, client.bottom);
    if (longSide <= 0) return;
    int cw, ch;
    if (aw >= ah) {
        cw = longSide;
        ch = static_cast<int>(std::lround(static_cast<double>(longSide) * ah / aw));
    } else {
        ch = longSide;
        cw = static_cast<int>(std::lround(static_cast<double>(longSide) * aw / ah));
    }
    // Shrink (keeping the shape) if that would not fit on the work area.
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
        const int maxW = (mi.rcWork.right - mi.rcWork.left) * 95 / 100;
        const int maxH = (mi.rcWork.bottom - mi.rcWork.top) * 90 / 100;
        const double k = (std::min)({1.0, static_cast<double>(maxW) / cw, static_cast<double>(maxH) / ch});
        cw = static_cast<int>(cw * k);
        ch = static_cast<int>(ch * k);
    }
    if (std::abs(cw - client.right) <= 1 && std::abs(ch - client.bottom) <= 1) return;
    setClientSizeCentred(h, cw, ch);
}

// ---- tray icon -------------------------------------------------------------
void trayAdd() {
    if (g.testOffscreen) {  // --dev --test-offscreen: nothing on the user's taskbar either
        g.trayOk = false;
        return;
    }
    g.nid = {};
    g.nid.cbSize = sizeof(g.nid);
    g.nid.hWnd = g.hwnd;
    g.nid.uID = 1;
    g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g.nid.uCallbackMessage = WM_PM_TRAY;
    g.nid.hIcon = g.iconSmall;
    wcscpy_s(g.nid.szTip, fmt(S::TrayTip, {tr(S::AppName)}).substr(0, 127).c_str());
    g.trayOk = Shell_NotifyIconW(NIM_ADD, &g.nid) != FALSE;
    g.nid.uVersion = NOTIFYICON_VERSION_4;
    if (g.trayOk) Shell_NotifyIconW(NIM_SETVERSION, &g.nid);
}

// Language switch: the tooltip in the new language.
void trayRetip() {
    if (!g.trayOk) return;
    g.nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcscpy_s(g.nid.szTip, fmt(S::TrayTip, {tr(S::AppName)}).substr(0, 127).c_str());
    Shell_NotifyIconW(NIM_MODIFY, &g.nid);
    g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
}

void trayRemove() {
    if (!g.trayOk) return;
    Shell_NotifyIconW(NIM_DELETE, &g.nid);
    g.trayOk = false;
}

// `update`: the 「有新版本」 balloon (a click opens the dialog, WM_PM_TRAY).
void trayBalloon(const std::wstring& title, const std::wstring& text, bool update = false) {
    g.balloonUpdate = update;
    if (g.log)
        g.log->write("info", std::string(g.trayOk ? "tray balloon: " : "tray balloon (no tray icon): ") + toUtf8(title) +
                                 " | " + toUtf8(text));
    if (!g.trayOk) return;
    NOTIFYICONDATAW n = g.nid;
    n.uFlags = NIF_INFO;
    wcscpy_s(n.szInfoTitle, title.substr(0, 63).c_str());
    wcscpy_s(n.szInfo, text.substr(0, 255).c_str());
    n.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    n.hBalloonIcon = g.iconBig;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// Install-folder layout names shared with installer/zizai.iss (identifiers,
// the same in every UI language): 程式 (program files), 截圖, 錄影, 安裝檔.
const wchar_t kDirProgram[] = L"程式";
const wchar_t kDirShots[] = L"截圖";
const wchar_t kDirRecordings[] = L"錄影";
const wchar_t kDirInstallers[] = L"安裝檔";
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

// dir\自在投影_YYYYMMDD_HHMMSS<ext> (ZizaiCast_… in English), with _2, _3 ... if taken.
fs::path stampedFile(const fs::path& dir, const wchar_t* ext) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    wchar_t stamp[32];
    wcsftime(stamp, 32, L"%Y%m%d_%H%M%S", &tm);
    const std::wstring prefix = std::wstring(tr(S::FilePrefix)) + L"_";
    fs::path file = dir / (prefix + stamp + ext);
    std::error_code ec;
    for (int i = 2; fs::exists(file, ec); ++i) file = dir / (prefix + stamp + L"_" + std::to_wstring(i) + ext);
    return file;
}

void offerShare(const fs::path& file);  // 傳到手機 chip (below)

void takeSnapshot() {
    const fs::path file = stampedFile(screenshotDir(), L".png");
    // With the iPhone frame on, the snapshot shows the framed picture too.
    const bool ok = g.settings.deviceFrame ? g.window->saveSnapshotFramed(file.wstring())
                                           : g.window->saveSnapshot(file.wstring());
    if (ok) {
        g.log->write("info", std::string("snapshot ") + (g.settings.deviceFrame ? "(framed) " : "") +
                                 toUtf8(file.wstring()));
        g.window->showToast(tr(S::ShotSaved));
        g.lastShot = file;
        offerShare(file);
    } else {
        g.window->showToast(tr(S::ShotNone));
    }
}

// ---- options (tray menu, context menu and idle screen stay in sync) -------
void refreshIdleOptions() {
    std::vector<pm::VideoWindow::IdleOption> opts;
    opts.push_back({OptAutostart, tr(S::OptAutostart), g.autostart});
    opts.push_back({OptPin, tr(S::OptPin), g.settings.requirePin});
    const HWND h = g.hwnd;
    // The callback runs inside the window's click handling: only post, so
    // that setIdleOptions() is never re-entered from its own callback.
    g.window->setIdleOptions(std::move(opts), [h](int id, bool checked) {
        PostMessageW(h, WM_PM_OPTION, static_cast<WPARAM>(id), checked ? 1 : 0);
    });
}

void setAutostartOption(bool on) {
    if (setAutostart(on)) {
        g.autostart = on;
        g.window->showToast(tr(on ? S::AutostartOn : S::AutostartOff));
    } else {
        g.window->showToast(tr(S::AutostartFail));
    }
    g.autostart = !readAutostart().empty();
    refreshIdleOptions();
}

void setPinOption(bool on) {
    if (on != g.settings.requirePin) {
        g.settings.requirePin = on;
        saveSettings();
        if (g.sessionActive) {
            g.restartPending = true;
            g.window->showToast(tr(S::PinDeferred));
        } else {
            restartServer();
            g.window->showToast(tr(on ? S::PinOn : S::PinOff));
        }
    }
    refreshIdleOptions();
}

const wchar_t* qualityLabel(int q) {
    return tr(q == 0 ? S::QualityStdLabel : q == 2 ? S::QualityMaxLabel : S::QualityHighLabel);
}

void setQuality(int q) {
    if (q < 0 || q > 2 || q == g.settings.quality) return;
    if (q > 0 && !g.hevc) {
        g.window->showToast(tr(S::NeedHevc));
        return;
    }
    g.settings.quality = q;
    saveSettings();
    if (g.sessionActive) {
        g.restartPending = true;
        g.window->showToast(tr(S::QualityDeferred));
    } else {
        restartServer();
        g.window->showToast(fmt(S::QualityToast, {qualityLabel(q)}));
    }
}


// Audio path latency the picture should wait for: jitter buffer + device buffer.
int audioLatencyMs() {
    if (!g.audio) return 0;
    const pm::AudioStats st = g.audio->stats();
    return static_cast<int>(st.bufferMs + st.deviceBufferMs + 0.5);
}

void applySyncMode() {
    if (g.settings.avSync) {
        g.window->setSyncMode(true, audioLatencyMs());
        SetTimer(g.hwnd, kSyncTimer, 1000, nullptr);
    } else {
        KillTimer(g.hwnd, kSyncTimer);
        g.window->setSyncMode(false, 0);
    }
}

void setAvSync(bool on) {
    if (on == g.settings.avSync) return;
    g.settings.avSync = on;
    saveSettings();
    g.log->write("info", std::string("av sync ") + (on ? "on" : "off"));
    applySyncMode();
    g.window->showToast(tr(on ? S::AvSyncOn : S::AvSyncOff));
}

void setTopmost(bool on) {
    g.settings.topmost = on;
    saveSettings();
    applyTopmost();
    g.window->showToast(tr(on ? S::TopmostOn : S::TopmostOff));
}

// ---- 錄影 ---------------------------------------------------------------------
bool recording() { return !g.recordingFile.empty(); }

bool pictureShowing() { return g.videoState == StateMirroring || g.videoState == StatePaused; }

unsigned long long freeBytes(const fs::path& dir) {
    ULARGE_INTEGER avail{};
    if (!GetDiskFreeSpaceExW(dir.c_str(), &avail, nullptr, nullptr)) return ~0ull;  // unknown: do not block
    return avail.QuadPart;
}

// Detaches the taps and finalizes the MP4. `toast` (if not empty) replaces
// the default 「錄影已儲存：file」.
void stopRecording(const std::wstring& toast = {}) {
    if (!recording()) return;
    KillTimer(g.hwnd, kRecordTimer);
    g.window->setFrameTap(nullptr);
    if (g.audio) g.audio->setPcmMonitor(nullptr);
    g.recorder->stop();
    g.window->setRecording(false);
    const fs::path file = g.recordingFile;
    g.recordingFile.clear();
    g.recordingStoppedAtMs = nowMs();
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    g.log->write("info", "recording stopped: " + toUtf8(file.wstring()) +
                             (ec ? std::string(" (no file)") : " (" + std::to_string(size / 1024) + " KB)"));
    g.window->showToast(toast.empty() ? fmt(S::RecSaved, {file.filename().wstring()}) : toast);
    if (!ec && size > 0) {
        g.lastRec = file;
        if (!g.quitting) offerShare(file);
    }
}

void startRecording() {
    if (recording()) return;
    if (!pictureShowing()) {
        g.window->showToast(tr(S::RecNeedPicture));
        return;
    }
    const fs::path dir = recordingDir();
    if (freeBytes(dir) < kMinFreeBytes) {
        g.window->showToast(tr(S::RecNoSpace));
        return;
    }
    const fs::path file = stampedFile(dir, L".mp4");
    if (!g.recorder->start(file.wstring())) {
        g.log->write("error", "recording failed to start: " + toUtf8(file.wstring()));
        g.window->showToast(tr(S::RecStartFail));
        return;
    }
    g.recordingFile = file;
    pm::Recorder* rec = g.recorder;
    g.window->setFrameTap([rec](const uint8_t* nv12, int w, int h, int stride, uint64_t ptsNs) {
        rec->onVideoFrame(nv12, w, h, stride, ptsNs);
    });
    if (g.audio)
        g.audio->setPcmMonitor([rec](const int16_t* pcm, size_t frames, int ch, int rate, uint64_t whenNs) {
            rec->onPcm(pcm, frames, ch, rate, whenNs);
        });
    g.window->setRecording(true);
    SetTimer(g.hwnd, kRecordTimer, 1000, nullptr);
    g.log->write("info", "recording started: " + toUtf8(file.wstring()));
    g.window->showToast(fmt(S::RecStarted, {file.filename().wstring()}));
}

// Every second while recording: the encoder gave up (write error, disk
// full, encoder failure) or the disk is almost full -> stop with a toast.
void checkRecording() {
    if (!recording()) {
        KillTimer(g.hwnd, kRecordTimer);
        return;
    }
    if (!g.recorder->active()) {
        g.log->write("error", "recorder stopped by itself (encoder / write failure)");
        stopRecording(fmt(S::RecError, {g.recordingFile.filename().wstring()}));
        return;
    }
    if (freeBytes(g.recordingFile.parent_path()) < kMinFreeBytes) {
        g.log->write("warn", "disk almost full: stopping the recording");
        stopRecording(fmt(S::RecDiskFull, {g.recordingFile.filename().wstring()}));
    }
}

void toggleRecording() {
    if (recording()) stopRecording();
    else startRecording();
}

// ---- 畫面: rotation / flip / iPhone frame ----------------------------------------
void applyView(bool refit) {
    g.window->setRotation(g.settings.rotation);
    g.window->setMirrored(g.settings.mirrored);
    g.window->setDeviceFrame(g.settings.deviceFrame);
    // Give the renderer a moment to take the new shape before fitting to it.
    if (refit) SetTimer(g.hwnd, kFitTimer, 60, nullptr);
}

void rotateView(int quarterTurns) {
    g.settings.rotation = (g.settings.rotation + quarterTurns + 4) % 4;
    saveSettings();
    applyView(true);
    g.window->showToast(fmt(S::RotateToast, {std::to_wstring(g.settings.rotation * 90)}));
}

void toggleMirror() {
    g.settings.mirrored = !g.settings.mirrored;
    saveSettings();
    applyView(false);
    g.window->showToast(tr(g.settings.mirrored ? S::FlipOn : S::FlipOff));
}

void resetView() {
    const bool changed = g.settings.rotation != 0 || g.settings.mirrored;
    g.settings.rotation = 0;
    g.settings.mirrored = false;
    saveSettings();
    g.window->resetMagnifier();  // 還原 also ends the magnifier (zoom 1×)
    applyView(changed);
    g.window->showToast(tr(S::ViewResetToast));
}

void toggleDeviceFrame() {
    g.settings.deviceFrame = !g.settings.deviceFrame;
    saveSettings();
    applyView(true);
    g.window->showToast(tr(g.settings.deviceFrame ? S::FrameOn : S::FrameOff));
}

// ---- 主題 -------------------------------------------------------------------------
constexpr pm::VideoWindow::Theme kThemes[4] = {pm::VideoWindow::Theme::Sakura, pm::VideoWindow::Theme::Mint,
                                               pm::VideoWindow::Theme::Night, pm::VideoWindow::Theme::MilkTea};
constexpr S kThemeNames[4] = {S::ThemeSakura, S::ThemeMint, S::ThemeNight, S::ThemeMilkTea};

// Idle screen / overlays (video) and the popup menus follow the theme.
void applyTheme() {
    const pm::VideoWindow::Theme t = kThemes[g.settings.theme & 3];
    g.window->setTheme(t);
    const std::array<uint32_t, 3> sw = pm::VideoWindow::themeSwatch(t);
    pm::ui::setPalette(sw[0], sw[1], sw[2]);
    g.pairPanel.retheme();
    g.aboutPanel.retheme();
    g.updatePanel.retheme();
    g.sharePanel.retheme();
    g.shareChip.retheme();
    g.sharePicker.retheme();
    g.askPanel.retheme();
}

void setTheme(int t) {
    if (t < 0 || t > 3) return;
    const bool changed = t != g.settings.theme;
    g.settings.theme = t;
    if (changed) saveSettings();
    applyTheme();
    g.window->showToast(fmt(S::ThemeToast, {tr(kThemeNames[t])}));
}

// ---- 新手機連線時：接手 / 保持目前 --------------------------------------------------
// The policy is an AirPlayServer option, read at start(): restart the server
// now, or after the current session like the PIN / 畫質 options.
void setTakeover(bool keep) {
    if (keep == g.settings.takeoverKeep) return;
    g.settings.takeoverKeep = keep;
    g_takeoverNew = !keep;  // Miracast / Android arbitration: at once
    saveSettings();
    const wchar_t* what = tr(keep ? S::TakeoverKeepToast : S::TakeoverNewToast);
    if (g.sessionActive) {
        g.restartPending = true;
        g.window->showToast(fmt(S::AppliesAfter, {what}));
    } else {
        restartServer();
        g.window->showToast(what);
    }
}

// ---- 放大鏡 / 翻譯 (0.7) ----------------------------------------------------------
// Magnifier, colour filters, freeze (pm_video) and on-screen translation
// (pm_translate, docs/translate.md). Only while a phone picture is shown: the
// menu items are greyed otherwise, the shortcuts do nothing, and when the
// picture ends the translation closes and the zoom goes back to 1× (the colour
// filter is a viewing preference: settings.ini filter=).
constexpr pm::VideoWindow::Filter kFilters[5] = {pm::VideoWindow::Filter::None, pm::VideoWindow::Filter::Contrast,
                                                 pm::VideoWindow::Filter::Grayscale, pm::VideoWindow::Filter::Invert,
                                                 pm::VideoWindow::Filter::YellowOnBlack};
constexpr S kFilterNames[5] = {S::FilterNone, S::FilterContrast, S::FilterGray, S::FilterInvert, S::FilterYellow};
constexpr int kLiveSeconds = 5;  // 連續翻譯: every 5 s

// A phone picture is on screen (AirPlay / Android through the status sink, or
// a Miracast cast, which draws into the window itself).
bool viewAvailable() {
    if (g.videoState == StateMirroring || g.videoState == StatePaused) return g_active.load() != SrcNone;
    return g_active.load() == SrcMiracast && g.liveSource == SrcMiracast;
}

pm::translate::Lang translateTarget() {
    using L = pm::translate::Lang;
    switch (g.settings.translateTo) {
    case 1: return L::ZhHant;
    case 2: return L::En;
    case 3: return L::Ja;
    case 4: return L::Ko;
    default: return pm::translate::defaultTarget();
    }
}

bool translating() { return g.translator && g.translator->active(); }

std::wstring zoomText(float z) {
    wchar_t buf[16];
    swprintf_s(buf, std::fabs(z - std::round(z)) < 0.05f ? L"%.0f×" : L"%.1f×", z);
    return buf;
}

// The picture went away (or came): translation closed, zoom back to 1×.
void syncViewTools() {
    const bool on = viewAvailable();
    if (on == g.viewToolsOn) return;
    g.viewToolsOn = on;
    if (on) return;
    if (g.translator) g.translator->close();
    g.window->resetMagnifier();
    g.window->setFrozen(false);
}

// Menus / shortcuts / toolbar: nothing to act on without a picture.
bool needPicture() {
    if (viewAvailable()) return true;
    g.window->showToast(tr(S::MenuNeedPicture));
    return false;
}

void zoomBy(int steps) {
    if (needPicture()) g.window->zoomStep(steps);
}

void zoomReset() {
    if (needPicture()) g.window->resetMagnifier();
}

// Toolbar 放大鏡: 1× → 2× → 4× → 1× (fine steps: Ctrl+wheel / Ctrl+= / Ctrl+-).
void magnifierCycle() {
    if (!needPicture()) return;
    const float z = g.window->viewState().zoom;
    if (z < 1.95f) g.window->setZoom(2);
    else if (z < 3.95f) g.window->setZoom(4);
    else g.window->resetMagnifier();
}

void setFilterOption(int f, bool toast = true) {
    if (f < 0 || f > 4) return;
    if (f != g.settings.filter) {
        g.settings.filter = f;
        saveSettings();
    }
    g.window->setFilter(kFilters[f]);
    g.log->write("info", std::string("filter ") + Settings::kFilterKeys[f]);
    if (toast) g.window->showToast(fmt(S::FilterToast, {tr(kFilterNames[f])}));
}

void toggleFreeze() {
    if (!needPicture()) return;
    const bool on = !g.window->viewState().frozen;
    g.window->setFrozen(on);
    g.log->write("info", on ? "picture frozen" : "picture unfrozen");
    refreshPausedTitle();
}

void translateCommand(UINT cmd) {
    if (!g.translator) return;
    pm::translate::ScreenTranslator& t = *g.translator;
    switch (cmd) {
    case CmdTranslateToggle:
        if (t.active()) {
            t.close();
            break;
        }
        [[fallthrough]];
    case CmdTranslateScreen:
        if (!needPicture()) break;
        t.setTarget(translateTarget());
        g.log->write("info", "translate screen");
        t.translateScreen();
        break;
    case CmdTranslateRegion:
        if (!needPicture()) break;
        t.setTarget(translateTarget());
        g.log->write("info", "translate region (drag)");
        t.translateRegion();
        break;
    case CmdTranslateOriginal:
        if (t.active()) t.setShowOriginal(!t.showOriginal());
        break;
    case CmdTranslateLive:
        if (!t.live() && !needPicture()) break;
        t.setTarget(translateTarget());
        t.setLive(!t.live(), kLiveSeconds);
        g.log->write("info", t.live() ? "translate live on" : "translate live off");
        break;
    case CmdTranslateClose: t.close(); break;
    case CmdTrLayoutAuto:
    case CmdTrLayoutInPlace:
    case CmdTrLayoutList: {
        g.settings.translateLayout = static_cast<int>(cmd - CmdTrLayoutAuto);
        saveSettings();
        g.window->setTextOverlayStyle(g.settings.translateLayout, g.settings.translateDark);
        static const S names[3] = {S::MenuTrLayoutAuto, S::MenuTrLayoutInPlace, S::MenuTrLayoutList};
        g.window->showToast(fmt(S::TrLayoutToast, {tr(names[g.settings.translateLayout])}));
        g.log->write("info", std::string("translate layout ") + Settings::kLayoutKeys[g.settings.translateLayout]);
        break;
    }
    case CmdTrDarkCards:
        g.settings.translateDark = !g.settings.translateDark;
        saveSettings();
        g.window->setTextOverlayStyle(g.settings.translateLayout, g.settings.translateDark);
        g.log->write("info", g.settings.translateDark ? "translate cards dark" : "translate cards lens");
        break;
    case CmdTrTargetZh:
    case CmdTrTargetEn:
    case CmdTrTargetJa:
    case CmdTrTargetKo: {
        g.settings.translateTo = cmd == CmdTrTargetZh ? 1 : cmd == CmdTrTargetEn ? 2 : cmd == CmdTrTargetJa ? 3 : 4;
        saveSettings();
        t.setTarget(translateTarget());
        g.window->showToast(fmt(S::TrTargetToast, {pm::translate::langName(translateTarget())}));
        break;
    }
    }
    refreshToolbar();
}

// ---- 管理翻譯模型 ----
fs::path modelPairDir(int i) {
    if (std::string(kModelPairs[i]) == "ocr") return fs::path(pm::translate::ModelStore::root()) / L"ocr";
    return fs::path(pm::translate::ModelStore::root()) / L"bergamot" / kModelPairs[i];
}

unsigned long long dirBytes(const fs::path& d) {
    unsigned long long n = 0;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(d, ec))
        if (e.is_regular_file(ec)) n += e.file_size(ec);
    return n;
}

std::wstring mbText(unsigned long long b) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f MB", b / 1e6);
    return buf;
}

// 「日文 → 英文」 for "ja-en".
std::wstring pairLabel(int i) {
    using L = pm::translate::Lang;
    auto lang = [](const std::string& c) {
        return c == "ja" ? L::Ja : c == "ko" ? L::Ko : c == "en" ? L::En : c == "zhHans" ? L::ZhHans : L::ZhHant;
    };
    const std::string p = kModelPairs[i];
    if (p == "ocr") return tr(S::TrOcrModelLabel);
    const size_t dash = p.find('-');
    return pm::translate::langName(lang(p.substr(0, dash))) + L" → " + pm::translate::langName(lang(p.substr(dash + 1)));
}

void deleteModels(std::vector<int> pairs) {
    if (g.translator) g.translator->close();
    pm::translate::PaddleOcr::unload();  // the OCR model files are not kept open, but free them anyway
    unsigned long long freed = 0;
    bool failed = false;
    for (int i : pairs) {
        const fs::path d = modelPairDir(i);
        const unsigned long long before = dirBytes(d);
        std::error_code ec;
        fs::remove_all(d, ec);
        if (ec || fs::exists(d)) failed = true;
        freed += before - dirBytes(d);
        g.log->write("info", std::string("translation model ") + kModelPairs[i] + (ec ? " delete failed: " + ec.message() : " deleted"));
    }
    g.window->showToast(failed ? std::wstring(tr(S::TrDeleteFailed)) : fmt(S::TrDeleted, {mbText(freed)}), 4000);
}

void askDeleteModels(int pair) {  // -1: all
    std::vector<int> pairs;
    unsigned long long bytes = 0;
    for (int i = 0; i < kModelPairCount; ++i)
        if ((pair < 0 || pair == i) && dirBytes(modelPairDir(i)) > 0) pairs.push_back(i), bytes += dirBytes(modelPairDir(i));
    if (pairs.empty()) return;
    if (IsWindowVisible(g.hwnd) == FALSE) bringToFront();
    pm::ui::AskPanel::Info a;
    a.glyph = 0xE74D;  // Delete
    a.title = tr(S::TrDeleteTitle);
    a.body = pair < 0 ? fmt(S::TrDeleteAllAsk, {mbText(bytes)}) : fmt(S::TrDeleteAsk, {pairLabel(pair), mbText(bytes)});
    a.primary = tr(S::TrDeleteBtn);
    a.secondary = tr(S::Cancel);
    a.danger = true;
    a.done = [pairs](int choice) {
        g.log->write("info", std::string("delete translation models: ") + (choice == 1 ? "confirmed" : "cancelled"));
        if (choice == 1) deleteModels(pairs);
    };
    g.askPanel.open(g.hwnd, std::move(a));
}

void openModelsFolder() {
    const fs::path d = fs::path(pm::translate::ModelStore::root());
    std::error_code ec;
    fs::create_directories(d, ec);
    g.log->write("info", "models folder " + toUtf8(d.wstring()));
    if (!g.testOffscreen) ShellExecuteW(g.hwnd, L"open", d.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// pm_translate callbacks (UI thread): themed dialogs instead of MessageBox.
void translatorAskDownload(pm::translate::Lang src, pm::translate::Lang tgt, double mb, std::function<void(bool)> answer) {
    wchar_t size[32];
    swprintf_s(size, L"%.0f MB", mb);
    pm::ui::AskPanel::Info a;
    a.glyph = 0xE896;  // Download
    const bool ocr = src == pm::translate::Lang::Unknown;  // the text recognition models (PaddleOCR)
    a.title = tr(ocr ? S::TrOcrConsentTitle : S::TrConsentTitle);
    a.body = ocr ? std::wstring(tr(S::TrOcrConsentBody))
                 : fmt(S::TrConsentBody, {pm::translate::langName(src), pm::translate::langName(tgt)});
    a.rows = {{tr(S::TrConsentSize), size},
              {tr(S::TrConsentSource), tr(ocr ? S::TrOcrConsentSourceVal : S::TrConsentSourceVal)},
              {tr(S::TrConsentLicense), tr(ocr ? S::TrOcrConsentLicenseVal : S::TrConsentLicenseVal)},
              {tr(S::TrConsentPlace), pm::translate::ModelStore::root()}};
    a.primary = tr(S::TrConsentYes);
    a.secondary = tr(S::TrConsentNo);
    a.done = [answer, mb](int choice) {
        g.log->write("info", "model download consent (" + std::to_string(static_cast<int>(mb)) + " MB): " +
                                 (choice == 1 ? "yes" : "no"));
        answer(choice == 1);
    };
    g.log->write("info", ocr ? std::string("model download consent asked: OCR (PaddleOCR)")
                             : "model download consent asked: " + toUtf8(pm::translate::langName(src)) + " -> " +
                                   toUtf8(pm::translate::langName(tgt)));
    g.askPanel.open(g.hwnd, std::move(a));
}

void translatorNotify(const std::wstring& title, const std::wstring& text, bool important) {
    g.log->write("info", "translate: " + toUtf8(title) + " | " + toUtf8(text));
    if (!important) {
        g.window->showToast(text, 4000);
        return;
    }
    pm::ui::AskPanel::Info a;
    a.title = title;
    a.body = text;
    if (title == tr(S::TrOcrMissingTitle)) {  // add the OCR language in Windows Settings
        a.glyph = 0xF2B7;  // LocaleLanguage
        a.primary = tr(S::TrOpenLangSettings);
        a.secondary = tr(S::AboutClose);
        a.done = [](int choice) {
            if (choice != 1) return;
            g.log->write("info", "open ms-settings:regionlanguage");
            if (!g.testOffscreen) ShellExecuteW(g.hwnd, L"open", L"ms-settings:regionlanguage", nullptr, nullptr, SW_SHOWNORMAL);
        };
    } else {
        a.glyph = 0xE7BA;  // Warning
        a.primary = tr(S::DlgOk);
    }
    g.askPanel.open(g.hwnd, std::move(a));
}

void createTranslator() {
    pm::translate::ScreenTranslator::Callbacks cb;
    cb.askDownload = translatorAskDownload;
    cb.notify = translatorNotify;
    cb.changed = []() { refreshToolbar(); };
    cb.finished = [](bool ok, const pm::translate::ScreenTranslator::Timing& t) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "translate %s: %d lines, %d blocks, %d translated, ocr %.0f ms, translate %.0f ms, total %.0f ms",
                      ok ? "done" : "ended", t.lines, t.blocks, t.translated, t.ocrMs, t.translateMs, t.totalMs);
        g.log->write("info", buf);
        // Nothing to show (declined download, no text, OCR language missing):
        // do not leave the picture frozen with an empty overlay.
        if (!ok && g.translator && !g.translator->live() && !g.translator->busy()) g.translator->close();
    };
    g.translator = std::make_unique<pm::translate::ScreenTranslator>(*g.window, std::move(cb));
    g.window->setTextOverlayStyle(g.settings.translateLayout, g.settings.translateDark);
    g.translator->setTarget(translateTarget());
    std::wstring err;
    if (!pm::translate::Engine::available(&err)) g.log->write("warn", "translate: engine unavailable: " + toUtf8(err));
}

// ---- 自動更新 ---------------------------------------------------------------------
const char* kAppVersion = PM_APP_VERSION_STR;
// File names shared with the installer / the local-update folder (identifiers,
// not UI text): 自在投影-安裝程式-X.Y.Z.exe, %TEMP%\自在投影-更新-X.Y.Z.exe.
const wchar_t kInstallerPattern[] = L"自在投影-安裝程式-*.exe";
const wchar_t kUpdateTempPrefix[] = L"自在投影-更新-";

void scanLocalUpdates();
bool offerLocal();
void announceUpdate(bool manual = false);

bool windowHidden() { return !IsWindowVisible(g.hwnd) || IsIconic(g.hwnd); }

// A result of 檢查更新 the user must see: toast, plus a balloon when the
// window is in the tray (the toast is drawn in the hidden window).
void updateResultNote(const std::wstring& text) {
    g.log->write("info", "update check result shown: " + toUtf8(text));
    g.window->showToast(text, 5000);
    if (windowHidden()) trayBalloon(tr(S::AppName), text);
}

void checkForUpdates(bool manual) {
    if (manual) {  // 檢查更新: a newer installer in 安裝檔 answers at once
        scanLocalUpdates();
        if (offerLocal()) {
            g.updateNotified.clear();
            announceUpdate(true);
            return;
        }
    }
    const std::string url = g.settings.effectiveUpdateUrl();
    if (url.empty()) {
        if (manual) updateResultNote(tr(S::UpdDisabled));
        else announceUpdate();  // a local installer may still be on offer
        return;
    }
    if (g.updateChecking) {
        g.updateManual |= manual;
        return;
    }
    g.updateChecking = true;
    g.updateManual = manual;
    if (manual) g.window->showToast(tr(S::UpdChecking));
    g.log->write("info", "update check: " + url);
    const HWND h = g.hwnd;
    std::thread([h, url]() {
        auto* r = new UpdateResult;
        r->ok = pm::update::fetchManifest(toWide(url), r->manifest, r->error);
        if (!PostMessageW(h, WM_PM_UPDATE, UpdChecked, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

// The update on offer: the newer of an installer in <install>\安裝檔 (本機更新)
// and the manifest's (自動更新); local wins a tie (nothing to download).
bool offerLocal() {
    return !g.localVersion.empty() &&
           (g.update.version.empty() || pm::update::compareVersions(g.localVersion, g.update.version) >= 0);
}
std::string offerVersion() { return offerLocal() ? g.localVersion : g.update.version; }

void refreshIdleHints();
void installUpdate();

// A phone on screen (or connecting / held after a drop) or a recording: an
// automatic prompt must not cover the mirror.
int liveSourceNow();
bool updateBusy() { return liveSourceNow() != SrcNone || recording() || g.status->holding(); }

// The 「有新版本」 dialog for the offer (manifest or 安裝檔). `user`: opened by a
// click (menus, toolbar, idle pill, balloon, 檢查更新) -> takes the focus; an
// automatic prompt only does when the app window is in front.
void openUpdateDialog(bool user) {
    const std::string ver = offerVersion();
    if (ver.empty()) {
        if (user) bringToFront();
        return;
    }
    g.updatePending = false;
    const bool local = offerLocal();
    const pm::update::Manifest& m = local ? g.localInfo : g.update;
    pm::ui::UpdatePanel::Info info;
    info.current = toWide(kAppVersion);
    info.version = toWide(ver);
    info.local = local;
    info.date = toWide(m.date);
    info.size = local ? g.localSize : m.size;
    for (const std::string& c : m.changesZh) info.changesZh.push_back(toWide(c));
    for (const std::string& c : m.changesEn) info.changesEn.push_back(toWide(c));
    for (const std::string& c : m.changesJa) info.changesJa.push_back(toWide(c));
    for (const std::string& c : m.changesKo) info.changesKo.push_back(toWide(c));
    info.notes = toWide(m.notes);
    info.icon = g.iconBig;
    info.onInstall = [ver]() {
        g.log->write("info", "update dialog: install v" + ver);
        if (offerVersion() == ver) installUpdate();
    };
    info.onLater = [ver]() {  // no automatic prompt for 24 h (or until the next start)
        g.log->write("info", "update dialog: later v" + ver + " (snoozed 24 h)");
        g.snoozeVersion = ver;
        g.snoozeUntilMs = nowMs() + kUpdateSnoozeMs;
        g.updateNotified.clear();  // a check after the snooze may prompt again
    };
    info.onSkip = [ver]() {
        g.log->write("info", "update dialog: skip v" + ver + " (skip_version written)");
        g.settings.skipVersion = ver;
        saveSettings();
        g.window->showToast(fmt(S::UpdSkipped, {toWide(ver)}), 5000);
    };
    const bool activate = user || GetForegroundWindow() == g.hwnd;
    g.log->write("info", "update dialog: v" + ver + (user ? " (opened by the user)" : " (automatic)") +
                             (local ? ", local installer" : ", manifest") + ", " +
                             std::to_string(m.changesZh.size()) + " zh / " + std::to_string(m.changesEn.size()) +
                             " en changes");
    if (!g.updatePanel.open(g.hwnd, std::move(info), activate)) g.log->write("warn", "update dialog could not be created");
}

// After every message (appProc): a deferred prompt opens once the mirror /
// recording has ended and the window is on screen.
void maybeShowDeferredUpdate() {
    if (!g.updatePending || g.quitting || g.updatePanel.isOpen() || windowHidden() || updateBusy()) return;
    if (offerVersion().empty()) {
        g.updatePending = false;
        return;
    }
    g.log->write("info", "update dialog: deferred prompt now (mirroring ended / window shown)");
    openUpdateDialog(false);
}

// The offer changed or a check finished: the idle pill / menus / toolbar item
// follow the offer (refreshIdleHints, refreshToolbar); then, once per version
// per run, a prompt. `manual` (檢查更新) always shows the dialog. Automatic:
// not for skip_version, not while snoozed (稍後提醒), not before the first
// check (3 s after start); window in front and idle -> the dialog; phone live /
// recording -> toolbar + menu item, toast and balloon, dialog when it ends;
// window in the tray -> balloon 「點這裡看更新內容」 (click = dialog).
void announceUpdate(bool manual) {
    refreshIdleHints();
    const std::string ver = offerVersion();
    if (ver.empty()) {
        g.updatePending = false;
        g.updatePanel.close();
        return;
    }
    if (g.updatePanel.isOpen() && g.updatePanel.version() != toWide(ver)) openUpdateDialog(false);  // newer offer
    if (!manual) {
        if (!g.updatePromptReady || g.updateNotified == ver) return;
        if (ver == g.settings.skipVersion) {
            g.updateNotified = ver;
            g.log->write("info", "update offered: " + ver + " skipped (skip_version), no prompt");
            return;
        }
        if (ver == g.snoozeVersion && nowMs() < g.snoozeUntilMs) {
            g.log->write("info", "update offered: " + ver + " snoozed (later), no prompt");
            return;
        }
    }
    g.updateNotified = ver;
    const std::wstring v = toWide(ver);
    g.log->write("info", "update offered: " + ver + (offerLocal() ? " (local installer)" : " (manifest)") +
                             (manual ? ", manual check" : ""));
    const bool hidden = windowHidden(), busy = updateBusy();
    if (manual || (!hidden && !busy)) {
        openUpdateDialog(manual);
        return;
    }
    g.updatePending = true;
    g.log->write("info", std::string("update dialog deferred: ") + (busy ? "phone live / recording" : "window hidden"));
    if (!hidden) g.window->showToast(fmt(S::UpdAvailable, {v}), 5000);
    trayBalloon(fmt(S::UpdBalloonTitle, {tr(S::AppName), v}), tr(S::UpdBalloonClick), true);
}

void onUpdateChecked(const UpdateResult& r) {
    g.updateChecking = false;
    const bool manual = g.updateManual;
    g.updateManual = false;
    if (!r.ok) {
        g.log->write("warn", std::string("update check failed") + (manual ? " (manual)" : " (automatic, silent)") + ": " +
                                 r.error);
        if (manual) updateResultNote(tr(S::UpdCheckFail));
        else announceUpdate();  // a local installer may still be on offer
        return;
    }
    const int cmp = pm::update::compareVersions(r.manifest.version, kAppVersion);
    g.log->write("info", "update manifest: version " + r.manifest.version + " (running " + kAppVersion + ")" +
                             (cmp > 0 ? " -> update available" : ""));
    if (cmp <= 0) {
        g.update = {};
        if (manual && offerLocal()) {  // nothing newer online, but in 安裝檔
            g.updateNotified.clear();
            announceUpdate(true);
        } else {
            if (manual) updateResultNote(fmt(S::UpdLatest, {toWide(kAppVersion)}));
            announceUpdate();
        }
        return;
    }
    g.update = r.manifest;
    if (manual) g.updateNotified.clear();  // asked for it: say it again
    announceUpdate(manual);
}

bool confirmUpdateWhileRecording(const std::wstring& v);
void quitApp();
void installLocalUpdate();

// 更新到 vX.Y.Z (menus, idle button, toolbar). Recording: ask first (the
// recording is saved). Local installer: install it now. Manifest: download
// (worker thread) -> verify SHA-256 -> quit -> run the installer after the
// message loop (see runPendingInstaller).
void installUpdate() {
    const std::string ver = offerVersion();
    if (ver.empty() || g.updateDownloading) return;
    if (recording() && !confirmUpdateWhileRecording(toWide(ver))) return;
    if (offerLocal()) {
        installLocalUpdate();
        return;
    }
    g.updateDownloading = true;
    const std::wstring v = toWide(g.update.version);
    g.window->showToast(fmt(S::UpdDownloadingV, {v}));
    g.log->write("info", "downloading update " + g.update.version + " from " + g.update.url);
    const HWND h = g.hwnd;
    const pm::update::Manifest m = g.update;
    const fs::path file = fs::temp_directory_path() / (kUpdateTempPrefix + v + L".exe");
    std::thread([h, m, file]() {
        auto* r = new UpdateResult;
        r->manifest = m;
        r->file = file;
        r->ok = pm::update::download(toWide(m.url), file.wstring(), r->error);
        if (r->ok) {
            const std::string got = pm::update::sha256File(file.wstring());
            if (got != m.sha256) {
                r->ok = false;
                r->error = "sha256 mismatch: got " + (got.empty() ? std::string("(unreadable)") : got);
                DeleteFileW(file.c_str());
            }
        }
        if (!PostMessageW(h, WM_PM_UPDATE, UpdDownloaded, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

// Saves any recording, remembers the target version (「已更新到」 toast at
// the next start), quits; the installer starts after the message loop.
void beginInstall(const fs::path& installer, const std::string& version) {
    if (g.testNoInstall) {  // --dev updater test: stop before anything is installed
        g.log->write("info", "test-no-install: would install " + version + " from " + toUtf8(installer.wstring()));
        g.window->showToast(fmt(S::UpdTestReady, {toWide(version)}), 5000);
        return;
    }
    stopRecording();
    g.pendingInstaller = installer;
    g.relaunchHidden = !IsWindowVisible(g.hwnd);
    g.settings.updatedTo = version;
    saveSettings();
    g.window->showToast(fmt(S::UpdInstalling, {toWide(version)}));
    quitApp();
}

// 本機更新: the installer in 安裝檔 is checked again (still there, same
// version) and copied to %TEMP%\自在投影-更新-X.Y.Z.exe (the 安裝檔 copy
// stays unlocked, so it can be replaced meanwhile; the copy is removed at
// the next start like a downloaded one).
void installLocalUpdate() {
    const std::string ver = g.localVersion;
    const fs::path src = g.localInstaller;
    std::string now;
    if (!pm::update::installerVersion(src.wstring(), now) || now != ver) {
        g.log->write("warn", "local installer changed or gone: " + toUtf8(src.wstring()));
        g.window->showToast(tr(S::UpdLocalChanged));
        SetTimer(g.hwnd, kLocalSettleTimer, 200, nullptr);  // rescan
        return;
    }
    const fs::path tmp = fs::temp_directory_path() / (kUpdateTempPrefix + toWide(ver) + L".exe");
    std::error_code ec;
    fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        g.log->write("error", "could not copy " + toUtf8(src.wstring()) + ": " + ec.message());
        g.window->showToast(tr(S::UpdLocalUnreadable));
        return;
    }
    g.log->write("info", "local update " + ver + " from " + toUtf8(src.wstring()) + "; quitting to install");
    beginInstall(tmp, ver);
}

// <install>\安裝檔 (installed layout: exe in <install>\程式, created if
// missing; a dev build: next to the exe, only if it exists).
fs::path localUpdateDir() {
    const fs::path exeDir = exePath().parent_path();
    std::error_code ec;
    if (installedLayout(exeDir)) {
        const fs::path d = exeDir.parent_path() / kDirInstallers;
        fs::create_directories(d, ec);
        return fs::is_directory(d, ec) ? d : fs::path();
    }
    const fs::path d = exeDir / kDirInstallers;
    return fs::is_directory(d, ec) ? d : fs::path();
}

// 本機更新 scan (startup, every 10 min, after a change in the folder): the
// highest-version 「自在投影-安裝程式-*.exe」 whose VERSIONINFO says 自在投影
// and that is newer than this app. A file still being written (size / time
// changed within 2 s, or not openable exclusively) is skipped and the folder
// scanned again a second later.
void scanLocalUpdates() {
    KillTimer(g.hwnd, kLocalSettleTimer);
    if (g.localDir.empty()) return;
    const long long now = nowMs();
    std::map<std::wstring, App::LocalSeen> seen;
    bool unsettled = false;
    std::string bestV;
    fs::path best;
    WIN32_FIND_DATAW fd{};
    HANDLE f = FindFirstFileW((g.localDir / kInstallerPattern).c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring n = fd.cFileName;
            App::LocalSeen e;
            e.size = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            e.mtime = (static_cast<unsigned long long>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                      fd.ftLastWriteTime.dwLowDateTime;
            auto it = g.localSeen.find(n);
            if (it != g.localSeen.end() && it->second.size == e.size && it->second.mtime == e.mtime) e = it->second;
            else e.since = now;
            if (now - e.since < kLocalSettleMs) {
                unsettled = true;
            } else if (!e.checked) {
                const fs::path p = g.localDir / n;
                HANDLE x = CreateFileW(p.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (x == INVALID_HANDLE_VALUE) {
                    unsettled = true;  // still open for writing (copy in progress)
                } else {
                    CloseHandle(x);
                    e.checked = true;
                    if (!pm::update::installerVersion(p.wstring(), e.version)) e.version.clear();
                    g.log->write("info", "local installer " + toUtf8(n) + ": " +
                                             (e.version.empty() ? std::string("not a ZizaiCast installer")
                                                                : "version " + e.version));
                }
            }
            if (e.checked && !e.version.empty() &&
                (bestV.empty() || pm::update::compareVersions(e.version, bestV) > 0)) {
                bestV = e.version;
                best = g.localDir / n;
            }
            seen[n] = e;
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    g.localSeen.swap(seen);
    if (unsettled) SetTimer(g.hwnd, kLocalSettleTimer, 1000, nullptr);
    if (!bestV.empty() && pm::update::compareVersions(bestV, kAppVersion) <= 0) bestV.clear(), best.clear();
    if (bestV == g.localVersion && best == g.localInstaller) return;
    g.localVersion = bestV;
    g.localInstaller = best;
    // 「這次更新了什麼」 for a local installer: a sidecar <installer>.json (the
    // manifest schema) if it is there and names the same version (or none).
    g.localInfo = {};
    g.localSize = 0;
    if (!best.empty()) {
        std::error_code ec;
        g.localSize = fs::file_size(best, ec);
        if (ec) g.localSize = 0;
        pm::update::Manifest side;
        const fs::path sideFile = best.wstring() + L".json";
        if (pm::update::readManifestFile(sideFile.wstring(), side) && (side.version.empty() || side.version == bestV)) {
            g.localInfo = side;
            g.log->write("info", "local update: notes from " + toUtf8(sideFile.filename().wstring()) + " (" +
                                     std::to_string(side.changesZh.size()) + " zh / " +
                                     std::to_string(side.changesEn.size()) + " en changes)");
        }
    }
    g.log->write("info", bestV.empty() ? std::string("local update: none on offer")
                                       : "local update: " + bestV + " in " + toUtf8(best.wstring()));
    announceUpdate();
}

// Watcher thread: FindFirstChangeNotification on 安裝檔 -> WM_PM_LOCALDIR.
void startLocalWatch() {
    if (g.localDir.empty() || g.localWatchAlive) return;
    if (g.localWatch.joinable()) g.localWatch.join();
    HANDLE ch = FindFirstChangeNotificationW(
        g.localDir.c_str(), FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
    if (ch == INVALID_HANDLE_VALUE) {
        g.log->write("warn", "local update: cannot watch " + toUtf8(g.localDir.wstring()));
        return;
    }
    if (!g.localStop) g.localStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g.localWatchAlive = true;
    const HWND h = g.hwnd;
    g.localWatch = std::thread([ch, h, stop = g.localStop]() {
        HANDLE hs[2] = {stop, ch};
        for (;;) {
            if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
            PostMessageW(h, WM_PM_LOCALDIR, 0, 0);
            if (!FindNextChangeNotification(ch)) break;  // folder deleted
        }
        FindCloseChangeNotification(ch);
        g.localWatchAlive = false;
    });
}

void stopLocalWatch() {
    if (g.localStop) SetEvent(g.localStop);
    if (g.localWatch.joinable()) g.localWatch.join();
    if (g.localStop) CloseHandle(g.localStop);
    g.localStop = nullptr;
}

void onUpdateDownloaded(const UpdateResult& r) {
    g.updateDownloading = false;
    if (!r.ok) {
        g.log->write("error", "update download failed: " + r.error);
        g.window->showToast(tr(r.error.rfind("sha256", 0) == 0 ? S::UpdShaFail : S::UpdDownloadFail));
        return;
    }
    g.log->write("info", "update " + r.manifest.version + " verified (sha256 ok): " + toUtf8(r.file.wstring()) +
                             "; quitting to install");
    beginInstall(r.file, r.manifest.version);
}

// After the message loop: start the verified installer. Installed layout
// (<install>\程式\自在投影.exe): silent update into the same folder; the
// installer waits for this process to exit and starts the app again with
// the same --dev / --background. Anything else (a dev build): the normal
// installer UI.
void runPendingInstaller() {
    if (g.pendingInstaller.empty()) return;
    const fs::path exeDir = exePath().parent_path();
    std::wstring params;
    auto installerLang = []() -> std::wstring {
        switch (pm::i18n::lang()) {
        case pm::i18n::Lang::En: return L"english";
        case pm::i18n::Lang::Ja: return L"japanese";
        case pm::i18n::Lang::Ko: return L"korean";
        default: return L"chinesetrad";
        }
    };
    if (installedLayout(exeDir)) {
        // /LANG: the shortcuts / readme follow the app's UI language (0.6.0+ installers).
        // Scripted off-screen tests: /VERYSILENT (no progress window on the user's screen).
        params = std::wstring(g.testOffscreen ? L"/VERYSILENT" : L"/SILENT") +
                 L" /SUPPRESSMSGBOXES /NORESTART /NOCANCEL /UPDATE /DIR=\"" + exeDir.parent_path().wstring() +
                 L"\" /LANG=" + installerLang();
        std::wstring args;
        if (g.dev) args += L"--dev";
        if (g.relaunchHidden) args += std::wstring(args.empty() ? L"" : L" ") + L"--background";
        if (g.testOffscreen) args += L" --test-offscreen";  // --dev scripted update tests
        if (!args.empty()) params += L" /APPARGS=\"" + args + L"\"";
    }
    g.log->write("info", "starting installer " + toUtf8(g.pendingInstaller.wstring()) + " " + toUtf8(params));
    const HINSTANCE rc = ShellExecuteW(nullptr, L"open", g.pendingInstaller.c_str(),
                                       params.empty() ? nullptr : params.c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(rc) <= 32)
        g.log->write("error", "could not start the installer (" + std::to_string(reinterpret_cast<INT_PTR>(rc)) + ")");
}

void stopSharing();  // 傳到手機 (below)

void quitApp() {
    g.quitting = true;
    stopRecording();
    stopSharing();
    g.aboutPanel.close();
    g.updatePanel.close();
    g.askPanel.close();
    if (g.translator) g.translator->close();
    trayRemove();
    g.window->setLiveToolbar({}, nullptr);
    g.window->close();
}

void openPairPanel();
void openTutorial(const wchar_t* anchor);
void openAbout();
void setLanguage(int pref);
void setMiracastOption(bool on);
void setAndroidAuto(bool on);
void androidNav(UINT cmd);
void stopAndroid();
void disconnectLive();
void showContextMenu(int x, int y);
void shareCommand(UINT cmd);

void runCommand(UINT cmd) {
    switch (cmd) {
    case CmdAndroidPair: openPairPanel(); break;
    case CmdTutorial: openTutorial(nullptr); break;
    case CmdMiracast: setMiracastOption(!g.settings.miracast); break;
    case CmdAndroidAuto: setAndroidAuto(!g.settings.androidAuto); break;
    case CmdAndroidBack:
    case CmdAndroidHome:
    case CmdAndroidRecents: androidNav(cmd); break;
    case CmdAndroidStop: stopAndroid(); break;
    case CmdDisconnect: disconnectLive(); break;
    case CmdMoreMenu: {
        POINT pt{};
        GetCursorPos(&pt);
        showContextMenu(pt.x, pt.y);
        break;
    }
    case CmdMiracastHelp: openTutorial(L"miracast"); break;
    case CmdLangAuto:
    case CmdLangZh:
    case CmdLangEn: setLanguage(static_cast<int>(cmd - CmdLangAuto)); break;
    case CmdLangJa: setLanguage(3); break;
    case CmdLangKo: setLanguage(4); break;
    case CmdAbout: openAbout(); break;
    case CmdShareLast:
    case CmdShareLastShot:
    case CmdShareLastRec:
    case CmdSharePick:
    case CmdShareStop:
    case CmdShareAuto: shareCommand(cmd); break;
    case CmdShow: bringToFront(); break;
    case CmdAutostart: setAutostartOption(!g.autostart); break;
    case CmdPin: setPinOption(!g.settings.requirePin); break;
    case CmdOpenShots: openScreenshotDir(); break;
    case CmdExit: quitApp(); break;
    case CmdFullscreen:
        if (!IsWindowVisible(g.hwnd)) bringToFront();
        CallWindowProcW(g.prevProc, g.hwnd, WM_KEYDOWN, VK_F11, 0);  // the window's own toggle
        break;
    case CmdTopmost: setTopmost(!g.settings.topmost); break;
    case CmdSnapshot: takeSnapshot(); break;
    case CmdQualityStandard:
    case CmdQualityHigh:
    case CmdQualityMax: setQuality(static_cast<int>(cmd - CmdQualityStandard)); break;
    case CmdAvSync: setAvSync(!g.settings.avSync); break;
    case CmdRecord: toggleRecording(); break;
    case CmdOpenRecordings: openRecordingDir(); break;
    case CmdRotateRight: rotateView(+1); break;
    case CmdRotateLeft: rotateView(-1); break;
    case CmdMirror: toggleMirror(); break;
    case CmdResetView: resetView(); break;
    case CmdDeviceFrame: toggleDeviceFrame(); break;
    case CmdTheme0:
    case CmdTheme1:
    case CmdTheme2:
    case CmdTheme3: setTheme(static_cast<int>(cmd - CmdTheme0)); break;
    case CmdTakeoverNew: setTakeover(false); break;
    case CmdTakeoverKeep: setTakeover(true); break;
    case CmdCheckUpdate: checkForUpdates(true); break;
    case CmdInstallUpdate: installUpdate(); break;
    case CmdUpdateDialog: openUpdateDialog(true); break;
    case CmdZoomIn: zoomBy(+1); break;
    case CmdZoomOut: zoomBy(-1); break;
    case CmdZoomReset: zoomReset(); break;
    case CmdMagCycle: magnifierCycle(); break;
    case CmdFilter0:
    case CmdFilter1:
    case CmdFilter2:
    case CmdFilter3:
    case CmdFilter4: setFilterOption(static_cast<int>(cmd - CmdFilter0)); break;
    case CmdFilterCycle:
        if (needPicture()) setFilterOption((g.settings.filter + 1) % 5);
        break;
    case CmdFreeze: toggleFreeze(); break;
    case CmdTranslateToggle:
    case CmdTranslateScreen:
    case CmdTranslateRegion:
    case CmdTranslateOriginal:
    case CmdTranslateLive:
    case CmdTranslateClose:
    case CmdTrTargetZh:
    case CmdTrTargetEn:
    case CmdTrTargetJa:
    case CmdTrTargetKo:
    case CmdTrLayoutAuto:
    case CmdTrLayoutInPlace:
    case CmdTrLayoutList:
    case CmdTrDarkCards:
        // From the tray with the window hidden: show the picture being translated.
        if (cmd != CmdTrTargetZh && cmd != CmdTrTargetEn && cmd != CmdTrTargetJa && cmd != CmdTrTargetKo &&
            (cmd < CmdTrLayoutAuto || cmd > CmdTrDarkCards) && windowHidden() && viewAvailable())
            bringToFront();
        translateCommand(cmd);
        break;
    case CmdTrModelsFolder: openModelsFolder(); break;
    case CmdTrDeleteAll: askDeleteModels(-1); break;
    default:
        if (cmd >= CmdTrDelete0 && cmd < CmdTrDelete0 + kModelPairCount) askDeleteModels(static_cast<int>(cmd - CmdTrDelete0));
        break;
    }
}

// The source on screen (or an iPhone connecting / asking for its PIN), else
// SrcNone. Used for 中斷連線 (menus, toolbar, Ctrl+D).
int liveSourceNow() {
    const int a = g_active.load();
    if (a != SrcNone) return a;
    if (g.sessionActive) return SrcAirPlay;
    return SrcNone;
}

// 「Galaxy S24（Android）」 (short: 「Galaxy S24」) for menus and toasts; the
// source's label if no name is known.
std::wstring liveName(bool shortName = false) {
    if (!g.peerName.empty() && g.liveSource == liveSourceNow()) return shortName ? g.sourceName : g.peerName;
    const int s = liveSourceNow();
    return s == SrcAirPlay ? std::wstring(L"iPhone") : std::wstring(sourceLabel(s));
}

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

using pm::ui::MenuItem;

MenuItem checkItem(UINT id, const wchar_t* text, wchar_t icon, bool on, std::wstring right = {}) {
    MenuItem m = MenuItem::command(id, text, icon, std::move(right));
    m.checkable = true;
    m.checked = on;
    return m;
}

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

// Long explanations (Miracast reasons) as several note rows (menus do not wrap).
// English / 한국어 (spaces between words): word-wrapped at ~52 / ~34 characters.
void appendNotesRaw(std::vector<MenuItem>& v, const std::wstring& text, size_t width);
void appendNotes(std::vector<MenuItem>& v, const std::wstring& text, size_t width = 22) {
    const size_t first = v.size();
    appendNotesRaw(v, text, width);
    // A stub last row (「投影。」, one word) joins the row above (0.7.2).
    if (v.size() >= first + 2) {
        const std::wstring& last = v.back().text;
        const bool spaced = pm::i18n::en() || pm::i18n::lang() == pm::i18n::Lang::Ko;
        if (last.size() <= (spaced ? 12u : 4u)) {
            const std::wstring& prev = v[v.size() - 2].text;
            const bool latinEdge = (!prev.empty() && prev.back() < 0x80 && iswalnum(prev.back())) ||
                                   (last.front() < 0x80 && iswalnum(last.front()));
            std::wstring joined = prev + (spaced || latinEdge ? L" " : L"") + last;
            v.pop_back();
            v.back().text = std::move(joined);
        }
    }
}
void appendNotesRaw(std::vector<MenuItem>& v, const std::wstring& text, size_t width) {
    if (pm::i18n::en() || pm::i18n::lang() == pm::i18n::Lang::Ko) {
        const size_t maxLine = pm::i18n::en() ? 52 : 34;
        std::wstring line, word;
        auto flush = [&]() {
            if (!line.empty()) v.push_back(MenuItem::note(line));
            line.clear();
        };
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i < text.size() && text[i] != L' ') {
                word += text[i];
                continue;
            }
            if (!line.empty() && line.size() + 1 + word.size() > maxLine) flush();
            if (!word.empty()) line += (line.empty() ? L"" : L" ") + word;
            word.clear();
        }
        flush();
        return;
    }
    if (pm::i18n::lang() == pm::i18n::Lang::Ja) width = width * 3 / 2;  // kana run longer: break at 、。」 instead
    auto punct = [](wchar_t c) {
        return c == L'，' || c == L'：' || c == L'；' || c == L'。' || c == L'、' || c == L'）' || c == L'」';
    };  // ，：；。、）」
    const bool ja = pm::i18n::lang() == pm::i18n::Lang::Ja;
    auto emit = [&](std::wstring s) {
        while (!s.empty() && s.front() == L' ') s.erase(s.begin());
        while (!s.empty() && s.back() == L' ') s.pop_back();
        if (!s.empty()) v.push_back(MenuItem::note(s));
    };
    std::wstring line;
    size_t lastSpace = std::wstring::npos;  // in `line`: a break between two Latin words
    for (size_t i = 0; i < text.size(); ++i) {
        line += text[i];
        // 日本語 (0.7.2): also after a particle (「…」を / …には), a phrase end,
        // instead of in the middle of インストール|しました.
        const bool soft = punct(text[i]) || text[i] == L'→' ||
                          (ja && std::wstring(L"をがはにで").find(text[i]) != std::wstring::npos);
        if (text[i] == L' ') lastSpace = line.size();
        // Never start a row with punctuation (日本語: nor with a particle such as を / が).
        const bool nextPunct = i + 1 < text.size() &&
                               (punct(text[i + 1]) || (ja && std::wstring(L"をがはにでともの").find(text[i + 1]) != std::wstring::npos));
        if (nextPunct) continue;
        if (soft && line.size() >= width / 2) {
            emit(line);
            line.clear();
            lastSpace = std::wstring::npos;
        } else if (line.size() >= width) {
            // Full row: not inside a Latin word (「Andr|oid」) when a space is near (0.7.2).
            const bool inWord = i + 1 < text.size() && iswalnum(text[i]) && iswalnum(text[i + 1]) && text[i] < 0x80;
            if (inWord && lastSpace != std::wstring::npos && lastSpace >= width / 3) {
                emit(line.substr(0, lastSpace));
                line.erase(0, lastSpace);
            } else {
                emit(line);
                line.clear();
            }
            lastSpace = std::wstring::npos;
        }
    }
    while (!line.empty() && line.front() == L' ') line.erase(line.begin());
    if (!line.empty()) v.push_back(MenuItem::note(line));
}

// 開機自動啟動 / PIN / Miracast / Android check items, the 畫質 and
// 新手機連線時 radio groups (tray menu and 設定). 影音同步 is deliberately
// not offered (see docs/app.md).
void appendOptionItems(std::vector<MenuItem>& v) {
    v.push_back(checkItem(CmdAutostart, tr(S::OptAutostart), kIcoPower, g.autostart));
    v.push_back(checkItem(CmdPin, tr(S::OptPin), kIcoLock, g.settings.requirePin));
    MenuItem mc = checkItem(CmdMiracast, tr(S::OptMiracast), kIcoCast, g.settings.miracast && !g.miracastUnsupported);
    mc.enabled = g.miracast && !g.miracastUnsupported;
    v.push_back(std::move(mc));
    if (g.miracastUnsupported || (g.settings.miracast && !g.miracastReason.empty()))
        appendNotes(v, g.miracastReason.empty() ? std::wstring(tr(S::MiracastNoPc)) : moduleText(g.miracastReason));
    if (g.miracastUnsupported) v.push_back(MenuItem::command(CmdMiracastHelp, tr(S::MenuMiracastHelp), kIcoHelp));
    MenuItem aa = checkItem(CmdAndroidAuto, tr(S::OptAndroidAuto), kIcoConnect, g.settings.androidAuto);
    aa.enabled = g.androidOk;
    v.push_back(std::move(aa));
    v.push_back(MenuItem::separator());
    v.push_back(MenuItem::caption(tr(S::MenuQuality)));
    const wchar_t* names[3] = {tr(S::QualityStd), tr(S::QualityHigh), tr(S::QualityMax)};
    const wchar_t* sizes[3] = {L"1920×1080", L"2560×1440", L"3840×2160"};
    for (int q = 0; q < 3; ++q) {
        MenuItem m = MenuItem::command(CmdQualityStandard + q, names[q], 0, sizes[q]);
        m.radio = true;
        m.checked = effectiveQuality() == q;
        m.enabled = q == 0 || g.hevc;
        v.push_back(std::move(m));
    }
    if (!g.hevc) v.push_back(MenuItem::note(tr(S::QualityHevcNote)));
    v.push_back(MenuItem::separator());
    v.push_back(MenuItem::caption(tr(S::MenuTakeover)));
    MenuItem takeNew = MenuItem::command(CmdTakeoverNew, tr(S::TakeoverNew), 0, tr(S::TakeoverNewNote));
    takeNew.radio = true;
    takeNew.checked = !g.settings.takeoverKeep;
    v.push_back(std::move(takeNew));
    MenuItem keep = MenuItem::command(CmdTakeoverKeep, tr(S::TakeoverKeep), 0, tr(S::TakeoverKeepNote));
    keep.radio = true;
    keep.checked = g.settings.takeoverKeep;
    v.push_back(std::move(keep));
}

// 開始錄影 / 停止錄影 (only while a picture is shown, or to stop).
MenuItem recordItem() {
    const bool rec = recording();
    MenuItem m = MenuItem::command(CmdRecord, tr(rec ? S::MenuStopRec : S::MenuStartRec), rec ? kIcoStop : kIcoRecord,
                                   L"Ctrl+R");
    m.enabled = rec || pictureShowing();
    m.bold = rec;
    return m;
}

// ---- 傳到手機: which files ----
bool isShareImage(const fs::path& p) {
    std::wstring e = p.extension().wstring();
    for (auto& c : e) c = towlower(c);
    return e == L".png" || e == L".jpg" || e == L".jpeg";
}
bool isShareVideo(const fs::path& p) {
    std::wstring e = p.extension().wstring();
    for (auto& c : e) c = towlower(c);
    return e == L".mp4" || e == L".mov";
}
// Newest picture / video in `dir` (not the recording in progress).
fs::path newestIn(const fs::path& dir, bool video) {
    std::error_code ec;
    fs::path best;
    fs::file_time_type bestT{};
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec) || (video ? !isShareVideo(e.path()) : !isShareImage(e.path()))) continue;
        if (!g.recordingFile.empty() && e.path() == g.recordingFile) continue;
        const auto t = e.last_write_time(ec);
        if (best.empty() || t > bestT) best = e.path(), bestT = t;
    }
    return best;
}
fs::path lastShotFile() {
    std::error_code ec;
    if (!g.lastShot.empty() && fs::exists(g.lastShot, ec)) return g.lastShot;
    return newestIn(screenshotDir(), false);
}
fs::path lastRecFile() {
    std::error_code ec;
    if (!g.lastRec.empty() && fs::exists(g.lastRec, ec)) return g.lastRec;
    return newestIn(recordingDir(), true);
}
// Toolbar 傳到手機: the newer of the two.
fs::path lastShareable() {
    const fs::path a = lastShotFile(), b = lastRecFile();
    if (a.empty() || b.empty()) return a.empty() ? b : a;
    std::error_code e1, e2;
    return fs::last_write_time(b, e2) > fs::last_write_time(a, e1) ? b : a;
}

size_t unsentCount();  // 傳到手機 (below)

// 傳到手機 (N 個未傳), 把最後一張截圖／最後一段錄影傳到手機, 傳到手機…,
// 截圖／錄影後自動傳到手機 (both menus).
void appendShareItems(std::vector<MenuItem>& v) {
    const size_t unsent = unsentCount();
    MenuItem send = MenuItem::command(CmdShareLast, tr(S::ShareChip), kIcoShare,
                                      unsent ? fmt(S::ShareUnsentCount, {std::to_wstring(unsent)}) : std::wstring());
    send.enabled = unsent > 0 || !lastShareable().empty();
    v.push_back(std::move(send));
    // 把最後一張截圖／最後一段錄影: only when there is one (0.7.2: two greyed
    // rows in every menu before the first capture).
    if (!lastShotFile().empty()) v.push_back(MenuItem::command(CmdShareLastShot, tr(S::MenuShareLastShot), kIcoShare));
    if (!lastRecFile().empty()) v.push_back(MenuItem::command(CmdShareLastRec, tr(S::MenuShareLastRec), kIcoShare));
    v.push_back(MenuItem::command(CmdSharePick, tr(S::MenuSharePick), kIcoSharePick));
    v.push_back(checkItem(CmdShareAuto, tr(S::MenuShareAuto), kIcoShare, g.settings.autoShare));
}

// 連接 Android（掃 QR） (greyed without the bundled adb / scrcpy-server).
MenuItem androidPairItem() {
    MenuItem m = MenuItem::command(CmdAndroidPair, tr(S::MenuAndroidPair), kIcoQr);
    m.enabled = g.androidOk;
    return m;
}

// 語言 / Language submenu (both menus): 自動 / 繁體中文 / English / 日本語 /
// 한국어 (each language's name in that language).
MenuItem languageItem() {
    std::vector<MenuItem> sub;
    const S names[5] = {S::LangAuto, S::LangZh, S::LangEn, S::LangJa, S::LangKo};
    const UINT ids[5] = {CmdLangAuto, CmdLangZh, CmdLangEn, CmdLangJa, CmdLangKo};
    for (int i = 0; i < 5; ++i) {
        MenuItem m = MenuItem::command(ids[i], tr(names[i]));
        m.radio = true;
        m.checked = g.settings.language == i;
        sub.push_back(std::move(m));
    }
    return MenuItem::submenu(tr(S::MenuLanguage), kIcoLanguage, std::move(sub));
}

MenuItem aboutItem() { return MenuItem::command(CmdAbout, fmt(S::MenuAbout, {tr(S::AppName)}), kIcoInfo); }

// 「更新到 vX.Y.Z」 (+ separator) at the top of both menus when a newer
// version is on offer.
void appendUpdateItem(std::vector<MenuItem>& v) {
    const std::string ver = offerVersion();
    if (ver.empty()) return;
    const bool busy = g.updateDownloading && !offerLocal();
    MenuItem m = MenuItem::command(CmdUpdateDialog, fmt(S::MenuUpdateTo, {toWide(ver)}), kIcoUpdate,
                                   busy ? tr(S::MenuDownloading) : L"");
    m.bold = true;
    m.enabled = !busy;
    v.push_back(std::move(m));
    v.push_back(MenuItem::separator());
}

// Recording while 更新到 is chosen: a small themed confirm (the custom menu)
// over the window; the recording is saved before the app quits.
bool confirmUpdateWhileRecording(const std::wstring& v) {
    std::vector<MenuItem> items;
    items.push_back(MenuItem::caption(tr(S::ConfirmRecTitle)));
    items.push_back(MenuItem::note(fmt(S::ConfirmRecNote, {tr(S::AppName)})));
    MenuItem ok = MenuItem::command(CmdInstallUpdate, fmt(S::ConfirmRecOk, {v}), kIcoUpdate);
    ok.bold = true;
    items.push_back(std::move(ok));
    items.push_back(MenuItem::command(1, tr(S::Cancel), kIcoExit));
    POINT pt{};
    if (IsWindowVisible(g.hwnd) && !IsIconic(g.hwnd)) {
        RECT r{};
        GetWindowRect(g.hwnd, &r);
        pt = {(r.left + r.right) / 2 - 120, (r.top + r.bottom) / 2 - 60};
    } else {
        GetCursorPos(&pt);
    }
    SetForegroundWindow(g.hwnd);
    pm::ui::MenuOptions o;
    o.selectFirst = true;
    const UINT cmd = pm::ui::trackMenu(g.hwnd, items, pt, o);
    PostMessageW(g.hwnd, WM_NULL, 0, 0);
    g.log->write("info", std::string("update while recording: ") + (cmd == CmdInstallUpdate ? "confirmed" : "cancelled"));
    return cmd == CmdInstallUpdate;
}

std::vector<MenuItem> viewItems() {
    std::vector<MenuItem> v;
    v.push_back(MenuItem::command(CmdRotateRight, tr(S::MenuRotateRight), kIcoRotate, L"Ctrl+→"));
    v.push_back(MenuItem::command(CmdRotateLeft, tr(S::MenuRotateLeft), kIcoRotate, L"Ctrl+←"));
    v.push_back(checkItem(CmdMirror, tr(S::MenuFlip), kIcoFlip, g.settings.mirrored, L"Ctrl+H"));
    MenuItem reset = MenuItem::command(CmdResetView, tr(S::MenuResetView), kIcoReset, L"Ctrl+0");
    reset.enabled = g.settings.rotation != 0 || g.settings.mirrored || g.window->viewState().zoom > 1.01f;
    v.push_back(std::move(reset));
    if (g.settings.rotation)
        v.push_back(MenuItem::note(fmt(S::MenuRotatedNote, {std::to_wstring(g.settings.rotation * 90)})));
    v.push_back(MenuItem::separator());
    v.push_back(checkItem(CmdDeviceFrame, tr(S::MenuDeviceFrame), kIcoPhone, g.settings.deviceFrame, L"Ctrl+F"));
    return v;
}

std::vector<MenuItem> themeItems() {
    std::vector<MenuItem> v;
    for (int t = 0; t < 4; ++t) {
        MenuItem m = MenuItem::command(CmdTheme0 + t, tr(kThemeNames[t]));
        m.radio = true;
        m.checked = g.settings.theme == t;
        const std::array<uint32_t, 3> sw = pm::VideoWindow::themeSwatch(kThemes[t]);
        m.hasSwatch = true;
        for (int i = 0; i < 3; ++i) m.swatch[i] = sw[i];
        v.push_back(std::move(m));
    }
    return v;
}

// 放大鏡 ▸ 放大 / 縮小 / 還原 1×, colours (radio), 凍結畫面.
std::vector<MenuItem> magnifierItems() {
    const bool on = viewAvailable();
    const pm::VideoWindow::ViewState vs = g.window->viewState();
    std::vector<MenuItem> v;
    if (!on) v.push_back(MenuItem::note(tr(S::MenuNeedPicture)));
    MenuItem in = MenuItem::command(CmdZoomIn, tr(S::MenuZoomIn), kIcoZoomIn, L"Ctrl+=");
    in.enabled = on && vs.zoom < 7.99f;
    v.push_back(std::move(in));
    MenuItem out = MenuItem::command(CmdZoomOut, tr(S::MenuZoomOut), kIcoZoomOut, L"Ctrl+-");
    out.enabled = on && vs.zoom > 1.01f;
    v.push_back(std::move(out));
    MenuItem reset = MenuItem::command(CmdZoomReset, tr(S::MenuZoomReset), kIcoReset, L"Ctrl+Shift+0");
    reset.enabled = on && vs.zoom > 1.01f;
    v.push_back(std::move(reset));
    if (on && vs.zoom > 1.01f) v.push_back(MenuItem::note(fmt(S::MagZoomFmt, {zoomText(vs.zoom).substr(0, zoomText(vs.zoom).size() - 1)})));
    v.push_back(MenuItem::separator());
    v.push_back(MenuItem::caption(tr(S::MenuFilter)));
    for (int f = 0; f < 5; ++f) {
        // Ctrl+K cycles the colours: named in the caption, not on 原色's row (0.7.2).
        MenuItem m = MenuItem::command(CmdFilter0 + f, tr(kFilterNames[f]));
        m.radio = true;
        m.checked = g.settings.filter == f;
        m.enabled = on;
        v.push_back(std::move(m));
    }
    v.push_back(MenuItem::separator());
    MenuItem fr = checkItem(CmdFreeze, tr(S::MenuFreeze), kIcoFreeze, on && vs.frozen, L"Ctrl+P");
    fr.enabled = on;
    v.push_back(std::move(fr));
    return v;
}

// 管理翻譯模型 ▸ one row per pair (已下載 · size / 未下載 · size; a downloaded
// one is deleted after a confirm), 開啟模型資料夾, 刪除全部.
std::vector<MenuItem> modelItems() {
    std::vector<MenuItem> v;
    v.push_back(MenuItem::note(tr(S::TrModelsNote)));
    bool any = false;
    for (int i = 0; i < kModelPairCount; ++i) {
        const unsigned long long have = dirBytes(modelPairDir(i));
        const bool ready = pm::translate::ModelStore::installed(kModelPairs[i]);
        const std::wstring right = ready ? fmt(S::TrModelReady, {mbText(have)})
                                         : fmt(S::TrModelMissing, {mbText(pm::translate::ModelStore::missingBytes({kModelPairs[i]}))});
        MenuItem m = MenuItem::command(CmdTrDelete0 + i, pairLabel(i), ready ? kIcoDelete : 0, right);
        m.enabled = have > 0;
        any |= have > 0;
        v.push_back(std::move(m));
    }
    v.push_back(MenuItem::separator());
    v.push_back(MenuItem::command(CmdTrModelsFolder, tr(S::TrModelsFolder), kIcoFolder));
    MenuItem all = MenuItem::command(CmdTrDeleteAll, tr(S::TrModelsDeleteAll), kIcoDelete);
    all.enabled = any;
    v.push_back(std::move(all));
    return v;
}

// 翻譯 ▸ 翻譯整個畫面, 框選翻譯, 顯示原文 ✓, 連續翻譯 ✓, [關閉翻譯], 翻成：繁體中文 / English, 管理翻譯模型 ▸.
std::vector<MenuItem> translateItems() {
    const bool on = viewAvailable() && g.translator;
    const bool active = on && g.translator->active();
    std::vector<MenuItem> v;
    if (!viewAvailable()) v.push_back(MenuItem::note(tr(S::MenuNeedPicture)));
    MenuItem screen = MenuItem::command(CmdTranslateScreen, tr(S::MenuTrScreen), kIcoTranslate, active ? L"" : L"Ctrl+L");
    screen.enabled = on && !g.translator->busy();
    v.push_back(std::move(screen));
    MenuItem region = MenuItem::command(CmdTranslateRegion, tr(S::MenuTrRegion), kIcoRegion, L"Ctrl+Shift+L");
    region.enabled = on && !g.translator->busy();
    v.push_back(std::move(region));
    MenuItem orig = checkItem(CmdTranslateOriginal, tr(S::MenuTranslateOriginal), kIcoOriginal,
                              active && g.translator->showOriginal(), L"Ctrl+O");
    orig.enabled = active;
    v.push_back(std::move(orig));
    MenuItem live = checkItem(CmdTranslateLive, fmt(S::MenuTrLive, {std::to_wstring(kLiveSeconds)}).c_str(), kIcoLive,
                              on && g.translator->live());
    live.enabled = on;
    v.push_back(std::move(live));
    if (active) {
        MenuItem close = MenuItem::command(CmdTranslateClose, tr(S::MenuTranslateClose), kIcoExit, L"Ctrl+L");
        close.bold = true;
        v.push_back(std::move(close));
    }
    v.push_back(MenuItem::separator());
    v.push_back(MenuItem::caption(tr(S::MenuTrTarget)));
    // Targets with an offline model from English (other sources pivot via English):
    // 繁體中文, English, 日本語, 한국어 — each named in its own language.
    const pm::translate::Lang target = translateTarget();
    const struct {
        UINT id;
        S name;
        pm::translate::Lang lang;
    } targets[4] = {{CmdTrTargetZh, S::LangZh, pm::translate::Lang::ZhHant},
                    {CmdTrTargetEn, S::LangEn, pm::translate::Lang::En},
                    {CmdTrTargetJa, S::LangJa, pm::translate::Lang::Ja},
                    {CmdTrTargetKo, S::LangKo, pm::translate::Lang::Ko}};
    for (const auto& t : targets) {
        MenuItem m = MenuItem::command(t.id, tr(t.name));
        m.radio = true;
        m.checked = target == t.lang;
        v.push_back(std::move(m));
    }
    // 翻譯顯示方式: 自動 / 原位顯示 / 清單顯示, 深色方框 ✓ (0.7.1).
    v.push_back(MenuItem::separator());
    v.push_back(MenuItem::caption(tr(S::MenuTrLayout)));
    const UINT layouts[3] = {CmdTrLayoutAuto, CmdTrLayoutInPlace, CmdTrLayoutList};
    const S layoutNames[3] = {S::MenuTrLayoutAuto, S::MenuTrLayoutInPlace, S::MenuTrLayoutList};
    for (int i = 0; i < 3; ++i) {
        MenuItem m = MenuItem::command(layouts[i], tr(layoutNames[i]));
        m.radio = true;
        m.checked = g.settings.translateLayout == i;
        v.push_back(std::move(m));
    }
    v.push_back(checkItem(CmdTrDarkCards, tr(S::MenuTrDarkCards), 0, g.settings.translateDark));
    v.push_back(MenuItem::separator());
    v.push_back(MenuItem::submenu(tr(S::MenuTrModels), kIcoModels, modelItems()));
    return v;
}

void runMenu(const std::vector<MenuItem>& items, POINT pt, bool keyboard, bool tray) {
    // Tray: the owner must be the foreground window so the menu sees clicks
    // elsewhere and gets the keyboard (it closes when another app activates).
    SetForegroundWindow(g.hwnd);
    pm::ui::MenuOptions o;
    o.selectFirst = keyboard;
    if (tray) {
        o.iconInstance = GetModuleHandleW(nullptr);
        o.headerIconId = IDI_APP;
    }
    static bool logged = false;
    const UINT cmd = pm::ui::trackMenu(g.hwnd, items, pt, o);
    if (!logged) {
        logged = true;
        char buf[64];
        std::snprintf(buf, sizeof buf, "menu opened in %.1f ms", pm::ui::lastOpenMs());
        g.log->write("info", buf);
    }
    PostMessageW(g.hwnd, WM_NULL, 0, 0);
    if (cmd) runCommand(cmd);
}

std::vector<MenuItem> trayMenuItems() {
    std::vector<MenuItem> v;
    v.push_back(MenuItem::header(tr(S::AppName), g.opts.legacyPorts || g.demoBranding ? L"v" PM_APP_VERSION_STR : tr(S::DevBuild)));
    appendUpdateItem(v);
    MenuItem show = MenuItem::command(CmdShow, tr(S::MenuShowWindow), kIcoShow);
    show.bold = true;  // default (double-click)
    v.push_back(std::move(show));
    if (liveSourceNow() != SrcNone)
        v.push_back(MenuItem::command(CmdDisconnect, fmt(S::MenuDisconnectName, {liveName(true)}), kIcoDisconnect,
                                      L"Ctrl+D"));
    v.push_back(MenuItem::separator());
    v.push_back(androidPairItem());
    v.push_back(MenuItem::command(CmdTutorial, tr(S::MenuTutorial), kIcoHelp));
    v.push_back(MenuItem::separator());
    v.push_back(recordItem());
    appendShareItems(v);
    // 放大鏡 only while a picture is shown; 翻譯 always (翻成 / 管理翻譯模型).
    if (viewAvailable()) v.push_back(MenuItem::submenu(tr(S::MenuMagnifier), kIcoZoom, magnifierItems()));
    v.push_back(MenuItem::submenu(tr(S::MenuTranslateSub), kIcoTranslate, translateItems()));
    v.push_back(MenuItem::separator());
    appendOptionItems(v);
    v.push_back(MenuItem::separator());
    v.push_back(MenuItem::submenu(tr(S::MenuTheme), kIcoTheme, themeItems()));
    v.push_back(languageItem());
    v.push_back(MenuItem::command(CmdOpenShots, tr(S::MenuOpenShots), kIcoFolder));
    v.push_back(MenuItem::command(CmdOpenRecordings, tr(S::MenuOpenRecs), kIcoVideoFolder));
    v.push_back(MenuItem::command(CmdCheckUpdate, tr(S::MenuCheckUpdate), kIcoSync));
    v.push_back(aboutItem());
    v.push_back(MenuItem::separator());
    v.push_back(MenuItem::command(CmdExit, tr(S::MenuExit), kIcoExit));
    return v;
}

void showTrayMenu(int x, int y) { runMenu(trayMenuItems(), POINT{x, y}, false, true); }

std::vector<MenuItem> contextMenuItems() {
    std::vector<MenuItem> settings;
    appendOptionItems(settings);
    settings.push_back(MenuItem::separator());
    settings.push_back(languageItem());
    settings.push_back(MenuItem::command(CmdCheckUpdate, tr(S::MenuCheckUpdate), kIcoSync));
    std::vector<MenuItem> v;
    appendUpdateItem(v);
    if (liveSourceNow() != SrcNone) {  // the phone on screen: (Android: its buttons) + 中斷連線
        v.push_back(MenuItem::caption(liveName()));
        if (g_active.load() == SrcAndroid) {
            v.push_back(MenuItem::command(CmdAndroidBack, tr(S::MenuBack), kIcoBack, tr(S::KeyRightClick)));
            v.push_back(MenuItem::command(CmdAndroidHome, tr(S::MenuHome), kIcoHome, tr(S::KeyMiddleClick)));
            v.push_back(MenuItem::command(CmdAndroidRecents, tr(S::MenuRecents), kIcoRecents));
        }
        v.push_back(MenuItem::command(CmdDisconnect, tr(S::MenuDisconnect), kIcoDisconnect, L"Ctrl+D"));
        v.push_back(MenuItem::separator());
    }
    v.push_back(checkItem(CmdFullscreen, tr(S::MenuFullscreen), kIcoFullscreen, isFullscreen(), L"F11"));
    v.push_back(checkItem(CmdTopmost, tr(S::MenuTopmost), kIcoPin, g.settings.topmost, L"Ctrl+T"));
    v.push_back(MenuItem::separator());
    v.push_back(MenuItem::submenu(tr(S::MenuView), kIcoDisplay, viewItems()));
    v.push_back(MenuItem::submenu(tr(S::MenuMagnifier), kIcoZoom, magnifierItems()));
    v.push_back(MenuItem::submenu(tr(S::MenuTranslateSub), kIcoTranslate, translateItems()));
    v.push_back(MenuItem::submenu(tr(S::MenuTheme), kIcoTheme, themeItems()));
    v.push_back(MenuItem::separator());
    v.push_back(MenuItem::command(CmdSnapshot, tr(S::MenuSnapshot), kIcoCamera, L"Ctrl+S"));
    v.push_back(recordItem());
    appendShareItems(v);
    v.push_back(MenuItem::command(CmdOpenShots, tr(S::MenuOpenShots), kIcoFolder));
    v.push_back(MenuItem::command(CmdOpenRecordings, tr(S::MenuOpenRecs), kIcoVideoFolder));
    v.push_back(MenuItem::separator());
    v.push_back(androidPairItem());
    v.push_back(MenuItem::command(CmdTutorial, tr(S::MenuTutorial), kIcoHelp));
    v.push_back(MenuItem::submenu(tr(S::MenuSettings), kIcoSettings, std::move(settings)));
    v.push_back(aboutItem());
    v.push_back(MenuItem::separator());
    v.push_back(MenuItem::command(CmdExit, tr(S::MenuExit), kIcoExit));
    return v;
}

void showContextMenu(int x, int y) {
    const bool keyboard = x == -1 && y == -1;
    if (keyboard) {  // Shift+F10 / menu key: centre of the window
        RECT r{};
        GetWindowRect(g.hwnd, &r);
        x = (r.left + r.right) / 2;
        y = (r.top + r.bottom) / 2;
    }
    runMenu(contextMenuItems(), POINT{x, y}, keyboard, false);
}

// --dev: both menus as PNGs in %TEMP%\pmshots (no window, no mouse capture),
// lp = row to highlight in the tray menu (-1 none). 901: the 設定 and 語言
// submenus too (menu_settings.png, menu_language.png).
void devMenuShots(int hotRow, bool submenus = false) {
    const fs::path dir = fs::temp_directory_path() / L"pmshots";
    std::error_code ec;
    fs::create_directories(dir, ec);
    pm::ui::MenuOptions tray;
    tray.iconInstance = GetModuleHandleW(nullptr);
    tray.headerIconId = IDI_APP;
    const bool a = pm::ui::renderMenuPng(trayMenuItems(), (dir / L"menu_tray.png").wstring(), tray, hotRow);
    const bool b = pm::ui::renderMenuPng(contextMenuItems(), (dir / L"menu_context.png").wstring());
    g.log->write("info", std::string("dev menu shots: ") + (a ? "tray ok" : "tray failed") + ", " +
                             (b ? "context ok" : "context failed"));
    if (!submenus) return;
    for (const MenuItem& m : contextMenuItems())
        if (m.kind == MenuItem::Kind::Submenu && m.text == tr(S::MenuSettings)) {
            pm::ui::renderMenuPng(m.sub, (dir / L"menu_settings.png").wstring());
            for (const MenuItem& s : m.sub)
                if (s.kind == MenuItem::Kind::Submenu) pm::ui::renderMenuPng(s.sub, (dir / L"menu_language.png").wstring());
        }
}

// --dev 907: the 放大鏡, 翻譯 and 管理翻譯模型 submenus as they would open now
// (menu_magnifier.png, menu_translate.png, menu_models.png).
void devViewMenuShots() {
    const fs::path dir = fs::temp_directory_path() / L"pmshots";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const bool a = pm::ui::renderMenuPng(magnifierItems(), (dir / L"menu_magnifier.png").wstring());
    const bool b = pm::ui::renderMenuPng(translateItems(), (dir / L"menu_translate.png").wstring());
    const bool c = pm::ui::renderMenuPng(modelItems(), (dir / L"menu_models.png").wstring());
    g.log->write("info", std::string("dev view menu shots: ") + (a && b && c ? "ok" : "failed"));
}

void hideToTray() {
    if (isFullscreen()) CallWindowProcW(g.prevProc, g.hwnd, WM_KEYDOWN, VK_F11, 0);
    ShowWindow(g.hwnd, SW_HIDE);
    if (!g.settings.trayHintShown) {
        g.settings.trayHintShown = true;
        saveSettings();
        trayBalloon(fmt(S::TrayHintTitle, {tr(S::AppName)}), tr(S::TrayHintText));
    }
}

// ---- live toolbar (drawn by the video window over the picture) -------------
// Shown on mouse movement while a phone is live; clicks come back as
// WM_PM_TOOL (posted: the window is inside its own click handling). Updated
// after every message that may change it (appProc), only when it changed.
void refreshToolbar() {
    refreshPausedTitle();  // 翻譯 / 凍結 changed the frozen state
    using TI = pm::VideoWindow::ToolbarItem;
    std::vector<TI> v;
    const int src = liveSourceNow();
    if (src != SrcNone) {
        const bool fs = isFullscreen(), rec = recording();
        if (src == SrcAndroid) {
            v.push_back({CmdAndroidBack, kIcoBack, tr(S::TipBack)});
            v.push_back({CmdAndroidHome, kIcoHome, tr(S::TipHome)});
            v.push_back({CmdAndroidRecents, kIcoRecents, tr(S::TipRecents)});
        }
        TI shot{CmdSnapshot, kIcoCamera, tr(S::TipSnapshot)};
        shot.groupStart = src == SrcAndroid;
        v.push_back(shot);
        TI recItem{CmdRecord, kIcoRecord, tr(rec ? S::TipStopRec : S::TipStartRec)};
        recItem.toggled = rec;
        recItem.recording = true;
        v.push_back(recItem);
        // optional: left out of a window too narrow for every button (still in 更多).
        TI share{CmdShareLast, kIcoShare, tr(S::TipShare)};
        share.optional = true;
        v.push_back(share);
        TI rot{CmdRotateRight, kIcoRotate, tr(S::TipRotate)};
        rot.optional = true;
        v.push_back(rot);
        TI full{CmdFullscreen, fs ? kIcoBackToWindow : kIcoFullscreen, tr(fs ? S::TipExitFullscreen : S::TipFullscreen)};
        full.optional = true;
        v.push_back(full);
        if (viewAvailable()) {  // 放大鏡 (1× → 2× → 4×) · 翻譯 · 凍結
            const pm::VideoWindow::ViewState vs = g.window->viewState();
            TI mag{CmdMagCycle, kIcoZoom, fmt(S::TipMagnifier, {zoomText(vs.zoom)})};
            mag.toggled = vs.zoom > 1.01f;
            mag.groupStart = true;
            v.push_back(mag);
            const bool tra = translating();
            TI trn{CmdTranslateToggle, kIcoTranslate, tr(tra ? S::TipTranslateClose : S::TipTranslate)};
            trn.toggled = tra;
            v.push_back(trn);
            TI frz{CmdFreeze, kIcoFreeze, tr(vs.frozen ? S::TipUnfreeze : S::TipFreeze)};
            frz.toggled = vs.frozen;
            frz.optional = true;
            v.push_back(frz);
        }
        if (!offerVersion().empty()) {  // 本機更新 / 自動更新 on offer (never installs by itself)
            TI up{CmdUpdateDialog, kIcoUpdate, fmt(S::MenuUpdateTo, {toWide(offerVersion())})};
            up.groupStart = true;
            v.push_back(up);
        }
        v.push_back({CmdMoreMenu, kIcoMore, tr(S::TipMore)});
        TI dc{CmdDisconnect, kIcoDisconnect, tr(S::TipDisconnect)};
        dc.danger = true;
        dc.groupStart = true;
        v.push_back(dc);
    }
    std::string key;
    for (const TI& t : v)
        key += std::to_string(t.id) + ":" + std::to_string(t.glyph) + (t.toggled ? "t" : "") + toUtf8(t.tooltip) + ";";
    if (key == g.toolbarKey) return;
    g.toolbarKey = key;
    const HWND h = g.hwnd;
    g.window->setLiveToolbar(std::move(v), [h](int id) { PostMessageW(h, WM_PM_TOOL, static_cast<WPARAM>(id), 0); });
}

// ---- events from the AirPlay core -----------------------------------------
// The "connection lost" hold (StateLost): last frame + toast for ~3 s.
void endLostHold() {
    KillTimer(g.hwnd, kLostTimer);
    g.status->releaseHold();
    g.window->setDimmed(false);
}

// After sleep the phone is normally gone, but the core may only notice after
// its feedback timeout. Re-announce on the (possibly new) network and, if no
// new video arrives shortly, drop the frozen picture for the idle screen.
void onResume() {
    const long long now = nowMs();
    if (now - g.lastResumeMs < 5000) return;  // RESUMEAUTOMATIC + RESUMESUSPEND
    g.lastResumeMs = now;
    g.log->write("info", "resumed from sleep: refreshing network");
    if (g.server) {
        g.server->refreshNetwork();
        std::string ifs;
        for (const std::string& i : g.server->advertisedInterfaces()) ifs += (ifs.empty() ? "" : ", ") + i;
        g.log->write("info", "advertised interfaces: " + (ifs.empty() ? std::string("(none yet)") : ifs));
    }
    if (g.status->mirroring() || g.status->holding()) {
        g.resumeFramesIn = g.window->stats().framesIn;
        SetTimer(g.hwnd, kResumeTimer, kResumeCheckMs, nullptr);
    }
}

void checkAfterResume() {
    KillTimer(g.hwnd, kResumeTimer);
    if (g.status->holding()) {
        endLostHold();
    } else if (g.status->mirroring() && g.videoState != StatePaused &&
               g.window->stats().framesIn == g.resumeFramesIn) {
        g.log->write("info", "no video since resume: back to the idle screen");
        g.status->forceIdle();
        g.window->showToast(tr(S::LostIphone));
    }
}

// The phone named in the title / toasts: 「Galaxy S24（Android）」.
void setPeer(int source, const std::wstring& name) {
    g.liveSource = source;
    g.sourceName = name;
    g.peerName = fmt(S::PeerFmt, {name, sourceLabel(source)});
}

// Miracast / Android owns the window (not AirPlay, not free).
bool otherSourceLive() {
    const int a = g_active.load();
    return a == SrcMiracast || a == SrcAndroid;
}

// An iPhone wants to mirror while a Miracast / Android phone is shown:
// takeover=keep refuses it (restarting the AirPlay server drops the
// connection), takeover=new hands it the window now. True = refused.
bool refuseAirPlay(const std::wstring& who) {
    if (!otherSourceLive()) return false;
    if (!g.settings.takeoverKeep) {
        claimSource(SrcAirPlay, true);
        return false;
    }
    static long long lastToast = -100000;
    g.log->write("info", "AirPlay client \"" + toUtf8(who) + "\" refused: " + toUtf8(sourceLabel(g_active.load())) +
                             " is live (takeover=keep)");
    if (nowMs() - lastToast > 5000) {
        lastToast = nowMs();
        g.window->showToast(fmt(S::RefusedIphone, {g.sourceName, who}));
    }
    g.sessionActive = false;
    restartServer();
    return true;
}

void onEvent(EventKind kind, const std::wstring& text) {
    switch (kind) {
    case EvConnecting: {
        const std::wstring who = text.empty() ? L"iPhone" : text;
        if (refuseAirPlay(who)) break;
        endLostHold();
        g.sessionActive = true;
        g.window->showPin(L"");
        g.announcePending = true;
        setPeer(SrcAirPlay, who);
        g.window->setConnecting(g.peerName);
        SetWindowTextW(g.hwnd, fmt(S::TitleConnecting, {tr(S::AppName), g.peerName}).c_str());
        bringToFront();
        break;
    }
    case EvPin:
        if (!text.empty() && refuseAirPlay(L"iPhone")) break;
        g.window->showPin(text);
        if (!text.empty()) {
            g.sessionActive = true;  // the PIN request comes before onClientConnecting
            SetWindowTextW(g.hwnd, fmt(S::TitlePinIphone, {tr(S::AppName)}).c_str());
            bringToFront();
        }
        break;
    case EvTakeover: {
        // Another iPhone took over the receiver (takeover=new). The core has
        // already reset the sinks, which looks like an unexpected loss to
        // StatusVideoSink: end that hold (no dimmed frame, no 連線中斷) at once.
        // A recording of the previous phone is finished (new picture size).
        const bool savedJustNow = nowMs() - g.recordingStoppedAtMs < 1500;
        endLostHold();
        if (g.videoState == StateLost) g.videoState = StateIdle;
        setPeer(SrcAirPlay, text.empty() ? L"iPhone" : text);
        g.lastOrientation = 0;
        const std::wstring msg = fmt(S::TookOver, {g.sourceName});
        g.takeoverAtMs = nowMs();
        if (recording()) stopRecording(fmt(S::RecSavedSuffix, {msg}));
        else g.window->showToast(savedJustNow ? fmt(S::RecSavedSuffix, {msg}) : msg);
        SetWindowTextW(g.hwnd, fmt(S::TitleMirroringName, {tr(S::AppName), g.peerName}).c_str());
        g.announcePending = false;
        bringToFront();
        break;
    }
    case EvDisconnected:
        if (otherSourceLive()) {  // a refused / replaced iPhone: the picture is someone else's
            g.sessionActive = false;
            if (g.restartPending) restartServer();
            break;
        }
        g.status->lostWithoutReset();  // no-op unless still mirroring
        // A hold just started: its StateLost handler saves the recording (with
        // the 連線中斷 toast). Otherwise save it here.
        if (recording() && !g.status->holding())
            stopRecording(fmt(S::MirrorEndedRec, {g.recordingFile.filename().wstring()}));
        g.sessionActive = false;
        g.announcePending = false;
        g.window->showPin(L"");
        SetWindowTextW(g.hwnd, g.idleTitle.c_str());
        g.lastOrientation = 0;
        if (g.restartPending) restartServer();
        break;
    }
}

// ---- sources: arbitration (UI side), Miracast, Android -------------------------
// Back to the idle screen state (title, recording, …) as if the stream had
// ended; the window itself is reset by the caller / source.
void sourceEndedIdle() { SendMessageW(g.hwnd, WM_PM_STATE, StateIdle, 0); }

// A source took the window from another one (claimSource, any thread). The
// old owner is already stopped unless it was AirPlay.
void onSourceTakeover(int now, int prev) {
    if (prev == SrcNone || prev == now) return;
    g.log->write("info", std::string("source ") + toUtf8(sourceLabel(now)) + " took the window from " +
                             toUtf8(sourceLabel(prev)));
    if (recording()) stopRecording(fmt(S::SourceSwitchedRec, {g.recordingFile.filename().wstring()}));
    KillTimer(g.hwnd, kLostTimer);
    g.status->switchSource();
    g.window->setDimmed(false);
    g.lastOrientation = 0;
    if (prev == SrcAndroid) {
        g.window->setPointerHandler(nullptr);
        g.window->setKeyHandler(nullptr);
    }
    if (prev == SrcAirPlay) {  // drop the iPhone (the core has no "disconnect client")
        g.sessionActive = false;
        g.window->showPin(L"");
        restartServer();
    }
}

// Idle-screen hint lines: Miracast 投放 is usable right now?
bool miracastListening() {
    using St = pm::MiracastReceiver::Status;
    if (!g.miracast || !g.settings.miracast || g.miracastUnsupported) return false;
    const int s = g.miracastStatus;
    return s < 0 || s == static_cast<int>(St::Idle) || s == static_cast<int>(St::Connecting) ||
           s == static_cast<int>(St::Connected);
}

// Short muted note after the Android 投放 hint when Miracast cannot receive
// right now (empty: listening). Derived from unsupportedReason() / the
// Disabled detail; an unusable PC wins over the user's own switch.
std::wstring miracastNote() {
    if (miracastListening()) return {};
    if (g.miracastUnsupported) {
        // Which unsupportedReason() (the module's Chinese text, see the
        // table's ModMira* entries) it is.
        const std::wstring& r = g.miracastReason;
        auto is = [&](S id) { return r == tr(id, pm::i18n::Lang::ZhTW); };
        if (is(S::ModMiraReboot)) return tr(S::NoteReboot);
        if (is(S::ModMiraNeedFeature)) return tr(S::NoteWirelessDisplay);
        if (is(S::ModMiraOldWindows)) return tr(S::NoteWindowsUpdate);
        if (is(S::ModMiraPolicy)) return tr(S::NotePolicy);
        return tr(S::NoteUnsupported);
    }
    if (!g.settings.miracast) return tr(S::NoteOff);
    return tr(S::NoteUnavailable);
}

void refreshIdleHints() {
    // Both Android ways are always listed; one that cannot be used right now
    // gets a muted note (tab = muted suffix, see VideoWindow::setIdleHints).
    std::vector<std::wstring> lines;
    lines.push_back(fmt(S::HintIphone, {g.name}));
    const std::wstring mira = miracastNote();
    lines.push_back(fmt(S::HintMiracast, {g.name}) + (mira.empty() ? L"" : L"\t" + mira));
    lines.push_back(std::wstring(tr(S::HintAdb)) + (g.androidOk ? L"" : L"\t" + std::wstring(tr(S::NoteNoTools))));
    g.window->setIdleHints(std::move(lines));
    // Actions: the QR pairing panel (only with the Android tools; the video
    // window draws actions on the idle screen only, never while a phone is
    // live) and the tutorial.
    std::vector<pm::VideoWindow::IdleAction> actions;
    if (const std::string ver = offerVersion(); !ver.empty()) {
        const HWND h = g.hwnd;  // posted: the window is inside its own click handling
        actions.push_back({fmt(S::MenuUpdateTo, {toWide(ver)}), true,
                           [h]() { PostMessageW(h, WM_PM_TOOL, CmdUpdateDialog, 0); }});
    }
    if (g.androidOk) actions.push_back({tr(S::ActShowQr), true, []() { openPairPanel(); }});
    actions.push_back({tr(S::ActHowTo), false, []() { openTutorial(nullptr); }});
    g.window->setIdleActions(std::move(actions));
}

// ---- 使用教學 ----
// The guide in the UI language: 自在投影教學.html / ZizaiCast-Guide.html (file
// names shared with the installer and docs/tutorial). Looked for at the top
// of the install folder (exe in 程式\), next to the exe (the installer puts
// both languages in 程式\; dev build: copied by app/CMakeLists.txt), then in
// the source tree; the other language's guide if this one is missing.
const wchar_t kGuideZh[] = L"自在投影教學.html";
const wchar_t kGuideEn[] = L"ZizaiCast-Guide.html";
const wchar_t kGuideJa[] = L"ZizaiCast-Guide-ja.html";  // 0.7.0: short 日本語 / 한국어 guides
const wchar_t kGuideKo[] = L"ZizaiCast-Guide-ko.html";
const wchar_t* guideName() {
    switch (pm::i18n::lang()) {
    case pm::i18n::Lang::En: return kGuideEn;
    case pm::i18n::Lang::Ja: return kGuideJa;
    case pm::i18n::Lang::Ko: return kGuideKo;
    default: return kGuideZh;
    }
}

fs::path tutorialFile() {
    const fs::path exeDir = exePath().parent_path();
    std::error_code ec;
    // The UI language's guide, else English, else 中文.
    for (const wchar_t* name : {guideName(), kGuideEn, kGuideZh}) {
        std::vector<fs::path> c;
        if (installedLayout(exeDir)) c.push_back(exeDir.parent_path() / name);
        c.push_back(exeDir / name);
#ifdef PM_SOURCE_DIR
        c.push_back(fs::path(toWide(PM_SOURCE_DIR)) / L"docs" / L"tutorial" / name);
#endif
        for (const fs::path& p : c)
            if (fs::exists(p, ec)) return p;
    }
    return {};
}

// Third-party licences: <exe dir>\licenses (installed: 程式\licenses), else
// the source tree's docs\licenses.
fs::path licensesDir() {
    std::error_code ec;
    const fs::path d = exePath().parent_path() / L"licenses";
    if (fs::is_directory(d, ec)) return d;
#ifdef PM_SOURCE_DIR
    const fs::path s = fs::path(toWide(PM_SOURCE_DIR)) / L"docs" / L"licenses";
    if (fs::is_directory(s, ec)) return s;
#endif
    return {};
}

// Opens the tutorial in the default browser; `anchor` (e.g. L"adb") jumps to
// a section: a file path cannot carry "#…", so the browser is started with a
// file:/// URL when the .html association is known.
void openTutorial(const wchar_t* anchor) {
    const fs::path file = tutorialFile();
    if (file.empty()) {
        g.log->write("warn", "tutorial not found");
        g.window->showToast(fmt(S::TutorialMissing, {guideName()}));
        return;
    }
    g.log->write("info", "opening tutorial " + toUtf8(file.wstring()) + (anchor ? "#" + toUtf8(anchor) : ""));
    if (g.testOffscreen) {  // --dev --test-offscreen: no browser on the user's screen
        g.log->write("info", "test-offscreen: tutorial not opened");
        return;
    }
    if (anchor) {
        wchar_t exe[MAX_PATH * 2] = {};
        DWORD n = static_cast<DWORD>(std::size(exe));
        if (SUCCEEDED(AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_EXECUTABLE, L".html", L"open", exe, &n)) && exe[0]) {
            std::wstring url = L"file:///" + file.wstring();
            std::replace(url.begin(), url.end(), L'\\', L'/');
            std::wstring enc;
            for (wchar_t ch : url) enc += ch == L' ' ? std::wstring(L"%20") : std::wstring(1, ch);
            enc += L"#";
            enc += anchor;
            const std::wstring args = L"\"" + enc + L"\"";
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(g.hwnd, L"open", exe, args.c_str(), nullptr, SW_SHOWNORMAL)) > 32)
                return;
        }
    }
    ShellExecuteW(g.hwnd, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---- Miracast ----
// start() / stop() on a helper thread (WinRT status query + settings apply
// take up to a few hundred ms); events arrive on thread-pool threads.
void miracastApply(bool on) {
    if (g.testNoNetwork) return;
    if (!g.miracast) return;
    if (g.miracastOp.joinable()) g.miracastOp.join();
    pm::MiracastReceiver* m = g.miracast;
    const HWND h = g.hwnd;
    if (!on) {
        if (g_active.load() == SrcMiracast) {  // stop() ends the cast without onDisconnected
            releaseSource(SrcMiracast);
            sourceEndedIdle();
        }
        g.miracastOp = std::thread([m]() { m->stop(); });
        return;
    }
    pm::VideoWindow* w = g.window;
    const std::wstring name = g.name;
    pm::MiracastReceiver::Events ev;
    ev.onStatus = [h, m](pm::MiracastReceiver::Status s, const std::wstring& detail) {
        if (s == pm::MiracastReceiver::Status::Connecting) {
            // Arbitrate here, before the cast's first picture reaches the window.
            const int a = g_active.load();
            if (a != SrcNone && a != SrcMiracast && !g_takeoverNew.load()) {
                m->disconnect();
                postSource(h, MiraRefused, detail);
                return;
            }
            claimSource(SrcMiracast, true);
            postSource(h, MiraConnecting, detail);
            return;
        }
        postSource(h, MiraStatus, std::to_wstring(static_cast<int>(s)) + detail);
    };
    ev.onConnected = [h](const std::wstring& name) { postSource(h, MiraConnected, name); };
    ev.onDisconnected = [h]() { postSource(h, MiraDisconnected); };
    ev.onPin = [h](const std::wstring& pin) { postSource(h, MiraPin, pin); };
    g.miracastOp = std::thread([m, w, h, name, ev]() mutable {
        const bool ok = m->start(name, w, std::move(ev));
        postSource(h, MiraStarted, ok ? L"1" : L"0");
    });
}

void setMiracastOption(bool on) {
    if (g.miracastUnsupported) {
        g.window->showToast(g.miracastReason.empty() ? std::wstring(tr(S::MiracastCantReceive))
                                                     : moduleText(g.miracastReason));
        return;
    }
    g.settings.miracast = on;
    saveSettings();
    g.miracastStatus = -1;
    miracastApply(on);
    refreshIdleHints();
    g.window->showToast(tr(on ? S::MiracastOn : S::MiracastOff));
}

void onMiracastEvent(SourceEvent kind, const std::wstring& text) {
    using St = pm::MiracastReceiver::Status;
    switch (kind) {
    case MiraStatus: {
        const int s = text.empty() ? 0 : text[0] - L'0';
        const std::wstring detail = text.size() > 1 ? text.substr(1) : L"";
        g.log->write("info", "miracast status " + std::to_string(s) + (detail.empty() ? "" : ": " + toUtf8(detail)));
        g.miracastStatus = s;
        if (s == static_cast<int>(St::Unavailable)) {
            g.miracastUnsupported = true;
            g.miracastReason = detail;
        } else if (s == static_cast<int>(St::Disabled)) {
            g.miracastReason = detail;
        } else {
            g.miracastReason.clear();
        }
        refreshIdleHints();
        break;
    }
    case MiraStarted:
        g.log->write(text == L"1" ? "info" : "warn", text == L"1" ? "miracast receiver listening" : "miracast receiver did not start");
        break;
    case MiraRefused: {
        const std::wstring who = text.empty() ? std::wstring(tr(S::AndroidPhone)) : moduleText(text);
        g.log->write("info", "miracast cast from \"" + toUtf8(who) + "\" refused (takeover=keep)");
        g.window->showToast(fmt(S::RefusedMiracast, {g.sourceName, who}));
        break;
    }
    case MiraConnecting: {
        if (g_active.load() != SrcMiracast) break;
        endLostHold();
        g.announcePending = true;
        setPeer(SrcMiracast, text.empty() ? std::wstring(tr(S::AndroidPhone)) : moduleText(text));
        g.window->setConnecting(g.peerName);
        SetWindowTextW(g.hwnd, fmt(S::TitleConnecting, {tr(S::AppName), g.peerName}).c_str());
        bringToFront();
        break;
    }
    case MiraConnected:
        if (g_active.load() != SrcMiracast) {  // lost the window meanwhile
            if (g.miracast) g.miracast->disconnect();
            break;
        }
        if (!text.empty()) setPeer(SrcMiracast, moduleText(text));
        g.announcePending = true;
        SendMessageW(g.hwnd, WM_PM_STATE, StateMirroring, 0);
        break;
    case MiraDisconnected:
        if (g_active.load() == SrcMiracast) {
            releaseSource(SrcMiracast);
            g.window->showPin(L"");
            sourceEndedIdle();
        }
        if (g.miracastRenamePending) {  // display name changed during the cast
            g.miracastRenamePending = false;
            if (g.settings.miracast && !g.miracastUnsupported) {
                miracastApply(false);
                miracastApply(true);
            }
        }
        break;
    case MiraPin:
        if (!text.empty() && g_active.load() != SrcMiracast && g_active.load() != SrcNone) break;
        g.window->showPin(text);
        if (!text.empty()) {
            SetWindowTextW(g.hwnd, fmt(S::TitlePinPhone, {tr(S::AppName)}).c_str());
            bringToFront();
        }
        break;
    default: break;
    }
}

// ---- Android (wireless debugging) ----
void setAndroidInput(bool on) {
    if (!on) {
        g.window->setPointerHandler(nullptr);
        g.window->setKeyHandler(nullptr);
        return;
    }
    if (g.testAndroid) {  // --test-source: log what would be sent
        g.window->setPointerHandler([](const pm::VideoWindow::PointerEvent& e) {
            if (e.kind == pm::VideoWindow::PointerEvent::Kind::Down)
                g.log->write("info", "test android: tap " + std::to_string(e.x) + "," + std::to_string(e.y));
        });
        g.window->setKeyHandler([](unsigned vk, bool down, wchar_t) {
            if (down) g.log->write("info", "test android: key " + std::to_string(vk));
        });
        return;
    }
    pm::AndroidSource* a = g.android;
    g.window->setPointerHandler([a](const pm::VideoWindow::PointerEvent& e) { a->sendPointer(e); });
    g.window->setKeyHandler([a](unsigned vk, bool down, wchar_t ch) { a->sendKey(vk, down, ch); });
}

bool androidLive() { return g_active.load() == SrcAndroid; }

// adb connected a phone (after pairing, or a paired phone reconnecting).
void onAndroidConnected(const std::wstring& rawName, bool test) {
    const std::wstring name = rawName.empty() ? std::wstring(tr(S::AndroidPhone)) : rawName;
    const bool userAsked = g.pairPanel.isOpen() || test;
    const int a = g_active.load();
    g.log->write("info", "android connected: \"" + toUtf8(name) + "\"" + (userAsked ? " (pairing)" : " (reconnect)"));
    if (a != SrcNone && a != SrcAndroid && (g.settings.takeoverKeep || !userAsked)) {
        // Busy: keep the current phone. A reconnect in the background never
        // takes over; it is tried again when the window is free.
        if (userAsked) {
            g.pairPanel.close();
            g.window->showToast(fmt(S::AndroidBusy, {g.sourceName, name}));
        }
        return;
    }
    g.pairPanel.close();
    g.androidUserStopped = false;
    claimSource(SrcAndroid, true);
    endLostHold();
    g.announcePending = true;
    setPeer(SrcAndroid, name);
    g.window->showPin(L"");
    g.window->setConnecting(g.peerName);
    SetWindowTextW(g.hwnd, fmt(S::TitleConnecting, {tr(S::AppName), g.peerName}).c_str());
    bringToFront();
    setAndroidInput(true);
    if (!test && !g.android->start(g.androidVideo, g.androidAudio)) {
        g.log->write("error", "android: start() refused");
        setAndroidInput(false);
        releaseSource(SrcAndroid);
        g.status->forceIdle();
        g.window->showToast(tr(S::AndroidStartFail));
    }
}

// 中斷 Android 連線 (old menu command): same as 中斷連線.
void stopAndroid() { disconnectLive(); }

// 中斷連線 (toolbar, Ctrl+D, menus, tray) for whichever phone is live:
//   AirPlay  — the core has no per-client disconnect: the picture goes idle
//              first (so the server's reset is not a "lost" hold), then the
//              AirPlay server restarts, which drops the iPhone (as for refusals).
//   Miracast — disconnect() ends the cast.
//   Android  — stop(); no auto-reconnect until the next pairing.
void disconnectLive() {
    const int src = liveSourceNow();
    if (src == SrcNone) {
        g.window->showToast(tr(S::NoLivePhone));
        return;
    }
    const std::wstring who = liveName();
    g.log->write("info", "user disconnected " + toUtf8(who) + " (" + toUtf8(sourceLabel(src)) + ")");
    ++g_testKick;
    const std::wstring msg = fmt(S::DisconnectedName, {who});
    if (recording()) stopRecording(fmt(S::RecSavedSuffix, {msg}));
    else g.window->showToast(msg);
    KillTimer(g.hwnd, kLostTimer);
    KillTimer(g.hwnd, kAndroidWatchTimer);
    g.window->setDimmed(false);
    switch (src) {
    case SrcAndroid:
        g.androidUserStopped = true;
        releaseSource(SrcAndroid);  // its sink resets are dropped from here on
        setAndroidInput(false);
        if (g.android) g.android->stop();
        g.status->forceIdle();
        break;
    case SrcMiracast:
        releaseSource(SrcMiracast);
        if (g.miracast) g.miracast->disconnect();
        g.window->showPin(L"");
        g.window->onReset();
        sourceEndedIdle();
        break;
    case SrcAirPlay:
        releaseSource(SrcAirPlay);
        g.status->forceIdle();  // mirroring off: the restart's reset is not a "loss"
        g.sessionActive = false;
        g.announcePending = false;
        g.window->showPin(L"");
        restartServer();
        break;
    }
    g.liveSource = SrcNone;
    g.lastOrientation = 0;
    SetWindowTextW(g.hwnd, g.idleTitle.c_str());
}

// The Android phone went away by itself (無線偵錯 turned off: the stream ends;
// Wi-Fi lost: the watchdog notices the silence): straight back to the idle
// screen — no 3 s hold, a reconnect takes longer than that anyway.
void androidGoneUi() {
    KillTimer(g.hwnd, kAndroidWatchTimer);
    KillTimer(g.hwnd, kLostTimer);
    g.window->setDimmed(false);
    SetWindowTextW(g.hwnd, g.idleTitle.c_str());
    g.lastOrientation = 0;
    if (recording())
        stopRecording(fmt(S::AndroidGoneRec, {g.recordingFile.filename().wstring()}));
    else
        g.window->showToast(tr(S::AndroidGone), 4000);
}

// Every second while an Android phone is live. scrcpy repeats the last video
// frame every 100 ms and the audio capture never pauses, so ~6 s without
// either means the phone is gone without the socket noticing (it left the
// Wi-Fi, or went out of range): stop it and go idle. Auto-reconnect stays on.
void androidWatch() {
    if (!androidLive()) {
        KillTimer(g.hwnd, kAndroidWatchTimer);
        g.androidWatchCount = -1;
        return;
    }
    const long long n = g.status->frames.load() + (g.androidAudioPackets ? g.androidAudioPackets->load() : 0);
    const long long now = nowMs();
    if (n != g.androidWatchCount || g.videoState != StateMirroring) {
        g.androidWatchCount = n;
        g.androidWatchAt = now;
        return;
    }
    if (now - g.androidWatchAt < kAndroidStallMs) return;
    g.log->write("warn", "android: no video or audio for " + std::to_string((now - g.androidWatchAt) / 1000) +
                             " s: phone gone (Wi-Fi lost?), stopping");
    releaseSource(SrcAndroid);
    setAndroidInput(false);
    if (g.android) g.android->stop();
    androidGoneUi();
    g.status->forceIdle();
    g.liveSource = SrcNone;
}

void androidNav(UINT cmd) {
    if (!androidLive() || !g.android) return;
    if (cmd == CmdAndroidBack) g.android->pressBack();
    else if (cmd == CmdAndroidHome) g.android->pressHome();
    else g.android->pressAppSwitch();
}

void tryAndroidReconnect() {
    if (!g.androidOk || !g.settings.androidAuto || g.androidUserStopped || g.pairPanel.isOpen()) return;
    if (g_active.load() != SrcNone || g.videoState == StateLost) return;
    using S = pm::AndroidSource::State;
    const S s = g.android->state();
    if (s != S::Idle && s != S::Error) return;
    if (g.android->connectKnownDevices()) g.log->write("info", "android: looking for paired phones");
}

void setAndroidAuto(bool on) {
    g.settings.androidAuto = on;
    saveSettings();
    g.window->showToast(tr(on ? S::AndroidAutoOn : S::AndroidAutoOff));
    if (on) tryAndroidReconnect();
}

// Which pm_android state text (Chinese, see the table's ModAnd* entries) `d` is.
bool androidDetailIs(const std::wstring& d, S id) {
    std::wstring zh = tr(id, pm::i18n::Lang::ZhTW);
    if (const size_t ph = zh.find(L"{0}"); ph != std::wstring::npos) zh.resize(ph);  // prefix
    return d.rfind(zh, 0) == 0;
}

// Pairing failed / timed out: what happened and what to do next.
std::wstring pairFailureText(const std::wstring& d, bool codeMode) {
    if (androidDetailIs(d, S::ModAndTimeout)) return tr(S::PairNoAnswer);
    if (androidDetailIs(d, S::ModAndPairFailed)) return tr(codeMode ? S::PairFailCode : S::PairFailQr);
    if (androidDetailIs(d, S::ModAndAdbFail)) return fmt(S::PairAdbFail, {tr(S::AppName)});
    return d.empty() ? std::wstring(tr(S::PairFailRetry)) : moduleText(d);
}

void startQrPairing() {
    std::vector<uint8_t> bgra;
    int size = 0;
    std::wstring text;
    if (g.android->beginQrPairing(bgra, size, text)) {
        g.pairPanel.setQr(bgra, size);
        g.pairPanel.setStatus(tr(S::PairWaiting));
    } else {
        g.pairPanel.setQr({}, 0);
        g.pairPanel.setStatus(tr(S::PairQrFail), true);
    }
}

void openPairPanel() {
    if (g.testNoNetwork && !g.pairPanel.isOpen()) {
        // --dev --test-no-network: the panel alone (screenshots in every language,
        // DevCommand 902) with a dummy code; adb is never started.
        pm::ui::PairPanel::Callbacks cb;
        cb.onClose = []() {};
        cb.onPairCode = [](const std::wstring&, const std::wstring&) { g.pairPanel.setStatus(tr(S::PairPairing)); };
        cb.onQrMode = []() {};
        cb.onHelp = []() { openTutorial(L"adb"); };
        if (!g.pairPanel.open(g.hwnd, std::move(cb))) return;
        constexpr int n = 33;
        std::vector<uint8_t> px(n * n * 4, 255);
        unsigned r = 12345;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                r = r * 1103515245u + 12345u;
                const bool finder = (x < 7 || x >= n - 7) && y < 7 || x < 7 && y >= n - 7;
                const int fx = x < 7 ? x : x - (n - 7), fy = y < 7 ? y : y - (n - 7);
                const bool dark = finder ? (fx == 0 || fx == 6 || fy == 0 || fy == 6 || (fx >= 2 && fx <= 4 && fy >= 2 && fy <= 4))
                                         : ((r >> 16) & 1) != 0;
                if (dark) std::fill_n(px.begin() + (y * n + x) * 4, 3, uint8_t{0});
            }
        g.pairPanel.setQr(px, n);
        g.pairPanel.setStatus(tr(S::PairWaiting));
        g.log->write("info", "test-no-network: pairing panel with a dummy code (no adb)");
        return;
    }
    if (!g.androidOk) {
        g.window->showToast(fmt(S::AndroidToolsMissing, {tr(S::AppName)}));
        return;
    }
    if (g.pairPanel.isOpen()) {
        g.pairPanel.open(g.hwnd, {});
        return;
    }
    pm::ui::PairPanel::Callbacks cb;
    cb.onClose = []() {
        KillTimer(g.hwnd, kPairTimer);
        using S = pm::AndroidSource::State;
        const S s = g.android->state();
        if (s == S::WaitingForPairing || s == S::Pairing) g.android->cancelPairing();
    };
    cb.onPairCode = [](const std::wstring& hostPort, const std::wstring& code) {
        KillTimer(g.hwnd, kPairTimer);
        if (g.android->pairWithCode(hostPort, code)) g.pairPanel.setStatus(tr(S::PairPairing));
        else g.pairPanel.setStatus(tr(S::PairBadFormat), true);
    };
    cb.onQrMode = []() { startQrPairing(); };
    cb.onHelp = []() { openTutorial(L"adb"); };
    if (!g.pairPanel.open(g.hwnd, std::move(cb))) {
        g.window->showToast(tr(S::PairPanelFail));
        return;
    }
    g.androidUserStopped = false;
    startQrPairing();
}

// ---- 關於 / About ----
void openAbout() {
    pm::ui::AboutPanel::Info info;
    info.version = g.opts.legacyPorts || g.demoBranding ? toWide(kAppVersion) : toWide(kAppVersion) + L" (" + tr(S::DevBuild) + L")";
    info.repoUrl = L"https://github.com/victor900106/ZizaiCast";
    info.icon = g.iconBig;
    info.onOpenUrl = [](const std::wstring& url) {
        g.log->write("info", "about: open " + toUtf8(url));
        if (!g.testOffscreen) ShellExecuteW(g.hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    info.onOpenLicenses = []() {
        const fs::path d = licensesDir();
        g.log->write("info", "about: licenses folder " + (d.empty() ? std::string("(not found)") : toUtf8(d.wstring())));
        if (d.empty()) g.window->showToast(tr(S::AboutNoLicenses));
        else if (!g.testOffscreen) ShellExecuteW(g.hwnd, L"open", d.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    if (!g.aboutPanel.open(g.hwnd, std::move(info))) g.log->write("warn", "about panel could not be created");
}

// ---- 語言 / Language ----
// Display name advertised to phones: --name, else settings display_name, else
// 自在投影 / Zizai Cast (+ " (測試)" / " (Test)" for --dev) in the UI language.
std::wstring displayNameFor() {
    if (!g.nameArg.empty()) return g.nameArg;
    if (!g.settings.displayName.empty()) return g.settings.displayName;
    return tr(g.dev && !g.demoBranding ? S::AppNameDev : S::AppName);
}

void refreshPausedTitle() {
    if (!g.hwnd || g.videoState != StatePaused) return;
    const std::wstring t = currentTitle();
    wchar_t cur[512] = {};
    GetWindowTextW(g.hwnd, cur, 512);
    if (t != cur) SetWindowTextW(g.hwnd, t.c_str());
}

// Window title for the current state, in the current language.
std::wstring currentTitle() {
    const std::wstring app = tr(S::AppName);
    if (g.videoState == StateLost) return fmt(S::TitleLost, {app});
    if (g.videoState == StatePaused)  // a frozen (translated) picture stays up: not "screen off"
        return g.window && g.window->viewState().frozen ? fmt(S::TitlePausedFrozen, {app}) : g.pausedTitle;
    if (g.videoState == StateMirroring && liveSourceNow() != SrcNone)
        return g.peerName.empty() ? fmt(S::TitleMirroring, {app}) : fmt(S::TitleMirroringName, {app, g.peerName});
    if (liveSourceNow() != SrcNone && !g.peerName.empty()) return fmt(S::TitleConnecting, {app, g.peerName});
    return g.idleTitle;
}

void refreshTitles() {
    g.idleTitle = fmt(S::TitleIdle, {tr(S::AppName), g.name});
    g.pausedTitle = fmt(S::TitlePaused, {tr(S::AppName)});
}

// 設定 → 語言 / Language: re-labels everything at once (menus are built on
// open, the toolbar / idle screen / panels are refreshed here); the AirPlay
// and Miracast names are re-advertised only if the display name changed
// (deferred while a phone uses that receiver).
void setLanguage(int pref) {
    if (pref < 0 || pref > 4) return;
    g.settings.language = pref;
    saveSettings();
    const pm::i18n::Lang lang = resolveLanguage(pref);
    const bool changed = lang != pm::i18n::lang();
    pm::i18n::setLang(lang);
    g.log->write("info", std::string("language ") + Settings::languageKey(pref) + " -> " +
                             pm::i18n::langKey(lang) + (changed ? "" : " (unchanged)"));
    if (changed) {
        if (g.liveSource != SrcNone && !g.sourceName.empty()) setPeer(g.liveSource, g.sourceName);
        const std::wstring newName = displayNameFor();
        const bool rename = newName != g.name;
        g.name = newName;
        refreshTitles();
        SetWindowTextW(g.hwnd, currentTitle().c_str());
        trayRetip();
        refreshIdleOptions();
        refreshIdleHints();
        refreshToolbar();
        g.pairPanel.relabel();
        g.aboutPanel.relabel();
        g.updatePanel.relabel();
        g.sharePanel.relabel();
        g.sharePicker.relabel();
        if (g.translator && g.settings.translateTo == 0) g.translator->setTarget(translateTarget());  // 翻成 follows the UI
        if (rename) {
            g.log->write("info", "display name -> \"" + toUtf8(newName) + "\"");
            bool deferred = false;
            if (g.sessionActive) {
                g.restartPending = true;
                deferred = true;
            } else {
                restartServer();
            }
            if (g.miracast && g.settings.miracast && !g.miracastUnsupported) {
                if (g_active.load() == SrcMiracast) {
                    g.miracastRenamePending = true;
                    deferred = true;
                } else {
                    miracastApply(false);
                    miracastApply(true);
                }
            }
            if (deferred) {
                g.window->showToast(fmt(S::NameChangeDeferred, {newName}), 4000);
                return;
            }
        }
    }
    const S labels[5] = {S::LangAuto, S::LangZh, S::LangEn, S::LangJa, S::LangKo};
    const S label = labels[pref];
    g.window->showToast(fmt(S::LangToast, {tr(label)}));
}

void onAndroidEvent(SourceEvent kind, const std::wstring& text) {
    using S = pm::AndroidSource::State;
    switch (kind) {
    case AndState: {
        const S s = static_cast<S>(text.empty() ? 0 : text[0] - L'0');
        const std::wstring detail = text.size() > 1 ? text.substr(1) : L"";
        g.log->write("info", "android state " + std::to_string(static_cast<int>(s)) +
                                 (detail.empty() ? "" : ": " + toUtf8(detail)));
        if (!g.pairPanel.isOpen()) {
            if (s == S::Error && androidLive())
                g.window->showToast(detail.empty() ? std::wstring(tr(I18n::AndroidError)) : moduleText(detail));
            break;
        }
        if (s == S::Mirroring) break;
        switch (s) {
        case S::WaitingForPairing:  // steps are on the panel; a failure text stays until the phone answers
            if (!g.pairPanel.statusIsError()) g.pairPanel.setStatus(tr(I18n::PairWaiting));
            // Listening for the phone from now on: no answer by then -> say what to check.
            if (!g.pairPanel.codeMode()) SetTimer(g.hwnd, kPairTimer, g.pairTimeoutMs, nullptr);
            break;
        case S::Pairing:
            KillTimer(g.hwnd, kPairTimer);
            g.pairPanel.setStatus(detail.empty() ? std::wstring(tr(I18n::PairPairing)) : moduleText(detail));
            break;
        case S::Connecting:
            KillTimer(g.hwnd, kPairTimer);
            g.pairPanel.setStatus(detail.empty() ? std::wstring(tr(I18n::PairConnecting)) : moduleText(detail));
            break;
        case S::Error: {
            KillTimer(g.hwnd, kPairTimer);
            const bool code = g.pairPanel.codeMode();
            const std::wstring msg = pairFailureText(detail, code);
            // QR mode: a fresh code, listening again, so 「再掃一次」 works.
            if (!code && !androidDetailIs(detail, I18n::ModAndAdbFail)) startQrPairing();
            g.pairPanel.setStatus(msg, true);
            break;
        }
        default: break;
        }
        break;
    }
    case AndConnected: onAndroidConnected(text, false); break;
    case AndTestConnected: onAndroidConnected(text, true); break;
    case AndDisconnected:
        g.log->write("info", "android disconnected");
        if (!androidLive()) break;
        setAndroidInput(false);
        // The sinks were reset already: a shown picture gets the 連線中斷 hold
        // (StateLost). Nothing shown yet: back to idle here.
        if (!g.status->mirroring() && !g.status->holding()) {
            releaseSource(SrcAndroid);
            g.status->forceIdle();
            androidGoneUi();
        }
        break;
    default: break;
    }
}

// ---- 傳到手機 / Send to phone (docs/share.md) ------------------------------
// Android phone live over adb → pushed straight into its gallery
// (AndroidSource::pushToGallery). Anything else (iPhone / iPad over AirPlay,
// Android over Miracast, nothing connected) → the QR path: pm::share::Server
// serves just these files on the LAN for 10 minutes; the panel shows the QR.
// 傳到手機 (toolbar, chip, menu) sends this run's captures not sent yet (a
// picker when there are several); 截圖／錄影後自動傳到手機 sends each new one at
// once (Android: queued adb pushes; else a live QR page that stays valid
// while the option is on, up to kLiveShareHours).
enum ShareEvent : WPARAM { ShareAccess = 1, ShareExpired, SharePushDone };
constexpr int kLiveShareHours = 12;
struct PushDone {
    fs::path file;
    pm::AndroidSource::PushResult result;
};

std::wstring shareFilesLabel(const std::vector<fs::path>& files) {
    if (files.empty()) return {};
    if (files.size() == 1) return files[0].filename().wstring();
    return fmt(S::SharePanelMany, {std::to_wstring(files.size())});
}

// An Android phone mirroring over adb (the --test-source android fake counts).
bool androidPushPath() {
    if (!androidLive()) return false;
    if (g.testAndroid) return true;
    return g.android && g.androidOk && g.android->state() == pm::AndroidSource::State::Mirroring;
}

void copyToClipboard(const std::wstring& text) {
    if (g.testOffscreen) {  // never touch the user's clipboard from a test
        g.log->write("info", "share: copy link (test-offscreen: clipboard left alone)");
        return;
    }
    if (!OpenClipboard(g.hwnd)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(mem), text.c_str(), bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
}

void setAutoShare(bool on);
void startShareQr(const std::vector<fs::path>& files, bool live);

bool liveShareOn() { return g.share.live() && !g.share.expired(); }

void markSent(const std::vector<fs::path>& files) {
    for (App::Capture& c : g.captures)
        if (std::find(files.begin(), files.end(), c.file) != files.end()) c.sent = true;
}

std::vector<fs::path> unsentCaptures() {
    std::vector<fs::path> v;
    std::error_code ec;
    for (const App::Capture& c : g.captures)
        if (!c.sent && fs::exists(c.file, ec)) v.push_back(c.file);
    return v;
}
size_t unsentCount() { return unsentCaptures().size(); }

// The QR panel for the running share (opened, or refreshed and brought up).
void openSharePanel() {
    const std::string url = g.share.url();
    if (url.empty()) return;
    std::vector<uint8_t> qr;
    int qrSize = 0;
    pm::share::renderQr(url, 1, 4, qr, qrSize);
    const bool live = g.share.live();
    pm::ui::SharePanel::Callbacks cb;
    cb.onStop = [live]() {
        if (live && g.settings.autoShare) {  // 停止自動傳送: the option goes off with the page
            setAutoShare(false);
            return;
        }
        if (g.share.running()) {
            g.share.stop();
            g.window->showToast(tr(S::ShareStopped));
        }
    };
    cb.onAgain = []() {
        std::vector<fs::path> again;
        for (const std::wstring& f : g.share.files()) again.push_back(f);
        startShareQr(again, g.share.live());
    };
    cb.onCopy = []() {
        copyToClipboard(toWide(g.share.url()));
        g.window->showToast(tr(S::ShareCopied));
    };
    if (live) cb.onHide = []() { g.log->write("info", "share: live panel hidden (still sharing)"); };
    if (!g.sharePanel.open(g.hwnd, std::move(cb))) {
        g.log->write("warn", "share panel could not be created");
        if (!live) g.share.stop();
        return;
    }
    std::vector<fs::path> served;
    for (const std::wstring& f : g.share.files()) served.push_back(f);
    g.sharePanel.setShare(url, qr, qrSize, live && !served.empty() ? served.back().filename().wstring() : shareFilesLabel(served),
                          g.share.secondsLeft());
    g.sharePanel.setLive(live, static_cast<int>(served.size()));
}

// Adds files to the running live page (the phone shows them within a second).
void addToLive(const std::vector<fs::path>& files, bool showPanel) {
    int added = 0;
    for (const fs::path& f : files)
        if (g.share.addFile(f.wstring()) >= 0) ++added;
    markSent(files);
    g.log->write("info", "share: " + std::to_string(added) + " file(s) added to the live page (" +
                             std::to_string(g.share.fileCount()) + " in all)");
    if (showPanel) openSharePanel();
    else if (g.sharePanel.isOpen()) {
        g.sharePanel.setLive(true, g.share.fileCount());
        if (!files.empty()) g.sharePanel.setFiles(files.back().filename().wstring());
    }
    if (added > 0) g.window->showToast(tr(S::ShareLiveAdded), 2500);
}

void startShareQr(const std::vector<fs::path>& files, bool live) {
    pm::share::Server::Options o;
    if (g.server) o.preferred = g.server->advertisedInterfaces();  // where phones already see us
    o.bindIp = g.shareBindTest;
    o.live = live;
    if (live) o.ttlSeconds = kLiveShareHours * 3600;
    if (g.shareTtlTest > 0) o.ttlSeconds = g.shareTtlTest;
    o.english = pm::i18n::en();
    o.lang = static_cast<int>(pm::i18n::lang());
    o.appName = tr(S::AppName);
    const pm::ui::Colors c = pm::ui::currentColors();
    o.palette = {pm::VideoWindow::themeSwatch(kThemes[g.settings.theme & 3])[0], c.cardTop, c.cardBottom, c.fg, c.dim,
                 c.accent, c.light};
    const HWND h = g.hwnd;
    Log* log = g.log;
    g.share.log = [log](const std::string& line) { log->write("share", line); };
    g.share.onAccess = [h](const pm::share::Server::Access& a) {
        auto* p = new pm::share::Server::Access(a);
        if (!PostMessageW(h, WM_PM_SHARE, ShareAccess, reinterpret_cast<LPARAM>(p))) delete p;
    };
    g.share.onExpired = [h]() { PostMessageW(h, WM_PM_SHARE, ShareExpired, 0); };
    std::vector<std::wstring> paths;
    for (const fs::path& f : files) paths.push_back(f.wstring());
    std::string err;
    if (!g.share.start(paths, o, &err)) {
        g.log->write("warn", "share: " + err);
        g.window->showToast(err == "no readable file" && !files.empty()
                                ? fmt(S::ShareFileGone, {files[0].filename().wstring()})
                                : std::wstring(tr(S::ShareStartFail)),
                            4000);
        return;
    }
    markSent(files);
    openSharePanel();
}

// The QR path for `files`: onto the live page when one runs, else a new
// share (live while 自動傳到手機 is on).
void sendViaQr(const std::vector<fs::path>& files, bool showPanel) {
    if (liveShareOn()) {
        addToLive(files, showPanel);
        return;
    }
    startShareQr(files, g.settings.autoShare);
}

void pushNext();

// Android over adb: queued, one push at a time on AndroidSource's thread
// (mirroring goes on). A file already waiting is not queued twice.
void queuePush(const std::vector<fs::path>& files) {
    for (const fs::path& f : files)
        if (std::find(g.pushQueue.begin(), g.pushQueue.end(), f) == g.pushQueue.end()) g.pushQueue.push_back(f);
    markSent(files);
    g.log->write("info", "share: " + std::to_string(files.size()) + " file(s) → Android gallery (adb)" +
                             (g.pushing ? ", queued (" + std::to_string(g.pushQueue.size()) + " waiting)" : ""));
    if (g.pushing) {
        if (!g.settings.autoShare)
            g.window->showToast(fmt(S::ShareQueued, {std::to_wstring(g.pushQueue.size())}), 2500);
        return;
    }
    pushNext();
}

void shareFiles(std::vector<fs::path> files) {
    std::error_code ec;
    files.erase(std::remove_if(files.begin(), files.end(),
                               [&](const fs::path& f) { return f.empty() || !fs::exists(f, ec); }),
                files.end());
    if (files.empty()) {
        g.window->showToast(tr(S::ShareNothing));
        return;
    }
    g.shareChip.hide();
    if (androidPushPath()) {
        queuePush(files);
        return;
    }
    g.log->write("info", "share: " + std::to_string(files.size()) + " file(s) → QR page");
    sendViaQr(files, true);
}

// Android auto path: one file at a time (AndroidSource pushes on its own thread).
void pushNext() {
    while (!g.pushQueue.empty()) {
        const fs::path f = g.pushQueue.front();
        g.pushQueue.erase(g.pushQueue.begin());
        if (g.testAndroid && !g.testPush) {  // --test-source android: the fake phone has no adb
            g.log->write("info", "test android: would push " + toUtf8(f.wstring()) + " → " +
                                     pm::AndroidSource::galleryPath(f.filename().wstring()));
            g.window->showToast(fmt(S::ShareTestPushed, {f.filename().wstring()}), 3000);
            continue;
        }
        const HWND h = g.hwnd;
        pm::AndroidSource* src = g.testPush ? g.testPush.get() : g.android;
        const bool started = src->pushToGallery(f.wstring(), [h, f](const pm::AndroidSource::PushResult& r) {
            auto* p = new PushDone{f, r};
            if (!PostMessageW(h, WM_PM_SHARE, SharePushDone, reinterpret_cast<LPARAM>(p))) delete p;
        });
        if (!started) {  // no adb device after all: QR for this and the rest
            g.log->write("warn", "share: pushToGallery refused; QR instead");
            std::vector<fs::path> rest{f};
            rest.insert(rest.end(), g.pushQueue.begin(), g.pushQueue.end());
            g.pushQueue.clear();
            g.pushing = false;
            g.window->showToast(tr(S::SharePushFail), 4000);
            sendViaQr(rest, true);
            return;
        }
        g.pushing = true;
        g.log->write("info", "share: pushing " + toUtf8(f.filename().wstring()) + " (" +
                                 std::to_string(g.pushQueue.size()) + " more waiting)");
        if (!g.settings.autoShare) g.window->showToast(fmt(S::SharePushing, {f.filename().wstring()}), 60000);
        return;
    }
    g.pushing = false;
}

void onPushDone(const PushDone& d) {
    g.pushing = false;
    const auto& r = d.result;
    if (r.ok) {
        g.log->write("info", "share: pushed " + toUtf8(d.file.filename().wstring()) + " → " + r.remotePath +
                                 (r.inGallery ? " (in the gallery)" : " (not in MediaStore yet)"));
        std::string dir = r.remotePath.substr(0, r.remotePath.rfind('/'));
        if (dir.rfind("/sdcard/", 0) == 0) dir = dir.substr(8);  // Pictures/ZizaiCast
        g.window->showToast(r.inGallery ? (g.settings.autoShare ? std::wstring(tr(S::ShareAutoPushed))
                                                                : fmt(S::SharePushedGallery, {d.file.filename().wstring()}))
                                        : fmt(S::SharePushedFolder, {toWide(dir)}),
                            g.settings.autoShare ? 2500 : 4000);
        pushNext();
        return;
    }
    g.log->write("warn", "share: push failed: " + r.error);
    std::vector<fs::path> rest{d.file};
    rest.insert(rest.end(), g.pushQueue.begin(), g.pushQueue.end());
    g.pushQueue.clear();
    g.window->showToast(tr(S::SharePushFail), 4000);
    sendViaQr(rest, true);
}

void onShareEvent(WPARAM kind, LPARAM lp) {
    if (kind == SharePushDone) {
        std::unique_ptr<PushDone> d(reinterpret_cast<PushDone*>(lp));
        if (d) onPushDone(*d);
        return;
    }
    if (kind == ShareExpired) {
        if (g.share.running()) g.sharePanel.setExpired();
        return;
    }
    if (kind == ShareAccess) {
        std::unique_ptr<pm::share::Server::Access> a(reinterpret_cast<pm::share::Server::Access*>(lp));
        if (!a || !g.sharePanel.isOpen() || a->status >= 400) return;
        if (g.share.live()) g.sharePanel.setLive(true, g.share.fileCount());
        const std::wstring ip = toWide(a->ip);
        if (a->file < 0) {
            g.sharePanel.setStatus(fmt(S::SharePanelOpened, {ip}));
            return;
        }
        const std::vector<std::wstring> files = g.share.files();
        if (a->file >= static_cast<int>(files.size())) return;
        const std::wstring name = fs::path(files[static_cast<size_t>(a->file)]).filename().wstring();
        g.sharePanel.setStatus(fmt(a->download ? S::SharePanelDownloading : S::SharePanelViewing, {name}));
    }
}

// 傳到手機… : any screenshots / recordings (multi-select), starting in 截圖.
std::vector<fs::path> pickShareFiles() {
    std::vector<fs::path> out;
    if (g.testOffscreen) {  // no dialog on the user's screen: the last of each
        g.log->write("info", "share: file picker skipped (test-offscreen): last screenshot + last recording");
        for (const fs::path& f : {lastShotFile(), lastRecFile()})
            if (!f.empty()) out.push_back(f);
        return out;
    }
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(tr(S::SharePickTitle));
        const std::wstring filterName = tr(S::SharePickFilter);
        COMDLG_FILTERSPEC spec[] = {{filterName.c_str(), L"*.png;*.jpg;*.jpeg;*.mp4;*.mov"}};
        dlg->SetFileTypes(1, spec);
        const fs::path shots = screenshotDir(), recs = recordingDir();
        for (const fs::path& d : {recs, shots}) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(d.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
                dlg->AddPlace(item, FDAP_TOP);
                if (d == shots) dlg->SetFolder(item);
                item->Release();
            }
        }
        if (SUCCEEDED(dlg->Show(g.hwnd))) {
            IShellItemArray* arr = nullptr;
            if (SUCCEEDED(dlg->GetResults(&arr))) {
                DWORD n = 0;
                arr->GetCount(&n);
                for (DWORD i = 0; i < n && out.size() < static_cast<size_t>(pm::share::Server::kMaxFiles); ++i) {
                    IShellItem* it = nullptr;
                    PWSTR path = nullptr;
                    if (SUCCEEDED(arr->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        out.push_back(path);
                        CoTaskMemFree(path);
                    }
                    if (it) it->Release();
                }
                arr->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return out;
}

// 傳到手機 with several of this run's captures: thumbnails, the unsent ones ticked.
void openSharePicker() {
    std::vector<pm::ui::SharePicker::Item> items;
    std::error_code ec;
    for (auto it = g.captures.rbegin(); it != g.captures.rend() && items.size() < 48; ++it) {  // newest first
        if (!fs::exists(it->file, ec)) continue;
        pm::ui::SharePicker::Item p;
        p.path = it->file.wstring();
        p.video = it->video;
        p.sent = it->sent;
        p.checked = !it->sent;
        std::tm tm{};
        localtime_s(&tm, &it->at);
        wchar_t hhmmss[16];
        wcsftime(hhmmss, 16, L"%H:%M:%S", &tm);
        p.label = hhmmss;
        items.push_back(std::move(p));
    }
    g.shareChip.hide();
    g.log->write("info", "share: picker with " + std::to_string(items.size()) + " capture(s)");
    g.sharePicker.open(g.hwnd, std::move(items), [](std::vector<std::wstring> paths) {
        std::vector<fs::path> files(paths.begin(), paths.end());
        shareFiles(std::move(files));
    });
}

// 傳到手機 (toolbar, chip, menu): the captures not sent yet; one (or none: the
// newest file, as before) goes at once, several open the picker.
void shareSession() {
    std::vector<fs::path> unsent = unsentCaptures();
    if (unsent.size() >= 2) {
        openSharePicker();
        return;
    }
    shareFiles(unsent.empty() ? std::vector<fs::path>{lastShareable()} : unsent);
}

void setAutoShare(bool on) {
    if (on == g.settings.autoShare) return;
    g.settings.autoShare = on;
    saveSettings();
    g.log->write("info", std::string("share: auto-send ") + (on ? "on" : "off"));
    if (on) {
        g.window->showToast(tr(S::ShareAutoOn), 3000);
        // Not an Android phone over adb: the live page now, so the phone can open it before the first capture.
        if (!androidPushPath() && !liveShareOn()) startShareQr({}, true);
        return;
    }
    if (g.share.live()) {
        g.sharePanel.close();
        g.share.stop();
    }
    g.window->showToast(tr(S::ShareAutoOff), 3000);
}

// A new screenshot / recording (自動傳到手機).
void autoSend(const fs::path& f) {
    g.log->write("info", "share: auto-send " + toUtf8(f.filename().wstring()));
    if (androidPushPath()) queuePush({f});
    else sendViaQr({f}, false);
}

void shareCommand(UINT cmd) {
    switch (cmd) {
    case CmdShareLast: shareSession(); break;
    case CmdShareAuto: setAutoShare(!g.settings.autoShare); break;
    case CmdShareLastShot: shareFiles({lastShotFile()}); break;
    case CmdShareLastRec: shareFiles({lastRecFile()}); break;
    case CmdSharePick: {
        std::vector<fs::path> files = pickShareFiles();
        if (!files.empty()) shareFiles(std::move(files));
        break;
    }
    case CmdShareStop:
        g.sharePanel.close();
        if (g.share.running()) {
            g.share.stop();
            g.window->showToast(tr(S::ShareStopped));
        }
        break;
    }
}

// After a screenshot / recording: remembered for 傳到手機, then sent at once
// (自動傳到手機) or the 「傳到手機」 chip above the toast.
void offerShare(const fs::path& file) {
    if (g.quitting) return;
    g.captures.push_back(App::Capture{file, isShareVideo(file), false, std::time(nullptr)});
    if (g.settings.autoShare) {
        autoSend(file);
        return;
    }
    g.shareChip.show(g.hwnd, []() { shareSession(); });
}

void stopSharing() {
    g.shareChip.hide();
    g.sharePicker.close();
    g.sharePanel.close();
    g.share.stop();
    g.testPush.reset();
}

LRESULT appProcInner(HWND h, UINT msg, WPARAM wp, LPARAM lp);

LRESULT CALLBACK appProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    const LRESULT r = appProcInner(h, msg, wp, lp);
    // Anything that may change the live toolbar (source, recording, fullscreen).
    if (!g.quitting && ((msg >= WM_APP && msg < WM_APP + 32) || msg == WM_SIZE || msg == WM_KEYDOWN ||
                        msg == WM_TIMER || msg == WM_CONTEXTMENU || (msg == g.devCmdMsg && g.devCmdMsg))) {
        syncViewTools();  // picture gone: translation closed, zoom 1×
        refreshToolbar();
        maybeShowDeferredUpdate();  // a prompt held back while mirroring / hidden
    }
    return r;
}

LRESULT appProcInner(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PM_TOOL:
        runCommand(static_cast<UINT>(wp));
        return 0;
    case WM_PM_SHARE:
        onShareEvent(wp, lp);
        return 0;
    case WM_MOVE:
    case WM_SIZE:
        g.shareChip.reposition();  // stays over the window's bottom
        break;
    case WM_PM_LOCALDIR:  // settle a moment: a copy fires many changes
        SetTimer(h, kLocalSettleTimer, 500, nullptr);
        return 0;
    case WM_PM_SOURCE:
        onSourceTakeover(static_cast<int>(wp), static_cast<int>(lp));
        return 0;
    case WM_PM_SRCEV: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
        const SourceEvent kind = static_cast<SourceEvent>(wp);
        if (kind >= AndState) onAndroidEvent(kind, text ? *text : std::wstring());
        else onMiracastEvent(kind, text ? *text : std::wstring());
        return 0;
    }
    case WM_PM_STATE: {
        const WPARAM prev = g.videoState;
        g.videoState = wp;
        if (wp == StateLost && g.liveSource == SrcAndroid) {
            // Android: no hold (see androidGoneUi).
            if (androidLive()) releaseSource(SrcAndroid);
            setAndroidInput(false);
            g.videoState = StateIdle;
            g.status->releaseHold();
            g.liveSource = SrcNone;
            androidGoneUi();
            return 0;
        }
        if (wp == StateLost) {
            // The window is free again (the held frame is only for show).
            if (int a = g_active.load(); a == SrcAirPlay || a == SrcAndroid) releaseSource(a);
            if (g.liveSource == SrcAndroid) g.window->setPointerHandler(nullptr), g.window->setKeyHandler(nullptr);
            SetWindowTextW(h, fmt(S::TitleLost, {tr(S::AppName)}).c_str());
            g.window->setDimmed(true);  // the held last frame, until the hold ends
            const std::wstring lostText = tr(g.liveSource == SrcAndroid ? S::LostAndroid : S::LostIphone);
            if (recording())
                stopRecording(fmt(S::LostRec, {g.recordingFile.filename().wstring()}));
            else
                g.window->showToast(lostText);
            SetTimer(h, kLostTimer, kLostHoldMs, nullptr);
            g.lastOrientation = 0;
            return 0;
        }
        if (wp == StateMirroring) {
            KillTimer(h, kLostTimer);  // a new session took over a held frame
            g.window->setDimmed(false);
            const std::wstring app = tr(S::AppName);
            SetWindowTextW(h, (g.peerName.empty() ? fmt(S::TitleMirroring, {app})
                                                  : fmt(S::TitleMirroringName, {app, g.peerName})).c_str());
            if ((g.announcePending || prev == StateIdle || prev == StateLost) && nowMs() - g.takeoverAtMs > 4000) {
                g.announcePending = false;
                if (g.liveSource == SrcAndroid)  // how to get around (right click is 返回, not the menu)
                    g.window->showToast(fmt(S::ConnectedAndroid, {g.sourceName}), 5000);
                else  // iPhone / Miracast: where the toolbar and the menu are (0.7.2)
                    g.window->showToast(g.peerName.empty() ? std::wstring(tr(S::Connected))
                                                           : fmt(S::ConnectedHint, {g.peerName}), 5000);
            }
            if (g.liveSource == SrcAndroid && androidLive()) {
                g.androidWatchCount = -1;
                SetTimer(h, kAndroidWatchTimer, 1000, nullptr);
            }
        } else if (wp == StatePaused) {
            SetWindowTextW(h, currentTitle().c_str());
        } else {
            if (int a = g_active.load(); a == SrcAirPlay || a == SrcAndroid) releaseSource(a);
            if (g.liveSource == SrcAndroid) g.window->setPointerHandler(nullptr), g.window->setKeyHandler(nullptr);
            if (g_active.load() == SrcNone) g.liveSource = SrcNone;
            SetWindowTextW(h, g.idleTitle.c_str());
            g.lastOrientation = 0;
            if (recording()) stopRecording(fmt(S::MirrorEndedRec, {g.recordingFile.filename().wstring()}));
            if (g.miracastRenamePending && g_active.load() == SrcNone) {  // a cast ended without its event
                g.miracastRenamePending = false;
                if (g.settings.miracast && !g.miracastUnsupported) {
                    miracastApply(false);
                    miracastApply(true);
                }
            }
        }
        return 0;
    }
    case WM_PM_EVENT: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
        onEvent(static_cast<EventKind>(wp), text ? *text : std::wstring());
        return 0;
    }
    case WM_PM_UPDATE: {
        std::unique_ptr<UpdateResult> r(reinterpret_cast<UpdateResult*>(lp));
        if (r && wp == UpdChecked) onUpdateChecked(*r);
        else if (r && wp == UpdDownloaded) onUpdateDownloaded(*r);
        return 0;
    }
    case WM_PM_OPTION:
        if (wp == OptAutostart) setAutostartOption(lp != 0);
        else if (wp == OptPin) setPinOption(lp != 0);
        return 0;
    case WM_PM_TRAY:
        switch (LOWORD(lp)) {
        case NIN_BALLOONUSERCLICK:
            if (g.balloonUpdate && !offerVersion().empty()) {  // 「點這裡看更新內容」
                g.log->write("info", "update balloon clicked");
                openUpdateDialog(true);
                break;
            }
            bringToFront();
            break;
        case WM_LBUTTONDBLCLK:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            bringToFront();
            break;
        case WM_CONTEXTMENU:
            showTrayMenu(static_cast<short>(LOWORD(wp)), static_cast<short>(HIWORD(wp)));
            break;
        }
        return 0;
    case WM_CONTEXTMENU:
        showContextMenu(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)));
        return 0;
    case WM_KEYDOWN:
        if (GetKeyState(VK_CONTROL) < 0 && !(GetKeyState(VK_MENU) < 0)) {
            if (wp == 'S') {
                takeSnapshot();
                return 0;
            }
            if (wp == 'T') {
                setTopmost(!g.settings.topmost);
                return 0;
            }
            const bool shift = GetKeyState(VK_SHIFT) < 0;
            switch (wp) {
            case 'R': toggleRecording(); return 0;
            case VK_RIGHT: rotateView(+1); return 0;
            case VK_LEFT: rotateView(-1); return 0;
            case 'H': toggleMirror(); return 0;
            case '0':
            case VK_NUMPAD0:
                if (shift) zoomReset();  // Ctrl+Shift+0: magnifier back to 1×
                else resetView();
                return 0;
            case 'F': toggleDeviceFrame(); return 0;
            case 'D': disconnectLive(); return 0;
            // 放大鏡 / 翻譯 (handled here so they never reach the phone, and do nothing without a picture)
            case VK_OEM_PLUS:
            case VK_ADD: zoomBy(+1); return 0;
            case VK_OEM_MINUS:
            case VK_SUBTRACT: zoomBy(-1); return 0;
            case 'K': runCommand(CmdFilterCycle); return 0;
            case 'P': toggleFreeze(); return 0;
            case 'L': runCommand(shift ? CmdTranslateRegion : CmdTranslateToggle); return 0;
            case 'O': runCommand(CmdTranslateOriginal); return 0;
            }
        }
        break;
    case WM_TIMER:
        if (wp == kOrientationTimer) {
            followOrientation(h);
            return 0;
        }
        if (wp == kSyncTimer) {
            if (g.settings.avSync) g.window->setSyncMode(true, audioLatencyMs());
            return 0;
        }
        if (wp == kLostTimer) {
            endLostHold();
            if (g.videoState == StateLost) {
                g.videoState = StateIdle;
                SetWindowTextW(h, g.idleTitle.c_str());
            }
            return 0;
        }
        if (wp == kResumeTimer) {
            checkAfterResume();
            return 0;
        }
        if (wp == kRecordTimer) {
            checkRecording();
            return 0;
        }
        if (wp == kFitTimer) {
            KillTimer(h, kFitTimer);
            fitWindowToPicture(h);
            return 0;
        }
        if (wp == kAndroidTimer) {
            tryAndroidReconnect();
            return 0;
        }
        if (wp == kAndroidWatchTimer) {
            androidWatch();
            return 0;
        }
        if (wp == kPairTimer) {
            KillTimer(h, kPairTimer);
            if (g.pairPanel.isOpen() && !g.pairPanel.codeMode() &&
                g.android->state() == pm::AndroidSource::State::WaitingForPairing) {
                g.log->write("info", "pairing: no phone after " + std::to_string(g.pairTimeoutMs / 1000) + " s");
                g.pairPanel.setStatus(tr(S::PairNoAnswer), true);
            }
            return 0;
        }
        if (wp == kLocalSettleTimer) {
            scanLocalUpdates();
            return 0;
        }
        if (wp == kLocalTimer) {
            if (g.localDir.empty() || !g.localWatchAlive) {  // folder (re)created since
                g.localDir = localUpdateDir();
                startLocalWatch();
            }
            scanLocalUpdates();
            return 0;
        }
        if (wp == kUpdateTimer) {
            SetTimer(h, kUpdateTimer, kUpdateIntervalMs, nullptr);  // then every 6 h
            g.updatePromptReady = true;  // offers may prompt from now on
            checkForUpdates(false);
            return 0;
        }
        break;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND) g.log->write("info", "system suspending");
        else if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) onResume();
        return TRUE;
    case WM_CLOSE:
        if (!g.quitting && g.trayOk) {
            hideToTray();
            return 0;
        }
        break;
    case WM_QUERYENDSESSION:
        g.quitting = true;
        break;
    case WM_DESTROY:
        trayRemove();
        break;
    default:
        if (msg == g.taskbarCreatedMsg && g.taskbarCreatedMsg) {  // Explorer restarted
            trayAdd();
            return 0;
        }
        if (msg == g.devCmdMsg && g.devCmdMsg) {  // --dev test scripts: run a menu command
            if (wp == 900 || wp == 901) devMenuShots(static_cast<int>(lp) - 1, wp == 901);  // menu PNGs (lp = hot row + 1)
            else if (wp == 905) {  // 放大鏡 / 翻譯: the open question dialog as ask.png
                const fs::path d = fs::temp_directory_path() / L"pmshots";
                std::error_code ec;
                fs::create_directories(d, ec);
                const bool a = g.askPanel.renderPng((d / L"ask.png").wstring());
                g.log->write("info", std::string("dev ask shot: ") + (a ? "ok (" + toUtf8(g.askPanel.title()) + ")" : "- (no dialog)"));
            } else if (wp == 906) {  // answer the open question dialog (lp 1 = primary button, 0 = Esc)
                g.log->write("info", std::string("dev ask answer: ") + (lp == 1 ? "primary" : "cancel"));
                g.askPanel.close(lp == 1 ? 1 : 0);
            } else if (wp == 907) devViewMenuShots();
            else if (wp == 902) {  // the open pairing panel / About window as PNGs (pair.png, about.png)
                const fs::path d = fs::temp_directory_path() / L"pmshots";
                std::error_code ec;
                fs::create_directories(d, ec);
                const bool p = g.pairPanel.renderPng((d / L"pair.png").wstring());
                const bool a = g.aboutPanel.renderPng((d / L"about.png").wstring());
                g.log->write("info", std::string("dev panel shots: pair ") + (p ? "ok" : "-") + ", about " + (a ? "ok" : "-"));
            } else if (wp == 904) {  // 自動更新: the open 「有新版本」 dialog as update.png
                const fs::path d = fs::temp_directory_path() / L"pmshots";
                std::error_code ec;
                fs::create_directories(d, ec);
                const bool u = g.updatePanel.renderPng((d / L"update.png").wstring());
                g.log->write("info", std::string("dev update shot: ") + (u ? "ok" : "- (dialog not open)"));
            } else if (wp == 903) {  // 傳到手機: panel + chip PNGs, the share URL for test scripts
                const fs::path d = fs::temp_directory_path() / L"pmshots";
                std::error_code ec;
                fs::create_directories(d, ec);
                const bool p = g.sharePanel.renderPng((d / L"share.png").wstring());
                const bool c = g.shareChip.renderPng((d / L"chip.png").wstring());
                const bool k = g.sharePicker.renderPng((d / L"picker.png").wstring());
                std::ofstream(d / L"share_url.txt", std::ios::trunc) << g.share.url();
                g.log->write("info", std::string("dev share shots: panel ") + (p ? "ok" : "-") + ", chip " + (c ? "ok" : "-") +
                                         ", chip visible " + (g.shareChip.visible() ? "yes" : "no") + ", picker " +
                                         (k ? "ok (" + std::to_string(g.sharePicker.checkedCount()) + " ticked, " +
                                                  std::to_string(g.sharePicker.thumbsPending()) + " thumbnails pending)"
                                            : "-"));
            } else if (wp == 908) {  // 傳到手機 picker: 傳送 N 個
                g.log->write("info", "dev picker: send " + std::to_string(g.sharePicker.checkedCount()));
                g.sharePicker.send();
            } else if (wp == 910) {  // 傳到手機 tests: the video window with its toast as window.png
                const fs::path d = fs::temp_directory_path() / L"pmshots";
                std::error_code ec;
                fs::create_directories(d, ec);
                g.log->write("info", std::string("dev window shot: ") +
                                         (g.window->saveWindowShot((d / L"window.png").wstring()) ? "ok" : "-"));
            } else if (wp == 909) {  // 傳到手機 picker: tick / untick item lp (-1: 全選 / 全不選)
                if (lp < 0) g.sharePicker.setAll(g.sharePicker.checkedCount() == 0);
                else g.sharePicker.toggle(static_cast<int>(lp));
            }
            else runCommand(static_cast<UINT>(wp));
            return 0;
        }
        if (msg == g.showMsg && g.showMsg) {  // a second launch asks us to show up
            bringToFront();
            return 0;
        }
        break;
    }
    return CallWindowProcW(g.prevProc, h, msg, wp, lp);
}

HICON loadAppIcon(int cx) {
    HICON icon = nullptr;
    if (FAILED(LoadIconWithScaleDown(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), cx, cx, &icon)))
        icon = LoadIconW(nullptr, IDI_APPLICATION);
    return icon;
}

// ---- --dev test harness (no iPhone needed) -----------------------------------
// --test-feed FILE.h264 [--test-seconds N] [--test-takeover S]: a fake phone
// "連線測試機" connects 1 s after start, its Annex-B H.264 file is looped at
// 30 fps into the same sinks as the core's, and after N s (default 0 = never)
// the video stops and a disconnect is posted (-> the 連線中斷 hold). With
// --test-takeover S, at S s a takeover by 「第二支 iPhone」 is posted.
// Registered message PhoneMirror.DevCommand (wParam = menu command id) runs a
// menu command, for scripted screenshots. Both only with --dev.
// --test-source android: the H.264 file comes from a fake Android phone
// 「Galaxy S24」 through the Android path (arbitration, gates, input handlers
// that log taps / keys) without adb. --test-source miracast: a fake Miracast
// cast 「Galaxy S24」 of generated BGRA pictures (no file needed).
// --test-delay S: connect after S s instead of 1 s.
struct TestFeed {
    std::wstring file;
    int seconds = 0;
    int stallAt = 0;  // --test-stall S: stop sending at S s without a reset (phone left the Wi-Fi)
    int takeoverAt = 0;
    int source = SrcAirPlay;
    int delayMs = 1000;
};

// Fake Miracast cast: arbitration like MiracastReceiver's onStatus(Connecting),
// then ~30 fps BGRA pictures (gradient, moving band, colour blocks).
void runTestMiracast(TestFeed tf, pm::VideoWindow* win, HWND h, Log* log, std::atomic<bool>* stop) {
    Sleep(tf.delayMs);
    const std::wstring name = L"Galaxy S24";
    const int a = g_active.load();
    if (a != SrcNone && a != SrcMiracast && !g_takeoverNew.load()) {
        postSource(h, MiraRefused, name);
        return;
    }
    claimSource(SrcMiracast, true);
    postSource(h, MiraConnecting, name);
    Sleep(400);
    postSource(h, MiraConnected, name);
    log->write("info", "test miracast: casting");
    const int W = 540, H = 1170;
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4);
    const auto t0 = std::chrono::steady_clock::now();
    for (long long n = 0; !*stop; ++n) {
        if (g_active.load() != SrcMiracast) return;  // taken over
        if (tf.seconds > 0 && std::chrono::steady_clock::now() - t0 >= std::chrono::seconds(tf.seconds)) break;
        const int band = static_cast<int>((n * 9) % (H + 120)) - 60;
        for (int y = 0; y < H; ++y) {
            uint8_t* row = px.data() + static_cast<size_t>(y) * W * 4;
            const bool inBand = y >= band && y < band + 60;
            for (int x = 0; x < W; ++x) {
                uint8_t r = static_cast<uint8_t>(40 + 160 * y / H), gg = static_cast<uint8_t>(60 + 120 * x / W), b = 170;
                if (y > 120 && y < 300 && x > 40 && x < W - 40) r = 250, gg = 250, b = 250;  // "status card"
                for (int k = 0; k < 4; ++k)
                    if (y > 360 + k * 180 && y < 500 + k * 180 && x > 40 + (k % 2) * 250 && x < 250 + (k % 2) * 250)
                        r = static_cast<uint8_t>(60 * k), gg = 200, b = static_cast<uint8_t>(255 - 50 * k);
                if (inBand) r = 255, gg = 160, b = 170;
                row[x * 4 + 0] = b;
                row[x * 4 + 1] = gg;
                row[x * 4 + 2] = r;
                row[x * 4 + 3] = 255;
            }
        }
        win->submitBgraFrame(px.data(), W, H, W * 4, 0);
        std::this_thread::sleep_until(t0 + std::chrono::milliseconds(33 * (n + 1)));
    }
    if (*stop || g_active.load() != SrcMiracast) return;
    log->write("info", "test miracast: cast ended");
    win->onReset();
    postSource(h, MiraDisconnected);
}

// Access units of an Annex-B H.264 stream: a new AU starts at an AUD / SPS /
// PPS / SEI after a slice, or at a slice with first_mb_in_slice == 0.
std::vector<std::pair<size_t, size_t>> splitH264(const std::string& d) {
    std::vector<size_t> starts;  // offsets of start codes (00 00 01 / 00 00 00 01)
    for (size_t i = 0; i + 3 < d.size(); ++i)
        if (d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (d[i + 2] == 0 && d[i + 3] == 1))) {
            starts.push_back(i);
            i += d[i + 2] == 1 ? 2 : 3;
        }
    std::vector<std::pair<size_t, size_t>> aus;
    size_t auStart = std::string::npos;
    bool haveSlice = false;
    for (size_t k = 0; k < starts.size(); ++k) {
        const size_t s = starts[k];
        const size_t hdr = s + (d[s + 2] == 1 ? 3 : 4);
        if (hdr >= d.size()) break;
        const int type = d[hdr] & 0x1F;
        const bool slice = type == 1 || type == 5;
        const bool firstMb = slice && hdr + 1 < d.size() && (static_cast<unsigned char>(d[hdr + 1]) & 0x80);
        const bool boundary = haveSlice && ((slice && firstMb) || type == 9 || type == 7 || type == 8 || type == 6);
        if (auStart == std::string::npos) auStart = s;
        if (boundary) {
            aus.push_back({auStart, s - auStart});
            auStart = s;
            haveSlice = false;
        }
        haveSlice |= slice;
    }
    if (auStart != std::string::npos && haveSlice) aus.push_back({auStart, d.size() - auStart});
    return aus;
}

void runTestFeed(TestFeed tf, pm::VideoSink* sink, HWND h, Log* log, std::atomic<bool>* stop) {
    std::ifstream in(fs::path(tf.file), std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto aus = splitH264(data);
    log->write("info", "test feed: " + std::to_string(aus.size()) + " access units from " + toUtf8(tf.file));
    if (aus.empty()) return;
    Sleep(tf.delayMs);
    const bool android = tf.source == SrcAndroid;
    if (android) {
        postSource(h, AndTestConnected, L"Galaxy S24");  // a model name, not translated
        for (int i = 0; i < 60 && g_active.load() != SrcAndroid && !*stop; ++i) Sleep(50);
        if (g_active.load() != SrcAndroid) {
            log->write("info", "test feed: android phone refused");
            return;
        }
    } else {
        postText(h, EvConnecting, tr(S::TestPhone));
    }
    Sleep(300);
    sink->onCodec(pm::VideoCodec::H264);
    const auto t0 = std::chrono::steady_clock::now();
    bool tookOver = false;
    size_t idx = 0;
    const int kick = g_testKick.load();
    for (long long n = 0; !*stop; ++n) {
        if (android && g_active.load() != SrcAndroid) return;  // taken over / stopped
        if (g_testKick.load() != kick) {  // 中斷連線 by the user: the phone stops sending
            log->write("info", "test feed: disconnected by the user");
            return;
        }
        if (tf.stallAt > 0 && std::chrono::steady_clock::now() - t0 >= std::chrono::seconds(tf.stallAt)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));  // silent, no reset: Wi-Fi gone
            continue;
        }
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        if (tf.takeoverAt > 0 && !tookOver && elapsed >= std::chrono::seconds(tf.takeoverAt)) {
            tookOver = true;
            sink->onReset();  // what the core does to the old session's sinks
            postText(h, EvTakeover, tr(S::TestPhone2));
            Sleep(200);
            postText(h, EvConnecting, tr(S::TestPhone2));
            sink->onCodec(pm::VideoCodec::H264);
            idx = 0;  // restart at the IDR
        }
        if (tf.seconds > 0 && elapsed >= std::chrono::seconds(tf.seconds)) break;
        if (idx == aus.size()) {  // loop: the decoder restarts at the IDR
            idx = 0;
            sink->onCodec(pm::VideoCodec::H264);
        }
        const auto& au = aus[idx++];
        sink->onFrame(reinterpret_cast<const uint8_t*>(data.data() + au.first), au.second, 0);
        std::this_thread::sleep_until(t0 + std::chrono::milliseconds(33 * (n + 1)));
    }
    if (*stop) return;
    log->write("info", "test feed: phone gone");
    if (android) {
        sink->onReset();  // AndroidSource resets the sinks before onDisconnected
        postSource(h, AndDisconnected);
        return;
    }
    postText(h, EvDisconnected, {});
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

}  // namespace

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
    HANDLE single = CreateMutexW(nullptr, TRUE, dev ? L"Local\\PhoneMirror.Dev" : L"Local\\PhoneMirror.SingleInstance");
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
    if (!window.create(g.idleTitle.c_str(), 540, 960)) {
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
    createTranslator();       // 翻譯 (pm_translate; bergamot.dll next to the exe)
    window.setViewHandler([](const pm::VideoWindow::ViewState&) { refreshToolbar(); });  // Ctrl+wheel / drag zoom

    // 錄影
    pm::Recorder recorder;
    recorder.log = [&log](const std::string& line) { log.write("recorder", line); };
    g.recorder = &recorder;

    StatusVideoSink videoSink(window, log);
    g.status = &videoSink;
    const fs::path volumeFile = dir / L"volume.txt";
    RememberVolumeAudioSink audioSink(audio, volumeFile);
    // Source arbitration: AirPlay and Android reach the window / player
    // only while they own it.
    GateVideoSink airplayVideo(SrcAirPlay, videoSink), androidVideo(SrcAndroid, videoSink);
    GateAudioSink airplayAudio(SrcAirPlay, audioSink), androidAudio(SrcAndroid, audio);
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
    g.opts.initialVolumeDb = RememberVolumeAudioSink::load(volumeFile, -15.0f);

    applySyncMode();
    if (g.testNoNetwork) {
        // --dev --test-no-network: a fresh build path would make Windows Firewall
        // ask about AirPlay's sockets on the user's screen; no receivers at all.
        log.write("info", "test-no-network: AirPlay, Miracast and adb not started");
    } else if (!startServer(1)) {
        log.write("error", "AirPlay server failed to start");
        trayRemove();
        MessageBoxW(IsWindowVisible(hwnd) ? hwnd : nullptr, tr(S::AirPlayStartFail), tr(S::AppName), MB_ICONERROR);
        g.quitting = true;
        window.close();
        window.runMessageLoop();
        return 1;
    }

    // 自動更新: first check 3 s after start (also with --background), then every
    // 6 h; offers prompt only from then on (announceUpdate). Installers left in
    // %TEMP% by an earlier update are removed (one still running stays locked).
    SetTimer(hwnd, kUpdateTimer, kFirstUpdateCheckMs, nullptr);
    {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(fs::temp_directory_path(ec), ec)) {
            const std::wstring n = e.path().filename().wstring();
            if (n.rfind(kUpdateTempPrefix, 0) == 0 && e.path().extension() == L".exe") {
                std::error_code rm;
                if (fs::remove(e.path(), rm)) log.write("info", "removed old update installer " + toUtf8(n));
            }
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
    KillTimer(hwnd, kAndroidTimer);
    stopLocalWatch();
    android.stop();
    if (g.miracastOp.joinable()) g.miracastOp.join();
    miracast.stop();
    g.recorder = nullptr;
    if (g.server) g.server->stop();
    g.server.reset();
    audio.stop();
    if (single) CloseHandle(single);  // the update installer waits for this mutex to go
    runPendingInstaller();
    pm::VideoWindow::setLogHandler(nullptr);
    return rc;
}
