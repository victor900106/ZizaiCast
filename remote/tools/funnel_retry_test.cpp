// 0.8.3.1: 常駐外網 start-up against a STUBBED tailscale CLI (TailscaleFunnel::setRunnerForTest):
// no process is ever started, no real Funnel (443 / 8443) can be touched.
//   pm_funnel_test.exe            (exit code 0 = every check passed)
// The stub keeps a fake tailscaled: BackendState per call, the serve config (443 = someone
// else's, never to change; 8443 = ours), and records every command line. Cases:
//   boot-kept     NoState, NoState, Starting, then Running with 8443 already kept by tailscaled
//                 (right after a reboot): waits 2, 4, 8 s, then up without any change
//   boot-new      Starting, then Running without 8443: one `funnel --bg --https=8443 <origin>`
//   no-daemon     the CLI cannot reach tailscaled yet (exit 1): waited for like NoState
//   needs-login   NeedsLogin: no waiting (the person has to sign in), fails at once
//   never         never Running: 2 4 8 16 32 60 60 60 58 s (300 s), then "still-starting"
//   0.8.3         what 0.8.3 did right after a reboot: start() once, NoState = failed
//   cancelled     the wait is cancelled (app closed): stops at once
//   443-changed   the funnel command changes port 443 (it must not): failure, no retry
// Every case also checks: no command other than status / funnel status / funnel --bg
// --https=8443 http://127.0.0.2:8091; 443's config is identical at the end.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <string>
#include <vector>

#include "pm/remote.h"
#include "util.h"

using pm::remote::TailscaleFunnel;

namespace {

const std::string kOrigin = "http://127.0.0.2:8091";
const std::string kName = "example-pc.example.ts.net";  // a made-up name
// Port 443 already served by something else on the same machine (a made-up local service):
// must stay byte-for-byte the same.
const std::string k443Web = R"({"Handlers":{"/":{"Proxy":"http://127.0.0.1:8080"}}})";

struct FakeTailscale {
    std::vector<std::string> states;  // BackendState per `status --json` call (last one repeats)
    bool daemonDown = false;          // first calls: the CLI cannot connect (exit 1)
    int daemonDownCalls = 0;
    bool has8443 = false;             // tailscaled's kept serve config
    bool funnelChanges443 = false;    // a broken CLI that touches 443 (must be caught)
    bool changed443 = false;
    int statusCalls = 0;
    std::string bgOut;                // 0.8.3.2: what "funnel --bg" prints instead of working (+ bgCode)
    unsigned long bgCode = 1;
    std::string other8443;            // 0.8.3.2: 8443 already serves this (another program)
    std::vector<std::string> commands;
    std::vector<std::string> bad;     // commands the app must never run

