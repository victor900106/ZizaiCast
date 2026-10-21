// pm::remote::Host: HTTP routes, the approval flow, one WebRTC session
// (video + audio track + input DataChannel), the capture/encode loop and the
// PC's sound.
#include "pm/remote.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <bcrypt.h>
#include <mfapi.h>
#include <timeapi.h>
#include <shellapi.h>  // DragQueryFileW (0.8.3: files copied in Explorer)
#pragma comment(lib, "shell32.lib")

#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <filesystem>
#include <cwctype>

#include "audio.h"
#include "capture.h"
#include "encoder.h"
#include "http_server.h"
#include "input.h"
#include "pm/share_server.h"  // pm::share::lanInterfaces (same LAN choice as 傳到手機)
#include "remote_apple_icon_png.h"
#include "remote_icon192_png.h"
#include "remote_icon512_png.h"
#include "remote_icon_png.h"
#include "remote_manifest.h"
#include "remote_page_html.h"
#include "remote_trackpad_js.h"
#include "remote_vkbd_js.h"
#include "util.h"

namespace pm::remote {

namespace {

using json = nlohmann::json;

std::string randomString(size_t len) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    const size_t k = sizeof(kAlphabet) - 1;
    std::string s;
    while (s.size() < len) {
        uint8_t b[64];
        BCryptGenRandom(nullptr, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        for (uint8_t x : b) {
            if (x >= 256 - (256 % k)) continue;
            s.push_back(kAlphabet[x % k]);
            if (s.size() == len) break;
        }
    }
    return s;
}

uint32_t randomU32() {
    uint32_t v = 0;
    BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&v), sizeof(v), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return v;
}

bool constantTimeEq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char d = 0;
    for (size_t i = 0; i < a.size(); ++i) d |= static_cast<unsigned char>(a[i] ^ b[i]);
    return d == 0;
}

std::string queryParam(const std::string& q, const std::string& key) {
    size_t p = 0;
    while (p <= q.size()) {
        const size_t amp = q.find('&', p);
        const std::string kv = q.substr(p, amp == std::string::npos ? std::string::npos : amp - p);
        const size_t eq = kv.find('=');
        if (kv.substr(0, eq) == key) return eq == std::string::npos ? "" : kv.substr(eq + 1);
        if (amp == std::string::npos) break;
        p = amp + 1;
    }
    return {};
}

constexpr int kBadTriesToBlock = 10;  // wrong links from one address in 10 minutes -> 15 minutes out

bool isTailnet(uint32_t addrNet) { return (ntohl(addrNet) & 0xFFC00000u) == 0x64400000u; }  // 100.64.0.0/10

// Device ids are made by the page (random, kept in its localStorage).
// 加到主畫面 (0.8.2, remote/web): the manifest ("start_url": "./" = the page it came
// with: /<token>/, /c/<link>/ or the fixed link's "/") and its icons; no secrets in them.
bool webAsset(const std::string& name, std::string& type, std::string& body) {
    auto bytes = [](const unsigned char* p, size_t n) { return std::string(reinterpret_cast<const char*>(p), n); };
    if (name == "manifest.webmanifest") type = "application/manifest+json; charset=utf-8", body = bytes(kRemoteManifest, sizeof(kRemoteManifest));
    else if (name == "apple-touch-icon.png" || name == "apple-touch-icon-precomposed.png") type = "image/png", body = bytes(kRemoteAppleIconPng, sizeof(kRemoteAppleIconPng));
    else if (name == "icon-192.png" || name == "favicon.ico") type = "image/png", body = bytes(kRemoteIcon192Png, sizeof(kRemoteIcon192Png));
    else if (name == "icon-512.png") type = "image/png", body = bytes(kRemoteIcon512Png, sizeof(kRemoteIcon512Png));
    else if (name == "icon.png") type = "image/png", body = bytes(kRemoteIconPng, sizeof(kRemoteIconPng));
    else return false;
    return true;
}

bool validDevice(const std::string& d) {
    if (d.size() < 8 || d.size() > 64) return false;
    for (char c : d)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    return true;
}

// "iPhone · Safari" from a user agent (shown in the PC's 允許 / 拒絕 dialog).
std::string deviceLabel(const std::string& ua) {
    auto has = [&](const char* s) { return ua.find(s) != std::string::npos; };
    const char* dev = has("iPhone")              ? "iPhone"
                      : has("iPad")              ? "iPad"
                      : has("Android")           ? (has("Mobile") ? "Android phone" : "Android tablet")
                      : has("Macintosh")         ? (has("Mobile") ? "iPad" : "Mac")
                      : has("Windows")           ? "Windows PC"
                      : has("Linux")             ? "Linux"
                                                 : "?";
    const char* br = has("EdgA") || has("EdgiOS") || has("Edg/") ? "Edge"
                     : has("SamsungBrowser")                    ? "Samsung Internet"
                     : has("CriOS") || has("Chrome/")           ? "Chrome"
                     : has("FxiOS") || has("Firefox/")          ? "Firefox"
                     : has("Safari/")                           ? "Safari"
                                                                : "browser";
    return std::string(dev) + " \xC2\xB7 " + br;
}

std::string hexColor(uint32_t c) {
    char b[8];
    snprintf(b, sizeof(b), "#%06X", unsigned(c & 0xFFFFFF));
    return b;
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) s.replace(p, from.size(), to);
    return s;
}

// JSON for embedding in a <script>: no "</" sequences.
std::string scriptJson(const json& j) { return replaceAll(j.dump(), "</", "<\\/"); }


// PC clipboard text (F7). OpenClipboard may be busy for a moment: retry.
bool openClipboardRetry() {
    for (int i = 0; i < 10; ++i) {
        if (OpenClipboard(nullptr)) return true;
        Sleep(15);
    }
    return false;
}
bool setClipboardText(const std::wstring& w) {
    if (!openClipboardRetry()) return false;
    EmptyClipboard();
    const size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (h) {
        if (void* p = GlobalLock(h)) {
            memcpy(p, w.c_str(), bytes);
            GlobalUnlock(h);
            ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
        }
        if (!ok) GlobalFree(h);
    }
    CloseClipboard();
    return ok;
}
bool getClipboardText(std::wstring& out) {
    out.clear();
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return true;  // no text (picture, files ...): empty
    if (!openClipboardRetry()) return false;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out.assign(p, wcsnlen(p, 32 * 1024));
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return true;
}

// ---- 0.8.3 files ----
std::string pctDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit(static_cast<unsigned char>(s[i + 1])) && isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            o += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else o += s[i] == '+' ? ' ' : s[i];
    }
    return o;
}
// A file name from the phone, safe to create in the upload folder: the last path part
// only, no control characters or <>:"/\|?*, no trailing dots / spaces, never a reserved
// device name (CON, NUL, COM1 ...), never empty or dots only; at most 120 characters.
std::wstring cleanFileName(const std::string& utf8) {
    std::wstring w = widen(utf8);
    if (const size_t p = w.find_last_of(L"/\\"); p != std::wstring::npos) w = w.substr(p + 1);
    std::wstring o;
    for (wchar_t c : w)
        if (c >= 32 && !wcschr(L"<>:\"/\\|?*", c) && c != 0x7F) o += c;
    while (!o.empty() && (o.back() == L'.' || o.back() == L' ')) o.pop_back();
    while (!o.empty() && o.front() == L' ') o.erase(o.begin());
    if (o.find_first_not_of(L'.') == std::wstring::npos) o = L"file";
    if (o.size() > 120) {
        const size_t dot = o.rfind(L'.');
        const std::wstring ext = dot != std::wstring::npos && o.size() - dot <= 12 ? o.substr(dot) : L"";
        o = o.substr(0, 120 - ext.size()) + ext;
    }
    std::wstring base = o.substr(0, o.find(L'.'));
    for (wchar_t& c : base) c = wchar_t(towupper(c));
    static const wchar_t* const kReserved[] = {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"};
    bool reserved = false;
    for (const wchar_t* r : kReserved) reserved |= base == r;
    if (base.size() == 4 && (base.rfind(L"COM", 0) == 0 || base.rfind(L"LPT", 0) == 0) && ((base[3] >= L'0' && base[3] <= L'9') || base[3] == 0xB9 || base[3] == 0xB2 || base[3] == 0xB3))
        reserved = true;
    if (reserved) o = L"_" + o;
    return o;
}
// The files copied in Explorer (CF_HDROP), regular files only, at most 32.
std::vector<std::wstring> clipboardFiles() {
    std::vector<std::wstring> v;
    if (!IsClipboardFormatAvailable(CF_HDROP) || !openClipboardRetry()) return v;
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
        if (auto drop = static_cast<HDROP>(GlobalLock(h))) {
            const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n && v.size() < 32; ++i) {
                wchar_t p[MAX_PATH * 4] = {};
                if (DragQueryFileW(drop, i, p, UINT(std::size(p))) == 0) continue;
                const DWORD a = GetFileAttributesW(p);
                if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) v.push_back(p);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return v;
}

}  // namespace

// ---- public helpers ----------------------------------------------------------

std::vector<Monitor> monitors() {
    std::vector<Monitor> v;
    int i = 0;
    for (const MonitorDesc& d : enumMonitors()) {
        Monitor m;
        m.index = i++;
        m.x = d.rect.left, m.y = d.rect.top, m.w = d.rect.right - d.rect.left, m.h = d.rect.bottom - d.rect.top;
        m.primary = d.primary;
        v.push_back(m);
    }
    return v;
}

Quality qualityPreset(int level) {
    switch (std::clamp(level, 0, kQualityLevels - 1)) {
    case 0: return {1920, 1080, 60, 6000};    // 流暢: phones on a busy Wi-Fi / mobile data
    case 1: return {2560, 1440, 60, 10000};   // 標準
    default: return {3840, 2160, 60, 14000};  // 清晰: native (3440x1440 measured clear and smooth)
    }
}

std::vector<AudioDevice> playbackDevices() {
    std::vector<AudioDevice> v;
    for (const PlaybackDevice& d : playbackDevicesList()) v.push_back(AudioDevice{d.id, d.name, d.isDefault, d.volume, d.hwVolume, d.muted, d.activeSessions});
    return v;
}

std::string tailscaleIp() {
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf;
    ULONG r = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && r == ERROR_BUFFER_OVERFLOW; ++tries) {
        buf.resize(size);
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                 reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (r != NO_ERROR) return {};
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* sa = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            if (!isTailnet(sa->sin_addr.s_addr)) continue;
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
            return ip;
        }
    }
    return {};
}

// ---- the tunnel path of a session (0.8.1) ------------------------------------

// Its WebSocket, a writer thread with a send queue, and the congestion rule
// for video on it: the page acks every frame it receives; a frame unacked
// for more than 0.5 s (or > 2 MB waiting) means the path is full: video is
// dropped until the backlog is gone, then it restarts at a key frame.
struct TunnelLink {
    enum Kind { Text, Video, Audio };
    struct Item {
        Kind kind = Text;
        std::string b;
    };
    std::mutex m;
    std::condition_variable cv;
    WsConn* ws = nullptr;  // null once its connection is gone
    std::deque<Item> q;
    size_t qVideo = 0;
    bool stop = false;
    std::atomic<bool> media{false};  // the page shows the picture from here (WebRTC did not connect)
    bool cc = true;                  // the congestion rule (Options::tunnelCongestion)
    int testKbps = 0;                // tests: a slow link (Options::tunnelTestKbps)
    int64_t nextSendUs = 0;
    bool waitKey = true;
    uint32_t seq = 0;
    std::deque<std::pair<uint32_t, int64_t>> unacked;  // video seq, sent at (nowUs)
    int64_t sent = 0, dropped = 0, sentBytes = 0;
    std::thread writer;

    void pushText(std::string&& t) {
        {
            std::lock_guard<std::mutex> lk(m);
            if (stop) return;
            q.push_back(Item{Text, std::move(t)});
        }
        cv.notify_one();
    }
    void pushAudio(const uint8_t* p, size_t n, double capMs) {
        {
            std::lock_guard<std::mutex> lk(m);
            if (stop || q.size() > 400) return;
            std::string b(9 + n, '\0');
            b[0] = 2;
            memcpy(&b[1], &capMs, 8);
            memcpy(&b[9], p, n);
            q.push_back(Item{Audio, std::move(b)});
        }
        cv.notify_one();
    }
    // false = dropped (wantKey: ask the encoder for a key frame)
    bool pushVideo(const std::vector<uint8_t>& au, bool key, double capMs, int64_t now, bool& wantKey) {
        {
            std::lock_guard<std::mutex> lk(m);
            if (stop) return false;
            while (!unacked.empty() && now - unacked.front().second > 3000000) unacked.pop_front();  // lost for good
            const bool full = cc && ((!unacked.empty() && now - unacked.front().second > 500000) || qVideo > 2 * 1024 * 1024);
            if (full) waitKey = true;
            if (full || (waitKey && !key)) {
                ++dropped;
                wantKey = waitKey;
                return false;
            }
            waitKey = false;
            std::string b(14 + au.size(), '\0');
            b[0] = 1;
            b[1] = key ? 1 : 0;
            memcpy(&b[2], &seq, 4);
            memcpy(&b[6], &capMs, 8);
            memcpy(&b[14], au.data(), au.size());
            unacked.emplace_back(seq, now);
            if (unacked.size() > 600) unacked.pop_front();
            ++seq, ++sent, sentBytes += int64_t(b.size());
            qVideo += b.size();
            q.push_back(Item{Video, std::move(b)});
        }
        cv.notify_one();
        return true;
    }
    void ack(uint32_t n) {
        std::lock_guard<std::mutex> lk(m);
        while (!unacked.empty() && int32_t(unacked.front().first - n) <= 0) unacked.pop_front();
    }
    size_t queued() {
        std::lock_guard<std::mutex> lk(m);
        return q.size();
    }
    void runWriter() {
        for (;;) {
            Item it;
            WsConn* w = nullptr;
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] { return stop || !q.empty(); });
                if (stop) return;
                it = std::move(q.front());
                q.pop_front();
                if (it.kind == Video) qVideo -= it.b.size();
                w = ws;
            }
            if (!w) return;
            if (testKbps > 0) {  // tests: as slow as a mobile link (the backlog builds here, like behind a real one)
                const int64_t now = nowUs();
                if (nextSendUs > now) Sleep(DWORD((nextSendUs - now) / 1000));
                nextSendUs = std::max(now, nextSendUs) + int64_t(it.b.size()) * 8000 / testKbps;
            }
            const bool ok = it.kind == Text ? w->sendText(it.b) : w->sendBinary(it.b.data(), it.b.size());
            if (!ok) {
                std::lock_guard<std::mutex> lk(m);
                stop = true;
                return;
            }
        }
    }
    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(m);
            stop = true;
        }
        cv.notify_all();
    }
    void closeWs() {  // from another thread: the page sees the connection end
        std::lock_guard<std::mutex> lk(m);
        if (ws) ws->close();
    }
};

// ---- session ------------------------------------------------------------------

struct Session {
    enum class State { Pending, Allowed, Denied, Ended };
    std::string id, device, label, peer, local, ua;
    bool tailscale = false;
    bool viaTunnel = false;        // came through the 外網連結 (0.8.1)
    bool viaFixed = false;         // came through the fixed link with the password (常駐外網, 0.8.2)
    std::string cred;              // ... its own path there
    std::atomic<std::shared_ptr<TunnelLink>> link;  // the tunnel's WebSocket, while open
    std::atomic<bool> pcUp{false}; // the direct (WebRTC) path is connected
    int quality = 2, monitor = 0;
    State state = State::Pending;  // guarded by Impl::smu
    bool announced = false;        // onRequest / onActive was called (onEnded follows once)
    bool activeNotified = false;
    std::string endWhy;            // set before a bye goes out: the reason wins over "disconnected"
    int64_t lastPollUs = 0;        // pending: the page's last poll (gone after 25 s silence)
    std::atomic<int64_t> lastActUs{0};   // last touch / input on the phone (idle disconnect, frame rate)
    std::atomic<bool> paused{false};     // page in the background: no video, no sound
    bool audioBeforePause = false;
    std::atomic<bool> clipWatch{false};  // the page's clipboard panel is open: push PC clipboard changes
    std::atomic<int64_t> moveSeq{0};     // last applied "move" (the page's cursor prediction)
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> video, audio;
    std::shared_ptr<rtc::DataChannel> dc;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtpV, rtpA;
    std::atomic<bool> videoOpen{false}, audioOpen{false}, dcOpen{false}, closed{false}, wantAudio{false};
    int64_t startUs = 0;
    std::atomic<bool> firstFrameSent{false};
    std::atomic<int64_t> lastMsgUs{0};  // last DataChannel message (pings every 500 ms)
    std::atomic<int64_t> lastIdrAskUs{0};  // 0.8.3.1: the page's last {"t":"idr"} (rate limit)
    std::mutex gmu;
    std::condition_variable gcv;
    bool gathered = false;
    // 0.8.3
    std::string jti;                      // fixed link: the login token's id (中斷 revokes it)
    std::atomic<bool> unlocked{false};    // fixed link: 僅觀看 lifted for this session with the password again
    std::atomic<int64_t> discUs{0};       // WebRTC "disconnected" since (LAN: closed only if it stays so for 4 s)
    // 0.8.3.2 臨時帳號: came in with a guest login (fixed link): always 允許, its rights enforced here
    std::string guestId, guestName;
    bool guestViewOnly = false, guestFiles = false;
    std::atomic<bool> guestDone{false};   // its end was handled (a 用一次 account is spent)
    bool guest() const { return !guestId.empty(); }
    bool filesOk() const { return !guest() || guestFiles; }

    Peer toPeer() const { return Peer{id, device, peer, label, tailscale, viaTunnel && !viaFixed, viaFixed, guestName, guestId}; }
};

struct Host::Impl {
    Options opt;
    std::string token, bindIp, tsIp;
    uint32_t lanAddr = 0, lanMask = 0;
    HttpServer http;
    std::unique_ptr<InputInjector> input;
    std::atomic<bool> running{false}, viewOnly{false}, stampOn{false};

    std::mutex smu;  // session / pending / device sets
    std::condition_variable pcv;  // pending state changes (long-polls wait on it)
    std::shared_ptr<Session> session;  // allowed one (connecting / connected)
    std::shared_ptr<Session> pending;  // waiting for 允許 / 拒絕
    std::set<std::string> allowedDevices;  // allowed while this host runs
    std::set<std::string> kickedDevices;   // kicked: ask again even if remembered
    std::deque<std::pair<std::string, int64_t>> denied;  // device, when (no new dialog for 20 s)
    // Wrong-link lockout (F5): per address, failures in the last 10 min.
    std::mutex bmu;
    std::map<std::string, std::deque<int64_t>> badTries;
    std::map<std::string, int64_t> blockedUntil;
    DWORD clipSeq = 0;  // GetClipboardSequenceNumber last pushed to the page
    std::mutex fcmu;        // testClipboard
    std::string fakeClip;
    DWORD fakeSeq = 1;
    bool clipGet(std::string& out) {
        if (opt.testClipboard) {
            std::lock_guard<std::mutex> lk(fcmu);
            out = fakeClip;
            return true;
        }
        std::wstring w;
        const bool ok = getClipboardText(w);
        out = narrow(w);
        return ok;
    }
    bool clipSet(const std::string& text) {
        if (opt.testClipboard) {
            std::lock_guard<std::mutex> lk(fcmu);
            fakeClip = text, ++fakeSeq;
            return true;
        }
        return setClipboardText(widen(text));
    }
    DWORD clipSeqNow() {
        if (opt.testClipboard) {
            std::lock_guard<std::mutex> lk(fcmu);
            return fakeSeq;
        }
        return GetClipboardSequenceNumber();
    }

