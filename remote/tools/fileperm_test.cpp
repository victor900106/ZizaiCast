// 0.8.3.5 files: the 傳檔 permission table (host side), no network / device / window.
//   pm_fileperm_test            exit 0 = PASS
// Locks: 主帳號 / 臨時帳號 / 僅觀看 x (PC switch on or off, session unlocked or not), and what a new
// 臨時帳號 gets from the panel's two switches. The expected table is written out by hand below,
// independent of the code under test; the web page's twin is remote/tools/fileperm_web_test.mjs.
#include <cstdio>
#include <cstring>

#include "file_perm.h"
#include "guest_rights.h"

using namespace pm::remote;

static int fails = 0, checks = 0;
static void expect(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++fails;
        std::printf("FAIL  %s\n", what);
    }
}

int main() {
    // ---- the three cases the owner has ----
    // 主帳號 (the fixed link's own password, or the LAN token): files on
    expect(filesDeny(false, false, false, false, false) == FilesDeny::None, "main account: files on");
    expect(filesDeny(false, false, false, false, true) == FilesDeny::None, "main account (guest flag ignored): files on");
    // 臨時帳號 made 可操作 + 可以傳檔: on; 可操作 without 可以傳檔: off, and the reason is the guest's, not 僅觀看
    expect(filesDeny(false, false, true, false, true) == FilesDeny::None, "guest control + files: on");
    expect(filesDeny(false, false, true, false, false) == FilesDeny::GuestNoFiles, "guest control, no files: off (guest)");
    // 僅觀看: off whatever else, and 僅觀看 is the reason given
    expect(filesDeny(true, false, false, false, false) == FilesDeny::ViewOnly, "main account, PC 僅觀看: off (viewOnly)");
    expect(filesDeny(false, false, true, true, true) == FilesDeny::ViewOnly, "guest 僅觀看 even with files flag: off (viewOnly)");
    expect(filesDeny(true, false, true, false, true) == FilesDeny::ViewOnly, "guest with files, PC 僅觀看: off (viewOnly)");
    // the password lifted the PC's 僅觀看 for this session: the plain rule again
    expect(filesDeny(true, true, false, false, false) == FilesDeny::None, "main account, 僅觀看 lifted: on");
    expect(filesDeny(true, true, true, true, true) == FilesDeny::ViewOnly, "unlock never lifts a 僅觀看 guest");
    expect(filesDeny(true, true, true, false, false) == FilesDeny::GuestNoFiles, "guest no files, 僅觀看 lifted: still off (guest)");

    // ---- every combination against the hand-written rule ----
    for (int m = 0; m < 32; ++m) {
        const bool pcVo = m & 1, unl = m & 2, guest = m & 4, gVo = m & 8, gFiles = m & 16;
        const bool vo = (pcVo && !unl) || gVo;
        const FilesDeny want = vo ? FilesDeny::ViewOnly : (guest && !gFiles) ? FilesDeny::GuestNoFiles : FilesDeny::None;
        char what[96];
        std::snprintf(what, sizeof what, "table pcVo=%d unl=%d guest=%d gVo=%d gFiles=%d", pcVo, unl, guest, gVo, gFiles);
        expect(filesDeny(pcVo, unl, guest, gVo, gFiles) == want, what);
        expect(sessionViewOnly(pcVo, unl, gVo) == vo, what);
    }

    // ---- what the page is told ----
    expect(std::strcmp(filesWhy(FilesDeny::None), "") == 0, "why none");
    expect(std::strcmp(filesWhy(FilesDeny::ViewOnly), "viewOnly") == 0, "why viewOnly");
    expect(std::strcmp(filesWhy(FilesDeny::GuestNoFiles), "guest") == 0, "why guest");

    // ---- the new-account panel: default and the 僅觀看 interlock ----
    using pm::ui::guestFilesEffective;
    expect(pm::ui::kGuestFilesDefault, "default: 可以傳檔 ticked");
    expect(guestFilesEffective(false, pm::ui::kGuestFilesDefault), "default 可操作 account can transfer");
    expect(!guestFilesEffective(true, true), "僅觀看 account never stores files");
    expect(!guestFilesEffective(false, false), "可操作 with the box cleared: no files");
    expect(guestFilesEffective(false, true), "back to 可操作: the tick returns");

    std::printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
