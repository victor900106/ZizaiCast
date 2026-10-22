// 0.8.3.6 連線時要不要允許 (每次問 / 記住這台裝置 / 只用密碼) on the app's side, no window, no network, no host.
//   pm_auth_policy_test.exe        (exit code 0 = every check passed)
// - settings.ini: no file / a file from before 0.8.3.6 (外面的新手機也要電腦按允許 on or off) / each saved value read
//   back: 臨時帳號 and 臨時網址 default to 每次問, the 主帳號 keeps what it did (只用密碼, or 記住 with the old switch on),
//   remote_fixed_approval is still written for older versions, a bad value falls back to the default
// - remembered_devices.h (remote_devices.txt): lines from before 0.8.3.6 (no scope) = LAN / the owner's own ways in,
//   scopes kept apart (a 臨時帳號's entry counts for that account only, a 臨時網址 entry for the temporary address only),
//   every clearing rule: 拒絕 / 中斷 (that device everywhere), a 臨時帳號 deleted / expired / used up (keepGuests) or
//   重設密碼 (its scope), the 主帳號 password changed ("fixed"), one row / 全部忘記; no IP address is ever stored,
//   bad ids / scopes / tabs in labels refused or cleaned, the list bounded
// - the sources: remote_actions.cpp calls each clearing rule where it happens (拒絕 in the 允許 dialog, kick(),
//   resetGuest(), saveGuests() for gone accounts, the password panel), offers 記住 only where the list counts,
//   gives the host the policies at start and on change, writes what let a phone in into the 連線紀錄, and the menu
//   has the 9 radio rows (3 ways in x 3 policies) wired to remoteCommand()
#include "core/base/settings.h"
#include "remote/remembered_devices.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

// settings.h's only outside needs (core/base/util.cpp has them with the whole app's dependencies).
namespace pm_app {
std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
}  // namespace pm_app

using namespace pm_app;

namespace {

int g_fail = 0;
void check(const std::string& name, bool ok, const std::string& detail = {}) {
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : "  -- ", detail.c_str());
    if (!ok) ++g_fail;
}
std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
void writeAll(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << s;
}
size_t countOf(const std::string& s, const std::string& what) {
    size_t n = 0;
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + what.size())) ++n;
    return n;
}
// The body of the function whose definition starts with head (to the next "\n}\n", CRLF or LF).
std::string body(std::string src, const std::string& head) {
    for (size_t at = src.find("\r\n"); at != std::string::npos; at = src.find("\r\n", at)) src.erase(at, 1);
    const size_t a = src.find(head);
    if (a == std::string::npos) return {};
    const size_t b = src.find("\n}\n", a);
    return src.substr(a, b == std::string::npos ? std::string::npos : b - a);
}
std::string pols(const Settings& s) { return std::to_string(s.remotePolicyFixed) + std::to_string(s.remotePolicyGuest) + std::to_string(s.remotePolicyTemp); }

}  // namespace

