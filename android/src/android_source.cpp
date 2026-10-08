#include "pm/android_source.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

#include "adb.h"
#include "mdns.h"
#include "pairing_qr.h"
#include "scrcpy_proto.h"
#include "scrcpy_session.h"

namespace pm {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {
constexpr const char* kPairingType = "_adb-tls-pairing._tcp";
constexpr const char* kConnectType = "_adb-tls-connect._tcp";
constexpr const char* kDeviceJar = "/data/local/tmp/zizai-scrcpy-server.jar";

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    return s.substr(i);
}

const char* stateName(AndroidSource::State s) {
    switch (s) {
        case AndroidSource::State::Idle: return "Idle";
        case AndroidSource::State::WaitingForPairing: return "WaitingForPairing";
        case AndroidSource::State::Pairing: return "Pairing";
        case AndroidSource::State::Connecting: return "Connecting";
        case AndroidSource::State::Mirroring: return "Mirroring";
        case AndroidSource::State::Error: return "Error";
    }
    return "?";
}
}  // namespace

struct AndroidSource::Impl {
    AndroidSource* self = nullptr;
    adb::Adb adb;
    std::wstring toolsDir, serverPath;
    bool inited = false;
    bool serverStarted = false;

    // Worker: all slow operations run here, one at a time.
    std::thread worker;
    std::mutex qmu;
    std::condition_variable qcv;
    std::deque<std::function<void()>> tasks;
    bool quit = false;
    std::atomic<bool> busy{false};  // a pairing / connect task is queued or running

    std::atomic<bool> cancelPair{false};

    // 傳到手機 (pushToGallery): one push at a time on its own thread, so it
    // never waits behind a pairing on the worker.
    std::thread pushThread;
    std::atomic<bool> pushBusy{false};
    std::atomic<bool> pushCancel{false};

    mutable std::mutex mu;  // everything below
    State state = State::Idle;
    std::string serial;      // connected adb serial ("ip:port")
    std::wstring deviceName;
    std::shared_ptr<scrcpy::Session> session;
    std::unique_ptr<adb::Process> serverProc;
    uint16_t forwardPort = 0;
    std::shared_ptr<std::atomic<bool>> startCancel;
    VideoSink* video = nullptr;
    AudioSink* audio = nullptr;
    scrcpy::InputTranslator translator{[this](std::vector<uint8_t> m) {
        if (session) session->sendControl(std::move(m));  // mu held by caller
    }};

    void logf(const std::string& s) {
        if (self->log) self->log("android: " + s);
    }

    void setState(State s, const std::wstring& detail) {
        {
            std::lock_guard<std::mutex> lk(mu);
            state = s;
        }
        logf(std::string("state ") + stateName(s) + " " + adb::narrow(detail));
        if (self->events.onState) self->events.onState(s, detail);
    }

    void post(std::function<void()> f) {
        {
            std::lock_guard<std::mutex> lk(qmu);
            tasks.push_back(std::move(f));
        }
        qcv.notify_one();
    }

    void loop() {
        for (;;) {
            std::function<void()> f;
            {
                std::unique_lock<std::mutex> lk(qmu);
                qcv.wait(lk, [&] { return quit || !tasks.empty(); });
                if (quit) return;
                f = std::move(tasks.front());
                tasks.pop_front();
            }
            f();
        }
    }

    bool ensureServer() {
        if (serverStarted) return true;
        serverStarted = adb.startServer();
        return serverStarted;
    }

    adb::Result adbRun(const std::vector<std::string>& args, int timeoutMs, const std::atomic<bool>* cancel = nullptr) {
        adb::Result r = adb.run(args, timeoutMs, cancel);
        std::string out = trim(r.output);
        if (out.size() > 300) out = out.substr(0, 300) + "...";
        std::string cmd;
        for (auto& a : args) cmd += a + " ";
        logf("adb " + cmd + "→ " + std::to_string(r.exitCode) + (out.empty() ? "" : " | " + out));
        return r;
    }