    // Connections ended inside a libdatachannel callback: closed and released
    // later on another thread (housekeeping, stop).
    std::vector<std::shared_ptr<rtc::PeerConnection>> graveyard;

    std::thread capThread;
    std::atomic<bool> stop{false};
    H264Encoder enc;
    LoopbackAudio audio;
    std::mutex amu;  // audio start / stop
    std::atomic<int> deskW{0}, deskH{0}, outW{0}, outH{0}, curMonitor{0}, curQuality{2};
    std::atomic<int64_t> audioPackets{0}, audioBytes{0};
    // sender thread (encoder thread never blocks on the network)
    struct Encoded {
        std::vector<uint8_t> au;
        int64_t captureUs = 0;
        bool idr = false;
    };
    std::mutex qmu;
    std::condition_variable qcv;
    std::deque<Encoded> sendQ;
    bool stopSender = false;
    std::atomic<bool> waitIdr{false};
    std::thread sendThread;
    // stats
    Samples c2s, c2e, encMs, renderMs;
    std::atomic<int64_t> bytes{0}, framesSent{0}, idrs{0}, maxAu{0}, queueDrops{0};
    int64_t winFrames = 0, winBytes = 0, winMaxAu = 0, winIdrs = 0, winQDrops = 0;
    std::vector<Samples::Summary> winC2s, winC2e, winEnc, winRender;
    std::mutex tmu;
    std::deque<std::pair<int64_t, int64_t>> encStart;  // captureUs -> encode() call time
    CpuMeter cpu, cpuLog;
    bool mfStarted = false;

    HttpServer::Response route(const HttpServer::Request& r);
    HttpServer::Response page(const HttpServer::Request& r, const std::string& prefix, const std::string& cred);
    HttpServer::Response sessionRequest(const HttpServer::Request& r, const std::string& cred);
    HttpServer::Response offerFor(const std::shared_ptr<Session>& s);
    HttpServer::Response answer(const HttpServer::Request& r);
    HttpServer::Response snap(const HttpServer::Request& r);
    bool buildPeerConnection(const std::shared_ptr<Session>& s, std::string& sdp);
    void onDcMessage(const std::shared_ptr<Session>& s, const std::string& m);
    void onDcMessageImpl(const std::shared_ptr<Session>& s, const std::string& m);
    void closeSession(const std::shared_ptr<Session>& s, const std::string& why, bool fromCallback = false);
    void endPending(const std::string& why);
    void notifyEnded(const std::shared_ptr<Session>& s, const std::string& why);
    void setAudio(const std::shared_ptr<Session>& s, bool on);
    void stopAudioNow(const char* why);
    std::string audioSrc;          // Options::audioSource (guarded by amu)
    int64_t audioOffSinceUs = 0;   // the phone turned the sound off then (capture closes 15 s later)
    void sendDc(const std::shared_ptr<Session>& s, const json& j);
    // After a bye: wait (max 400 ms) until the channel has handed it to the
    // network, plus a moment on the wire, so the page learns why it ended.
    void flushBye(const std::shared_ptr<Session>& s) {
        if (!s) return;
        if (auto l = s->link.load(); l && !(s->dcOpen && s->dc)) {
            for (int i = 0; i < 40 && l->queued(); ++i) Sleep(10);
            Sleep(60);
            return;
        }
        if (!s->dcOpen || !s->dc) return;
        for (int i = 0; i < 40; ++i) {
            size_t b = 0;
            try {
                b = s->dc->bufferedAmount();
            } catch (...) {
            }
            if (b == 0) break;
            Sleep(10);
        }
        Sleep(60);
    }
    void prewarm();
    std::thread warmThread;
    // ---- 常駐外網 (0.8.2) ----
    std::atomic<bool> fixedOn{false};
    std::string fixedHash, fixedSecret;  // guarded by tunMu
    bool fixedApproval = false;          // guarded by tunMu
    // 0.8.3.2 臨時帳號 (guarded by tunMu): the accounts, the spent 用一次 ones, and the guest logins
    // (token nonce "g..." -> account id): a token is only good while its account is.
    std::vector<GuestAccount> guests;
    std::set<std::string> spentGuests;
    std::map<std::string, std::string> guestTokens;
    bool guestFor(const std::string& tok, GuestAccount& out);  // the account behind a valid guest login (false: none / gone)
    void guestSessionEnded(const std::shared_ptr<Session>& s);
    static bool guestToken(const std::string& tok) { return tokenId(tok).rfind('g', 0) == 0; }
    std::mutex loginMu;                  // one password check at a time (it also paces guessing)
    // 0.8.3: wrong passwords are counted per address (5 in a row = that address out
    // for 15 minutes); for everyone together only a pace (more than 20 wrong in 15
    // minutes from all addresses = 3 s per try), never a lock: a stranger can no
    // longer lock the owner out (0.8.2 locked every address after 5).
    struct PwIp {
        int fails = 0;
        int64_t lockedUntil = 0, lastUs = 0;
    };
    std::map<std::string, PwIp> pwIps;   // guarded by loginMu
    std::deque<int64_t> pwWrongAll;      // guarded by loginMu
    struct PwResult {
        bool ok = false, locked = false, noPassword = false;
        int left = 0, retryAfter = 0, delayMs = 0;
    };
    // 0.8.3.2: verify (a guest login) replaces the owner's password check; the limits are the same.
    PwResult checkPassword(const std::string& ip, const std::string& pw, int64_t now, const char* what,
                           const std::function<bool(const std::string&)>& verify = {});
    // 0.8.3: logins ended by 中斷 (their token id -> expiry, epoch seconds); guarded by tunMu.
    std::map<std::string, long long> revoked;
    HttpServer::Response routeFixed(const HttpServer::Request& r);
    HttpServer::Response login(const HttpServer::Request& r, int64_t now);
    HttpServer::Response guestLogin(const HttpServer::Request& r, int64_t now, const std::string& user, const std::string& pw,
                                    const std::string& secret);
    std::string makeToken(const std::string& secret) const;
    static bool tokenOk(const std::string& tok, const std::string& secret);
    bool tokenRevoked(const std::string& tok);
    static std::string tokenId(const std::string& tok) {  // "<exp>.<nonce>.<mac>" -> nonce
        const size_t a = tok.find('.'), b = tok.rfind('.');
        return a == std::string::npos || b == a ? std::string() : tok.substr(a + 1, b - a - 1);
    }
    static long long tokenExp(const std::string& tok) { return atoll(tok.substr(0, tok.find('.')).c_str()); }
    // 0.8.3: 解除僅觀看 from the phone (fixed link, password again): checked off the DataChannel's thread.
    std::thread unlockThread;
    std::atomic<bool> unlockBusy{false};
    void unlockRequest(const std::shared_ptr<Session>& s, const std::string& pw);
    std::atomic<bool> lockOutside{false}, lockLan{false};  // 0.8.3: the app locks the PC when a session ends (the page says so)
    // 僅觀看 for this session (0.8.3.2: a 僅觀看 guest always, whatever the PC's switch says)
    bool vo(const std::shared_ptr<Session>& s) const { return (viewOnly && !(s && s->unlocked)) || (s && s->guestViewOnly); }
    std::mutex offerMu;  // 0.8.3: one session set-up at a time (two at once left an orphan that could still control)
    // ---- 外網連結 (0.8.1) ----
    mutable std::mutex tunMu;
    bool tunnelOn = false;
    std::string passcode, otk;
    int64_t otkUntil = 0;
    struct Cred {
        int64_t created = 0;
        bool verified = false;
        int wrong = 0;
        std::string ip;
    };
    std::map<std::string, Cred> creds;
    int tunnelKbps = 0;  // lowered on the tunnel when it is full (0 = the preset)
    int64_t lastCutUs = 0, lastDropUs = 0, lastRaiseUs = 0, lastKeyAskUs = 0;
    void newOtkLocked(int64_t now) {
        otk = randomString(24);
        otkUntil = now + 600000000LL;  // 10 minutes
    }
    bool badTry(const std::string& ip, int64_t now, const char* what);
    HttpServer::Response routeRest(const HttpServer::Request& r, const std::string& rest, const std::string& prefix, const std::string& cred);
    HttpServer::Response routeTunnel(const HttpServer::Request& r);
    HttpServer::Response passcodeCheck(const HttpServer::Request& r, const std::string& cred, int64_t now);
    HttpServer::Response websocket(const HttpServer::Request& r, const std::string& cred);
    std::string linkGonePage() const;
    void markActive(const std::shared_ptr<Session>& s, const char* via);
    void tunnelFull(int64_t now, bool wantKey);
    void tunnelOk(int64_t now);
    static bool wsMedia(const std::shared_ptr<Session>& s) {
        auto l = s->link.load();
        return l && l->media;
    }
    HttpServer::Response candidate(const HttpServer::Request& r);
    // ---- capture only while a phone is there (0.8.3) ----
    // 0.8.2 opened the capture + the hardware encoder at start and kept them (owner,
    // 常駐外網 on, no phone: 118 threads, 246 MB private, ~120 MB VRAM). Now a session
    // request (once its link / password is right) wakes the capture thread, which
    // opens them for that session's monitor and quality while ICE runs; 30 s after
    // the last session they are released and the thread sleeps on capCv.
    std::mutex capMu;
    std::condition_variable capCv;
    int hintQuality = -1, hintMonitor = 0;  // guarded by capMu: a session on its way
    int64_t hintUs = 0;
    void wakeCapture(int quality, int monitor) {
        {
            std::lock_guard<std::mutex> lk(capMu);
            hintQuality = quality, hintMonitor = monitor, hintUs = nowUs();
        }
        capCv.notify_all();
    }
    // ---- files (0.8.3) ----
    struct Upload {
        std::wstring part;      // <uploadDir>\<clean name>.<upload id>.part while it comes in
        std::wstring name;      // the clean name
        std::string session;
        uint64_t total = 0, got = 0;
        int64_t lastUs = 0;
    };
    struct Download {
        std::wstring path;
        std::string name, session;
        int64_t untilUs = 0;    // one use, 10 minutes
    };
    std::mutex fmu;
    std::map<std::string, Upload> uploads;    // upload id -> state
    std::map<std::string, Download> downloads;  // one-time key -> file
    HttpServer::Response upload(const HttpServer::Request& r);
    HttpServer::Response download(const HttpServer::Request& r, const std::string& key);
    void filesList(const std::shared_ptr<Session>& s);
    void dropUploads(const std::string& session, const char* why, int64_t olderThanUs);
    std::thread houseThread;  // housekeeping, once a second (0.8.3: no longer inside the capture loop)
    void houseLoop();
    std::atomic<bool> capOpen{false};
    int64_t lastIdleLogUs = 0;
    void captureLoop();
    void onEncoded(std::vector<uint8_t>&& au, int64_t captureUs, bool idr);
    void sendFrame(Encoded&& f);
    void senderLoop();
    void housekeeping(int64_t now);
    Quality effective(int q) const {
        Quality p = qualityPreset(q);
        if (opt.maxWidth > 0 && opt.maxHeight > 0) p.maxWidth = opt.maxWidth, p.maxHeight = opt.maxHeight;
        if (opt.fps > 0) p.fps = opt.fps;
        if (opt.kbps > 0) p.kbps = opt.kbps;
        return p;
    }
    std::shared_ptr<Session> current() {
        std::lock_guard<std::mutex> lk(smu);
        return session;
    }
};

Host::Host() : d_(std::make_unique<Impl>()) {}
Host::~Host() { stop(); }

std::string Host::url() const {
    return d_->running ? "http://" + d_->bindIp + ":" + std::to_string(d_->http.port()) + "/" + d_->token + "/" : "";
}
std::string Host::tailscaleUrl() const {
    return d_->running && !d_->tsIp.empty() ? "http://" + d_->tsIp + ":" + std::to_string(d_->http.port()) + "/" + d_->token + "/"
                                            : "";
}
std::string Host::ip() const { return d_->bindIp; }
uint16_t Host::port() const { return d_->http.port(); }
bool Host::running() const { return d_->running; }
bool Host::viewOnly() const {
    std::lock_guard<std::mutex> lk(d_->smu);
    return d_->viewOnly && !(d_->session && d_->session->unlocked);
}
bool Host::viewOnlyUnlocked() const {
    std::lock_guard<std::mutex> lk(d_->smu);
    return d_->session && d_->session->unlocked;
}

bool Host::active() const {
    std::lock_guard<std::mutex> lk(d_->smu);
    return d_->session && d_->session->activeNotified && !d_->session->closed;
}

Peer Host::peer() const {
    std::lock_guard<std::mutex> lk(d_->smu);
    if (d_->session && !d_->session->closed) return d_->session->toPeer();
    if (d_->pending) return d_->pending->toPeer();
    return {};
}

int64_t Host::msSinceRemoteInput() const {
    if (!d_->input) return -1;
    const int64_t t = d_->input->lastPressUs();
    return t ? (nowUs() - t) / 1000 : -1;
}

void Host::notifyPhone(const std::string& what) {
    if (auto s = d_->current()) d_->sendDc(s, json{{"t", "notice"}, {"what", what}});
}

bool Host::start(const Options& o) {
    stop();
    Impl& d = *d_;
    d.opt = o;
    d.stop = false;
    d.viewOnly = o.viewOnly;
    d.stampOn = o.stamp;
    d.curQuality = std::clamp(o.quality, 0, kQualityLevels - 1);
    d.curMonitor = std::max(0, o.monitor);
    d.audioSrc = o.audioSource;
    if (o.log) setLogHandler(o.log);
    rtc::InitLogger(rtc::LogLevel::Warning, [](rtc::LogLevel lvl, std::string m) { logf("rtc[%d]: %s", int(lvl), m.c_str()); });
    d.token = o.token.empty() ? randomString(32) : o.token;
    {
        std::lock_guard<std::mutex> lk(d.fcmu);
        d.fakeClip = o.testClipboardText, d.fakeSeq = 1;
    }
    {
        std::lock_guard<std::mutex> lk(d.smu);
        d.allowedDevices.clear(), d.kickedDevices.clear(), d.denied.clear();
        d.session.reset(), d.pending.reset();
    }
    // LAN interface: same choice as 傳到手機 (real LAN adapters first), never
    // the Tailscale adapter (that one is the second, optional bind).
    const auto lans = o.loopbackTest ? std::vector<pm::share::LanInterface>{} : pm::share::lanInterfaces();
    d.bindIp.clear();
    if (o.loopbackTest) {  // 0.8.3.1 tests: 127.0.0.3 stands in for the LAN (no firewall prompt)
        in_addr a{}, m{};
        inet_pton(AF_INET, "127.0.0.3", &a);
        inet_pton(AF_INET, "255.0.0.0", &m);
        d.bindIp = "127.0.0.3", d.lanAddr = a.s_addr, d.lanMask = m.s_addr;
        d.opt.stunServers.clear();  // (nothing to reach from 127.0.0.3)
        logf("host: loopback test mode: the LAN side is 127.0.0.3 (nothing listens on the LAN)");
    }
    for (const auto& l : lans) {
        if (isTailnet(l.addr)) continue;
        if (o.bindIp.empty() || l.ip == o.bindIp) {
            d.bindIp = l.ip, d.lanAddr = l.addr, d.lanMask = l.mask;
            break;
        }
    }
    if (d.bindIp.empty()) {
        logf("host: no LAN interface%s", o.bindIp.empty() ? "" : (" matching " + o.bindIp).c_str());
        return false;
    }
    for (const auto& l : lans) logf("host: LAN interface %s %s%s", l.ip.c_str(), l.name.c_str(), l.ip == d.bindIp ? "  <- used" : "");
    std::vector<HttpServer::Bind> binds;
    binds.push_back({d.bindIp, d.lanAddr, d.lanMask, false});
    {
        // 外網連結 (0.8.1): cloudflared on this PC forwards the tunnel to 127.0.0.1:<port>.
        // Nothing is served there until setTunnel(true).
        in_addr lo{};
        inet_pton(AF_INET, "127.0.0.1", &lo);
        binds.push_back({"127.0.0.1", lo.s_addr, 0, false, true});
        // 常駐外網 (0.8.2): the fixed link's origin (Tailscale Funnel :8443 / cloudflared fallback).
        in_addr lo2{};
        inet_pton(AF_INET, "127.0.0.2", &lo2);
        binds.push_back({"127.0.0.2", lo2.s_addr, 0, false, true, true});
    }
    d.tsIp.clear();
    if (o.tailscale && !o.loopbackTest) {
        const std::string ts = tailscaleIp();
        if (!ts.empty()) {
            in_addr a{};
            inet_pton(AF_INET, ts.c_str(), &a);
            binds.push_back({ts, a.s_addr, 0, true});
        } else {
            logf("host: no Tailscale address on this PC (only the LAN address is served)");
        }
    }
    d.input = std::make_unique<InputInjector>(o.inject);
    d.input->guard = o.injectGuard;
    d.input->moveGuard = o.injectMoveGuard;
    logf("host: input %s%s", o.inject ? "INJECTION ON (SendInput moves the real mouse/keyboard)" : "dry-run (events are only logged)",
         o.injectGuard ? ", test guard: only into the test window" : "");
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return logf("host: MFStartup failed"), false;
    d.mfStarted = true;
    // 0.8.3: a body over 64 KB only for the tests' snapshot upload on the right LAN link.
    d.http.setBodyLimit([&d](const HttpServer::Request& r) -> size_t {
        const std::string prefix = "/" + d.token + "/";
        if (!d.opt.snapDir.empty() && !r.tunnel && r.method == "POST" && r.path == prefix + "snap") return 8u * 1024 * 1024;
        // 0.8.3: a file piece (at most 4 MB) only from the current session (its id is the session's secret).
        if (r.method == "POST" && r.path.size() > 3 && r.path.compare(r.path.size() - 3, 3, "/up") == 0 && !d.opt.uploadDir.empty()) {
            auto s = d.current();
            if (s && !s->closed && constantTimeEq(queryParam(r.query, "id"), s->id)) return 4u * 1024 * 1024 + 4096;
        }
        return 64u * 1024;
    });
    d.http.setFixedViaCloudflared(false);
    if (!d.http.start(binds, o.port, [&d](const HttpServer::Request& r) { return d.route(r); })) {
        MFShutdown(), d.mfStarted = false;
        return false;
    }
    for (const auto& ip : d.http.boundIps())
        if (ip != d.bindIp && ip.rfind("127.", 0) != 0) d.tsIp = ip;
    {
        std::lock_guard<std::mutex> lk(d.tunMu);
        d.tunnelOn = false, d.creds.clear(), d.passcode.clear(), d.otk.clear();
        d.fixedHash = o.fixedPasswordHash, d.fixedSecret = o.fixedSecret, d.fixedApproval = o.fixedNeedApproval;
        d.guests = o.guests, d.guestTokens.clear(), d.spentGuests.clear();
        d.fixedOn = false;
        d.tunnelKbps = 0;
        d.revoked.clear();
        const long long nowS = epochMs() / 1000;
        for (const std::string& e : o.revokedLogins) {  // "<token id>:<expiry>"
            const size_t c = e.find(':');
            const long long exp = c == std::string::npos ? 0 : atoll(e.c_str() + c + 1);
            if (c != std::string::npos && c > 0 && exp > nowS) d.revoked[e.substr(0, c)] = exp;
        }
    }
    {
        std::lock_guard<std::mutex> lk(d.loginMu);
        d.pwIps.clear(), d.pwWrongAll.clear();
    }
    {
        std::lock_guard<std::mutex> lk(d.capMu);
        d.hintQuality = -1, d.hintUs = 0;
    }
    d.stopSender = false;
    d.running = true;
    d.warmThread = std::thread([&d] { d.prewarm(); });
    if (!o.inlineSend) d.sendThread = std::thread([&d] { d.senderLoop(); });
    d.capThread = std::thread([&d] { d.captureLoop(); });
    d.houseThread = std::thread([&d] { d.houseLoop(); });
    logf("host: started, approval %s, view-only %s, audio %s, quality %d", o.requireApproval ? "required for new devices" : "OFF (test)",
         o.viewOnly ? "on" : "off", o.audio ? "offered" : "off", int(d.curQuality));
    return true;
}

