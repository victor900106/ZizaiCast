// 傳到手機 LAN server (see pm/share_server.h, docs/share.md): one listening
// socket on a LAN address, a thread per connection (HTTP/1.1 keep-alive,
// GET / HEAD, single byte ranges), a generated mobile page and the shared
// files. Nothing but /<token>/, /<token>/v/<i>, /<token>/d/<i>, the page's
// script /<token>/app.js, its file list /<token>/list (long-polled by a
// live page) and all files as one ZIP /<token>/zip[?from=F] exists.
#include "pm/share_server.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <list>
#include <mutex>
#include <set>
#include <thread>

#include "page.h"

namespace pm::share {

using Clock = std::chrono::steady_clock;

namespace {

constexpr int kTokenLen = 32;
constexpr int kMaxConnections = 24;
// One LAN host cannot hold every slot: a phone's browser opens up to ~6.
constexpr int kMaxPerPeer = 8;
// A connection must send a complete request with the token this soon after
// accept (else it is closed): idle / slow / token-less sockets free their slot.
constexpr int kFirstRequestMs = 5000;
constexpr int kMaxHeaderBytes = 16 * 1024;
constexpr int kRecvTimeoutMs = 20000;
constexpr int kSendTimeoutMs = 30000;
constexpr int kMaxRequestsPerConn = 200;
constexpr int kLingerAfterExpiryS = 300;  // 410 answers this long after the deadline, then the socket closes
constexpr int kListWaitS = 20;            // /list?n=K holds the request this long for a file K+1 (live page)

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string randomToken() {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    const size_t k = sizeof(kAlphabet) - 1;  // 62
    std::string s;
    while (s.size() < size_t(kTokenLen)) {
        uint8_t b[64];
        BCryptGenRandom(nullptr, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        for (uint8_t x : b) {
            if (x >= 256 - (256 % k)) continue;  // no modulo bias
            s.push_back(kAlphabet[x % k]);
            if (s.size() == size_t(kTokenLen)) break;
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

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

std::wstring fileName(const std::wstring& path) {
    const size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

std::wstring extLower(const std::wstring& path) {
    const std::wstring n = fileName(path);
    const size_t p = n.rfind(L'.');
    std::wstring e = p == std::wstring::npos ? L"" : n.substr(p);
    for (wchar_t& c : e) c = towlower(c);
    return e;
}

// RFC 5987 / 6266: ASCII fallback + UTF-8 filename*.
std::string contentDisposition(bool attachment, const std::wstring& name) {
    const std::string u = narrow(name);
    std::string ascii, enc;
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned char c : u) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                          c == '-' || c == '_';
        if (safe) enc += char(c);
        else enc += '%', enc += hex[c >> 4], enc += hex[c & 15];
        if (c < 0x80 && c >= 0x20 && c != '"' && c != '\\') ascii += char(c);
    }
    // Non-ASCII names (自在投影_…) get a readable ASCII fallback.
    while (!ascii.empty() && (ascii.front() == '_' || ascii.front() == ' ')) ascii.erase(ascii.begin());
    if (ascii.empty() || ascii.front() == '.') ascii = "ZizaiCast" + ascii;
    else if (ascii.size() < u.size()) ascii = "ZizaiCast_" + ascii;
    return std::string(attachment ? "attachment" : "inline") + "; filename=\"" + ascii + "\"; filename*=UTF-8''" + enc;
}

std::string httpDate(std::time_t t) {
    std::tm tm{};
    gmtime_s(&tm, &t);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return buf;
}

// A blocking send() to a phone that stopped reading (socket buffers full)
// sits there until SO_SNDTIMEO (30 s), and Windows does not wake it for
// shutdown() from stop(): stop() / quitting hung for 30 s. So the socket is
// non-blocking while sending here, waiting in 100 ms steps that look at
// `stop`; no progress for kSendTimeoutMs still gives up as before.
bool sendAll(SOCKET s, const char* p, size_t n, const std::atomic<bool>& stop) {
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    auto lastProgress = std::chrono::steady_clock::now();
    bool ok = true;
    while (n > 0) {
        if (stop) {
            ok = false;
            break;
        }
        const int chunk = int(std::min<size_t>(n, 1 << 20));
        const int k = send(s, p, chunk, 0);
        if (k > 0) {
            p += k;
            n -= size_t(k);
            lastProgress = std::chrono::steady_clock::now();
            continue;
        }
        if (k == 0 || WSAGetLastError() != WSAEWOULDBLOCK ||
            std::chrono::steady_clock::now() - lastProgress > std::chrono::milliseconds(kSendTimeoutMs)) {
            ok = false;
            break;
        }
        fd_set wr;
        FD_ZERO(&wr);
        FD_SET(s, &wr);
        timeval tv{0, 100 * 1000};
        select(0, nullptr, &wr, nullptr, &tv);
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);  // recv() in serve() stays blocking (SO_RCVTIMEO)
    return ok;
}

const char* reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 410: return "Gone";
    case 416: return "Range Not Satisfiable";
    case 500: return "Internal Server Error";
    default: return "OK";
    }
}

// ---- ZIP (stored, no compression) for 「全部下載」 ---------------------------
// Classic ZIP (no ZIP64): local header + data per entry, the central
// directory, the end record. Sizes and CRCs are known before the first byte
// is sent (each file's CRC is computed once and cached), so the response
// has a Content-Length and no data descriptors -- every unzipper (iOS Files,
// Android Files, Windows, unzip) reads it. Names are UTF-8 (flag bit 11).
constexpr uint64_t kZipLimit = 0xFFFFFFFFull;

uint32_t crc32Update(uint32_t crc, const unsigned char* p, size_t n) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void put16(std::string& o, uint32_t v) {
    o += char(v & 0xFF);
    o += char((v >> 8) & 0xFF);
}
void put32(std::string& o, uint32_t v) {
    put16(o, v & 0xFFFF);
    put16(o, v >> 16);
}

struct ZipEntry {
    int index = -1;
    std::wstring path;
    std::string name;  // UTF-8, unique within the archive
    uint64_t size = 0;
    uint32_t crc = 0;
    uint16_t dosTime = 0, dosDate = 0x21;  // 1980-01-01
    uint64_t offset = 0;                   // of the local header
};

// The 30-byte local header (or the 46-byte central record) + the name.
std::string zipHeader(const ZipEntry& e, bool central) {
    std::string o;
    put32(o, central ? 0x02014b50u : 0x04034b50u);
    if (central) put16(o, 20);  // made by: MS-DOS, 2.0
    put16(o, 20);               // needed: 2.0
    put16(o, 0x0800);           // UTF-8 names
    put16(o, 0);                // stored
    put16(o, e.dosTime);
    put16(o, e.dosDate);
    put32(o, e.crc);
    put32(o, uint32_t(e.size));
    put32(o, uint32_t(e.size));
    put16(o, uint32_t(e.name.size()));
    put16(o, 0);  // extra
    if (central) {
        put16(o, 0);  // comment
        put16(o, 0);  // disk
        put16(o, 0);  // internal attributes
        put32(o, 0);  // external attributes
        put32(o, uint32_t(e.offset));
    }
    o += e.name;
    return o;
}

// "a.png" twice in one archive → "a (2).png".
std::string uniqueZipName(const std::string& name, std::set<std::string>& used) {
    if (used.insert(lower(name)).second) return name;
    const size_t dot = name.rfind('.');
    const bool ext = dot != std::string::npos && dot > 0;
    const std::string stem = ext ? name.substr(0, dot) : name, tail = ext ? name.substr(dot) : std::string();
    for (int k = 2;; ++k) {
        const std::string n = stem + " (" + std::to_string(k) + ")" + tail;
        if (used.insert(lower(n)).second) return n;
    }
}

}  // namespace

// ------------------------------------------------------------------ helpers --

std::string Server::contentType(const std::wstring& path) {
    const std::wstring e = extLower(path);
    if (e == L".png") return "image/png";
    if (e == L".jpg" || e == L".jpeg") return "image/jpeg";
    if (e == L".gif") return "image/gif";
    if (e == L".webp") return "image/webp";
    if (e == L".heic") return "image/heic";
    if (e == L".bmp") return "image/bmp";
    if (e == L".mp4" || e == L".m4v") return "video/mp4";
    if (e == L".mov") return "video/quicktime";
    if (e == L".webm") return "video/webm";
    return "application/octet-stream";
}

bool Server::isVideo(const std::wstring& path) { return contentType(path).rfind("video/", 0) == 0; }

bool Server::parseRange(const std::string& header, uint64_t size, uint64_t& first, uint64_t& last, bool& whole) {
    whole = true;
    first = 0;
    last = size ? size - 1 : 0;
    std::string h = trim(header);
    if (h.rfind("bytes=", 0) != 0) return true;  // other units: ignore
    h = h.substr(6);
    if (const size_t comma = h.find(','); comma != std::string::npos) h = h.substr(0, comma);  // first range only
    h = trim(h);
    const size_t dash = h.find('-');
    if (dash == std::string::npos) return true;
    const std::string a = trim(h.substr(0, dash)), b = trim(h.substr(dash + 1));
    auto num = [](const std::string& s, uint64_t& v) {
        if (s.empty() || s.size() > 19 || s.find_first_not_of("0123456789") != std::string::npos) return false;
        v = std::stoull(s);
        return true;
    };
    uint64_t x = 0, y = 0;
    if (a.empty()) {  // suffix: last y bytes
        if (!num(b, y)) return true;
        if (y == 0 || size == 0) return false;
        whole = false;
        first = y >= size ? 0 : size - y;
        last = size - 1;
        return true;
    }
    if (!num(a, x)) return true;
    if (!b.empty() && (!num(b, y) || y < x)) return true;  // malformed: ignore
    if (x >= size) return false;
    whole = false;
    first = x;
    last = b.empty() ? size - 1 : std::min(y, size - 1);
    return true;
}

// ------------------------------------------------------------------- Impl --

struct Server::Impl {
    Server* self = nullptr;
    mutable std::mutex mu;
    bool wsa = false;
    SOCKET listener = INVALID_SOCKET;
    std::thread acceptThread;
    std::atomic<bool> stop{false};
    std::atomic<bool> isRunning{false};
    std::atomic<bool> isExpired{false};
    std::string token, bindIp, ifName;
    uint32_t ifAddr = 0, ifMask = 0;  // LAN filter (network order); mask 0 = loopback-only bind
    uint16_t boundPort = 0;
    Clock::time_point deadline;
    std::time_t deadlineWall = 0;
    Options opts;
    struct File {
        std::wstring path, name;
        std::string type;
        bool video = false;
        uint64_t size = 0;
        // The ZIP's CRC-32, valid for this size + last-write time.
        bool crcOk = false;
        uint32_t crc = 0;
        uint64_t crcSize = 0, crcTime = 0;
    };
    // Grows while serving (addFile); indices never change. Guarded by fmu;
    // fcv wakes the long-polled /list requests (new file, expiry, stop).
    mutable std::mutex fmu;
    std::condition_variable fcv;
    std::vector<File> files;
    File fileAt(int i) const {
        std::lock_guard<std::mutex> lk(fmu);
        return files[size_t(i)];
    }
    int fileTotal() const {
        std::lock_guard<std::mutex> lk(fmu);
        return int(files.size());
    }
    void wakeLists() {
        { std::lock_guard<std::mutex> lk(fmu); }
        fcv.notify_all();
    }

