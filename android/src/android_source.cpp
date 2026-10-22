#include "pm/android_source.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cwctype>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

#include "adb.h"
#include "android_internal.h"
#include "backoff.h"
#include "clipboard.h"
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
constexpr const char* kDownloadDir = "/sdcard/Download/";

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    return s.substr(i);
}

std::string firstLine(const std::string& s) {
    std::string t = trim(s);
    const size_t nl = t.find_first_of("\r\n");
    if (nl != std::string::npos) t.resize(nl);
    if (t.size() > 160) t = t.substr(0, 160) + "...";
    return t;
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

bool isUsbSerial(const std::string& s) {
    return !s.empty() && s.find(':') == std::string::npos && s.rfind("emulator-", 0) != 0 &&
           s.find("._adb-tls") == std::string::npos;
}

int64_t nowMs() { return scrcpy::Session::nowMs(); }

// User-facing texts (zh-TW; the app translates them through its Mod* table).
const wchar_t* kTxtLost = L"手機連線中斷";
const wchar_t* kTxtReconnectFail = L"手機連線中斷，重新連線失敗（請確認手機的無線偵錯或 USB 偵錯仍開啟）";
const wchar_t* kTxtCaptureFail = L"手機無法錄製畫面（視訊編碼器失敗），請再試一次；若一再發生，請重新啟動手機";
const wchar_t* kTxtNoPhoneFile = L"沒有連線的手機，無法傳送檔案";
const wchar_t* kTxtNoPhoneApk = L"沒有連線的手機，無法安裝 App";
const wchar_t* kTxtCancelled = L"已取消";
}  // namespace

// ------------------------------------------------------------ helpers --

namespace android_internal {

Tuning& tuning() {
    static Tuning t;
    return t;
}

std::vector<std::string> serverArgs(const ServerOptions& o) {
    std::vector<std::string> a = {"scid=" + o.scid,
                                  "log_level=info",
                                  "audio_codec=aac",
                                  "max_size=" + std::to_string(o.maxSize),
                                  "video_bit_rate=8000000",
                                  "max_fps=60",
                                  "tunnel_forward=true",
                                  std::string("clipboard_autosync=") + (o.clipboardAutosync ? "true" : "false")};
    if (o.stayAwake) {
        a.push_back("stay_awake=true");   // stay_on_while_plugged_in (charging / USB), restored on exit
        a.push_back("keep_active=true");  // simulated user activity (Wi-Fi, not charging)
    }
    return a;
}

std::wstring explainServerLine(const std::string& line, int* kind) {
    auto has = [&](const char* s) { return line.find(s) != std::string::npos; };
    int k = 0;
    std::wstring w;
    if (has("Audio disabled")) {
        k = 2;
        w = has("Android 11") ? L"這支手機的 Android 版本低於 11，投影沒有聲音" : L"手機的聲音無法擷取，這次投影沒有聲音";
    } else if (has("ERROR")) {
        if (has("Audio") || has("audio")) {
            k = 2;
            w = L"手機的聲音無法擷取，這次投影沒有聲音";
        } else if (has("INJECT_EVENTS") || has("SecurityException") || has("Permission denial")) {
            k = 3;
            w = L"手機拒絕了操控需要的權限（小米／紅米手機請在開發人員選項開啟「USB 偵錯（安全性設定）」）";
        } else if (has("Capture/encoding error") || has("Video encoding error") || has("video encoder") ||
                   has("Could not create display") || has("MediaCodec") || has("encoder")) {
            k = 1;
            w = kTxtCaptureFail;
        } else {
            k = 3;
            std::string m = line;
            const size_t e = m.find("ERROR:");
            if (e != std::string::npos) m = m.substr(e + 6);
            w = L"scrcpy-server 錯誤：" + adb::widen(firstLine(m));
        }
    }
    if (kind) *kind = k;
    return w;
}

std::wstring explainInstallFailure(const std::string& out, int exitCode) {
    if (exitCode == -2) return L"安裝逾時：請看手機畫面，是否在等你按「安裝」或「允許」";
    if (exitCode == -1 && out.find("cannot start adb") != std::string::npos) return L"無法執行 adb，請重新啟動程式";
    auto has = [&](const char* s) { return out.find(s) != std::string::npos; };
    std::string code;
    for (const char* prefix : {"INSTALL_FAILED_", "INSTALL_PARSE_FAILED_", "INSTALL_CANCELED"}) {
        const size_t p = out.find(prefix);
        if (p == std::string::npos) continue;
        size_t e = p;
        while (e < out.size() && (isupper(uint8_t(out[e])) || isdigit(uint8_t(out[e])) || out[e] == '_')) ++e;
        code = out.substr(p, e - p);
        break;
    }
    struct Map { const char* code; const wchar_t* text; };
    static const Map kMap[] = {
        {"INSTALL_FAILED_VERSION_DOWNGRADE", L"手機上已經裝了較新的版本，不能換成舊版（要裝舊版請先在手機上解除安裝）"},
        {"INSTALL_FAILED_UPDATE_INCOMPATIBLE", L"手機上已經有同名但簽章不同的 App，請先在手機上解除安裝它再試"},
        {"INSTALL_FAILED_SHARED_USER_INCOMPATIBLE", L"手機上已經有同名但簽章不同的 App，請先在手機上解除安裝它再試"},
        {"INSTALL_FAILED_INSUFFICIENT_STORAGE", L"手機的儲存空間不足，請先清出空間再安裝"},
        {"INSTALL_FAILED_OLDER_SDK", L"這個 App 需要較新的 Android 版本，這支手機裝不了"},
        {"INSTALL_FAILED_DEPRECATED_SDK_VERSION", L"這個 App 太舊了，這支手機的 Android 不允許安裝"},
        {"INSTALL_FAILED_NO_MATCHING_ABIS", L"這個 App 不支援這支手機的處理器"},
        {"INSTALL_FAILED_MISSING_SPLIT", L"這個 App 需要多個安裝檔（.apks／.xapk），只有這個 APK 裝不起來"},
        {"INSTALL_FAILED_INVALID_APK", L"這個檔案不是完整的 APK（可能下載不完全或已損壞）"},
        {"INSTALL_PARSE_FAILED_", L"這個檔案不是完整的 APK（可能下載不完全或已損壞）"},
        {"INSTALL_FAILED_USER_RESTRICTED", L"手機拒絕從電腦安裝：請在手機上允許（小米／紅米手機請在開發人員選項開啟「USB 安裝」）"},
        {"INSTALL_FAILED_ABORTED", L"手機上的安裝被取消了（請在手機畫面按「安裝」或「允許」）"},
        {"INSTALL_CANCELED", L"手機上的安裝被取消了（請在手機畫面按「安裝」或「允許」）"},
        {"INSTALL_FAILED_VERIFICATION_FAILURE", L"手機的安全檢查（Play 安全防護）拒絕安裝這個 App"},
        {"INSTALL_FAILED_DUPLICATE_PERMISSION", L"這個 App 和手機上另一個 App 的權限衝突，無法安裝"},
        {"INSTALL_FAILED_TEST_ONLY", L"這是測試版 APK，手機不允許直接安裝"},
        {"INSTALL_FAILED_ALREADY_EXISTS", L"手機上已經裝了這個 App"},
        {"INSTALL_FAILED_CONFLICTING_PROVIDER", L"這個 App 和手機上另一個 App 衝突，無法安裝"},
        {"INSTALL_FAILED_MEDIA_UNAVAILABLE", L"手機的儲存空間目前無法使用"},
    };
    for (const auto& m : kMap)
        if (!code.empty() && code.rfind(m.code, 0) == 0) return m.text;
    if (has("device offline") || has("no devices/emulators") || has("not found") || has("closed") ||
        has("device unauthorized"))
        return L"手機連線中斷，無法安裝";
    if (has("Permission denied")) return L"手機拒絕從電腦安裝 App";
    if (!code.empty()) return L"安裝失敗：" + adb::widen(code);
    const std::string l = firstLine(out);
    return l.empty() ? L"安裝失敗" : L"安裝失敗：" + adb::widen(l);
}

std::wstring explainPushFailure(const std::string& out, int exitCode) {
    if (exitCode == -2) return L"傳送逾時（Wi-Fi 太慢，或手機沒有回應）";
    auto has = [&](const char* s) { return out.find(s) != std::string::npos; };
    if (has("No space left")) return L"手機的儲存空間不足";
    if (has("Read-only file system") || has("Permission denied")) return L"手機不允許寫入 Download 資料夾";
    if (has("cannot stat") || has("No such file or directory")) return L"找不到要傳送的檔案";
    if (has("device offline") || has("no devices/emulators") || has("not found") || has("closed"))
        return L"手機連線中斷，無法傳送";
    const std::string l = firstLine(out);
    return l.empty() ? L"傳送失敗" : L"傳送失敗：" + adb::widen(l);
}

std::string downloadName(const std::wstring& fileName) {
    const size_t slash = fileName.find_last_of(L"\\/");
    std::wstring n = slash == std::wstring::npos ? fileName : fileName.substr(slash + 1);
    for (wchar_t& c : n)
        if (c < 0x20 || c == 0x7f || wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    while (!n.empty() && (n.back() == L' ' || n.back() == L'.')) n.pop_back();
    while (!n.empty() && n.front() == L' ') n.erase(n.begin());
    if (n.empty()) n = L"file";
    return adb::narrow(n);
}

std::string numberedName(const std::string& name, int n) {
    size_t dot = name.rfind('.');
    if (dot == 0 || dot == std::string::npos) dot = name.size();  // ".bashrc" / no extension
    return name.substr(0, dot) + " (" + std::to_string(n) + ")" + name.substr(dot);
}

std::string shellQuote(const std::string& s) {
    std::string q = "'";
    for (char c : s) {
        if (c == '\'') q += "'\\''";
        else q.push_back(c);
    }
    return q + "'";
}

}  // namespace android_internal

namespace ai = android_internal;

// --------------------------------------------------------------- Impl --

struct AndroidSource::Impl {
    AndroidSource* self = nullptr;
    adb::Adb adb;
    ai::Tuning T = ai::tuning();
    std::wstring toolsDir, serverPath;
    bool inited = false;
    std::atomic<bool> serverStarted{false};

