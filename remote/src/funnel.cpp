// Tailscale Funnel for the fixed link (常駐外網, 0.8.2): the tailscale CLI,
// on port 8443 only. Every change is checked against a snapshot of port 443
// (another program may serve its own Funnel there: it must never change).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "pm/remote.h"
#include "util.h"

namespace pm::remote {

namespace {

using json = nlohmann::json;

// 0.8.3.1 tests: the stubbed tailscale CLI (setRunnerForTest); empty = real processes.
TailscaleFunnel::Runner& testRunner() {
    static TailscaleFunnel::Runner r;
    return r;
}

// 0.8.3.2: a CLI run of a real program (also the tests' fake one): in a job object (its whole
// process tree goes with it), at most timeoutMs, and stopped at once when stopWhen() says so (the
// CLI printed "visit https://login.tailscale.com/f/... to enable" and would wait for ever).
// cancelAll() (quit) ends every run that is still going. Never on the UI thread (the app's threads).
std::mutex& jobsMu() {
    static std::mutex m;
    return m;
}
std::set<HANDLE>& jobs() {
    static std::set<HANDLE> s;
    return s;
}
std::wstring& cliForTest() {
    static std::wstring p;
    return p;
}

// Runs a command line without a window, its output (stdout + stderr) and exit code.
bool runCapture(const std::wstring& cmd, DWORD timeoutMs, std::string& out, DWORD& code,
                const std::function<bool(const std::string&)>& stopWhen = {}, bool* stopped = nullptr) {
    out.clear(), code = DWORD(-1);
    if (stopped) *stopped = false;
    if (const auto& r = testRunner()) {  // tests: never a real process
        unsigned long c = DWORD(-1);
        const bool ok = r(cmd, out, c);
        code = DWORD(c);
        if (stopWhen && stopWhen(out) && stopped) *stopped = true;
        return ok;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr, si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cl(cmd.begin(), cmd.end());
    cl.push_back(0);
    const BOOL ok = CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        if (job) CloseHandle(job);
        return false;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    if (job) {
        std::lock_guard<std::mutex> lk(jobsMu());
        jobs().insert(job);
    }
    char b[4096];
    DWORD n = 0;
    const ULONGLONG t0 = GetTickCount64();
    auto kill = [&](const char* why) {
        if (job) TerminateJobObject(job, 1);
        else TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
        out += why;
    };
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
            if (!ReadFile(rd, b, sizeof(b), &n, nullptr) || !n) break;
            out.append(b, n);
            if (out.size() > 65536) out.erase(0, out.size() - 65536);
            if (stopWhen && stopWhen(out)) {
                if (stopped) *stopped = true;
                kill("\n(stopped: it waits for something to be done on a web page)");
                break;
            }
        } else if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail && ReadFile(rd, b, sizeof(b), &n, nullptr) && n) out.append(b, n);
            break;
        }
        if (GetTickCount64() - t0 > timeoutMs) {
            kill("\n(timed out)");
            break;
        }
        if (job) {  // cancelAll() (quit) closed it from another thread
            std::lock_guard<std::mutex> lk(jobsMu());
            if (!jobs().count(job)) {
                out += "\n(cancelled)";
                job = nullptr;
                WaitForSingleObject(pi.hProcess, 2000);
                break;
            }
        }
    }
    GetExitCodeProcess(pi.hProcess, &code);
    if (job) {
        std::lock_guard<std::mutex> lk(jobsMu());
        jobs().erase(job);
        CloseHandle(job);  // KILL_ON_JOB_CLOSE: nothing of it is left running
    }
    CloseHandle(pi.hProcess), CloseHandle(pi.hThread), CloseHandle(rd);
    return true;
}

