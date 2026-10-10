// 自在投影 app: help/about.cpp — help（說明、教學、快速鍵一覽）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "help/help.h"

namespace pm_app {

// ---- 關於 / About ----
void openAbout() {
    pm::ui::AboutPanel::Info info;
    info.version = !g.dev || g.demoBranding ? toWide(kAppVersion) : toWide(kAppVersion) + L" (" + tr(S::DevBuild) + L")";
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

}  // namespace pm_app