    // Worker: all slow operations run here, one at a time.  Timed tasks
    // (retries with backoff, the session monitor, the adb idle check) wait
    // in `timers` and run on the same thread when due.
    std::thread worker;
    std::mutex qmu;
    std::condition_variable qcv;
    std::deque<std::function<void()>> tasks;
    struct Timer {
        Clock::time_point at;
        std::function<void()> f;
    };
    std::vector<Timer> timers;
    bool quit = false;
    std::atomic<int> busy{0};  // pairing / connect tasks queued or running

    // One cancel token per pairing / reconnect request (never a shared flag
    // that the next task resets: a request cancelled while still queued must
    // stay cancelled).  `current` is the newest one (under qmu).
    struct Request {
        explicit Request(bool p) : pairing(p) {}
        const bool pairing;               // QR / code pairing (not a reconnect)
        std::atomic<bool> cancel{false};
        std::atomic<bool> paired{false};  // adb pair done: closing the panel no longer cancels
    };
    std::shared_ptr<Request> current;
    struct BusyGuard {
        std::atomic<int>& n;
        ~BusyGuard() { --n; }
    };
    // Queues a request task, cancelling the previous request.
    std::shared_ptr<Request> postRequest(bool pairing, std::function<void(Request&)> task) {
        auto req = std::make_shared<Request>(pairing);
        ++busy;
        {
            std::lock_guard<std::mutex> lk(qmu);
            if (current) current->cancel = true;
            current = req;
        }
        post([this, req, task = std::move(task)] {
            BusyGuard g{busy};
            if (req->cancel) {
                logf(std::string(req->pairing ? "pairing" : "reconnect") + " cancelled before it ran");
                return;
            }
            task(*req);
        });
        return req;
    }

    // 傳到手機 (pushToGallery): one push at a time on its own thread, so it
    // never waits behind a pairing on the worker.
    std::thread pushThread;
    std::atomic<bool> pushBusy{false};
    std::atomic<bool> pushCancel{false};

    // pushFile / installApk: FIFO on one transfer thread.
    struct Transfer {
        bool install = false;
        std::wstring path;
        std::function<void(bool, std::wstring)> done;
    };
    std::thread transferThread;
    std::mutex tmu;
    std::condition_variable tcv;
    std::deque<Transfer> transfers;
    bool tquit = false;
    std::atomic<bool> transferBusy{false};
    std::atomic<bool> transferCancel{false};

    // Options (any thread).
    std::atomic<bool> clipboardSync{true};
    std::atomic<bool> screenOnWanted{true};
    std::atomic<bool> stayAwake{false};

    // PC clipboard (clipMu).
    std::mutex clipMu;
    std::unique_ptr<clip::Backend> clip;
    uint32_t clipSeqSeen = 0;   // last PC sequence looked at
    uint32_t clipSeqOurs = 0;   // the sequence our own write (phone → PC) produced
    std::string clipLast;       // last text synced either way
    uint64_t clipMsgSeq = 0;    // SET_CLIPBOARD sequence numbers

    // Server output of the current scrcpy-server (slMu).
    std::mutex slMu;
    int serverErrorKind = 0;    // explainServerLine kind of the last ERROR
    std::wstring serverErrorText;
    std::vector<std::wstring> noticesSent;  // this session (said once)

    // adb idle shutdown (worker thread only, except lastActivity).
    std::atomic<int64_t> lastActivity{0};
    bool idleArmed = false;
    std::atomic<bool> chainPending{false};  // a reconnect chain is waiting / running

    mutable std::mutex mu;  // everything below
    State state = State::Idle;
    std::string serial;      // connected adb serial ("ip:port", or the USB serial)
    std::wstring deviceName;
    std::shared_ptr<scrcpy::Session> session;
    std::unique_ptr<adb::Process> serverProc;
    uint16_t forwardPort = 0;
    std::shared_ptr<std::atomic<bool>> startCancel;
    VideoSink* video = nullptr;
    AudioSink* audio = nullptr;
    // None: no mirroring.  Starting: start() queued / running.  Running: a
    // session streams.  Restarting: the session ended / stalled and a new
    // scrcpy-server is being started on the same sinks.
    enum class Phase { None, Starting, Running, Restarting } phase = Phase::None;
    uint64_t sessionGen = 0;
    bool inputSeen = false;          // the user controlled the phone in this mirroring
    bool screenOffApplied = false;   // SET_DISPLAY_POWER(false) sent to the current session
    bool resetSent = false;          // RESET_VIDEO sent for the current stall
    bool captureRetried = false;
    int64_t runningSince = 0;
    android::Backoff restartBo{500, 4000};
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

    void notice(const std::wstring& text) {
        {
            std::lock_guard<std::mutex> lk(slMu);
            if (std::find(noticesSent.begin(), noticesSent.end(), text) != noticesSent.end()) return;
            noticesSent.push_back(text);
        }
        logf("notice: " + adb::narrow(text));
        if (self->events.onNotice) self->events.onNotice(text);
    }

    // ---- worker ----

    void post(std::function<void()> f) {
        {
            std::lock_guard<std::mutex> lk(qmu);
            tasks.push_back(std::move(f));
        }
        qcv.notify_one();
    }

    void postAfter(int ms, std::function<void()> f) {
        {
            std::lock_guard<std::mutex> lk(qmu);
            timers.push_back({Clock::now() + std::chrono::milliseconds(std::max(0, ms)), std::move(f)});
        }
        qcv.notify_one();
    }