    struct Conn {
        std::thread t;
        SOCKET s = INVALID_SOCKET;
        uint32_t peer = 0;  // IPv4, network order
        std::atomic<bool> done{false};
    };
    std::mutex cmu;
    std::list<Conn> conns;

    void logf(const std::string& s) {
        if (self->log) self->log("share: " + s);
    }

    bool peerAllowed(const sockaddr_in& peer) const {
        const uint32_t a = peer.sin_addr.s_addr;
        if ((ntohl(a) >> 24) == 127) return true;  // this PC
        if (ifMask == 0) return false;
        return (a & ifMask) == (ifAddr & ifMask);
    }

    void reapConns(bool all) {
        std::list<Conn> dead;
        {
            std::lock_guard<std::mutex> lk(cmu);
            for (auto it = conns.begin(); it != conns.end();) {
                if (all || it->done) {
                    auto next = std::next(it);
                    dead.splice(dead.end(), conns, it);
                    it = next;
                } else {
                    ++it;
                }
            }
        }
        for (auto& c : dead)
            if (c.t.joinable()) c.t.join();
    }

    void acceptLoop() {
        bool expiredNotified = false;
        while (!stop) {
            if (!expiredNotified && Clock::now() >= deadline) {
                expiredNotified = true;
                isExpired = true;
                wakeLists();
                logf("link expired");
                if (self->onExpired) self->onExpired();
            }
            if (expiredNotified && Clock::now() >= deadline + std::chrono::seconds(kLingerAfterExpiryS)) {
                logf("closing the expired link's socket");
                break;
            }
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(listener, &rd);
            timeval tv{0, 250 * 1000};
            const int r = select(0, &rd, nullptr, nullptr, &tv);
            reapConns(false);
            if (r <= 0) continue;
            sockaddr_in peer{};
            int plen = sizeof(peer);
            SOCKET s = accept(listener, reinterpret_cast<sockaddr*>(&peer), &plen);
            if (s == INVALID_SOCKET) continue;
            char ip[INET_ADDRSTRLEN] = "?";
            inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
            if (!peerAllowed(peer)) {
                logf(std::string(ip) + " refused (not on this LAN)");
                closesocket(s);
                continue;
            }
            std::lock_guard<std::mutex> lk(cmu);
            if (int(conns.size()) >= kMaxConnections) {
                logf(std::string(ip) + " refused (too many connections)");
                closesocket(s);
                continue;
            }
            int fromPeer = 0;
            for (const Conn& c : conns)
                if (!c.done && c.peer == peer.sin_addr.s_addr) ++fromPeer;
            if (fromPeer >= kMaxPerPeer) {
                logf(std::string(ip) + " refused (too many connections from this address)");
                closesocket(s);
                continue;
            }
            DWORD rto = kRecvTimeoutMs, sto = kSendTimeoutMs;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rto), sizeof(rto));
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sto), sizeof(sto));
            conns.emplace_back();
            Conn& c = conns.back();
            c.s = s;
            c.peer = peer.sin_addr.s_addr;
            c.t =std::thread([this, &c, ipStr = std::string(ip)] {
                serve(c.s, ipStr);
                {
                    std::lock_guard<std::mutex> lk2(cmu);
                    closesocket(c.s);
                    c.s = INVALID_SOCKET;
                }
                c.done = true;
            });
        }
        std::lock_guard<std::mutex> lk(mu);
        if (listener != INVALID_SOCKET) {
            closesocket(listener);
            listener = INVALID_SOCKET;
        }
    }

    // One connection: requests until close / error / stop.
    // Until a request carried the token, its whole head must arrive within
    // kFirstRequestMs of accept (not per recv: a trickle does not count).
    void serve(SOCKET s, const std::string& ip) {
        std::string buf;
        bool authed = false;
        const auto firstBy = Clock::now() + std::chrono::milliseconds(kFirstRequestMs);
        auto tooSlow = [&] { logf(ip + " closed (no request within " + std::to_string(kFirstRequestMs / 1000) + " s)"); };
        for (int n = 0; n < kMaxRequestsPerConn && !stop; ++n) {
            size_t end;
            while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
                if (buf.size() > size_t(kMaxHeaderBytes)) return;
                if (!authed) {
                    const auto left =
                        std::chrono::duration_cast<std::chrono::milliseconds>(firstBy - Clock::now()).count();
                    if (left <= 0) return tooSlow();
                    const DWORD to = DWORD(left);
                    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
                }
                char tmp[4096];
                const int k = recv(s, tmp, sizeof(tmp), 0);
                if (k <= 0 || stop) {
                    if (!authed && k < 0 && WSAGetLastError() == WSAETIMEDOUT) tooSlow();
                    return;
                }
                buf.append(tmp, size_t(k));
            }
            const std::string head = buf.substr(0, end);
            buf.erase(0, end + 4);
            const bool was = authed;
            if (!handle(s, ip, head, authed)) return;
            if (authed && !was) {  // a real client: the normal keep-alive timeout from now on
                const DWORD to = kRecvTimeoutMs;
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
            }
        }
    }

    struct Request {
        std::string method, target, version, range, connection, ua;
    };

    static Request parse(const std::string& head) {
        Request r;
        size_t eol = head.find("\r\n");
        const std::string line = head.substr(0, eol);
        const size_t a = line.find(' '), b = line.rfind(' ');
        if (a != std::string::npos && b != std::string::npos && b > a) {
            r.method = line.substr(0, a);
            r.target = line.substr(a + 1, b - a - 1);
            r.version = line.substr(b + 1);
        }
        size_t pos = eol == std::string::npos ? head.size() : eol + 2;
        while (pos < head.size()) {
            size_t e = head.find("\r\n", pos);
            if (e == std::string::npos) e = head.size();
            const std::string h = head.substr(pos, e - pos);
            pos = e + 2;
            const size_t c = h.find(':');
            if (c == std::string::npos) continue;
            const std::string k = lower(trim(h.substr(0, c))), v = trim(h.substr(c + 1));
            if (k == "range") r.range = v;
            else if (k == "connection") r.connection = lower(v);
            else if (k == "user-agent") r.ua = v;
        }
        return r;
    }

    static std::string uaKind(const std::string& ua) {
        if (ua.find("iPhone") != std::string::npos || ua.find("iPad") != std::string::npos ||
            ua.find("iPod") != std::string::npos)
            return "ios";
        if (ua.find("Android") != std::string::npos) return "android";
        return "other";
    }

    // Returns false to close the connection.
    // `authed` is set once a request carries the token.
    bool handle(SOCKET s, const std::string& ip, const std::string& head, bool& authed) {
        const Request rq = parse(head);
        bool keep = rq.version == "HTTP/1.1" ? rq.connection.find("close") == std::string::npos
                                             : rq.connection.find("keep-alive") != std::string::npos;
        const bool isHead = rq.method == "HEAD";
        auto respond = [&](int status, const std::string& type, const std::string& body,
                           const std::string& extra = {}) {
            std::string h = "HTTP/1.1 " + std::to_string(status) + " " + reason(status) + "\r\n";
            h += "Date: " + httpDate(std::time(nullptr)) + "\r\n";
            h += "Server: ZizaiCast\r\n";
            h += "Content-Type: " + type + "\r\n";
            h += "Content-Length: " + std::to_string(body.size()) + "\r\n";
            h += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
                 "X-Robots-Tag: noindex, nofollow\r\nX-Frame-Options: DENY\r\n";
            if (type.rfind("text/html", 0) == 0)
                h += "Content-Security-Policy: default-src 'none'; img-src 'self'; media-src 'self'; "
                     "style-src 'unsafe-inline'; script-src 'self'; connect-src 'self'; base-uri 'none'; "
                     "form-action 'none'\r\n";
            h += extra;
            h += keep ? "Connection: keep-alive\r\nKeep-Alive: timeout=20\r\n" : "Connection: close\r\n";
            h += "\r\n";
            if (!isHead) h += body;
            return sendAll(s, h.data(), h.size(), stop);
        };
        if (rq.method.empty() || rq.target.empty() || rq.target[0] != '/') {
            keep = false;
            logf(ip + " bad request");
            respond(404, "text/plain; charset=utf-8", "404\n");
            return false;
        }
        if (rq.method != "GET" && !isHead) {
            keep = false;
            logf(ip + " " + rq.method.substr(0, 10) + " 405");
            respond(405, "text/plain; charset=utf-8", "405\n", "Allow: GET, HEAD\r\n");
            return false;
        }
        // Path segments (the query apart): /<token>[/<v|d>/<index>[/...]]
        std::string path = rq.target.substr(0, rq.target.find_first_of("?#"));
        const size_t qpos = rq.target.find('?');
        const std::string query = qpos == std::string::npos ? std::string() : rq.target.substr(qpos + 1);
        std::vector<std::string> seg;
        for (size_t p = 1; p <= path.size();) {
            size_t e = path.find('/', p);
            if (e == std::string::npos) e = path.size();
            seg.push_back(path.substr(p, e - p));
            p = e + 1;
        }
        const bool tokenOk = !seg.empty() && constantTimeEq(seg[0], token);
        auto notFound = [&] {
            logf(ip + " " + rq.method + " 404");
            return respond(404, "text/plain; charset=utf-8", "404\n") && keep;
        };
        if (!tokenOk) {  // no keep-alive without the token
            keep = false;
            notFound();
            return false;
        }
        authed = true;
        if (isExpired) {
            logf(ip + " " + rq.method + " 410 (expired)");
            if (self->onAccess) self->onAccess(Access{ip, -1, false, 410});
            return respond(410, "text/html; charset=utf-8", expiredPage(opts.lang >= 0 ? opts.lang : opts.english ? 1 : 0, opts.appName, pagePalette(), opts.live)) && keep;
        }
        if (seg.size() == 1) {  // /<token> → /<token>/ (relative links)
            logf(ip + " " + rq.method + " page 301");
            return respond(301, "text/plain; charset=utf-8", "", "Location: /" + token + "/\r\n") && keep;
        }
        if (seg.size() == 2 && seg[1].empty()) {
            const std::string kind = uaKind(rq.ua);
            logf(ip + " " + rq.method + " page 200 (" + kind + ")");
            if (self->onAccess) self->onAccess(Access{ip, -1, false, 200});
            return respond(200, "text/html; charset=utf-8", buildPage(kind)) && keep;
        }
        if (seg.size() == 2 && seg[1] == "app.js")
            return respond(200, "text/javascript; charset=utf-8", shareScript()) && keep;
        if (seg.size() == 2 && seg[1] == "list") {  // the page's file list; live: ?n=K waits for file K+1
            int n = -1;
            if (query.rfind("n=", 0) == 0 && query.size() > 2 && query.size() <= 5 &&
                query.find_first_not_of("0123456789", 2) == std::string::npos)
                n = std::stoi(query.substr(2));
            if (opts.live && n >= 0) {
                std::unique_lock<std::mutex> lk(fmu);
                fcv.wait_for(lk, std::chrono::seconds(kListWaitS),
                             [&] { return stop.load() || isExpired.load() || int(files.size()) > n; });
            }
            if (stop) return false;
            if (isExpired) {
                logf(ip + " " + rq.method + " list 410 (expired)");
                return respond(410, "application/json; charset=utf-8", "{\"ended\":true}") && keep;
            }
            return respond(200, "application/json; charset=utf-8", listJson(pageFiles(), opts.live)) && keep;
        }
        if (seg.size() == 2 && seg[1] == "zip") {  // 全部下載: files F.. as one stored ZIP
            int from = 0;
            if (!query.empty()) {
                if (query.rfind("from=", 0) != 0 || query.size() <= 5 || query.size() > 8 ||
                    query.find_first_not_of("0123456789", 5) != std::string::npos)
                    return notFound();
                from = std::stoi(query.substr(5));
            }
            if (from < fileTotal()) return sendZip(s, ip, rq, from, keep, isHead);
            return notFound();
        }
        if (seg.size() >= 3 && (seg[1] == "v" || seg[1] == "d") && !seg[2].empty() && seg[2].size() <= 3 &&
            seg[2].find_first_not_of("0123456789") == std::string::npos) {
            const int idx = std::stoi(seg[2]);
            if (idx >= 0 && idx < fileTotal()) return sendFile(s, ip, rq, idx, seg[1] == "d", keep, isHead);
        }
        return notFound();
    }

    PagePalette pagePalette() const {
        const Palette& p = opts.palette;
        return PagePalette{p.background, p.cardTop, p.cardBottom, p.fg, p.dim, p.accent, p.light};
    }

    std::vector<PageFile> pageFiles() const {
        std::lock_guard<std::mutex> lk(fmu);
        std::vector<PageFile> pf;
        for (size_t i = 0; i < files.size(); ++i)
            pf.push_back(PageFile{int(i), narrow(files[i].name), files[i].video, files[i].size, files[i].type});
        return pf;
    }

    std::string buildPage(const std::string& kind) {
        std::tm tm{};
        localtime_s(&tm, &deadlineWall);
        char hhmm[16];
        std::snprintf(hhmm, sizeof(hhmm), "%02d:%02d", tm.tm_hour, tm.tm_min);
        return sharePage(opts.lang >= 0 ? opts.lang : opts.english ? 1 : 0, opts.appName, pagePalette(), pageFiles(),
                         kind, hhmm, opts.ttlSeconds, opts.live);
    }

    bool sendFile(SOCKET s, const std::string& ip, const Request& rq, int idx, bool download, bool keep, bool isHead) {
        const File f = fileAt(idx);
        HANDLE h = CreateFileW(f.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        auto hdr = [&](int status, uint64_t len, const std::string& extra) {
            std::string out = "HTTP/1.1 " + std::to_string(status) + " " + reason(status) + "\r\n";
            out += "Date: " + httpDate(std::time(nullptr)) + "\r\nServer: ZizaiCast\r\n";
            out += "Content-Type: " + f.type + "\r\n";
            out += "Content-Length: " + std::to_string(len) + "\r\n";
            out += "Accept-Ranges: bytes\r\n";
            out += "Content-Disposition: " + contentDisposition(download, f.name) + "\r\n";
            out += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
                   "X-Robots-Tag: noindex, nofollow\r\n";
            out += extra;
            out += keep ? "Connection: keep-alive\r\nKeep-Alive: timeout=20\r\n" : "Connection: close\r\n";
            out += "\r\n";
            return out;
        };
        const std::string what = std::string(download ? "d/" : "v/") + std::to_string(idx) + " \"" + narrow(f.name) + "\"";
        if (h == INVALID_HANDLE_VALUE) {
            logf(ip + " " + rq.method + " " + what + " 404 (file gone)");
            const std::string body = "404\n";
            std::string out = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 4\r\n";
            out += keep ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
            if (!isHead) out += body;
            return sendAll(s, out.data(), out.size(), stop) && keep;
        }
        LARGE_INTEGER li{};
        GetFileSizeEx(h, &li);
        const uint64_t size = uint64_t(li.QuadPart);
        uint64_t first = 0, last = size ? size - 1 : 0;
        bool whole = true;
        if (!rq.range.empty() && !parseRange(rq.range, size, first, last, whole)) {
            CloseHandle(h);
            logf(ip + " " + rq.method + " " + what + " 416 (" + rq.range.substr(0, 40) + ")");
            const std::string out = hdr(416, 0, "Content-Range: bytes */" + std::to_string(size) + "\r\n");
            return sendAll(s, out.data(), out.size(), stop) && keep;
        }
        const uint64_t len = size == 0 ? 0 : last - first + 1;
        const int status = whole ? 200 : 206;
        const std::string extra = whole ? std::string()
                                        : "Content-Range: bytes " + std::to_string(first) + "-" + std::to_string(last) +
                                              "/" + std::to_string(size) + "\r\n";
        logf(ip + " " + rq.method + " " + what + " " + std::to_string(status) +
             (whole ? " (" + std::to_string(size) + " bytes)"
                    : " bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" + std::to_string(size)));
        // One access notification per file view / download, not per range chunk.
        if (self->onAccess && (whole || first == 0)) self->onAccess(Access{ip, idx, download, status});
        const std::string out = hdr(status, len, extra);
        if (!sendAll(s, out.data(), out.size(), stop) || isHead) {
            CloseHandle(h);
            return !isHead ? false : keep;
        }
        LARGE_INTEGER pos{};
        pos.QuadPart = LONGLONG(first);
        SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
        std::vector<char> chunk(256 * 1024);
        uint64_t left = len;
        bool ok = true;
        while (left > 0 && ok && !stop) {
            DWORD got = 0;
            const DWORD want = DWORD(std::min<uint64_t>(left, chunk.size()));
            if (!ReadFile(h, chunk.data(), want, &got, nullptr) || got == 0) {
                ok = false;
                break;
            }
            ok = sendAll(s, chunk.data(), got, stop);
            left -= got;
        }
        CloseHandle(h);
        if (!ok || left > 0) return false;  // short body: the client must not reuse the connection
        return keep;
    }

    // Copies `n` bytes of an open file to `sink` (CRC pass or the socket).
    template <class Sink>
    bool pump(HANDLE h, uint64_t n, std::vector<char>& chunk, Sink&& sink) {
        while (n > 0) {
            if (stop) return false;
            DWORD got = 0;
            const DWORD want = DWORD(std::min<uint64_t>(n, chunk.size()));
            if (!ReadFile(h, chunk.data(), want, &got, nullptr) || got == 0) return false;
            if (!sink(chunk.data(), size_t(got))) return false;
            n -= got;
        }
        return true;
    }

    // GET /<token>/zip[?from=F]: files F.. (in index order) as one stored ZIP
    // for 「全部下載」. Files that are gone are left out, and so are files that
    // would take the archive past 4 GB (classic ZIP; they keep their own 下載
    // button). The panel hears 「手機正在下載：name」 for each file in turn.
    bool sendZip(SOCKET s, const std::string& ip, const Request& rq, int from, bool keep, bool isHead) {
        std::vector<ZipEntry> entries;
        std::set<std::string> used;
        uint64_t offset = 0, central = 0;
        std::vector<char> chunk(256 * 1024);
        const int total = fileTotal();
        for (int i = from; i < total && !stop; ++i) {
            const File f = fileAt(i);
            HANDLE h = CreateFileW(f.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;
            LARGE_INTEGER li{};
            FILETIME wt{}, local{};
            GetFileSizeEx(h, &li);
            GetFileTime(h, nullptr, nullptr, &wt);
            ZipEntry e;
            e.index = i;
            e.path = f.path;
            e.size = uint64_t(li.QuadPart);
            if (FileTimeToLocalFileTime(&wt, &local)) {
                WORD dd = 0, dt = 0;
                if (FileTimeToDosDateTime(&local, &dd, &dt)) e.dosDate = dd, e.dosTime = dt;
            }
            const uint64_t when = (uint64_t(wt.dwHighDateTime) << 32) | wt.dwLowDateTime;
            const std::string name = narrow(f.name);
            const uint64_t nameMax = name.size() + 8;  // room for " (k)"
            if (offset + 30 + nameMax + e.size + central + 46 + nameMax + 22 > kZipLimit) {
                CloseHandle(h);
                logf(ip + " zip: left out file " + std::to_string(i) + " (the archive would pass 4 GB)");
                continue;
            }
            if (!isHead) {
                if (f.crcOk && f.crcSize == e.size && f.crcTime == when) {
                    e.crc = f.crc;
                } else {  // first time (or the file changed): read it once
                    uint32_t crc = 0;
                    const bool ok = pump(h, e.size, chunk, [&](const char* p, size_t n) {
                        crc = crc32Update(crc, reinterpret_cast<const unsigned char*>(p), n);
                        return true;
                    });
                    if (!ok) {
                        CloseHandle(h);
                        continue;
                    }
                    e.crc = crc;
                    std::lock_guard<std::mutex> lk(fmu);
                    File& g = files[size_t(i)];
                    g.crcOk = true;
                    g.crc = crc;
                    g.crcSize = e.size;
                    g.crcTime = when;
                }
            }
            CloseHandle(h);
            e.name = uniqueZipName(name, used);
            e.offset = offset;
            offset += 30 + e.name.size() + e.size;
            central += 46 + e.name.size();
            entries.push_back(std::move(e));
        }
        if (stop) return false;
        if (entries.empty()) {
            logf(ip + " " + rq.method + " zip 404 (no readable file)");
            const std::string out = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 4\r\n"
                                    "Connection: close\r\n\r\n" + std::string(isHead ? "" : "404\n");
            sendAll(s, out.data(), out.size(), stop);
            return false;
        }
        const uint64_t len = offset + central + 22;
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        wchar_t zipName[64];
        std::swprintf(zipName, 64, L"ZizaiCast_%04d%02d%02d_%02d%02d%02d.zip", tm.tm_year + 1900, tm.tm_mon + 1,
                      tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        std::string out = "HTTP/1.1 200 OK\r\nDate: " + httpDate(now) + "\r\nServer: ZizaiCast\r\n";
        out += "Content-Type: application/zip\r\nContent-Length: " + std::to_string(len) + "\r\n";
        out += "Content-Disposition: " + contentDisposition(true, zipName) + "\r\n";
        out += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
               "X-Robots-Tag: noindex, nofollow\r\n";
        out += keep ? "Connection: keep-alive\r\nKeep-Alive: timeout=20\r\n" : "Connection: close\r\n";
        out += "\r\n";
        logf(ip + " " + rq.method + " zip from " + std::to_string(from) + " 200 (" + std::to_string(entries.size()) +
             " file(s), " + std::to_string(len) + " bytes)");
        if (!sendAll(s, out.data(), out.size(), stop)) return false;
        if (isHead) return keep;
        for (const ZipEntry& e : entries) {
            if (self->onAccess) self->onAccess(Access{ip, e.index, true, 200});
            const std::string lh = zipHeader(e, false);
            if (!sendAll(s, lh.data(), lh.size(), stop)) return false;
            HANDLE h = CreateFileW(e.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                logf(ip + " zip: file " + std::to_string(e.index) + " vanished, download cut off");
                return false;  // a short body: the phone sees a failed download, never wrong data
            }
            const bool ok = pump(h, e.size, chunk, [&](const char* p, size_t n) { return sendAll(s, p, n, stop); });
            CloseHandle(h);
            if (!ok) return false;
        }
        std::string tail;
        for (const ZipEntry& e : entries) tail += zipHeader(e, true);
        put32(tail, 0x06054b50u);  // end of central directory
        put16(tail, 0);
        put16(tail, 0);
        put16(tail, uint32_t(entries.size()));
        put16(tail, uint32_t(entries.size()));
        put32(tail, uint32_t(central));
        put32(tail, uint32_t(offset));
        put16(tail, 0);
        if (!sendAll(s, tail.data(), tail.size(), stop)) return false;
        return keep;
    }
};

Server::Server() : d_(std::make_unique<Impl>()) { d_->self = this; }

Server::~Server() { stop(); }

bool Server::start(const std::vector<std::wstring>& paths, const Options& o, std::string* error) {
    stop();
    auto fail = [&](const std::string& e) {
        if (error) *error = e;
        d_->logf("cannot start: " + e);
        stop();
        return false;
    };
    Impl& d = *d_;
    std::unique_lock<std::mutex> flk(d.fmu);  // no thread serves yet; released before the accept thread starts
    d.files.clear();
    for (const std::wstring& p : paths) {
        if (int(d.files.size()) >= kMaxFiles) break;
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        Impl::File f;
        f.path = p;
        f.name = fileName(p);
        f.type = contentType(p);
        f.video = isVideo(p);
        f.size = (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        d.files.push_back(std::move(f));
    }
    const bool none = d.files.empty();
    flk.unlock();
    if (none && !o.live) return fail("no readable file");
    d.opts = o;
    if (d.opts.ttlSeconds <= 0) d.opts.ttlSeconds = 600;

    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return fail("WSAStartup failed");
    d.wsa = true;

    // Which address: forced (tests), else the first preferred one that is a
    // current LAN interface, else the best LAN interface.
    const std::vector<LanInterface> lan = lanInterfaces();
    d.ifName.clear();
    d.ifAddr = d.ifMask = 0;
    if (!o.bindIp.empty()) {
        d.bindIp = o.bindIp;
        for (const auto& l : lan)
            if (l.ip == o.bindIp) d.ifName = l.name, d.ifAddr = l.addr, d.ifMask = l.mask;
    } else {
        const LanInterface* pick = nullptr;
        for (const std::string& pref : o.preferred) {
            const size_t eq = pref.rfind('=');
            const std::string ip = eq == std::string::npos ? pref : pref.substr(eq + 1);
            for (const auto& l : lan)
                if (l.ip == ip) {
                    pick = &l;
                    break;
                }
            if (pick) break;
        }
        if (!pick && !lan.empty()) pick = &lan[0];
        if (!pick) return fail("no LAN interface");
        d.bindIp = pick->ip;
        d.ifName = pick->name;
        d.ifAddr = pick->addr;
        d.ifMask = pick->mask;
    }

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    if (inet_pton(AF_INET, d.bindIp.c_str(), &sa.sin_addr) != 1) return fail("bad bind address " + d.bindIp);
    d.listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (d.listener == INVALID_SOCKET) return fail("socket() failed");
    BOOL excl = TRUE;
    setsockopt(d.listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
    bool bound = false;
    if (o.port) {
        sa.sin_port = htons(o.port);
        bound = bind(d.listener, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0;
    } else {
        for (int i = 0; i < 24 && !bound; ++i) {  // random port (not the OS's sequential ephemeral one)
            sa.sin_port = htons(uint16_t(20000 + randomU32() % 41000));
            bound = bind(d.listener, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0;
        }
        if (!bound) {
            sa.sin_port = 0;
            bound = bind(d.listener, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0;
        }
    }
    if (!bound) return fail("bind " + d.bindIp + " failed (" + std::to_string(WSAGetLastError()) + ")");
    if (listen(d.listener, 16) != 0) return fail("listen failed");
    sockaddr_in got{};
    int glen = sizeof(got);
    getsockname(d.listener, reinterpret_cast<sockaddr*>(&got), &glen);
    d.boundPort = ntohs(got.sin_port);
    d.token = randomToken();
    d.stop = false;
    d.isExpired = false;
    d.deadline = Clock::now() + std::chrono::seconds(d.opts.ttlSeconds);
    d.deadlineWall = std::time(nullptr) + d.opts.ttlSeconds;
    d.isRunning = true;
    std::string names;
    for (const auto& f : d.files) names += (names.empty() ? "" : ", ") + narrow(f.name);
    d.logf(std::string(d.opts.live ? "live, " : "") + "serving " + std::to_string(d.files.size()) + " file(s) on " +
           d.bindIp + ":" + std::to_string(d.boundPort) + (d.ifName.empty() ? "" : " (" + d.ifName + ")") + " for " +
           std::to_string(d.opts.ttlSeconds) + " s: " + names);
    d.acceptThread = std::thread([this] { d_->acceptLoop(); });
    return true;
}

void Server::stop() {
    Impl& d = *d_;
    const bool was = d.isRunning.exchange(false);
    d.stop = true;
    d.wakeLists();
    {
        // Wake the accept loop and every connection at once.
        std::lock_guard<std::mutex> lk(d.cmu);
        for (auto& c : d.conns)
            if (c.s != INVALID_SOCKET) shutdown(c.s, SD_BOTH);
    }
    if (d.acceptThread.joinable()) d.acceptThread.join();
    d.reapConns(true);
    {
        std::lock_guard<std::mutex> lk(d.mu);
        if (d.listener != INVALID_SOCKET) {
            closesocket(d.listener);
            d.listener = INVALID_SOCKET;
        }
    }
    if (d.wsa) {
        WSACleanup();
        d.wsa = false;
    }
    if (was) d.logf("stopped");
    d.token.clear();
}

bool Server::running() const { return d_->isRunning; }
bool Server::expired() const { return d_->isRunning && d_->isExpired; }

std::string Server::url() const {
    if (!d_->isRunning) return {};
    return "http://" + d_->bindIp + ":" + std::to_string(d_->boundPort) + "/" + d_->token + "/";
}
std::string Server::ip() const { return d_->isRunning ? d_->bindIp : std::string(); }
uint16_t Server::port() const { return d_->isRunning ? d_->boundPort : 0; }
std::string Server::interfaceName() const { return d_->ifName; }

int Server::secondsLeft() const {
    if (!d_->isRunning || d_->isExpired) return 0;
    const auto left = std::chrono::duration_cast<std::chrono::seconds>(d_->deadline - Clock::now()).count();
    return left > 0 ? int(left) : 0;
}

std::vector<std::wstring> Server::files() const {
    std::lock_guard<std::mutex> lk(d_->fmu);
    std::vector<std::wstring> v;
    for (const auto& f : d_->files) v.push_back(f.path);
    return v;
}

int Server::addFile(const std::wstring& p) {
    Impl& d = *d_;
    if (!d.isRunning || d.isExpired) return -1;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return -1;
    int idx = -1;
    {
        std::lock_guard<std::mutex> lk(d.fmu);
        for (size_t i = 0; i < d.files.size(); ++i)
            if (_wcsicmp(d.files[i].path.c_str(), p.c_str()) == 0) return int(i);
        if (int(d.files.size()) >= kMaxLiveFiles) return -1;
        Impl::File f;
        f.path = p;
        f.name = fileName(p);
        f.type = contentType(p);
        f.video = isVideo(p);
        f.size = (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        d.files.push_back(std::move(f));
        idx = int(d.files.size()) - 1;
    }
    d.fcv.notify_all();
    d.logf("added file " + std::to_string(idx) + ": " + narrow(fileName(p)));
    return idx;
}

bool Server::live() const { return d_->isRunning && d_->opts.live; }

int Server::fileCount() const { return d_->fileTotal(); }

}  // namespace pm::share
