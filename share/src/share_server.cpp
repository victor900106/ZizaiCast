// 傳到手機 LAN server (see pm/share_server.h, docs/share.md): one listening
// socket on a LAN address, a thread per connection (HTTP/1.1 keep-alive,
// GET / HEAD, single byte ranges), a generated mobile page and the shared
// files. Nothing but /<token>/, /<token>/v/<i>, /<token>/d/<i>, the page's
// script /<token>/app.js and its file list /<token>/list (long-polled by a
// live page) exists.
#include "pm/share_server.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
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

bool sendAll(SOCKET s, const char* p, size_t n, const std::atomic<bool>& stop) {
    while (n > 0) {
        if (stop) return false;
        const int chunk = int(std::min<size_t>(n, 1 << 20));
        const int k = send(s, p, chunk, 0);
        if (k <= 0) return false;
        p += k;
        n -= size_t(k);
    }
    return true;
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
            DWORD rto = kRecvTimeoutMs, sto = kSendTimeoutMs;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rto), sizeof(rto));
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sto), sizeof(sto));
            conns.emplace_back();
            Conn& c = conns.back();
            c.s = s;
            c.t = std::thread([this, &c, ipStr = std::string(ip)] {
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
    void serve(SOCKET s, const std::string& ip) {
        std::string buf;
        for (int n = 0; n < kMaxRequestsPerConn && !stop; ++n) {
            size_t end;
            while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
                if (buf.size() > size_t(kMaxHeaderBytes)) return;
                char tmp[4096];
                const int k = recv(s, tmp, sizeof(tmp), 0);
                if (k <= 0 || stop) return;
                buf.append(tmp, size_t(k));
            }
            const std::string head = buf.substr(0, end);
            buf.erase(0, end + 4);
            if (!handle(s, ip, head)) return;
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
    bool handle(SOCKET s, const std::string& ip, const std::string& head) {
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
        if (!tokenOk) return notFound();
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
