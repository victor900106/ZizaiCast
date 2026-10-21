// 自在投影 app: remote（遠端操控，0.8.0） 對外宣告。See docs/remote.md.
// A phone browser sees and controls this PC (pm::remote::Host). Off at every
// start; turned on by hand (menu / tray). A new phone is let in only after
// 允許 on this PC (remembered phones go straight in); while one is connected a
// topmost bar shows 「正在被遠端操控」 with 中斷, and Ctrl+D ends it from anywhere.
// Built without libdatachannel / opus (pm_remote missing): remoteAvailable()
// is false and nothing of it shows.
#pragma once

#include "core/base/app_state.h"
#include "core/base/base.h"

namespace pm_app {

enum RemoteEvent : WPARAM {
    RemRequest = 1, RemActive, RemEnded, RemBlocked, RemTrouble, RemTunnelUrl, RemTunnelEnd, RemTunnelLink, RemTunnelCheck,
    RemFixedUp, RemFixedFail, RemFixedCfUrl, RemFixedCfEnd, RemFixedLogin, RemFixedStale,  // 常駐外網 (0.8.2)
    RemLoginRevoked, RemUnlocked, RemUploaded,  // 0.8.3
    RemFixedWait,  // 0.8.3.1: 常駐外網 waits for Tailscale to finish starting (after a reboot)
    RemGuestLogin  // 0.8.3.2: a 臨時帳號 login try
};

bool remoteAvailable();
bool remoteOn();
void setRemote(bool on);
void showRemotePanel();
void remoteCommand(UINT cmd);
MenuItem remoteItem();
void onRemoteEvent(WPARAM kind, LPARAM lp);
bool onRemoteHotkey(WPARAM id);  // WM_HOTKEY: true if it was the remote Ctrl+D
void stopRemote();               // quit / session end: everything off, nothing saved twice
void remoteRetheme();
void remoteRelabel();
void remoteDevShots();           // --dev DevCommand 960: remote_panel(_ts).png, remote_banner.png, remote_ask.png
void remoteDevAnswer(int how);   // --dev DevCommand 961: the open 允許 / 拒絕 (1 allow, 2 deny, 3 allow + remember)
// --dev DevCommand 962 (0.8.1): clicks as if from the phone (its input tag) or plain:
// 1 panel 關閉遠端操控 from the phone, 2 the same plain, 3 banner 中斷 from the phone,
// 4 banner 僅觀看 from the phone, 5 menu 開啟遠端操控 (off) from the phone.
void remoteDevClick(int how);
// 0.8.2 常駐外網 (fixed link + password, Tailscale Funnel :8443). At start (main.cpp,
// before the first-start tutorial): 常駐外網 on = remote control + the fixed link
// (quietly), else an 8443 left pointing at this app's origin is removed; the
// 新功能 note once after an upgrade (not from autostart).
void remoteStartup(bool background);
std::wstring remoteStatusLine();  // "" off, else 已開啟 · path · phones
// 0.8.2: the main window's 遠端控制 page (after the tabs in actions): the big switch,
// the three ways in (在家 / 固定網址＋密碼 / Tailscale), 進階設定 and 怎麼用？.
void remoteIdlePage(std::vector<pm::VideoWindow::IdleCard>& cards, std::vector<pm::VideoWindow::IdleAction>& actions);
MenuItem remoteTrayToggle();      // the tray menu's first row: 遠端操控 ✓
// --dev DevCommand 963: 1 the password window (lp 1: open it, 2: fill "Zizai-Test-2026"
// twice + save, 3: mismatching + save), 4 常駐外網 off, 5 shots (remote_fixed.png,
// remote_password.png, whats_new.png), 6 the 新功能 note now, 7 fill a too-short one + save,
// 8 進階設定 as remote_advanced.png, 9 close the 新功能 note (知道了).
void remoteDevFixed(int how);
void remoteDevLog(int how);       // --dev DevCommand 965 (0.8.3): 1 = the 連線紀錄 text into the log, 2 = 中斷 from the PC
// --dev DevCommand 981..986 (0.8.3.2 臨時帳號; the menu ids 440-442, 450+i 刪除, 460+i 重設密碼 run as usual):
// 981 the open 新增臨時帳號 form: lp = life (0-3) | 4 僅觀看 | 8 傳檔 | 16 建立 (the name: PM_TEST_GUEST_NAME, else
// the suggested one); 982 its PNG (lp = DPI, 0 = the window's) as guest_panel[_<dpi>].png; 983 複製全部 (with
// PM_TEST_GUEST_OUT: into that file, never the real clipboard); 984 close it; 985 the main window as if moved to
// a monitor at DPI lp & 0xFFFF with its client lp >> 16 px high (0: the same DIPs; WM_DPICHANGED);
// 986 the accounts into the log (names, kinds; never a password).
void remoteDevGuest(int which, LPARAM lp);

}  // namespace pm_app
