// 0.8.3.5: 允許 asked once per login, not once per connection (approval_memory.h), no network / UI.
//   pm_approval_test.exe          (exit code 0 = every check passed)
// The page opens a NEW session (POST session, the same device id and the same login) for every
// 切換畫質 (setQuality), 切換螢幕 (buildMonitorSeg), automatic quality step up / down, the temporary
// 降畫質, a reconnect after a short loss (connect('lost')), a reload and a tab opened again from the
// same browser (device id in localStorage). Until 0.8.3.4 a 臨時帳號 and the temporary address asked
// 允許 on the PC for each of them (owner log 2026-10-21 21:16:44).
// 1. A model of Host::Impl's session gate built from the same parts (ApprovalMemory +
//    letInWithoutAsking, the allowed / kicked / 20 s denied sets, the guest account's validity and the
//    temporary path's) drives each case; "0.8.3.4" rows show the old answer.
// 2. host.cpp is read: each place the model relies on is there (remember at 允許, forget at 拒絕 /
//    中斷 / account gone / temporary address off / fixed address off / host start + stop, the gate
//    asks letInWithoutAsking with okLogins.allows, a 用一次 account not spent by its own replacement).
// 3. index.html is read: every reconnect path goes through connect(), which has the one POST session
//    with the stored device id (so "the same device + the same login" really holds for each of them).
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "approval_memory.h"

using pm::remote::Admit;
using pm::remote::admitWithoutAsking;
using pm::remote::ApprovalMemory;
using pm::remote::ApprovalPolicies;
using pm::remote::ApprovalPolicy;
using pm::remote::AskFacts;
using pm::remote::letInWithoutAsking;

namespace {

int g_fail = 0;
void check(const std::string& name, bool ok, const std::string& detail = {}) {
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : "  -- ", detail.c_str());
    if (!ok) ++g_fail;
}

enum class Path { Lan, Fixed, Guest, Temp };
enum class Answer { In, Ask, Refused /* 401 / 403: logged out, link gone, just denied */ };
const char* str(Answer a) { return a == Answer::In ? "in" : a == Answer::Ask ? "asks 允許" : "refused"; }

// The gate of Host::Impl::sessionRequest + the host events that change it.
struct Gate {
    bool old0834 = false;  // the 0.8.3.4 rule (a guest / the temporary address always asks)
    bool fixedNeedApproval = false;
    ApprovalPolicies pol;  // 0.8.3.6 連線時要不要允許 (defaults: 主帳號 只用密碼, 臨時帳號 / 臨時網址 每次問)
    ApprovalMemory mem;
    std::set<std::string> allowed, kicked, remembered30;
    std::set<std::string> list;  // 0.8.3.6: the 記住 list, "<device>\t<scope>" (the app's remote_devices.txt)
    Admit lastAdmit = Admit::No;
    std::map<std::string, long long> deniedAt;  // device -> second
    std::map<std::string, std::string> guestOfToken;  // valid guest login token -> account id
    std::set<std::string> guests;                     // accounts that exist (not expired / used / deleted)
    std::set<std::string> tempPaths;                  // known paths of the temporary address
    bool tempOn = true;
    long long now = 1000;
    struct Pending {
        std::string device, cred, guestId;
        Path path;
    };
    std::vector<Pending> asked;