    void loop() {
        for (;;) {
            std::function<void()> f;
            {
                std::unique_lock<std::mutex> lk(qmu);
                for (;;) {
                    if (quit) return;
                    if (!tasks.empty()) {
                        f = std::move(tasks.front());
                        tasks.pop_front();
                        break;
                    }
                    auto due = timers.end();
                    for (auto it = timers.begin(); it != timers.end(); ++it)
                        if (due == timers.end() || it->at < due->at) due = it;
                    if (due != timers.end() && due->at <= Clock::now()) {
                        f = std::move(due->f);
                        timers.erase(due);
                        break;
                    }
                    if (due == timers.end()) qcv.wait(lk);
                    else qcv.wait_until(lk, due->at);
                }
            }
            f();
            lastActivity = nowMs();
            armIdle();
        }
    }

    // ---- adb server lifetime ----

    bool ensureServer() {
        if (serverStarted) return true;
        serverStarted = adb.startServer();
        if (serverStarted) logf("adb server started (port " + std::to_string(adb.port()) + ")");
        return serverStarted;
    }

    // The adb server (~18 MB, a few wake-ups per second for USB / mDNS) is
    // stopped when nothing uses it for T.idleKillMs; every path that needs
    // adb calls ensureServer() first (~1-2 s to start it again).
    void armIdle() {
        if (T.idleKillMs <= 0 || !serverStarted || idleArmed) return;
        idleArmed = true;
        postAfter(T.idleKillMs, [this] {
            idleArmed = false;
            idleCheck();
        });
    }

    bool serverInUse() {
        if (busy > 0 || chainPending || pushBusy || transferBusy) return true;
        {
            std::lock_guard<std::mutex> lk(tmu);
            if (!transfers.empty()) return true;
        }
        std::lock_guard<std::mutex> lk(mu);
        return session || phase != Phase::None || (state != State::Idle && state != State::Error);
    }

    void idleCheck() {
        if (!serverStarted) return;
        const int64_t idleFor = nowMs() - lastActivity.load();
        if (idleFor + 20 < T.idleKillMs) {  // something ran meanwhile: check again later
            idleArmed = true;
            postAfter(int(T.idleKillMs - idleFor), [this] {
                idleArmed = false;
                idleCheck();
            });
            return;
        }
        if (serverInUse()) return;  // re-armed when that work ends
        adb.killServer();
        serverStarted = false;
        {
            std::lock_guard<std::mutex> lk(mu);
            serial.clear();  // the transport went with the server
            deviceName.clear();
        }
        logf("adb server stopped (idle " + std::to_string(idleFor / 1000) + " s)");
    }

    adb::Result adbRun(const std::vector<std::string>& args, int timeoutMs, const std::atomic<bool>* cancel = nullptr) {
        adb::Result r = adb.run(args, timeoutMs, cancel);
        lastActivity = nowMs();
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

    // A real round trip to the phone (a dead Wi-Fi transport can still be
    // listed as "device" for a while).
    bool probeAlive(const std::string& ser) {
        if (ser.empty()) return false;
        auto r = adbRun({"-s", ser, "shell", "echo", "pm-alive"}, T.probeTimeoutMs);
        return r.ok() && r.output.find("pm-alive") != std::string::npos;
    }

    std::string deviceState(const std::string& ser) {
        for (auto& d : adb::parseDevices(adb.run({"devices", "-l"}, 4000).output))
            if (d.serial == ser) return d.state;
        return {};
    }

    // The phone's transport is up: remember it and tell the app.
    void connected(const std::string& ser) {
        std::wstring name = queryName(ser);
        {
            std::lock_guard<std::mutex> lk(mu);
            serial = ser;
            deviceName = name;
        }
        setState(State::Connecting, L"已連線：" + name + L"，準備投影");
        if (self->events.onConnected) self->events.onConnected(name);
    }

    // Waits for `ser` to be in "device" state (backoff polling).
    bool waitDevice(const std::string& ser, int ms, const std::atomic<bool>* cancel) {
        auto deadline = Clock::now() + std::chrono::milliseconds(ms);
        android::Backoff bo(100, 800, 0);
        while (Clock::now() < deadline && !(cancel && *cancel)) {
            if (deviceState(ser) == "device") return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(bo.next()));
        }
        return false;
    }