    std::string funnelJson() const {
        std::string web = "\"" + kName + ":443\":" + (changed443 ? std::string(R"({"Handlers":{"/":{"Proxy":"http://127.0.0.1:1"}}})") : k443Web);
        std::string allow = "\"" + kName + ":443\":true";
        if (has8443 || !other8443.empty()) {
            web += ",\"" + kName + ":8443\":{\"Handlers\":{\"/\":{\"Proxy\":\"" + (other8443.empty() ? kOrigin : other8443) + "\"}}}";
            allow += ",\"" + kName + ":8443\":true";
        }
        return "{\"TCP\":{\"443\":{\"HTTPS\":true}" + std::string(has8443 || !other8443.empty() ? ",\"8443\":{\"HTTPS\":true}" : "") + "},\"Web\":{" + web +
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
        if (args == "funnel --bg --https=8443 " + kOrigin && !bgOut.empty()) {
            out = bgOut, code = bgCode;
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
    // ---- 0.8.3.2: what the person has to do (no retry, the right key), against the stub ----
    struct Case {
        const char* name;
        std::string out, want;
    };
    for (const Case& c : {Case{"enable-funnel", "Funnel is not enabled on your tailnet.\nTo enable, visit:\n\n         https://login.tailscale.com/f/funnel?node=nX1\n",
                               "needs-enable: funnel https://login.tailscale.com/f/funnel?node=nX1"},
                          Case{"enable-https", "Serve is not enabled on your tailnet.\nTo enable, visit:\n\n         https://login.tailscale.com/f/serve?node=nX2\n",
                               "needs-enable: https https://login.tailscale.com/f/serve?node=nX2"},
                          Case{"no-attr", "Funnel not available; \"funnel\" node attribute not set. See https://tailscale.com/s/no-funnel.",
                               "no-funnel-attr: https://tailscale.com/s/no-funnel"},
                          Case{"old-cli", "flag provided but not defined: -bg\nUSAGE", "cli-too-old"}}) {
        FakeTailscale f;
        f.bgOut = c.out;
        const Result r = run(f);
        check((std::string(c.name) + ": no retry, " + c.want).c_str(), !r.ok && r.err == c.want && r.waits.empty(), r.err);
        check((std::string(c.name) + ": not taken for a start-up").c_str(), !TailscaleFunnel::backendStarting(r.err));
        common(c.name, f);
    }
    {
        FakeTailscale f;
        f.other8443 = "http://127.0.0.1:9000";
        const Result r = run(f);
        int bg = 0;
        for (const auto& c : f.commands) bg += c.find("funnel --bg") != std::string::npos;
        check("port-taken: 8443 of another program is left alone (no funnel command)", !r.ok && r.err == "port-taken: http://127.0.0.1:9000" && bg == 0, r.err);
        common("port-taken", f);
    }
    // ---- 0.8.3.2: a real (fake) CLI process: an enable page and never exiting, silence, cancel ----
    wchar_t me[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, me, MAX_PATH);
    std::wstring dir = me;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    const std::wstring fake = dir + L"pm_fake_tailscale.exe";
    if (GetFileAttributesW(fake.c_str()) == INVALID_FILE_ATTRIBUTES) {
        check("fake CLI present (pm_fake_tailscale.exe next to this test)", false);
    } else {
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring pids = std::wstring(tmp) + L"pm_fake_ts_pids_" + std::to_wstring(GetCurrentProcessId()) + L".txt";
        const std::wstring state = std::wstring(tmp) + L"pm_fake_ts_state_" + std::to_wstring(GetCurrentProcessId()) + L".txt";
        DeleteFileW(pids.c_str()), DeleteFileW(state.c_str());
        SetEnvironmentVariableW(L"PM_FAKE_TS_PIDS", pids.c_str());
        SetEnvironmentVariableW(L"PM_FAKE_TS_STATE", state.c_str());
        TailscaleFunnel::setCliForTest(fake);
        auto leftRunning = [&] {  // fake CLI processes still alive
            int n = 0;
            FILE* f = _wfopen(pids.c_str(), L"rb");
            if (!f) return 0;
            unsigned long pid = 0;
            char rest[512];
            while (fscanf_s(f, "%lu", &pid) == 1) {
                fgets(rest, sizeof(rest), f);
                if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
                    if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) ++n;
                    CloseHandle(h);
                }
            }
            fclose(f);
            return n;
        };
        auto startWith = [&](const wchar_t* mode, std::string& url, std::string& err) {
            SetEnvironmentVariableW(L"PM_FAKE_TS_MODE", mode);
            const ULONGLONG t0 = GetTickCount64();
            TailscaleFunnel::start(8443, kOrigin, url, err);
            return double(GetTickCount64() - t0) / 1000.0;
        };
        std::string url, err;
        double s = startWith(L"enable-hang", url, err);
        check("real CLI, enable page then waits for ever: stopped within 5 s", s < 5, std::to_string(s) + " s");
        check("real CLI, enable page: needs-enable: funnel <url>", err == "needs-enable: funnel https://login.tailscale.com/f/funnel?node=nFAKE0832", err);
        check("real CLI, enable page: no process left", leftRunning() == 0, std::to_string(leftRunning()));
        s = startWith(L"https-hang", url, err);
        check("real CLI, HTTPS page: stopped within 5 s, needs-enable: https", s < 5 && err.rfind("needs-enable: https https://login.tailscale.com/f/serve", 0) == 0, err);
        s = startWith(L"silent-hang", url, err);
        check("real CLI, silent and never exits: given up after ~15 s", s >= 14 && s < 20 && err == "timeout: funnel --bg", std::to_string(s) + " s, " + err);
        check("real CLI, silent: no process left", leftRunning() == 0, std::to_string(leftRunning()));
        s = startWith(L"needs-login", url, err);
        check("real CLI, NeedsLogin: at once", s < 5 && err == "not-running: NeedsLogin", err);
        s = startWith(L"port-taken", url, err);
        check("real CLI, 8443 of another program: left alone", s < 5 && err == "port-taken: http://127.0.0.1:9000", err);
        s = startWith(L"ok", url, err);
        check("real CLI, ok: up", s < 5 && url == "https://example-pc.example.ts.net:8443/", url + " " + err);
        std::string e2;
        check("real CLI, ok: off again", TailscaleFunnel::stop(8443, e2), e2);
        {  // quit while a CLI hangs: cancelAll() ends it at once
            SetEnvironmentVariableW(L"PM_FAKE_TS_MODE", L"silent-hang");
            std::string u3, e3;
            const ULONGLONG t0 = GetTickCount64();
            std::thread t([&] { TailscaleFunnel::start(8443, kOrigin, u3, e3); });
            Sleep(1500);
            TailscaleFunnel::cancelAll();
            t.join();
            const double took = double(GetTickCount64() - t0) / 1000.0;
            check("cancelAll (quit): the hanging run ends at once", took < 4, std::to_string(took) + " s, " + e3);
            check("cancelAll: no process left", leftRunning() == 0, std::to_string(leftRunning()));
        }
        TailscaleFunnel::setCliForTest(L"");
        DeleteFileW(pids.c_str()), DeleteFileW(state.c_str());
    }
    // 0.8.3.3: the fixed address's TLS check (probeTls) against local servers only (127.0.0.1): one that
    // closes every connection (as Tailscale does while it issues the certificate), one that answers plain
    // HTTP (no TLS); never a real address. The success case runs in the app test (app0833_test.mjs) against
    // a fake TLS server with a self-signed certificate.
    {
        WSADATA wd;
        WSAStartup(MAKEWORD(2, 2), &wd);
        auto server = [](bool answerHttp, int& port, std::atomic<bool>& stop) {
            SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
            bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a));
            int al = sizeof(a);
            getsockname(ls, reinterpret_cast<sockaddr*>(&a), &al);
            port = ntohs(a.sin_port);
            listen(ls, 8);
            return std::thread([ls, answerHttp, &stop] {
                while (!stop) {
                    fd_set r;
                    FD_ZERO(&r);
                    FD_SET(ls, &r);
                    timeval tv{0, 100000};
                    if (select(0, &r, nullptr, nullptr, &tv) <= 0) continue;
                    SOCKET c = accept(ls, nullptr, nullptr);
                    if (c == INVALID_SOCKET) continue;
                    if (answerHttp) {
                        char b[512];
                        recv(c, b, sizeof(b), 0);
                        const char* resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found";
                        send(c, resp, int(strlen(resp)), 0);
                    }
                    closesocket(c);
                }
                closesocket(ls);
            });
        };
        std::atomic<bool> stop{false};
        int p1 = 0, p2 = 0;
        std::thread t1 = server(false, p1, stop), t2 = server(true, p2, stop);
        std::string d;
        const ULONGLONG t0 = GetTickCount64();
        const bool closed = TailscaleFunnel::probeTls("https://127.0.0.1:" + std::to_string(p1) + "/", d, true, 3000);
        check("probeTls: a server that closes every connection = not ready", !closed && GetTickCount64() - t0 < 4000, d);
        const bool plain = TailscaleFunnel::probeTls("https://127.0.0.1:" + std::to_string(p2) + "/", d, true, 3000);
        check("probeTls: no TLS (plain HTTP) = not ready", !plain, d);
        check("probeTls: an http:// address is never checked", !TailscaleFunnel::probeTls("http://127.0.0.1:" + std::to_string(p2) + "/", d, true), d);
        check("probeTls: the test's insecure mode only for 127.0.0.1", !TailscaleFunnel::probeTls("https://example-pc.example.ts.net:8443/", d, true) &&
                                                                         d == "test probe: 127.0.0.1 only", d);
        stop = true;
        t1.join(), t2.join();
        WSACleanup();
    }
    check("backendStarting: NoState / Starting yes, Stopped / NeedsLogin no",
          TailscaleFunnel::backendStarting("not-running: NoState") && TailscaleFunnel::backendStarting("not-running: Starting") &&
              !TailscaleFunnel::backendStarting("not-running: Stopped") && !TailscaleFunnel::backendStarting("not-running: NeedsLogin") &&
              !TailscaleFunnel::backendStarting("no-tailscale") && !TailscaleFunnel::backendStarting("443-changed"));
    printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
