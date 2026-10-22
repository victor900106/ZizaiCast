// 0.8.3.5: 允許 given on the PC to a 臨時帳號 login or to a temporary-address (免安裝外網) path, for the
// phone (device id) that asked. Until 0.8.3.4 both asked again for every new session, and the page
// opens a new session for every 切換畫質 / 切換螢幕 / automatic quality step / reconnect after a short
// loss / reload (remote/web/index.html connect('switch' | 'lost')): each one popped 允許 up on the PC
// again (owner log 2026-10-21 21:16:44). Now the same device with the same credential goes straight
// back in while that credential is valid: a 臨時帳號 = its account (loginOf: "guest:<id>"; the page keeps a
// guest's login token for that page only, so after a reload / a tab opened again the person types the
// account's password again and gets a new token: that is the same account, not a new 允許), the
// temporary address = its own path key (the one-time link spent by this phone, /c/<key>/). In memory only (never the 30-day 記住 list), and forgotten
// when the PC says no: 拒絕, 中斷 / kicked, the account expired / used up / deleted, the temporary address
// turned off, remote control turned off (the host stops). Pure (no locking: Host::Impl::smu guards it);
// tested by remote/tools/approval_test.cpp.
#pragma once

#include <map>
#include <set>
#include <string>

#include "pm/remote.h"  // ApprovalPolicy (0.8.3.6)

namespace pm::remote {

class ApprovalMemory {
public:
    // What a login is remembered by (see above).
    static std::string loginOf(const std::string& guestId, const std::string& cred) { return guestId.empty() ? cred : "guest:" + guestId; }
    // guestId: the 臨時帳號's id; "" for the temporary address (cred = its path key).
    void remember(const std::string& device, const std::string& cred, const std::string& guestId) {
        if (device.empty() || cred.empty()) return;
        ok_[key(device, cred)] = guestId;
        if (ok_.size() > 64) ok_.erase(ok_.begin());  // bounded (a handful of phones at most)
    }
    // Allowed before, with this very credential (the caller has checked it is still valid: the guest
    // account exists and has not expired / been used, the temporary path is still known).
    bool allows(const std::string& device, const std::string& cred) const { return !device.empty() && !cred.empty() && ok_.count(key(device, cred)) > 0; }
    // 拒絕 / 中斷 / kicked: that phone asks again, whatever it used.
    void forgetDevice(const std::string& device) {
        for (auto it = ok_.begin(); it != ok_.end();) it = it->first.compare(0, device.size() + 1, device + '\n') == 0 ? ok_.erase(it) : std::next(it);
    }
    // 臨時帳號 gone (expired, 用一次 used, deleted): every login of an account not in keep.
    void keepGuests(const std::set<std::string>& keep) {
        for (auto it = ok_.begin(); it != ok_.end();) it = !it->second.empty() && !keep.count(it->second) ? ok_.erase(it) : std::next(it);
    }
    void forgetGuest(const std::string& guestId) {
        for (auto it = ok_.begin(); it != ok_.end();) it = !guestId.empty() && it->second == guestId ? ok_.erase(it) : std::next(it);
    }
    // The temporary address turned off (every path of it is invalid).
    void forgetTempLinks() {
        for (auto it = ok_.begin(); it != ok_.end();) it = it->second.empty() ? ok_.erase(it) : std::next(it);
    }
    void clear() { ok_.clear(); }  // remote control off / on (Host::start / stop)
    size_t size() const { return ok_.size(); }

private:
    static std::string key(const std::string& device, const std::string& cred) { return device + '\n' + cred; }
    std::map<std::string, std::string> ok_;  // device \n cred -> guest id ("" = temporary address)
};

// Host::Impl::sessionRequest's question: does this new session go in without 允許 on the PC?
// 0.8.3.6: and why (連線紀錄 says which policy let it in). The policy is the one of its way in
// (Options::policies: 主帳號 on the fixed address / 臨時帳號 / 臨時網址); LAN / Tailscale has none (as before:
// allowed this run, or on the 記住 list).
struct AskFacts {
    bool guest = false;           // a 臨時帳號 login (on the fixed address)
    bool viaTempLink = false;     // the temporary address (cloudflared)
    bool fixedMain = false;       // 主帳號 on the fixed address: came in with the owner's password (not a guest)
    ApprovalPolicy policy = ApprovalPolicy::Ask;  // of guest / viaTempLink / fixedMain (unused for LAN / Tailscale)
    bool requireApproval = true;  // Options::requireApproval (false: tests / the spike only)
    bool allowedDevice = false;   // allowed while this host runs (LAN / Tailscale / fixed link; device id)
    bool remembered = false;      // on the PC's 記住 list for this way in (approvalScope)
    bool kicked = false;          // 中斷 / kicked on the PC: asks again whatever it remembers, whatever the policy
    bool loginAllowed = false;    // 0.8.3.5: ApprovalMemory::allows(device, cred)
};
enum class Admit { No, NoApproval, SameLogin, ThisRun, Remembered, Password };
inline const char* admitName(Admit a) {
    switch (a) {
    case Admit::NoApproval: return "open";
    case Admit::SameLogin: return "same-login";
    case Admit::ThisRun: return "this-run";
    case Admit::Remembered: return "remembered";
    case Admit::Password: return "password";
    default: return "";
    }
}
inline Admit admitWithoutAsking(const AskFacts& f) {
    if (!f.guest && !f.requireApproval) return Admit::NoApproval;
    if (f.kicked) return Admit::No;  // 中斷 on the PC: 允許 once more (0.8.3: also with the password)
    if (f.guest || f.viaTempLink) {
        if (f.policy == ApprovalPolicy::PasswordOnly) return Admit::Password;  // the password / link + 通行碼 were right
        if (f.loginAllowed) return Admit::SameLogin;                           // 0.8.3.5: the same login, allowed before
        if (f.policy == ApprovalPolicy::Remember && f.remembered) return Admit::Remembered;
        return Admit::No;  // a 臨時帳號 / the temporary address is never let in by its device alone
    }
    if (f.fixedMain && f.policy == ApprovalPolicy::PasswordOnly) return Admit::Password;  // 常駐外網 as before 0.8.3.6
    if (f.allowedDevice) return Admit::ThisRun;
    if ((!f.fixedMain || f.policy == ApprovalPolicy::Remember) && f.remembered) return Admit::Remembered;
    return Admit::No;
}
inline bool letInWithoutAsking(const AskFacts& f) { return admitWithoutAsking(f) != Admit::No; }
// Where the 記住 list counts (the host asks the app only then; the 允許 dialog offers 記住 only then).
inline bool listCounts(bool lan, ApprovalPolicy policy) { return lan || policy == ApprovalPolicy::Remember; }

}  // namespace pm::remote