void Host::stop() {
    Impl& d = *d_;
    if (!d.running && !d.capThread.joinable() && !d.sendThread.joinable()) return;
    d.stop = true;
    d.pcv.notify_all();  // long-polls return at once
    if (auto s = d.current()) {
        // The page says 「電腦端已關閉遠端操控」 only if this bye arrives: let it
        // leave before the connection goes (0.8.0 closed 4 ms later, the phone
        // saw a plain disconnect and tried to reconnect).
        d.sendDc(s, json{{"t", "bye"}, {"why", "stopped"}});
        d.flushBye(s);
        d.closeSession(s, "host stopping");
    }
    d.endPending("host stopping");
    d.http.stop();
    if (d.warmThread.joinable()) d.warmThread.join();
    if (d.unlockThread.joinable()) d.unlockThread.join();
    {
        std::lock_guard<std::mutex> lk(d.amu);
        d.audio.stop();
    }
    d.capCv.notify_all();
    if (d.houseThread.joinable()) d.houseThread.join();
    if (d.capThread.joinable()) d.capThread.join();
    d.enc.close();
    {
        std::lock_guard<std::mutex> lk(d.qmu);
        d.stopSender = true;
        d.sendQ.clear();
    }
    d.qcv.notify_all();
    if (d.sendThread.joinable()) d.sendThread.join();
    if (d.input) d.input->releaseAll(), d.input->flushLog(true);
    {
        std::vector<std::shared_ptr<rtc::PeerConnection>> graves;
        {
            std::lock_guard<std::mutex> lk(d.smu);
            graves.swap(d.graveyard);
            d.session.reset(), d.pending.reset();
        }
        for (auto& pc : graves) {
            try {
                pc->close();
            } catch (...) {
            }
        }
    }
    if (d.mfStarted) MFShutdown(), d.mfStarted = false;
    d.running = false;
    logf("host: stopped");
}

void Host::approve(const std::string& session, bool allow, bool remember) {
    Impl& d = *d_;
    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lk(d.smu);
        if (!d.pending || d.pending->id != session || d.pending->state != Session::State::Pending) return;
        s = d.pending;
        if (allow) {
            s->state = Session::State::Allowed;
            if (!s->guest()) d.allowedDevices.insert(s->device);  // 0.8.3.2: a guest asks every time
            d.kickedDevices.erase(s->device);
        } else {
            s->state = Session::State::Denied;
            d.pending.reset();
            d.denied.emplace_back(s->device, nowUs());
        }
    }
    logf("host: %s %s (%s)%s%s%s", allow ? "ALLOWED" : "DENIED", s->label.c_str(), s->peer.c_str(),
         allow && remember && !s->guest() ? ", remembered" : "", s->guest() ? ", 臨時帳號 " : "", s->guestName.c_str());
    d.pcv.notify_all();
    if (!allow) d.notifyEnded(s, "denied");
}

void Host::kick(const std::string& why) {
    Impl& d = *d_;
    if (auto s = d.current()) {
        {
            std::lock_guard<std::mutex> lk(d.smu);
            d.allowedDevices.erase(s->device);
            d.kickedDevices.insert(s->device);  // the next connection asks again
        }
        // 0.8.3: the fixed link's login goes too. The device id is the page's own (a new
        // one is just a new random string), so 0.8.2's "kicked device" alone let the same
        // login straight back in; now that phone needs the password again.
        if (s->viaFixed && !s->jti.empty()) {
            long long exp = 0;
            {
                std::lock_guard<std::mutex> lk(d.tunMu);
                exp = Impl::tokenExp(s->cred.substr(6));
                d.revoked[s->jti] = exp;
                if (d.revoked.size() > 200) d.revoked.erase(d.revoked.begin());
            }
            logf("fixed: the login of %s (%s) is revoked: the password is needed again", s->label.c_str(), s->peer.c_str());
            if (d.opt.onLoginRevoked) d.opt.onLoginRevoked(s->jti, exp);
        }
        logf("host: session of %s (%s) ended on the PC: %s", s->label.c_str(), s->peer.c_str(), why.c_str());
        {
            std::lock_guard<std::mutex> lk(d.smu);
            s->endWhy = "kicked: " + why;
        }
        d.sendDc(s, json{{"t", "bye"}, {"why", "kicked"}});
        d.flushBye(s);  // let the bye go out before the close
        d.closeSession(s, "kicked: " + why);
    }
    d.endPending("kicked: " + why);
}

void Host::setViewOnly(bool on) {
    Impl& d = *d_;
    d.viewOnly = on;
    auto s = d.current();
    // 僅觀看 turned on on the PC also ends a 解除 the phone did with the password (0.8.3).
    if (on && s && s->unlocked.exchange(false)) logf("host: view-only back on from the PC (the phone's 解除 ends)");
    if (on && s) s->clipWatch = false;  // 0.8.3: no clipboard in 僅觀看
    if (on && d.input) d.input->releaseAll();
    logf("host: view-only %s", on ? "ON (input dropped)" : "off");
    if (s) d.sendDc(s, json{{"t", "mode"}, {"viewOnly", d.vo(s)}});
}

// ---- HTTP ----------------------------------------------------------------

HttpServer::Response Host::Impl::route(const HttpServer::Request& r) {
    const std::string prefix = "/" + token + "/";
    const int64_t now = nowUs();
    {
        // Shut out for a while after too many wrong links (F5).
        std::lock_guard<std::mutex> lk(bmu);
        if (auto it = blockedUntil.find(r.peer); it != blockedUntil.end()) {
            if (now < it->second) return {429, "text/plain; charset=utf-8", "too many wrong links, try again later"};
            blockedUntil.erase(it);
        }
    }
    if (r.fixed) return routeFixed(r);    // 常駐外網: the password link, never the LAN one
    if (r.tunnel) return routeTunnel(r);  // 外網連結: its own links only, never the LAN one
    if (r.path.size() < prefix.size() || !constantTimeEq(r.path.substr(0, prefix.size()), prefix)) {
        // Browsers ask for these by themselves (no token): not an attempt; the icons have no secret.
        if (std::string type, body; r.method == "GET" && (r.path == "/favicon.ico" || r.path.rfind("/apple-touch-icon", 0) == 0) &&
                                    webAsset(r.path.substr(1), type, body))
            return {200, type, body};
        if (r.path == "/favicon.ico" || r.path.rfind("/apple-touch-icon", 0) == 0) return {404, "text/plain", "not found"};
        logf("http: %s %s %s -> 403 (no / wrong token)", r.peer.c_str(), r.method.c_str(), r.path.substr(0, 12).c_str());
        badTry(r.peer, now, "a wrong link");
        return {403, "text/plain; charset=utf-8", "forbidden"};
    }
    return routeRest(r, r.path.substr(prefix.size()), prefix, "");
}

// Counts a wrong link / 通行碼 from an address; true = it is shut out now (10 in 10 minutes -> 15 minutes).
bool Host::Impl::badTry(const std::string& ip, int64_t now, const char* what) {
    int fails = 0;
    bool block = false;
    {
        std::lock_guard<std::mutex> lk(bmu);
        auto& q = badTries[ip];
        q.push_back(now);
        while (!q.empty() && now - q.front() > 600000000LL) q.pop_front();  // 10 minutes
        fails = int(q.size());
        if (fails >= kBadTriesToBlock) {
            blockedUntil[ip] = now + 900000000LL;  // 15 minutes
            q.clear();
            block = true;
        }
        // Bounded. 0.8.3: the address with the fewest tries goes (the oldest of those): 0.8.2
        // cleared every count once 257 addresses had tried, so a few hundred throw-away
        // addresses reset the count of the one that was about to be blocked.
        while (badTries.size() > 256) {
            auto pick = badTries.end();
            for (auto it2 = badTries.begin(); it2 != badTries.end(); ++it2) {
                if (it2->first == ip) continue;
                if (pick == badTries.end() || it2->second.size() < pick->second.size() ||
                    (it2->second.size() == pick->second.size() && !it2->second.empty() && !pick->second.empty() && it2->second.back() < pick->second.back()))
                    pick = it2;
            }
            if (pick == badTries.end()) break;
            badTries.erase(pick);
        }
        if (blockedUntil.size() > 256)  // expired blocks go (they are also dropped on the next request)
            for (auto it2 = blockedUntil.begin(); it2 != blockedUntil.end();) it2 = now >= it2->second ? blockedUntil.erase(it2) : std::next(it2);
    }
    if (block) {
        logf("http: %s used %s %d times in 10 minutes -> blocked for 15 minutes", ip.c_str(), what, fails);
        if (opt.onBlocked) opt.onBlocked(ip, fails);
    }
    return block;
}

// The page's routes after its secret prefix (LAN: /<token>/; tunnel: /c/<its path>/).
HttpServer::Response Host::Impl::routeRest(const HttpServer::Request& r, const std::string& rest, const std::string& prefix,
                                           const std::string& cred) {
    if (rest.empty() && (r.method == "GET" || r.method == "HEAD")) return page(r, prefix, cred);
    if (!cred.empty() && r.method == "GET" && rest == "ws") return websocket(r, cred);
    if (r.method == "GET" && rest == "trackpad.js")
        return {200, "text/javascript; charset=utf-8",
                std::string(reinterpret_cast<const char*>(kRemoteTrackpadJs), sizeof(kRemoteTrackpadJs))};
    if (r.method == "GET" && rest == "vkbd.js")
        return {200, "text/javascript; charset=utf-8", std::string(reinterpret_cast<const char*>(kRemoteVkbdJs), sizeof(kRemoteVkbdJs))};
    if (r.method == "GET" && rest == "help" && !opt.helpFile.empty()) {
        FILE* f = _wfopen(opt.helpFile.c_str(), L"rb");
        if (!f) return {404, "text/plain", "no guide"};
        std::string body;
        char buf[65536];
        for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0 && body.size() < 16 * 1024 * 1024;) body.append(buf, n);
        fclose(f);
        return {200, "text/html; charset=utf-8", body};
    }
    if (std::string type, body; r.method == "GET" && webAsset(rest, type, body)) return {200, type, body};  // 加到主畫面
    if (r.method == "GET" && rest.rfind("dl/", 0) == 0) return download(r, rest.substr(3));  // 0.8.3: a copied file, one-time key
    if (r.method != "POST") return {405, "text/plain", "method not allowed"};
    if (rest == "session") return sessionRequest(r, cred);
    if (rest == "answer") return answer(r);
    if (rest == "candidate") return candidate(r);
    if (rest == "up") return upload(r);  // 0.8.3: a piece of a file from the phone
    if (rest == "snap") return snap(r);
    if (rest == "diag") {  // page capabilities / failures, works even when WebRTC does not
        std::string b = r.body.substr(0, 4096);
        for (char& c : b)
            if (c == '\r' || c == '\n') c = ' ';
        logf("page-diag %s: %s", r.peer.c_str(), b.c_str());
        // A plain reason the page worked out (F6): tell the PC too.
        json j = json::parse(b, nullptr, false);
        if (!j.is_discarded() && j.is_object() && j.contains("reason") && j["reason"].is_string() && opt.onTrouble)
            opt.onTrouble(r.peer, j["reason"].get<std::string>().substr(0, 40));
        return {204, "text/plain", ""};
    }
    return {404, "text/plain", "not found"};
}

HttpServer::Response Host::Impl::page(const HttpServer::Request& r, const std::string& prefix, const std::string& cred) {
    logf("http: %s GET page via %s%s (%s)", r.peer.c_str(), r.local.c_str(), r.tunnel ? " (tunnel)" : r.tailscale ? " (Tailscale)" : "",
         r.headers.count("user-agent") ? r.headers.at("user-agent").c_str() : "?");
    std::string html(reinterpret_cast<const char*>(kRemotePageHtml), sizeof(kRemotePageHtml));
    // The two scripts inline: two fewer requests (each a new connection) before the page can start.
    html = replaceAll(html, "<script src=\"trackpad.js\"></script>",
                      "<script>" + std::string(reinterpret_cast<const char*>(kRemoteTrackpadJs), sizeof(kRemoteTrackpadJs)) + "</script>");
    html = replaceAll(html, "<script src=\"vkbd.js\"></script>",
                      "<script>" + std::string(reinterpret_cast<const char*>(kRemoteVkbdJs), sizeof(kRemoteVkbdJs)) + "</script>");
    json cfg{{"lang", std::clamp(opt.lang, 0, 3)},
             {"app", opt.appName},
             {"bg", hexColor(opt.pageBg)},
             {"accent", hexColor(opt.pageAccent)},
             {"fg", hexColor(opt.pageFg)},
             {"help", opt.helpFile.empty() ? opt.helpUrl : prefix + "help"},
             {"audio", opt.audio},
             {"approval", opt.requireApproval},
             {"tailscale", r.tailscale},
             {"quality", int(curQuality)}};
    if (cred.rfind("fixed:", 0) == 0) {  // 常駐外網: the page asks for the password (or uses its saved login)
        cfg["tunnel"] = true;
        cfg["fixed"] = true;
        cfg["verified"] = false;
        cfg["ice"] = opt.stunServers;
    } else if (!cred.empty()) {
        bool verified = false;
        {
            std::lock_guard<std::mutex> lk(tunMu);
            if (auto it = creds.find(cred); it != creds.end()) verified = it->second.verified;
        }
        cfg["tunnel"] = true;
        cfg["verified"] = verified;
        cfg["ice"] = opt.stunServers;
    }
    html = replaceAll(html, "/*PM_CONFIG*/{}", scriptJson(cfg));
    // 0.8.3: a phone with the right link is on its way: the capture and the encoder start
    // opening now, while the page loads and asks for its session (first picture sooner).
    // Not for the fixed link's public "/" (anybody may load it): there after the login check.
    if (cred.empty() && r.method == "GET") wakeCapture(r.tailscale ? 0 : int(curQuality), 0);
    return {200, "text/html; charset=utf-8", html};
}

// POST session            {"device":"...","q":2,"m":0}  -> 200 offer | 202 pending | 403 denied
// POST session?id=<id>    long-poll while pending (up to ~15 s)
HttpServer::Response Host::Impl::sessionRequest(const HttpServer::Request& r, const std::string& cred) {
    json body = json::parse(r.body.empty() ? std::string("{}") : r.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) body = json::object();
    const std::string pollId = queryParam(r.query, "id");
    const std::string ua = r.headers.count("user-agent") ? r.headers.at("user-agent").substr(0, 300) : "";
    if (!pollId.empty()) {
        std::shared_ptr<Session> s;
        {
            std::unique_lock<std::mutex> lk(smu);
            s = pending && constantTimeEq(pollId, pending->id) ? pending : nullptr;
            if (!s) {
                // Denied (or replaced / expired): tell the page plainly.
                return {403, "application/json", json{{"denied", true}}.dump()};
            }
            s->lastPollUs = nowUs();
            pcv.wait_for(lk, std::chrono::seconds(15), [&] { return stop || s->state != Session::State::Pending; });
            s->lastPollUs = nowUs();
            if (stop) return {503, "text/plain", "stopping"};
            if (s->state == Session::State::Denied) return {403, "application/json", json{{"denied", true}}.dump()};
            if (s->state == Session::State::Pending) return {202, "application/json", json{{"id", s->id}, {"pending", true}}.dump()};
            if (pending == s) pending.reset();
        }
        return offerFor(s);
    }

    // 0.8.3: a string or nothing (json::value() throws on another type: a 500 instead of a 400)
    const std::string device = body.contains("device") && body["device"].is_string() ? body["device"].get<std::string>() : std::string();
    if (!validDevice(device)) return {400, "text/plain", "bad device id"};
    auto s = std::make_shared<Session>();
    s->id = randomString(16);
    s->device = device;
    s->ua = ua;
    s->label = deviceLabel(ua);
    s->peer = r.peer;
    s->local = r.local;
    s->tailscale = r.tailscale;
    s->viaTunnel = r.tunnel;
    s->cred = cred;
    const bool viaFixedReq = cred.rfind("fixed:", 0) == 0;
    s->viaFixed = viaFixedReq;
    bool fixedNeedsOk = false;
    {
        std::lock_guard<std::mutex> lk(tunMu);
        fixedNeedsOk = fixedApproval;
    }
    if (viaFixedReq) s->jti = tokenId(cred.substr(6));
    if (GuestAccount ga; viaFixedReq && guestToken(cred.substr(6))) {
        if (!guestFor(cred.substr(6), ga)) return {401, "application/json", json{{"needLogin", true}, {"guestGone", true}}.dump()};
        s->guestId = ga.id, s->guestName = ga.name, s->guestViewOnly = ga.viewOnly, s->guestFiles = ga.files;
    }
    {
        const json& q = body.contains("q") && body["q"].is_number_integer() ? body["q"] : json(int(curQuality));
        const json& m = body.contains("m") && body["m"].is_number_integer() ? body["m"] : json(0);
        s->quality = std::clamp(q.get<int>(), 0, kQualityLevels - 1);
        s->monitor = std::clamp(m.get<int>(), 0, std::max(0, int(enumMonitors().size()) - 1));
    }
    s->lastPollUs = nowUs();
    // 0.8.3: the capture + encoder open now, for this session's screen and quality,
    // while the PC asks / ICE runs (nothing is sent before the session is allowed).
    wakeCapture(s->quality, s->monitor);
    bool ask = false;
    std::shared_ptr<Session> replacedPending;
    {
        std::lock_guard<std::mutex> lk(smu);
        const int64_t now = nowUs();
        while (!denied.empty() && now - denied.front().second > 20000000) denied.pop_front();
        for (const auto& dn : denied)
            if (dn.first == device) {
                logf("host: %s (%s) was just denied, not asking again yet", s->label.c_str(), r.peer.c_str());
                return {403, "application/json", json{{"denied", true}}.dump()};
            }
        const bool kicked = kickedDevices.count(device) > 0;
        // 常駐外網: the right password lets a phone in without 允許 (unless the owner asked
        // for it: fixedNeedApproval), never one that was just 中斷'd.
        const bool passwordIn = viaFixedReq && !fixedNeedsOk && !kicked && !s->guest();
        // 0.8.3.2: a guest always waits for 允許 (never "allowed before" / remembered)
        const bool ok = !s->guest() && (!opt.requireApproval || allowedDevices.count(device) ||
                                        (!kicked && opt.remembered && opt.remembered(device)) || passwordIn);
        if (passwordIn && !allowedDevices.count(device))
            logf("host: %s from %s came in with the password (fixed link): no 允許 needed", s->label.c_str(), r.peer.c_str());
        if (ok) {
            s->state = Session::State::Allowed;
            allowedDevices.insert(device);
        } else {
            if (pending && pending->device == device && pending->state == Session::State::Pending) {
                // Same phone asking again (page reloaded): keep the open dialog.
                pending->lastPollUs = now;
                pending->peer = r.peer, pending->local = r.local, pending->tailscale = r.tailscale;
                pending->viaTunnel = r.tunnel, pending->cred = cred, pending->viaFixed = viaFixedReq;
                pending->guestId = s->guestId, pending->guestName = s->guestName;
                pending->guestViewOnly = s->guestViewOnly, pending->guestFiles = s->guestFiles;
                pending->quality = s->quality, pending->monitor = s->monitor;
                return {202, "application/json", json{{"id", pending->id}, {"pending", true}}.dump()};
            }
            if (pending) replacedPending = pending, pending->state = Session::State::Ended;
            pending = s;
            ask = true;
        }
    }
    if (replacedPending) {
        pcv.notify_all();
        notifyEnded(replacedPending, "replaced");
    }
    if (ask) {
        logf("host: NEW DEVICE asks to connect: %s from %s%s%s%s (device %s) -> waiting for 允許 / 拒絕 on the PC", s->label.c_str(),
             r.peer.c_str(), viaFixedReq ? " (fixed link)" : r.tunnel ? " (外網連結 tunnel)" : r.tailscale ? " (Tailscale)" : "",
             s->guest() ? ", 臨時帳號 " : "", s->guestName.c_str(), device.substr(0, 8).c_str());
        {
            std::lock_guard<std::mutex> lk(smu);
            s->announced = true;
        }
        if (opt.onRequest) opt.onRequest(s->toPeer());
        return {202, "application/json", json{{"id", s->id}, {"pending", true}}.dump()};
    }
    return offerFor(s);
}

