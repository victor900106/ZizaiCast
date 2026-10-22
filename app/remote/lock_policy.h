// 自在投影 app: 0.8.3 遠端斷線後自動鎖定電腦 -- when a remote session's end locks the PC (pure, tested
// by app/tools/pm_lock_default_test.cpp). Both switches are OFF by default (settings.h; the owner wants
// them to stay off) and nothing else can turn locking on: the host's own copy (Host::setLockAfterSession)
// only tells the page whether it will happen.
#pragma once

#include <string>

namespace pm_app {

// outside: the session came over the fixed address (常駐外網, also every 臨時帳號 login) or the
// temporary address (免安裝外網); else LAN / Tailscale. why: how it ended (RemEnded's reason).
// Only when that path's switch is on AND the phone's side ended it (it left, went idle, lost the
// connection), never when the person at the PC did (中斷 / 關閉 / turned a link off / 拒絕).
inline bool shouldLockAfterSession(bool outside, const std::string& why, bool lockOutside, bool lockLan) {
    if (!(outside ? lockOutside : lockLan)) return false;
    for (const char* pcSide : {"kicked", "host stopping", "remote control turned off", "replaced", "常駐外網 turned off", "外網連結 turned off", "denied"})
        if (why.rfind(pcSide, 0) == 0) return false;
    return true;
}

}  // namespace pm_app
