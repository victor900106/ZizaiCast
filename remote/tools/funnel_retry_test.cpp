// 0.8.3.1: 常駐外網 start-up against a STUBBED tailscale CLI (TailscaleFunnel::setRunnerForTest):
// no process is ever started, the owner's Funnel (443 / 8443) cannot be touched.
//   pm_funnel_test.exe            (exit code 0 = every check passed)
// The stub keeps a fake tailscaled: BackendState per call, the serve config (443 = someone
// else's, never to change; 8443 = ours), and records every command line. Cases:
//   boot-kept     NoState, NoState, Starting, then Running with 8443 already kept by tailscaled
//                 (the owner's 09:14 reboot): waits 2, 4, 8 s, then up without any change
//   boot-new      Starting, then Running without 8443: one `funnel --bg --https=8443 <origin>`
//   no-daemon     the CLI cannot reach tailscaled yet (exit 1): waited for like NoState
//   needs-login   NeedsLogin: no waiting (the person has to sign in), fails at once
//   never         never Running: 2 4 8 16 32 60 60 60 58 s (300 s), then "still-starting"
//   0.8.3         what 0.8.3 did at the owner's 09:14 reboot: start() once, NoState = failed
//   cancelled     the wait is cancelled (app closed): stops at once
//   443-changed   the funnel command changes port 443 (it must not): failure, no retry
// Every case also checks: no command other than status / funnel status / funnel --bg
// --https=8443 http://127.0.0.2:8091; 443's config is identical at the end.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "pm/remote.h"
#include "util.h"

using pm::remote::TailscaleFunnel;

namespace {

const std::string kOrigin = "http://127.0.0.2:8091";
const std::string kName = "pc.tail-test.ts.net";
// Port 443 as on the owner's PC (another app's Funnel): must stay byte-for-byte the same.
const std::string k443Web = R"({"Handlers":{"/":{"Proxy":"http://127.0.0.1:8790"}}})";

struct FakeTailscale {
    std::vector<std::string> states;  // BackendState per `status --json` call (last one repeats)
    bool daemonDown = false;          // first calls: the CLI cannot connect (exit 1)
    int daemonDownCalls = 0;
    bool has8443 = false;             // tailscaled's kept serve config
    bool funnelChanges443 = false;    // a broken CLI that touches 443 (must be caught)
    bool changed443 = false;
    int statusCalls = 0;
    std::vector<std::string> commands;
    std::vector<std::string> bad;     // commands the app must never run