    std::vector<adb::MdnsService> adbMdns() {
        adb::Result r = adb.run({"mdns", "services"}, 4000);
        return adb::parseMdnsServices(r.output);
    }

    // Phone's user-visible name.
    std::wstring queryName(const std::string& ser) {
        for (auto args : std::vector<std::vector<std::string>>{
                 {"-s", ser, "shell", "settings", "get", "global", "device_name"},
                 {"-s", ser, "shell", "getprop", "ro.product.marketname"},
                 {"-s", ser, "shell", "getprop", "ro.product.model"}}) {
            auto r = adb.run(args, 5000);
            std::string v = trim(r.output);
            if (r.ok() && !v.empty() && v != "null" && v.find("error") == std::string::npos) return adb::widen(v);
        }
        return adb::widen(ser);
    }

    // adb connect + wait for "device" state; sets serial/name, fires onConnected.
    bool connectTo(const adb::MdnsService& svc, const std::atomic<bool>* cancel) {
        std::string hp = svc.hostPort();
        auto r = adbRun({"connect", hp}, 15000, cancel);
        if (!adb::parseConnectResult(r.output)) return false;
        auto deadline = Clock::now() + 10s;
        bool ready = false;
        while (Clock::now() < deadline && !(cancel && *cancel)) {
            for (auto& d : adb::parseDevices(adb.run({"devices", "-l"}, 4000).output))
                if (d.serial == hp && d.state == "device") ready = true;
            if (ready) break;
            std::this_thread::sleep_for(300ms);
        }
        if (!ready) {
            logf("device " + hp + " not ready (unauthorized/offline)");
            return false;
        }
        std::wstring name = queryName(hp);
        {
            std::lock_guard<std::mutex> lk(mu);
            serial = hp;
            deviceName = name;
        }
        setState(State::Connecting, L"已連線：" + name + L"，準備投影");
        if (self->events.onConnected) self->events.onConnected(name);
        return true;
    }

    // After a successful `adb pair`: find the phone's connect service (same
    // instance as the pairing guid, else same IP) and connect.
    bool connectAfterPair(const std::string& guid, const std::string& host, mdns::Browser& br,
                          const std::atomic<bool>* cancel) {
        setState(State::Connecting, L"配對成功，正在連線…");
        auto deadline = Clock::now() + 20s;
        auto lastAdb = Clock::time_point{};
        while (Clock::now() < deadline && !(cancel && *cancel)) {
            std::vector<adb::MdnsService> all = br.services();
            if (Clock::now() - lastAdb > 1500ms) {
                lastAdb = Clock::now();
                for (auto& s : adbMdns()) all.push_back(s);
            }
            const adb::MdnsService* pick = nullptr;
            for (auto& s : all)
                if (s.type == kConnectType && !guid.empty() && s.instance == guid) pick = &s;
            if (!pick)
                for (auto& s : all)
                    if (s.type == kConnectType && s.host == host) pick = &s;
            if (pick && connectTo(*pick, cancel)) return true;
            std::this_thread::sleep_for(300ms);
        }
        return false;
    }

