// 自在投影 app: menus（組選單項目） 對外宣告。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出
#pragma once

#include "core/base/app_state.h"
#include "core/base/base.h"

namespace pm_app {

// ---- menus/menu_items.cpp
MenuItem checkItem(UINT id, const wchar_t* text, wchar_t icon, bool on, std::wstring right = {});
void appendNotes(std::vector<MenuItem>& v, const std::wstring& text, size_t width = 22);
// Long explanations (Miracast reasons) as several note rows (menus do not wrap).
// English / 한국어 (spaces between words): word-wrapped at ~52 / ~34 characters.
void appendNotesRaw(std::vector<MenuItem>& v, const std::wstring& text, size_t width);
MenuItem valueSubmenu(const wchar_t* text, wchar_t icon, std::vector<MenuItem> items, std::wstring value);
std::vector<MenuItem> closeItems();
std::vector<MenuItem> qualityItems();
std::vector<MenuItem> takeoverItems();
MenuItem rightClickItem();
std::vector<MenuItem> settingsItems();
MenuItem miracastUnavailableItem();
std::vector<MenuItem> androidItems();
MenuItem recordItem();
MenuItem shareItem();
MenuItem androidPairItem();
MenuItem languageItem();
MenuItem aboutItem();
void appendFolderItems(std::vector<MenuItem>& v);
MenuItem helpItem();
void appendCaptureItems(std::vector<MenuItem>& v);
MenuItem magnifierItem();
void appendUpdateItem(std::vector<MenuItem>& v);

// ---- menus/menu_view_translate.cpp
std::vector<MenuItem> viewItems();
std::vector<MenuItem> themeItems();
MenuItem themeItem();
std::vector<MenuItem> magnifierItems();
std::vector<MenuItem> modelItems();
std::vector<MenuItem> translateItems();
std::vector<MenuItem> translateTargetItems();
std::vector<MenuItem> translateLayoutItems();
std::vector<MenuItem> translateAdvancedItems();

// ---- menus/menu_roots.cpp
std::vector<MenuItem> trayMenuItems();
std::vector<MenuItem> contextMenuItems();

// ---- menus/menu_devshots.cpp
void devMenuShots(int hotRow, bool submenus = false);
void devViewMenuShots();

}  // namespace pm_app
