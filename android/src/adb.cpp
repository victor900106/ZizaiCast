#include "adb.h"

#include <windows.h>

#include <chrono>
#include <sstream>

namespace pm::adb {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

namespace {

// CreateProcess quoting (CommandLineToArgvW rules).
void appendArg(std::wstring& cmd, const std::wstring& a) {
    if (!cmd.empty()) cmd.push_back(L' ');
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) {
        cmd += a;
        return;
    }
    cmd.push_back(L'"');
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++bs; continue; }
        if (c == L'"') cmd.append(bs * 2 + 1, L'\\');
        else cmd.append(bs, L'\\');
        bs = 0;
        cmd.push_back(c);
    }
    cmd.append(bs * 2, L'\\');
    cmd.push_back(L'"');
}

// Current environment + overrides, as a CREATE_UNICODE_ENVIRONMENT block.
std::wstring environmentBlock(const std::vector<std::pair<std::wstring, std::wstring>>& overrides) {
    std::vector<std::wstring> vars;
    if (wchar_t* env = GetEnvironmentStringsW()) {
        for (const wchar_t* p = env; *p; p += wcslen(p) + 1) {
            std::wstring v(p);
            bool overridden = false;
            for (auto& o : overrides) {
                if (v.size() > o.first.size() && v[o.first.size()] == L'=' &&
                    _wcsnicmp(v.c_str(), o.first.c_str(), o.first.size()) == 0)
                    overridden = true;
            }
            if (!overridden) vars.push_back(std::move(v));
        }
        FreeEnvironmentStringsW(env);
    }
    for (auto& o : overrides)
        if (!o.second.empty()) vars.push_back(o.first + L"=" + o.second);  // empty = remove
    std::wstring block;
    for (auto& v : vars) { block += v; block.push_back(L'\0'); }
    block.push_back(L'\0');
    return block;
}

struct Child {
    HANDLE process = nullptr;
    HANDLE readPipe = nullptr;
};

bool launch(const std::wstring& exe, const std::wstring& cmdLine, const std::wstring& env, HANDLE job, Child& out) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    // Only these handles are inherited (no leaks of other threads' pipes).
    HANDLE inherit[2] = {wr, nul};
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<uint8_t> attrBuf(attrSize);
    auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                              nul != INVALID_HANDLE_VALUE ? sizeof(inherit) : sizeof(HANDLE), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.StartupInfo.hStdInput = nul != INVALID_HANDLE_VALUE ? nul : nullptr;
    si.StartupInfo.hStdOutput = wr;
    si.StartupInfo.hStdError = wr;
    si.lpAttributeList = attrs;

    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdLine;
    std::wstring envCopy = env;
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT |
                                 CREATE_SUSPENDED,
                             envCopy.data(), nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        return false;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    out.process = pi.hProcess;
    out.readPipe = rd;
    return true;
}

// Reads what is available right now (non-blocking).
bool drain(HANDLE pipe, std::string& out) {
    bool any = false;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) return any;
        char buf[4096];
        DWORD got = 0;
        if (!ReadFile(pipe, buf, std::min<DWORD>(avail, sizeof(buf)), &got, nullptr) || got == 0) return any;
        out.append(buf, got);
        any = true;
    }
}

}  // namespace

// ------------------------------------------------------------- Process --

Process::~Process() {
    kill();
    if (reader_.joinable()) reader_.join();
    if (process_) CloseHandle(process_);
}

bool Process::running() const { return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT; }

void Process::kill() {
    if (running()) TerminateProcess(process_, 1);
}

bool Process::wait(int ms) { return !process_ || WaitForSingleObject(process_, DWORD(ms)) == WAIT_OBJECT_0; }

int Process::exitCode() const {
    DWORD c = 0;
    if (!process_ || !GetExitCodeProcess(process_, &c)) return -1;
    return c == STILL_ACTIVE ? -1 : int(c);
}

// ----------------------------------------------------------------- Adb --

Adb::Adb() {
    // Kill-on-close job: children (incl. the forked adb server) die with us.
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }
}

Adb::~Adb() {
    if (job_) CloseHandle(job_);
}