// What the CLI's output means, when it is something the person has to do (0.8.3.2). "" = nothing known.
//   needs-enable: funnel <url>   Funnel is not on for this tailnet yet (a page to click "enable" on)
//   needs-enable: https <url>    HTTPS certificates / Serve are not on yet
//   no-funnel-attr: <url>        the tailnet's access rules give this node no "funnel" attribute
//   cli-too-old                  the CLI has no "funnel --bg"
//   needs-login                  Tailscale is signed out
std::string classify(const std::string& outIn) {
    std::string o = outIn;
    for (char& c : o) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    auto urlAfter = [&](const std::string& key) {
        const size_t p = outIn.find(key);
        if (p == std::string::npos) return std::string();
        size_t e = p;
        while (e < outIn.size() && !isspace(static_cast<unsigned char>(outIn[e])) && outIn[e] != '"' && outIn[e] != '\'') ++e;
        std::string u = outIn.substr(p, std::min<size_t>(e - p, 300));
        while (!u.empty() && (u.back() == '.' || u.back() == ',' || u.back() == ')')) u.pop_back();
        return u;
    };
    const std::string enable = urlAfter("https://login.tailscale.com/f/");
    if (!enable.empty() || o.find("is not enabled on your tailnet") != std::string::npos) {
        const bool https = enable.find("/f/serve") != std::string::npos || enable.find("/f/https") != std::string::npos ||
                           (o.find("serve is not enabled") != std::string::npos) || (o.find("https") != std::string::npos && o.find("certificate") != std::string::npos);
        return std::string("needs-enable: ") + (https ? "https " : "funnel ") + enable;
    }
    if (o.find("node attribute") != std::string::npos || o.find("funnel not available") != std::string::npos)
        return "no-funnel-attr: " + urlAfter("https://");
    if ((o.find("flag provided but not defined") != std::string::npos || o.find("unknown flag") != std::string::npos) && o.find("bg") != std::string::npos)
        return "cli-too-old";
    if (o.find("needslogin") != std::string::npos || o.find("logged out") != std::string::npos || o.find("not logged in") != std::string::npos ||
        o.find("log in") != std::string::npos)
        return "needs-login";
    return {};
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

}  // namespace

bool TailscaleFunnel::targetAnswers(const std::string& target) {
    const std::string pre = "http://127.0.0.2:";
    if (target.rfind(pre, 0) != 0) return false;
    const int port = std::atoi(target.c_str() + pre.size());
    if (port <= 0 || port > 65535) return false;
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return true;  // unsure: treat as in use (never remove)
    bool answers = true;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s != INVALID_SOCKET) {
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, "127.0.0.2", &a.sin_addr);
        connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        fd_set w, e;
        FD_ZERO(&w);
        FD_ZERO(&e);
        FD_SET(s, &w);
        FD_SET(s, &e);
        timeval tv{1, 0};
        const int n = select(0, nullptr, &w, &e, &tv);
        answers = n > 0 && FD_ISSET(s, &w) && !FD_ISSET(s, &e);
        if (n == 0) answers = true;  // no answer within 1 s either way: leave it alone
        closesocket(s);
    }
    WSACleanup();
    return answers;
}

void TailscaleFunnel::setRunnerForTest(Runner r) { testRunner() = std::move(r); }
void TailscaleFunnel::setCliForTest(const std::wstring& path) { cliForTest() = path; }

void TailscaleFunnel::cancelAll() {
    std::lock_guard<std::mutex> lk(jobsMu());
    for (HANDLE j : jobs()) TerminateJobObject(j, 1), CloseHandle(j);
    if (!jobs().empty()) logf("funnel: %zu tailscale CLI run(s) ended (cancelled)", jobs().size());
    jobs().clear();
}

std::wstring TailscaleFunnel::findCli() {
    if (testRunner()) return L"tailscale-test-stub.exe";  // (never started: runCapture uses the stub)
    if (!cliForTest().empty()) return cliForTest();       // 0.8.3.2 tests: a fake CLI (never tailscale)
    wchar_t b[MAX_PATH] = {};
    for (const wchar_t* v : {L"ProgramFiles", L"ProgramW6432", L"ProgramFiles(x86)"}) {
        const DWORD n = GetEnvironmentVariableW(v, b, MAX_PATH);
        if (!n || n >= MAX_PATH) continue;
        const std::wstring p = std::wstring(b, n) + L"\\Tailscale\\tailscale.exe";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    wchar_t found[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"tailscale.exe", nullptr, MAX_PATH, found, nullptr)) return found;
    return {};
}

