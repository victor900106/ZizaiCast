// 自在投影 app: core/app/dev_083.cpp — --dev DevCommand hooks of the 0.8.3
// feat-ux features (scripted off-screen screenshots; nothing here runs
// without --dev).
//   975       the 畫筆 ▸ and 連接手機 ▸ submenus as menu_ink.png / menu_android.png
//   976 lp=1  the drop hint for the files in PM_DROP_TEST ("a|b|c"), lp=0 hides it
//   977       drops the PM_DROP_TEST files (the drop target's own path)
//   978       logs the ink state (mode, strokes)
//   990 lp=0  0.8.3.4 F3: a made-up "no paired phone found" report (lp=1: "reconnect after a drop failed") as AndState
//   991       0.8.3.4 B6: presses 重新產生 QR in the open pairing window (logs whether it was offered)

#include "core/app/core_app.h"
#include "core/base/app_state.h"
#include "core/base/base.h"
#include "connect/android_control.h"
#include "connect/connect.h"
#include "menus/menus.h"
#include "share/drop.h"
#include "view/ink.h"

namespace pm_app {

namespace {
std::vector<fs::path> dropTestFiles() {
    std::vector<fs::path> v;
    wchar_t buf[4096] = {};
    const DWORD n = GetEnvironmentVariableW(L"PM_DROP_TEST", buf, 4096);
    if (!n || n >= 4096) return v;
    std::wstring s(buf, n), cur;
    for (wchar_t c : s + L"|") {
        if (c == L'|') {
            if (!cur.empty()) v.emplace_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    return v;
}
}  // namespace

bool devCommand083(WPARAM wp, LPARAM lp) {
    if (wp == 975) {
        const fs::path d = fs::temp_directory_path() / L"pmshots";
        std::error_code ec;
        fs::create_directories(d, ec);
        const bool a = pm::ui::renderMenuPng(inkMenuItem().sub, (d / L"menu_ink.png").wstring());
        const bool b = pm::ui::renderMenuPng(androidItems(), (d / L"menu_android.png").wstring());
        g.log->write("info", std::string("dev 0.8.3 menu shots: ink ") + (a ? "ok" : "-") + ", android " + (b ? "ok" : "-"));
        return true;
    }
    if (wp == 976) {
        dropShowHint(lp ? dropTestFiles() : std::vector<fs::path>{});
        g.log->write("info", "dev drop hint " + std::string(lp ? "on" : "off"));
        return true;
    }
    if (wp == 977) {
        dropFiles(dropTestFiles());
        return true;
    }
    if (wp == 978) {
        g.log->write("info", std::string("dev ink: mode ") + (inkOn() ? "on" : "off") + ", " +
                                 std::to_string(g.window->inkCount()) + " stroke(s)");
        return true;
    }
    if (wp == 990) {
        const std::wstring detail = lp ? tr(S::ModAndReconnectFail, pm::i18n::Lang::ZhTW) : tr(S::ModAndNoneFound, pm::i18n::Lang::ZhTW);
        onAndroidEvent(AndState, std::wstring(lp ? L"5" : L"0") + detail);  // State::Error = 5, Idle = 0
        return true;
    }
    if (wp == 991) {
        g.log->write("info", std::string("dev pair: retry ") + (g.pairPanel.devPressRetry() ? "pressed" : "not offered"));
        return true;
    }
    return false;
}

}  // namespace pm_app
