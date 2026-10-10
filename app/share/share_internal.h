// 自在投影 app: share/ 內部共用（只給本資料夾的 .cpp）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出
#pragma once

#include "share/share.h"

namespace pm_app {

// ---- 傳到手機 / Send to phone (docs/share.md) ------------------------------
// Android phone live over adb → pushed straight into its gallery
// (AndroidSource::pushToGallery). Anything else (iPhone / iPad over AirPlay,
// Android over Miracast, nothing connected) → the QR path: pm::share::Server
// serves just these files on the LAN for 10 minutes; the panel shows the QR.
// 傳到手機 (toolbar, chip, menu) sends this run's captures not sent yet (a
// picker when there are several); 截圖／錄影後自動傳到手機 sends each new one at
// once (Android: queued adb pushes; else a live QR page that stays valid
// while the option is on, up to kLiveShareHours).
enum ShareEvent : WPARAM { ShareAccess = 1, ShareExpired, SharePushDone };
constexpr int kLiveShareHours = 12;
struct ShareAccessMsg {
    pm::share::Server::Access a;
    unsigned gen = 0;
};
struct PushDone {
    fs::path file;
    pm::AndroidSource::PushResult result;
};

extern unsigned g_shareGen;  // UI thread only (see share_qr.cpp)

}  // namespace pm_app