FunnelStatus TailscaleFunnel::status(int port) {
    FunnelStatus st;
    const std::wstring cli = findCli();
    if (cli.empty()) return st.error = "no-tailscale", st;
    st.cli = true;
    std::string out;
    DWORD code = 0;
    if (!runCapture(L"\"" + cli + L"\" status --json", 15000, out, code)) return st.error = "not-running: the CLI did not start", st;
    if (out.find("(timed out)") != std::string::npos) return st.error = "timeout: status", st;  // 0.8.3.2: not "still starting"
    if (code != 0) return st.error = "not-running: " + trimmed(out).substr(0, 200), st;
    json j = json::parse(out, nullptr, false);
    if (j.is_discarded()) return st.error = "status: not JSON", st;
    st.backend = j.value("BackendState", std::string());
    if (j.contains("Self") && j["Self"].is_object()) {
        st.dnsName = j["Self"].value("DNSName", std::string());
        while (!st.dnsName.empty() && st.dnsName.back() == '.') st.dnsName.pop_back();
        if (j["Self"].contains("CapMap") && j["Self"]["CapMap"].is_object())
            for (const auto& [k, v] : j["Self"]["CapMap"].items())
                if (k.find("funnel") != std::string::npos) st.funnelCap = true;
    }
    if (st.backend != "Running") return st.error = "not-running: " + st.backend, st;
    st.running = true;
    if (!runCapture(L"\"" + cli + L"\" funnel status --json", 15000, out, code) || code != 0) {
        const std::string k = classify(out);  // 0.8.3.2: e.g. HTTPS not on yet
        return st.error = !k.empty() ? k : out.find("(timed out)") != std::string::npos ? "timeout: funnel status" : "funnel status: " + trimmed(out).substr(0, 200), st;
    }
    json f = json::parse(out.empty() ? std::string("{}") : out, nullptr, false);
    if (f.is_discarded()) f = json::object();
    const std::string hp443 = st.dnsName + ":443", hp = st.dnsName + ":" + std::to_string(port);
    json snap{{"tcp", f["TCP"].is_object() && f["TCP"].contains("443") ? f["TCP"]["443"] : json()},
              {"web", f["Web"].is_object() && f["Web"].contains(hp443) ? f["Web"][hp443] : json()},
              {"funnel", f["AllowFunnel"].is_object() && f["AllowFunnel"].contains(hp443) ? f["AllowFunnel"][hp443] : json()}};
    st.port443 = snap.dump();
    if (f["Web"].is_object() && f["Web"].contains(hp)) {
        st.hasPort = true;
        const auto& h = f["Web"][hp];
        if (h.contains("Handlers") && h["Handlers"].contains("/")) st.target = h["Handlers"]["/"].value("Proxy", std::string());
        st.portFunnel = f["AllowFunnel"].is_object() && f["AllowFunnel"].value(hp, false);
    }
    return st;
}

bool TailscaleFunnel::start(int port, const std::string& target, std::string& url, std::string& err) {
    const FunnelStatus before = status(port);
    if (!before.running) return err = before.error, false;
    // 0.8.3.1: tailscaled keeps its serve config across restarts: 8443 already pointing at our
    // origin with Funnel on is left exactly as it is (no command, nothing changes).
    if (before.hasPort && before.portFunnel && before.target == target && !before.dnsName.empty()) {
        url = "https://" + before.dnsName + ":" + std::to_string(port) + "/";
        logf("funnel: on: %s -> %s (kept by tailscaled, nothing changed; port 443 untouched)", url.c_str(), target.c_str());
        return true;
    }
    // 0.8.3.2: 8443 already serving something else (another program): never taken over. Ours from an
    // earlier run (127.0.0.2, another port) may be replaced.
    if (before.hasPort && before.target != target && before.target.rfind("http://127.0.0.2:", 0) != 0) {
        err = "port-taken: " + before.target.substr(0, 120);
        logf("funnel: port %d is in use by something else (%s): left alone", port, before.target.c_str());
        return false;
    }
    const std::wstring cmd = L"\"" + findCli() + L"\" funnel --bg --https=" + std::to_wstring(port) + L" " + widen(target);
    std::string out;
    DWORD code = 0;
    bool stoppedEarly = false;
    logf("funnel: tailscale funnel --bg --https=%d %s (port 443 before: %s)", port, target.c_str(), before.port443.c_str());
    // 0.8.3.2: at most 15 s; a CLI that printed an enable page ("visit https://login.tailscale.com/f/...")
    // waits for that page for ever: stopped at once, the page is what the person needs.
    const bool ran = runCapture(cmd, 15000, out, code, [](const std::string& o) { return o.find("https://login.tailscale.com/f/") != std::string::npos; }, &stoppedEarly);
    if (!ran || code != 0 || stoppedEarly) {
        const std::string k = classify(out);
        err = !k.empty() ? k : !ran ? "funnel: the CLI did not start" : out.find("(timed out)") != std::string::npos ? "timeout: funnel --bg" : "funnel: " + trimmed(out).substr(0, 300);
        logf("funnel: failed (exit %lu%s): %s -> %s", code, stoppedEarly ? ", stopped: it waited for a web page" : "", trimmed(out).substr(0, 300).c_str(), err.c_str());
        return false;
    }
    const FunnelStatus after = status(port);
    if (after.port443 != before.port443) {
        logf("funnel: WARNING port 443 changed! before %s after %s", before.port443.c_str(), after.port443.c_str());
        err = "443-changed";
        return false;
    }
    if (!after.hasPort || !after.portFunnel || after.target != target) {
        err = "funnel: port " + std::to_string(port) + " not in the Funnel config after the command";
        logf("funnel: %s (%s)", err.c_str(), trimmed(out).substr(0, 200).c_str());
        return false;
    }
    url = "https://" + after.dnsName + ":" + std::to_string(port) + "/";
    logf("funnel: on: %s -> %s (port 443 unchanged)", url.c_str(), target.c_str());
    return true;
}