    void taskPairQr(pairing::Credentials cred) {
        struct Busy { std::atomic<bool>& b; ~Busy() { b = false; } } busyGuard{busy};
        if (!ensureServer()) return setState(State::Error, L"無法啟動 adb");
        setState(State::WaitingForPairing, L"請用手機掃描 QR 圖碼（開發人員選項 → 無線偵錯 → 使用 QR 圖碼配對裝置）");
        mdns::Browser br;
        br.log = [this](const std::string& s) { logf(s); };
        br.start({kPairingType, kConnectType});
        auto deadline = Clock::now() + 10min;
        auto lastAdb = Clock::time_point{};
        adb::MdnsService target;
        bool found = false;
        while (!found && !cancelPair && Clock::now() < deadline) {
            for (auto& s : br.services())
                if (s.type == kPairingType && s.instance == cred.service) target = s, found = true;
            if (!found && Clock::now() - lastAdb > 2s) {
                lastAdb = Clock::now();
                for (auto& s : adbMdns())
                    if (s.type == kPairingType && s.instance == cred.service) target = s, found = true;
            }
            if (!found) std::this_thread::sleep_for(150ms);
        }
        if (cancelPair) return setState(State::Idle, L"已取消配對");
        if (!found) return setState(State::Error, L"等候逾時：沒有偵測到手機掃描 QR 圖碼");

        setState(State::Pairing, L"正在配對 " + adb::widen(target.host) + L"…");
        auto r = adbRun({"pair", target.hostPort(), cred.password}, 30000, &cancelPair);
        std::string guid;
        if (!adb::parsePairResult(r.output, &guid))
            return setState(State::Error, L"配對失敗：" + adb::widen(trim(r.output)));
        if (!connectAfterPair(guid, target.host, br, &cancelPair))
            return setState(cancelPair ? State::Idle : State::Error,
                            cancelPair ? L"已取消" : L"配對成功，但連線失敗（請確認手機的無線偵錯仍開啟）");
    }

    void taskPairCode(std::string hostPort, std::string code) {
        struct Busy { std::atomic<bool>& b; ~Busy() { b = false; } } busyGuard{busy};
        if (!ensureServer()) return setState(State::Error, L"無法啟動 adb");
        setState(State::Pairing, L"正在以配對碼配對 " + adb::widen(hostPort) + L"…");
        mdns::Browser br;
        br.log = [this](const std::string& s) { logf(s); };
        br.start({kConnectType});
        auto r = adbRun({"pair", hostPort, code}, 30000, &cancelPair);
        std::string guid;
        if (!adb::parsePairResult(r.output, &guid))
            return setState(cancelPair ? State::Idle : State::Error,
                            cancelPair ? L"已取消配對" : L"配對失敗：" + adb::widen(trim(r.output)));
        std::string host;
        uint16_t port;
        adb::splitHostPort(hostPort, host, port);
        if (!connectAfterPair(guid, host, br, &cancelPair))
            setState(cancelPair ? State::Idle : State::Error,
                     cancelPair ? L"已取消" : L"配對成功，但連線失敗（請確認手機的無線偵錯仍開啟）");
    }

    void taskReconnect() {
        struct Busy { std::atomic<bool>& b; ~Busy() { b = false; } } busyGuard{busy};
        if (!ensureServer()) return setState(State::Error, L"無法啟動 adb");
        setState(State::Connecting, L"正在尋找已配對的手機…");
        // Still connected from before?
        for (auto& d : adb::parseDevices(adb.run({"devices", "-l"}, 4000).output)) {
            if (d.state == "device" && d.serial.find(':') != std::string::npos) {
                adb::MdnsService s;
                if (adb::splitHostPort(d.serial, s.host, s.port) && connectTo(s, &cancelPair)) return;
            }
        }
        mdns::Browser br;
        br.log = [this](const std::string& s) { logf(s); };
        br.start({kConnectType});
        std::vector<std::string> tried;
        auto deadline = Clock::now() + 6s;
        auto lastAdb = Clock::time_point{};
        while (Clock::now() < deadline && !cancelPair) {
            std::vector<adb::MdnsService> all = br.services();
            if (Clock::now() - lastAdb > 1500ms) {
                lastAdb = Clock::now();
                for (auto& s : adbMdns()) all.push_back(s);
            }
            for (auto& s : all) {
                if (s.type != kConnectType) continue;
                std::string hp = s.hostPort();
                if (std::find(tried.begin(), tried.end(), hp) != tried.end()) continue;
                tried.push_back(hp);
                if (connectTo(s, &cancelPair)) return;  // only paired phones accept
            }
            std::this_thread::sleep_for(200ms);
        }
        setState(State::Idle, tried.empty() ? L"區域網路上沒有找到開啟無線偵錯的手機" : L"找到手機但尚未配對，請掃描 QR 圖碼配對");
    }