    Answer request(const std::string& device, Path path, const std::string& cred) {
        std::string guestId;
        if (path == Path::Guest) {  // guestFor(): a login of an account that is still there
            auto t = guestOfToken.find(cred);
            if (t == guestOfToken.end() || !guests.count(t->second)) return Answer::Refused;
            guestId = t->second;
        }
        if (path == Path::Temp && (!tempOn || !tempPaths.count(cred))) return Answer::Refused;  // routeTunnel: 404 / 403
        if (auto d = deniedAt.find(device); d != deniedAt.end() && now - d->second <= 20) return Answer::Refused;
        AskFacts f;
        f.guest = path == Path::Guest, f.viaTempLink = path == Path::Temp, f.fixedMain = path == Path::Fixed, f.kicked = kicked.count(device) > 0;
        ApprovalPolicy fixedPol = pol.fixed;
        if (fixedNeedApproval && fixedPol == ApprovalPolicy::PasswordOnly) fixedPol = ApprovalPolicy::Remember;  // as the host
        f.policy = f.guest ? pol.guest : f.viaTempLink ? pol.temp : fixedPol;
        const bool passwordIn = path == Path::Fixed && !fixedNeedApproval && !f.kicked;  // (the 0.8.3.4 rule)
        f.allowedDevice = allowed.count(device) > 0;
        const std::string scope = scopeOf(path, guestId);
        f.remembered = !f.kicked && (remembered30.count(device) > 0 || list.count(device + "\t" + scope) > 0) &&
                       (old0834 || pm::remote::listCounts(path == Path::Lan, f.policy));
        f.loginAllowed = !old0834 && mem.allows(device, ApprovalMemory::loginOf(guestId, cred));
        lastAdmit = old0834 ? Admit::No : admitWithoutAsking(f);
        const bool in = old0834 ? (!f.guest && (!f.requireApproval || (!f.viaTempLink && (f.allowedDevice || f.remembered)) || passwordIn))
                                : letInWithoutAsking(f);
        if (in) {
            if (path != Path::Guest) allowed.insert(device);
            return Answer::In;
        }
        asked.push_back({device, cred, guestId, path});
        return Answer::Ask;
    }
    static std::string scopeOf(Path path, const std::string& guestId) {
        return pm::remote::approvalScope(path == Path::Fixed || path == Path::Guest, path == Path::Temp || path == Path::Fixed || path == Path::Guest, guestId);
    }
    void allowRemember() {  // 允許 with 記住 ticked: the app writes the 記住 list (its scope)
        allow();
        list.insert(asked.back().device + "\t" + scopeOf(asked.back().path, asked.back().guestId));
    }
    void forgetListDevice(const std::string& device) {  // the app at 拒絕 / 中斷 (remote_devices.txt)
        for (auto it = list.begin(); it != list.end();) it = it->rfind(device + "\t", 0) == 0 ? list.erase(it) : std::next(it);
    }
    void allow() {  // Host::approve(id, true)
        const Pending p = asked.back();
        if (p.path != Path::Guest) allowed.insert(p.device);
        if (p.path == Path::Guest || p.path == Path::Temp) mem.remember(p.device, ApprovalMemory::loginOf(p.guestId, p.cred), p.guestId);
        kicked.erase(p.device);
    }
    void deny() {  // Host::approve(id, false)
        const Pending p = asked.back();
        deniedAt[p.device] = now;
        mem.forgetDevice(p.device);
        forgetListDevice(p.device);
    }
    void kick(const std::string& device) {  // 中斷 on the PC (Host::kick)
        allowed.erase(device), kicked.insert(device);
        mem.forgetDevice(device);
        forgetListDevice(device);
    }
    void guestGone(const std::string& id) {  // expired / 用一次 used / deleted (Host::setGuests)
        guests.erase(id);
        for (auto it = guestOfToken.begin(); it != guestOfToken.end();) it = it->second == id ? guestOfToken.erase(it) : std::next(it);
        mem.keepGuests(guests);
        for (auto it = list.begin(); it != list.end();) it = it->find("\tguest:" + id) != std::string::npos ? list.erase(it) : std::next(it);
    }
    void tempOff() {  // 臨時網址 turned off (Host::setTunnel(false))
        tempOn = false, tempPaths.clear();
        mem.forgetTempLinks();
    }
    void fixedOff() { mem.keepGuests({}); }  // 關閉固定網址 (Host::setFixed(false))
    void remoteOffOn() {  // 遠端控制 off, then on (Host::stop, Host::start)
        mem.clear(), allowed.clear(), kicked.clear(), deniedAt.clear();
        tempOn = true, tempPaths.clear();
    }
};

std::string readAll(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
size_t countOf(const std::string& s, const std::string& what) {
    size_t n = 0;
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + what.size())) ++n;
    return n;
}
// The body of the function whose definition starts with head (to the next "\n}\n").
std::string body(const std::string& src, const std::string& head) {
    const size_t a = src.find(head);
    if (a == std::string::npos) return {};
    const size_t b = src.find("\n}\n", a);
    return src.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

}  // namespace