    // adb connect + wait for "device" state; sets serial/name, fires onConnected.
    // A transport adb still lists after the phone dropped off the network
    // ("offline", or "already connected" to a dead one) is disconnected and
    // connected once more: without that a reconnect never succeeds until the
    // adb server restarts.
    bool connectTo(const adb::MdnsService& svc, const std::atomic<bool>* cancel) {
        std::string hp = svc.hostPort();
        for (int round = 0; round < 2; ++round) {
            auto r = adbRun({"connect", hp}, 15000, cancel);
            if (!adb::parseConnectResult(r.output)) {
                if (round == 0 && r.output.find("already") == std::string::npos && !deviceState(hp).empty()) {
                    adbRun({"disconnect", hp}, 5000);  // stale entry blocks a fresh connect
                    continue;
                }
                return false;
            }
            if (waitDevice(hp, round == 0 ? 5000 : 10000, cancel)) {
                connected(hp);
                return true;
            }
            if (cancel && *cancel) return false;
            logf("device " + hp + " not ready (" + deviceState(hp) + "): reconnecting the transport");
            adbRun({"disconnect", hp}, 5000);
        }
        logf("device " + hp + " not ready (unauthorized/offline)");
        return false;
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

    void taskPairQr(pairing::Credentials cred, Request& req) {
        std::atomic<bool>& cancelPair = req.cancel;
        if (!ensureServer()) return setState(State::Error, L"無法啟動 adb");
        setState(State::WaitingForPairing, L"請用手機掃描 QR 圖碼（開發人員選項 → 無線偵錯 → 使用 QR 圖碼配對裝置）");
        mdns::Browser br;
        br.log = [this](const std::string& s) { logf(s); };
        if (T.mdnsBrowser) br.start({kPairingType, kConnectType}, T.mdnsTargets);
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
        req.paired = true;
        if (!connectAfterPair(guid, target.host, br, &cancelPair))
            return setState(cancelPair ? State::Idle : State::Error,
                            cancelPair ? L"已取消" : L"配對成功，但連線失敗（請確認手機的無線偵錯仍開啟）");
    }

    void taskPairCode(std::string hostPort, std::string code, Request& req) {
        std::atomic<bool>& cancelPair = req.cancel;
        if (!ensureServer()) return setState(State::Error, L"無法啟動 adb");
        setState(State::Pairing, L"正在以配對碼配對 " + adb::widen(hostPort) + L"…");
        mdns::Browser br;
        br.log = [this](const std::string& s) { logf(s); };
        if (T.mdnsBrowser) br.start({kConnectType}, T.mdnsTargets);
        auto r = adbRun({"pair", hostPort, code}, 30000, &cancelPair);
        std::string guid;
        if (!adb::parsePairResult(r.output, &guid))
            return setState(cancelPair ? State::Idle : State::Error,
                            cancelPair ? L"已取消配對" : L"配對失敗：" + adb::widen(trim(r.output)));
        req.paired = true;
        std::string host;
        uint16_t port;
        adb::splitHostPort(hostPort, host, port);
        if (!connectAfterPair(guid, host, br, &cancelPair))
            setState(cancelPair ? State::Idle : State::Error,
                     cancelPair ? L"已取消" : L"配對成功，但連線失敗（請確認手機的無線偵錯仍開啟）");
    }

    // Browses _adb-tls-connect._tcp for `ms` and connects every phone found
    // (only paired ones accept); `preferHost` first.
    bool browseAndConnect(int ms, const std::string& preferHost, const std::atomic<bool>* cancel,
                          bool* sawAny = nullptr) {
        mdns::Browser br;
        br.log = [this](const std::string& s) { logf(s); };
        if (T.mdnsBrowser) br.start({kConnectType}, T.mdnsTargets);
        std::vector<std::string> tried;
        auto deadline = Clock::now() + std::chrono::milliseconds(ms);
        auto lastAdb = Clock::time_point{};
        while (Clock::now() < deadline && !(cancel && *cancel)) {
            std::vector<adb::MdnsService> all = br.services();
            if (Clock::now() - lastAdb > 1500ms) {
                lastAdb = Clock::now();
                for (auto& s : adbMdns()) all.push_back(s);
            }
            std::stable_partition(all.begin(), all.end(), [&](const adb::MdnsService& s) { return s.host == preferHost; });
            for (auto& s : all) {
                if (s.type != kConnectType) continue;
                std::string hp = s.hostPort();
                if (std::find(tried.begin(), tried.end(), hp) != tried.end()) continue;
                tried.push_back(hp);
                if (sawAny) *sawAny = true;
                if (connectTo(s, cancel)) return true;  // only paired phones accept
            }
            std::this_thread::sleep_for(200ms);
        }
        return false;
    }

    // adb stopped (idle): is any phone advertising wireless debugging at all?
    // Answered by our own mDNS browser, so the app's periodic scan does not
    // start adb (~2 s, ~130 ms CPU) every time just to find nothing.
    int coldScans = 0;
    bool anyPhoneAdvertising(const std::atomic<bool>* cancel) {
        mdns::Browser br;
        br.log = [this](const std::string& s) { logf(s); };
        if (!br.start({kConnectType}, T.mdnsTargets)) return true;  // cannot tell: full scan
        auto deadline = Clock::now() + std::chrono::milliseconds(T.coldScanMs);
        while (Clock::now() < deadline && !(cancel && *cancel)) {
            for (auto& s : br.services())
                if (s.type == kConnectType) return true;
            std::this_thread::sleep_for(100ms);
        }
        return false;
    }

    void taskReconnect(Request& req) {
        std::atomic<bool>& cancelPair = req.cancel;
        if (!serverStarted && T.mdnsBrowser && T.coldScanFullEvery > 0 && ++coldScans % T.coldScanFullEvery != 0) {
            setState(State::Connecting, L"正在尋找已配對的手機…");
            if (!anyPhoneAdvertising(&cancelPair)) {
                logf("no phone advertises wireless debugging (adb not started)");
                return setState(State::Idle, cancelPair ? L"已取消" : L"區域網路上沒有找到開啟無線偵錯的手機");
            }
        }
        if (!ensureServer()) return setState(State::Error, L"無法啟動 adb");
        setState(State::Connecting, L"正在尋找已配對的手機…");
        // Still connected from before?
        for (auto& d : adb::parseDevices(adb.run({"devices", "-l"}, 4000).output)) {
            if (d.state == "device" && d.serial.find(':') != std::string::npos) {
                adb::MdnsService s;
                if (adb::splitHostPort(d.serial, s.host, s.port) && connectTo(s, &cancelPair)) return;
            }
        }
        bool sawAny = false;
        if (browseAndConnect(6000, {}, &cancelPair, &sawAny)) return;
        setState(State::Idle, !sawAny ? L"區域網路上沒有找到開啟無線偵錯的手機" : L"找到手機但尚未配對，請掃描 QR 圖碼配對");
    }

    // ---- USB ----

    void taskUsb(Request& req) {
        if (!ensureServer()) return setState(State::Error, L"無法啟動 adb");
        setState(State::Connecting, L"正在尋找用 USB 連接的手機…");
        const auto start = Clock::now();
        android::Backoff bo(250, 2000, 0);
        bool askedAuth = false;
        for (;;) {
            if (req.cancel) return setState(State::Idle, L"已取消");
            std::string unauthorized, other;
            for (auto& d : adb::parseDevices(adbRun({"devices", "-l"}, 4000).output)) {
                if (!isUsbSerial(d.serial)) continue;
                if (d.state == "device") return connected(d.serial);
                if (d.state == "unauthorized") unauthorized = d.serial;
                else other = d.serial;
            }
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            if (!unauthorized.empty() && !askedAuth) {
                askedAuth = true;
                setState(State::Connecting, L"請在手機上允許 USB 偵錯（勾選「一律允許這部電腦」再按「允許」）");
            }
            if (unauthorized.empty() && other.empty() && waited >= T.usbFindMs)
                return setState(State::Idle, L"沒有找到用 USB 連接的手機（請確認傳輸線已接上，並在手機的開發人員選項開啟「USB 偵錯」）");
            if (waited >= T.usbAuthWaitMs)
                return setState(State::Error, !unauthorized.empty()
                                                   ? L"手機還沒有允許 USB 偵錯，請在手機上按「允許」後再試一次"
                                                   : L"USB 連接的手機沒有回應（請拔掉傳輸線重新接上）");
            std::this_thread::sleep_for(std::chrono::milliseconds(bo.next()));
        }
    }

    // ---- reconnect after a drop (exponential backoff) ----

    void startChain(const std::string& ser) {
        if (T.reconnectAttempts <= 0 || ser.empty()) return;
        auto req = std::make_shared<Request>(false);
        {
            std::lock_guard<std::mutex> lk(qmu);
            if (current) current->cancel = true;
            current = req;
        }
        chainPending = true;
        auto bo = std::make_shared<android::Backoff>(T.reconnectBaseMs, T.reconnectCapMs);
        const int delay = bo->next();
        logf("reconnect " + ser + ": attempt 1 in " + std::to_string(delay) + " ms");
        postAfter(delay, [this, req, ser, bo] { chainAttempt(req, ser, bo); });
    }

    void chainAttempt(std::shared_ptr<Request> req, std::string ser, std::shared_ptr<android::Backoff> bo) {
        if (req->cancel) {
            chainPending = false;
            logf("reconnect " + ser + " cancelled");
            return;
        }
        ++busy;
        BusyGuard g{busy};
        const int n = bo->attempts();
        setState(State::Connecting, L"手機連線中斷，正在重新連線（第 " + std::to_wstring(n) + L" 次）…");
        if (ensureServer() && reconnectOnce(ser, &req->cancel)) {
            chainPending = false;
            return;
        }
        if (req->cancel) {
            chainPending = false;
            return;
        }
        if (n >= T.reconnectAttempts) {
            chainPending = false;
            logf("reconnect " + ser + ": gave up after " + std::to_string(n) + " attempts");
            setState(State::Idle, kTxtReconnectFail);
            return;
        }
        const int delay = bo->next();
        logf("reconnect " + ser + ": attempt " + std::to_string(n + 1) + " in " + std::to_string(delay) + " ms");
        postAfter(delay, [this, req, ser, bo] { chainAttempt(req, ser, bo); });
    }

    bool reconnectOnce(const std::string& ser, const std::atomic<bool>* cancel) {
        if (probeAlive(ser)) {  // the transport survived (scrcpy alone died)
            connected(ser);
            return true;
        }
        if (isUsbSerial(ser)) return false;  // cable: wait for it to come back
        adb::MdnsService s;
        if (!adb::splitHostPort(ser, s.host, s.port)) return false;
        adbRun({"disconnect", ser}, 5000);  // drop the dead transport first
        if (connectTo(s, cancel)) return true;
        // 無線偵錯 toggled / phone rebooted: same IP, new port (mDNS).
        return !(cancel && *cancel) && browseAndConnect(T.rediscoverMs, s.host, cancel);
    }

    // ---- server output ----

    void onServerLine(uint64_t gen, const std::string& l) {
        logf("[server] " + l);
        int kind = 0;
        std::wstring text = ai::explainServerLine(l, &kind);
        if (!kind) return;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (gen != sessionGen) return;  // an older server
        }
        {
            std::lock_guard<std::mutex> lk(slMu);
            if (kind != 2) {
                serverErrorKind = kind;
                serverErrorText = text;
            }
        }
        if (kind == 2 || kind == 3) notice(text);  // mirroring goes on
    }

    // ---- mirroring ----