    void taskStart(std::shared_ptr<std::atomic<bool>> cancel, VideoSink* v, AudioSink* a) {
        std::string ser;
        std::wstring name;
        {
            std::lock_guard<std::mutex> lk(mu);
            ser = serial;
            name = deviceName;
        }
        auto cancelled = [&] { return cancel->load(); };
        setState(State::Connecting, L"正在啟動投影：" + name);
        auto r = adbRun({"-s", ser, "push", adb::narrow(serverPath), kDeviceJar}, 60000, cancel.get());
        if (cancelled()) return;
        if (!r.ok()) return setState(State::Error, L"無法傳送 scrcpy-server 到手機（連線中斷？）");

        uint32_t scid = 0;
        BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&scid), sizeof(scid), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        scid &= 0x7fffffff;
        char sock[32];
        snprintf(sock, sizeof(sock), "scrcpy_%08x", scid);
        r = adbRun({"-s", ser, "forward", "tcp:0", std::string("localabstract:") + sock}, 10000, cancel.get());
        uint16_t port = uint16_t(atoi(trim(r.output).c_str()));
        if (!r.ok() || port == 0) return setState(State::Error, L"adb forward 失敗");

        char scidArg[32];
        snprintf(scidArg, sizeof(scidArg), "scid=%08x", scid);
        std::vector<std::string> args = {"-s", ser, "shell", std::string("CLASSPATH=") + kDeviceJar, "app_process", "/",
                                         "com.genymobile.scrcpy.Server", scrcpy::kServerVersion, scidArg,
                                         "log_level=info", "audio_codec=aac", "max_size=1920",
                                         "video_bit_rate=8000000", "max_fps=60", "tunnel_forward=true",
                                         "clipboard_autosync=false"};
        auto proc = adb.spawn(args, [this](const std::string& l) { logf("[server] " + l); });
        if (!proc) return setState(State::Error, L"無法啟動 scrcpy-server");

