#pragma once
// cloudflared "quick tunnel" (https://<random>.trycloudflare.com -> this PC):
// no Cloudflare account, no app on the phone. Runs the cloudflared executable
// (Apache-2.0, shipped next to the app or already installed) as a child
// process inside a job object that kills it with us, reads its log for the
// public address. The address changes every time it starts.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace pm::remote {

class Tunnel {
public:
    ~Tunnel() { stop(); }
    // cloudflared.exe: next to `appDir` (<appDir>\cloudflared\cloudflared.exe),
    // else an installed one (Program Files, PATH). "" when there is none.
    static std::wstring findExe(const std::wstring& appDir);
    // Starts `exe tunnel --url http://127.0.0.1:<port>`. onUrl: the public
    // https address (once); onEnd: it failed / stopped by itself (reason).
    bool start(const std::wstring& exe, uint16_t port, std::function<void(const std::string& url)> onUrl,
               std::function<void(const std::string& why)> onEnd, const std::string& host = "127.0.0.1");
    // After the address is known: is it reachable from the internet (through Cloudflare back to
    // this PC)? Called once with the result (a fresh quick tunnel sometimes stalls for minutes).
    std::function<void(bool ok, int ms)> onCheck;
    void stop();
    bool running() const { return running_; }
    std::string url() const {
        std::lock_guard<std::mutex> lk(mu_);
        return url_;
    }
    DWORD pid() const { return pid_; }

private:
    void readLoop();
    static void selfCheck(std::string url, std::shared_ptr<std::atomic<bool>> stopped, std::function<void(bool, int)> onCheck);
    std::shared_ptr<std::atomic<bool>> checkStop_;
    HANDLE job_ = nullptr, proc_ = nullptr, out_ = nullptr;
    DWORD pid_ = 0;
    std::thread reader_;
    std::atomic<bool> running_{false}, stopping_{false};
    mutable std::mutex mu_;
    std::string url_;
    int64_t startUs_ = 0;
    std::function<void(const std::string&)> onUrl_, onEnd_;
};

}  // namespace pm::remote
