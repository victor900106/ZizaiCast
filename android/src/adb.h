// Runs Google's adb.exe as hidden child processes against a private adb
// server port (default 15037, not 5037) so we never fight Android Studio /
// another scrcpy over the server. Every child (and the adb server daemon it
// forks) is put in a kill-on-close job object: if our process dies, they die.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace pm::adb {

struct Result {
    int exitCode = -1;      // -1 = could not start, -2 = timed out (killed)
    std::string output;     // stdout + stderr (UTF-8 as printed by adb)
    bool ok() const { return exitCode == 0; }
};

// A long-running child (the scrcpy server shell). Output lines go to onLine.
class Process {
public:
    ~Process();
    bool running() const;
    void kill();
    // Waits up to ms for exit; returns true if it exited.
    bool wait(int ms);
    int exitCode() const;

private:
    friend class Adb;
    void* process_ = nullptr;
    std::thread reader_;
};

class Adb {
public:
    Adb();
    ~Adb();
    // adbExe: full path of adb.exe. port: private adb server port.
    bool init(const std::wstring& adbExe, uint16_t port = 15037);
    const std::wstring& exe() const { return exe_; }
    uint16_t port() const { return port_; }

    // Runs `adb -P <port> args...` and waits. cancel (optional) aborts early.
    Result run(const std::vector<std::string>& args, int timeoutMs = 15000,
               const std::atomic<bool>* cancel = nullptr) const;
    // Spawns a long-running `adb -P <port> args...`; output lines → onLine.
    std::unique_ptr<Process> spawn(const std::vector<std::string>& args,
                                   std::function<void(const std::string&)> onLine) const;

    bool startServer() const;  // idempotent
    void killServer() const;

    std::function<void(const std::string&)> log;

private:
    std::wstring commandLine(const std::vector<std::string>& args) const;
    std::wstring exe_;
    uint16_t port_ = 15037;
    void* job_ = nullptr;
};

// ---- output parsers (pure, unit-tested) ----

struct MdnsService {
    std::string instance;  // e.g. adb-R5CT123-AbCdEf  /  studio-XXXX
    std::string type;      // _adb-tls-pairing._tcp / _adb-tls-connect._tcp / _adb._tcp
    std::string host;      // IPv4
    uint16_t port = 0;
    std::string hostPort() const { return host + ":" + std::to_string(port); }
};
// `adb mdns services` → services (header / blank lines skipped).
std::vector<MdnsService> parseMdnsServices(const std::string& out);

struct Device {
    std::string serial, state, model, product, device;
};
// `adb devices -l`.
std::vector<Device> parseDevices(const std::string& out);

// `adb pair` → true on "Successfully paired"; guid = text inside [guid=...].
bool parsePairResult(const std::string& out, std::string* guid);
// `adb connect` (exit code is 0 even on failure) → true on connected/already.
bool parseConnectResult(const std::string& out);

// "a:b" host/port split; false if malformed.
bool splitHostPort(const std::string& hp, std::string& host, uint16_t& port);

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& w);

}  // namespace pm::adb
