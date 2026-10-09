// 傳到手機 / Send to phone, LAN path (iPhone / iPad, or an Android phone
// without adb): a tiny HTTP server on the PC's LAN address that serves ONLY
// the files the user chose, behind a random unguessable token, for a limited
// time (default 10 minutes). The phone scans a QR code of url() with its
// camera and gets a mobile page (zh / en / ja / ko, themed like the app) with
// the pictures (long-press → 「加入照片」 in iOS Safari) and videos (play + a
// download button with instructions), plus 「全部下載（ZIP）」 (/<token>/zip:
// every file as one stored ZIP, streamed; the iPhone opens it in Files) --
// or 「全部儲存」 (navigator.share with all files) where the browser can share
// files, which needs a secure context, so not on this http page. Live mode
// (自動傳到手機): the share may start empty, addFile() adds
// captures while it runs and the open page shows them within a second
// (long-polled /list). See docs/share.md.
//
// Security: bound to one LAN interface address (never 0.0.0.0), random port,
// 32-character token (~190 bits) required in every path, constant-time
// compared; no directory listing (files are addressed by index only); peers
// outside the interface's subnet are refused; every request is logged with
// the peer IP; at most 24 connections, 8 per peer address, and a connection
// that has not sent a request with the token within 5 s of accept (or sends
// a wrong token) is closed, so one LAN host cannot hold every slot. Expiry: no new requests are answered (410) after the deadline;
// stop() cuts everything at once.
//
// Threading: start / stop / getters from any thread (the app uses its UI
// thread). `log` and `onAccess` / `onExpired` are called from the server's
// threads -- marshal to the UI thread yourself. Set them before start().
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pm::share {

// IPv4 interfaces that phones on the LAN can reach, best first: up, not
// loopback / link-local, real adapters before VPN / VM ones (those only when
// nothing else is up), adapters with a gateway first, then route metric --
// the same rules as the AirPlay mDNS responder (core/src/mdnsd).
struct LanInterface {
    std::string name;   // friendly name (UTF-8), e.g. "Wi-Fi"
    std::string ip;     // dotted IPv4
    uint32_t addr = 0;  // network byte order
    uint32_t mask = 0;  // network byte order
    bool isVirtual = false;
    bool gateway = false;
};
std::vector<LanInterface> lanInterfaces();

// QR code of `text` (ECC medium) as a square BGRA image (white quiet zone of
// `border` modules, `scale` px per module, opaque). False if too long.
bool renderQr(const std::string& text, int scale, int border, std::vector<uint8_t>& bgra, int& size);

class Server {
public:
    struct Palette {  // 0xRRGGBB, the app's current theme (pm::ui::currentColors)
        uint32_t background = 0x1E1416, cardTop = 0x332326, cardBottom = 0x2A1C1F, fg = 0xF6E9EA, dim = 0xBFA9AC,
                 accent = 0xF5A7A7;
        bool light = false;
    };
    struct Options {
        // Addresses to prefer, best first, e.g. the AirPlay server's
        // advertisedInterfaces() ("Wi-Fi=192.168.1.20" or just the IP); the
        // first one that is a current LAN interface wins, else lanInterfaces()[0].
        std::vector<std::string> preferred;
        // Tests: bind exactly here (e.g. "127.0.0.1"); empty = a LAN interface.
        std::string bindIp;
        int ttlSeconds = 600;  // live: the hard cap (the app uses 12 h; the toggle / 停止 end it sooner)
        bool live = false;     // 自動傳到手機: may start with no file, addFile() later, the page updates itself
        bool english = false;  // page language (when lang < 0)
        int lang = -1;         // page language as pm::i18n::Lang (0 zh-TW, 1 en, 2 ja, 3 ko); -1: by `english`
        std::wstring appName = L"自在投影";
        Palette palette;
        uint16_t port = 0;     // 0 = random free port in 20000..60999
    };
    struct Access {            // one answered request
        std::string ip;        // peer
        int file = -1;         // index into files(), -1 = the page
        bool download = false; // the 下載 link (attachment)
        int status = 200;
    };

    Server();
    ~Server();  // stop()
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    std::function<void(const std::string&)> log;  // "share: …" lines
    std::function<void(const Access&)> onAccess;
    std::function<void()> onExpired;               // once, at the deadline

    // Starts serving `files` (absolute paths; missing / unreadable ones are
    // skipped; at most kMaxFiles). A running share is stopped first.
    // False (and *error) if no file is usable or nothing could be bound.
    // Live mode may start with no file.
    bool start(const std::vector<std::wstring>& files, const Options& options, std::string* error = nullptr);
    void stop();
    // Adds a file to the running share (any mode; the page picks it up at its
    // next load, a live page at once). Returns its index (an already shared
    // path keeps its index), -1 if not running / expired / missing / full.
    int addFile(const std::wstring& path);
    bool live() const;
    int fileCount() const;
    bool running() const;  // started and not stopped (true after expiry too, until stop())
    bool expired() const;
    std::string url() const;                // http://ip:port/<token>/
    std::string ip() const;
    uint16_t port() const;
    std::string interfaceName() const;      // e.g. "Wi-Fi" ("" when bindIp was forced)
    int secondsLeft() const;                // 0 once expired
    std::vector<std::wstring> files() const;
    static constexpr int kMaxFiles = 50;       // per start()
    static constexpr int kMaxLiveFiles = 500;  // per share, with addFile()

    // Pure helpers (unit-tested).
    static std::string contentType(const std::wstring& path);
    static bool isVideo(const std::wstring& path);
    // "bytes=a-b" against a file of `size` bytes → [first, last]; false =
    // unsatisfiable (416). A malformed header gives whole = true (ignore it).
    static bool parseRange(const std::string& header, uint64_t size, uint64_t& first, uint64_t& last, bool& whole);

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

}  // namespace pm::share
