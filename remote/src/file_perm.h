// 0.8.3.5 files: who may send / receive files, in one place (host.cpp and pm_fileperm_test use it).
//
//   account                          僅觀看 (PC switch)   可操作 + 傳檔      可操作, no 傳檔
//   主帳號 (fixed link / token)      files off            files ok           (n/a: always ok)
//   臨時帳號 (guest)                 files off            files ok           files off (FilesDeny::GuestNoFiles)
//
// 僅觀看 always wins (also for a guest made 僅觀看, whatever its files flag says). A session that
// lifted 僅觀看 with the password again (unlocked) is back to the plain rule for its account.
#pragma once

namespace pm::remote {

enum class FilesDeny { None, ViewOnly, GuestNoFiles };

// 僅觀看 for one session: the PC's switch (unless this session lifted it) or a 僅觀看 guest.
inline bool sessionViewOnly(bool pcViewOnly, bool unlocked, bool guestViewOnly) {
    return (pcViewOnly && !unlocked) || guestViewOnly;
}

inline FilesDeny filesDeny(bool pcViewOnly, bool unlocked, bool guest, bool guestViewOnly, bool guestFiles) {
    if (sessionViewOnly(pcViewOnly, unlocked, guestViewOnly)) return FilesDeny::ViewOnly;
    if (guest && !guestFiles) return FilesDeny::GuestNoFiles;
    return FilesDeny::None;
}

// What the web page is told ("filesWhy"): "" = allowed.
inline const char* filesWhy(FilesDeny d) {
    return d == FilesDeny::ViewOnly ? "viewOnly" : d == FilesDeny::GuestNoFiles ? "guest" : "";
}

}  // namespace pm::remote
