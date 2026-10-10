// 自在投影 app: view（視窗動作、檢視選項） 對外宣告。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出
#pragma once

#include "core/base/app_state.h"
#include "core/base/base.h"

namespace pm_app {

// ---- view/window_fit.cpp
void applyTopmost();
bool desiredAspect(int& w, int& h);
void setClientSizeCentred(HWND h, int cw, int ch);
bool windowResizable(HWND h);
void followOrientation(HWND h);
void fitWindowToPicture(HWND h);

// ---- view/options.cpp
void refreshIdleOptions();
void setAutostartOption(bool on);
void setPinOption(bool on);
bool audioAdvertItemVisible();
void toggleAudioAdvertAB();
const wchar_t* qualityLabel(int q);
void setQuality(int q);
int audioLatencyMs();
void applySyncMode();
void setAvSync(bool on);
void setTopmost(bool on);
void setCloseButton(int c, bool toast);
void setTakeover(bool keep);

// ---- view/picture.cpp
void applyView(bool refit);
void rotateView(int quarterTurns);
void toggleMirror();
void resetView();
void toggleDeviceFrame();
void applyTheme();
void setTheme(int t);

// ---- view/close_flow.cpp
void quitApp();
void hideToTray();
void onCloseRequest();

// ---- view/language.cpp
void setLanguage(int pref);

}  // namespace pm_app