int main(int argc, char** argv) {
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const fs::path dir = fs::path(tmp) / (L"pm_auth_policy_test_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path ini = dir / L"settings.ini";

    // ---- settings.ini ----
    {
        fs::remove(ini, ec);
        const Settings s = Settings::load(ini);
        check("settings: no file -> 主帳號 只用密碼 (as before), 臨時帳號 每次問, 臨時網址 每次問", pols(s) == "200" && !s.remoteFixedNeedApproval, pols(s));
    }
    {
        writeAll(ini, "[settings]\nlanguage=zh-TW\nremote_fixed_approval=0\n");
        const Settings s = Settings::load(ini);
        check("settings: 0.8.3.5 file, 外面的新手機也要允許 off -> 主帳號 只用密碼, the others 每次問", pols(s) == "200", pols(s));
        writeAll(ini, "[settings]\nremote_fixed_approval=1\n");
        const Settings t = Settings::load(ini);
        check("settings: 0.8.3.5 file, 外面的新手機也要允許 on -> 主帳號 記住這台裝置 (it asked and offered 記住), the others 每次問",
              pols(t) == "100" && t.remoteFixedNeedApproval, pols(t));
    }
    for (int f = 0; f < 3; ++f)
        for (int gp = 0; gp < 3; ++gp)
            for (int tp = 0; tp < 3; ++tp) {
                Settings s;
                s.remotePolicyFixed = f, s.remotePolicyGuest = gp, s.remotePolicyTemp = tp;
                s.save(ini);
                const Settings r = Settings::load(ini);
                const std::string text = readAll(ini);
                if (pols(r) != pols(s) || r.remoteFixedNeedApproval != (f != 2) ||
                    text.find(std::string("remote_fixed_approval=") + (f != 2 ? "1" : "0")) == std::string::npos)
                    check("settings: saved and read back " + pols(s), false, pols(r) + "\n" + text);
            }
    check("settings: all 27 combinations saved and read back (remote_fixed_approval follows the 主帳號 for older versions)", true);
    {
        Settings s;
        s.remotePolicyFixed = 0, s.remotePolicyGuest = 2, s.remotePolicyTemp = 1;
        s.save(ini);
        const std::string text = readAll(ini);
        check("settings: written as words (ask / remember / password)", text.find("remote_policy_fixed=ask") != std::string::npos &&
                                                                          text.find("remote_policy_guest=password") != std::string::npos &&
                                                                          text.find("remote_policy_temp=remember") != std::string::npos);
        writeAll(ini, "[settings]\nremote_fixed_approval=1\nremote_policy_fixed=always\nremote_policy_guest=\nremote_policy_temp=PASSWORD\n");
        const Settings r = Settings::load(ini);
        check("settings: a bad / empty value -> the safe default (主帳號 follows the old switch, others 每次問)", pols(r) == "100", pols(r));
    }

    // ---- remembered_devices.h ----
    const std::string A = "a1b2c3d4e5f60718293a4b5c6d7e8f90", B = "ffffeeee0000111122223333aaaabbbb", C = "c0ffee00c0ffee00c0ffee00c0ffee00";
    {
        RememberedDevices d;
        d.parse(A + "\tiPhone · Safari\t2026-10-01\r\n" + B + "\tAndroid · Chrome\t2026-10-02\nbadid\tx\t2026\n\n");
        check("list: lines from before 0.8.3.6 (no scope) read as LAN, a bad id dropped, CRLF / LF", d.size() == 2 && d.list()[0].scope == "lan" && d.list()[1].label == "Android · Chrome",
              std::to_string(d.size()));
        check("list: a LAN entry counts for the 主帳號 on the fixed address too (as before: one list for the owner's own ways in)",
              d.has(A, "lan") && d.has(A, "fixed"));
        check("list: ... never for a 臨時帳號 or the temporary address", !d.has(A, "guest:g1") && !d.has(A, "temp"));
        check("list: written back with its scope", d.serialize().find(A + "\tiPhone · Safari\t2026-10-01\tlan\n") == 0, d.serialize());
    }
    {
        RememberedDevices d;
        check("add: a 臨時帳號 entry", d.add(A, "iPhone · Safari", "2026-10-22", "guest:9f3a"));
        check("add: the same device + scope twice -> once", !d.add(A, "iPhone · Safari", "2026-10-22", "guest:9f3a") && d.size() == 1);
        check("scope: a 臨時帳號 entry counts for that account only", d.has(A, "guest:9f3a") && !d.has(A, "guest:77aa") && !d.has(A, "fixed") && !d.has(A, "lan") && !d.has(A, "temp"));
        d.add(A, "iPhone · Safari", "2026-10-22", "temp");
        check("scope: a 臨時網址 entry counts for the temporary address only", d.has(A, "temp") && !d.has(B, "temp") && !d.has(A, "fixed"));
        check("add: a bad id / scope is refused", !d.add("short", "x", "d", "lan") && !d.add(B, "x", "d", "guest:") && !d.add(B, "x", "d", "everything") &&
                                                       !d.add(B, "x", "d", "guest:a\tb") && !d.add("192.168.1.37", "x", "d", "lan"));
        d.add(B, "Bad\tLabel\nnext", "2026-10-22", "fixed");
        check("add: tabs / newlines in a label cleaned (one line per entry)", d.serialize().find("Bad Label next\t") != std::string::npos && countOf(d.serialize(), "\n") == 3);
        RememberedDevices e;
        e.parse(d.serialize());
        check("list: written and read back the same", e.serialize() == d.serialize());
    }
    {
        RememberedDevices d;  // the clearing rules
        auto fill = [&] {
            d.clear();
            d.add(A, "a", "d", "lan"), d.add(A, "a", "d", "fixed"), d.add(A, "a", "d", "guest:g1"), d.add(A, "a", "d", "temp");
            d.add(B, "b", "d", "guest:g1"), d.add(B, "b", "d", "guest:g2"), d.add(C, "c", "d", "fixed"), d.add(C, "c", "d", "lan");
        };
        fill();
        check("拒絕 / 中斷: every entry of that device goes, the others stay", d.forgetDevice(A) == 4 && !d.has(A, "lan") && !d.has(A, "temp") && d.size() == 4);
        fill();
        check("臨時帳號 重設密碼 / deleted: its entries go (that account only)", d.forgetScope("guest:g1") == 2 && !d.has(A, "guest:g1") && !d.has(B, "guest:g1") && d.has(B, "guest:g2"));
        fill();
        check("臨時帳號 expired / used up / deleted (accounts left: g2): g1's entries go", d.keepGuests({"g2"}) == 2 && d.has(B, "guest:g2") && d.has(A, "temp"));
        check("... no account left: every 臨時帳號 entry goes, nothing else", d.keepGuests({}) == 1 && d.size() == 5);
        fill();
        const size_t n = d.forgetScope("fixed");
        check("主帳號 password changed: the fixed-address entries go (LAN ones stay)", n == 2 && !d.list().empty() && d.has(C, "lan") && d.has(A, "lan"));
        fill();
        RememberedPhone gone;
        check("one row forgotten from the menu", d.removeAt(1, &gone) && gone.id == A && gone.scope == "fixed" && d.size() == 7 && !d.removeAt(99));
        d.clear();
        check("全部忘記", d.size() == 0 && d.serialize().empty());
        check("forgetDevice(\"\") forgets nothing", [&] { fill(); return d.forgetDevice("") == 0 && d.size() == 8; }());
    }
    {
        RememberedDevices d;
        for (int i = 0; i < 260; ++i) d.add("dev" + std::to_string(100000 + i), "x", "d", "lan");
        check("list: bounded (" + std::to_string(RememberedDevices::kMax) + ", oldest out)", d.size() == RememberedDevices::kMax && !d.has("dev100000", "lan") && d.has("dev100259", "lan"));
    }

    // ---- the sources ----
    const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::path(PM_APP_SOURCE_DIR);
    const std::string ra = readAll(root / L"remote" / L"remote_actions.cpp"), rd = readAll(root / L"remote" / L"remembered_devices.h");
    const std::string cmds = readAll(root / L"core" / L"app" / L"commands.cpp");
    check("sources found", !ra.empty() && !rd.empty() && !cmds.empty(), root.string());
    const std::string req = body(ra, "void onRemoteEvent(");
    check("允許 dialog: 記住 offered only where the list counts (LAN / Tailscale, or 記住這台裝置)",
          req.find("const bool offerRemember = way < 0 || policySetting(way) == 1;") != std::string::npos &&
              req.find("remember = allow && offerRemember && rs.ask.checked();") != std::string::npos);
    check("允許 dialog: 記住 writes the entry with its way in's scope", req.find("if (remember) rememberDevice(device, label, scope);") != std::string::npos &&
                                                                       req.find("const std::string scope = scopeOf(m->fixed, m->tunnel, m->guestId);") != std::string::npos);
    check("拒絕 in the 允許 dialog: that device is forgotten", req.find("if (choice == 2) forgetDeviceAll(device, \"拒絕 on the PC\");") != std::string::npos);
    check("中斷 (kick: the bar, Ctrl+D, the menu): that device is forgotten", body(ra, "void kick(").find("forgetDeviceAll(device, \"中斷 on the PC\")") != std::string::npos);
    check("臨時帳號 重設密碼: its entries forgotten", body(ra, "void resetGuest(").find("forgetScope(\"guest:\" + v[i].id,") != std::string::npos);
    check("臨時帳號 deleted / expired / used up (every saveGuests): entries of gone accounts forgotten",
          body(ra, "void saveGuests(").find("R().devices.keepGuests(ids)") != std::string::npos &&
              body(ra, "void deleteGuest(").find("saveGuests();") != std::string::npos && body(ra, "bool pruneGuests(").find("saveGuests();") != std::string::npos &&
              body(ra, "void guestSessionEnded(").find("saveGuests();") != std::string::npos);
    check("主帳號 password changed: fixed-address entries forgotten", body(ra, "void openPasswordPanel(").find("forgetScope(\"fixed\",") != std::string::npos);
    check("the host gets the policies at start and on every change", ra.find("o.policies = approvalPolicies();") != std::string::npos &&
                                                                        body(ra, "void setPolicy(").find("R().host.setApprovalPolicies(approvalPolicies());") != std::string::npos);
    check("the host asks the list with the scope", ra.find("return R().devices.has(device, scope);") != std::string::npos);
    check("連線紀錄: what let it in (policy:admit) on each connection", req.find("\"ok\", m->policy + \":\" + m->admit);") != std::string::npos);
    check("menu: 3 ways in x 3 policies as radio rows (CmdRemotePolicy0 + way * 3 + policy)",
          body(ra, "MenuItem policyMenu(").find("CmdRemotePolicy0 + static_cast<UINT>(way * 3 + p)") != std::string::npos &&
              body(ra, "MenuItem policyMenu(").find("m.checked = policySetting(way) == p;") != std::string::npos);
    check("menu: each row reaches setPolicy (remoteCommand <- commands.cpp)", ra.find("setPolicy(static_cast<int>(cmd - CmdRemotePolicy0) / 3, static_cast<int>(cmd - CmdRemotePolicy0) % 3);") != std::string::npos &&
                                                                              cmds.find("cmd >= CmdRemotePolicy0 && cmd <= CmdRemotePolicyLast") != std::string::npos);
    check("menu: the old 外面的新手機也要電腦按允許 row is gone (its place: 連線時要不要允許 ▸)",
          ra.find("checkItem(CmdRemoteFixedApproval") == std::string::npos && body(ra, "MenuItem remoteSettingsMenu(").find("sub.push_back(policyMenu());") != std::string::npos);
    // 0.8.4: the tray / right-click submenu is only 開/關, 複製網址, 中斷, 打開設定…; the full settings are the 進階設定 menu of the page
    check("menu 0.8.4: the tray / right-click 遠端操控 submenu holds only 開關, 複製網址, 中斷 and 打開設定",
          body(ra, "MenuItem remoteItem(").find("CmdRemoteToggle") != std::string::npos && body(ra, "MenuItem remoteItem(").find("CmdRemoteAddrCopy") != std::string::npos &&
              body(ra, "MenuItem remoteItem(").find("CmdRemoteKick") != std::string::npos && body(ra, "MenuItem remoteItem(").find("CmdRemoteOpenSettings") != std::string::npos &&
              body(ra, "MenuItem remoteItem(").find("policyMenu") == std::string::npos && body(ra, "MenuItem remoteItem(").find("CmdRemotePassword") == std::string::npos &&
              body(ra, "MenuItem remoteItem(").find("CmdRemoteViewOnly") == std::string::npos && body(ra, "MenuItem remoteItem(").find("CmdRemoteIdle") == std::string::npos);
    check("menu 0.8.4: 進階設定 (the page) is built from the full list and keeps 連線紀錄",
          body(ra, "std::vector<MenuItem> remoteAdvancedItems() {").find("remoteSettingsMenu()") != std::string::npos &&
              body(ra, "std::vector<MenuItem> remoteAdvancedItems() {").find("CmdRemoteLog") != std::string::npos);
    check("menu 0.8.4: 打開設定… selects the 遠端控制 page and brings the window up", ra.find("case CmdRemoteOpenSettings:") != std::string::npos &&
                                                                                  body(ra, "void remoteCommand(").find("g.settings.idleTab = 1;") != std::string::npos);
    check("the list holds no IP address: an entry is id, label, date, scope only (rememberDevice gets no address)",
          rd.find("std::string id, label, date, scope;") != std::string::npos && ra.find("void rememberDevice(const std::string& id, const std::string& label, const std::string& scope)") != std::string::npos);
    check("remote_devices.txt is written in one place (saveDevicesLocked), the file removed when empty",
          countOf(ra, "devicesFile(), std::ios::app") == 0 && body(ra, "void saveDevicesLocked(").find("fs::remove(devicesFile(), ec);") != std::string::npos);

    fs::remove_all(dir, ec);
    printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