bool TailscaleFunnel::backendStarting(const std::string& err) {
    std::string e = err;
    for (char& c : e) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (e.rfind("not-running: ", 0) != 0) return false;
    const std::string st = trimmed(e.substr(13));
    if (st == "nostate" || st == "starting" || st.empty()) return true;
    // The service is not listening yet (tailscaled still starting after a reboot).
    return st.find("failed to connect to local tailscale") != std::string::npos || st.find("is tailscale running") != std::string::npos ||
           st.find("tailscaled") != std::string::npos;
}

bool TailscaleFunnel::startPatiently(int port, const std::string& target, std::string& url, std::string& err,
                                     const std::function<bool(int, int)>& wait, int maxTotalSeconds) {
    int waited = 0;
    for (int attempt = 1;; ++attempt) {
        err.clear();
        if (start(port, target, url, err)) {
            if (attempt > 1) logf("funnel: up on try %d, after %d s of waiting for Tailscale to start", attempt, waited);
            return true;
        }
        if (!backendStarting(err)) return false;  // signed out, no Funnel, 443 changed, ...: not a wait
        int delay = (std::min)(60, 2 << (std::min)(attempt - 1, 5));  // 2 4 8 16 32 60 60 ... (the last one up to the limit)
        if (waited + delay > maxTotalSeconds) delay = maxTotalSeconds - waited;  // (2 4 8 16 32 60 60 60 58 = 300 s)
        if (delay <= 0) {
            logf("funnel: Tailscale still not running after %d s (%s): giving up", waited, err.c_str());
            err = "still-starting: " + err.substr((std::min<size_t>)(err.size(), 13));
            return false;
        }
        logf("funnel: Tailscale is still starting (%s): try %d in %d s", err.c_str(), attempt + 1, delay);
        if (!wait(attempt, delay)) {
            err = "cancelled";
            return false;
        }
        waited += delay;
    }
}

bool TailscaleFunnel::stop(int port, std::string& err) {
    const FunnelStatus before = status(port);
    if (!before.running) return err = before.error, false;
    if (!before.hasPort) return true;
    std::string out;
    DWORD code = 0;
    const std::wstring cmd = L"\"" + findCli() + L"\" funnel --https=" + std::to_wstring(port) + L" off";
    if (!runCapture(cmd, 15000, out, code) || code != 0) {
        err = "funnel off: " + trimmed(out).substr(0, 300);
        logf("funnel: off failed (exit %lu): %s", code, trimmed(out).substr(0, 300).c_str());
        return false;
    }
    const FunnelStatus after = status(port);
    if (after.port443 != before.port443) logf("funnel: WARNING port 443 changed while turning %d off!", port);
    logf("funnel: port %d off%s (port 443 %s)", port, after.hasPort ? " FAILED (still there)" : "", after.port443 == before.port443 ? "unchanged" : "CHANGED");
    return !after.hasPort;
}

}  // namespace pm::remote
