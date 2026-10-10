// 自在投影 app: help/shortcuts.cpp — help（說明、教學、快速鍵一覽）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "help/help.h"

namespace pm_app {

// ---- 說明 ▸ 快速鍵一覽 (F1, 0.7.8) ----
// Every keyboard / mouse shortcut in four groups (連線與錄製, 畫面, 放大鏡,
// 翻譯) plus the Android mouse buttons, in a SettingsPanel (key caps on the
// right). The row names are the menus' own strings.
std::vector<pm::ui::SettingsPanel::Item> shortcutItems() {
    using Item = pm::ui::SettingsPanel::Item;
    using K = Item::Kind;
    std::vector<Item> v;
    auto note = [&](const std::wstring& t) {
        Item i;
        i.kind = K::Note;
        i.text = t;
        v.push_back(std::move(i));
    };
    auto head = [&](const std::wstring& t) {
        Item i;
        i.kind = K::Heading;
        i.text = t;
        v.push_back(std::move(i));
    };
    auto key = [&](const std::wstring& t, const std::wstring& k) {
        Item i;
        i.kind = K::KeyRow;
        i.text = t;
        i.sub = k;
        v.push_back(std::move(i));
    };
    // Two related rows in one (調大聲／調小聲 Ctrl+↑ / ↓) keep the list short.
    const bool cjkSlash = pm::i18n::lang() == pm::i18n::Lang::ZhTW || pm::i18n::lang() == pm::i18n::Lang::Ja;
    auto pair = [&](S a, S b) { return std::wstring(tr(a)) + (cjkSlash ? L"／" : L" / ") + tr(b); };
    note(tr(S::KeysNote));
    head(tr(S::KeysGroupConnect));
    key(tr(S::MenuSnapshot), L"Ctrl+S");
    key(tr(S::KeysRecord), L"Ctrl+R");
    key(tr(S::MenuDisconnect), L"Ctrl+D");
    key(tr(S::MenuVolMute), L"Ctrl+M");
    key(pair(S::MenuVolUp, S::MenuVolDown), L"Ctrl+↑ / ↓");
    head(tr(S::MenuView));
    key(tr(S::MenuFullscreen), L"F11");
    key(tr(S::MenuTopmost), L"Ctrl+T");
    key(pair(S::MenuRotateRight, S::MenuRotateLeft), L"Ctrl+→ / ←");
    key(tr(S::MenuFlip), L"Ctrl+H");
    key(tr(S::MenuResetView), L"Ctrl+0");
    key(tr(S::MenuDeviceFrame), L"Ctrl+F");
    head(tr(S::MenuMagnifier));
    key(pair(S::MenuZoomIn, S::MenuZoomOut), L"Ctrl+= / -");
    key(tr(S::KeysZoomWheel), tr(S::KeyCtrlWheel));
    key(tr(S::MenuZoomReset), L"Ctrl+Shift+0");
    key(shortLabel(tr(S::MenuFilter)), L"Ctrl+K");
    key(tr(S::MenuFreeze), L"Ctrl+P");
    head(tr(S::MenuTranslateSub));
    key(tr(S::MenuTrScreen), L"Ctrl+L");
    key(tr(S::MenuTrRegion), L"Ctrl+Shift+L");
    key(tr(S::MenuTranslateOriginal), L"Ctrl+O");
    head(tr(S::KeysGroupAndroid));
    if (g.settings.rightClickMenu) {  // 0.7.8 UX (p7) 右鍵行為 = 開啟選單: Back is on the toolbar only
        key(tr(S::KeysMenu), tr(S::KeyRightClick));
        key(tr(S::MenuHome), tr(S::KeyMiddleClick));
    } else {
        key(tr(S::MenuBack), tr(S::KeyRightClick));
        key(tr(S::MenuHome), tr(S::KeyMiddleClick));
        key(tr(S::KeysMenu), std::wstring(L"Shift+") + tr(S::KeyRightClick));
    }
    Item close;
    close.kind = K::Buttons;
    close.footer = true;
    close.buttons.push_back({1, tr(S::AboutClose), true});
    v.push_back(std::move(close));
    return v;
}

void openShortcuts() {
    pm::ui::SettingsPanel::testOffscreen = g.testOffscreen;
    if (g.keysPanel.isOpen()) {  // language changed / opened again: new texts, to the front
        g.keysPanel.setTitle(tr(S::MenuShortcuts));
        g.keysPanel.setItems(shortcutItems());
        if (!g.testOffscreen) SetForegroundWindow(g.keysPanel.hwnd());
        return;
    }
    pm::ui::SettingsPanel::Callbacks cb;
    cb.onAction = [](int id) {
        if (id == 1) g.keysPanel.close();
    };
    g.log->write("info", "shortcuts panel opened");
    g.keysPanel.open(g.hwnd, tr(S::MenuShortcuts), kIcoKeyboard, shortcutItems(), std::move(cb));
}

// The latest release's page (the installer; a reinstall brings back the
// Android tools).  Not opened in --test-offscreen runs.
void openDownloadPage() {
    const wchar_t* url = L"https://github.com/victor900106/ZizaiCast/releases/latest";
    g.log->write("info", "opening download page " + toUtf8(url));
    if (g.testOffscreen) {
        g.log->write("info", "test-offscreen: download page not opened");
        return;
    }
    ShellExecuteW(g.hwnd, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace pm_app
