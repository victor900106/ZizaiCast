// 0.8.3.5: 遠端斷線後自動鎖定電腦 stays OFF unless the person turns it on (the owner's request).
//   pm_lock_default_test.exe        (exit code 0 = every check passed; no window, no network, never locks)
// - settings.ini: no file at all / a file from before 0.8.3 (no remote_lock_* keys) / an empty value /
//   the saved defaults read back: remoteLockOutside and remoteLockLan are false
// - lock_policy.h: with both off, no session end locks the PC: 臨時帳號 and 常駐外網 (fixed address),
//   免安裝外網 (temporary address), LAN / Tailscale, whatever the reason; with one on, only its path
//   and only when the phone's side ended it
// - the sources: LockWorkStation is called in one place only (lockAfterSession, after
//   shouldLockAfterSession and the --dev stub), the host never locks (its setLockAfterSession copy
//   only tells the page), and the app gives the host the settings' values (no other initial value)
#include "core/base/settings.h"
#include "remote/lock_policy.h"

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
size_t countOf(const std::string& s, const std::string& what) {
    size_t n = 0;
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + what.size())) ++n;
    return n;
}

}  // namespace

int main() {
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const fs::path dir = fs::path(tmp) / (L"pm_lock_default_test_" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(dir);
    auto write = [&](const wchar_t* name, const std::string& text) {
        const fs::path p = dir / name;
        std::ofstream(p, std::ios::binary) << text;
        return p;
    };
    auto both = [](const Settings& s) { return std::string("outside ") + (s.remoteLockOutside ? "on" : "off") + ", LAN " + (s.remoteLockLan ? "on" : "off"); };

    // ---- the defaults ----
    {
        const Settings s;
        check("Settings{}: both off", !s.remoteLockOutside && !s.remoteLockLan, both(s));
    }
    {
        const Settings s = Settings::load(dir / L"no-such-settings.ini");  // a first start: no settings.ini
        check("no settings.ini: both off", !s.remoteLockOutside && !s.remoteLockLan, both(s));
    }
    {
        // A settings.ini from 0.8.2 (常駐外網 on, a password): no remote_lock_* keys at all.
        const Settings s = Settings::load(write(L"old082.ini", "[settings]\r\nlanguage=zh-TW\r\nremote_port=8091\r\nremote_persistent=1\r\n"
                                                               "remote_pw_hash=pbkdf2-sha256$310000$00$00\r\nremote_fixed_secret=ab\r\n"
                                                               "remote_fixed_approval=1\r\nremote_audio=1\r\n"));
        check("old settings.ini without remote_lock_outside / remote_lock_lan: both off", !s.remoteLockOutside && !s.remoteLockLan, both(s));
    }
    {
        const Settings s = Settings::load(write(L"empty-values.ini", "[settings]\nremote_lock_outside=\nremote_lock_lan=\n"));
        check("remote_lock_*= (empty values): both off", !s.remoteLockOutside && !s.remoteLockLan, both(s));
    }
    {
        const Settings s = Settings::load(write(L"junk.ini", "[settings]\nremote_lock_outside=yes\nremote_lock_lan=true\n"));
        check("remote_lock_*=yes / true (not \"1\"): both off", !s.remoteLockOutside && !s.remoteLockLan, both(s));
    }
    {
        const fs::path p = dir / L"saved.ini";
        Settings().save(p);
        const std::string text = readAll(p);
        const Settings s = Settings::load(p);
        check("defaults saved and read back: both off", !s.remoteLockOutside && !s.remoteLockLan && text.find("remote_lock_outside=0") != std::string::npos &&
                                                            text.find("remote_lock_lan=0") != std::string::npos,
              both(s));
    }
    {
        const Settings s = Settings::load(write(L"on.ini", "[settings]\nremote_lock_outside=1\nremote_lock_lan=1\n"));
        check("turned on by the person (=1): read as on (the keys do work)", s.remoteLockOutside && s.remoteLockLan, both(s));
    }

    // ---- which session ends lock the PC ----
    struct End {
        const char* path;
        bool outside;
    };
    const End paths[] = {{"臨時帳號 (fixed address)", true}, {"常駐外網 password login", true}, {"免安裝外網 (temporary address)", true}, {"LAN", false}, {"Tailscale", false}};
    const char* phoneSide[] = {"the phone left", "idle 30 min", "connection lost", "timeout", "closed on the phone", ""};
    const char* pcSide[] = {"kicked", "host stopping", "remote control turned off", "replaced", "常駐外網 turned off", "外網連結 turned off", "denied"};
    {
        const Settings d;  // the defaults
        std::string locked;
        for (const End& e : paths) {
            for (const char* w : phoneSide)
                if (shouldLockAfterSession(e.outside, w, d.remoteLockOutside, d.remoteLockLan)) locked += std::string(e.path) + "/" + w + "; ";
            for (const char* w : pcSide)
                if (shouldLockAfterSession(e.outside, w, d.remoteLockOutside, d.remoteLockLan)) locked += std::string(e.path) + "/" + w + "; ";
        }
        check("defaults: no session end locks the PC (臨時帳號, fixed, temporary, LAN, Tailscale; every reason)", locked.empty(), locked);
    }
    {
        std::string wrong;
        for (const End& e : paths)
            for (const char* w : phoneSide) {
                if (shouldLockAfterSession(e.outside, w, true, false) != e.outside) wrong += std::string("outside-only ") + e.path + "/" + w + "; ";
                if (shouldLockAfterSession(e.outside, w, false, true) != !e.outside) wrong += std::string("LAN-only ") + e.path + "/" + w + "; ";
            }
        check("one switch on: only its own path locks (the phone ended it)", wrong.empty(), wrong);
    }
    {
        std::string wrong;
        for (const End& e : paths)
            for (const char* w : pcSide)
                if (shouldLockAfterSession(e.outside, w, true, true)) wrong += std::string(e.path) + "/" + w + "; ";
        check("both on: ended at the PC (中斷 / 關閉 / 拒絕 / link off) never locks", wrong.empty(), wrong);
    }

    // ---- no other path ----
    const fs::path src = PM_SOURCE_DIR;
    const std::string actions = readAll(src / "app" / "remote" / "remote_actions.cpp");
    const std::string host = readAll(src / "remote" / "src" / "host.cpp");
    check("sources found", !actions.empty() && !host.empty());
    const size_t call = actions.find("    LockWorkStation();");
    const size_t fn = actions.find("void lockAfterSession(const RemoteMsg& m) {");
    const size_t rule = actions.find("if (!shouldLockAfterSession(outside, why, g.settings.remoteLockOutside, g.settings.remoteLockLan)) return;", fn);
    const size_t stub = actions.find("if (g.dev) {", fn);
    check("app: LockWorkStation() called once, in lockAfterSession, after the rule and the --dev stub",
          countOf(actions, "LockWorkStation();") == 1 && fn != std::string::npos && rule != std::string::npos && stub != std::string::npos && fn < rule && rule < stub &&
              stub < call && actions.find("\n}", fn) > call);
    check("host: never locks the PC itself", host.find("LockWorkStation") == std::string::npos);
    check("host: its copy starts off (lockOutside{false}, lockLan{false})", host.find("std::atomic<bool> lockOutside{false}, lockLan{false};") != std::string::npos);
    check("host: its copy is only told to the page (\"lockAfter\")", countOf(host, "lockOutside") == 3 && countOf(host, "lockLan") == 3, std::to_string(countOf(host, "lockOutside")) + " / " + std::to_string(countOf(host, "lockLan")));
    {
        // Every setLockAfterSession call in the app passes the settings' two values.
        size_t n = 0, ok = 0;
        for (size_t at = actions.find("setLockAfterSession("); at != std::string::npos; at = actions.find("setLockAfterSession(", at + 1)) {
            ++n;
            if (actions.compare(at, 76, "setLockAfterSession(g.settings.remoteLockOutside, g.settings.remoteLockLan);") == 0) ++ok;
        }
        check("app: every setLockAfterSession(...) gets g.settings.remoteLockOutside / remoteLockLan", n >= 2 && ok == n, std::to_string(ok) + " of " + std::to_string(n));
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