    // Starts scrcpy-server for the connected phone.  restart = the previous
    // session ended / stalled: same sinks, no state change on success.
    void taskStart(std::shared_ptr<std::atomic<bool>> cancel, VideoSink* v, AudioSink* a, bool restart) {
        std::string ser;
        std::wstring name;
        uint64_t gen;
        {
            std::lock_guard<std::mutex> lk(mu);
            ser = serial;
            name = deviceName;
            gen = ++sessionGen;
            if (restart) {
                v = video;
                a = audio;
            }
        }
        {
            std::lock_guard<std::mutex> lk(slMu);
            serverErrorKind = 0;
            serverErrorText.clear();
            if (!restart) noticesSent.clear();
        }
        auto cancelled = [&] { return cancel->load(); };
        // Cancelled by stop() (an iPhone took over during start-up): never
        // leave Connecting behind, auto-reconnect only runs from Idle / Error.
        // Cancelled by a newer start(): that one sets its own state.
        auto cancelledIdle = [&] {
            bool idle;
            {
                std::lock_guard<std::mutex> lk(mu);
                idle = startCancel == cancel && state == State::Connecting;
                if (startCancel == cancel && phase == Phase::Starting) phase = Phase::None;
            }
            if (idle) setState(State::Idle, L"已停止投影");
        };
        if (cancelled()) return cancelledIdle();  // stopped while queued
        if (!ensureServer()) return startFailed(restart, ser, L"無法啟動 adb");
        if (!restart) setState(State::Connecting, L"正在啟動投影：" + name);
        else logf("restarting scrcpy-server on " + ser);
        auto r = adbRun({"-s", ser, "push", adb::narrow(serverPath), kDeviceJar}, 60000, cancel.get());
        if (cancelled()) return cancelledIdle();
        if (!r.ok()) return startFailed(restart, ser, L"無法傳送 scrcpy-server 到手機（連線中斷？）");

        uint32_t scid = 0;
        BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&scid), sizeof(scid), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        scid &= 0x7fffffff;
        char sock[32];
        snprintf(sock, sizeof(sock), "scrcpy_%08x", scid);
        r = adbRun({"-s", ser, "forward", "tcp:0", std::string("localabstract:") + sock}, 10000, cancel.get());
        uint16_t port = uint16_t(atoi(trim(r.output).c_str()));
        if (cancelled()) {
            if (r.ok() && port) adb.run({"-s", ser, "forward", "--remove", std::string("tcp:") + std::to_string(port)}, 5000);
            return cancelledIdle();
        }
        if (!r.ok() || port == 0) return startFailed(restart, ser, L"adb forward 失敗");

        char scidHex[16];
        snprintf(scidHex, sizeof(scidHex), "%08x", scid);
        ai::ServerOptions so;
        so.scid = scidHex;
        so.clipboardAutosync = true;  // filtered here: setClipboardSync works at once both ways
        so.stayAwake = stayAwake;
        std::vector<std::string> args = {"-s", ser, "shell", std::string("CLASSPATH=") + kDeviceJar, "app_process", "/",
                                         "com.genymobile.scrcpy.Server", scrcpy::kServerVersion};
        for (auto& s : ai::serverArgs(so)) args.push_back(s);
        auto proc = adb.spawn(args, [this, gen](const std::string& l) { onServerLine(gen, l); });
        if (!proc) return startFailed(restart, ser, L"無法啟動 scrcpy-server");

