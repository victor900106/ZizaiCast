// 自在投影 app: connect（iPhone / Android / Miracast 連線狀態） 對外宣告。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出
#pragma once

#include "core/base/app_state.h"
#include "core/base/base.h"

namespace pm_app {

// ---- connect/airplay.cpp
bool startServer(int attempts);
void restartServer();

// ---- connect/airplay_events.cpp
void endLostHold();
void onResume();
void checkAfterResume();
void setPeer(int source, const std::wstring& name);
bool otherSourceLive();
bool refuseAirPlay(const std::wstring& who);
void pinSessionEnded();
void onEvent(EventKind kind, const std::wstring& text);

// ---- connect/sources.cpp
void sourceEndedIdle();
void onSourceTakeover(int now, int prev);
bool miracastListening();
MiraWhy miracastWhy();

// ---- connect/idle_hints.cpp
void refreshIdleHints();

// ---- connect/miracast.cpp
void miracastApply(bool on);
void setMiracastOption(bool on);
void onMiracastEvent(SourceEvent kind, const std::wstring& text);
void miracastPendingGone();

// ---- connect/android.cpp
bool androidRightClick(const pm::VideoWindow::PointerEvent& e);
void setRightClickMenu(bool menu);
void setAndroidInput(bool on);
void onAndroidConnected(const std::wstring& rawName, bool test);
void stopAndroid();
void androidGoneUi();
void androidWatch();
void androidNav(UINT cmd);
void tryAndroidReconnect();
void setAndroidAuto(bool on);
bool androidDetailIs(const std::wstring& d, S id);
std::wstring pairFailureText(const std::wstring& d, bool codeMode);
void startQrPairing();
void openPairPanel();
void onAndroidEvent(SourceEvent kind, const std::wstring& text);

// ---- connect/session.cpp
void disconnectLive();
bool applyPendingNow();
std::wstring unbrokenName(const std::wstring& s);
void applyNow();
long long airplayAudioPackets();
bool connectingNow();
void armConnectWatch();
void connectWatchTick();

}  // namespace pm_app