    std::string funnelJson() const {
        std::string web = "\"" + kName + ":443\":" + (changed443 ? std::string(R"({"Handlers":{"/":{"Proxy":"http://127.0.0.1:1"}}})") : k443Web);
        std::string allow = "\"" + kName + ":443\":true";
        if (has8443) {
            web += ",\"" + kName + ":8443\":{\"Handlers\":{\"/\":{\"Proxy\":\"" + kOrigin + "\"}}}";
            allow += ",\"" + kName + ":8443\":true";
        }
        return "{\"TCP\":{\"443\":{\"HTTPS\":true}" + std::string(has8443 ? ",\"8443\":{\"HTTPS\":true}" : "") + "},\"Web\":{" + web +
               "},\"AllowFunnel\":{" + allow + "}}";
    }
    bool run(const std::wstring& wcmd, std::string& out, unsigned long& code) {
        std::string cmd;  // (ASCII command lines)
        for (wchar_t ch : wcmd) cmd += ch < 128 ? char(ch) : '?';
        commands.push_back(cmd);
        const std::string exe = "\"tailscale-test-stub.exe\" ";
        if (cmd.rfind(exe, 0) != 0) bad.push_back("not the stub: " + cmd);
        const std::string args = cmd.substr(std::min(cmd.size(), exe.size()));
        if (args == "status --json") {
            if (daemonDown && daemonDownCalls-- > 0) {
                out = "failed to connect to local Tailscale daemon for /localapi/v0/status; not running? Is Tailscale running?";
                code = 1;
                return true;
            }
            const std::string st = states.empty() ? "Running" : states[std::min<size_t>(size_t(statusCalls), states.size() - 1)];
            ++statusCalls;
            out = "{\"BackendState\":\"" + st + "\",\"Self\":{\"DNSName\":\"" + kName + ".\",\"CapMap\":{\"funnel\":null}}}";
            code = 0;
            return true;
        }
        if (args == "funnel status --json") {
            out = funnelJson();
            code = 0;
            return true;
        }
        if (args == "funnel --bg --https=8443 " + kOrigin) {
            has8443 = true;
            if (funnelChanges443) changed443 = true;
            out = "Available on the internet:\nhttps://" + kName + ":8443/";
            code = 0;
            return true;
        }
        bad.push_back(cmd);  // anything else (443, off, reset, serve ...) is a failure
        out = "refused by the test";
        code = 1;
        return true;
    }
};

int g_fail = 0;
void check(const char* name, bool ok, const std::string& detail = {}) {
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : "  -- ", detail.c_str());
    if (!ok) ++g_fail;
}
std::string join(const std::vector<int>& v) {
    std::string s;
    for (int x : v) s += (s.empty() ? "" : ",") + std::to_string(x);
    return s;
}

struct Result {
    bool ok = false;
    std::string url, err;
    std::vector<int> waits;
};
Result run(FakeTailscale& f, int cancelAfter = -1) {
    TailscaleFunnel::setRunnerForTest([&f](const std::wstring& c, std::string& o, unsigned long& code) { return f.run(c, o, code); });
    Result r;
    r.ok = TailscaleFunnel::startPatiently(8443, kOrigin, r.url, r.err, [&](int, int secs) {
        if (cancelAfter >= 0 && int(r.waits.size()) >= cancelAfter) return false;
        r.waits.push_back(secs);  // (no real sleeping: the stub's clock is the list)
        return true;
    });
    TailscaleFunnel::setRunnerForTest(nullptr);
    return r;
}
void common(const char* c, const FakeTailscale& f) {
    std::string bad;
    for (const auto& b : f.bad) bad += b + "; ";
    check((std::string(c) + ": only status / funnel status / funnel --bg --https=8443 run").c_str(), f.bad.empty(), bad);
    check((std::string(c) + ": port 443 unchanged").c_str(), !f.changed443 || f.funnelChanges443);
}

}  // namespace

int main() {
    pm::remote::setLogHandler([](const std::string& l) { printf("    log: %s\n", l.c_str()); });
    {
        FakeTailscale f;
        f.states = {"NoState", "Running"};
        f.has8443 = true;
        TailscaleFunnel::setRunnerForTest([&f](const std::wstring& c, std::string& o, unsigned long& code) { return f.run(c, o, code); });
        std::string url, err;
        const bool ok = TailscaleFunnel::start(8443, kOrigin, url, err);  // 0.8.3: the only try
        TailscaleFunnel::setRunnerForTest(nullptr);
        printf("INFO 0.8.3 behaviour (one start(), no retry): %s  err=%s\n", ok ? "up" : "FAILED", err.c_str());
        check("0.8.3 behaviour reproduced: NoState at the first try = failed", !ok && err == "not-running: NoState", err);
    }
    {
        FakeTailscale f;
        f.states = {"NoState", "NoState", "Starting", "Running"};
        f.has8443 = true;
        const Result r = run(f);
        check("boot-kept: up after Tailscale started", r.ok && r.url == "https://" + kName + ":8443/", r.err);
        check("boot-kept: waited 2, 4, 8 s", join(r.waits) == "2,4,8", join(r.waits));
        bool ran = false;
        for (const auto& c : f.commands) ran |= c.find("funnel --bg") != std::string::npos;
        check("boot-kept: 8443 kept by tailscaled -> no funnel command at all", !ran);
        check("boot-kept: 8443 -> 127.0.0.2:8091", f.has8443 && f.funnelJson().find(kName + ":8443\":{\"Handlers\":{\"/\":{\"Proxy\":\"" + kOrigin) != std::string::npos);
        common("boot-kept", f);
    }
    {
        FakeTailscale f;
        f.states = {"Starting", "Running"};
        const Result r = run(f);
        int n = 0;
        for (const auto& c : f.commands) n += c.find("funnel --bg --https=8443 " + kOrigin) != std::string::npos;
        check("boot-new: up on the 2nd try", r.ok && join(r.waits) == "2", join(r.waits) + " " + r.err);
        check("boot-new: exactly one funnel --bg --https=8443 http://127.0.0.2:8091", n == 1, std::to_string(n));
        common("boot-new", f);
    }
    {
        FakeTailscale f;
        f.daemonDown = true, f.daemonDownCalls = 2, f.has8443 = true;
        const Result r = run(f);
        check("no-daemon: CLI cannot connect yet = wait, then up", r.ok && join(r.waits) == "2,4", join(r.waits) + " " + r.err);
        common("no-daemon", f);
    }
    {
        FakeTailscale f;
        f.states = {"NeedsLogin"};
        const Result r = run(f);
        check("needs-login: fails at once, no waiting", !r.ok && r.waits.empty() && r.err == "not-running: NeedsLogin", r.err);
        check("needs-login: not taken for a start-up", !TailscaleFunnel::backendStarting(r.err));
        common("needs-login", f);
    }
    {
        FakeTailscale f;
        f.states = {"NoState"};
        const Result r = run(f);
        int total = 0;
        for (int w : r.waits) total += w;
        check("never: 2 4 8 16 32 60 60 60 58 s, then gives up", !r.ok && join(r.waits) == "2,4,8,16,32,60,60,60,58", join(r.waits));
        check("never: 5 minutes at most", total == 300, std::to_string(total) + " s");
        check("never: err says still starting", r.err == "still-starting: NoState", r.err);
        check("never: no funnel command while not Running", f.commands.size() == r.waits.size() + 1);
        common("never", f);
    }
    {
        FakeTailscale f;
        f.states = {"NoState"};
        const Result r = run(f, 2);
        check("cancelled: stops at once", !r.ok && r.err == "cancelled" && r.waits.size() == 2, r.err);
        common("cancelled", f);
    }
    {
        FakeTailscale f;
        f.states = {"Starting", "Running"};
        f.funnelChanges443 = true;
        const Result r = run(f);
        check("443-changed: caught, no retry", !r.ok && r.err == "443-changed" && r.waits.size() == 1, r.err);
        common("443-changed", f);
    }
    check("backendStarting: NoState / Starting yes, Stopped / NeedsLogin no",
          TailscaleFunnel::backendStarting("not-running: NoState") && TailscaleFunnel::backendStarting("not-running: Starting") &&
              !TailscaleFunnel::backendStarting("not-running: Stopped") && !TailscaleFunnel::backendStarting("not-running: NeedsLogin") &&
              !TailscaleFunnel::backendStarting("no-tailscale") && !TailscaleFunnel::backendStarting("443-changed"));
    printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