int main(int argc, char** argv) {
    // The reconnects of the page: each is a new POST session with the same device id and login.
    const char* reconnects[] = {"切換畫質 (setQuality)", "切換螢幕 (buildMonitorSeg)", "自動調整畫質 step down", "自動調整畫質 step up",
                                "臨時降畫質", "斷線重連 (connect('lost'))", "重新整理頁面", "關掉分頁再打開 (same browser)"};
    const std::string dev = "phone-a-0001", dev2 = "phone-b-0002";

    // ---- 臨時帳號 ----
    for (const bool old : {true, false}) {
        Gate g;
        g.old0834 = old;
        g.guests = {"g1"}, g.guestOfToken = {{"fixed:tokA", "g1"}};
        const std::string v = old ? "0.8.3.4 " : "";
        check(v + "臨時帳號: the first connection asks 允許", g.request(dev, Path::Guest, "fixed:tokA") == Answer::Ask);
        g.allow();
        std::string asks;
        for (const char* why : reconnects)
            if (g.request(dev, Path::Guest, "fixed:tokA") != Answer::In) asks += std::string(why) + "; ";
        if (old) check("0.8.3.4 臨時帳號: every reconnect asked 允許 again (the bug)", asks.size() > 0 && countOf(asks, ";") == std::size(reconnects), asks);
        else check("臨時帳號: 切換畫質 / 切換螢幕 / 自動調整畫質 / 斷線重連 / 重新整理 / 重開分頁: no 允許 again", asks.empty(), asks);
    }
    {
        Gate g;
        g.guests = {"g1"}, g.guestOfToken = {{"fixed:tokA", "g1"}, {"fixed:tokB", "g1"}};
        g.request(dev, Path::Guest, "fixed:tokA"), g.allow();
        // A reload / the tab opened again: the page keeps a guest's token for that page only, the person types
        // the account's password again and gets a new token: the same account on the same phone.
        check("臨時帳號: 重新整理 / 重開分頁 (logged in again: a new token, the same account, the same phone): no 允許 again",
              g.request(dev, Path::Guest, "fixed:tokB") == Answer::In);
        check("臨時帳號: another phone with the same account asks", g.request(dev2, Path::Guest, "fixed:tokA") == Answer::Ask);
        g.guests.insert("g2"), g.guestOfToken["fixed:tokC"] = "g2";
        check("臨時帳號: another account on the same phone asks", g.request(dev, Path::Guest, "fixed:tokC") == Answer::Ask);
    }
    {
        Gate g;
        g.guests = {"g1"}, g.guestOfToken = {{"fixed:tokA", "g1"}};
        g.request(dev, Path::Guest, "fixed:tokA"), g.allow();
        g.request(dev, Path::Guest, "fixed:tokA");
        g.kick(dev);  // 中斷 / kicked on the PC
        check("臨時帳號: after 中斷 on the PC a reconnect asks again", g.request(dev, Path::Guest, "fixed:tokA") == Answer::Ask);
    }
    {
        Gate g;
        g.guests = {"g1"}, g.guestOfToken = {{"fixed:tokA", "g1"}};
        g.request(dev, Path::Guest, "fixed:tokA"), g.allow();
        g.request(dev, Path::Guest, "fixed:tokA");  // a 切換畫質 went straight in
        g.kick(dev);
        g.request(dev, Path::Guest, "fixed:tokA"), g.deny();  // asked again: 拒絕
        check("臨時帳號: 拒絕 -> refused for 20 s", g.request(dev, Path::Guest, "fixed:tokA") == Answer::Refused);
        g.now += 25;
        check("臨時帳號: 拒絕 -> after that a reconnect asks again (not let in)", g.request(dev, Path::Guest, "fixed:tokA") == Answer::Ask);
    }
    {
        Gate g;  // 拒絕 the very first time
        g.guests = {"g1"}, g.guestOfToken = {{"fixed:tokA", "g1"}};
        g.request(dev, Path::Guest, "fixed:tokA"), g.deny();
        g.now += 25;
        check("臨時帳號: 拒絕 the first time -> the reconnect asks again", g.request(dev, Path::Guest, "fixed:tokA") == Answer::Ask);
    }
    {
        Gate g;
        g.guests = {"g1"}, g.guestOfToken = {{"fixed:tokA", "g1"}};
        g.request(dev, Path::Guest, "fixed:tokA"), g.allow();
        g.guestGone("g1");  // expired
        check("臨時帳號: expired -> its login no longer works (refused: login again)", g.request(dev, Path::Guest, "fixed:tokA") == Answer::Refused);
        g.guests.insert("g1"), g.guestOfToken["fixed:tokA"] = "g1";  // (even if that login came back somehow)
        check("臨時帳號: expired -> the memory of 允許 is gone (asks)", g.request(dev, Path::Guest, "fixed:tokA") == Answer::Ask);
    }
    {
        Gate g;
        g.guests = {"g1", "g2"}, g.guestOfToken = {{"fixed:tokA", "g1"}, {"fixed:tokC", "g2"}};
        g.request(dev, Path::Guest, "fixed:tokA"), g.allow();
        g.request(dev2, Path::Guest, "fixed:tokC"), g.allow();
        g.guestGone("g1");  // deleted on the PC
        check("臨時帳號: deleted -> refused", g.request(dev, Path::Guest, "fixed:tokA") == Answer::Refused);
        check("臨時帳號: another account is not touched by it", g.request(dev2, Path::Guest, "fixed:tokC") == Answer::In);
        g.fixedOff();  // 關閉固定網址 on the PC
        check("臨時帳號: 關閉固定網址 on the PC -> asks again", g.request(dev2, Path::Guest, "fixed:tokC") == Answer::Ask);
    }
    {
        Gate g;
        g.guests = {"g1"}, g.guestOfToken = {{"fixed:tokA", "g1"}};
        g.request(dev, Path::Guest, "fixed:tokA"), g.allow();
        g.remoteOffOn();
        check("臨時帳號: 遠端控制 turned off and on -> asks again", g.request(dev, Path::Guest, "fixed:tokA") == Answer::Ask);
    }

    // ---- 臨時網址 (the temporary address, cloudflared) ----
    for (const bool old : {true, false}) {
        Gate g;
        g.old0834 = old;
        g.tempPaths = {"pathX"};
        const std::string v = old ? "0.8.3.4 " : "";
        check(v + "臨時網址: the first connection asks 允許", g.request(dev, Path::Temp, "pathX") == Answer::Ask);
        g.allow();
        std::string asks;
        for (const char* why : reconnects)
            if (g.request(dev, Path::Temp, "pathX") != Answer::In) asks += std::string(why) + "; ";
        if (old) check("0.8.3.4 臨時網址: every reconnect asked 允許 again (the bug)", countOf(asks, ";") == std::size(reconnects), asks);
        else check("臨時網址: 切換畫質 / 切換螢幕 / 自動調整畫質 / 斷線重連 / 重新整理 / 重開分頁: no 允許 again", asks.empty(), asks);
    }
    {
        Gate g;
        g.tempPaths = {"pathX", "pathY"};
        g.request(dev, Path::Temp, "pathX"), g.allow();
        check("臨時網址: another use of the one-time link (another path) asks", g.request(dev, Path::Temp, "pathY") == Answer::Ask);
        g.kick(dev);
        check("臨時網址: after 中斷 a reconnect asks again", g.request(dev, Path::Temp, "pathX") == Answer::Ask);
        g.deny();
        g.now += 25;
        check("臨時網址: 拒絕 -> asks again", g.request(dev, Path::Temp, "pathX") == Answer::Ask);
    }
    {
        Gate g;
        g.tempPaths = {"pathX"};
        g.request(dev, Path::Temp, "pathX"), g.allow();
        g.tempOff();
        check("臨時網址 turned off -> its link no longer works", g.request(dev, Path::Temp, "pathX") == Answer::Refused);
        g.tempOn = true, g.tempPaths = {"pathX"};  // (even the same path again)
        check("臨時網址 turned off -> the memory of 允許 is gone (asks)", g.request(dev, Path::Temp, "pathX") == Answer::Ask);
        g.allow();
        g.remoteOffOn();
        g.tempPaths = {"pathX"};
        check("臨時網址: 遠端控制 turned off and on -> asks again", g.request(dev, Path::Temp, "pathX") == Answer::Ask);
    }
    // ---- unchanged: LAN / Tailscale, 常駐外網 password ----
    {
        Gate g;
        check("LAN: the first connection asks", g.request(dev, Path::Lan, "") == Answer::Ask);
        g.allow();
        std::string asks;
        for (const char* why : reconnects)
            if (g.request(dev, Path::Lan, "") != Answer::In) asks += std::string(why) + "; ";
        check("LAN / Tailscale: reconnects go straight in (as before)", asks.empty(), asks);
        g.kick(dev);
        check("LAN: after 中斷 asks again (as before)", g.request(dev, Path::Lan, "") == Answer::Ask);
        Gate h;
        h.remembered30 = {dev};
        check("LAN: a phone on the 30-day 記住 list goes in (as before)", h.request(dev, Path::Lan, "") == Answer::In);
        check("臨時帳號 / 臨時網址: the 30-day 記住 list never lets them in", [&] {
            Gate k;
            k.remembered30 = {dev}, k.guests = {"g1"}, k.guestOfToken = {{"fixed:tokA", "g1"}}, k.tempPaths = {"pathX"};
            return k.request(dev, Path::Guest, "fixed:tokA") == Answer::Ask && k.request(dev, Path::Temp, "pathX") == Answer::Ask;
        }());
    }
    {
        Gate g;
        check("常駐外網 password: in without 允許 (as before)", g.request(dev, Path::Fixed, "fixed:pw1") == Answer::In);
        Gate h;
        h.fixedNeedApproval = true;
        check("常駐外網 password + 外面的新手機也要允許: asks once", h.request(dev, Path::Fixed, "fixed:pw1") == Answer::Ask);
        h.allow();
        check("常駐外網 password + 允許: reconnects go straight in (as before)", h.request(dev, Path::Fixed, "fixed:pw1") == Answer::In);
    }
    {
        ApprovalMemory m;
        m.remember("dev1", "fixed:t", "g1"), m.remember("dev10", "fixed:t", "g1"), m.remember("dev1", "pathX", "");
        m.forgetDevice("dev1");
        check("ApprovalMemory: forgetDevice is exact (dev1 does not drop dev10)", !m.allows("dev1", "fixed:t") && !m.allows("dev1", "pathX") && m.allows("dev10", "fixed:t"));
        m.remember("", "x", ""), m.remember("d", "", "");
        check("ApprovalMemory: an empty device / credential is never remembered", m.size() == 1 && !m.allows("", "x") && !m.allows("d", ""));
        for (int i = 0; i < 100; ++i) m.remember("d" + std::to_string(i), "c", "g");
        check("ApprovalMemory: bounded (64)", m.size() <= 64, std::to_string(m.size()));
    }

    // ---- 0.8.3.6 連線時要不要允許: every way in from outside x every policy ----
    {
        // (a) The defaults change nothing: every combination of facts gives the 0.8.3.5 answer.
        int diff = 0, n = 0;
        std::string first;
        for (int bits = 0; bits < (1 << 9); ++bits) {
            const bool guest = bits & 1, temp = bits & 2, fixedMain = bits & 4, req = bits & 8, allowedDev = bits & 16, rem = bits & 32,
                       kick = bits & 64, login = bits & 128, needOk = bits & 256;
            if ((guest && temp) || (guest && fixedMain) || (temp && fixedMain) || (kick && allowedDev) || (needOk && !fixedMain)) continue;
            ++n;
            const bool passwordIn = fixedMain && !needOk && !kick && !guest;
            const bool old0835 = ((guest || temp) && !kick && login) ||
                                 (!guest && (!req || (!temp && (allowedDev || (!kick && rem))) || passwordIn));
            AskFacts f;
            f.guest = guest, f.viaTempLink = temp, f.fixedMain = fixedMain, f.requireApproval = req, f.allowedDevice = allowedDev, f.kicked = kick,
            f.loginAllowed = login;
            const ApprovalPolicies def;
            f.policy = guest ? def.guest : temp ? def.temp : needOk ? ApprovalPolicy::Remember : def.fixed;
            // the host asks the 記住 list only where it counts (LAN, or 記住這台裝置)
            f.remembered = rem && !kick && !allowedDev && pm::remote::listCounts(!guest && !temp && !fixedMain, f.policy);
            if (letInWithoutAsking(f) != old0835 && ++diff == 1) first = std::to_string(bits);
        }
        check("0.8.3.6 defaults (主帳號 只用密碼 / 外面的新手機也要允許 = 記住, 臨時帳號 每次問, 臨時網址 每次問) = the 0.8.3.5 answer in every case",
              diff == 0 && n > 50, std::to_string(n) + " cases, " + std::to_string(diff) + " differ" + (first.empty() ? "" : ", first " + first));
        const ApprovalPolicies def;
        check("0.8.3.6 safe defaults: 臨時帳號 and 臨時網址 每次問, 主帳號 as before (只用密碼)",
              def.guest == ApprovalPolicy::Ask && def.temp == ApprovalPolicy::Ask && def.fixed == ApprovalPolicy::PasswordOnly);
    }
    {
        const char* pn[] = {"每次問", "記住這台裝置", "只用密碼"};
        const char* wn[] = {"主帳號 (固定網址)", "臨時帳號", "臨時網址"};
        const Path paths[] = {Path::Fixed, Path::Guest, Path::Temp};
        auto setup = [&](Gate& g, Path path, ApprovalPolicy p) {
            g.guests = {"g1", "g2"}, g.guestOfToken = {{"fixed:tokA", "g1"}, {"fixed:tokB", "g1"}, {"fixed:tokC", "g2"}};
            g.tempPaths = {"pathX", "pathY"};
            (path == Path::Fixed ? g.pol.fixed : path == Path::Guest ? g.pol.guest : g.pol.temp) = p;
        };
        auto cred = [](Path path, int i) {
            return path == Path::Guest ? std::string(i ? "fixed:tokB" : "fixed:tokA") : path == Path::Temp ? std::string(i ? "pathY" : "pathX") : std::string("fixed:pw1");
        };
        auto again = [](Gate& g) { g.remoteOffOn(), g.tempPaths = {"pathX", "pathY"}; };
        for (int w = 0; w < 3; ++w)
            for (int pi = 0; pi < 3; ++pi) {
                const Path path = paths[w];
                const ApprovalPolicy p = static_cast<ApprovalPolicy>(pi);
                const std::string tag = std::string("0.8.3.6 ") + wn[w] + " / " + pn[pi] + ": ";
                const bool pwOnly = p == ApprovalPolicy::PasswordOnly;
                {
                    Gate g;
                    setup(g, path, p);
                    const Answer a = g.request(dev, path, cred(path, 0));
                    check(tag + "the first connection " + (pwOnly ? "goes straight in (the password is right)" : "asks 允許"), a == (pwOnly ? Answer::In : Answer::Ask), str(a));
                    if (pwOnly) check(tag + "連線紀錄 says 只用密碼", g.lastAdmit == Admit::Password, pm::remote::admitName(g.lastAdmit));
                    if (!pwOnly) g.allow();
                    std::string asks;
                    for (const char* why : reconnects)
                        if (g.request(dev, path, cred(path, 0)) != Answer::In) asks += std::string(why) + "; ";
                    check(tag + "切換畫質 / 切換螢幕 / 斷線重連 / 重新整理: no 允許 again", asks.empty(), asks);
                    if (!pwOnly)
                        check(tag + "... let in as the same login / this phone this run", g.lastAdmit == (path == Path::Fixed ? Admit::ThisRun : Admit::SameLogin),
                              pm::remote::admitName(g.lastAdmit));
                    g.kick(dev);
                    const Answer k = g.request(dev, path, cred(path, 0));
                    check(tag + "after 中斷 on the PC: asks 允許 again (whatever the policy)", k == Answer::Ask, str(k));
                    g.deny();
                    check(tag + "拒絕: refused for 20 s", g.request(dev, path, cred(path, 0)) == Answer::Refused);
                    g.now += 25;
                    check(tag + "拒絕: then asks again", g.request(dev, path, cred(path, 0)) == Answer::Ask);
                }
                {
                    Gate g;  // 允許 with 記住 ticked, then 遠端控制 off / on (this run's memory is gone; the list stays)
                    setup(g, path, p);
                    if (g.request(dev, path, cred(path, 0)) == Answer::Ask) g.allowRemember();
                    again(g);
                    const Answer a = g.request(dev, path, cred(path, 1));
                    const bool expectIn = p != ApprovalPolicy::Ask;
                    check(tag + "記住 ticked, next time (遠端控制 off and on, a new login): " + (expectIn ? "straight in" : "asks again (每次問 keeps no list)"),
                          a == (expectIn ? Answer::In : Answer::Ask), str(a));
                    if (p == ApprovalPolicy::Remember) {
                        check(tag + "連線紀錄 says 記住的裝置", g.lastAdmit == Admit::Remembered, pm::remote::admitName(g.lastAdmit));
                        check(tag + "another phone is not let in by it", g.request(dev2, path, cred(path, 1)) == Answer::Ask);
                    }
                }
                {
                    Gate g;  // allowed without 記住: the next run asks (unless 只用密碼)
                    setup(g, path, p);
                    if (g.request(dev, path, cred(path, 0)) == Answer::Ask) g.allow();
                    again(g);
                    const Answer a = g.request(dev, path, cred(path, 1));
                    check(tag + "允許 without 記住, next run: " + (pwOnly ? "straight in" : "asks again"), a == (pwOnly ? Answer::In : Answer::Ask), str(a));
                }
                if (p == ApprovalPolicy::Remember) {
                    Gate g;  // 記住 then 中斷: the list entry is gone too
                    setup(g, path, p);
                    g.request(dev, path, cred(path, 0)), g.allowRemember();
                    g.kick(dev);
                    again(g);
                    check(tag + "記住 then 中斷: asks again, also after a restart (the list entry is gone)", g.request(dev, path, cred(path, 1)) == Answer::Ask);
                    Gate h;  // 記住, later 中斷 + 拒絕
                    setup(h, path, p);
                    h.request(dev, path, cred(path, 0)), h.allowRemember();
                    again(h);
                    h.request(dev, path, cred(path, 1));
                    h.kick(dev), h.request(dev, path, cred(path, 0)), h.deny();
                    again(h), h.now += 25;
                    check(tag + "記住 then 拒絕: asks again (the list entry is gone)", h.request(dev, path, cred(path, 1)) == Answer::Ask);
                    Gate k;  // the list counts only with 記住這台裝置
                    setup(k, path, p);
                    k.request(dev, path, cred(path, 0)), k.allowRemember();
                    again(k);
                    (path == Path::Fixed ? k.pol.fixed : path == Path::Guest ? k.pol.guest : k.pol.temp) = ApprovalPolicy::Ask;
                    check(tag + "on the list, the policy changed to 每次問: asks (the list does not count)", k.request(dev, path, cred(path, 1)) == Answer::Ask);
                }
            }
        {
            Gate g;  // 臨時帳號 記住: only for that account
            g.guests = {"g1", "g2"}, g.guestOfToken = {{"fixed:tokA", "g1"}, {"fixed:tokC", "g2"}};
            g.pol.guest = ApprovalPolicy::Remember;
            g.request(dev, Path::Guest, "fixed:tokA"), g.allowRemember();
            g.remoteOffOn();
            check("0.8.3.6 臨時帳號 記住: another 臨時帳號 on the same phone asks", g.request(dev, Path::Guest, "fixed:tokC") == Answer::Ask);
            g.pol.fixed = ApprovalPolicy::Remember;
            check("0.8.3.6 臨時帳號 記住: not the 主帳號 either (asks)", g.request(dev, Path::Fixed, "fixed:pw1") == Answer::Ask);
            g.guestGone("g1");  // deleted / expired / used up (重設密碼: the app drops that account's entries too)
            g.guests.insert("g1"), g.guestOfToken["fixed:tokA"] = "g1";
            check("0.8.3.6 臨時帳號 記住: the account gone -> its list entry gone (asks)", g.request(dev, Path::Guest, "fixed:tokA") == Answer::Ask);
        }
        {
            Gate g;  // 臨時網址 記住: a new temporary address next time, the same phone goes in
            g.tempPaths = {"pathX"};
            g.pol.temp = ApprovalPolicy::Remember;
            g.request(dev, Path::Temp, "pathX"), g.allowRemember();
            g.tempOff();
            g.tempOn = true, g.tempPaths = {"pathZ"};
            check("0.8.3.6 臨時網址 記住: a new temporary address (link + 通行碼 again): the remembered phone goes in",
                  g.request(dev, Path::Temp, "pathZ") == Answer::In && g.lastAdmit == Admit::Remembered);
            g.guests = {"g1"}, g.guestOfToken = {{"fixed:tokA", "g1"}};
            g.pol.guest = ApprovalPolicy::Remember;
            check("0.8.3.6 臨時網址 記住: not as a 臨時帳號 (asks)", g.request(dev, Path::Guest, "fixed:tokA") == Answer::Ask);
            Gate h;
            h.tempPaths = {"pathX"}, h.pol.temp = ApprovalPolicy::PasswordOnly;
            check("0.8.3.6 臨時網址 只用密碼: a wrong / spent link is still refused", h.request(dev, Path::Temp, "pathGone") == Answer::Refused);
            Gate k;
            k.guestOfToken = {{"fixed:tokA", "g1"}}, k.pol.guest = ApprovalPolicy::PasswordOnly;  // g1 no longer exists
            check("0.8.3.6 臨時帳號 只用密碼: an expired / deleted account is still refused", k.request(dev, Path::Guest, "fixed:tokA") == Answer::Refused);
        }
        {
            Gate g;  // LAN / Tailscale: the policies do not touch it
            g.pol = {ApprovalPolicy::Ask, ApprovalPolicy::PasswordOnly, ApprovalPolicy::PasswordOnly};
            check("0.8.3.6 LAN: the policies do not touch it (asks)", g.request(dev, Path::Lan, "") == Answer::Ask);
            g.allowRemember();
            g.remoteOffOn();
            check("0.8.3.6 LAN: 記住 works as before (straight in)", g.request(dev, Path::Lan, "") == Answer::In && g.lastAdmit == Admit::Remembered);
        }
        check("0.8.3.6 policy names round-trip (settings.ini / the spike)", [] {
            for (ApprovalPolicy p : {ApprovalPolicy::Ask, ApprovalPolicy::Remember, ApprovalPolicy::PasswordOnly}) {
                ApprovalPolicy q{};
                if (!pm::remote::parsePolicy(pm::remote::policyName(p), q) || q != p) return false;
            }
            ApprovalPolicy q = ApprovalPolicy::Ask;
            return !pm::remote::parsePolicy("", q) && !pm::remote::parsePolicy("always", q) && q == ApprovalPolicy::Ask;
        }());
        check("0.8.3.6 scopes: LAN / 主帳號 / 臨時帳號 / 臨時網址 each its own",
              pm::remote::approvalScope(false, false, "") == "lan" && pm::remote::approvalScope(true, true, "") == "fixed" &&
                  pm::remote::approvalScope(true, true, "g1") == "guest:g1" && pm::remote::approvalScope(false, true, "") == "temp");
    }

    // ---- the host and the page really do it this way ----
    std::string dir = argc > 1 ? argv[1] : std::string(PM_REMOTE_SOURCE_DIR);
    const std::string host = readAll(dir + "/src/host.cpp"), page = readAll(dir + "/web/index.html");
    check("sources found (host.cpp, index.html)", !host.empty() && !page.empty(), dir);
    const std::string approve = body(host, "void Host::approve("), kick = body(host, "void Host::kick("), req = body(host, "HttpServer::Response Host::Impl::sessionRequest(");
    const std::string offer = body(host, "HttpServer::Response Host::Impl::offerFor("), guests = body(host, "void Host::setGuests("),
                      spent = body(host, "void Host::Impl::guestSessionEnded("), tunnel = body(host, "void Host::setTunnel("),
                      fixed = body(host, "void Host::setFixed("), start = body(host, "bool Host::start("), stop = body(host, "void Host::stop(");
    check("host: 允許 remembers a 臨時帳號 / temporary-address login", approve.find("if (s->guest() || (s->viaTunnel && !s->viaFixed)) d.okLogins.remember(s->device, ApprovalMemory::loginOf(s->guestId, s->cred), s->guestId);") != std::string::npos);
    check("host: 拒絕 forgets that phone", approve.find("d.okLogins.forgetDevice(s->device);") != std::string::npos);
    check("host: 中斷 / kick forgets that phone", kick.find("d.okLogins.forgetDevice(s->device);") != std::string::npos);
    check("host: the gate is admitWithoutAsking with okLogins.allows(device, loginOf(...))", req.find("f.loginAllowed = okLogins.allows(device, ApprovalMemory::loginOf(s->guestId, cred));") != std::string::npos &&
                                                                                       req.find("const Admit admit = admitWithoutAsking(f);") != std::string::npos);
    check("host: a guest login is checked first (expired / used / deleted: 401)", req.find("guestFor(cred.substr(6), ga)") < req.find("admitWithoutAsking"));
    // 0.8.3.6: the gate takes each way in's policy (copied under tunMu), asks the 記住 list only where it counts,
    // with that way in's scope, and keeps what let it in for the 連線紀錄 (approve(): 允許 / 允許 + 記住).
    check("host: 0.8.3.6 the policy of the way in (臨時帳號 / 臨時網址 / 主帳號) goes into the gate",
          req.find("f.policy = f.guest ? pol.guest : viaTempLink ? pol.temp : pol.fixed;") != std::string::npos && req.find("pol = policies;") != std::string::npos);
    check("host: 0.8.3.6 外面的新手機也要允許 (fixedNeedApproval) = 記住這台裝置",
          req.find("if (fixedNeedsOk && pol.fixed == ApprovalPolicy::PasswordOnly) pol.fixed = ApprovalPolicy::Remember;") != std::string::npos);
    check("host: 0.8.3.6 the 記住 list is asked with its scope, only where it counts",
          req.find("listCounts(lan, f.policy) && opt.remembered && opt.remembered(device, scope)") != std::string::npos);
    check("host: 0.8.3.6 what let it in is kept (s->admit) and 允許 says whether 記住 was ticked",
          req.find("s->admit = admitName(admit);") != std::string::npos && approve.find("s->admit = remember ? \"allowed-remember\" : \"allowed\";") != std::string::npos);
    check("host: 0.8.3.6 setApprovalPolicies sets them under tunMu", body(host, "void Host::setApprovalPolicies(").find("d_->policies = p;") != std::string::npos &&
                                                                   start.find("d.policies = o.policies;") != std::string::npos);
    check("host: 0.8.3.6 the old passwordIn shortcut is gone (one rule: admitWithoutAsking)", host.find("passwordIn") == std::string::npos);
    check("host: accounts gone -> forgotten", guests.find("d.okLogins.keepGuests(keep);") != std::string::npos);
    check("host: a 用一次 account used -> forgotten", spent.find("okLogins.forgetGuest(s->guestId);") != std::string::npos);
    check("host: 臨時網址 off (and on) -> its paths forgotten", tunnel.find("d.okLogins.forgetTempLinks();") != std::string::npos);
    check("host: 關閉固定網址 -> every 臨時帳號 forgotten", fixed.find("d.okLogins.keepGuests({});") != std::string::npos);
    check("host: 遠端控制 off / on -> all forgotten", start.find("d.okLogins.clear();") != std::string::npos && stop.find("d.okLogins.clear();") != std::string::npos);
    check("host: a 用一次 account is not spent when its own login replaces its session (切換畫質)",
          offer.find("if (old->guest() && old->guestId == s->guestId && old->device == s->device) old->guestDone = true;") != std::string::npos);
    const std::string mem = readAll(dir + "/src/approval_memory.h");
    check("host: the memory stays in memory (one remember call; approval_memory.h has no file / settings access)",
          countOf(host, "okLogins.remember(") == 1 && !mem.empty() && mem.find("fstream") == std::string::npos && mem.find("fopen") == std::string::npos && mem.find("CreateFile") == std::string::npos);
    // Other host actions that could make an allowed phone ask again: only start() clears the sets.
    check("host: allowedDevices cleared only in start() (僅觀看 / 聲音 / 畫質 / settings never do)", countOf(host, "allowedDevices.clear()") == 1 && start.find("allowedDevices.clear()") != std::string::npos);
    check("host: allowedDevices.erase only in kick()", countOf(host, "allowedDevices.erase(") == 1 && kick.find("allowedDevices.erase(") != std::string::npos);
    // The page: one POST session, with the stored device id; every reconnect is connect(...).
    check("page: one POST session, with the stored device id", countOf(page, "post('session', JSON.stringify({device, q: curQ, m: prefs.mon") == 1 &&
                                                                 page.find("let device = store.get('device', '');") != std::string::npos);
    // 0.8.3.6: 切換畫質 / 每秒畫面 / 解析度跟手機一樣 / 切換螢幕 / 自動調整畫質 go through applyStream() (inside the
    // connection); it falls back to connect('switch') (not connected / beyond the level / no answer), as does 臨時降畫質.
    const size_t sw = countOf(page, "connect('switch')"), lost = countOf(page, "connect('lost')"), as = countOf(page, "applyStream('");
    check("page: picture changes = applyStream() falling back to connect('switch'), reconnects = connect('lost')", sw >= 4 && lost >= 5 && as >= 9,
          std::to_string(as) + " applyStream, " + std::to_string(sw) + " switch, " + std::to_string(lost) + " lost");
    printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
