// PhoneMirror: pm::AirPlayServer, a thin C++ facade over the UxPlay protocol
// library. The callback wiring mirrors _ref/uxplay/uxplay.cpp (start_dnssd,
// start_raop_server, conn_*/video_*/audio_* callbacks, feedback watchdog and
// httpd relaunch on connection loss, "-pin" + "-reg" client access control),
// minus GStreamer/HLS/password.
//
// Copyright (C) 2026 PhoneMirror contributors. Derived from UxPlay
// (Copyright (C) F. Duncanh and contributors). GPL-3.0-or-later.

#include "pm/airplay_server.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

extern "C" {
#include "dnssd.h"
#include "logger.h"
#include "raop.h"
extern int pm_mirror_quick_ack;  // raop_rtp_mirror.c
}

namespace pm {

namespace {

constexpr int kDefaultWidth = 1920, kDefaultHeight = 1080;
constexpr int kDefaultWidth265 = 3840, kDefaultHeight265 = 2160;

std::string findMac() {
    // Same selection rule as uxplay.cpp find_mac(): first adapter that is up,
    // Ethernet (6) or IEEE 802.11 (71), with a 6-octet physical address.
    std::string mac;
    ULONG buflen = 16 * 1024;
    std::vector<unsigned char> buf(buflen);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG rc = GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, addrs, &buflen);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(buflen);
        addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        rc = GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, addrs, &buflen);
    }
    if (rc != NO_ERROR) return mac;
    for (auto* a = addrs; a; a = a->Next) {
        if (a->PhysicalAddressLength != 6 || (a->IfType != 6 && a->IfType != 71) ||
            a->OperStatus != IfOperStatusUp) {
            continue;
        }
        char s[18];
        std::snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x", a->PhysicalAddress[0],
                      a->PhysicalAddress[1], a->PhysicalAddress[2], a->PhysicalAddress[3],
                      a->PhysicalAddress[4], a->PhysicalAddress[5]);
        mac = s;
        break;
    }
    return mac;
}

std::string randomMac() {
    std::random_device rd;
    unsigned char b[6];
    for (auto& x : b) x = static_cast<unsigned char>(rd());
    b[0] = static_cast<unsigned char>((b[0] & 0xfc) | 0x02);  // unicast, locally administered
    char s[18];
    std::snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
    return s;
}

int64_t steadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// avg/min/max accumulator (milliseconds)
struct Acc {
    double sum = 0, mn = 0, mx = 0;
    unsigned n = 0;
    void add(double v) {
        if (!n || v < mn) mn = v;
        if (!n || v > mx) mx = v;
        sum += v;
        ++n;
    }
    double avg() const { return n ? sum / n : 0; }
};

// Per-frame receive-path timing of the mirror stream, summarised every 5 s.
// All times are wall-clock ns (raop_ntp_get_local_time / localTimeNs).
struct VideoTiming {
    uint64_t windowStart = 0, lastHeader = 0;
    unsigned frames = 0;
    uint64_t bytes = 0;
    int maxBytes = 0;
    Acc lead;      // pts mapped to local by NTP (ntp_time_local) - header arrival
    Acc leadRel;   // pts mapped with the latched first-packet offset - header arrival
    Acc recv;      // header -> payload complete
    Acc decrypt;   // payload complete -> decrypted/Annex-B
    Acc sink;      // time spent inside VideoSink::onFrame
    Acc interval;  // header arrival - previous header arrival
    unsigned noNtp = 0;
};