        auto sess = std::make_shared<scrcpy::Session>();
        sess->log = [this](const std::string& s) { logf(s); };
        scrcpy::Session::Config cfg;
        cfg.port = port;
        std::string err;
        adb::Process* pp = proc.get();
        bool ok = sess->connect(cfg, [&] { return !cancelled() && pp->running(); }, &err);
        if (!ok || cancelled()) {
            proc->kill();
            adb.run({"-s", ser, "forward", "--remove", std::string("tcp:") + std::to_string(port)}, 5000);
            if (!cancelled()) setState(State::Error, L"投影啟動失敗：" + adb::widen(err));
            return;
        }
        std::weak_ptr<scrcpy::Session> weak = sess;
        sess->onEnded = [this, weak] {
            post([this, weak] { onSessionEnded(weak.lock()); });
        };
        sess->onVideoSize = [this](int w, int h) {
            std::lock_guard<std::mutex> lk(mu);
            translator.setVideoSize(w, h);
        };
        bool aborted = false;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (cancelled()) {
                aborted = true;
            } else {
                session = sess;
                serverProc = std::move(proc);
                forwardPort = port;
                video = v;
                audio = a;
                translator = scrcpy::InputTranslator([this](std::vector<uint8_t> m) {
                    if (session) session->sendControl(std::move(m));
                });
                sess->run(v, a);  // under mu: a concurrent stop() tears down a running session
            }
        }
        if (aborted) {
            sess->stop();
            proc->kill();
            adb.run({"-s", ser, "forward", "--remove", std::string("tcp:") + std::to_string(port)}, 5000);
            return;
        }
        std::wstring dn = sess->deviceName().empty() ? name : adb::widen(sess->deviceName());
        {
            std::lock_guard<std::mutex> lk(mu);
            if (session != sess) return;  // stopped meanwhile
        }
        setState(State::Mirroring, L"正在投影：" + dn);
    }

    // Takes the session out (under mu) and tears it down on this thread.
    void teardown() {
        std::shared_ptr<scrcpy::Session> s;
        std::unique_ptr<adb::Process> p;
        uint16_t port;
        std::string ser;
        VideoSink* v;
        AudioSink* a;
        {
            std::lock_guard<std::mutex> lk(mu);
            s = std::move(session);
            p = std::move(serverProc);
            port = forwardPort;
            forwardPort = 0;
            ser = serial;
            v = video;
            a = audio;
            video = nullptr;
            audio = nullptr;
            translator.setVideoSize(0, 0);
        }
        if (s) s->stop();
        if (p && !p->wait(1500)) p->kill();  // the server exits when its sockets close
        if (port) {
            std::string spec = "tcp:" + std::to_string(port);
            post([this, ser, spec] { adb.run({"-s", ser, "forward", "--remove", spec}, 5000); });
        }
        if (s) {
            if (v) v->onReset();
            if (a) a->onFlush();
        }
    }

    void onSessionEnded(std::shared_ptr<scrcpy::Session> s) {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!s || s != session) return;  // already stopped
        }
        teardown();
        setState(State::Idle, L"手機連線中斷");
        if (self->events.onDisconnected) self->events.onDisconnected();
    }

    // 傳到手機: push, then make MediaStore index the file. On Android 11+ a
    // file written through /sdcard (FUSE) is usually indexed on close already;
    // the explicit scans cover older versions and OEM builds that do not.
    void taskPush(std::wstring local, std::string ser, std::function<void(const PushResult&)> done) {
        struct Busy { std::atomic<bool>& b; ~Busy() { b = false; } } busyGuard{pushBusy};
        PushResult res;
        const size_t slash = local.find_last_of(L"\\/");
        const std::string remote = galleryPath(slash == std::wstring::npos ? local : local.substr(slash + 1));
        const size_t rs = remote.rfind('/');
        const std::string dir = remote.substr(0, rs), name = remote.substr(rs + 1);
        const bool video = remote.rfind("/sdcard/Movies/", 0) == 0;
        // MediaProvider resolves paths itself: give it the canonical one.
        const std::string canonical = "/storage/emulated/0" + remote.substr(7);
        res.remotePath = remote;
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        GetFileAttributesExW(local.c_str(), GetFileExInfoStandard, &fa);
        const uint64_t bytes = (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        logf("push " + adb::narrow(local) + " (" + std::to_string(bytes / 1024) + " KB) → " + ser + ":" + remote);
        adbRun({"-s", ser, "shell", "mkdir -p " + dir}, 10000, &pushCancel);
        // Wi-Fi: allow ~256 KB/s at worst, at least a minute, at most 30 min.
        const int timeout = int(std::min<uint64_t>(60000 + bytes / 256, 30ull * 60 * 1000));
        auto r = adbRun({"-s", ser, "push", adb::narrow(local), remote}, timeout, &pushCancel);
        if (!r.ok()) {
            res.error = trim(r.output);
            if (res.error.empty()) res.error = r.exitCode == -2 ? "timed out" : "adb push failed";
            logf("push failed: " + res.error);
            if (done) done(res);
            return;
        }
        res.ok = true;
        const std::string table = video ? "video" : "images";
        auto indexed = [&](int tries) {
            for (int i = 0; i < tries && !pushCancel; ++i) {
                if (i) std::this_thread::sleep_for(500ms);
                auto q = adbRun({"-s", ser, "shell",
                                 "content query --uri content://media/external/" + table +
                                     "/media --projection _id --where \"_display_name='" + name + "'\""},
                                10000, &pushCancel);
                if (q.output.find("Row:") != std::string::npos) return true;
            }
            return false;
        };
        // 1. MediaProvider's own single-file scan (Android 10+, shell has WRITE_MEDIA_STORAGE).
        adbRun({"-s", ser, "shell", "content call --uri content://media --method scan_file --arg " + canonical}, 15000,
               &pushCancel);
        // 2. The classic broadcast (Android ≤ 10, and many OEM builds after).
        adbRun({"-s", ser, "shell",
                "am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d file://" + canonical},
               15000, &pushCancel);
        res.inGallery = indexed(4);
        if (!res.inGallery && !pushCancel) {
            // 3. Rescan the primary volume (slower, but indexes everything new).
            adbRun({"-s", ser, "shell", "content call --uri content://media --method scan_volume --arg external_primary"},
                   60000, &pushCancel);
            res.inGallery = indexed(6);
        }
        logf(std::string("push done: ") + (res.inGallery ? "in the gallery" : "on the phone (not seen in MediaStore yet)"));
        if (done) done(res);
    }
};