bool Adb::init(const std::wstring& adbExe, uint16_t port) {
    exe_ = adbExe;
    port_ = port;
    return GetFileAttributesW(adbExe.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring Adb::commandLine(const std::vector<std::string>& args) const {
    std::wstring cmd;
    appendArg(cmd, exe_);
    appendArg(cmd, L"-P");
    appendArg(cmd, std::to_wstring(port_));
    for (auto& a : args) appendArg(cmd, widen(a));
    return cmd;
}

static std::wstring adbEnv(uint16_t port) {
    return environmentBlock({
        {L"ANDROID_ADB_SERVER_PORT", std::to_wstring(port)},
        {L"ADB_MDNS_OPENSCREEN", L"1"},  // built-in mDNS (no Bonjour needed)
        {L"ADB_MDNS_AUTO_CONNECT", L"0"},  // we connect explicitly (one transport per phone)
        {L"ADB_SERVER_SOCKET", L""},  // removed: -P decides
    });
}

Result Adb::run(const std::vector<std::string>& args, int timeoutMs, const std::atomic<bool>* cancel) const {
    Result r;
    Child c;
    if (!launch(exe_, commandLine(args), adbEnv(port_), job_, c)) {
        r.output = "cannot start adb.exe (error " + std::to_string(GetLastError()) + ")";
        return r;
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        // Do not wait for pipe EOF: a forked adb server may inherit nothing,
        // but we never rely on it -- process exit + final drain is the end.
        drain(c.readPipe, r.output);
        if (WaitForSingleObject(c.process, 15) == WAIT_OBJECT_0) {
            drain(c.readPipe, r.output);
            DWORD code = 0;
            GetExitCodeProcess(c.process, &code);
            r.exitCode = int(code);
            break;
        }
        if (std::chrono::steady_clock::now() > deadline || (cancel && cancel->load())) {
            TerminateProcess(c.process, 1);
            WaitForSingleObject(c.process, 1000);
            drain(c.readPipe, r.output);
            r.exitCode = -2;
            break;
        }
    }
    CloseHandle(c.readPipe);
    CloseHandle(c.process);
    return r;
}

std::unique_ptr<Process> Adb::spawn(const std::vector<std::string>& args,
                                    std::function<void(const std::string&)> onLine) const {
    Child c;
    if (!launch(exe_, commandLine(args), adbEnv(port_), job_, c)) return nullptr;
    auto p = std::make_unique<Process>();
    p->process_ = c.process;
    HANDLE proc = nullptr;
    DuplicateHandle(GetCurrentProcess(), c.process, GetCurrentProcess(), &proc, SYNCHRONIZE, FALSE, 0);
    p->reader_ = std::thread([pipe = c.readPipe, proc, onLine = std::move(onLine)] {
        std::string pending;
        auto flushLines = [&](bool all) {
            size_t pos;
            while ((pos = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (onLine && !line.empty()) onLine(line);
                pending.erase(0, pos + 1);
            }
            if (all && !pending.empty()) {
                if (onLine) onLine(pending);
                pending.clear();
            }
        };
        for (;;) {
            bool any = drain(pipe, pending);
            if (any) flushLines(false);
            if (WaitForSingleObject(proc, any ? 0 : 20) == WAIT_OBJECT_0) {
                drain(pipe, pending);
                flushLines(true);
                break;
            }
        }
        CloseHandle(pipe);
        CloseHandle(proc);
    });
    return p;
}

bool Adb::startServer() const {
    Result r = run({"start-server"}, 20000);
    if (!r.ok() && log) log("adb start-server failed (" + std::to_string(r.exitCode) + "): " + r.output);
    return r.ok();
}

void Adb::killServer() const { run({"kill-server"}, 5000); }

// ------------------------------------------------------------- parsers --

static std::vector<std::string> splitWs(const std::string& line) {
    std::vector<std::string> t;
    std::istringstream is(line);
    std::string w;
    while (is >> w) t.push_back(w);
    return t;
}

bool splitHostPort(const std::string& hp, std::string& host, uint16_t& port) {
    size_t c = hp.rfind(':');
    if (c == std::string::npos || c == 0 || c + 1 >= hp.size()) return false;
    host = hp.substr(0, c);
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    unsigned long v = 0;
    for (size_t i = c + 1; i < hp.size(); ++i) {
        if (hp[i] < '0' || hp[i] > '9') return false;
        v = v * 10 + unsigned(hp[i] - '0');
        if (v > 65535) return false;
    }
    if (v == 0) return false;
    port = uint16_t(v);
    return true;
}

std::vector<MdnsService> parseMdnsServices(const std::string& out) {
    std::vector<MdnsService> v;
    std::istringstream is(out);
    std::string line;
    while (std::getline(is, line)) {
        auto t = splitWs(line);
        if (t.size() < 3) continue;
        MdnsService s;
        s.instance = t[0];
        int typeIdx = -1;
        for (size_t i = 1; i < t.size(); ++i)
            if (t[i].rfind("_adb", 0) == 0) { typeIdx = int(i); break; }
        if (typeIdx < 0) continue;
        s.type = t[size_t(typeIdx)];
        while (!s.type.empty() && s.type.back() == '.') s.type.pop_back();
        // Instance names may contain spaces: everything before the type.
        s.instance.clear();
        for (int i = 0; i < typeIdx; ++i) s.instance += (i ? " " : "") + t[size_t(i)];
        bool found = false;
        for (size_t i = size_t(typeIdx) + 1; i < t.size() && !found; ++i)
            found = splitHostPort(t[i], s.host, s.port);
        if (found) v.push_back(std::move(s));
    }
    return v;
}

std::vector<Device> parseDevices(const std::string& out) {
    std::vector<Device> v;
    std::istringstream is(out);
    std::string line;
    while (std::getline(is, line)) {
        if (line.rfind("List of devices", 0) == 0 || line.rfind("*", 0) == 0) continue;
        auto t = splitWs(line);
        if (t.size() < 2) continue;
        Device d;
        d.serial = t[0];
        d.state = t[1];
        for (size_t i = 2; i < t.size(); ++i) {
            auto kv = t[i];
            auto c = kv.find(':');
            if (c == std::string::npos) continue;
            auto k = kv.substr(0, c), val = kv.substr(c + 1);
            if (k == "model") d.model = val;
            else if (k == "product") d.product = val;
            else if (k == "device") d.device = val;
        }
        v.push_back(std::move(d));
    }
    return v;
}

bool parsePairResult(const std::string& out, std::string* guid) {
    if (out.find("Successfully paired") == std::string::npos) return false;
    if (guid) {
        guid->clear();
        auto p = out.find("[guid=");
        if (p != std::string::npos) {
            auto e = out.find(']', p);
            if (e != std::string::npos) *guid = out.substr(p + 6, e - p - 6);
        }
    }
    return true;
}

bool parseConnectResult(const std::string& out) {
    if (out.find("failed") != std::string::npos || out.find("cannot") != std::string::npos ||
        out.find("unable") != std::string::npos)
        return false;
    return out.find("connected to") != std::string::npos;  // "connected to" / "already connected to"
}

}  // namespace pm::adb
