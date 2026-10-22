// 0.8.3.5 files: what a new 臨時帳號 gets for 傳檔 (remote_guest_panel and pm_fileperm_test).
// 「僅觀看」and 「可以傳檔」are two separate switches, but a 僅觀看 account can never transfer
// files (the PC refuses it, remote/src/file_perm.h), so the panel shows that and stores `false`.
#pragma once

namespace pm::ui {

constexpr bool kGuestFilesDefault = true;  // 可操作 is made with 可以傳檔 ticked (what the owner expects)

// pref = what the box says when 可操作 is chosen (remembered while the panel is open, so 僅觀看 then
// 可操作 again brings the tick back).
constexpr bool guestFilesEffective(bool viewOnly, bool pref) { return !viewOnly && pref; }

}  // namespace pm::ui