        auto sess = std::make_shared<scrcpy::Session>();
        sess->log = [this](const std::string& s) { logf(s); };
        scrcpy::Session::Config cfg;
        cfg.port = port;
        std::string err;
        adb::Process* pp = proc.get();
        bool ok = sess->connect(cfg, [&] { return !cancelled() && pp->running(); }, &err);
        if (!ok || cancelled()) {
            const bool died = !pp->running();
            proc->kill();
            adb.run({"-s", ser, "forward", "--remove", std::string("tcp:") + std::to_string(port)}, 5000);
            if (cancelled()) return cancelledIdle();
            // Say why: the server's own error when it exited, not "cancelled".
            std::wstring why;
            {
                std::lock_guard<std::mutex> lk(slMu);
                why = serverErrorText;
            }
            if (why.empty())
                why = died ? L"scrcpy-server 意外結束（結束代碼 " + std::to_wstring(pp->exitCode()) + L"）" : adb::widen(err);
            return startFailed(restart, ser, L"投影啟動失敗：" + why);
        }
        std::weak_ptr<scrcpy::Session> weak = sess;
        sess->onEnded = [this, weak] {
            post([this, weak] { onSessionEnded(weak.lock(), nullptr); });
        };
        sess->onVideoSize = [this](int w, int h) {
            std::lock_guard<std::mutex> lk(mu);
            translator.setVideoSize(w, h);
        };
        sess->onClipboard = [this](const std::string& t) { onDeviceClipboard(t); };
        sess->onAudioUnavailable = [this](bool error) {
            notice(error ? L"手機的聲音無法擷取，這次投影沒有聲音" : L"這支手機的 Android 版本低於 11，投影沒有聲音");
        };
        bool aborted = false;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (cancelled() || (restart && phase != Phase::Restarting)) {
                aborted = true;
            } else {
                session = sess;
                serverProc = std::move(proc);
                forwardPort = port;
                video = v;
                audio = a;
                phase = Phase::Running;
                resetSent = false;
                runningSince = nowMs();
                if (!restart) {
                    inputSeen = false;
                    captureRetried = false;
                    restartBo.reset();
                }
                translator = scrcpy::InputTranslator([this](std::vector<uint8_t> m) {
                    if (session) session->sendControl(std::move(m));
                });
                sess->run(v, a);  // under mu: a concurrent stop() tears down a running session
                screenOffApplied = !screenOnWanted;
                if (screenOffApplied) sess->sendControl(scrcpy::msgSetDisplayPower(false));
            }
        }
        if (aborted) {
            sess->stop();
            proc->kill();
            adb.run({"-s", ser, "forward", "--remove", std::string("tcp:") + std::to_string(port)}, 5000);
            return cancelledIdle();
        }
        {
            std::lock_guard<std::mutex> lk(clipMu);
            if (clip) clipSeqSeen = clip->sequence();  // only later PC copies are sent
        }
        postAfter(T.monitorMs, [this, gen] { monitor(gen); });
        std::wstring dn = sess->deviceName().empty() ? name : adb::widen(sess->deviceName());
        {
            std::lock_guard<std::mutex> lk(mu);
            if (session != sess) return;  // stopped meanwhile
        }
        if (restart) logf("mirroring resumed after a scrcpy-server restart");
        else setState(State::Mirroring, L"正在投影：" + dn);
    }

    void startFailed(bool restart, const std::string& ser, const std::wstring& why) {
        if (!restart) {
            {
                std::lock_guard<std::mutex> lk(mu);
                if (phase == Phase::Starting) phase = Phase::None;
            }
            return setState(State::Error, why);
        }
        logf("scrcpy-server restart failed: " + adb::narrow(why));
        recover(ser, why);
    }

    // Takes the session out (under mu) and tears it down on this thread.
    // graceful (stop()): the phone's panel is switched back on first.
    void teardown(bool graceful) {
        std::shared_ptr<scrcpy::Session> s;
        std::unique_ptr<adb::Process> p;
        uint16_t port;
        std::string ser;
        VideoSink* v;
        AudioSink* a;
        bool hadPicture, screenOff;
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
            hadPicture = s != nullptr || phase == Phase::Restarting;
            phase = Phase::None;
            ++sessionGen;  // the monitor and pending restarts of it stop
            screenOff = screenOffApplied;
            screenOffApplied = false;
            translator.setVideoSize(0, 0);
        }
        if (s && graceful && screenOff) {
            s->sendControl(scrcpy::msgSetDisplayPower(true));
            if (!s->drainControl(500)) logf("screen-on message not confirmed sent");
            else logf("phone screen switched back on");
        }
        if (s) s->stop();
        if (p && !p->wait(1500)) p->kill();  // the server exits when its sockets close
        if (port) {
            std::string spec = "tcp:" + std::to_string(port);
            post([this, ser, spec] { adb.run({"-s", ser, "forward", "--remove", spec}, 5000); });
        }
        if (hadPicture) {
            if (v) v->onReset();
            if (a) a->onFlush();
        }
    }

    // The session ended (stream EOF / error) or stalled (`stall` set).
    void onSessionEnded(std::shared_ptr<scrcpy::Session> s, const char* stall) {
        std::unique_ptr<adb::Process> p;
        uint16_t port = 0;
        std::string ser;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!s || s != session) return;  // already stopped / replaced
            session = nullptr;
            p = std::move(serverProc);
            port = forwardPort;
            forwardPort = 0;
            ser = serial;
            phase = Phase::Restarting;
            ++sessionGen;
            screenOffApplied = false;
            translator.setVideoSize(0, 0);
            if (nowMs() - runningSince > T.restartForgetMs) {  // it ran fine for a while
                restartBo.reset();
                captureRetried = false;
            }
            if (!startCancel || *startCancel) startCancel = std::make_shared<std::atomic<bool>>(false);
        }
        const scrcpy::Session::End end = s->endReason();
        s->stop();
        if (p && !p->wait(stall ? 0 : 500)) p->kill();
        if (port) adb.run({"-s", ser, "forward", "--remove", "tcp:" + std::to_string(port)}, 5000);
        int kind;
        std::wstring serverText;
        {
            std::lock_guard<std::mutex> lk(slMu);
            kind = serverErrorKind;
            serverText = serverErrorText;
        }
        const bool capture = end == scrcpy::Session::End::CaptureError || kind == 1;
        logf(std::string("session ended: ") +
             (stall ? stall
                    : capture ? "capture error on the phone"
                    : end == scrcpy::Session::End::StreamError  ? "stream error"
                                                                : "stream closed"));
        if (capture) {
            bool again;
            {
                std::lock_guard<std::mutex> lk(mu);
                again = captureRetried;
                captureRetried = true;
            }
            if (again) return giveUp(serverText.empty() || kind != 1 ? std::wstring(kTxtCaptureFail) : serverText);
        }
        recover(ser, capture ? std::wstring(kTxtCaptureFail) : serverText);
    }

    // Between two sessions (phase Restarting): restart scrcpy-server if the
    // phone still answers, else the connection is lost.
    void recover(const std::string& ser, const std::wstring& why) {
        std::shared_ptr<std::atomic<bool>> cancel;
        int delay = -1;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (phase != Phase::Restarting) return;  // stop() meanwhile
            cancel = startCancel;
        }
        if (!probeAlive(ser)) return lost(ser);
        {
            std::lock_guard<std::mutex> lk(mu);
            if (phase != Phase::Restarting || cancel != startCancel) return;
            if (restartBo.attempts() < T.restartAttempts) delay = restartBo.next();
        }
        if (delay < 0) return giveUp(why.empty() ? std::wstring(kTxtLost) : why);
        logf("scrcpy-server restart in " + std::to_string(delay) + " ms");
        postAfter(delay, [this, cancel] {
            {
                std::lock_guard<std::mutex> lk(mu);
                if (phase != Phase::Restarting || *cancel) return;
            }
            taskStart(cancel, nullptr, nullptr, true);
        });
    }

    // Mirroring cannot continue (the phone is reachable): say why, no reconnect.
    void giveUp(const std::wstring& why) {
        teardown(false);
        setState(State::Error, why);
        if (self->events.onDisconnected) self->events.onDisconnected();
    }

    // The phone is gone: back to Idle, then reconnect with backoff.
    void lost(const std::string& ser) {
        teardown(false);
        setState(State::Idle, kTxtLost);
        if (self->events.onDisconnected) self->events.onDisconnected();
        startChain(ser);
    }

    // Every T.monitorMs while a session runs (worker thread).
    void monitor(uint64_t gen) {
        std::shared_ptr<scrcpy::Session> s;
        bool sendReset = false;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!session || gen != sessionGen || phase != Phase::Running) return;
            s = session;
            const int64_t last = s->lastVideoMs(), now = nowMs();
            const int64_t quiet = now - (last ? last : s->startedMs());
            if (last && quiet >= T.stallResetMs && !resetSent) sendReset = resetSent = true;
            if (last && quiet < T.stallResetMs) resetSent = false;
            if (quiet >= (last ? T.stallMs : T.firstFrameMs)) {
                s = nullptr;  // stalled: handled below
            }
        }
        if (!s) {
            std::shared_ptr<scrcpy::Session> cur;
            {
                std::lock_guard<std::mutex> lk(mu);
                cur = session;
            }
            logf("no video from the phone: stream stalled");
            return onSessionEnded(cur, "stalled (no video)");
        }
        if (sendReset) {
            logf("no video for " + std::to_string(T.stallResetMs) + " ms: RESET_VIDEO");
            s->sendControl(scrcpy::msgResetVideo());
        }
        pollPcClipboard(s);
        postAfter(T.monitorMs, [this, gen] { monitor(gen); });
    }

    // ---- clipboard ----

    void onDeviceClipboard(const std::string& text) {
        if (!clipboardSync) return;
        std::lock_guard<std::mutex> lk(clipMu);
        if (!clip || text == clipLast) return;
        clipLast = text;
        const uint32_t seq = clip->setText(text);
        if (seq) clipSeqOurs = clipSeqSeen = seq;
        logf("clipboard phone → PC (" + std::to_string(text.size()) + " bytes)" + (seq ? "" : " FAILED"));
    }

    // PC → phone when the PC clipboard changed, only once the user controls
    // the phone (only watching: nothing is sent).
    void pollPcClipboard(const std::shared_ptr<scrcpy::Session>& s) {
        if (!clipboardSync) return;
        bool controlling;
        {
            std::lock_guard<std::mutex> lk(mu);
            controlling = inputSeen;
        }
        std::string text;
        {
            std::lock_guard<std::mutex> lk(clipMu);
            if (!clip) return;
            const uint32_t seq = clip->sequence();
            if (seq == clipSeqSeen) return;
            if (!controlling) return;  // looked at again once the user takes control
            clipSeqSeen = seq;
            if (seq == clipSeqOurs || !clip->getText(text) || text == clipLast) return;
            clipLast = text;
            ++clipMsgSeq;
        }
        s->sendControl(scrcpy::msgSetClipboard(0, false, text));
        logf("clipboard PC → phone (" + std::to_string(text.size()) + " bytes)");
    }

    // Ctrl+V on the mirror: the PC clipboard, pasted (true = consumed).
    bool pasteToPhone() {
        std::string text;
        {
            std::lock_guard<std::mutex> lk(clipMu);
            if (!clip || !clip->getText(text)) return false;
            clipLast = text;
            clipSeqSeen = clip->sequence();
            ++clipMsgSeq;
        }
        std::lock_guard<std::mutex> lk(mu);
        if (!session) return false;
        session->sendControl(scrcpy::msgSetClipboard(0, true, text));
        logf("clipboard PC → phone + paste (" + std::to_string(text.size()) + " bytes)");
        return true;
    }

    // ---- 傳到手機 (gallery) ----

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

    // ---- pushFile / installApk ----

    void enqueueTransfer(Transfer t) {
        {
            std::lock_guard<std::mutex> lk(tmu);
            if (tquit) {
                // Destroying: still exactly one callback, from a thread of ours.
                std::thread([t = std::move(t)] {
                    if (t.done) t.done(false, kTxtCancelled);
                }).detach();
                return;
            }
            transfers.push_back(std::move(t));
            if (!transferThread.joinable()) transferThread = std::thread([this] { transferLoop(); });
        }
        tcv.notify_one();
    }

    void transferLoop() {
        for (;;) {
            Transfer t;
            {
                std::unique_lock<std::mutex> lk(tmu);
                tcv.wait(lk, [&] { return tquit || !transfers.empty(); });
                if (transfers.empty()) return;  // tquit
                if (tquit) {
                    // Destroying: everything left is cancelled, still from this thread.
                    std::deque<Transfer> left;
                    left.swap(transfers);
                    lk.unlock();
                    for (auto& x : left)
                        if (x.done) x.done(false, kTxtCancelled);
                    return;
                }
                t = std::move(transfers.front());
                transfers.pop_front();
                transferBusy = true;
            }
            bool ok = false;
            std::wstring msg;
            if (transferCancel) msg = kTxtCancelled;
            else if (t.install) ok = runInstall(t.path, msg);
            else ok = runPushFile(t.path, msg);
            transferBusy = false;
            lastActivity = nowMs();
            post([] {});  // the worker re-arms the adb idle check
            if (t.done) t.done(ok, msg);
        }
    }

    std::string connectedSerial() {
        std::lock_guard<std::mutex> lk(mu);
        return serial;
    }

    static uint64_t fileSize(const std::wstring& path, bool* isDir) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
            if (isDir) *isDir = false;
            return ~0ull;
        }
        if (isDir) *isDir = (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        return (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    }

    bool runPushFile(const std::wstring& local, std::wstring& msg) {
        const std::string ser = connectedSerial();
        if (ser.empty() || !serverStarted) return msg = kTxtNoPhoneFile, false;
        bool dir = false;
        const uint64_t bytes = fileSize(local, &dir);
        if (bytes == ~0ull) return msg = L"找不到要傳送的檔案", false;
        if (dir) return msg = L"只能傳送檔案，不能傳送資料夾", false;
        // A free name in Download ("name (1).ext" …).
        const std::string base = ai::downloadName(local);
        std::string name = base, remote;
        for (int n = 1; n < 100; ++n) {
            remote = kDownloadDir + name;
            auto r = adb.run({"-s", ser, "shell", "ls -d " + ai::shellQuote(remote) + " 2>/dev/null"}, 10000, &transferCancel);
            if (r.exitCode == -2 || transferCancel) return msg = kTxtCancelled, false;
            if (trim(r.output).empty() || !r.ok()) break;
            name = ai::numberedName(base, n);
        }
        logf("pushFile " + adb::narrow(local) + " (" + std::to_string(bytes / 1024) + " KB) → " + ser + ":" + remote);
        // Progress: the phone-side size, polled while adb push runs.
        std::atomic<bool> pushing{true};
        std::thread progress([&] {
            int lastPct = -1;
            uint64_t lastBytes = ~0ull;
            while (pushing) {
                for (int i = 0; i < T.transferPollMs / 50 && pushing; ++i) std::this_thread::sleep_for(50ms);
                if (!pushing) break;
                auto r = adb.run({"-s", ser, "shell", "stat -c %s " + ai::shellQuote(remote) + " 2>/dev/null"}, 5000,
                                 &transferCancel);
                const std::string v = trim(r.output);
                if (!r.ok() || v.empty() || v.find_first_not_of("0123456789") != std::string::npos) continue;
                const uint64_t got = std::stoull(v);
                const int pct = bytes ? int(std::min<uint64_t>(100, got * 100 / bytes)) : 100;
                if (pct != lastPct || got != lastBytes) {
                    logf("pushFile progress " + std::to_string(pct) + "% (" + std::to_string(got / 1024) + " / " +
                         std::to_string(bytes / 1024) + " KB)");
                    lastPct = pct;
                    lastBytes = got;
                }
            }
        });
        const auto t0 = Clock::now();
        const int timeout = int(std::min<uint64_t>(60000 + bytes / 256, 30ull * 60 * 1000));
        auto r = adbRun({"-s", ser, "push", adb::narrow(local), remote}, timeout, &transferCancel);
        pushing = false;
        progress.join();
        const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
        if (transferCancel) return msg = kTxtCancelled, false;
        if (!r.ok()) {
            msg = ai::explainPushFailure(r.output, r.exitCode);
            logf("pushFile failed after " + std::to_string(int(secs)) + " s: " + adb::narrow(msg));
            return false;
        }
        char rate[64];
        snprintf(rate, sizeof(rate), "%.1f s, %.2f MB/s", secs, secs > 0 ? double(bytes) / 1048576.0 / secs : 0.0);
        logf("pushFile progress 100% (" + std::to_string(bytes / 1024) + " KB) done in " + rate);
        // Files app / MediaStore see it at once (non-fatal).
        adbRun({"-s", ser, "shell", "content call --uri content://media --method scan_file --arg " +
                                         ai::shellQuote("/storage/emulated/0/Download/" + name)},
               15000, &transferCancel);
        msg = L"已傳送到手機的 Download 資料夾：" + adb::widen(name);
        return true;
    }

    bool runInstall(const std::wstring& local, std::wstring& msg) {
        std::wstring ext;
        if (const size_t dot = local.rfind(L'.'); dot != std::wstring::npos) ext = local.substr(dot);
        for (wchar_t& c : ext) c = towlower(c);
        if (ext == L".apks" || ext == L".xapk" || ext == L".apkm" || ext == L".aab")
            return msg = L"這是分割安裝包（.apks／.xapk／.aab），請改用單一的 .apk 檔", false;
        if (ext != L".apk") return msg = L"只能安裝 .apk 檔", false;
        const std::string ser = connectedSerial();
        if (ser.empty() || !serverStarted) return msg = kTxtNoPhoneApk, false;
        bool dir = false;
        const uint64_t bytes = fileSize(local, &dir);
        if (bytes == ~0ull || dir) return msg = L"找不到要安裝的 APK 檔", false;
        const size_t slash = local.find_last_of(L"\\/");
        const std::wstring fname = slash == std::wstring::npos ? local : local.substr(slash + 1);
        logf("installApk " + adb::narrow(local) + " (" + std::to_string(bytes / 1024) + " KB) on " + ser);
        // Streamed install over Wi-Fi + verification + the phone's own
        // confirmation dialog on some brands: generous timeout.
        const int timeout = int(std::min<uint64_t>(180000 + bytes / 256, 30ull * 60 * 1000));
        const auto t0 = Clock::now();
        auto r = adbRun({"-s", ser, "install", "-r", adb::narrow(local)}, timeout, &transferCancel);
        const int secs = int(std::chrono::duration<double>(Clock::now() - t0).count());
        if (transferCancel) return msg = kTxtCancelled, false;
        if (r.ok() && r.output.find("Success") != std::string::npos) {
            logf("installApk done in " + std::to_string(secs) + " s");
            msg = L"已安裝到手機：" + fname;
            return true;
        }
        msg = ai::explainInstallFailure(r.output, r.exitCode);
        logf("installApk failed after " + std::to_string(secs) + " s: " + adb::narrow(msg));
        return false;
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
    if (ser.empty() || !d_->serverStarted || d_->pushBusy.exchange(true)) return false;
    if (d_->pushThread.joinable()) d_->pushThread.join();  // the previous one has finished (pushBusy was false)
    d_->pushCancel = false;
    d_->pushThread = std::thread([this, path, ser, done = std::move(done)]() mutable {
        d_->taskPush(path, ser, std::move(done));
        d_->lastActivity = nowMs();
        d_->post([] {});  // the worker re-arms the adb idle check
    });
    return true;
}

AndroidSource::AndroidSource() : d_(std::make_unique<Impl>()) {
    d_->self = this;
    d_->restartBo = android::Backoff(d_->T.restartBaseMs, d_->T.restartCapMs);
    d_->worker = std::thread([this] { d_->loop(); });
}

AndroidSource::~AndroidSource() {
    {
        std::lock_guard<std::mutex> lk(d_->qmu);
        if (d_->current) d_->current->cancel = true;
    }
    d_->pushCancel = true;
    if (d_->pushThread.joinable()) d_->pushThread.join();
    {
        std::lock_guard<std::mutex> lk(d_->tmu);
        d_->tquit = true;
    }
    d_->transferCancel = true;  // the running transfer stops; queued ones are cancelled
    d_->tcv.notify_all();
    if (d_->transferThread.joinable()) d_->transferThread.join();
    stop();
    {
        std::lock_guard<std::mutex> lk(d_->qmu);
        d_->quit = true;
        d_->tasks.clear();
        d_->timers.clear();
    }
    d_->qcv.notify_all();
    if (d_->worker.joinable()) d_->worker.join();
    if (d_->serverStarted) d_->adb.killServer();
    std::lock_guard<std::mutex> lk(d_->clipMu);
    d_->clip.reset();
}

bool AndroidSource::init(const std::wstring& toolsDir, bool warmUp) {
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
    {
        std::lock_guard<std::mutex> lk(d_->clipMu);
        if (!d_->clip) d_->clip = d_->T.clipboardFactory ? d_->T.clipboardFactory() : clip::makeWindowsBackend();
    }
    d_->inited = true;
    if (warmUp) this->warmUp();
    return true;
}

void AndroidSource::warmUp() {
    if (!d_->inited || d_->serverStarted) return;
    d_->post([this] { d_->ensureServer(); });  // ~1 s in the background; stopped again when idle
}

bool AndroidSource::beginQrPairing(std::vector<uint8_t>& bgra, int& size, std::wstring& qrText) {
    if (!d_->inited) return false;
    cancelPairing();
    auto cred = pairing::randomCredentials();
    if (!pairing::renderQr(cred.qrText(), 8, 4, bgra, size)) return false;
    qrText = adb::widen(cred.qrText());
    d_->chainPending = false;
    d_->postRequest(true, [this, cred](Impl::Request& req) { d_->taskPairQr(cred, req); });
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
    d_->chainPending = false;
    d_->postRequest(true, [this, hp, c](Impl::Request& req) { d_->taskPairCode(hp, c, req); });
    return true;
}

// Cancels the pairing request whether it is still queued, listening for the
// phone or running `adb pair`; once paired, the connect is left to finish.
void AndroidSource::cancelPairing() {
    std::lock_guard<std::mutex> lk(d_->qmu);
    if (d_->current && d_->current->pairing && !d_->current->paired) d_->current->cancel = true;
}

bool AndroidSource::connectKnownDevices() {
    if (!d_->inited || d_->busy > 0) return false;
    d_->chainPending = false;  // replaced (postRequest cancels it)
    d_->postRequest(false, [this](Impl::Request& req) { d_->taskReconnect(req); });
    return true;
}

bool AndroidSource::connectUsb() {
    if (!d_->inited || d_->busy > 0) return false;
    d_->chainPending = false;
    d_->postRequest(false, [this](Impl::Request& req) { d_->taskUsb(req); });
    return true;
}

bool AndroidSource::start(VideoSink* video, AudioSink* audio) {
    if (!d_->inited) return false;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        if (d_->serial.empty() || d_->session || d_->phase == Impl::Phase::Restarting) return false;
        if (d_->startCancel) *d_->startCancel = true;
        d_->startCancel = cancel;
        d_->phase = Impl::Phase::Starting;
    }
    d_->post([this, cancel, video, audio] { d_->taskStart(cancel, video, audio, false); });
    return true;
}

