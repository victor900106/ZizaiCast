// 自在投影 app: menus/menu_devshots.cpp — menus（組選單項目）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "share/share.h"
#include "translate/translate.h"
#include "connect/connect.h"
#include "view/view.h"
#include "menus/menus.h"

namespace pm_app {

// --dev: both menus as PNGs in %TEMP%\pmshots (no window, no mouse capture),
// lp = row to highlight in the tray menu (-1 none). 901: the 設定 and 語言
// submenus too (menu_settings.png, menu_language.png).
void devMenuShots(int hotRow, bool submenus) {
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
    // Motion states on the tray menu's 顯示視窗 row: keyboard focus ring,
    // pressed, and the hover wash halfway in (menu_focus / _pressed / _hover50.png).
    {
        pm::ui::MenuOptions o = tray;
        o.selectFirst = true;
        pm::ui::renderMenuPng(trayMenuItems(), (dir / L"menu_focus.png").wstring(), o, 1);
        o.selectFirst = false;
        o.shotPressed = true;
        pm::ui::renderMenuPng(trayMenuItems(), (dir / L"menu_pressed.png").wstring(), o, 1);
        o.shotPressed = false;
        o.shotHover = 0.5f;
        pm::ui::renderMenuPng(trayMenuItems(), (dir / L"menu_hover50.png").wstring(), o, 1);
    }
    // Every submenu of the tray menu, two levels deep: menu_sub<row>.png and
    // menu_sub<row>_<row>.png; 設定 and its 語言 also as menu_settings.png /
    // menu_language.png.
    const std::vector<MenuItem> items = trayMenuItems();
    for (size_t i = 0; i < items.size(); ++i) {
        const MenuItem& m = items[i];
        if (m.kind != MenuItem::Kind::Submenu) continue;
        const std::wstring base = L"menu_sub" + std::to_wstring(i);
        pm::ui::renderMenuPng(m.sub, (dir / (base + L".png")).wstring());
        for (size_t j = 0; j < m.sub.size(); ++j) {
            if (m.sub[j].kind != MenuItem::Kind::Submenu) continue;
            const std::wstring b2 = base + L"_" + std::to_wstring(j);
            pm::ui::renderMenuPng(m.sub[j].sub, (dir / (b2 + L".png")).wstring());
            for (size_t k = 0; k < m.sub[j].sub.size(); ++k)
                if (m.sub[j].sub[k].kind == MenuItem::Kind::Submenu)
                    pm::ui::renderMenuPng(m.sub[j].sub[k].sub, (dir / (b2 + L"_" + std::to_wstring(k) + L".png")).wstring());
        }
        if (m.text != tr(S::MenuSettings)) continue;
        pm::ui::renderMenuPng(m.sub, (dir / L"menu_settings.png").wstring());
        for (const MenuItem& s : m.sub)
            if (s.kind == MenuItem::Kind::Submenu && s.text == tr(S::MenuLanguage))
                pm::ui::renderMenuPng(s.sub, (dir / L"menu_language.png").wstring());
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

}  // namespace pm_app
