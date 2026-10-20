#include "http_server.h"

#include <ws2tcpip.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "util.h"

namespace pm::remote {

namespace {

constexpr int kMaxConnections = 16;
constexpr int kMaxPerPeer = 8;
constexpr int kMaxLoopback = 24;  // the tunnel: every phone comes in as 127.0.0.1 (cloudflared)
constexpr size_t kMaxHeaderBytes = 16 * 1024;
constexpr size_t kMaxBodyBytes = 8 * 1024 * 1024;  // snapshot PNG upload (tests); SDP is a few KB
constexpr size_t kSmallBodyBytes = 64 * 1024;      // 0.8.3: every other request (before it is routed / checked)
constexpr DWORD kTimeoutMs = 10000;
constexpr ULONGLONG kRequestDeadlineMs = 10000;    // 0.8.3: the whole request (headers + body), not per recv()
constexpr DWORD kWsReadTimeoutMs = 65000;          // 0.8.3: the page pings every 0.5 s (a hidden tab: throttled, still < 1 min)

const char* reason(int s) {
    switch (s) {
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 303: return "See Other";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "OK";
    }
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

bool sendAll(SOCKET s, const char* p, size_t n) {
    while (n > 0) {
        const int k = send(s, p, int(std::min<size_t>(n, 1 << 20)), 0);
        if (k <= 0) return false;
        p += k, n -= size_t(k);
    }
    return true;
}

// Sec-WebSocket-Accept: base64(SHA-1(key + GUID)) (BCrypt, no extra library).
std::string wsAccept(const std::string& key) {
    const std::string in = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[20] = {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return {};
    BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(in.data())), ULONG(in.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(alg, 0);
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (int i = 0; i < 20; i += 3) {
        const unsigned v = (unsigned(digest[i]) << 16) | (i + 1 < 20 ? unsigned(digest[i + 1]) << 8 : 0u) | (i + 2 < 20 ? unsigned(digest[i + 2]) : 0u);
        out += b64[(v >> 18) & 63];
        out += b64[(v >> 12) & 63];
        out += i + 1 < 20 ? b64[(v >> 6) & 63] : '=';
        out += i + 2 < 20 ? b64[v & 63] : '=';
    }
    return out;
}

bool recvAll(SOCKET s, void* p, size_t n) {
    char* c = static_cast<char*>(p);
    while (n > 0) {
        const int k = recv(s, c, int(std::min<size_t>(n, 1 << 20)), 0);
        if (k <= 0) return false;
        c += k, n -= size_t(k);
    }
    return true;
}

// A dotted IPv4 / IPv6 text from a header (CF-Connecting-IP), else "".
std::string ipText(const std::string& v) {
    if (v.empty() || v.size() > 45) return {};
    for (char c : v)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '.' || c == ':')) return {};
    return v;
}

}  // namespace

// ---- WsConn ---------------------------------------------------------------

bool WsConn::sendFrame(int opcode, const void* p, size_t n) {
    if (closed_) return false;
    unsigned char h[10];
    size_t hl = 2;
    h[0] = static_cast<unsigned char>(0x80 | opcode);  // FIN, never fragmented; server frames are not masked
    if (n < 126) {
        h[1] = static_cast<unsigned char>(n);
    } else if (n < 65536) {
        h[1] = 126, h[2] = static_cast<unsigned char>(n >> 8), h[3] = static_cast<unsigned char>(n & 0xFF), hl = 4;
    } else {
        h[1] = 127, hl = 10;
        for (int i = 0; i < 8; ++i) h[2 + i] = static_cast<unsigned char>((uint64_t(n) >> (56 - 8 * i)) & 0xFF);
    }
    std::lock_guard<std::mutex> lk(wmu_);
    if (!sendAll(s_, reinterpret_cast<const char*>(h), hl) || (n && !sendAll(s_, static_cast<const char*>(p), n))) {
        closed_ = true;
        return false;
    }
    return true;
}

void WsConn::close() {
    if (closed_.exchange(true)) return;
    const unsigned char bye[4] = {0x88, 0x02, 0x03, 0xE8};  // close, 1000
    {
        std::lock_guard<std::mutex> lk(wmu_);
        sendAll(s_, reinterpret_cast<const char*>(bye), sizeof(bye));
    }
    shutdown(s_, SD_BOTH);
}

void WsConn::run(const std::function<void(const std::string&)>& onText) {
    std::string msg;
    int msgOp = 0;
    while (!closed_) {
        unsigned char h[2];
        if (!recvAll(s_, h, 2)) break;
        const bool fin = (h[0] & 0x80) != 0;
        const int op = h[0] & 0x0F;
        uint64_t n = h[1] & 0x7F;
        if (n == 126) {
            unsigned char e[2];
            if (!recvAll(s_, e, 2)) break;
            n = (uint64_t(e[0]) << 8) | e[1];
        } else if (n == 127) {
            unsigned char e[8];
            if (!recvAll(s_, e, 8)) break;
            n = 0;
            for (int i = 0; i < 8; ++i) n = (n << 8) | e[i];
        }
        if (n > (1u << 20)) break;  // the page only sends small control messages
        unsigned char mask[4] = {0, 0, 0, 0};
        if ((h[1] & 0x80) && !recvAll(s_, mask, 4)) break;
        std::string data(size_t(n), '\0');
        if (n && !recvAll(s_, data.data(), size_t(n))) break;
        if (h[1] & 0x80)
            for (size_t i = 0; i < data.size(); ++i) data[i] = char(data[i] ^ mask[i & 3]);
        if (op == 0x8) {  // close: answer and go
            close();
            break;
        }
        if (op == 0x9) {  // ping -> pong
            sendFrame(0xA, data.data(), data.size());
            continue;
        }
        if (op == 0xA) continue;
        if (op != 0x0) msg.clear(), msgOp = op;
        msg += data;
        if (msg.size() > (1u << 20)) break;
        if (fin) {
            if (msgOp == 0x1 && onText) onText(msg);
            msg.clear();
        }
    }
    closed_ = true;
}

bool HttpServer::start(const std::vector<Bind>& binds, uint16_t port, Handler h) {
    stop();
    if (binds.empty()) return false;
    WSADATA wd;
    wsa_ = WSAStartup(MAKEWORD(2, 2), &wd) == 0;
    handler_ = std::move(h);
    for (size_t i = 0; i < binds.size(); ++i) {
        const Bind& b = binds[i];
        Listener l;
        l.bind = b;
        l.s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (l.s == INVALID_SOCKET) return logf("http: socket failed"), stop(), false;
        BOOL excl = TRUE;  // nobody else may bind the same port on another address
        setsockopt(l.s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(i == 0 ? port : port_);
        bool bound = inet_pton(AF_INET, b.ip.c_str(), &sa.sin_addr) == 1;
        // A port just closed by this process can be busy for a moment (connections
        // in TIME_WAIT with SO_EXCLUSIVEADDRUSE): retry the same one for up to 3 s
        // instead of moving to the next (the phone's saved link has this port).
        for (int t = 0; bound && t < 30; ++t) {
            if (bind(l.s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0) break;
            if (WSAGetLastError() != WSAEADDRINUSE || t == 29) {
                bound = false;
                break;
            }
            Sleep(100);
        }
        if (!bound || listen(l.s, 16) != 0) {
            logf("http: bind %s:%u failed (%d)", b.ip.c_str(), i == 0 ? port : port_, WSAGetLastError());
            closesocket(l.s);
            if (i == 0) return stop(), false;
            continue;
        }
        if (i == 0) {
            int len = sizeof(sa);
            getsockname(l.s, reinterpret_cast<sockaddr*>(&sa), &len);
            port_ = ntohs(sa.sin_port);
        }
        logf("http: listening on %s:%u (%s)", b.ip.c_str(), port_,
             b.fixed ? "fixed-link origin: this PC only" : b.loopback ? "tunnel origin: this PC only" : b.tailscale ? "Tailscale peers only" : "LAN only");
        listeners_.push_back(l);
    }
    stop_ = false;
    // 0.8.3: the accept thread sleeps until a connection (or stop()) instead of waking every 200 ms.
    wake_ = WSACreateEvent();
    for (Listener& l : listeners_) {
        l.ev = WSACreateEvent();
        if (l.ev) WSAEventSelect(l.s, l.ev, FD_ACCEPT);
    }
    acceptThread_ = std::thread([this] { acceptLoop(); });
    return true;
}

std::vector<std::string> HttpServer::boundIps() const {
    std::vector<std::string> v;
    for (const Listener& l : listeners_) v.push_back(l.bind.ip);
    return v;
}

void HttpServer::stop() {
    // 0.8.3: the accept thread first, then every connection (their threads use the
    // listeners' Bind), and only then the listeners themselves.
    stop_ = true;
    if (wake_) WSASetEvent(wake_);
    if (acceptThread_.joinable()) acceptThread_.join();
    {
        std::lock_guard<std::mutex> lk(cmu_);
        for (Conn& c : conns_)
            if (c.s != INVALID_SOCKET) shutdown(c.s, SD_BOTH);
    }
    for (Conn& c : conns_)
        if (c.t.joinable()) c.t.join();
    conns_.clear();
    for (Listener& l : listeners_) {
        if (l.ev) WSACloseEvent(l.ev), l.ev = nullptr;
        if (l.s != INVALID_SOCKET) closesocket(l.s);
    }
    listeners_.clear();
    if (wake_) WSACloseEvent(wake_), wake_ = nullptr;
    if (wsa_) WSACleanup(), wsa_ = false;
}

bool HttpServer::peerAllowed(uint32_t a, const Bind& b) {
    if (a == b.addr) return true;  // this PC through that address
    if (b.loopback) return (ntohl(a) >> 24) == 127;  // the tunnel origins: only cloudflared / tailscaled on this PC
    if (b.tailscale) return (ntohl(a) & 0xFFC00000u) == 0x64400000u;  // 100.64.0.0/10 (the tailnet)
    return b.mask != 0 && (a & b.mask) == (b.addr & b.mask);
}

void HttpServer::acceptLoop() {
    std::vector<WSAEVENT> evs;
    for (const Listener& l : listeners_)
        if (l.ev) evs.push_back(l.ev);
    if (wake_) evs.push_back(wake_);
    while (!stop_) {
        // Wakes for a connection or stop(); every 30 s anyway (finished connections are reaped).
        const DWORD r = evs.empty() ? (Sleep(200), WSA_WAIT_TIMEOUT) : WSAWaitForMultipleEvents(DWORD(evs.size()), evs.data(), FALSE, 30000, FALSE);
        {  // reap finished connections
            std::list<Conn> dead;
            {
                std::lock_guard<std::mutex> lk(cmu_);
                for (auto it = conns_.begin(); it != conns_.end();) {
                    auto next = std::next(it);
                    if (it->done) dead.splice(dead.end(), conns_, it);
                    it = next;
                }
            }
            for (Conn& c : dead)
                if (c.t.joinable()) c.t.join();
        }
        if (stop_) break;
        if (r == WSA_WAIT_TIMEOUT || r == WSA_WAIT_FAILED) {
            if (r == WSA_WAIT_FAILED) Sleep(100);
            continue;
        }
        for (const Listener& l : listeners_) {
            WSANETWORKEVENTS ne{};
            if (!l.ev || WSAEnumNetworkEvents(l.s, l.ev, &ne) != 0 || !(ne.lNetworkEvents & FD_ACCEPT)) continue;
            for (int pass = 0; pass < 64; ++pass) {  // every connection waiting on this listener
            sockaddr_in peer{};
            int plen = sizeof(peer);
            SOCKET s = accept(l.s, reinterpret_cast<sockaddr*>(&peer), &plen);
            if (s == INVALID_SOCKET) break;
            // The accepted socket inherits the listener's event selection and non-blocking mode: undo both.
            WSAEventSelect(s, nullptr, 0);
            u_long blocking = 0;
            ioctlsocket(s, FIONBIO, &blocking);
            char ip[INET_ADDRSTRLEN] = "?";
            inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
            if (!peerAllowed(peer.sin_addr.s_addr, l.bind)) {
                logf("http: %s refused (not on this %s)", ip, l.bind.loopback ? "PC" : l.bind.tailscale ? "tailnet" : "LAN");
                closesocket(s);
                continue;
            }
            std::lock_guard<std::mutex> lk(cmu_);
            int fromPeer = 0;
            for (const Conn& c : conns_)
                if (!c.done && c.peer == peer.sin_addr.s_addr) ++fromPeer;
            if (int(conns_.size()) >= kMaxConnections + (l.bind.loopback ? kMaxLoopback : 0) ||
                fromPeer >= (l.bind.loopback ? kMaxLoopback : kMaxPerPeer)) {
                logf("http: %s refused (too many connections)", ip);
                closesocket(s);
                continue;
            }
            DWORD to = kTimeoutMs;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
            conns_.emplace_back();
            Conn& c = conns_.back();
            c.s = s;
            c.peer = peer.sin_addr.s_addr;
            c.t = std::thread([this, &c, ipStr = std::string(ip), &l] {
                serve(c.s, ipStr, l);
                // Let the browser close first (it does once it has the whole
                // reply: Connection: close). Closing first ourselves leaves the
                // connection in TIME_WAIT for ~2 minutes, and with
                // SO_EXCLUSIVEADDRUSE that blocks binding this port again: 0.8.1
                // came back on the next port after a restart (owner's log
                // 23:11:34) and the phone's saved link broke. No FIN within 1.5 s:
                // an abortive close (RST, no TIME_WAIT either).
                if (!stop_) {
                    DWORD to = 1500;
                    setsockopt(c.s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
                    char drain[512];
                    int k = 0;
                    while ((k = recv(c.s, drain, sizeof(drain), 0)) > 0) {
                    }
                    if (k != 0) {
                        linger lg{1, 0};
                        setsockopt(c.s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&lg), sizeof(lg));
                    }
                } else {
                    linger lg{1, 0};
                    setsockopt(c.s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&lg), sizeof(lg));
                }
                {
                    std::lock_guard<std::mutex> lk2(cmu_);
                    closesocket(c.s);
                    c.s = INVALID_SOCKET;
                }
                c.done = true;
            });
            }
        }
    }
}

void HttpServer::serve(SOCKET s, const std::string& peer, const Listener& l) {
    std::string buf;
    char tmp[16384];
    size_t hdrEnd = std::string::npos;
    // 0.8.3: one deadline for the whole request (a peer dripping a byte every 9 s
    // used to hold a connection slot for as long as it liked).
    ULONGLONG deadline = GetTickCount64() + kRequestDeadlineMs;
    while ((hdrEnd = buf.find("\r\n\r\n")) == std::string::npos) {
        if (buf.size() > kMaxHeaderBytes || stop_ || GetTickCount64() > deadline) return;
        const int k = recv(s, tmp, sizeof(tmp), 0);
        if (k <= 0) return;
        buf.append(tmp, size_t(k));
    }
    Request req;
    req.peer = peer;
    req.local = l.bind.ip;
    req.tailscale = l.bind.tailscale;
    req.tunnel = l.bind.loopback;
    req.fixed = l.bind.fixed;
    const std::string head = buf.substr(0, hdrEnd);
    size_t lineEnd = head.find("\r\n");
    const std::string first = head.substr(0, lineEnd);
    const size_t sp1 = first.find(' '), sp2 = first.find(' ', sp1 + 1);
    auto reply = [&](const Response& r) {
        std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) + "\r\n";
        out += "Content-Type: " + r.type + "\r\nContent-Length: " + std::to_string(r.body.size()) + "\r\n";
        if (r.status == 303) out += "Location: " + r.body + "\r\n";
        out += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
               "Connection: close\r\n\r\n";
        if (req.method != "HEAD") out += r.body;
        sendAll(s, out.data(), out.size());
    };
    if (sp1 == std::string::npos || sp2 == std::string::npos) return reply({400, "text/plain", "bad request"});
    req.method = first.substr(0, sp1);
    std::string target = first.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t q = target.find('?');
    req.path = target.substr(0, q);
    if (q != std::string::npos) req.query = target.substr(q + 1);
    while (lineEnd != std::string::npos && lineEnd < head.size()) {
        const size_t next = head.find("\r\n", lineEnd + 2);
        const std::string line = head.substr(lineEnd + 2, next == std::string::npos ? std::string::npos : next - lineEnd - 2);
        const size_t c = line.find(':');
        if (c != std::string::npos) req.headers[lower(trim(line.substr(0, c)))] = trim(line.substr(c + 1));
        lineEnd = next;
    }
    // Where it came from (before the body: the body limit may depend on it).
    if (req.tunnel) {
        // Through a tunnel the socket peer is cloudflared / tailscaled on this PC: the
        // phone's own address is in a header. 0.8.3: only the header that relay itself
        // sets is believed (a client can send any header, the relay adds / overwrites
        // its own): cloudflared (外網連結 on 127.0.0.1, or the fixed link's cloudflared
        // fallback) -> CF-Connecting-IP; Tailscale Funnel (the fixed link) -> the LAST
        // X-Forwarded-For entry (earlier ones are whatever the client sent).
        const bool viaCf = !req.fixed || fixedViaCf_.load();
        std::string ip;
        if (viaCf) {
            if (auto it = req.headers.find("cf-connecting-ip"); it != req.headers.end()) ip = ipText(it->second);
        } else if (auto x = req.headers.find("x-forwarded-for"); x != req.headers.end()) {
            const std::string& v = x->second;
            const size_t c = v.rfind(',');
            ip = ipText(trim(c == std::string::npos ? v : v.substr(c + 1)));
        }
        req.peer = ip.empty() ? (req.fixed ? "funnel" : "tunnel") : ip;
    }
    size_t clen = 0;
    if (auto it = req.headers.find("content-length"); it != req.headers.end()) clen = size_t(atoll(it->second.c_str()));
    // 0.8.3: a big body only where the handler allows it for this very request
    // (routed and checked first: the tests' snapshot upload); 64 KB everywhere else.
    const size_t limit = bodyLimit_ ? std::min(kMaxBodyBytes, bodyLimit_(req)) : kSmallBodyBytes;
    if (clen > limit) return reply({413, "text/plain", "too large"});
    // A big body (an upload piece) gets 10 s + 1 s per 32 KB (a link down to ~256 kbps).
    if (clen > kSmallBodyBytes) deadline = GetTickCount64() + 10000 + ULONGLONG(clen / 32768) * 1000;
    req.body = buf.substr(hdrEnd + 4);
    while (req.body.size() < clen) {
        if (stop_ || GetTickCount64() > deadline) return;
        const int k = recv(s, tmp, sizeof(tmp), 0);
        if (k <= 0) return;
        req.body.append(tmp, size_t(k));
    }
    req.body.resize(std::min(req.body.size(), clen));
    Response r;
    try {
        r = handler_(req);
    } catch (...) {
        r = {500, "text/plain", "error"};
    }
    if (!r.file.empty()) {  // 0.8.3: a download, streamed in 1 MB pieces
        HANDLE f = CreateFileW(r.file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        LARGE_INTEGER size{};
        if (f == INVALID_HANDLE_VALUE || !GetFileSizeEx(f, &size)) {
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            return reply({404, "text/plain", "file gone"});
        }
        std::string ascii, pct;
        for (unsigned char c : r.fileName) {
            ascii += (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') ? char(c) : '_';
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') pct += char(c);
            else {
                char h[4];
                snprintf(h, sizeof(h), "%%%02X", c);
                pct += h;
            }
        }
        const std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " + std::to_string(size.QuadPart) +
                                 "\r\nContent-Disposition: attachment; filename=\"" + ascii + "\"; filename*=UTF-8''" + pct +
                                 "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\nConnection: close\r\n\r\n";
        bool ok = sendAll(s, hdr.data(), hdr.size());
        std::vector<char> chunk(1 << 20);
        for (int64_t left = size.QuadPart; ok && left > 0 && !stop_ && req.method != "HEAD";) {
            DWORD got = 0;
            if (!ReadFile(f, chunk.data(), DWORD(std::min<int64_t>(left, int64_t(chunk.size()))), &got, nullptr) || got == 0) break;
            ok = sendAll(s, chunk.data(), got);
            left -= got;
        }
        CloseHandle(f);
        return;
    }
    if (r.websocket) {
        auto up = req.headers.find("upgrade"), key = req.headers.find("sec-websocket-key");
        if (up == req.headers.end() || lower(up->second) != "websocket" || key == req.headers.end() || key->second.size() > 64)
            return reply({400, "text/plain", "websocket expected"});
        const std::string out = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                                wsAccept(key->second) + "\r\n\r\n";
        if (!sendAll(s, out.data(), out.size())) return;
        // A long-lived connection: the page pings every 0.5 s (throttled to <= 1 / min hidden)
        // (0.8.3; 0.8.2 had no read timeout: a silent peer held the slot for ever).
        DWORD wsTo = kWsReadTimeoutMs;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&wsTo), sizeof(wsTo));
        BOOL nodelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
        WsConn ws(s);
        ws.peer = req.peer;
        try {
            r.websocket(ws);
        } catch (const std::exception& e) {
            logf("http: WebSocket handler failed: %s", e.what());
        } catch (...) {
            logf("http: WebSocket handler failed");
        }
        ws.close();
        return;
    }
    reply(r);
}

}  // namespace pm::remote
