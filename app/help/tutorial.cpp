// 自在投影 app: help/tutorial.cpp — help（說明、教學、快速鍵一覽）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "help/help.h"

namespace pm_app {

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

}  // namespace pm_app
