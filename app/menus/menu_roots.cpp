// 自在投影 app: menus/menu_roots.cpp — menus（組選單項目）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "share/share.h"
#include "translate/translate.h"
#include "connect/connect.h"
#include "view/view.h"
#include "menus/menus.h"

namespace pm_app {

// Tray menu, frequent actions first (0.7.4: ~38 rows -> ~15): 顯示視窗,
// 中斷連線, 音量 ▸ | 截圖, 錄影, 傳到手機 ▸, 放大鏡 ▸, 翻譯 ▸ | 連接手機 ▸,
// 設定 ▸, 開啟截圖資料夾, 開啟錄影資料夾, 說明 ▸ | 結束 (0.7.8). The right-click menu has the same groups.
std::vector<MenuItem> trayMenuItems() {
    std::vector<MenuItem> v;
    v.push_back(MenuItem::header(tr(S::AppName), !g.dev || g.demoBranding ? L"v" PM_APP_VERSION_STR : tr(S::DevBuild)));
    appendUpdateItem(v);
    MenuItem show = MenuItem::command(CmdShow, tr(S::MenuShowWindow), kIcoShow);
    show.bold = true;  // default (double-click)
    v.push_back(std::move(show));
    if (liveSourceNow() != SrcNone)
        v.push_back(MenuItem::command(CmdDisconnect, fmt(S::MenuDisconnectName, {liveName(true)}), kIcoDisconnect,
                                      L"Ctrl+D"));
    v.push_back(g_volumeUi.menuItem());  // 音量：60% ▸ (靜音 ✓, 調大聲 / 調小聲, presets)
    v.push_back(MenuItem::separator());
    appendCaptureItems(v);
    v.push_back(shareItem());
    // 0.7.8: 放大鏡 always (greyed without a picture); 翻譯 always (翻成 / 管理翻譯模型).
    v.push_back(magnifierItem());
    v.push_back(MenuItem::submenu(tr(S::MenuTranslateSub), kIcoTranslate, translateItems()));
    v.push_back(MenuItem::separator());
    v.push_back(MenuItem::submenu(tr(S::MenuAndroidSub), kIcoPhone, androidItems()));
    v.push_back(MenuItem::submenu(tr(S::MenuSettings), kIcoSettings, settingsItems()));
    appendFolderItems(v);
    v.push_back(helpItem());
    v.push_back(MenuItem::separator());
    v.push_back(MenuItem::command(CmdExit, tr(S::MenuExit), kIcoExit));
    return v;
}

// Video window right-click menu: the phone's own rows (Android buttons,
// 中斷連線) and the window rows (全螢幕, 最上層, 畫面 ▸) around the same
// groups as the tray menu.
std::vector<MenuItem> contextMenuItems() {
    std::vector<MenuItem> v;
    appendUpdateItem(v);
    if (liveSourceNow() != SrcNone) {  // the phone on screen: (Android: its buttons) + 中斷連線
        v.push_back(MenuItem::caption(liveName()));
        if (g_active.load() == SrcAndroid) {
            v.push_back(MenuItem::command(CmdAndroidBack, tr(S::MenuBack), kIcoBack,
                                          g.settings.rightClickMenu ? L"" : tr(S::KeyRightClick)));  // p7: 右鍵行為
            v.push_back(MenuItem::command(CmdAndroidHome, tr(S::MenuHome), kIcoHome, tr(S::KeyMiddleClick)));
            v.push_back(MenuItem::command(CmdAndroidRecents, tr(S::MenuRecents), kIcoRecents));
        }
        v.push_back(MenuItem::command(CmdDisconnect, tr(S::MenuDisconnect), kIcoDisconnect, L"Ctrl+D"));
        v.push_back(MenuItem::separator());
    }
    v.push_back(checkItem(CmdFullscreen, tr(S::MenuFullscreen), kIcoFullscreen, isFullscreen(), L"F11"));
    v.push_back(checkItem(CmdTopmost, tr(S::MenuTopmost), kIcoPin, g.settings.topmost, L"Ctrl+T"));
    v.push_back(g_volumeUi.menuItem());  // 音量 ▸
    v.push_back(MenuItem::separator());
    appendCaptureItems(v);
    v.push_back(shareItem());
    v.push_back(MenuItem::submenu(tr(S::MenuView), kIcoDisplay, viewItems()));
    v.push_back(magnifierItem());
    v.push_back(MenuItem::submenu(tr(S::MenuTranslateSub), kIcoTranslate, translateItems()));
    v.push_back(MenuItem::separator());
    v.push_back(MenuItem::submenu(tr(S::MenuAndroidSub), kIcoPhone, androidItems()));
    v.push_back(MenuItem::submenu(tr(S::MenuSettings), kIcoSettings, settingsItems()));
    appendFolderItems(v);
    v.push_back(helpItem());
    v.push_back(MenuItem::separator());
    v.push_back(MenuItem::command(CmdExit, tr(S::MenuExit), kIcoExit));
    return v;
}

}  // namespace pm_app