void AndroidSource::stop() {
    bool idle;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        if (d_->startCancel) *d_->startCancel = true;
        // A session, or a phone connected / starting without one yet: Idle,
        // so auto-reconnect (Idle / Error only) still runs later.
        idle = d_->session != nullptr || d_->phase != Impl::Phase::None || d_->state == State::Connecting;
    }
    {
        // A reconnect after a drop stops too (pairing requests are left alone).
        std::lock_guard<std::mutex> lk(d_->qmu);
        if (d_->current && !d_->current->pairing) d_->current->cancel = true;
    }
    d_->teardown(true);
    if (idle) d_->setState(State::Idle, L"已停止投影");
}

void AndroidSource::sendPointer(const VideoWindow::PointerEvent& e) {
    std::lock_guard<std::mutex> lk(d_->mu);
    if (!d_->session) return;
    if (e.kind != VideoWindow::PointerEvent::Kind::Move) d_->inputSeen = true;
    d_->translator.pointer(e);
}

void AndroidSource::sendKey(unsigned vk, bool down, wchar_t ch) {
    // Clipboard shortcuts (scrcpy's way): Ctrl+V = SET_CLIPBOARD + paste on
    // the phone, Ctrl+C / Ctrl+X = GET_CLIPBOARD with COPY / CUT (the text
    // comes back as a device clipboard message).
    if (down && d_->clipboardSync && (vk == 'V' || vk == 'C' || vk == 'X') && GetKeyState(VK_CONTROL) < 0 &&
        GetKeyState(VK_MENU) >= 0 && GetKeyState(VK_SHIFT) >= 0) {
        bool live;
        {
            std::lock_guard<std::mutex> lk(d_->mu);
            live = d_->session != nullptr;
            if (live) d_->inputSeen = true;
        }
        if (!live) return;
        if (vk == 'V') {
            if (d_->pasteToPhone()) return;
        } else {
            std::lock_guard<std::mutex> lk(d_->mu);
            if (d_->session) {
                d_->session->sendControl(scrcpy::msgGetClipboard(vk == 'C' ? scrcpy::kCopyKeyCopy : scrcpy::kCopyKeyCut));
                return;
            }
        }
    }
    std::lock_guard<std::mutex> lk(d_->mu);
    if (!d_->session) return;
    if (down) d_->inputSeen = true;
    d_->translator.key(vk, down, ch);
}

