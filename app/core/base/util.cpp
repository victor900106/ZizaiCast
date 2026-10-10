// 自在投影 app: core/base/util.cpp — core/base（第 0 層：共用型別、狀態與原語）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"

namespace pm_app {

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
    std::wstring leaf = dev ? L"PhoneMirror-dev" : L"PhoneMirror";
    // --dev test runs side by side (several worktrees): PM_DEV_INSTANCE=x
    // gives this one its own folder (and its own single-instance mutex).
    wchar_t inst[32] = {};
    if (dev && GetEnvironmentVariableW(L"PM_DEV_INSTANCE", inst, 32) > 0 && inst[0]) leaf += std::wstring(L"-") + inst;
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

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace pm_app