HttpServer::Response Host::Impl::offerFor(const std::shared_ptr<Session>& s) {
    // 0.8.3: one at a time. Two requests at once each closed "the old one", built their
    // own connection and set itself current: the loser stayed connected, out of reach
    // of 中斷, and its DataChannel still drove the input.
    std::lock_guard<std::mutex> offerLock(offerMu);
    if (stop || s->state == Session::State::Ended) return {409, "text/plain", "no such session"};
    wakeCapture(s->quality, s->monitor);  // (again: 允許 may have taken longer than the capture's 30 s grace)
    if (auto old = current()) {
        {
            std::lock_guard<std::mutex> lk(smu);
            old->endWhy = "replaced by a new session";
        }
        sendDc(old, json{{"t", "bye"}, {"why", "replaced"}});
        closeSession(old, "replaced by a new session");
    }
    std::string sdp;
    if (!buildPeerConnection(s, sdp)) return {500, "text/plain", "session setup failed"};
    {
        std::lock_guard<std::mutex> lk(smu);
        session = s;
    }
    json mons = json::array();
    for (const Monitor& m : monitors()) mons.push_back(json{{"w", m.w}, {"h", m.h}, {"primary", m.primary}});
    json j{{"id", s->id},          {"type", "offer"},       {"sdp", sdp},   {"monitors", mons},
           {"monitor", s->monitor}, {"quality", s->quality}, {"viewOnly", vo(s)},
           {"audio", opt.audio},    {"inject", opt.inject},  {"unlockable", s->viaFixed && !s->guest()},
           {"lockAfter", s->viaTunnel ? bool(lockOutside) : bool(lockLan)},
           {"upload", !opt.uploadDir.empty() && s->filesOk()}, {"uploadMax", opt.uploadMaxBytes},
           {"guest", s->guestName}, {"files", s->filesOk()}};
    return {200, "application/json", j.dump()};
}

bool Host::Impl::buildPeerConnection(const std::shared_ptr<Session>& s, std::string& sdp) {
    s->startUs = nowUs();
    rtc::Configuration cfg;
    if (s->viaTunnel) {
        // 外網連結: direct WebRTC from wherever the phone is: public STUN (no
        // TURN; when hole punching fails the tunnel's WebSocket carries it).
        cfg.bindAddress = bindIp;
        for (const auto& u : opt.stunServers) cfg.iceServers.emplace_back(u);
    } else {
        cfg.bindAddress = s->local;  // host candidates on the address the page came in on; no STUN / TURN
    }
    cfg.disableAutoNegotiation = true;
    try {
        s->pc = std::make_shared<rtc::PeerConnection>(cfg);
        std::weak_ptr<Session> ws = s;
        s->pc->onStateChange([this, ws](rtc::PeerConnection::State st) {
            auto sp = ws.lock();
            if (!sp) return;
            const char* names[] = {"new", "connecting", "connected", "disconnected", "failed", "closed"};
            logf("webrtc[%s]: state %s", sp->id.substr(0, 6).c_str(), names[std::clamp(int(st), 0, 5)]);
            if (st == rtc::PeerConnection::State::Connected) {
                rtc::Candidate l, rc;
                if (sp->pc->getSelectedCandidatePair(&l, &rc)) {
                    const char* types[] = {"unknown", "host", "srflx", "prflx", "relay"};
                    logf("webrtc[%s]: selected pair local %s %s:%u <-> remote %s %s:%u", sp->id.substr(0, 6).c_str(),
                         types[std::clamp(int(l.type()), 0, 4)], l.address().value_or("?").c_str(), l.port().value_or(0),
                         types[std::clamp(int(rc.type()), 0, 4)], rc.address().value_or("?").c_str(), rc.port().value_or(0));
                }
                sp->pcUp = true;
                if (sp->discUs.exchange(0)) logf("webrtc[%s]: back from disconnected", sp->id.substr(0, 6).c_str());
                markActive(sp, sp->viaTunnel ? "direct WebRTC through the 外網連結" : "WebRTC");
            }
            // 0.8.3: "disconnected" often comes back by itself (a Wi-Fi hiccup): on the LAN
            // the session waits 4 s (housekeeping) before it is closed.
            if (st == rtc::PeerConnection::State::Disconnected && !sp->viaTunnel && !sp->closed) {
                sp->discUs = nowUs();
                logf("webrtc[%s]: disconnected: waiting 4 s before closing", sp->id.substr(0, 6).c_str());
                return;
            }
            if (st == rtc::PeerConnection::State::Failed || st == rtc::PeerConnection::State::Disconnected ||
                st == rtc::PeerConnection::State::Closed) {
                sp->pcUp = false;
                if (sp->viaTunnel && sp->link.load() && !sp->closed) {
                    // The tunnel's WebSocket is still there: the page goes on over it.
                    sp->videoOpen = false, sp->audioOpen = false, sp->dcOpen = false;
                    logf("webrtc[%s]: direct path %s -> the tunnel carries on", sp->id.substr(0, 6).c_str(), names[std::clamp(int(st), 0, 5)]);
                    sendDc(sp, json{{"t", "direct-lost"}});
                    return;
                }
                closeSession(sp, std::string("connection ") + names[std::clamp(int(st), 0, 5)], true);
            }
        });
        s->pc->onGatheringStateChange([ws](rtc::PeerConnection::GatheringState st) {
            auto sp = ws.lock();
            if (!sp || st != rtc::PeerConnection::GatheringState::Complete) return;
            std::lock_guard<std::mutex> lk(sp->gmu);
            sp->gathered = true;
            sp->gcv.notify_all();
        });

        // The stream size this session will get (for the SDP level).
        const Quality q = effective(s->quality);
        const auto mons = enumMonitors();
        int dw = 1920, dh = 1080;
        if (!mons.empty()) {
            const RECT& rc = mons[size_t(s->monitor < int(mons.size()) ? s->monitor : 0)].rect;
            dw = rc.right - rc.left, dh = rc.bottom - rc.top;
        }
        const double sc = std::min({1.0, double(q.maxWidth) / dw, double(q.maxHeight) / dh});
        const int ow = std::max(16, int(dw * sc) & ~1), oh = std::max(16, int(dh * sc) & ~1);

        // Video: H.264 constrained baseline, packetization-mode 1, at the
        // level the stream needs (3.1 for 720p30 ... 5.2 for 4K60).
        const uint32_t ssrcV = randomU32() | 1;
        const int ptV = 96;
        rtc::Description::Video vmedia("video", rtc::Description::Direction::SendOnly);
        const int mbs = ((ow + 15) / 16) * ((oh + 15) / 16);
        struct Lvl {
            int idc, maxFs, maxMbps;
        };
        static const Lvl kLevels[] = {{31, 3600, 108000},  {32, 5120, 216000},  {40, 8192, 245760}, {42, 8704, 522240},
                                      {50, 22080, 589824}, {51, 36864, 983040}, {52, 36864, 2073600}};
        int level = 52;
        for (const Lvl& l : kLevels)
            if (mbs <= l.maxFs && mbs * std::max(1, q.fps) <= l.maxMbps) {
                level = l.idc;
                break;
            }
        char fmtp[96];
        snprintf(fmtp, sizeof(fmtp), "profile-level-id=42e0%02x;packetization-mode=1;level-asymmetry-allowed=1", level);
        vmedia.addH264Codec(ptV, fmtp);
        vmedia.addSSRC(ssrcV, "pm-remote", "pm-stream", "pm-video");
        const int playoutId = 6;
        if (opt.playoutDelayZero)
            vmedia.addExtMap(rtc::Description::Entry::ExtMap(playoutId, "http://www.webrtc.org/experiments/rtp-hdrext/playout-delay"));
        s->video = s->pc->addTrack(vmedia);
        s->rtpV = std::make_shared<rtc::RtpPacketizationConfig>(ssrcV, "pm-remote", uint8_t(ptV), rtc::H264RtpPacketizer::ClockRate);
        if (opt.playoutDelayZero) s->rtpV->playoutDelayId = playoutId, s->rtpV->playoutDelayMin = 0, s->rtpV->playoutDelayMax = 0;
        // RTP payload: 1200 bytes fits a 1500-byte Wi-Fi MTU with IP/UDP/SRTP/
        // extensions; Tailscale's MTU is 1280, so at most 1100 there.
        const int payload = std::clamp(opt.rtpPayload, 500, s->tailscale ? 1100 : 1400);
        auto pack = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, s->rtpV, size_t(payload));
        pack->addToChain(std::make_shared<rtc::RtcpSrReporter>(s->rtpV));
        pack->addToChain(std::make_shared<rtc::RtcpNackResponder>());
        pack->addToChain(std::make_shared<rtc::PliHandler>([this, ws] {
            if (auto sp = ws.lock()) logf("webrtc[%s]: PLI/FIR from the browser -> IDR", sp->id.substr(0, 6).c_str());
            enc.forceKeyFrame();
        }));
        s->video->setMediaHandler(pack);
        s->video->onOpen([this, ws] {
            if (auto sp = ws.lock()) {
                sp->videoOpen = true;
                logf("webrtc[%s]: video track open (%.0f ms after the offer request)", sp->id.substr(0, 6).c_str(),
                     (nowUs() - sp->startUs) / 1000.0);
            }
            enc.forceKeyFrame();
        });
        s->video->onClosed([ws] {
            if (auto sp = ws.lock()) sp->videoOpen = false;
        });

        // Audio: the PC's sound, Opus 48 kHz stereo, same stream id (lip sync).
        if (opt.audio) {
            const uint32_t ssrcA = randomU32() | 1;
            const int ptA = 111;
            rtc::Description::Audio amedia("audio", rtc::Description::Direction::SendOnly);
            amedia.addOpusCodec(ptA, "minptime=10;useinbandfec=1;stereo=1;sprop-stereo=1;maxaveragebitrate=128000");
            amedia.addSSRC(ssrcA, "pm-remote", "pm-stream", "pm-audio");
            s->audio = s->pc->addTrack(amedia);
            s->rtpA = std::make_shared<rtc::RtpPacketizationConfig>(ssrcA, "pm-remote", uint8_t(ptA), rtc::OpusRtpPacketizer::DefaultClockRate);
            auto apack = std::make_shared<rtc::OpusRtpPacketizer>(s->rtpA);
            apack->addToChain(std::make_shared<rtc::RtcpSrReporter>(s->rtpA));
            s->audio->setMediaHandler(apack);
            s->audio->onOpen([ws] {
                if (auto sp = ws.lock()) {
                    sp->audioOpen = true;
                    logf("webrtc[%s]: audio track open", sp->id.substr(0, 6).c_str());
                }
            });
            s->audio->onClosed([ws] {
                if (auto sp = ws.lock()) sp->audioOpen = false;
            });
        }

        // Input: reliable + ordered (a button up must never be lost).
        s->dc = s->pc->createDataChannel("input");
        s->dc->onOpen([this, ws] {
            if (auto sp = ws.lock()) {
                sp->dcOpen = true;
                logf("webrtc[%s]: DataChannel open", sp->id.substr(0, 6).c_str());
                sendDc(sp, json{{"t", "mode"}, {"viewOnly", vo(sp)}});
            }
        });
        s->dc->onClosed([this, ws] {
            if (auto sp = ws.lock()) sp->dcOpen = false;
            if (input) input->releaseAll();
        });
        s->dc->onMessage([this, ws](rtc::message_variant m) {
            auto sp = ws.lock();
            if (!sp) return;
            if (const auto* str = std::get_if<std::string>(&m)) onDcMessage(sp, *str);
        });

        s->pc->setLocalDescription(rtc::Description::Type::Offer);
        {
            std::unique_lock<std::mutex> lk(s->gmu);
            s->gcv.wait_for(lk, std::chrono::seconds(3), [&] { return s->gathered; });
        }
        const auto desc = s->pc->localDescription();
        if (!desc) return false;
        sdp = std::string(*desc);
        int cands = 0;
        for (size_t p = sdp.find("a=candidate:"); p != std::string::npos; p = sdp.find("a=candidate:", p + 1)) ++cands;
        logf("webrtc[%s]: offer for %s (%s) via %s%s ready in %.0f ms (%d host candidates, gathering %s), stream %dx%d@%d %d kbps, "
             "level %d.%d, RTP payload %d%s",
             s->id.substr(0, 6).c_str(), s->peer.c_str(), s->label.c_str(), s->local.c_str(), s->tailscale ? " (Tailscale)" : "",
             (nowUs() - s->startUs) / 1000.0, cands, s->gathered ? "complete" : "timed out", ow, oh, q.fps, q.kbps, level / 10,
             level % 10, payload, opt.audio ? ", + Opus audio track" : "");
        return true;
    } catch (const std::exception& e) {
        logf("webrtc: session setup failed: %s", e.what());
        return false;
    }
}

HttpServer::Response Host::Impl::answer(const HttpServer::Request& r) {
    auto s = current();
    const std::string id = queryParam(r.query, "id");
    if (!s || !constantTimeEq(id, s->id)) return {409, "text/plain", "no such session"};
    if (r.body.empty() || r.body.size() > 64 * 1024) return {400, "text/plain", "bad sdp"};
    try {
        int cands = 0, mdns = 0;
        for (size_t p = r.body.find("a=candidate:"); p != std::string::npos; p = r.body.find("a=candidate:", p + 1)) {
            ++cands;
            const size_t e = r.body.find('\n', p);
            if (r.body.substr(p, e - p).find(".local") != std::string::npos) ++mdns;
        }
        logf("webrtc[%s]: answer from %s (%d candidates, %d mDNS .local)", s->id.substr(0, 6).c_str(), r.peer.c_str(), cands, mdns);
        s->pc->setRemoteDescription(rtc::Description(r.body, rtc::Description::Type::Answer));
    } catch (const std::exception& e) {
        logf("webrtc: bad answer: %s", e.what());
        return {400, "text/plain", "bad answer"};
    }
    return {204, "text/plain", ""};
}

// POST candidate?id=<id>  {"c":[{"candidate":"candidate:...","mid":"0"}, ...]}
// The page sends its answer at once (no wait for its ICE gathering) and its
// candidates as they come. mDNS (.local) ones are skipped: this side cannot
// resolve them, the browser's own checks reach it anyway (peer-reflexive).
HttpServer::Response Host::Impl::candidate(const HttpServer::Request& r) {
    auto s = current();
    const std::string id = queryParam(r.query, "id");
    if (!s || !s->pc || !constantTimeEq(id, s->id)) return {409, "text/plain", "no such session"};
    json j = json::parse(r.body.substr(0, 16384), nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("c") || !j["c"].is_array()) return {400, "text/plain", "bad candidates"};
    int added = 0, mdns = 0, bad = 0;
    std::string kinds;
    for (const auto& c : j["c"]) {
        if (!c.is_object() || added + mdns + bad >= 32) continue;
        auto str = [&c](const char* k) { return c.contains(k) && c[k].is_string() ? c[k].get<std::string>() : std::string(); };
        const std::string cand = str("candidate"), mid = str("mid");
        if (cand.empty() || cand.size() > 512) continue;
        if (cand.find(".local") != std::string::npos) {
            ++mdns;
            continue;
        }
        try {
            s->pc->addRemoteCandidate(rtc::Candidate(cand, mid));
            ++added;
            const size_t t = cand.find(" typ ");
            if (t != std::string::npos) kinds += (kinds.empty() ? "" : ",") + cand.substr(t + 5, cand.find(' ', t + 5) - t - 5);
        } catch (...) {
            ++bad;
        }
    }
    logf("webrtc[%s]: %d candidates from the page%s%s (%d mDNS skipped%s)", s->id.substr(0, 6).c_str(), added, kinds.empty() ? "" : ": ",
         kinds.c_str(), mdns, bad ? (", " + std::to_string(bad) + " bad").c_str() : "");
    return {204, "text/plain", ""};
}

// The first PeerConnection of a process pays for the DTLS certificate and
// the ICE / SCTP set-up (0.8.0: the first offer took 170-240 ms, later ones
// 2-5 ms). Paid here, in the background, before any phone asks.
void Host::Impl::prewarm() {
    const int64_t t0 = nowUs();
    struct W {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
    };
    auto w = std::make_shared<W>();
    try {
        rtc::Configuration cfg;
        cfg.bindAddress = bindIp;
        cfg.disableAutoNegotiation = true;
        auto pc = std::make_shared<rtc::PeerConnection>(cfg);
        pc->onGatheringStateChange([w](rtc::PeerConnection::GatheringState st) {
            if (st != rtc::PeerConnection::GatheringState::Complete) return;
            std::lock_guard<std::mutex> lk(w->m);
            w->done = true;
            w->cv.notify_all();
        });
        auto dc = pc->createDataChannel("warm");
        pc->setLocalDescription(rtc::Description::Type::Offer);
        {
            std::unique_lock<std::mutex> lk(w->m);
            w->cv.wait_for(lk, std::chrono::seconds(3), [&] { return w->done || stop.load(); });
        }
        pc->close();
    } catch (const std::exception& e) {
        logf("host: WebRTC warm-up failed: %s", e.what());
        return;
    }
    logf("host: WebRTC warmed up in %.0f ms (certificate, ICE agent)", (nowUs() - t0) / 1000.0);
}

// ---- 常駐外網 (0.8.2) ----------------------------------------------------------

std::string Host::Impl::makeToken(const std::string& secret) const {
    const long long exp = epochMs() / 1000 + 30LL * 24 * 3600;  // 30 days
    const std::string payload = std::to_string(exp) + "." + randomHex(8);
    return payload + "." + hmacSha256Hex(secret, payload);
}

bool Host::Impl::tokenOk(const std::string& tok, const std::string& secret) {
    if (secret.empty() || tok.size() > 128) return false;
    const size_t a = tok.find('.'), b = tok.rfind('.');
    if (a == std::string::npos || b == a) return false;
    const long long exp = atoll(tok.substr(0, a).c_str());
    if (exp < epochMs() / 1000) return false;
    return constantTimeEq(tok.substr(b + 1), hmacSha256Hex(secret, tok.substr(0, b)));
}

