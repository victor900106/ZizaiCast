// 自在投影 app: 拖檔到視窗 (0.8.3) -- files dragged onto the window.
//   * An Android phone mirroring over wireless debugging: .apk ->
//     AndroidSource::installApk, anything else -> pushFile (one at a time;
//     a toast when each is done or failed).
//   * iPhone / iPad (AirPlay), Miracast, or no phone: the files join the
//     running 傳到手機 page (or start one) and its QR panel opens.
//   * While files are over the window it says what will happen
//     (VideoWindow::setDropHint: 「放開以傳到手機」 / 「放開以安裝 App」).
#pragma once

#include "core/base/app_state.h"
#include "core/base/base.h"

namespace pm_app {

// ---- share/drop.cpp
void dropInit();      // RegisterDragDrop on the window (UI thread, after it exists)
void dropShutdown();  // RevokeDragDrop
void onDropDone(WPARAM wp, LPARAM lp);  // WM_PM_DROP
// What a drop of these files does (the same path the drop target takes).
void dropFiles(std::vector<fs::path> files);
// The hint for these files (empty: hidden), as while dragging them.
void dropShowHint(const std::vector<fs::path>& files);

}  // namespace pm_app