bool parseMac(const std::string& str, std::vector<char>& out) {
    out.clear();
    unsigned v[6];
    if (std::sscanf(str.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return false;
    for (unsigned x : v) out.push_back(static_cast<char>(x));
    return true;
}

}  // namespace

struct AirPlayServer::Impl {
    // Winsock must be up before dnssd_init(): mdnsd's gethostname() fails
    // otherwise and the SRV target silently becomes "UxPlay.local".
    bool wsaUp = false;
    Impl() {
        WSADATA wsa;
        wsaUp = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }
    ~Impl() {
        if (wsaUp) WSACleanup();
    }

    // configuration
    Options opts;
    LogCallback logCb;
    std::string name;
    std::string mac;
    VideoSink* video = nullptr;
    AudioSink* audio = nullptr;
    Events events;

    // client access control (requirePin): uxplay "-pin" + "-reg"
    std::mutex regMutex;
    std::string registerFile;               // empty = registration off (accept returning clients)
    std::vector<std::string> registeredKeys;  // base64 Ed25519 pks of clients that entered the PIN
    std::atomic<bool> pinShown{false};
    // The iPhone closes the pair-pin-start connection, shows its PIN prompt and
    // only reconnects (pair-setup-pin) after the user typed the PIN, so the PIN
    // must outlive that connection: it stays up until pairing, a new PIN or
    // kPinShowSec supervisor seconds.
    static constexpr int kPinShowSec = 90;
    std::atomic<int> pinSecondsLeft{0};
    std::atomic<bool> clientAnnounced{false};

    // multi-phone takeover: name of the admitted session's phone
    mutable std::mutex clientMutex;
    std::string currentClient;
    void setCurrentClient(const std::string& n) {
        std::lock_guard<std::mutex> lk(clientMutex);
        currentClient = n;
    }

    // audio stream start timing (ms logs for the "sound comes back late" diagnosis)
    std::atomic<int64_t> audioMarkNs{0};
    std::atomic<int> audioMarkWhat{0};  // 0 none, 1 SETUP, 2 FLUSH

    // library objects
    raop_t* raop = nullptr;
    dnssd_t* dnssd = nullptr;
    unsigned short port = 0;
    bool running = false;

    // session state (touched from network threads)
    std::atomic<int> openConnections{0};
    std::atomic<unsigned> missedFeedback{0};
    std::mutex clockMutex;
    bool haveClockOffset = false;
    bool clockFromNtp = false;      // offset taken from the NTP clock sync (else provisional, from arrival)
    int64_t remoteClockOffset = 0;  // local - remote (ns)
    bool h265 = false;              // effective (Options::h265, or forced on for height > 1080)
    std::atomic<unsigned> badVideoFrames{0};
    VideoTiming vt;  // only touched from the mirror thread
    std::atomic<float> currentVolumeDb{0.0f};  // reported back in GET /info (initialVolume)

    // supervisor thread (feedback watchdog + httpd relaunch)
    std::thread supervisor;
    std::mutex supMutex;
    std::condition_variable supCv;
    bool supQuit = false;
    bool resetHttpd = false;

    void log(LogLevel lvl, const char* fmt, ...) {
        if (!logCb) return;
        char buf[2048];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        logCb(lvl, buf);
    }

    void resetClock() {
        std::lock_guard<std::mutex> lk(clockMutex);
        haveClockOffset = false;
        clockFromNtp = false;
        remoteClockOffset = 0;
    }

    // Sender timestamp (remote clock) -> localTimeNs() clock. As in uxplay.cpp
    // ONE remote->local offset is used for audio and video of a session, so
    // the two streams share a timeline. The offset comes from the NTP clock
    // sync (raop_ntp: ntpLocal = remote mapped with the current NTP offset,
    // 0 while not synced yet): latched at the first synced packet, re-latched
    // only if the NTP estimate moves away by more than kClockRelatchNs (drift
    // or a better sync), so the timeline has no jitter from NTP updates.
    // Before the first NTP sync a provisional offset is taken from the arrival
    // time and replaced once NTP is available (both logged as "[clock]").
    static constexpr int64_t kClockRelatchNs = 20'000'000;
    uint64_t toLocal(uint64_t ntpLocal, uint64_t ntpRemote, const char* stream) {
        if (!ntpRemote) return get_local_time();  // no sender timestamp (audio before its first sync)
        std::lock_guard<std::mutex> lk(clockMutex);
        if (ntpLocal) {
            const int64_t off = static_cast<int64_t>(ntpLocal) - static_cast<int64_t>(ntpRemote);
            if (!haveClockOffset) {
                log(LogLevel::Info, "[clock] timeline offset latched from the NTP clock sync (first %s packet)", stream);
            } else if (!clockFromNtp) {
                log(LogLevel::Info, "[clock] NTP clock sync established (%s): provisional offset corrected by %+.1f ms",
                    stream, (off - remoteClockOffset) / 1e6);
            } else if (std::llabs(off - remoteClockOffset) > kClockRelatchNs) {
                log(LogLevel::Info, "[clock] NTP offset moved by %+.1f ms (clock drift / resync): timeline re-latched",
                    (off - remoteClockOffset) / 1e6);
            } else {
                return static_cast<uint64_t>(static_cast<int64_t>(ntpRemote) + remoteClockOffset);
            }
            remoteClockOffset = off;
            haveClockOffset = true;
            clockFromNtp = true;
        } else if (!haveClockOffset) {
            remoteClockOffset = static_cast<int64_t>(get_local_time()) - static_cast<int64_t>(ntpRemote);
            haveClockOffset = true;
            clockFromNtp = false;
            log(LogLevel::Info, "[clock] no NTP clock sync yet: provisional offset from the arrival of the first %s packet",
                stream);
        }
        return static_cast<uint64_t>(static_cast<int64_t>(ntpRemote) + remoteClockOffset);
    }

    void requestHttpdReset(const char* why) {
        log(LogLevel::Warning, "session reset requested: %s", why);
        {
            std::lock_guard<std::mutex> lk(supMutex);
            resetHttpd = true;
        }
        supCv.notify_all();
    }

    void supervisorLoop() {
        std::unique_lock<std::mutex> lk(supMutex);
        while (!supQuit) {
            supCv.wait_for(lk, std::chrono::seconds(1));
            if (supQuit) break;
            if (pinSecondsLeft.load() > 0 && --pinSecondsLeft == 0) {
                log(LogLevel::Info, "PIN timed out (not entered within %d s)", kPinShowSec);
                hidePin();
            }
            if (!resetHttpd) {
                // uxplay.cpp feedback_callback(), once per second
                if (openConnections.load() > 0) {
                    unsigned missed = missedFeedback.load();
                    if (opts.feedbackTimeoutSec && missed > opts.feedbackTimeoutSec) {
                        log(LogLevel::Error,
                            "lost connection with client: no feedback heartbeat for %u s (limit %u s)",
                            missed, opts.feedbackTimeoutSec);
                        resetHttpd = true;
                    } else {
                        if (missed > 2)
                            log(LogLevel::Warning,
                                "%u s since last client feedback (expected every 2 s); client may be offline",
                                missed);
                        missedFeedback++;
                    }
                } else {
                    missedFeedback = 0;
                }
            }
            if (resetHttpd) {
                resetHttpd = false;
                lk.unlock();
                relaunchHttpd();
                lk.lock();
            }
        }
    }

    // uxplay.cpp main(): "if (reset_httpd) { raop_stop_httpd; ... raop_start_httpd }"
    void relaunchHttpd() {
        if (!raop) return;
        log(LogLevel::Info, "relaunching AirPlay server (dropping current client)");
        raop_stop_httpd(raop);
        if (video) video->onReset();
        if (audio) audio->onFlush();
        resetClock();
        missedFeedback = 0;
        unsigned short p = raop_get_port(raop);
        if (raop_start_httpd(raop, &p) < 0) {
            log(LogLevel::Error, "failed to restart AirPlay server on port %u", p);
            return;
        }
        raop_set_port(raop, p);
        log(LogLevel::Info, "AirPlay server listening again on TCP port %u", p);
    }

    // ------------------------------------------------------------- display mode
    struct Display {
        int width, height, refreshRate, maxFps;
        bool h265;
    };
    Display resolveDisplay() {
        Display d{opts.width, opts.height, opts.refreshRate, opts.maxFps, opts.h265};
        if (d.width <= 0 && d.height <= 0) {  // uxplay default: 1080p, 4K with -h265
            d.width = d.h265 ? kDefaultWidth265 : kDefaultWidth;
            d.height = d.h265 ? kDefaultHeight265 : kDefaultHeight;
        } else if (d.width <= 0) {
            d.width = (d.height * 16 / 9 + 1) & ~1;
        } else if (d.height <= 0) {
            d.height = (d.width * 9 / 16 + 1) & ~1;
        }
        const Display asked = d;
        d.width = std::clamp(d.width, 320, 7680);
        d.height = std::clamp(d.height, 240, 4320);
        d.refreshRate = d.refreshRate > 0 ? std::min(d.refreshRate, 255) : 60;
        d.maxFps = d.maxFps > 0 ? std::min(d.maxFps, 255) : 60;
        if (asked.width != d.width || asked.height != d.height)
            log(LogLevel::Warning, "display size %dx%d out of range, using %dx%d", asked.width, asked.height, d.width,
                d.height);
        if (d.height > 1080 && !d.h265) {
            // iOS only mirrors above 1080p in H.265; without feature bit 42 it
            // sends a codec packet without payload ("unsupported codec").
            d.h265 = true;
            log(LogLevel::Warning, "display height %d > 1080 requires H.265: advertising H.265 (feature bit 42)",
                d.height);
        }
        return d;
    }

    // ----------------------------------------------------------- network watch
    // mDNS is advertised on every suitable IPv4 interface (mdnsd.c). Interface
    // and address changes (NotifyIpInterfaceChange / NotifyUnicastIpAddress-
    // Change, which also fire on resume from sleep and Wi-Fi reconnects) are
    // debounced for 2 s; then the interface list is re-scanned and, if it
    // changed (or an interface/address was added/removed, or refreshNetwork()
    // was called), the mDNS socket is re-opened on the new set and the
    // services are re-announced (twice, 1 s apart, RFC 6762 8.3). The RTSP
    // listener binds the wildcard address and survives interface changes; it
    // is only relaunched if it stopped running and no client is connected.
    // An active session is never dropped by this.
    std::thread netThread;
    std::mutex netMutex;
    std::condition_variable netCv;
    bool netQuit = false;
    bool netForced = false;       // refreshNetwork()
    bool netPending = false;      // OS notification(s) waiting for the debounce
    bool netSignificant = false;  // an interface or address was added/removed
    unsigned netEvents = 0;
    int64_t netLastEventNs = 0;
    HANDLE ifNotify = nullptr, addrNotify = nullptr;
    static constexpr int64_t kNetDebounceNs = 2'000'000'000;

    static std::string ifaceText(const dnssd_pm_iface_t& f) {
        char ip[INET_ADDRSTRLEN] = "?";
        in_addr a;
        a.s_addr = f.addr;
        inet_ntop(AF_INET, &a, ip, sizeof(ip));
        return std::string(f.name) + "=" + ip;
    }
    std::vector<dnssd_pm_iface_t> currentInterfaces() const {
        std::vector<dnssd_pm_iface_t> v(16);
        v.resize(dnssd ? static_cast<size_t>(dnssd_pm_get_interfaces(dnssd, v.data(), 16)) : 0);
        return v;
    }
    static std::vector<dnssd_pm_iface_t> scanInterfaces() {
        std::vector<dnssd_pm_iface_t> v(16);
        v.resize(static_cast<size_t>(dnssd_pm_scan_interfaces(v.data(), 16)));
        return v;
    }
    static std::string ifaceList(const std::vector<dnssd_pm_iface_t>& v) {
        std::string s;
        for (const auto& f : v) s += (s.empty() ? "" : ", ") + ifaceText(f);
        return s.empty() ? std::string("(none)") : s;
    }
    static bool sameInterfaces(const std::vector<dnssd_pm_iface_t>& a, const std::vector<dnssd_pm_iface_t>& b) {
        if (a.size() != b.size()) return false;
        for (const auto& x : a) {
            bool found = false;
            for (const auto& y : b) found = found || (x.addr == y.addr && x.ifindex == y.ifindex);
            if (!found) return false;
        }
        return true;
    }
    void logInterfaces(const char* what, const std::vector<dnssd_pm_iface_t>& v) {
        if (v.empty())
            log(LogLevel::Warning, "%s: no usable IPv4 interface (no network?); waiting for a network change", what);
        else
            log(LogLevel::Info, "%s %zu interface(s): %s", what, v.size(), ifaceList(v).c_str());
    }

    void onNetEvent(bool significant) {
        {
            std::lock_guard<std::mutex> lk(netMutex);
            netPending = true;
            netSignificant = netSignificant || significant;
            ++netEvents;
            netLastEventNs = steadyNs();
        }
        netCv.notify_all();
    }
    static void NETIOAPI_API_ cbIfChange(PVOID ctx, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE type) {
        if (type == MibInitialNotification) return;
        static_cast<Impl*>(ctx)->onNetEvent(type != MibParameterNotification);
    }
    static void NETIOAPI_API_ cbAddrChange(PVOID ctx, PMIB_UNICASTIPADDRESS_ROW row, MIB_NOTIFICATION_TYPE type) {
        if (type == MibInitialNotification) return;
        if (row && row->Address.si_family == AF_INET) {
            const uint32_t host = ntohl(row->Address.Ipv4.sin_addr.s_addr);
            if ((host >> 24) == 127) return;  // loopback
        }
        static_cast<Impl*>(ctx)->onNetEvent(type != MibParameterNotification);
    }

    void startNetworkWatch() {
        {
            std::lock_guard<std::mutex> lk(netMutex);
            netQuit = netForced = netPending = netSignificant = false;
            netEvents = 0;
        }
        if (NotifyIpInterfaceChange(AF_INET, &Impl::cbIfChange, this, FALSE, &ifNotify) != NO_ERROR) {
            ifNotify = nullptr;
            log(LogLevel::Warning, "NotifyIpInterfaceChange failed: network changes are not detected automatically");
        }
        if (NotifyUnicastIpAddressChange(AF_INET, &Impl::cbAddrChange, this, FALSE, &addrNotify) != NO_ERROR) {
            addrNotify = nullptr;
            log(LogLevel::Warning, "NotifyUnicastIpAddressChange failed: address changes are not detected automatically");
        }
        netThread = std::thread([this] { netLoop(); });
    }

    void stopNetworkWatch() {
        // CancelMibChangeNotify2 waits for running callbacks to return
        if (ifNotify) CancelMibChangeNotify2(ifNotify);
        if (addrNotify) CancelMibChangeNotify2(addrNotify);
        ifNotify = addrNotify = nullptr;
        {
            std::lock_guard<std::mutex> lk(netMutex);
            netQuit = true;
        }
        netCv.notify_all();
        if (netThread.joinable()) netThread.join();
    }

    void netLoop() {
        std::unique_lock<std::mutex> lk(netMutex);
        int64_t announceAgainAt = 0;
        while (!netQuit) {
            netCv.wait_for(lk, std::chrono::milliseconds(250));
            if (netQuit) break;
            const int64_t now = steadyNs();
            if (netForced || (netPending && now - netLastEventNs >= kNetDebounceNs)) {
                const bool forced = netForced, significant = netSignificant;
                const unsigned events = netEvents;
                netForced = netPending = netSignificant = false;
                netEvents = 0;
                lk.unlock();
                const bool restarted = refreshNetworkNow(forced, significant, events);
                lk.lock();
                announceAgainAt = restarted ? steadyNs() + 1'000'000'000 : 0;
            } else if (announceAgainAt && now >= announceAgainAt) {
                announceAgainAt = 0;
                lk.unlock();
                if (dnssd) dnssd_pm_announce(dnssd);
                log(LogLevel::Info, "[network] mDNS announcement repeated");
                lk.lock();
            }
        }
    }

    // Returns true if the mDNS responder was re-opened (announcement sent).
    bool refreshNetworkNow(bool forced, bool significant, unsigned events) {
        if (!dnssd || !raop) return false;
        const auto before = currentInterfaces();
        const auto now = scanInterfaces();
        const bool changed = !sameInterfaces(before, now);
        const std::string why = forced ? std::string("refreshNetwork()")
                                       : std::to_string(events) + " network change notification(s)";
        if (!forced && !changed && !significant) {
            log(LogLevel::Debug, "[network] %s: interfaces unchanged (%s)", why.c_str(), ifaceList(now).c_str());
            return false;
        }
        log(LogLevel::Info, "[network] %s: interfaces %s: before [%s], now [%s]", why.c_str(),
            changed ? "CHANGED" : "unchanged", ifaceList(before).c_str(), ifaceList(now).c_str());
        const int rc = dnssd_pm_restart(dnssd);
        if (rc != 0)
            log(LogLevel::Error, "[network] mDNS restart failed (error %d); will retry on the next network change", rc);
        else
            logInterfaces("[network] mDNS re-opened and re-announced on", currentInterfaces());
        const int conns = openConnections.load();
        if (!raop_is_running(raop)) {
            if (conns <= 0) {
                requestHttpdReset("AirPlay listener not running after a network change");
            } else {
                log(LogLevel::Warning, "[network] AirPlay listener not running but %d connection(s) open: "
                    "left to the session watchdog", conns);
            }
        } else {
            log(LogLevel::Info, "[network] AirPlay listener on TCP port %u still up (wildcard bind); %d open "
                "connection(s) kept", port, conns);
        }
        return rc == 0;
    }

    // ---------------------------------------------------------------- callbacks
    static Impl* self(void* cls) { return static_cast<Impl*>(cls); }

    static void cbLog(void* cls, int level, const char* msg) {
        auto* s = self(cls);
        LogLevel l;
        switch (level) {
        case LOGGER_EMERG: case LOGGER_ALERT: case LOGGER_CRIT: case LOGGER_ERR: l = LogLevel::Error; break;
        case LOGGER_WARNING: l = LogLevel::Warning; break;
        case LOGGER_NOTICE: case LOGGER_INFO: l = LogLevel::Info; break;
        default: l = LogLevel::Debug; break;
        }
        if (s->logCb) s->logCb(l, msg ? msg : "");
    }

    static void cbConnInit(void* cls) {
        auto* s = self(cls);
        int n = ++s->openConnections;
        s->log(LogLevel::Info, "conn_init: open connections = %d", n);
    }

    static void cbConnDestroy(void* cls) {
        auto* s = self(cls);
        int n = --s->openConnections;
        s->log(LogLevel::Info, "conn_destroy: open connections = %d", n);
        if (n <= 0) {
            s->resetClock();
            if (s->audio) s->audio->onFlush();
            // no hidePin(): the PIN is still needed while the user types it
            s->setCurrentClient(std::string());
            // Only after a client actually asked to connect (GET /info probes
            // from other Apple devices also open and close connections).
            if (s->clientAnnounced.exchange(false) && s->events.onClientDisconnected)
                s->events.onClientDisconnected();
        }
    }

    void hidePin() {
        pinSecondsLeft = 0;
        if (pinShown.exchange(false)) {
            log(LogLevel::Info, "PIN no longer needed (paired, cancelled or replaced)");
            if (events.onPin) events.onPin(std::string());
        }
    }

    // raop_handler_pairpinstart: a client that has not paired before asks for
    // a PIN; the library picks a fresh random 4-digit PIN for every attempt.
    static void cbDisplayPin(void* cls, char* pin) {
        auto* s = self(cls);
        std::string p = pin ? pin : "";
        s->log(LogLevel::Info, "client must enter PIN %s on the iPhone", p.c_str());
        s->pinShown = true;
        s->pinSecondsLeft = kPinShowSec;
        if (s->events.onPin) s->events.onPin(p);
    }

    // uxplay register_client(): called at SETUP after the client paired with
    // the PIN (pair-setup-pin); remember its public key so that it can come
    // back without a PIN (pair-verify -> check_register).
    static void cbRegisterClient(void* cls, const char* deviceId, const char* pk, const char* name) {
        auto* s = self(cls);
        if (!pk) return;
        std::lock_guard<std::mutex> lk(s->regMutex);
        if (s->registerFile.empty()) return;
        if (std::find(s->registeredKeys.begin(), s->registeredKeys.end(), pk) != s->registeredKeys.end()) return;
        s->registeredKeys.emplace_back(pk);
        std::ofstream f(std::filesystem::u8path(s->registerFile), std::ios::app | std::ios::binary);
        if (f) f << pk << "," << (deviceId ? deviceId : "") << "," << (name ? name : "") << "\n";
        s->log(LogLevel::Info, "remembered PIN-paired client \"%s\" (%s)%s", name ? name : "?",
               deviceId ? deviceId : "?", f ? "" : " (could not write the register file)");
    }

    // uxplay check_register(): a returning client skipped the PIN (pair-verify
    // only); admit it only if it paired with a PIN on this receiver before.
    static bool cbCheckRegister(void* cls, const char* pk) {
        auto* s = self(cls);
        std::lock_guard<std::mutex> lk(s->regMutex);
        if (s->registerFile.empty()) return true;
        bool ok = pk && std::find(s->registeredKeys.begin(), s->registeredKeys.end(), pk) != s->registeredKeys.end();
        if (!ok)
            s->log(LogLevel::Warning,
                   "returning client is not in the PIN register (%s): refused; forget this receiver on the "
                   "iPhone or delete the register file to pair again",
                   s->registerFile.c_str());
        return ok;
    }

    void loadRegister() {
        std::lock_guard<std::mutex> lk(regMutex);
        registeredKeys.clear();
        registerFile.clear();
        if (!opts.requirePin || opts.keyFile.empty()) return;
        std::filesystem::path kf = std::filesystem::u8path(opts.keyFile);
        {
            auto u8 = (kf.parent_path() / "airplay_pin_clients.txt").u8string();
            registerFile.assign(u8.begin(), u8.end());
        }
        std::ifstream f(std::filesystem::u8path(registerFile), std::ios::binary);
        std::string line;
        while (std::getline(f, line)) {
            // "pk,deviceid,name": the pk is 44 base64 chars (32-byte Ed25519 key)
            size_t comma = line.find(',');
            std::string pk = line.substr(0, comma);
            while (!pk.empty() && (pk.back() == '\r' || pk.back() == ' ')) pk.pop_back();
            if (pk.size() == 44) registeredKeys.push_back(pk);
        }
        log(LogLevel::Info, "PIN register %s: %zu paired client(s)", registerFile.c_str(), registeredKeys.size());
    }

    void markAudio(int what) {
        audioMarkNs = steadyNs();
        audioMarkWhat = what;
    }

    static void cbConnFeedback(void* cls) { self(cls)->missedFeedback = 0; }

    static void cbConnReset(void* cls, int reason) {
        auto* s = self(cls);
        s->requestHttpdReset(reason == 1 ? "lost connection with client (network problem?)"
                             : reason == 2 ? "unsupported HLS streaming source"
                                           : "conn_reset");
    }

    static void cbVideoReset(void* cls, reset_type_t type) {
        auto* s = self(cls);
        static const char* names[] = {"NoHold (new client took over)", "RTP shutdown (mirroring stopped)",
                                      "HLS shutdown", "HLS EOS", "on_video_play", "RTP->HLS teardown"};
        s->log(LogLevel::Info, "video_reset: %s", (type >= 0 && type <= 5) ? names[type] : "?");
        // HLS is disabled: HLS-only reset types must not disturb a mirroring session.
        if (type == RESET_TYPE_ON_VIDEO_PLAY || type == RESET_TYPE_HLS_SHUTDOWN || type == RESET_TYPE_HLS_EOS)
            return;
        s->resetClock();
        s->vt = VideoTiming{};
        if (s->video) s->video->onReset();
        if (type == RESET_TYPE_NOHOLD && s->audio) s->audio->onFlush();
    }

    static int cbVideoSetCodec(void* cls, video_codec_t codec) {
        auto* s = self(cls);
        if (codec == VIDEO_CODEC_H265 && !s->h265) {
            s->log(LogLevel::Warning, "client sent H.265 although H.265 was not advertised; forwarding anyway");
        }
        VideoCodec c = (codec == VIDEO_CODEC_H265) ? VideoCodec::H265 : VideoCodec::H264;
        s->log(LogLevel::Info, "video_set_codec: %s", c == VideoCodec::H265 ? "H.265" : "H.264");
        if (s->video) s->video->onCodec(c);
        return 0;
    }

    static void cbVideoProcess(void* cls, raop_ntp_t*, video_decode_struct* d) {
        auto* s = self(cls);
        if (!d || !d->data || d->data_len < 5) return;
        // raop_rtp_mirror.c already rewrote the AVCC 4-byte length prefixes to
        // 00 00 00 01 start codes and prepends SPS/PPS(/VPS) before the next
        // frame after a codec packet: the payload is Annex-B. If decryption or
        // NAL parsing failed it sets data[0] = 1 (see renderers/video_renderer.c).
        if (d->data[0] != 0) {
            unsigned n = ++s->badVideoFrames;
            if (n <= 5 || (n % 100) == 0)
                s->log(LogLevel::Error, "dropping undecryptable/invalid video frame (%u so far)", n);
            return;
        }
        uint64_t pts = s->toLocal(d->ntp_time_local, d->ntp_time_remote, "video");
        uint64_t t0 = get_local_time();
        if (s->video) s->video->onFrame(d->data, static_cast<size_t>(d->data_len), pts);
        if (s->opts.timingLog) s->videoTiming(d, pts, t0, get_local_time());
    }

    void videoTiming(const video_decode_struct* d, uint64_t ptsRel, uint64_t sinkIn, uint64_t sinkOut) {
        VideoTiming& t = vt;
        const uint64_t hdr = d->pm_t_header ? d->pm_t_header : sinkIn;
        auto ms = [](uint64_t a, uint64_t b) { return (static_cast<int64_t>(a) - static_cast<int64_t>(b)) / 1e6; };
        if (!t.windowStart) t.windowStart = hdr;
        ++t.frames;
        t.bytes += d->data_len;
        if (d->data_len > t.maxBytes) t.maxBytes = d->data_len;
        if (d->ntp_time_local) t.lead.add(ms(d->ntp_time_local, hdr));
        else ++t.noNtp;
        t.leadRel.add(ms(ptsRel, hdr));
        if (d->pm_t_payload) t.recv.add(ms(d->pm_t_payload, hdr));
        if (d->pm_t_decrypted && d->pm_t_payload) t.decrypt.add(ms(d->pm_t_decrypted, d->pm_t_payload));
        t.sink.add(ms(sinkOut, sinkIn));
        if (t.lastHeader) t.interval.add(ms(hdr, t.lastHeader));
        t.lastHeader = hdr;
        const double span = ms(hdr, t.windowStart);
        if (span < 5000) return;
        log(LogLevel::Info,
            "[video-timing] %.1f s: %u frames (%.1f fps), avg %.1f KB max %.1f KB, interval avg %.1f max %.1f ms | "
            "lead = pts(NTP->local) - arrival: avg %.1f min %.1f max %.1f ms%s | lead vs session timeline (ntpLocalNs): avg "
            "%.1f min %.1f max %.1f ms | recv payload avg %.2f max %.1f ms, decrypt avg %.2f max %.1f ms, "
            "sink onFrame avg %.2f max %.1f ms",
            span / 1000, t.frames, t.frames * 1000.0 / span, t.bytes / 1024.0 / t.frames, t.maxBytes / 1024.0,
            t.interval.avg(), t.interval.mx, t.lead.avg(), t.lead.mn, t.lead.mx,
            t.noNtp ? (t.noNtp == t.frames ? " (NO NTP clock sync)" : " (some frames before NTP sync)") : "",
            t.leadRel.avg(), t.leadRel.mn, t.leadRel.mx,
            t.recv.avg(), t.recv.mx, t.decrypt.avg(), t.decrypt.mx, t.sink.avg(), t.sink.mx);
        const uint64_t last = t.lastHeader;
        t = VideoTiming{};
        t.windowStart = last;
        t.lastHeader = last;
    }

    static void cbVideoPause(void* cls) {
        self(cls)->vt = VideoTiming{};
        self(cls)->log(LogLevel::Info, "video_pause (client suspended stream)");
        if (self(cls)->video) self(cls)->video->onPaused(true);
    }
    static void cbVideoResume(void* cls) {
        self(cls)->log(LogLevel::Info, "video_resume");
        if (self(cls)->video) self(cls)->video->onPaused(false);
    }
    static void cbVideoFlush(void* cls) { self(cls)->log(LogLevel::Debug, "video_flush"); }

    static void cbVideoReportSize(void* cls, float* ws, float* hs, float* w, float* h) {
        auto* s = self(cls);
        s->log(LogLevel::Info, "video_report_size: source %.0fx%.0f, stream %.0fx%.0f", *ws, *hs, *w, *h);
        if (s->video) s->video->onSourceSize(static_cast<int>(*ws), static_cast<int>(*hs));
    }

    static void cbAudioGetFormat(void* cls, unsigned char* ct, unsigned short* spf, bool* usingScreen,
                                 bool* isMedia, uint64_t* audioFormat) {
        auto* s = self(cls);
        s->log(LogLevel::Info,
               "[audio-timing] audio stream SETUP: audio_get_format ct=%u spf=%u usingScreen=%d isMedia=%d "
               "audioFormat=0x%llx",
               *ct, *spf, (int)*usingScreen, (int)*isMedia, (unsigned long long)*audioFormat);
        s->markAudio(1);
        // All AirPlay audio formats are 44100 Hz stereo (renderers/audio_renderer.c caps).
        AudioCodec codec;
        int samples;
        switch (*ct) {
        case 2: codec = AudioCodec::ALAC; samples = *spf ? *spf : 352; break;
        case 4: codec = AudioCodec::AAC_LC; samples = 1024; break;
        case 8: codec = AudioCodec::AAC_ELD; samples = 480; break;  // ASC f8e85000: 480-sample frames
        default:
            s->log(LogLevel::Warning, "audio ct=%u (PCM/unknown) is not forwarded to the audio sink", *ct);
            return;
        }
        if (s->audio) s->audio->onFormat(codec, 44100, 2, samples);
    }

    static void cbAudioProcess(void* cls, raop_ntp_t*, audio_decode_struct* d) {
        auto* s = self(cls);
        if (!d || !d->data || d->data_len <= 0) return;
        if (d->ct != 2 && d->ct != 4 && d->ct != 8) return;
        if (int what = s->audioMarkWhat.exchange(0))
            s->log(LogLevel::Info, "[audio-timing] first audio packet to the sink %.1f ms after %s (seq %u, %d bytes)",
                   (steadyNs() - s->audioMarkNs.load()) / 1e6, what == 1 ? "SETUP" : "FLUSH", d->seqnum,
                   d->data_len);
        uint64_t pts = s->toLocal(d->ntp_time_local, d->ntp_time_remote, "audio");
        if (s->audio) s->audio->onPacket(d->data, static_cast<size_t>(d->data_len), pts);
    }

    static void cbAudioFlush(void* cls) {
        auto* s = self(cls);
        s->log(LogLevel::Info, "[audio-timing] audio FLUSH");
        s->markAudio(2);
        if (s->audio) s->audio->onFlush();
    }

    // Required by the library (GET /info handler calls it unguarded).
    static double cbAudioGetClientVolume(void* cls) { return self(cls)->currentVolumeDb.load(); }

    static void cbAudioSetVolume(void* cls, float volume) {
        auto* s = self(cls);
        s->currentVolumeDb = volume;
        s->log(LogLevel::Info, "audio_set_volume: %.2f dB", volume);
        if (s->audio) s->audio->onVolume(volume);
    }

    static void cbReportClient(void* cls, char* deviceid, char* model, char* name, bool* admit) {
        auto* s = self(cls);
        s->log(LogLevel::Info, "connection request from \"%s\" (%s), deviceID %s", name ? name : "?",
               model ? model : "?", deviceid ? deviceid : "?");
        *admit = true;
        // SETUP comes after pair-verify/fp-setup: any PIN entry is done now.
        s->hidePin();
        s->clientAnnounced = true;
        s->setCurrentClient(name ? name : "");
        if (s->events.onClientConnecting) s->events.onClientConnecting(name ? name : "", model ? model : "");
    }

    // raop_handlers.h SETUP (PM): newName was admitted while oldName was
    // mirroring; the old connection is closed and video_reset(NOHOLD) (->
    // onReset + onFlush) already ran.
    static void cbTakeover(void* cls, const char* oldName, const char* newName) {
        auto* s = self(cls);
        const std::string o = oldName ? oldName : "", n = newName ? newName : "";
        s->log(LogLevel::Info, "[takeover] \"%s\" replaced \"%s\"", n.c_str(), o.c_str());
        if (s->events.onTakeover) s->events.onTakeover(o, n);
    }

    // A connection closed that carried the admitted session (clientName !=
    // nullptr; also on a full TEARDOWN) and/or had asked for a PIN.
    static void cbPmConnEnd(void* cls, const char* clientName, bool pinRequested) {
        auto* s = self(cls);
        // pinRequested: the newcomer closed its pair-pin-start connection, which
        // the iPhone always does before showing its PIN prompt; keep the PIN up
        // (the supervisor hides it if the user cancelled).
        (void)pinRequested;
        if (clientName) {
            std::lock_guard<std::mutex> lk(s->clientMutex);
            if (s->currentClient == clientName) s->currentClient.clear();
        }
    }

    // HLS ("AirPlay video", e.g. YouTube) is not advertised (features bits 0/4
    // off) and the library ignores such requests, but several HLS handlers
    // call these unguarded: keep harmless stubs so a stray request can't crash.
    static void cbOnVideoPlay(void* cls, const char* location, const float) {
        self(cls)->log(LogLevel::Warning, "ignoring HLS video play request (%s)", location ? location : "");
    }
    static void cbOnVideoScrub(void*, const float) {}
    static void cbOnVideoRate(void*, const float) {}
    static void cbOnVideoStop(void*) {}
    static void cbOnVideoPlaybackInfo(void*, playback_info_t* info) {
        info->duration = -1.0;
        info->position = -1.0;
    }
    static float cbOnVideoPlaylistRemove(void*) { return -1.0f; }

    static const char* cbPasswd(void*, int* len) {
        *len = 0;  // no password
        return nullptr;
    }
};

AirPlayServer::AirPlayServer() : impl_(std::make_unique<Impl>()) {}
AirPlayServer::~AirPlayServer() { stop(); }

void AirPlayServer::setLogCallback(LogCallback cb) { impl_->logCb = std::move(cb); }
void AirPlayServer::setOptions(const Options& opts) { impl_->opts = opts; }
void AirPlayServer::setEvents(Events events) { impl_->events = std::move(events); }
bool AirPlayServer::isRunning() const { return impl_->running; }
unsigned short AirPlayServer::port() const { return impl_->port; }
std::string AirPlayServer::deviceId() const { return impl_->mac; }
std::string AirPlayServer::currentClientName() const {
    std::lock_guard<std::mutex> lk(impl_->clientMutex);
    return impl_->currentClient;
}
uint64_t AirPlayServer::localTimeNs() { return get_local_time(); }

bool AirPlayServer::start(const std::string& displayName, VideoSink* video, AudioSink* audio) {
    Impl& s = *impl_;
    if (s.running) return true;
    s.video = video;
    s.audio = audio;
    s.name = displayName.empty() ? std::string("PhoneMirror") : displayName;
    s.openConnections = 0;
    s.missedFeedback = 0;
    s.badVideoFrames = 0;
    s.pinShown = false;
    s.pinSecondsLeft = 0;
    s.clientAnnounced = false;
    s.setCurrentClient(std::string());
    s.currentVolumeDb = s.opts.initialVolumeDb;  // PM: remembered volume instead of uxplay's full volume
    if (s.audio) s.audio->onVolume(s.opts.initialVolumeDb);
    s.resetClock();
    // uxplay.cpp main(): initialises the QPC frequency used by the Windows
    // kernel-timestamp NTP path (division by zero otherwise).
    ntp_global_init();

    // --- MAC / deviceid (uxplay.cpp main) ---
    s.mac = s.opts.macAddress.empty() ? findMac() : s.opts.macAddress;
    std::vector<char> hw;
    if (s.mac.empty() || !parseMac(s.mac, hw)) {
        if (!s.mac.empty()) s.log(LogLevel::Warning, "invalid MAC address \"%s\"", s.mac.c_str());
        s.mac = randomMac();
        parseMac(s.mac, hw);
        s.log(LogLevel::Info, "using random MAC address %s", s.mac.c_str());
    } else {
        s.log(LogLevel::Info, "using MAC address %s as AirPlay deviceid", s.mac.c_str());
    }

    // --- start_dnssd() ---
    // uxplay "-pin": pin_pw = 1 -> TXT pw=true and RAOP sf=0x8c, so the iPhone
    // asks for an on-screen PIN (pair-pin-start / pair-setup-pin) the first
    // time it connects to this receiver identity.
    int err = 0;
    s.dnssd = dnssd_init(s.name.c_str(), static_cast<int>(s.name.size()), hw.data(), static_cast<int>(hw.size()),
                         s.opts.requirePin ? 1 : 0, &err);
    if (!s.dnssd || err) {
        s.log(LogLevel::Error, "dnssd_init failed (error %d)", err);
        s.dnssd = nullptr;
        return false;
    }
    // --- display mode (uxplay "-s wxh@r", "-fps", "-h265") ---
    const Impl::Display disp = s.resolveDisplay();
    s.h265 = disp.h265;

    // Feature bits as in uxplay.cpp start_dnssd() defaults (0x5A7FFEE6,0x0) with
    // hls_support = false (bits 0 and 4 off) and setup_legacy_pairing = true.
    dnssd_set_airplay_features(s.dnssd, 0, 0);   // HLS video off
    dnssd_set_airplay_features(s.dnssd, 4, 0);   // HLS off
    dnssd_set_airplay_features(s.dnssd, 7, 1);   // screen mirroring
    dnssd_set_airplay_features(s.dnssd, 9, 1);   // audio
    dnssd_set_airplay_features(s.dnssd, 27, 1);  // legacy pairing (uxplay sets it with -pin; always on here)
    dnssd_set_airplay_features(s.dnssd, 42, s.h265 ? 1 : 0);  // Screen Multi Codec (H.265)

    // --- start_raop_server() ---
    raop_callbacks_t cbs;
    std::memset(&cbs, 0, sizeof(cbs));
    cbs.cls = &s;
    cbs.conn_init = Impl::cbConnInit;
    cbs.conn_destroy = Impl::cbConnDestroy;
    cbs.conn_reset = Impl::cbConnReset;
    cbs.conn_feedback = Impl::cbConnFeedback;
    cbs.audio_process = Impl::cbAudioProcess;
    cbs.video_process = Impl::cbVideoProcess;
    cbs.audio_flush = Impl::cbAudioFlush;
    cbs.video_flush = Impl::cbVideoFlush;
    cbs.video_pause = Impl::cbVideoPause;
    cbs.video_resume = Impl::cbVideoResume;
    cbs.audio_set_volume = Impl::cbAudioSetVolume;
    cbs.audio_set_client_volume = Impl::cbAudioGetClientVolume;
    cbs.on_video_play = Impl::cbOnVideoPlay;
    cbs.on_video_scrub = Impl::cbOnVideoScrub;
    cbs.on_video_rate = Impl::cbOnVideoRate;
    cbs.on_video_stop = Impl::cbOnVideoStop;
    cbs.on_video_acquire_playback_info = Impl::cbOnVideoPlaybackInfo;
    cbs.on_video_playlist_remove = Impl::cbOnVideoPlaylistRemove;
    cbs.audio_get_format = Impl::cbAudioGetFormat;
    cbs.video_report_size = Impl::cbVideoReportSize;
    cbs.report_client_request = Impl::cbReportClient;
    cbs.passwd = Impl::cbPasswd;
    cbs.video_reset = Impl::cbVideoReset;
    cbs.video_set_codec = Impl::cbVideoSetCodec;
    cbs.pm_takeover = Impl::cbTakeover;
    cbs.pm_conn_end = Impl::cbPmConnEnd;
    if (s.opts.requirePin) {
        cbs.display_pin = Impl::cbDisplayPin;
        s.loadRegister();
        cbs.register_client = Impl::cbRegisterClient;
        cbs.check_register = Impl::cbCheckRegister;
    }

    s.raop = raop_init(&cbs);
    if (!s.raop) {
        s.log(LogLevel::Error, "raop_init failed (winsock init or callbacks)");
        dnssd_destroy(s.dnssd);
        s.dnssd = nullptr;
        return false;
    }
    raop_set_log_callback(s.raop, Impl::cbLog, &s);
    raop_set_log_level(s.raop, s.opts.debugLog ? LOGGER_DEBUG : LOGGER_INFO);
    // nohold = 1 below keeps a second client's connection open next to the
    // running session; the policy decides at its SETUP (raop_handlers.h, PM).
    raop_pm_set_takeover_policy(s.raop, s.opts.takeoverPolicy == TakeoverPolicy::KeepCurrent
                                            ? RAOP_PM_TAKEOVER_KEEP_CURRENT
                                            : RAOP_PM_TAKEOVER_NEW_REPLACES_OLD);
    if (raop_init2(s.raop, 1 /* nohold */, s.mac.c_str(), s.opts.keyFile.c_str())) {
        s.log(LogLevel::Error, "raop_init2 failed (pairing key setup)");
        raop_destroy(s.raop);
        s.raop = nullptr;
        dnssd_destroy(s.dnssd);
        s.dnssd = nullptr;
        return false;
    }
    // GET /info "displays"[0]: width/height/widthPixels/heightPixels,
    // refreshRate (as 1/r), maxFPS (raop_handlers.h). These are the only
    // places UxPlay uses them; feature bit 42 (above) is the H.265 interplay.
    raop_set_plist(s.raop, "width", disp.width);
    raop_set_plist(s.raop, "height", disp.height);
    raop_set_plist(s.raop, "refreshRate", disp.refreshRate);
    raop_set_plist(s.raop, "maxFPS", disp.maxFps);
    s.log(LogLevel::Info, "display mode offered to the client: %dx%d @%d Hz, maxFPS %d, H.265 %s", disp.width,
          disp.height, disp.refreshRate, disp.maxFps, s.h265 ? "on" : "off");
    if (s.opts.reportedAudioLatencyMs >= 0)  // uxplay "-al": RECORD Audio-Latency header
        raop_set_plist(s.raop, "audio_delay_micros", s.opts.reportedAudioLatencyMs * 1000);
    s.vt = VideoTiming{};
    pm_mirror_quick_ack = s.opts.mirrorQuickAck ? 1 : 0;
    // uxplay: "if (pin_pw == 1) raop_set_plist(raop, "pin", pin)"; pin 0 = a
    // new random PIN for every pair-pin-start.
    if (s.opts.requirePin) raop_set_plist(s.raop, "pin", 0);

    unsigned short tcp[3] = {0, 0, 0}, udp[3] = {0, 0, 0};
    if (s.opts.legacyPorts) {  // uxplay "-p": TCP 7100,7000,(7001) UDP 7011,6001,6000
        tcp[0] = 7100; tcp[1] = 7000; tcp[2] = 7001;
        udp[0] = 7011; udp[1] = 6001; udp[2] = 6000;
    }
    raop_set_tcp_ports(s.raop, tcp);
    raop_set_udp_ports(s.raop, udp);

    unsigned short p = raop_get_port(s.raop);
    if (raop_start_httpd(s.raop, &p) < 0) {
        s.log(LogLevel::Error, "could not start AirPlay HTTP server on TCP port %u (port in use?)", p);
        raop_destroy(s.raop);
        s.raop = nullptr;
        dnssd_destroy(s.dnssd);
        s.dnssd = nullptr;
        return false;
    }
    raop_set_port(s.raop, p);
    s.port = p;
    raop_set_dnssd(s.raop, s.dnssd);  // also hands the Ed25519 pk to the TXT records
    s.log(LogLevel::Info, "AirPlay server listening on TCP port %u", p);

    // --- register_dnssd() ---
    err = dnssd_register_raop(s.dnssd, p);
    if (!err) err = dnssd_register_airplay(s.dnssd, p);
    if (err) {
        s.log(LogLevel::Error,
              "mDNS registration failed (error %d): UDP 5353 / multicast blocked, or another responder "
              "holds the port exclusively",
              err);
        raop_destroy(s.raop);
        s.raop = nullptr;
        dnssd_destroy(s.dnssd);
        s.dnssd = nullptr;
        return false;
    }
    std::string raopId;
    for (char c : s.mac)
        if (c != ':') raopId += static_cast<char>(toupper(static_cast<unsigned char>(c)));
    s.log(LogLevel::Info,
          "mDNS: advertising \"%s._airplay._tcp.local\" and \"%s@%s._raop._tcp.local\" on port %u, "
          "features=0x%llX (H.265 %s, PIN %s, takeover %s)",
          s.name.c_str(), raopId.c_str(), s.name.c_str(), p,
          (unsigned long long) dnssd_get_airplay_features(s.dnssd), s.h265 ? "on" : "off",
          s.opts.requirePin ? "required" : "off",
          s.opts.takeoverPolicy == TakeoverPolicy::KeepCurrent ? "KeepCurrent" : "NewReplacesOld");
    s.logInterfaces("mDNS advertising on", s.currentInterfaces());

    {
        std::lock_guard<std::mutex> lk(s.supMutex);
        s.supQuit = false;
        s.resetHttpd = false;
    }
    s.supervisor = std::thread([&s] { s.supervisorLoop(); });
    s.startNetworkWatch();
    s.running = true;
    return true;
}

void AirPlayServer::refreshNetwork() {
    Impl& s = *impl_;
    {
        std::lock_guard<std::mutex> lk(s.netMutex);
        if (!s.netThread.joinable()) return;
        s.netForced = true;
    }
    s.netCv.notify_all();
}

std::vector<std::string> AirPlayServer::advertisedInterfaces() const {
    std::vector<std::string> out;
    for (const auto& f : impl_->currentInterfaces()) out.push_back(Impl::ifaceText(f));
    return out;
}

void AirPlayServer::applyQualityPreset(Options& o, QualityPreset preset) {
    switch (preset) {
    case QualityPreset::Standard: o.width = 1920; o.height = 1080; o.h265 = false; break;
    case QualityPreset::High:     o.width = 2560; o.height = 1440; o.h265 = true; break;
    case QualityPreset::Highest:  o.width = 3840; o.height = 2160; o.h265 = true; break;
    }
    o.refreshRate = 60;
    o.maxFps = 60;
}

void AirPlayServer::stop() {
    Impl& s = *impl_;
    if (!s.running) return;
    s.stopNetworkWatch();
    {
        std::lock_guard<std::mutex> lk(s.supMutex);
        s.supQuit = true;
    }
    s.supCv.notify_all();
    if (s.supervisor.joinable()) s.supervisor.join();

    if (s.dnssd) {  // withdraw the advertisement (mDNS goodbye) before tearing down
        dnssd_unregister_raop(s.dnssd);
        dnssd_unregister_airplay(s.dnssd);
    }
    if (s.raop) {
        raop_destroy(s.raop);
        s.raop = nullptr;
    }
    if (s.dnssd) {
        dnssd_destroy(s.dnssd);
        s.dnssd = nullptr;
    }
    s.running = false;
    s.port = 0;
    s.log(LogLevel::Info, "AirPlay server stopped");
}

}  // namespace pm