// The fixed link (127.0.0.2): /, /login, /k/<token>/... only.
HttpServer::Response Host::Impl::routeFixed(const HttpServer::Request& r) {
    const int64_t now = nowUs();
    if (!fixedOn) return {404, "text/plain", "not found"};
    std::string secret;
    {
        std::lock_guard<std::mutex> lk(tunMu);
        secret = fixedSecret;
    }
    const std::string& p = r.path;
    if ((p == "/" || p == "/index.html") && (r.method == "GET" || r.method == "HEAD")) return page(r, "/", "fixed:");
    // 加到主畫面: the manifest opens "/" (the saved login goes straight on, else the password).
    if (std::string type, body; r.method == "GET" && p.size() > 1 && webAsset(p.substr(1), type, body)) return {200, type, body};
    if (r.method == "POST" && p == "/login") return login(r, now);
    if (r.method == "GET" && p == "/help") return routeRest(r, "help", "/", "");  // the guide (no secrets in it)
    if (p.rfind("/k/", 0) == 0) {
        const size_t e = p.find('/', 3);
        const std::string tok = e == std::string::npos ? std::string() : p.substr(3, e - 3);
        if (GuestAccount ga; !tok.empty() && tokenOk(tok, secret) && guestToken(tok) && !guestFor(tok, ga)) {
            logf("fixed: %s used the login of a 臨時帳號 that is gone (deleted / expired / used once) -> 401", r.peer.c_str());
            return {401, "application/json", json{{"needLogin", true}, {"guestGone", true}}.dump()};
        }
        if (!tok.empty() && tokenOk(tok, secret) && tokenRevoked(tok)) {
            logf("fixed: %s used a login ended by 中斷 -> the password again", r.peer.c_str());
            return {401, "application/json", json{{"needLogin", true}, {"revoked", true}}.dump()};
        }
        if (!tok.empty() && tokenOk(tok, secret)) {
            const std::string rest = p.substr(e + 1);
            if (rest == "check" && r.method == "POST") {
                wakeCapture(0, 0);  // 0.8.3: a valid login: the phone is on its way (the page asks for 流暢 outside)
                return {204, "text/plain", ""};
            }
            return routeRest(r, rest, "/k/" + tok + "/", "fixed:" + tok);
        }
        if (tok != "-") badTry(r.peer, now, "an expired / wrong login token");  // "-": the page before the password
        return {401, "application/json", json{{"needLogin", true}}.dump()};
    }
    logf("http: %s %s %s -> 404 (fixed link)", r.peer.c_str(), r.method.c_str(), p.substr(0, 24).c_str());
    badTry(r.peer, now, "a wrong fixed-link path");
    return {404, "text/plain", "not found"};
}

// 0.8.3: one password check (POST /login and 解除僅觀看 on the phone). Per address:
// 5 wrong in a row = that address out for 15 minutes, each wrong one answered later
// (0.25, 0.5, 1, 2, 4 s; one try at a time per address). All addresses together:
// more than 20 wrong in 15 minutes = one try every 3 s (429 + retryAfter, no lock;
// an address that logged in before is not paced). 0.8.2 locked EVERY address after
// 5 wrong ones: anybody with the address could keep the owner out.
Host::Impl::PwResult Host::Impl::checkPassword(const std::string& ip, const std::string& pwIn, int64_t now, const char* what,
                                               const std::function<bool(const std::string&)>& verify) {
    PwResult res;
    std::string hash;
    {
        std::lock_guard<std::mutex> lk2(tunMu);
        hash = fixedHash;
    }
    if (hash.empty() && !verify) return res.noPassword = true, res;
    static int64_t lastPacedUs = 0;  // guarded by loginMu
    static std::set<std::string> busy, good;
    {
        std::lock_guard<std::mutex> lk(loginMu);
        if (pwIps.size() > 512) {  // bounded: the oldest address goes
            auto oldest = std::min_element(pwIps.begin(), pwIps.end(), [](const auto& a, const auto& b) { return a.second.lastUs < b.second.lastUs; });
            pwIps.erase(oldest);
        }
        PwIp& st = pwIps[ip];
        st.lastUs = now;
        if (now < st.lockedUntil) {
            res.locked = true, res.retryAfter = int((st.lockedUntil - now) / 1000000) + 1;
            logf("fixed: %s from %s refused: this address is locked for %d s more", what, ip.c_str(), res.retryAfter);
            return res;
        }
        if (busy.count(ip)) return res.retryAfter = 1, res;  // one try at a time per address
        while (!pwWrongAll.empty() && now - pwWrongAll.front() > 900000000LL) pwWrongAll.pop_front();
        if (pwWrongAll.size() > 20 && !good.count(ip)) {
            if (now < lastPacedUs + 3000000) {
                res.retryAfter = int((lastPacedUs + 3000000 - now) / 1000000) + 1;
                logf("fixed: %s from %s paced (%zu wrong passwords from all addresses in 15 minutes): try again in %d s", what, ip.c_str(),
                     pwWrongAll.size(), res.retryAfter);
                return res;
            }
            lastPacedUs = now;
        }
        busy.insert(ip);
    }
    std::string pw = pwIn;
    if (pw.size() > 256) pw.resize(256);
    const int64_t t0 = nowUs();
    const bool ok = !pw.empty() && (verify ? verify(pw) : verifyPassword(pw, hash));
    std::fill(pw.begin(), pw.end(), '\0');
    const double ms = (nowUs() - t0) / 1000.0;
    int fails = 0;
    {
        std::lock_guard<std::mutex> lk(loginMu);
        busy.erase(ip);
        PwIp& st = pwIps[ip];
        if (ok) {
            st.fails = 0;
            good.insert(ip);
            if (good.size() > 64) good.erase(good.begin());
        } else {
            fails = ++st.fails;
            pwWrongAll.push_back(now);
            res.delayMs = std::min(4000, 250 << std::min(fails - 1, 4));
            if (fails >= 5) st.lockedUntil = now + 900000000LL, st.fails = 0, res.locked = true, res.retryAfter = 900;
            res.left = res.locked ? 0 : 5 - fails;
        }
    }
    if (ok) {
        logf("fixed: password right from %s for %s (checked in %.0f ms)", ip.c_str(), what, ms);
        if (opt.onLogin && !verify) opt.onLogin(ip, true, 0, false);
        res.ok = true;
        return res;
    }
    logf("fixed: WRONG password from %s for %s (%d in a row from this address)%s", ip.c_str(), what, fails,
         res.locked ? " -> this address is locked for 15 minutes" : "");
    badTry(ip, now, "a wrong password");
    if (opt.onLogin && !verify) opt.onLogin(ip, false, fails, res.locked);
    Sleep(DWORD(res.delayMs));  // this address waits (others do not)
    return res;
}

bool Host::Impl::tokenRevoked(const std::string& tok) {
    const std::string id = tokenId(tok);
    std::lock_guard<std::mutex> lk(tunMu);
    return !id.empty() && revoked.count(id) > 0;
}

// POST /login {"password": "..."} -> a signed 30-day login (see checkPassword for the limits).
HttpServer::Response Host::Impl::login(const HttpServer::Request& r, int64_t now) {
    std::string secret;
    {
        std::lock_guard<std::mutex> lk2(tunMu);
        secret = fixedSecret;
    }
    if (secret.empty()) return {503, "application/json", json{{"ok", false}, {"noPassword", true}}.dump()};
    json j = json::parse(r.body.substr(0, 1024), nullptr, false);
    const std::string pw = !j.is_discarded() && j.is_object() && j.contains("password") && j["password"].is_string() ? j["password"].get<std::string>() : std::string();
    // 0.8.3.2: an account name = a 臨時帳號 (empty = the owner's password, as before)
    std::string user = !j.is_discarded() && j.is_object() && j.contains("user") && j["user"].is_string() ? j["user"].get<std::string>() : std::string();
    while (!user.empty() && (user.back() == ' ' || user.back() == '\t')) user.pop_back();
    while (!user.empty() && (user.front() == ' ' || user.front() == '\t')) user.erase(0, 1);
    if (!user.empty()) return guestLogin(r, now, user.substr(0, 40), pw, secret);
    const PwResult res = checkPassword(r.peer, pw, now, "the login");
    if (res.noPassword) return {503, "application/json", json{{"ok", false}, {"noPassword", true}}.dump()};
    if (res.ok) {
        logf("fixed: %s -> a 30-day login", r.peer.c_str());
        return {200, "application/json", json{{"ok", true}, {"token", makeToken(secret)}}.dump()};
    }
    if (res.delayMs == 0)  // refused before the check (locked / paced / busy)
        return {429, "application/json", json{{"ok", false}, {"locked", res.locked}, {"retryAfter", res.retryAfter}}.dump()};
    return {403, "application/json",
            json{{"ok", false}, {"left", res.left}, {"locked", res.locked}, {"retryAfter", res.locked ? 900 : 0}, {"delayMs", res.delayMs}}.dump()};
}

