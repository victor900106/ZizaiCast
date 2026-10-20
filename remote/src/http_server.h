#pragma once
// Minimal HTTP/1.1 server for the remote page and SDP exchange. Same rules
// as 傳到手機 (share/src/share_server.cpp): bound to chosen addresses only
// (never 0.0.0.0): the LAN interface, plus the Tailscale interface when there
// is one; a LAN peer must be on that interface's subnet, a Tailscale peer in
// 100.64.0.0/10 (the tailnet); connection caps (total / per address), request
// size caps, short timeouts, every peer IP logged. One request per connection
// (Connection: close). The token is checked by the handler.

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pm::remote {

// One WebSocket (RFC 6455, server side) on a connection the server handed
// over: the tunnel path (cloudflared quick tunnel -> 127.0.0.1) carries the
// page's control messages, and H.264 / Opus when WebRTC cannot get through.
// send*() are serialised (one writer at a time); run() reads until the peer
// closes or close() is called.
class WsConn {
public:
    explicit WsConn(SOCKET s) : s_(s) {}
    bool sendText(const std::string& t) { return sendFrame(0x1, t.data(), t.size()); }
    bool sendBinary(const void* p, size_t n) { return sendFrame(0x2, p, n); }
    void close();
    bool closed() const { return closed_; }
    // Calls onText for every text message; binary messages are ignored. Returns when closed.
    void run(const std::function<void(const std::string&)>& onText);
    std::string peer;  // the phone's address (CF-Connecting-IP)

private:
    bool sendFrame(int opcode, const void* p, size_t n);
    SOCKET s_;
    std::mutex wmu_;
    std::atomic<bool> closed_{false};
};

class HttpServer {
public:
    struct Bind {
        std::string ip;         // dotted IPv4
        uint32_t addr = 0;      // network order
        uint32_t mask = 0;      // network order (LAN: the subnet peers must be on)
        bool tailscale = false; // peers: 100.64.0.0/10 instead of the subnet
        bool loopback = false;  // 127.0.0.1: the tunnel's origin (cloudflared runs on this PC); peers: this PC only
        bool fixed = false;     // 127.0.0.2: the fixed link's origin (常駐外網: Tailscale Funnel / cloudflared)
    };
    struct Request {
        std::string method, path, query, body, peer;
        std::string local;       // the bound address the request came in on
        bool tailscale = false;  // ... the Tailscale one
        bool tunnel = false;     // ... the loopback one: came through the tunnel; peer = CF-Connecting-IP
        bool fixed = false;      // ... the fixed link's one (127.0.0.2); peer = X-Forwarded-For / CF-Connecting-IP
        std::map<std::string, std::string> headers;  // lower-case names
    };
    struct Response {
        int status = 200;
        std::string type = "text/plain; charset=utf-8";
        std::string body;
        // Set (with status 101) on a WebSocket upgrade request: the connection
        // becomes a WsConn handed to this function (it runs on the connection's thread).
        std::function<void(WsConn&)> websocket;
    };
    using Handler = std::function<Response(const Request&)>;

    ~HttpServer() { stop(); }
    // Listens on every bind at the same port (0 = any free one, the same on
    // all). False if the first (LAN) bind fails; a failing extra bind is
    // logged and skipped.
    bool start(const std::vector<Bind>& binds, uint16_t port, Handler h);
    void stop();
    uint16_t port() const { return port_; }
    std::vector<std::string> boundIps() const;

private:
    struct Listener {
        SOCKET s = INVALID_SOCKET;
        Bind bind;
    };
    void acceptLoop();
    void serve(SOCKET s, const std::string& peer, const Listener& l);
    static bool peerAllowed(uint32_t a, const Bind& b);

    struct Conn {
        std::thread t;
        SOCKET s = INVALID_SOCKET;
        uint32_t peer = 0;
        std::atomic<bool> done{false};
    };
    Handler handler_;
    std::vector<Listener> listeners_;
    std::thread acceptThread_;
    std::atomic<bool> stop_{false};
    uint16_t port_ = 0;
    std::mutex cmu_;
    std::list<Conn> conns_;
    bool wsa_ = false;
};

}  // namespace pm::remote