std::string AndroidSource::galleryPath(const std::wstring& fileName) {
    std::wstring ext;
    std::wstring base = fileName;
    if (const size_t dot = fileName.rfind(L'.'); dot != std::wstring::npos) {
        ext = fileName.substr(dot);
        base = fileName.substr(0, dot);
    }
    for (wchar_t& c : ext) c = towlower(c);
    const bool video = ext == L".mp4" || ext == L".mov" || ext == L".m4v" || ext == L".webm" || ext == L".mkv" ||
                       ext == L".3gp";
    // ASCII only (shell- and gallery-safe): 自在投影_20261008_101010 → ZizaiCast_20261008_101010.
    std::string name;
    bool dropped = false;
    for (wchar_t c : base)
        if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'_' ||
            c == L'-' || c == L'.')
            name.push_back(char(c));
        else if ((c == L' ' || c == L'(' || c == L')') && !name.empty() && name.back() != '_')
            name.push_back('_');
        else
            dropped = dropped || c > 0x7F;
    while (!name.empty() && name.back() == '_') name.pop_back();
    if (name.empty() || name.front() == '_' || name.front() == '.' || name.front() == '-' ||
        (dropped && name.rfind("ZizaiCast", 0) != 0))
        name = "ZizaiCast" + (name.empty() || name.front() == '_' ? name : "_" + name);
    std::string e;
    for (wchar_t c : ext)
        if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'.') e.push_back(char(c));
    if (e.size() < 2) e = video ? ".mp4" : ".png";
    return std::string(video ? "/sdcard/Movies/ZizaiCast/" : "/sdcard/Pictures/ZizaiCast/") + name + e;
}

bool AndroidSource::pushToGallery(const std::wstring& path, std::function<void(const PushResult&)> done) {
    if (!d_->inited || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    std::string ser;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        ser = d_->serial;
    }
    if (ser.empty() || d_->pushBusy.exchange(true)) return false;
    if (d_->pushThread.joinable()) d_->pushThread.join();  // the previous one has finished (pushBusy was false)
    d_->pushCancel = false;
    d_->pushThread = std::thread([this, path, ser, done = std::move(done)]() mutable {
        d_->taskPush(path, ser, std::move(done));
    });
    return true;
}

AndroidSource::AndroidSource() : d_(std::make_unique<Impl>()) {
    d_->self = this;
    d_->worker = std::thread([this] { d_->loop(); });
}

AndroidSource::~AndroidSource() {
    d_->cancelPair = true;
    d_->pushCancel = true;
    if (d_->pushThread.joinable()) d_->pushThread.join();
    stop();
    {
        std::lock_guard<std::mutex> lk(d_->qmu);
        d_->quit = true;
        d_->tasks.clear();
    }
    d_->qcv.notify_all();
    if (d_->worker.joinable()) d_->worker.join();
    if (d_->serverStarted) d_->adb.killServer();
}

