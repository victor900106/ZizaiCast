#include "tunnel.h"

#include <winhttp.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "util.h"

namespace pm::remote {

namespace {

bool fileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring env(const wchar_t* name) {
    wchar_t b[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(name, b, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(b, n) : std::wstring();
}

// The https://<words>.trycloudflare.com address in a line of cloudflared's log.
std::string findUrl(const std::string& s) {
    const size_t e = s.find(".trycloudflare.com");
    if (e == std::string::npos) return {};
    const size_t b = s.rfind("https://", e);
    if (b == std::string::npos || e - b > 100) return {};
    for (size_t i = b + 8; i < e; ++i)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-')) return {};
    return s.substr(b, e + 18 - b);
}

}  // namespace

std::wstring Tunnel::findExe(const std::wstring& appDir) {
    std::vector<std::wstring> c;
    if (!appDir.empty()) c.push_back(appDir + L"\\cloudflared\\cloudflared.exe"), c.push_back(appDir + L"\\cloudflared.exe");
    for (const wchar_t* v : {L"ProgramFiles(x86)", L"ProgramFiles", L"ProgramW6432"}) {
        const std::wstring pf = env(v);
        if (!pf.empty()) c.push_back(pf + L"\\cloudflared\\cloudflared.exe");
    }
    for (const auto& p : c)
        if (fileExists(p)) return p;
    wchar_t found[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"cloudflared.exe", nullptr, MAX_PATH, found, nullptr)) return found;
    return {};
}

bool Tunnel::start(const std::wstring& exe, uint16_t port, std::function<void(const std::string&)> onUrl,
                   std::function<void(const std::string&)> onEnd, const std::string& host) {
    stop();
    if (exe.empty() || !fileExists(exe)) return logf("tunnel: cloudflared not found"), false;
    onUrl_ = std::move(onUrl), onEnd_ = std::move(onEnd);
    {
        std::lock_guard<std::mutex> lk(mu_);
        url_.clear();
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return logf("tunnel: CreatePipe failed"), false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    // Dies with us (crash included): a public address must never outlive the app.
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job_) SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof(li));
    std::wstring cmd = L"\"" + exe + L"\" tunnel --no-autoupdate --url http://" + widen(host) + L":" + std::to_wstring(port);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr, si.hStdError = wr, si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cl(cmd.begin(), cmd.end());
    cl.push_back(0);
    if (!CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        logf("tunnel: cloudflared could not start (%lu)", GetLastError());
        CloseHandle(rd), CloseHandle(wr);
        if (job_) CloseHandle(job_), job_ = nullptr;
        return false;
    }
    if (job_) AssignProcessToJobObject(job_, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(wr);
    proc_ = pi.hProcess, out_ = rd, pid_ = pi.dwProcessId;
    startUs_ = nowUs();
    stopping_ = false;
    running_ = true;
    logf("tunnel: cloudflared started (pid %lu): %s", pi.dwProcessId, narrow(cmd).c_str());
    reader_ = std::thread([this] { readLoop(); });
    return true;
}

void Tunnel::readLoop() {
    std::string buf, tail;
    char b[4096];
    DWORD n = 0;
    while (ReadFile(out_, b, sizeof(b), &n, nullptr) && n > 0) {
        buf.append(b, n);
        for (size_t nl; (nl = buf.find('\n')) != std::string::npos;) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty()) continue;
            tail = line;
            bool haveUrl;
            {
                std::lock_guard<std::mutex> lk(mu_);
                haveUrl = !url_.empty();
            }
            if (!haveUrl) {
                const std::string u = findUrl(line);
                if (!u.empty()) {
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        url_ = u;
                    }
                    logf("tunnel: public address %s (%.1f s after start)", u.c_str(), (nowUs() - startUs_) / 1e6);
                    if (onUrl_) onUrl_(u);
                    {
                        // Detached: a slow check never holds up stop() / quitting.
                        auto flag = checkStop_ = std::make_shared<std::atomic<bool>>(false);
                        std::thread([u, flag, cb = onCheck] { selfCheck(u, flag, cb); }).detach();
                    }
                    continue;
                }
            }
            // cloudflared's own warnings / errors (and the connection lines) go to our log.
            if (line.find(" ERR ") != std::string::npos || line.find(" WRN ") != std::string::npos ||
                line.find("Registered tunnel connection") != std::string::npos || line.find("protocol") != std::string::npos)
                logf("tunnel: cloudflared: %s", line.substr(0, 300).c_str());
        }
    }
    DWORD code = 0;
    if (proc_) WaitForSingleObject(proc_, 2000), GetExitCodeProcess(proc_, &code);
    running_ = false;
    if (!stopping_) {
        const std::string why = "cloudflared exited (" + std::to_string(code) + ")" + (tail.empty() ? "" : ": " + tail.substr(0, 200));
        logf("tunnel: %s", why.c_str());
        if (onEnd_) onEnd_(why);
    }
}

