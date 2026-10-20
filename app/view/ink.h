// 自在投影 app: 簡報畫筆 (0.8.3) -- presentation pen over the live picture
// (VideoWindow ink API): toolbar buttons, menu, shortcuts, settings.
#pragma once

#include "core/base/app_state.h"
#include "core/base/base.h"

namespace pm_app {

// ---- view/ink.cpp
void inkInit();                    // once the window exists: handler, style, capture options
bool inkOn();                      // ink mode
void setInkMode(bool on, bool toast = true);
void inkCommand(UINT cmd);         // CmdInk* (app_common.h)
// WM_KEYDOWN in the app's subclass, before the video window: Ctrl+E (mode),
// Ctrl+Shift+E (clear), Ctrl+Z (undo: in ink mode, or strokes on a picture
// that is not an Android phone's), Esc (ink mode off).  True = handled.
bool inkKey(WPARAM wp);
// Live toolbar: 畫筆 (+ in ink mode: 工具 · 顏色 · 粗細 · 復原 · 清除).
void appendInkToolbar(std::vector<pm::VideoWindow::ToolbarItem>& v);
MenuItem inkMenuItem();            // 畫筆 ▸ (context menu)
// After every app message (with syncViewTools): the picture went away -> ink off, strokes cleared.
void inkSyncPicture();

}  // namespace pm_app