bool AndroidSource::init(const std::wstring& toolsDir) {
    std::wstring dir = toolsDir;
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    d_->toolsDir = dir;
    d_->serverPath = dir + L"\\scrcpy-server";
    d_->adb.log = [this](const std::string& s) { d_->logf(s); };
    // Private adb server port; PM_ADB_PORT overrides (tests run next to the app).
    uint16_t port = 15037;
    wchar_t env[16];
    if (GetEnvironmentVariableW(L"PM_ADB_PORT", env, 16) > 0 && _wtoi(env) > 0 && _wtoi(env) < 65536)
        port = uint16_t(_wtoi(env));
    if (!d_->adb.init(dir + L"\\adb.exe", port)) {
        d_->logf("adb.exe not found in " + adb::narrow(dir));
        return false;
    }
    if (GetFileAttributesW(d_->serverPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        d_->logf("scrcpy-server not found in " + adb::narrow(dir));
        return false;
    }
    d_->inited = true;
    d_->post([this] { d_->ensureServer(); });  // warm up (~1 s) in the background
    return true;
}

bool AndroidSource::beginQrPairing(std::vector<uint8_t>& bgra, int& size, std::wstring& qrText) {
    if (!d_->inited) return false;
    cancelPairing();
    auto cred = pairing::randomCredentials();
    if (!pairing::renderQr(cred.qrText(), 8, 4, bgra, size)) return false;
    qrText = adb::widen(cred.qrText());
    d_->busy = true;
    d_->post([this, cred] {
        d_->cancelPair = false;
        d_->taskPairQr(cred);
    });
    return true;
}

bool AndroidSource::pairWithCode(const std::wstring& hostPort, const std::wstring& code) {
    if (!d_->inited) return false;
    std::string hp = adb::narrow(hostPort), c = adb::narrow(code), host;
    uint16_t port;
    if (!adb::splitHostPort(hp, host, port) || c.size() != 6 ||
        c.find_first_not_of("0123456789") != std::string::npos)
        return false;
    cancelPairing();
    d_->busy = true;
    d_->post([this, hp, c] {
        d_->cancelPair = false;
        d_->taskPairCode(hp, c);
    });
    return true;
}

void AndroidSource::cancelPairing() { d_->cancelPair = true; }

bool AndroidSource::connectKnownDevices() {
    if (!d_->inited || d_->busy) return false;
    d_->busy = true;
    d_->post([this] {
        d_->cancelPair = false;
        d_->taskReconnect();
    });
    return true;
}

bool AndroidSource::start(VideoSink* video, AudioSink* audio) {
    if (!d_->inited) return false;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        if (d_->serial.empty() || d_->session) return false;
        if (d_->startCancel) *d_->startCancel = true;
        d_->startCancel = cancel;
    }
    d_->post([this, cancel, video, audio] { d_->taskStart(cancel, video, audio); });
    return true;
}

void AndroidSource::stop() {
    bool had;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        if (d_->startCancel) *d_->startCancel = true;
        had = d_->session != nullptr;
    }
    d_->teardown();
    if (had) d_->setState(State::Idle, L"已停止投影");
}

void AndroidSource::sendPointer(const VideoWindow::PointerEvent& e) {
    std::lock_guard<std::mutex> lk(d_->mu);
    if (d_->session) d_->translator.pointer(e);
}

void AndroidSource::sendKey(unsigned vk, bool down, wchar_t ch) {
    std::lock_guard<std::mutex> lk(d_->mu);
    if (d_->session) d_->translator.key(vk, down, ch);
}

static void pressKey(AndroidSource::Impl* d, uint32_t keycode) {
    std::lock_guard<std::mutex> lk(d->mu);
    if (!d->session) return;
    d->session->sendControl(scrcpy::msgKeycode(scrcpy::kKeyDown, keycode, 0, 0));
    d->session->sendControl(scrcpy::msgKeycode(scrcpy::kKeyUp, keycode, 0, 0));
}

void AndroidSource::pressBack() { pressKey(d_.get(), scrcpy::kKeyBack); }
void AndroidSource::pressHome() { pressKey(d_.get(), scrcpy::kKeyHome); }
void AndroidSource::pressAppSwitch() { pressKey(d_.get(), scrcpy::kKeyAppSwitch); }

AndroidSource::State AndroidSource::state() const {
    std::lock_guard<std::mutex> lk(d_->mu);
    return d_->state;
}

}  // namespace pm