// GET <url>/ through the internet: our own server answers 404 "not found" when the
// tunnel works end to end. Up to 8 tries, 3 s apart.
void Tunnel::selfCheck(std::string url, std::shared_ptr<std::atomic<bool>> stopped, std::function<void(bool, int)> onCheck) {
    const int64_t t0 = nowUs();
    const std::wstring host = widen(url.substr(8));
    bool ok = false;
    // The new name needs some seconds to exist in DNS. Asking the normal resolver too
    // early gets a "no such name" that is then cached for minutes (Windows here, the
    // carrier's resolver on a phone that scans too soon): so first wait until it
    // resolves on Cloudflare's DNS-over-HTTPS (no cache on the way), up to 60 s.
    {
        const std::wstring path = L"/dns-query?name=" + host + L"&type=A";
        bool named = false;
        for (int i = 0; i < 40 && !*stopped && !named; ++i) {
            if (i) for (int k = 0; k < 15 && !*stopped; ++k) Sleep(100);
            HINTERNET s = WinHttpOpen(L"ZizaiCast-tunnel-check", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
            if (!s) break;
            WinHttpSetTimeouts(s, 4000, 4000, 4000, 4000);
            if (HINTERNET c = WinHttpConnect(s, L"cloudflare-dns.com", INTERNET_DEFAULT_HTTPS_PORT, 0)) {
                if (HINTERNET r = WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) {
                    if (WinHttpSendRequest(r, L"accept: application/dns-json\r\n", DWORD(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                        WinHttpReceiveResponse(r, nullptr)) {
                        char b[2048] = {};
                        DWORD n = 0, total = 0;
                        while (total < sizeof(b) - 1 && WinHttpReadData(r, b + total, sizeof(b) - 1 - total, &n) && n) total += n;
                        named = std::string(b, total).find("\"Answer\"") != std::string::npos;
                    }
                    WinHttpCloseHandle(r);
                }
                WinHttpCloseHandle(c);
            }
            WinHttpCloseHandle(s);
        }
        if (*stopped) return;
        logf("tunnel: the name %s in public DNS (%.1f s)", named ? "is" : "is still NOT", (nowUs() - t0) / 1e6);
    }
    for (int i = 0; i < 8 && !*stopped && !ok; ++i) {
        if (i) for (int k = 0; k < 30 && !*stopped; ++k) Sleep(100);
        HINTERNET s = WinHttpOpen(L"ZizaiCast-tunnel-check", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
        if (!s) break;
        WinHttpSetTimeouts(s, 5000, 5000, 5000, 5000);
        if (HINTERNET c = WinHttpConnect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0)) {
            if (HINTERNET r = WinHttpOpenRequest(c, L"GET", L"/", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) {
                if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
                    DWORD code = 0, sz = sizeof(code);
                    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &sz, nullptr);
                    char b[64] = {};
                    DWORD n = 0;
                    WinHttpReadData(r, b, sizeof(b) - 1, &n);
                    ok = code == 404 && std::string(b, n).rfind("not found", 0) == 0;  // our own answer, not Cloudflare's error page
                    if (!ok && !*stopped) logf("tunnel: self-check %d: HTTP %lu from Cloudflare (not yet through to this PC)", i + 1, code);
                } else if (!*stopped) {
                    logf("tunnel: self-check %d: no answer (WinHTTP error %lu)", i + 1, GetLastError());
                }
                WinHttpCloseHandle(r);
            }
            WinHttpCloseHandle(c);
        }
        WinHttpCloseHandle(s);
    }
    if (*stopped) return;
    const int ms = int((nowUs() - t0) / 1000);
    logf("tunnel: self-check: %s (%d ms)", ok ? "reachable from the internet" : "NOT reachable from the internet", ms);
    if (onCheck) onCheck(ok, ms);
}

void Tunnel::stop() {
    stopping_ = true;
    if (checkStop_) *checkStop_ = true;
    if (job_) TerminateJobObject(job_, 0);  // cloudflared and anything it started
    else if (proc_) TerminateProcess(proc_, 0);
    if (reader_.joinable()) reader_.join();
    if (out_) CloseHandle(out_), out_ = nullptr;
    if (proc_) CloseHandle(proc_), proc_ = nullptr;
    if (job_) CloseHandle(job_), job_ = nullptr;
    if (pid_) logf("tunnel: cloudflared stopped (pid %lu)", pid_);
    pid_ = 0;
    running_ = false;
    std::lock_guard<std::mutex> lk(mu_);
    url_.clear();
}

}  // namespace pm::remote

// ---- public wrapper (pm/remote.h) ----
#include "pm/remote.h"

namespace pm::remote {

struct QuickTunnel::Impl {
    Tunnel t;
};
QuickTunnel::QuickTunnel() : d_(std::make_unique<Impl>()) {}
QuickTunnel::~QuickTunnel() = default;
std::wstring QuickTunnel::findExe(const std::wstring& appDir) { return Tunnel::findExe(appDir); }
bool QuickTunnel::start(const std::wstring& exe, uint16_t port, std::function<void(const std::string&)> onUrl,
                        std::function<void(const std::string&)> onEnd, const std::string& host) {
    d_->t.onCheck = onCheck;
    return d_->t.start(exe, port, std::move(onUrl), std::move(onEnd), host);
}
void QuickTunnel::stop() { d_->t.stop(); }
bool QuickTunnel::running() const { return d_->t.running(); }
std::string QuickTunnel::url() const { return d_->t.url(); }

}  // namespace pm::remote