static void pressKey(AndroidSource::Impl* d, uint32_t keycode) {
    std::lock_guard<std::mutex> lk(d->mu);
    if (!d->session) return;
    d->inputSeen = true;
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

// ---- 0.8.3 features ----

void AndroidSource::setClipboardSync(bool on) {
    if (d_->clipboardSync.exchange(on) == on) return;
    d_->logf(std::string("clipboard sync ") + (on ? "on" : "off"));
    if (on) {
        std::lock_guard<std::mutex> lk(d_->clipMu);
        if (d_->clip) d_->clipSeqSeen = d_->clip->sequence();  // from now on
    }
}

void AndroidSource::pushFile(const std::wstring& path, std::function<void(bool ok, std::wstring msg)> done) {
    Impl::Transfer t;
    t.install = false;
    t.path = path;
    t.done = std::move(done);
    d_->enqueueTransfer(std::move(t));
}

void AndroidSource::installApk(const std::wstring& path, std::function<void(bool ok, std::wstring msg)> done) {
    Impl::Transfer t;
    t.install = true;
    t.path = path;
    t.done = std::move(done);
    d_->enqueueTransfer(std::move(t));
}

void AndroidSource::setPhoneScreenOn(bool on) {
    d_->screenOnWanted = on;
    std::lock_guard<std::mutex> lk(d_->mu);
    if (!d_->session) return;  // applied when mirroring starts
    if (d_->screenOffApplied == !on) return;
    d_->screenOffApplied = !on;
    d_->session->sendControl(scrcpy::msgSetDisplayPower(on));
    d_->logf(std::string("phone screen ") + (on ? "on" : "off") + " (mirroring continues)");
}

void AndroidSource::setStayAwake(bool on) {
    if (d_->stayAwake.exchange(on) == on) return;
    d_->logf(std::string("stay awake ") + (on ? "on" : "off"));
    // Server options: a running scrcpy-server is restarted in place.
    std::shared_ptr<scrcpy::Session> s;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        if (d_->phase != Impl::Phase::Running) return;
        s = d_->session;
        d_->restartBo.reset();
    }
    d_->post([this, s] { d_->onSessionEnded(s, "stay-awake option changed"); });
}

}  // namespace pm