// 0.8.3.2 臨時帳號: POST /login {"user": "<name>", "password": "..."} -> a login for this account
// only (at most 12 hours, never past the account's end, gone with the account), the same
// per-address limits as the owner's password. Every failure looks the same (401): a name
// that does not exist, has expired or was spent costs as long as a wrong password.
HttpServer::Response Host::Impl::guestLogin(const HttpServer::Request& r, int64_t now, const std::string& user, const std::string& pw,
                                            const std::string& secret) {
    static const std::string kDummy = hashPassword("not-a-guest-password");
    auto lower = [](std::string s) {
        for (char& c : s) c = (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
        return s;
    };
    GuestAccount found;
    auto verify = [&](const std::string& typed) {
        GuestAccount cand;
        {
            std::lock_guard<std::mutex> lk(tunMu);
            const long long nowS = epochMs() / 1000;
            for (const GuestAccount& g : guests)
                if (lower(g.name) == lower(user) && !spentGuests.count(g.id) && g.expires > nowS) {
                    cand = g;
                    break;
                }
        }
        std::string key = guestPasswordKey(typed);
        const bool ok = verifyPassword(key, cand.id.empty() ? kDummy : cand.pwHash) && !cand.id.empty();
        std::fill(key.begin(), key.end(), '\0');
        if (ok) found = cand;
        return ok;
    };
    const PwResult res = checkPassword(r.peer, pw, now, "a 臨時帳號 login", verify);
    if (opt.onGuestLogin && (res.ok || res.delayMs > 0)) opt.onGuestLogin(r.peer, user, res.ok, res.ok ? 0 : 5 - res.left, res.locked);
    if (res.ok) {
        const long long nowS = epochMs() / 1000;
        const long long exp = std::min(found.expires, nowS + 12LL * 3600);
        const std::string nonce = "g" + randomHex(8);
        const std::string payload = std::to_string(exp) + "." + nonce;
        {
            std::lock_guard<std::mutex> lk(tunMu);
            if (guestTokens.size() > 200) guestTokens.erase(guestTokens.begin());
            guestTokens[nonce] = found.id;
        }
        logf("fixed: %s -> a 臨時帳號 login for %s (%s%s%s, until %lld; no 30-day login, 允許 on the PC needed)", r.peer.c_str(),
             found.name.c_str(), found.once ? "用一次" : "timed", found.viewOnly ? ", 僅觀看" : "", found.files ? ", 傳檔" : "", exp);
        return {200, "application/json", json{{"ok", true}, {"token", payload + "." + hmacSha256Hex(secret, payload)}, {"guest", true}, {"name", found.name}}.dump()};
    }
    if (res.delayMs == 0)
        return {429, "application/json", json{{"ok", false}, {"locked", res.locked}, {"retryAfter", res.retryAfter}, {"guest", true}}.dump()};
    return {401, "application/json",
            json{{"ok", false}, {"guest", true}, {"left", res.left}, {"locked", res.locked}, {"retryAfter", res.locked ? 900 : 0}, {"delayMs", res.delayMs}}.dump()};
}

// The account behind a guest login, while it is valid. One that expires during its own
// session keeps that session (it is judged when the session ends).
bool Host::Impl::guestFor(const std::string& tok, GuestAccount& out) {
    const std::string nonce = tokenId(tok);
    const auto cur = current();
    std::lock_guard<std::mutex> lk(tunMu);
    const auto t = guestTokens.find(nonce);
    if (t == guestTokens.end() || spentGuests.count(t->second)) return false;
    for (const GuestAccount& g : guests)
        if (g.id == t->second) {
            const bool inSession = cur && !cur->closed && cur->guestId == g.id;
            if (g.expires <= epochMs() / 1000 && !inSession) return false;
            out = g;
            return true;
        }
    return false;
}

// A guest's session ended (connected, denied or given up): a 用一次 account is spent now.
void Host::Impl::guestSessionEnded(const std::shared_ptr<Session>& s) {
    bool once = false;
    {
        std::lock_guard<std::mutex> lk(tunMu);
        for (const GuestAccount& g : guests)
            if (g.id == s->guestId) once = g.once;
        if (once) {
            spentGuests.insert(s->guestId);
            for (auto it = guestTokens.begin(); it != guestTokens.end();) it = it->second == s->guestId ? guestTokens.erase(it) : std::next(it);
        }
    }
    logf("host: 臨時帳號 %s: its session ended%s", s->guestName.c_str(), once ? " -> spent (用一次): its login no longer works" : "");
}

// 0.8.3: 解除僅觀看 from the phone: only over the fixed link, only with the password
// (the same limits as the login), only for this session. Answered on the phone with
// {"t":"unlock", ok, left, locked, retryAfter}; the PC is told (onUnlocked).
void Host::Impl::unlockRequest(const std::shared_ptr<Session>& s, const std::string& pw) {
    if (!s->viaFixed || !fixedOn || s->guest()) {
        logf("page %s: 解除僅觀看 refused (only over the fixed link with the owner's password, never for a 臨時帳號)", s->peer.c_str());
        sendDc(s, json{{"t", "unlock"}, {"ok", false}, {"notHere", true}});
        return;
    }
    if (!viewOnly || s->unlocked) {
        sendDc(s, json{{"t", "unlock"}, {"ok", true}});
        return;
    }
    if (unlockBusy.exchange(true)) {
        sendDc(s, json{{"t", "unlock"}, {"ok", false}, {"retryAfter", 1}});
        return;
    }
    if (unlockThread.joinable()) unlockThread.join();
    std::weak_ptr<Session> wk = s;
    unlockThread = std::thread([this, wk, pw = std::string(pw)]() mutable {
        auto sp = wk.lock();
        if (sp && !stop) {
            const PwResult res = checkPassword(sp->peer, pw, nowUs(), "解除僅觀看");
            std::fill(pw.begin(), pw.end(), '\0');
            const bool still = !sp->closed && current() == sp;
            if (res.ok && still) {
                sp->unlocked = true;
                if (input) input->releaseAll();
                logf("host: 僅觀看 lifted for this session by %s (%s) with the password (fixed link)", sp->label.c_str(), sp->peer.c_str());
                sendDc(sp, json{{"t", "mode"}, {"viewOnly", vo(sp)}});
                if (opt.onUnlocked) opt.onUnlocked(sp->toPeer());
            }
            if (still)
                sendDc(sp, json{{"t", "unlock"}, {"ok", res.ok}, {"left", res.left}, {"locked", res.locked}, {"retryAfter", res.retryAfter},
                                {"noPassword", res.noPassword}});
        }
        unlockBusy = false;
    });
}

// ---- 外網連結 (0.8.1) ----------------------------------------------------------

// GET /t/<one-time>  -> 303 /c/<its own path>/ (the one-time link is spent; a new one replaces it)
// /c/<path>/...       the page's routes; until the 通行碼 is right only the page itself, POST pass, diag
HttpServer::Response Host::Impl::routeTunnel(const HttpServer::Request& r) {
    const int64_t now = nowUs();
    bool on;
    {
        std::lock_guard<std::mutex> lk(tunMu);
        on = tunnelOn;
    }
    if (!on) return {404, "text/plain", "not found"};  // the tunnel is off (cloudflared may still be stopping)
    if (r.path == "/favicon.ico" || r.path.rfind("/apple-touch-icon", 0) == 0 || r.path == "/") return {404, "text/plain", "not found"};
    if (r.path.rfind("/t/", 0) == 0 && (r.method == "GET" || r.method == "HEAD")) {
        const std::string t = r.path.substr(3);
        std::string cred, next;
        {
            std::lock_guard<std::mutex> lk(tunMu);
            if (!otk.empty() && constantTimeEq(t, otk) && now < otkUntil) {
                cred = randomString(32);
                creds[cred] = Cred{now, false, 0, r.peer};
                if (creds.size() > 32) creds.erase(std::min_element(creds.begin(), creds.end(), [](const auto& a, const auto& b) {
                                           return a.second.created < b.second.created;
                                       }));
                newOtkLocked(now);
                next = "/t/" + otk;
            }
        }
        if (cred.empty()) {
            logf("tunnel: %s used a spent / expired one-time link", r.peer.c_str());
            badTry(r.peer, now, "a spent one-time link");
            return {410, "text/html; charset=utf-8", linkGonePage()};
        }
        logf("tunnel: one-time link used by %s -> its own path, now the 通行碼 (a new one-time link replaces it)", r.peer.c_str());
        if (opt.onTunnelLink) opt.onTunnelLink(next);
        return {303, "text/plain", "/c/" + cred + "/"};
    }
    if (r.path.rfind("/c/", 0) == 0 && r.path.size() >= 36 && r.path[35] == '/') {
        const std::string c = r.path.substr(3, 32);
        bool known = false, verified = false;
        std::string key;
        {
            std::lock_guard<std::mutex> lk(tunMu);
            for (const auto& [k, v] : creds)
                if (constantTimeEq(k, c)) known = true, verified = v.verified, key = k;
        }
        if (known) {
            const std::string rest = r.path.substr(36);
            if (rest == "pass" && r.method == "POST") return passcodeCheck(r, key, now);
            std::string type, body;
            const bool open = rest.empty() || webAsset(rest, type, body) || rest == "diag" || rest == "help";
            if (!verified && !open) return {401, "application/json", json{{"needPass", true}}.dump()};
            return routeRest(r, rest, "/c/" + key + "/", key);
        }
    }
    logf("http: %s %s %s -> 403 (no / wrong tunnel link)", r.peer.c_str(), r.method.c_str(), r.path.substr(0, 12).c_str());
    badTry(r.peer, now, "a wrong tunnel link");
    return {403, "text/html; charset=utf-8", linkGonePage()};
}

// POST pass {"code":"123456"}: the 通行碼 shown on the PC. 5 wrong: this path is dead.
HttpServer::Response Host::Impl::passcodeCheck(const HttpServer::Request& r, const std::string& cred, int64_t now) {
    json j = json::parse(r.body.substr(0, 256), nullptr, false);
    const std::string code = !j.is_discarded() && j.is_object() && j.contains("code") && j["code"].is_string() ? j["code"].get<std::string>() : std::string();
    bool ok = false, dead = false;
    int left = 0;
    {
        std::lock_guard<std::mutex> lk(tunMu);
        auto it = creds.find(cred);
        if (it == creds.end()) return {403, "application/json", json{{"ok", false}, {"dead", true}}.dump()};
        ok = code.size() == 6 && passcode.size() == 6 && constantTimeEq(code, passcode);
        if (ok) it->second.verified = true;
        else if (++it->second.wrong >= 5) creds.erase(it), dead = true;
        else left = 5 - it->second.wrong;
    }
    if (ok) {
        logf("tunnel: 通行碼 right from %s", r.peer.c_str());
        return {200, "application/json", json{{"ok", true}}.dump()};
    }
    logf("tunnel: wrong 通行碼 from %s%s", r.peer.c_str(), dead ? " (5th: this link is dead now)" : "");
    const bool blocked = badTry(r.peer, now, "a wrong 通行碼");
    return {403, "application/json", json{{"ok", false}, {"left", left}, {"dead", dead}, {"blocked", blocked}}.dump()};
}

// GET ws?id=<session>: the tunnel's WebSocket for the current (allowed) session.
HttpServer::Response Host::Impl::websocket(const HttpServer::Request& r, const std::string& cred) {
    const std::string id = queryParam(r.query, "id");
    auto s = current();
    if (!s || !s->viaTunnel || s->cred != cred || !constantTimeEq(id, s->id)) return {409, "text/plain", "no such session"};
    HttpServer::Response resp{101, "text/plain", ""};
    std::weak_ptr<Session> wk = s;
    resp.websocket = [this, wk](WsConn& conn) {
        auto sp = wk.lock();
        if (!sp || sp->closed) return;
        auto link = std::make_shared<TunnelLink>();
        link->ws = &conn;
        link->cc = opt.tunnelCongestion;
        link->testKbps = opt.tunnelTestKbps;
        auto old = sp->link.exchange(link);
        if (old) old->shutdown(), old->closeWs();  // a reconnect replaces it (0.8.3: its connection ends too)
        link->writer = std::thread([link] { link->runWriter(); });
        const int64_t t0 = nowUs();
        logf("tunnel[%s]: WebSocket open from %s (%.0f ms after the offer request)", sp->id.substr(0, 6).c_str(), conn.peer.c_str(),
             (t0 - sp->startUs) / 1000.0);
        sendDc(sp, json{{"t", "mode"}, {"viewOnly", vo(sp)}});
        conn.run([this, sp](const std::string& m) { onDcMessage(sp, m); });
        {
            std::lock_guard<std::mutex> lk(link->m);
            link->ws = nullptr;  // the connection object goes with this function
        }
        link->shutdown();
        if (link->writer.joinable()) link->writer.join();
        std::shared_ptr<TunnelLink> expect = link;
        const bool last = sp->link.compare_exchange_strong(expect, nullptr);
        logf("tunnel[%s]: WebSocket closed after %.1f s (%lld video frames sent, %lld dropped, %.1f MB)", sp->id.substr(0, 6).c_str(),
             (nowUs() - t0) / 1e6, (long long)link->sent, (long long)link->dropped, link->sentBytes / 1048576.0);
        if (last && !sp->closed && !sp->pcUp) closeSession(sp, "the tunnel connection closed");
    };
    return resp;
}

// A spent / wrong tunnel link: say what to do, in the PC's language.
std::string Host::Impl::linkGonePage() const {
    static const char* const title[4] = {"這個連結已經用過或已失效", "This link was already used or has expired",
                                         "このリンクは使用済みか、期限が切れています", "이 링크는 이미 사용되었거나 만료되었습니다"};
    static const char* const body[4] = {"外網連結的 QR 碼每個只能用一次。請在電腦的「遠端控制」視窗切到「免安裝外網」，再掃一次新的 QR 碼。",
                                        "Each no-app link QR code works once. On the PC, open Remote control, switch to “No-app link” and scan the new QR code.",
                                        "このリンクの QR コードは 1 回だけ使えます。パソコンの「リモート操作」で「アプリ不要」タブを開き、新しい QR コードを読み取ってください。",
                                        "이 링크의 QR 코드는 한 번만 쓸 수 있습니다. PC의 “원격 제어”에서 “앱 없이 외부” 탭을 열고 새 QR 코드를 스캔하세요."};
    const int l = std::clamp(opt.lang, 0, 3);
    return "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<meta name=\"referrer\" content=\"no-referrer\"><title>" + opt.appName + "</title></head>"
           "<body style=\"margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;background:" + hexColor(opt.pageBg) +
           ";color:" + hexColor(opt.pageFg) + ";font:16px/1.5 -apple-system,system-ui,sans-serif;text-align:center\"><div style=\"padding:24px;max-width:460px\">"
           "<h2 style=\"font-size:20px\">" + title[l] + "</h2><p style=\"opacity:.8\">" + body[l] + "</p></div></body></html>";
}

HttpServer::Response Host::Impl::snap(const HttpServer::Request& r) {
    if (opt.snapDir.empty() || r.body.size() < 8 || memcmp(r.body.data(), "\x89PNG", 4) != 0) return {404, "text/plain", "not found"};
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf(name, 64, L"\\remote-snap-%02d%02d%02d.png", st.wHour, st.wMinute, st.wSecond);
    const std::wstring path = opt.snapDir + name;
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return {500, "text/plain", "cannot write"};
    fwrite(r.body.data(), 1, r.body.size(), f);
    fclose(f);
    logf("http: %s snapshot of the decoded video saved: %s (%zu bytes)", r.peer.c_str(), narrow(path).c_str(), r.body.size());
    return {204, "text/plain", ""};
}

// ---- files (0.8.3) -------------------------------------------------------------

// POST up?id=<session>&f=<upload id>&n=<name>&o=<offset>&t=<total>, body = the next piece
// (in order; a piece that does not start where the file is now gets 409 + where it is).
// Only the current, connected session, never in 僅觀看, only into uploadDir.
HttpServer::Response Host::Impl::upload(const HttpServer::Request& r) {
    auto s = current();
    const std::string id = queryParam(r.query, "id"), fid = queryParam(r.query, "f");
    if (!s || s->closed || !constantTimeEq(id, s->id) || !s->activeNotified) return {409, "application/json", json{{"ok", false}, {"noSession", true}}.dump()};
    if (vo(s)) {
        logf("files: upload from %s refused (僅觀看)", s->peer.c_str());
        return {403, "application/json", json{{"ok", false}, {"viewOnly", true}}.dump()};
    }
    if (!s->filesOk()) {
        logf("files: upload from %s refused (臨時帳號 %s: no 傳檔)", s->peer.c_str(), s->guestName.c_str());
        return {403, "application/json", json{{"ok", false}, {"noFiles", true}}.dump()};
    }
    if (opt.uploadDir.empty()) return {404, "application/json", json{{"ok", false}, {"off", true}}.dump()};
    bool fidOk = fid.size() >= 8 && fid.size() <= 32;
    for (char c : fid) fidOk &= isalnum(static_cast<unsigned char>(c)) != 0;
    const uint64_t off = strtoull(queryParam(r.query, "o").c_str(), nullptr, 10), total = strtoull(queryParam(r.query, "t").c_str(), nullptr, 10);
    if (!fidOk) return {400, "application/json", json{{"ok", false}}.dump()};
    if (total > opt.uploadMaxBytes) {
        logf("files: upload from %s refused: %.1f MB is over the %.0f MB limit", s->peer.c_str(), total / 1048576.0, opt.uploadMaxBytes / 1048576.0);
        return {413, "application/json", json{{"ok", false}, {"tooBig", true}, {"max", opt.uploadMaxBytes}}.dump()};
    }
    s->lastActUs = nowUs();  // sending a file is using it (no idle disconnect meanwhile)
    std::lock_guard<std::mutex> lk(fmu);
    auto it = uploads.find(fid);
    if (it == uploads.end()) {
        if (off != 0) return {409, "application/json", json{{"ok", false}, {"got", 0}}.dump()};
        int mine = 0;
        for (const auto& [k, u] : uploads) mine += u.session == s->id;
        if (mine >= 4) return {429, "application/json", json{{"ok", false}, {"busy", true}}.dump()};
        std::error_code ec;
        std::filesystem::create_directories(opt.uploadDir, ec);
        ULARGE_INTEGER freeBytes{};
        if (GetDiskFreeSpaceExW(opt.uploadDir.c_str(), &freeBytes, nullptr, nullptr) && freeBytes.QuadPart < total + 256ull * 1024 * 1024) {
            logf("files: upload refused: not enough disk space (%.0f MB free)", freeBytes.QuadPart / 1048576.0);
            return {507, "application/json", json{{"ok", false}, {"diskFull", true}}.dump()};
        }
        Upload u;
        u.name = cleanFileName(pctDecode(queryParam(r.query, "n")));
        u.part = opt.uploadDir + L"\\" + u.name + L"." + widen(fid) + L".part";
        u.session = s->id, u.total = total;
        HANDLE h = CreateFileW(u.part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {500, "application/json", json{{"ok", false}}.dump()};
        CloseHandle(h);
        logf("files: %s sends \"%s\" (%.2f MB)", s->peer.c_str(), narrow(u.name).c_str(), total / 1048576.0);
        it = uploads.emplace(fid, std::move(u)).first;
    }
    Upload& u = it->second;
    if (u.session != s->id) return {409, "application/json", json{{"ok", false}, {"noSession", true}}.dump()};
    if (off != u.got || u.got + r.body.size() > u.total) return {409, "application/json", json{{"ok", false}, {"got", u.got}}.dump()};
    HANDLE h = CreateFileW(u.part.c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wrote = 0;
    const bool ok = h != INVALID_HANDLE_VALUE && (r.body.empty() || WriteFile(h, r.body.data(), DWORD(r.body.size()), &wrote, nullptr)) && wrote == r.body.size();
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) {
        DeleteFileW(u.part.c_str());
        uploads.erase(it);
        return {500, "application/json", json{{"ok", false}}.dump()};
    }
    u.got += wrote, u.lastUs = nowUs();
    if (u.got < u.total) return {200, "application/json", json{{"ok", true}, {"got", u.got}}.dump()};
    // Done: the final name, never over an existing file ("name (2).ext" ...).
    const size_t dot = u.name.rfind(L'.');
    const std::wstring stem = dot == std::wstring::npos || dot == 0 ? u.name : u.name.substr(0, dot), ext = stem.size() == u.name.size() ? L"" : u.name.substr(dot);
    std::wstring finalName = u.name;
    bool moved = false;
    for (int n = 1; n < 1000 && !moved; ++n) {
        if (n > 1) finalName = stem + L" (" + std::to_wstring(n) + L")" + ext;
        moved = MoveFileExW(u.part.c_str(), (opt.uploadDir + L"\\" + finalName).c_str(), 0) != 0;
        if (!moved && GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS) break;
    }
    const uint64_t savedBytes = u.total;
    const std::string nameU8 = narrow(finalName);
    if (!moved) DeleteFileW(u.part.c_str());
    uploads.erase(it);
    if (!moved) return {500, "application/json", json{{"ok", false}}.dump()};
    logf("files: \"%s\" from %s saved (%.2f MB)", nameU8.c_str(), s->peer.c_str(), savedBytes / 1048576.0);
    if (opt.onUploaded) opt.onUploaded(s->toPeer(), nameU8, savedBytes);
    return {200, "application/json", json{{"ok", true}, {"got", savedBytes}, {"done", true}, {"name", nameU8}}.dump()};
}

void Host::Impl::dropUploads(const std::string& sid, const char* why, int64_t olderThanUs) {
    std::lock_guard<std::mutex> lk(fmu);
    const int64_t now = nowUs();
    for (auto it = uploads.begin(); it != uploads.end();) {
        const bool drop = (sid.empty() || it->second.session == sid) && (olderThanUs == 0 || now - it->second.lastUs > olderThanUs);
        if (!drop) {
            ++it;
            continue;
        }
        logf("files: unfinished \"%s\" removed (%s)", narrow(it->second.name).c_str(), why);
        DeleteFileW(it->second.part.c_str());
        it = uploads.erase(it);
    }
    for (auto it = downloads.begin(); it != downloads.end();) {
        const bool drop = now > it->second.untilUs || (olderThanUs == 0 && (sid.empty() || it->second.session == sid));
        it = drop ? downloads.erase(it) : std::next(it);
    }
}

// The files copied in Explorer -> the phone: names, sizes and a one-time key each (10 minutes).
void Host::Impl::filesList(const std::shared_ptr<Session>& s) {
    if (vo(s) || !s->filesOk()) {
        logf("files: list for %s refused (%s)", s->peer.c_str(), vo(s) ? "僅觀看" : "臨時帳號 without 傳檔");
        sendDc(s, json{{"t", "files-denied"}});
        return;
    }
    const std::vector<std::wstring> paths = opt.testClipboard ? opt.testClipboardFiles : clipboardFiles();
    json list = json::array();
    const int64_t now = nowUs();
    std::lock_guard<std::mutex> lk(fmu);
    for (auto it = downloads.begin(); it != downloads.end();) it = it->second.session == s->id || now > it->second.untilUs ? downloads.erase(it) : std::next(it);
    for (const std::wstring& p : paths) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const uint64_t size = (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        const std::string key = randomString(24);
        const size_t slash = p.find_last_of(L"\\/");
        Download d{p, narrow(slash == std::wstring::npos ? p : p.substr(slash + 1)), s->id, now + 600000000LL};
        list.push_back(json{{"n", d.name}, {"s", size}, {"k", key}});
        downloads.emplace(key, std::move(d));
    }
    logf("files: %zu copied file(s) offered to %s (one-time links, 10 minutes)", list.size(), s->peer.c_str());
    sendDc(s, json{{"t", "files"}, {"list", list}});
}

// GET dl/<key>: once, for the session it was offered to, not in 僅觀看.
HttpServer::Response Host::Impl::download(const HttpServer::Request& r, const std::string& key) {
    auto s = current();
    Download d;
    {
        std::lock_guard<std::mutex> lk(fmu);
        auto it = key.size() == 24 ? downloads.find(key) : downloads.end();
        if (it == downloads.end() || nowUs() > it->second.untilUs) {
            if (it != downloads.end()) downloads.erase(it);
            logf("files: %s used a spent / expired download link", r.peer.c_str());
            return {410, "text/plain; charset=utf-8", "this download link was used or has expired"};
        }
        d = it->second;
        downloads.erase(it);  // one use
    }
    if (!s || s->closed || s->id != d.session) return {410, "text/plain", "session ended"};
    if (vo(s)) return {403, "text/plain", "view only"};
    if (!s->filesOk()) return {403, "text/plain", "no file transfer for this guest account"};
    logf("files: %s downloads \"%s\"", s->peer.c_str(), d.name.c_str());
    HttpServer::Response resp{200, "application/octet-stream", ""};
    resp.file = d.path, resp.fileName = d.name;
    return resp;
}

void Host::Impl::sendDc(const std::shared_ptr<Session>& s, const json& j) {
    if (!s) return;
    if (s->dc && s->dcOpen) {
        try {
            s->dc->send(j.dump());
            return;
        } catch (...) {
        }
    }
    if (auto l = s->link.load()) l->pushText(j.dump());  // 外網連結: the WebSocket
}

void Host::Impl::markActive(const std::shared_ptr<Session>& sp, const char* via) {
    bool notify = false;
    {
        std::lock_guard<std::mutex> lk(smu);
        if (session == sp && !sp->closed && !sp->activeNotified) notify = sp->activeNotified = sp->announced = true;
        if (notify) sp->lastActUs = nowUs();  // the idle clocks start now
    }
    if (notify) {
        logf("host: %s (%s%s) is now %s this PC (%s)", sp->label.c_str(), sp->peer.c_str(),
             sp->viaTunnel ? ", 外網連結" : sp->tailscale ? ", Tailscale" : "", vo(sp) ? "VIEWING" : "CONTROLLING", via);
        if (opt.onActive) opt.onActive(sp->toPeer());
    }
}

void Host::Impl::notifyEnded(const std::shared_ptr<Session>& s, const std::string& why) {
    if (s && s->guest() && !s->guestDone.exchange(true)) guestSessionEnded(s);
    bool notify = false;
    {
        std::lock_guard<std::mutex> lk(smu);
        notify = s->announced;
        s->announced = false;
    }
    if (notify && opt.onEnded) opt.onEnded(s->toPeer(), why);
}

void Host::Impl::closeSession(const std::shared_ptr<Session>& s, const std::string& whyIn, bool fromCallback) {
    if (!s || s->closed.exchange(true)) return;
    std::string why = whyIn;
    bool last = false;
    {
        std::lock_guard<std::mutex> lk(smu);
        if (!s->endWhy.empty()) why = s->endWhy;
        s->state = Session::State::Ended;
        if (session == s) session.reset(), last = true;
    }
    logf("webrtc[%s]: closing (%s)", s->id.substr(0, 6).c_str(), why.c_str());
    dropUploads(s->id, "the session ended", 0);  // 0.8.3: its unfinished uploads and its download links
    if (auto l = s->link.load()) l->closeWs();
    if (input) input->releaseAll();
    if (last) {
        if (s->wantAudio) logf("host: PC sound -> phone off");
        s->wantAudio = false;
        stopAudioNow("the session ended");
    }
    if (fromCallback) {
        // pc->close() inside a libdatachannel callback would wait on itself:
        // housekeeping closes it a moment later.
        std::lock_guard<std::mutex> lk(smu);
        if (s->pc) graveyard.push_back(s->pc);
    } else {
        try {
            if (s->pc) s->pc->close();
        } catch (...) {
        }
    }
    notifyEnded(s, why);
}

void Host::Impl::endPending(const std::string& why) {
    std::shared_ptr<Session> p;
    {
        std::lock_guard<std::mutex> lk(smu);
        p = pending;
        pending.reset();
        if (p) p->state = Session::State::Ended;
    }
    pcv.notify_all();
    if (p) {
        logf("host: pending request of %s (%s) ended: %s", p->label.c_str(), p->peer.c_str(), why.c_str());
        notifyEnded(p, why);
    }
}

void Host::Impl::setAudio(const std::shared_ptr<Session>& s, bool on) {
    std::lock_guard<std::mutex> lk(amu);
    if (s) s->wantAudio = on;
    if (!on) {
        // Sending stops at once (wantAudio); the capture a little later: the
        // phone often turns it straight back on (0.8.0 reopened the loopback
        // 3 times in 7 s, owner's log 20:31:42-49). The session's end stops it now.
        if (audio.running()) {
            if (!audioOffSinceUs) audioOffSinceUs = nowUs();
            audio.setMute(false);  // the PC's speakers come back at once
            logf("host: PC sound -> phone off");
        }
        return;
    }
    audioOffSinceUs = 0;
    if (audio.running()) {
        audio.setMute(opt.mutePc);
        logf("host: PC sound -> phone ON (capture still open, nothing reopened)");
    } else if (opt.audio) {
        audio.setSynthetic(opt.audioSynthetic);
        audio.setLevelOptions(opt.audioAgc, opt.audioVolumeComp);  // 0.8.3
        audio.start(
            [this](std::vector<uint8_t>&& pkt, double, int64_t captureUs) {
                auto cur = current();
                if (!cur || !cur->wantAudio) return;
                const double sec = double(captureUs - cur->startUs) / 1e6;
                if (sec < 0) return;
                bool sent = false;
                if (cur->audio && cur->audioOpen) {
                    try {
                        cur->audio->sendFrame(reinterpret_cast<const rtc::byte*>(pkt.data()), pkt.size(),
                                              rtc::FrameInfo(std::chrono::duration<double>(sec)));
                        sent = true;
                    } catch (const std::exception& e) {
                        logf("webrtc: audio sendFrame failed: %s", e.what());
                    }
                }
                if (auto l = cur->link.load(); l && l->media) {  // 外網連結: Opus over the WebSocket
                    l->pushAudio(pkt.data(), pkt.size(), double(epochMs()) - (nowUs() - captureUs) / 1000.0);
                    sent = true;
                }
                if (sent) {
                    ++audioPackets;
                    audioBytes += int64_t(pkt.size());
                }
            },
            opt.mutePc, audioSrc);
        logf("host: PC sound -> phone ON");
    }
}

void Host::Impl::stopAudioNow(const char* why) {
    std::lock_guard<std::mutex> lk(amu);
    audioOffSinceUs = 0;
    if (!audio.running()) return;
    audio.stop();
    logf("host: PC sound capture stopped (%s)", why);
}

void Host::setTunnel(bool on) {
    Impl& d = *d_;
    {
        std::lock_guard<std::mutex> lk(d.tunMu);
        d.tunnelOn = on;
        d.creds.clear();
        d.passcode.clear(), d.otk.clear();
        if (on) {
            // 6 digits, uniform (rejection sampling on 32-bit randoms).
            uint32_t v = 0;
            do v = randomU32();
            while (v >= 4294000000u);
            char b[8];
            snprintf(b, sizeof(b), "%06u", unsigned(v % 1000000u));
            d.passcode = b;
            d.newOtkLocked(nowUs());
        }
    }
    logf("tunnel: %s", on ? "routes on: a new 通行碼 and one-time link" : "routes off: every tunnel link is invalid now");
    if (!on)
        if (auto s = d.current(); s && s->viaTunnel && !s->viaFixed) {
            {
                std::lock_guard<std::mutex> lk(d.smu);
                s->endWhy = "外網連結 turned off";
            }
            d.sendDc(s, json{{"t", "bye"}, {"why", "stopped"}});
            d.flushBye(s);
            d.closeSession(s, "外網連結 turned off");
        }
}
void Host::setFixed(bool on) {
    Impl& d = *d_;
    d.fixedOn = on;
    logf("fixed: routes %s on 127.0.0.2:%u", on ? "on" : "off", unsigned(d.http.port()));
    if (!on)
        if (auto s = d.current(); s && s->viaFixed) {
            {
                std::lock_guard<std::mutex> lk(d.smu);
                s->endWhy = "常駐外網 turned off";
            }
            d.sendDc(s, json{{"t", "bye"}, {"why", "stopped"}});
            d.flushBye(s);
            d.closeSession(s, "常駐外網 turned off");
        }
}
bool Host::fixedOn() const { return d_->fixedOn; }
void Host::setGuests(std::vector<GuestAccount> list) {
    Impl& d = *d_;
    std::set<std::string> keep;
    for (const GuestAccount& g : list) keep.insert(g.id);
    size_t dropped = 0;
    {
        std::lock_guard<std::mutex> lk(d.tunMu);
        for (auto it = d.guestTokens.begin(); it != d.guestTokens.end();)
            if (keep.count(it->second)) ++it;
            else it = d.guestTokens.erase(it), ++dropped;
        d.guests = std::move(list);
    }
    logf("host: 臨時帳號: %zu account(s)%s", keep.size(), dropped ? (", " + std::to_string(dropped) + " login(s) of removed accounts ended").c_str() : "");
    // An account that is gone: its session ends at once (connected: bye; waiting for 允許: denied).
    if (auto s = d.current(); s && s->guest() && !keep.count(s->guestId)) {
        logf("host: session of %s (%s) ended: its 臨時帳號 %s was deleted on the PC", s->label.c_str(), s->peer.c_str(), s->guestName.c_str());
        {
            std::lock_guard<std::mutex> lk(d.smu);
            s->endWhy = "guest account deleted";
        }
        d.sendDc(s, json{{"t", "bye"}, {"why", "kicked"}});
        d.flushBye(s);
        d.closeSession(s, "guest account deleted");
    }
    std::string pendingId;
    {
        std::lock_guard<std::mutex> lk(d.smu);
        if (d.pending && d.pending->guest() && !keep.count(d.pending->guestId) && d.pending->state == Session::State::Pending) pendingId = d.pending->id;
    }
    if (!pendingId.empty()) approve(pendingId, false);
}

void Host::setFixedOptions(const std::string& passwordHash, const std::string& secret, bool needApproval) {
    std::lock_guard<std::mutex> lk(d_->tunMu);
    d_->fixedHash = passwordHash, d_->fixedSecret = secret, d_->fixedApproval = needApproval;
}
std::string Host::fixedOrigin() const { return "http://127.0.0.2:" + std::to_string(d_->http.port()); }
void Host::setLockAfterSession(bool outside, bool lan) {
    d_->lockOutside = outside, d_->lockLan = lan;
}
void Host::setFixedViaCloudflared(bool on) {
    d_->http.setFixedViaCloudflared(on);
    logf("fixed: the phone's address is taken from %s", on ? "CF-Connecting-IP (cloudflared fallback)" : "the last X-Forwarded-For entry (Tailscale Funnel)");
}

bool Host::tunnelOn() const {
    std::lock_guard<std::mutex> lk(d_->tunMu);
    return d_->tunnelOn;
}
std::string Host::tunnelPasscode() const {
    std::lock_guard<std::mutex> lk(d_->tunMu);
    return d_->passcode;
}
std::string Host::tunnelLinkPath() const {
    std::lock_guard<std::mutex> lk(d_->tunMu);
    return d_->otk.empty() ? std::string() : "/t/" + d_->otk;
}

// 0.8.2: options that take effect at once (0.8.1 restarted the host after
// the session: the port could not be bound again and moved, owner's log 23:11:34).
void Host::setMutePc(bool on) {
    std::lock_guard<std::mutex> lk(d_->amu);
    d_->opt.mutePc = on;
    auto s = d_->current();
    const bool playing = s && s->wantAudio;
    if (d_->audio.running()) d_->audio.setMute(on && playing);
    logf("host: 手機播聲音時電腦靜音 %s%s", on ? "on" : "off", on && !playing ? " (from when the phone turns its sound on)" : "");
}
void Host::setAudioOffered(bool on) {
    d_->opt.audio = on;
    if (!on) {
        if (auto s = d_->current()) {
            s->wantAudio = false;
            d_->sendDc(s, json{{"t", "mode"}, {"viewOnly", d_->vo(s)}, {"audio", false}});
        }
        d_->stopAudioNow("電腦聲音傳到手機 turned off");
    } else if (auto s = d_->current()) {
        d_->sendDc(s, json{{"t", "mode"}, {"viewOnly", d_->vo(s)}, {"audio", true}});
    }
    logf("host: PC sound offered to the phone: %s", on ? "yes" : "no");
}
void Host::setIdleMinutes(double minutes) {
    d_->opt.idleMinutes = minutes;
    if (auto s = d_->current()) s->lastActUs = nowUs();
    logf("host: idle disconnect after %.0f minutes%s", minutes, minutes > 0 ? "" : " (never)");
}

void Host::setAudioSource(const std::string& source) {
    std::lock_guard<std::mutex> lk(d_->amu);
    d_->audioSrc = source;
    d_->opt.audioSource = source;
    if (d_->audio.running()) d_->audio.setSource(source);
    logf("host: sound source set to %s", source.empty() ? "automatic" : source == "default" ? "the Windows default" : source.c_str());
}

// ---- DataChannel ------------------------------------------------------------

// 0.8.3: a message of the wrong shape (e.g. {"t":"ping","id":"x"}: json::value() throws
// on a type mismatch) used to escape the DataChannel / WebSocket callback and end the
// whole program (std::terminate). Every message is handled inside a try now.
void Host::Impl::onDcMessage(const std::shared_ptr<Session>& s, const std::string& m) {
    try {
        onDcMessageImpl(s, m);
    } catch (const std::exception& e) {
        logf("page %s: message ignored (%s): %s", s ? s->peer.c_str() : "?", e.what(), m.substr(0, 80).c_str());
    } catch (...) {
        logf("page %s: message ignored: %s", s ? s->peer.c_str() : "?", m.substr(0, 80).c_str());
    }
}

void Host::Impl::onDcMessageImpl(const std::shared_ptr<Session>& s, const std::string& m) {
    if (m.size() > 8192 || s->closed) return;
    // 0.8.3: only the current session is listened to (a replaced / orphaned one is closed).
    if (current() != s) {
        if (!s->closed) closeSession(s, "not the current session any more", true);
        return;
    }
    json j = json::parse(m, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("t") || !j["t"].is_string()) return;
    const std::string t = j["t"];
    const int64_t now = nowUs();
    s->lastMsgUs = now;
    if (t == "ka") return;  // the tunnel's keep-alive (0.8.3)
    if (t == "leave") {     // 0.8.3: the page is closing / 中斷連線 on the phone: end it now
        logf("page %s: leaving (closed / 中斷連線 on the phone)", s->peer.c_str());
        if (input) input->releaseAll();
        closeSession(s, "the phone left", true);
        return;
    }
    // Anything the person did on the phone (not the page's own pings / reports).
    if (t != "ping" && t != "report" && t != "stamp" && t != "bitrate" && t != "idr" && t != "pause" && t != "release" &&
        t != "clip-stop" && t != "ack" && t != "path" && t != "ws-media" && t != "unlock")
        s->lastActUs = now;
    if (t == "ack") {  // 外網連結: the page got video frame n
        if (auto l = s->link.load()) l->ack(uint32_t(j.value("n", 0)));
        return;
    }
    if (t == "unlock") {  // 0.8.3: 解除僅觀看 with the password (fixed link only)
        unlockRequest(s, j.contains("pw") && j["pw"].is_string() ? j["pw"].get<std::string>() : std::string());
        return;
    }
    if (t == "ws-media") {  // 外網連結: picture + sound over the WebSocket (WebRTC did not connect) or not
        auto l = s->link.load();
        const bool on = j.value("on", false);
        if (l && l->media != on) {
            l->media = on;
            logf("tunnel[%s]: picture and sound %s the tunnel (%s)", s->id.substr(0, 6).c_str(), on ? "now over" : "no longer over",
                 j.value("why", std::string("page")).substr(0, 60).c_str());
            if (on) {
                waitIdr = false;
                enc.forceKeyFrame();
                markActive(s, "WebSocket through the 外網連結 (WebRTC did not connect)");
            }
        }
        return;
    }
    if (t == "path") {  // the page's path and its RTT (shown on the phone)
        logf("page %s: path %s, RTT %.0f ms", s->peer.c_str(), j.value("via", std::string("?")).substr(0, 16).c_str(), j.value("rtt", 0.0));
        return;
    }
    if (t == "ping") {
        sendDc(s, json{{"t", "pong"}, {"id", j.value("id", 0)}, {"c", j.value("c", 0.0)}, {"s", epochMs()}});
        return;
    }
    if (t == "report") {
        logf("page-report %s: %s", s->peer.c_str(), m.c_str());
        return;
    }
    if (t == "bitrate") {
        enc.setBitrate(j.value("kbps", 0));
        return;
    }
    if (t == "idr") {
        // 0.8.3.1: the page asks when it has no picture yet / starts playing after a tap (with a
        // reason, logged); at most one every 300 ms whatever a page sends.
        if (now - s->lastIdrAskUs < 300000) return;
        s->lastIdrAskUs = now;
        if (j.contains("why") && j["why"].is_string())
            logf("page %s: asked for a key frame (%s) -> IDR", s->peer.c_str(), j["why"].get<std::string>().substr(0, 80).c_str());
        enc.forceKeyFrame();
        return;
    }
    if (t == "audio") {
        const bool on = j.value("on", false);
        logf("page %s: sound %s", s->peer.c_str(), on ? "on" : "off");
        setAudio(s, on);
        return;
    }
    if (t == "stamp") {
        stampOn = j.value("on", false);
        return;
    }
    if (t == "release") {
        if (input) input->releaseAll();
        return;
    }
    if (t == "act") return;  // a touch (also in 僅觀看): keeps the session awake, full frame rate
    if (t == "pause") {      // page went to the background / came back (O5)
        const bool on = j.value("on", false);
        if (on == s->paused) return;
        s->paused = on;
        logf("page %s: %s", s->peer.c_str(), on ? "in the background -> picture and sound paused" : "back -> resuming (IDR)");
        if (on) {
            if (input) input->releaseAll();
            s->audioBeforePause = s->wantAudio;
            if (s->wantAudio) setAudio(s, false);
            s->wantAudio = false;
        } else {
            enc.forceKeyFrame();
            if (s->audioBeforePause) setAudio(s, true);
        }
        return;
    }
    if (t == "clip-get") {  // the clipboard panel opened (F7): PC clipboard text to the phone
        // 0.8.3: not in 僅觀看 (0.8.2 sent the PC's clipboard to a view-only phone, and kept
        // pushing every change while its panel was open).
        if (vo(s)) {
            s->clipWatch = false;
            logf("page %s: clipboard refused (僅觀看)", s->peer.c_str());
            sendDc(s, json{{"t", "clip-denied"}});
            return;
        }
        s->clipWatch = j.contains("watch") && j["watch"].is_boolean() ? j["watch"].get<bool>() : true;
        std::string text;
        clipGet(text);
        clipSeq = clipSeqNow();
        sendDc(s, json{{"t", "clip"}, {"s", text}});
        return;
    }
    if (t == "clip-stop") {
        s->clipWatch = false;
        return;
    }
    if (t == "files-get") {  // 0.8.3: the files copied in Explorer (never in 僅觀看)
        filesList(s);
        return;
    }
    if (vo(s)) return;  // 僅觀看: no input at all (unless this session lifted it with the password, 0.8.3)
    if (t == "clip-set") {  // phone text -> PC clipboard (and typed, if asked)
        std::string text = j.value("s", std::string());
        if (text.size() > 64 * 1024) text.resize(64 * 1024);
        const bool ok = clipSet(text);
        clipSeq = clipSeqNow();  // not echoed back to the phone
        logf("page %s: clipboard set on the PC (%zu bytes)%s", s->peer.c_str(), text.size(), ok ? "" : " FAILED");
        if (ok && j.value("type", false) && input) input->handle("text", 0, 0, 0, text);
        sendDc(s, json{{"t", "clip-ok"}, {"ok", ok}});
        return;
    }
    // "abs" carries x / y (0..1 of the streamed monitor), the others dx / dy.
    const char* kx = t == "abs" ? "x" : "dx";
    const char* ky = t == "abs" ? "y" : "dy";
    const double dx = j.contains(kx) && j[kx].is_number() ? j[kx].get<double>() : 0;
    const double dy = j.contains(ky) && j[ky].is_number() ? j[ky].get<double>() : 0;
    const int b = j.contains("b") && j["b"].is_number_integer() ? j["b"].get<int>() : 0;
    std::string text;
    if (j.contains("s") && j["s"].is_string()) text = j["s"];
    if (j.contains("k") && j["k"].is_string()) text = j["k"];
    if (t == "combo") {
        std::vector<std::string> keys;
        if (j.contains("keys") && j["keys"].is_array())
            for (const auto& k : j["keys"])
                if (k.is_string() && keys.size() < 6) keys.push_back(k.get<std::string>());
        if (!input->combo(keys)) logf("input: ignored combo %s", m.substr(0, 120).c_str());
        return;
    }
    if (!input->handle(t, dx, dy, b, text)) logf("input: ignored message %s", m.substr(0, 120).c_str());
    if (t == "move" && j.contains("n") && j["n"].is_number_integer()) s->moveSeq = j["n"].get<int64_t>();
}


// ---- capture / encode / send -------------------------------------------------

// Encoder thread: timing + hand-off to the sender thread (or inline send).
void Host::Impl::onEncoded(std::vector<uint8_t>&& au, int64_t captureUs, bool idr) {
    const int64_t outUs = nowUs();
    int64_t startUs = 0;
    {
        std::lock_guard<std::mutex> lk(tmu);
        while (!encStart.empty() && encStart.front().first < captureUs) encStart.pop_front();
        if (!encStart.empty() && encStart.front().first == captureUs) startUs = encStart.front().second, encStart.pop_front();
    }
    if (startUs) encMs.add((outUs - startUs) / 1000.0);
    c2e.add((outUs - captureUs) / 1000.0);
    int64_t prev = maxAu.load();
    while (int64_t(au.size()) > prev && !maxAu.compare_exchange_weak(prev, int64_t(au.size()))) {
    }
    if (idr) ++idrs;
    if (opt.inlineSend) {
        sendFrame(Encoded{std::move(au), captureUs, idr});
        return;
    }
    {
        std::lock_guard<std::mutex> lk(qmu);
        sendQ.push_back(Encoded{std::move(au), captureUs, idr});
        // Sender behind (network stalled): drop the backlog, resume at an IDR.
        if (sendQ.size() > 4) {
            const size_t n = sendQ.size() - 1;
            sendQ.erase(sendQ.begin(), sendQ.end() - 1);
            if (!sendQ.back().idr) {
                sendQ.clear();
                waitIdr = true;
                enc.forceKeyFrame();
            }
            queueDrops += int64_t(n);
        }
    }
    qcv.notify_one();
}

void Host::Impl::sendFrame(Encoded&& f) {
    auto s = current();
    if (!s) return;
    auto link = s->link.load();
    const bool viaWs = link && link->media, viaRtp = s->videoOpen;
    if (!viaRtp && !viaWs) return;
    if (waitIdr) {
        if (!f.idr) return;
        waitIdr = false;
    }
    try {
        const double sec = double(f.captureUs - s->startUs) / 1e6;
        if (sec < 0) return;  // encoded before this session started
        if (viaRtp)
            s->video->sendFrame(reinterpret_cast<const rtc::byte*>(f.au.data()), f.au.size(),
                                rtc::FrameInfo(std::chrono::duration<double>(sec)));
        if (viaWs) {
            const int64_t now = nowUs();
            bool wantKey = false;
            if (link->pushVideo(f.au, f.idr, double(epochMs()) - (now - f.captureUs) / 1000.0, now, wantKey)) tunnelOk(now);
            else {
                tunnelFull(now, wantKey);
                if (!viaRtp) return;
            }
        }
        bytes += int64_t(f.au.size());
        ++framesSent;
        c2s.add((nowUs() - f.captureUs) / 1000.0);
        if (!s->firstFrameSent.exchange(true))
            logf("webrtc[%s]: first frame sent %.0f ms after the offer request (%s, %.1f KB)", s->id.substr(0, 6).c_str(),
                 (nowUs() - s->startUs) / 1000.0, f.idr ? "IDR" : "P", f.au.size() / 1024.0);
    } catch (const std::exception& e) {
        logf("webrtc: sendFrame failed: %s", e.what());
    }
}

// 外網連結 full: ask for a key frame (at most once a second) and lower the
// bitrate (70 %, at most every 2 s, not below 800 kbps).
void Host::Impl::tunnelFull(int64_t now, bool wantKey) {
    lastDropUs = now;
    if (wantKey && now - lastKeyAskUs > 1000000) lastKeyAskUs = now, enc.forceKeyFrame();
    if (now - lastCutUs < 2000000) return;
    lastCutUs = now;
    const int base = effective(curQuality).kbps;
    const int from = tunnelKbps ? tunnelKbps : base;
    tunnelKbps = std::max(800, from * 7 / 10);
    enc.setBitrate(tunnelKbps);
    logf("tunnel: the tunnel is full (frames not arriving in 0.5 s) -> %d kbps", tunnelKbps);
}
// ... and back up (115 % every 4 s) after 8 s without a dropped frame.
void Host::Impl::tunnelOk(int64_t now) {
    if (!tunnelKbps || now - lastDropUs < 8000000 || now - lastRaiseUs < 4000000) return;
    lastRaiseUs = now;
    const int base = effective(curQuality).kbps;
    tunnelKbps = tunnelKbps * 115 / 100;
    if (tunnelKbps >= base) tunnelKbps = 0;
    enc.setBitrate(tunnelKbps ? tunnelKbps : base);
    logf("tunnel: flowing again -> %d kbps", tunnelKbps ? tunnelKbps : base);
}

void Host::Impl::senderLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    while (true) {
        Encoded f;
        {
            std::unique_lock<std::mutex> lk(qmu);
            qcv.wait(lk, [this] { return stopSender || !sendQ.empty(); });
            if (stopSender && sendQ.empty()) return;
            f = std::move(sendQ.front());
            sendQ.pop_front();
        }
        sendFrame(std::move(f));
    }
}

// Once a second: a pending request whose page went away, an allowed session
// that never connected, a held button with a silent page.
void Host::Impl::housekeeping(int64_t now) {
    std::shared_ptr<Session> stale, dead;
    {
        std::lock_guard<std::mutex> lk(smu);
        // pending (or allowed but never picked up by a poll)
        if (pending && now - pending->lastPollUs > 25000000) stale = pending;
        if (session && !session->activeNotified && now - session->startUs > 20000000) dead = session;
    }
    if (stale) endPending("the phone page went away");
    std::vector<std::shared_ptr<rtc::PeerConnection>> graves;
    {
        std::lock_guard<std::mutex> lk(smu);
        graves.swap(graveyard);
    }
    for (auto& pc : graves) {
        try {
            pc->close();
        } catch (...) {
        }
    }
    if (dead) closeSession(dead, "never connected (20 s)");
    dropUploads("", "no piece for 2 minutes", 120000000LL);  // 0.8.3: stalled uploads, expired download links
    // 0.8.3: a LAN session "disconnected" for 4 s is closed (it did not come back by itself).
    if (auto c = current(); c && !c->closed && c->discUs && now - c->discUs > 4000000) {
        c->pcUp = false;
        closeSession(c, "connection disconnected (4 s)");
    }
    {
        // 外網連結: an unused one-time link expires after 10 minutes; a new one replaces it.
        std::string path;
        {
            std::lock_guard<std::mutex> lk(tunMu);
            if (tunnelOn && now > otkUntil) newOtkLocked(now), path = "/t/" + otk;
        }
        if (!path.empty()) {
            logf("tunnel: the one-time link expired unused -> a new one");
            if (opt.onTunnelLink) opt.onTunnelLink(path);
        }
    }
    {
        bool idleAudio = false;
        {
            std::lock_guard<std::mutex> lk(amu);
            idleAudio = audioOffSinceUs && now - audioOffSinceUs > 15000000 && audio.running();
        }
        if (idleAudio) stopAudioNow("sound off on the phone for 15 s");
    }
    auto s = current();
    if (input) input->flushLog(false);
    // 0.8.3: a page that went away without a word (tab closed, phone off the network): its
    // pings (every 0.5 s) stop. 0.8.2 kept such a session "connected" until ICE gave up or
    // the next phone replaced it (measured: still current 8+ s after the tab was closed),
    // with the 「正在被遠端操控」 bar up. 15 s of silence (10 minutes while the page said it
    // is in the background: iOS stops its timers there) ends it.
    if (s && s->activeNotified && !s->closed && s->lastMsgUs > 0 && now - s->lastMsgUs > (s->paused ? 600000000LL : 15000000LL)) {
        logf("host: no message from the page for %.0f s -> ending the session", (now - s->lastMsgUs) / 1e6);
        closeSession(s, "the phone page went silent");
        return;
    }
    if (input && s && input->anyHeld() && now - s->lastMsgUs > 2000000) {
        logf("input: page silent for %.1f s with a button held -> releasing", (now - s->lastMsgUs) / 1e6);
        input->releaseAll();
    }
    // Idle disconnect (F5): nobody touched the phone for idleMinutes.
    if (s && s->activeNotified && opt.idleMinutes > 0 && s->lastActUs > 0 &&
        now - s->lastActUs > int64_t(opt.idleMinutes * 60e6)) {
        logf("host: no touch on the phone for %.1f minutes -> ending the session", opt.idleMinutes);
        {
            std::lock_guard<std::mutex> lk(smu);
            s->endWhy = "idle for " + std::to_string(int(opt.idleMinutes + 0.5)) + " minutes";
        }
        sendDc(s, json{{"t", "bye"}, {"why", "idle"}, {"minutes", std::max(1, int(opt.idleMinutes + 0.5))}});
        flushBye(s);
        closeSession(s, "idle");
        return;
    }
    // PC clipboard -> phone while its clipboard panel is open (F7).
    if (s && s->clipWatch && (s->dcOpen || s->link.load()) && !vo(s)) {
        const DWORD seq = clipSeqNow();
        if (seq != clipSeq) {
            clipSeq = seq;
            std::string text;
            if (clipGet(text)) sendDc(s, json{{"t", "clip"}, {"s", text}});
        }
    }
}

// 0.8.3: housekeeping on its own thread, once a second (0.8.2 ran it inside the
// capture loop, which therefore never slept even with no phone at all).
void Host::Impl::houseLoop() {
    while (!stop) {
        {
            std::unique_lock<std::mutex> lk(capMu);
            capCv.wait_for(lk, std::chrono::seconds(1), [this] { return stop.load(); });
        }
        if (stop) break;
        try {
            housekeeping(nowUs());
        } catch (const std::exception& e) {
            logf("host: housekeeping failed: %s", e.what());
        }
    }
}

void Host::Impl::captureLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    timeBeginPeriod(1);
    DesktopCapture cap;
    DesktopCapture::Options co;
    co.drawCursor = opt.drawCursor;
    co.scene = opt.scene == "static"   ? DesktopCapture::Scene::Static
               : opt.scene == "motion" ? DesktopCapture::Scene::Motion
                                       : DesktopCapture::Scene::Desktop;
    std::vector<uint8_t> nv12;
    int64_t nextDue = 0, lastEncode = 0, lastStats = nowUs(), lastLog = nowUs();
    bool open = false, encOpen = false;
    int openQuality = -1, openMonitor = -1, encFps = 0, encKbps = 0;
    int64_t poolFull = 0, skippedStatic = 0;
    int tailLeft = 0;  // follow-up frames after a lone change (see below)
    Quality q = effective(curQuality);
    GpuMeter gpu;
    bool awake = false;  // SetThreadExecutionState held (F1)
    int tierFps = 0;     // frame rate in use (O5 / O6)
    POINT lastCur{INT_MIN, INT_MIN};
    int64_t lastCurSeq = -1, lastCurUs = 0;
    bool lastCurVisible = true;

    int64_t lastWantUs = 0, lastChangeUs = 0;  // 0.8.3: lastChangeUs = the picture changed (full rate while it moves)
    while (!stop) {
        auto s = current();
        // 0.8.3: capture + encoder only while a phone is there (or on its way: a session
        // request woke us), released 30 s after the last one; until then this thread sleeps.
        {
            int hq = -1, hm = 0;
            int64_t hus = 0;
            {
                std::lock_guard<std::mutex> lk(capMu);
                hq = hintQuality, hm = hintMonitor, hus = hintUs;
            }
            const int64_t t = nowUs();
            const bool hinted = hq >= 0 && t - hus < 30000000;
            if (s || hinted) lastWantUs = t;
            if (!s && hinted && !open) curQuality = hq, curMonitor = hm;  // open it for the session on its way
            if (!s && !hinted && (open || encOpen) && t - lastWantUs > 30000000) {
                enc.close();  // the encoder first (it holds pool textures of the capture)
                cap.close();
                open = encOpen = false;
                openQuality = openMonitor = -1;
                capOpen = false;
                logf("host: no phone for 30 s -> screen capture and video encoder released");
            }
            if (!s && !hinted && !open) {
                if (awake) awake = false, SetThreadExecutionState(ES_CONTINUOUS);
                std::unique_lock<std::mutex> lk(capMu);
                capCv.wait_for(lk, std::chrono::seconds(60), [&] { return stop.load() || hintUs != hus; });
                continue;
            }
        }
        // The session decides the monitor and the quality; reopen on a change.
        if (s && (s->quality != openQuality || s->monitor != openMonitor)) {
            if (open) logf("host: switching to monitor %d, quality %d", s->monitor, s->quality);
            open = false;  // reopened below (encoder first: it holds pool textures)
            curQuality = s->quality, curMonitor = s->monitor;
        }
        if (!open) {
            q = effective(curQuality);
            co.maxWidth = q.maxWidth, co.maxHeight = q.maxHeight, co.monitor = curMonitor;
            // the encoder holds pool textures of the old capture: close it first
            enc.close();
            encOpen = false;
            const int64_t t0 = nowUs();
            open = cap.open(co);
            if (!open) {
                Sleep(1000);
                continue;
            }
            openQuality = curQuality, openMonitor = curMonitor;
            if (!capOpen.exchange(true)) logf("host: screen capture opened in %.0f ms (a phone is there / on its way)", (nowUs() - t0) / 1000.0);
            deskW = cap.desktopWidth(), deskH = cap.desktopHeight();
            if (input) input->setScreen(cap.desktopRect());
        }
        if (!encOpen || cap.width() != outW || cap.height() != outH || encFps != q.fps || encKbps != q.kbps) {
            enc.close();
            H264Encoder::Options eo;
            eo.width = cap.width(), eo.height = cap.height(), eo.fps = q.fps, eo.kbps = q.kbps;
            eo.idrSeconds = opt.idrSeconds;
            eo.vbvFrames = opt.vbvFrames;
            eo.vbr = opt.vbr;
            eo.kind = opt.encoder == "hw"   ? H264Encoder::Kind::Hardware
                      : opt.encoder == "sw" ? H264Encoder::Kind::Software
                                            : H264Encoder::Kind::Auto;
            if (!opt.cpuPath) eo.device = cap.device(), eo.adapterLuid = cap.adapterLuid();
            encOpen = enc.open(eo, [this](std::vector<uint8_t>&& au, int64_t c, bool idr) { onEncoded(std::move(au), c, idr); });
            outW = cap.width(), outH = cap.height(), encFps = q.fps, encKbps = q.kbps;
            deskW = cap.desktopWidth(), deskH = cap.desktopHeight();
            if (!encOpen) {
                Sleep(1000);
                continue;
            }
            logf("host: video path = %s", enc.gpuInput() ? "GPU zero-copy (DXGI -> VideoProcessor NV12 -> MFT, no CPU readback)"
                                                         : "CPU readback (staging copy -> system-memory NV12 -> MFT)");
        }
        int64_t now = nowUs();
        // Frame rate follows use (O5 / O6): full while the phone is touched
        // (+1.5 s), half otherwise, idleFps after idleAfterSec untouched,
        // nothing while the page is in the background.
        int fps = q.fps;
        // 0.8.3: a picture that moves (a video, an animation, scrolling on the PC) keeps the
        // full rate even without a touch on the phone (0.8.2 halved it after 1.5 s untouched
        // and went to 15 fps after 60 s: a video watched on the phone ran at 30 / 15 fps).
        // A still picture costs nothing at any rate (unchanged frames are not encoded).
        const bool moving = lastChangeUs && now - lastChangeUs < 1000000;
        if (opt.adaptiveFps && s && !moving) {
            const int64_t sinceAct = s->lastActUs > 0 ? now - s->lastActUs : INT64_MAX;
            if (sinceAct < 1500000) fps = q.fps;
            else if (sinceAct > int64_t(opt.idleAfterSec * 1e6)) fps = std::min(q.fps, std::max(1, opt.idleFps));
            else fps = std::max(1, q.fps / 2);
        }
        if (fps != tierFps && s && (s->videoOpen || wsMedia(s))) {
            logf("host: frame rate %d -> %d fps (%s)", tierFps, fps,
                 fps == q.fps ? (moving ? "the picture moves" : "phone in use") : fps == std::max(1, q.fps / 2) ? "phone not touched" : "phone idle");
            tierFps = fps;
        }
        const int64_t period = 1000000 / std::max(1, fps);
        const int64_t refresh = int64_t(std::max(100, opt.staticRefreshMs)) * 1000;
        const bool active = s && (s->videoOpen || wsMedia(s)) && !s->paused;  // WebRTC or the tunnel
        // Keep the PC awake (and its screen on) while a phone is connected (F1).
        const bool wantAwake = opt.keepAwake && s && s->activeNotified && !s->closed;
        if (wantAwake != awake) {
            awake = wantAwake;
            SetThreadExecutionState(awake ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED) : ES_CONTINUOUS);
            logf("host: %s", awake ? "keeping the PC awake (no sleep, screen stays on) while the phone is connected"
                                   : "PC may sleep again");
        }
        // Where the PC pointer is (O7): the page draws its prediction from it.
        if (active && (s->dcOpen || s->link.load()) && now - lastCurUs >= 16000) {
            CURSORINFO ci{sizeof(ci)};
            if (GetCursorInfo(&ci)) {
                const RECT rc = cap.desktopRect();
                const bool vis = (ci.flags & CURSOR_SHOWING) != 0;
                const int64_t seq = s->moveSeq;
                if (ci.ptScreenPos.x != lastCur.x || ci.ptScreenPos.y != lastCur.y || seq != lastCurSeq || vis != lastCurVisible) {
                    lastCur = ci.ptScreenPos, lastCurSeq = seq, lastCurVisible = vis, lastCurUs = now;
                    const double w = std::max(1L, rc.right - rc.left), h = std::max(1L, rc.bottom - rc.top);
                    sendDc(s, json{{"t", "cur"},
                                   {"x", (ci.ptScreenPos.x - rc.left) / w},
                                   {"y", (ci.ptScreenPos.y - rc.top) / h},
                                   {"v", vis},
                                   {"n", seq}});
                }
            }
        }

        if (!active) nextDue = now;
        bool changed = false;
        if (!active) {
            Sleep(50);  // no viewer: no capture at all
        } else if (!cap.poll(int(std::clamp<int64_t>((nextDue - now + 999) / 1000, 0, 100)), changed)) {
            open = false;
            continue;
        }
        now = nowUs();
        if (changed) lastChangeUs = now;
        // Frame slots every `period`: a changed picture is encoded at once if
        // its slot is (nearly) open. Unchanged pictures are not encoded at all
        // (skipStatic) except a refresh every staticRefreshMs; without
        // skipStatic every slot is encoded.
        bool encodeNow = false;
        if (active) {
            if (changed && now >= nextDue - period / 4) encodeNow = true;
            else if (now >= nextDue && (!opt.skipStatic || now - lastEncode >= refresh || tailLeft > 0)) encodeNow = true;
            else if (now >= nextDue && !changed) {
                ++skippedStatic;
                nextDue += period;
            }
        }
        if (encodeNow) {
            // A lone frame after a quiet period is held by the browser until the
            // next frame arrives (measured: up to ~1 s with 1 fps refresh). So
            // a change is followed by tailFrames repeats on the next slots.
            if (changed || now - lastEncode >= refresh) tailLeft = opt.tailFrames;
            else if (tailLeft > 0) --tailLeft;
            nextDue += period;
            if (nextDue < now - period) nextDue = now + period / 2;  // resync after a stall
            lastEncode = now;
            const int64_t captureUs = now;
            const bool stamp = stampOn;
            const uint32_t stampMs = uint32_t(uint64_t(epochMs()) & 0xFFFFFFFFu);
            const int slot = cap.render(stamp, stampMs);
            if (slot < 0) {
                ++poolFull;  // every texture still inside the encoder
            } else {
                const int64_t t1 = nowUs();
                {
                    std::lock_guard<std::mutex> lk(tmu);
                    encStart.emplace_back(captureUs, t1);
                    if (encStart.size() > 64) encStart.pop_front();
                }
                if (enc.gpuInput()) {
                    renderMs.add((t1 - captureUs) / 1000.0);
                    enc.encodeTexture(cap.texture(slot), captureUs, [&cap, slot] { cap.release(slot); });
                } else {
                    const bool ok = cap.readback(slot, nv12);
                    cap.release(slot);
                    if (ok) {
                        if (stamp && !cap.stampOnGpu()) drawStampCpu(nv12, cap.width(), cap.height(), stampMs);
                        renderMs.add((nowUs() - captureUs) / 1000.0);
                        enc.encode(nv12.data(), nv12.size(), captureUs);
                    }
                }
            }
        }
        if (now - lastStats >= 1000000) {
            const double secs = (now - lastStats) / 1e6;
            lastStats = now;
            const auto a = c2s.take(), e = encMs.take(), rd = renderMs.take(), ce = c2e.take();
            double cpuAll = 0, cpu1 = 0;
            cpu.sample(cpuAll, cpu1);
            const int64_t fr = framesSent.exchange(0), by = bytes.exchange(0), mx = maxAu.exchange(0), ir = idrs.exchange(0),
                          qd = queueDrops.exchange(0);
            winFrames += fr, winBytes += by, winMaxAu = std::max(winMaxAu, mx), winIdrs += ir, winQDrops += qd;
            winC2s.push_back(a), winC2e.push_back(ce), winEnc.push_back(e), winRender.push_back(rd);
            if (s && (s->dcOpen || s->link.load())) {
                json j{{"t", "stats"},           {"fps", fr / secs},           {"kbps", by * 8 / 1000.0 / secs},
                       {"c2s_avg", a.avg},       {"c2s_p95", a.p95},           {"c2e_avg", ce.avg},
                       {"c2e_p95", ce.p95},      {"enc_avg", e.avg},           {"enc_p95", e.p95},
                       {"render_avg", rd.avg},   {"cpu", cpuAll},              {"cpu1", cpu1},
                       {"encoder", enc.name()},  {"hw", enc.hardware()},       {"gpuPath", enc.gpuInput()},
                       {"w", int(outW)},         {"h", int(outH)},             {"deskW", int(deskW)},
                       {"deskH", int(deskH)},    {"inject", opt.inject},       {"stamp", bool(stampOn)},
                       {"dropped", enc.dropped()}, {"maxKB", mx / 1024.0},     {"viewOnly", vo(s)},
                       {"audio", audio.running()}, {"monitor", int(curMonitor)}, {"quality", int(curQuality)},
                       {"fpsNow", tierFps},        {"paused", bool(s->paused)}, {"idleMinutes", opt.idleMinutes},
                       {"aOn", bool(s->wantAudio)}, {"aPeak", audio.recentPeak()}, {"aDev", audio.currentDevice()}};

                sendDc(s, j);
            }
            const bool logNow = active ? now - lastLog >= 5000000 : now - lastLog >= 15000000;
            if (logNow) {
                const double ws = (now - lastLog) / 1e6;
                lastLog = now;
                double lAll = 0, l1 = 0;  // CPU over the whole log interval
                cpuLog.sample(lAll, l1);
                const std::string g = gpu.sample();
                auto avg = [](const std::vector<Samples::Summary>& v, bool p95) {
                    double sum = 0;
                    int n = 0;
                    for (const auto& x : v)
                        if (x.n) sum += (p95 ? x.p95 : x.avg) * x.n, n += x.n;
                    return n ? sum / n : 0.0;
                };
                int apk = 0;
                double aLat = 0, aPeak = 0;
                std::string aDev, aOthers;
                audio.takeStats(apk, aLat, aPeak, aDev, aOthers);
                const int64_t aSent = audioPackets.exchange(0), aBytes = audioBytes.exchange(0);
                if (active || winFrames) {
                    const std::string extra =
                        (skippedStatic ? " | static skips " + std::to_string(skippedStatic) : std::string()) +
                        [&] {
                            const int64_t fb = enc.takeFillerBytes();
                            return fb ? " | filler stripped " + std::to_string(fb / 1024) + " KB" : std::string();
                        }() +
                        (poolFull || winQDrops
                             ? " | pool full " + std::to_string(poolFull) + ", queue drops " + std::to_string(winQDrops)
                             : std::string());
                    logf("stats: %dx%d %.1f fps sent, %.0f kbps, max frame %.1f KB, %lld IDR | capture->encoded avg %.1f p95 "
                         "%.1f ms | capture->sent avg %.1f p95 %.1f ms | render %.2f ms | encode %.1f ms | CPU %.2f%% "
                         "of machine (%.1f%% of one core) | GPU %s | %s, %s%s",
                         int(outW), int(outH), winFrames / ws, winBytes * 8 / 1000.0 / ws, winMaxAu / 1024.0, (long long)winIdrs,
                         avg(winC2e, false), avg(winC2e, true), avg(winC2s, false), avg(winC2s, true), avg(winRender, false),
                         avg(winEnc, false), lAll, l1, g.c_str(), enc.name().c_str(), enc.gpuInput() ? "zero-copy" : "readback",
                         extra.c_str());
                    if (audio.running() || aSent)
                        logf("stats audio: %lld Opus packets sent (%.0f kbps), %d encoded, capture->encoded avg %.1f ms, peak %.5f (AGC x%.2f, volume x%.2f), "
                             "RMS %.1f dBFS from %s%s%s",  // 0.8.3.2: the RMS of what was sent
                             (long long)aSent, aBytes * 8 / 1000.0 / ws, apk, aLat, aPeak, audio.agcGain(), audio.compGain(), audio.takeRmsDb(),
                             aDev.empty() ? "?" : aDev.c_str(),
                             aOthers.empty() ? "" : " | other devices: ", aOthers.c_str());
                } else {
                    logf("idle (no viewer): CPU %.2f%% of machine (%.1f%% of one core) | GPU %s", lAll, l1, g.c_str());
                }
                winFrames = winBytes = winMaxAu = winIdrs = winQDrops = 0;
                winC2s.clear(), winC2e.clear(), winEnc.clear(), winRender.clear();
                skippedStatic = poolFull = 0;
            }
        }
    }
    if (awake) SetThreadExecutionState(ES_CONTINUOUS);
    enc.close();  // releases every pool texture (callbacks into cap) before cap goes away
    cap.close();
    timeEndPeriod(1);
    CoUninitialize();
}

}  // namespace pm::remote
