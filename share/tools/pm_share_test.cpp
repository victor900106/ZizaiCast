// Offline tests for 傳到手機 (no phone, no LAN: everything on 127.0.0.1).
//   pm_share_test                      run the checks
//   pm_share_test --serve N [--en|--lang L] [--live] [--ttl S] FILE... [--later FILE]... [--every S]
//                                      serve FILEs for N s on
//                                      127.0.0.1 and print the URL (page shots)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "page.h"
#include "pm/share_server.h"
#if PM_SHARE_TEST_ANDROID
#include "pm/android_source.h"
#endif

namespace fs = std::filesystem;
using namespace std::chrono_literals;

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const std::string& what) {
    (ok ? g_pass : g_fail)++;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
}
static void section(const char* s) { std::printf("\n== %s\n", s); }

static std::string u8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// ---- a minimal HTTP client --------------------------------------------------
struct Resp {
    int status = 0;
    std::string head, body;
    bool closed = false;
    std::string header(const std::string& name) const {
        std::string lh = head, ln = name + ":";
        for (auto& c : lh) c = char(tolower(c));
        for (auto& c : ln) c = char(tolower(c));
        const size_t p = lh.find("\r\n" + ln);
        if (p == std::string::npos) return {};
        size_t s = p + 2 + ln.size(), e = head.find("\r\n", s);
        while (s < e && head[s] == ' ') ++s;
        return head.substr(s, e - s);
    }
};

static SOCKET connectTo(uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    DWORD to = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

// Reads one response (Content-Length framed; HEAD: headers only). `pending`
// keeps bytes that belong to the next response.
static Resp readResp(SOCKET s, bool headOnly, std::string& pending) {
    Resp r;
    std::string& b = pending;
    size_t end;
    while ((end = b.find("\r\n\r\n")) == std::string::npos) {
        char tmp[8192];
        const int k = recv(s, tmp, sizeof(tmp), 0);
        if (k <= 0) {
            r.closed = true;
            return r;
        }
        b.append(tmp, size_t(k));
    }
    r.head = b.substr(0, end);
    b.erase(0, end + 4);
    std::sscanf(r.head.c_str(), "HTTP/1.1 %d", &r.status);
    const std::string cl = r.header("Content-Length");
    const size_t len = headOnly || cl.empty() ? 0 : size_t(std::stoull(cl));
    while (b.size() < len) {
        char tmp[65536];
        const int k = recv(s, tmp, sizeof(tmp), 0);
        if (k <= 0) {
            r.closed = true;
            break;
        }
        b.append(tmp, size_t(k));
    }
    r.body = b.substr(0, std::min(len, b.size()));
    b.erase(0, r.body.size());
    return r;
}

static Resp get(uint16_t port, const std::string& path, const std::string& extra = {}, const char* method = "GET") {
    SOCKET s = connectTo(port);
    Resp r;
    if (s == INVALID_SOCKET) {
        r.status = -1;
        return r;
    }
    const std::string req = std::string(method) + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n" + extra +
                            "Connection: close\r\n\r\n";
    send(s, req.data(), int(req.size()), 0);
    std::string pending;
    r = readResp(s, std::string(method) == "HEAD", pending);
    closesocket(s);
    return r;
}

static std::string pathOf(const std::string& url) {  // http://ip:port/tok/ → /tok/
    const size_t p = url.find('/', 7);
    return p == std::string::npos ? "/" : url.substr(p);
}

static std::string fileBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

static fs::path tempDir() {
    fs::path d = fs::temp_directory_path() / L"pm_share_test";
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

// ---------------------------------------------------------------- tests --
static void testPure() {
    section("helpers");
    using pm::share::Server;
    uint64_t a, b;
    bool whole;
    check(Server::parseRange("bytes=0-9", 100, a, b, whole) && !whole && a == 0 && b == 9, "range 0-9");
    check(Server::parseRange("bytes=90-", 100, a, b, whole) && !whole && a == 90 && b == 99, "range 90-");
    check(Server::parseRange("bytes=-10", 100, a, b, whole) && !whole && a == 90 && b == 99, "range -10 (suffix)");
    check(Server::parseRange("bytes=-500", 100, a, b, whole) && !whole && a == 0 && b == 99, "suffix longer than file");
    check(Server::parseRange("bytes=50-5000", 100, a, b, whole) && !whole && a == 50 && b == 99, "end clamped");
    check(!Server::parseRange("bytes=100-", 100, a, b, whole), "start past end → 416");
    check(Server::parseRange("bytes=5-1", 100, a, b, whole) && whole, "reversed → ignored (200)");
    check(Server::parseRange("items=1-2", 100, a, b, whole) && whole, "other unit → ignored");
    check(Server::parseRange("bytes=0-1,5-6", 100, a, b, whole) && !whole && a == 0 && b == 1, "multi-range → first");
    check(Server::parseRange("bytes=abc", 100, a, b, whole) && whole, "garbage → ignored");
    check(Server::contentType(L"a.PNG") == "image/png" && Server::contentType(L"x.jpeg") == "image/jpeg" &&
              Server::contentType(L"v.mp4") == "video/mp4" && Server::contentType(L"v.MOV") == "video/quicktime" &&
              Server::contentType(L"x.exe") == "application/octet-stream",
          "content types");
    check(Server::isVideo(L"自在投影_1.mp4") && !Server::isVideo(L"自在投影_1.png"), "isVideo");
    check(pm::share::htmlEscape("<a href=\"x\">&'") == "&lt;a href=&quot;x&quot;&gt;&amp;&#39;", "html escape");
    check(pm::share::jsonEscape("</script>\"\\\n&") == "\\u003c/script\\u003e\\\"\\\\\\n\\u0026",
          "json escape (no raw <, quotes, backslash, newline, &)");
#if PM_SHARE_TEST_ANDROID
    using pm::AndroidSource;
    check(AndroidSource::galleryPath(L"自在投影_20261008_101010.png") ==
              "/sdcard/Pictures/ZizaiCast/ZizaiCast_20261008_101010.png",
          "gallery path: 自在投影_… → ZizaiCast_… (Pictures)");
    check(AndroidSource::galleryPath(L"ZizaiCast_20261008_101010_2.MP4") ==
              "/sdcard/Movies/ZizaiCast/ZizaiCast_20261008_101010_2.mp4",
          "gallery path: video → Movies, ext lower-case");
    check(AndroidSource::galleryPath(L"我的 截圖 (1).jpg") == "/sdcard/Pictures/ZizaiCast/ZizaiCast_1.jpg" ||
              AndroidSource::galleryPath(L"我的 截圖 (1).jpg").rfind("/sdcard/Pictures/ZizaiCast/ZizaiCast", 0) == 0,
          "gallery path: non-ASCII name → ASCII (" + AndroidSource::galleryPath(L"我的 截圖 (1).jpg") + ")");
    check(AndroidSource::galleryPath(L"a;rm -rf $(x)`.png").find_first_of(";$`() ") == std::string::npos,
          "gallery path: shell metacharacters dropped (" + AndroidSource::galleryPath(L"a;rm -rf $(x)`.png") + ")");
#endif
}

static void testServer(const fs::path& dir) {
    section("HTTP server on 127.0.0.1");
    // A "screenshot" (bytes 0..255 repeated) and a "recording".
    const fs::path png = dir / L"自在投影_20261008_101010.png", mp4 = dir / L"自在投影_20261008_101500.mp4",
                   secret = dir / L"secret.txt";
    {
        std::string a(70000, '\0'), v(300000, '\0');
        for (size_t i = 0; i < a.size(); ++i) a[i] = char(i * 7);
        for (size_t i = 0; i < v.size(); ++i) v[i] = char(i * 13 + 1);
        std::ofstream(png, std::ios::binary) << a;
        std::ofstream(mp4, std::ios::binary) << v;
        std::ofstream(secret, std::ios::binary) << "do not serve";
    }
    pm::share::Server srv;
    std::vector<std::string> logs;
    std::mutex lmu;
    srv.log = [&](const std::string& s) {
        std::lock_guard<std::mutex> lk(lmu);
        logs.push_back(s);
    };
    std::atomic<int> accesses{0}, expiredCalls{0};
    srv.onAccess = [&](const pm::share::Server::Access&) { ++accesses; };
    srv.onExpired = [&] { ++expiredCalls; };
    pm::share::Server::Options o;
    o.bindIp = "127.0.0.1";
    o.ttlSeconds = 4;
    std::string err;
    const bool started = srv.start({png.wstring(), mp4.wstring(), (dir / L"missing.png").wstring()}, o, &err);
    check(started, "start (" + err + ")");
    if (!started) return;
    const std::string url = srv.url();
    const uint16_t port = srv.port();
    const std::string base = pathOf(url);  // /<token>/
    std::printf("     url %s\n", url.c_str());
    check(url.rfind("http://127.0.0.1:", 0) == 0 && base.size() == 34 && base.back() == '/', "url = http://ip:port/<32-char token>/");
    check(srv.files().size() == 2, "missing file skipped (2 served)");
    check(port >= 20000 && port < 61000, "random port " + std::to_string(port));
    check(srv.secondsLeft() >= 3 && srv.secondsLeft() <= 4, "secondsLeft");
    const std::string tok = base.substr(1, 32);

    // Token required, nothing else exists.
    check(get(port, "/").status == 404, "GET / → 404 (no listing)");
    check(get(port, "/favicon.ico").status == 404, "GET /favicon.ico → 404");
    std::string wrong = tok;
    wrong[5] = wrong[5] == 'A' ? 'B' : 'A';
    check(get(port, "/" + wrong + "/").status == 404, "wrong token → 404");
    check(get(port, "/" + wrong + "/v/0").status == 404, "wrong token file → 404");
    check(get(port, "/" + tok.substr(0, 31) + "/").status == 404, "short token → 404");
    check(get(port, base + "v/2").status == 404, "index out of range → 404");
    check(get(port, base + "v/-1").status == 404 && get(port, base + "v/00x").status == 404, "bad index → 404");
    check(get(port, base + "..%2Fsecret.txt").status == 404 && get(port, base + "../secret.txt").status == 404 &&
              get(port, base + "v/0/../../secret.txt").body.find("do not serve") == std::string::npos &&
              get(port, "/" + tok + "/..\\secret.txt").status == 404,
          "no path traversal (only indexes exist)");
    check(get(port, base + "x/0").status == 404, "unknown route → 404");
    check(get(port, base, {}, "POST").status == 405, "POST → 405");
    {
        Resp r = get(port, "/" + tok);
        check(r.status == 301 && r.header("Location") == base, "/<token> → 301 /<token>/");
    }
    // Page.
    {
        Resp r = get(port, base, "User-Agent: Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X)\r\n");
        check(r.status == 200 && r.header("Content-Type") == "text/html; charset=utf-8", "page 200 text/html");
        check(r.body.find(u8(L"自在投影_20261008_101010.png")) != std::string::npos &&
                  r.body.find("src=\"v/0\"") != std::string::npos && r.body.find("src=\"v/1#t=0.1\"") != std::string::npos,
              "page lists both files (img v/0, video v/1)");
        check(r.body.find(u8(L"加入照片")) != std::string::npos && r.body.find(u8(L"儲存影片")) != std::string::npos,
              "iPhone page: 加入照片 + 儲存影片 steps (zh)");
        check(r.body.find("secret") == std::string::npos && r.body.find("missing.png") == std::string::npos,
              "page shows only shared files");
        check(r.header("Cache-Control") == "no-store" && r.header("Referrer-Policy") == "no-referrer" &&
                  r.header("X-Content-Type-Options") == "nosniff" &&
                  r.header("Content-Security-Policy").find("default-src 'none'") != std::string::npos,
              "security headers (no-store, no-referrer, nosniff, CSP)");
        {  // Script only from the same origin: the external app.js and a JSON data block (not executed).
            size_t scripts = 0, p = 0;
            bool onlyOurs = true;
            while ((p = r.body.find("<script", p)) != std::string::npos) {
                ++scripts;
                const std::string tag = r.body.substr(p, r.body.find('>', p) - p);
                onlyOurs &= tag == "<script type=\"application/json\" id=\"pm-t\"" || tag == "<script src=\"app.js\"";
                ++p;
            }
            const std::string csp = r.header("Content-Security-Policy");
            check(scripts == 2 && onlyOurs && csp.find("script-src 'self';") != std::string::npos &&
                      csp.find("connect-src 'self';") != std::string::npos && csp.find("unsafe-eval") == std::string::npos &&
                      csp.find("script-src 'self' 'unsafe-inline'") == std::string::npos,
                  "scripts: only app.js (same origin) + the JSON text block; CSP script-src 'self', no inline");
            const size_t j = r.body.find("id=\"pm-t\">");
            const std::string data = j == std::string::npos ? "" : r.body.substr(j, r.body.find("</script>", j) - j);
            check(!data.empty() && data.find('<') == std::string::npos && data.find("\"saveAll\"") != std::string::npos,
                  "page texts block: JSON with no raw '<' (cannot close the script)");
            check(r.body.find("id=\"saveall\"") != std::string::npos && r.body.find("data-save=\"0\"") != std::string::npos &&
                      r.body.find("data-live=\"0\"") != std::string::npos,
                  "page: 全部儲存 bar (hidden until the script) + per-file save hooks, not live");
        }
        {
            Resp js = get(port, base + "app.js");
            check(js.status == 200 && js.header("Content-Type") == "text/javascript; charset=utf-8" &&
                      js.body.find("navigator.share") != std::string::npos && js.body.find("list?n=") != std::string::npos,
                  "app.js 200 text/javascript (navigator.share, live poll)");
            Resp l = get(port, base + "list?n=0");  // not live: answered at once
            check(l.status == 200 && l.header("Content-Type") == "application/json; charset=utf-8" &&
                      l.body.find("\"live\":false,\"n\":2") != std::string::npos &&
                      l.body.find("\"type\":\"video/mp4\"") != std::string::npos,
                  "list: JSON of the 2 files (" + l.body.substr(0, 60) + "…)");
            check(get(port, "/" + wrong + "/list").status == 404 && get(port, "/" + wrong + "/app.js").status == 404,
                  "list / app.js need the token");
        }
        Resp a = get(port, base, "User-Agent: Mozilla/5.0 (Linux; Android 14; Pixel 8)\r\n");
        check(a.body.find(u8(L"下載圖片")) != std::string::npos && a.body.find(u8(L"加入照片")) == std::string::npos,
              "Android page: Android tips only");
    }
    // Files.
    const std::string pngBytes = fileBytes(png), mp4Bytes = fileBytes(mp4);
    {
        Resp r = get(port, base + "v/0");
        check(r.status == 200 && r.body == pngBytes, "v/0 → whole file (" + std::to_string(r.body.size()) + " bytes)");
        check(r.header("Content-Type") == "image/png" && r.header("Accept-Ranges") == "bytes", "image/png, Accept-Ranges");
        check(r.header("Content-Disposition").rfind("inline; filename=\"ZizaiCast_20261008_101010.png\"; filename*=UTF-8''%E8%87%AA", 0) == 0,
              "inline + ASCII fallback + UTF-8 filename* (" + r.header("Content-Disposition") + ")");
        Resp d = get(port, base + "d/0");
        check(d.status == 200 && d.header("Content-Disposition").rfind("attachment;", 0) == 0 && d.body == pngBytes,
              "d/0 → attachment");
        Resp v = get(port, base + "v/1");
        check(v.status == 200 && v.header("Content-Type") == "video/mp4" && v.body == mp4Bytes, "v/1 → video/mp4");
    }
    // Ranges (what Safari's video player asks for).
    {
        Resp r = get(port, base + "v/1", "Range: bytes=0-1\r\n");
        check(r.status == 206 && r.body == mp4Bytes.substr(0, 2) &&
                  r.header("Content-Range") == "bytes 0-1/" + std::to_string(mp4Bytes.size()) && r.header("Content-Length") == "2",
              "Range 0-1 → 206 (Safari's probe)");
        r = get(port, base + "v/1", "Range: bytes=1000-1999\r\n");
        check(r.status == 206 && r.body == mp4Bytes.substr(1000, 1000), "Range 1000-1999");
        r = get(port, base + "v/1", "Range: bytes=299000-\r\n");
        check(r.status == 206 && r.body == mp4Bytes.substr(299000), "Range 299000- (open end)");
        r = get(port, base + "v/1", "Range: bytes=-100\r\n");
        check(r.status == 206 && r.body == mp4Bytes.substr(mp4Bytes.size() - 100), "Range -100 (suffix)");
        r = get(port, base + "v/1", "Range: bytes=300000-\r\n");
        check(r.status == 416 && r.header("Content-Range") == "bytes */300000", "Range past the end → 416");
        r = get(port, base + "v/1", {}, "HEAD");
        check(r.status == 200 && r.header("Content-Length") == "300000" && r.body.empty(), "HEAD → headers only");
    }
    // Keep-alive: two requests on one connection.
    {
        SOCKET s = connectTo(port);
        std::string pending;
        const std::string r1 = "GET " + base + "v/1 HTTP/1.1\r\nHost: x\r\nRange: bytes=0-9\r\n\r\n";
        send(s, r1.data(), int(r1.size()), 0);
        Resp a = readResp(s, false, pending);
        const std::string r2 = "GET " + base + "v/0 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        send(s, r2.data(), int(r2.size()), 0);
        Resp b = readResp(s, false, pending);
        closesocket(s);
        check(a.status == 206 && a.body == mp4Bytes.substr(0, 10) && b.status == 200 && b.body == pngBytes,
              "keep-alive: two requests, one connection");
    }
    // Slow-loris style garbage: closed, server still fine.
    {
        SOCKET s = connectTo(port);
        std::string junk(20000, 'A');
        send(s, junk.data(), int(junk.size()), 0);
        char c;
        const int k = recv(s, &c, 1, 0);
        closesocket(s);
        check(k <= 0 && get(port, base).status == 200, "oversized header → connection dropped, server alive");
    }
    check(accesses > 0, "onAccess called (" + std::to_string(accesses.load()) + ")");
    // Expiry.
    std::this_thread::sleep_for(std::chrono::milliseconds(srv.secondsLeft() * 1000 + 1600));
    check(srv.expired() && expiredCalls == 1 && srv.secondsLeft() == 0,
          "expired after ttl, onExpired once (expired=" + std::to_string(srv.expired()) +
              " calls=" + std::to_string(expiredCalls.load()) + " left=" + std::to_string(srv.secondsLeft()) + ")");
    {
        Resp r = get(port, base);
        check(r.status == 410 && r.body.find(u8(L"這個分享已結束")) != std::string::npos, "after expiry: page → 410 (zh)");
        check(get(port, base + "v/0").status == 410, "after expiry: file → 410");
        check(get(port, "/" + wrong + "/").status == 404, "after expiry: wrong token still 404");
    }
    // Stop.
    srv.stop();
    check(!srv.running() && get(port, base).status == -1, "stop() → connection refused");
    {
        std::lock_guard<std::mutex> lk(lmu);
        bool sawServe = false, sawRange = false, sawPage = false;
        for (auto& l : logs) {
            sawServe |= l.find("share: serving 2 file(s) on 127.0.0.1:") == 0;
            sawRange |= l.find("127.0.0.1 GET v/1 \"" + u8(L"自在投影_20261008_101500.mp4") + "\" 206 bytes 1000-1999/300000") != std::string::npos;
            sawPage |= l.find("127.0.0.1 GET page 200 (ios)") != std::string::npos;
        }
        check(sawServe && sawRange && sawPage, "access log lines (IP, file, status, range)");
        bool tokenLogged = false;
        for (auto& l : logs) tokenLogged |= l.find(tok) != std::string::npos;
        check(!tokenLogged, "token never written to the log");
    }
    // Restart: new token + port; English page; a stopped server refuses start without files.
    {
        o.english = true;
        o.ttlSeconds = 600;
        check(srv.start({png.wstring()}, o), "restart");
        const std::string base2 = pathOf(srv.url());
        check(base2 != base, "new token on restart");
        Resp r = get(srv.port(), base2, "User-Agent: Mozilla/5.0 (iPhone)\r\n");
        check(r.status == 200 && r.body.find("Save to Photos") != std::string::npos && r.body.find("lang=\"en\"") != std::string::npos,
              "English page");
        check(get(srv.port(), base).status == 404, "old token dead");
        srv.stop();
        std::string e2;
        check(!srv.start({(dir / L"nope.png").wstring()}, o, &e2) && !srv.running(), "no usable file → start fails (" + e2 + ")");
    }
    // Stop cuts a download in progress.
    {
        const fs::path big = dir / L"big.mp4";
        {
            std::ofstream f(big, std::ios::binary);
            std::string chunk(1 << 20, 'x');
            for (int i = 0; i < 64; ++i) f << chunk;
        }
        o.ttlSeconds = 60;
        srv.start({big.wstring()}, o);
        SOCKET s = connectTo(srv.port());
        const std::string rq = "GET " + pathOf(srv.url()) + "v/0 HTTP/1.1\r\nHost: x\r\n\r\n";
        send(s, rq.data(), int(rq.size()), 0);
        char buf[4096];
        recv(s, buf, sizeof(buf), 0);  // headers + some body; then do not read on (socket buffers fill)
        const auto t0 = std::chrono::steady_clock::now();
        srv.stop();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        size_t total = 0;
        for (int k; (k = recv(s, buf, sizeof(buf), 0)) > 0;) total += size_t(k);
        closesocket(s);
        check(ms < 2000 && total < (64u << 20), "stop() during a 64 MB download returns in " + std::to_string(ms) +
                                                    " ms, transfer cut (" + std::to_string(total >> 10) + " KB received)");
    }
}

// 自動傳到手機: a live share starts empty, files are added while it runs and a
// waiting list?n=K answers as soon as file K+1 exists.
static void testLive(const fs::path& dir) {
    section("live share (自動傳到手機)");
    const fs::path a = dir / L"自在投影_20261008_120000.png", b = dir / L"自在投影_20261008_120100.mp4";
    std::ofstream(a, std::ios::binary) << std::string(5000, 'a');
    std::ofstream(b, std::ios::binary) << std::string(9000, 'b');
    pm::share::Server srv;
    std::atomic<int> expiredCalls{0};
    srv.onExpired = [&] { ++expiredCalls; };
    pm::share::Server::Options o;
    o.bindIp = "127.0.0.1";
    o.live = true;
    o.ttlSeconds = 7;
    std::string err;
    check(srv.start({}, o, &err) && srv.live() && srv.fileCount() == 0, "live share starts with no file (" + err + ")");
    const uint16_t port = srv.port();
    const std::string base = pathOf(srv.url());
    {
        Resp r = get(port, base, "User-Agent: Mozilla/5.0 (iPhone)\r\n");
        check(r.status == 200 && r.body.find("data-live=\"1\" data-n=\"0\"") != std::string::npos &&
                  r.body.find("id=\"empty\">") != std::string::npos && r.body.find(u8(L"即時更新")) != std::string::npos,
              "empty live page: 即時更新 + waiting card");
    }
    // A waiting list?n=0 returns when the first file arrives (not before).
    {
        Resp res;
        auto t0 = std::chrono::steady_clock::now();
        std::thread t([&] { res = get(port, base + "list?n=0"); });
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        const int idx = srv.addFile(a.wstring());
        t.join();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        check(idx == 0 && res.status == 200 && res.body.find("\"n\":1") != std::string::npos &&
                  res.body.find(u8(L"自在投影_20261008_120000.png")) != std::string::npos,
              "list?n=0 held until addFile, then the new file (" + std::to_string(ms) + " ms)");
        check(ms >= 1400 && ms < 3500, "long poll: waited for the file, answered at once after it (" + std::to_string(ms) + " ms)");
    }
    check(srv.addFile(a.wstring()) == 0, "same file again → same index");
    check(srv.addFile((dir / L"nope.png").wstring()) == -1, "missing file → -1");
    check(srv.addFile(b.wstring()) == 1 && srv.fileCount() == 2, "second file → index 1");
    {
        Resp r = get(port, base + "list?n=1");  // already 2: at once
        check(r.status == 200 && r.body.find("\"n\":2") != std::string::npos && r.body.find("\"video\":true") != std::string::npos,
              "list?n=1 with 2 files → at once");
        Resp p = get(port, base, "User-Agent: Mozilla/5.0 (iPhone)\r\n");
        const size_t c1 = p.body.find("data-i=\"1\""), c0 = p.body.find("data-i=\"0\"");
        check(c1 != std::string::npos && c0 != std::string::npos && c1 < c0 && p.body.find("id=\"empty\" hidden") != std::string::npos,
              "live page: newest first, waiting card hidden");
        check(get(port, base + "v/1").body == std::string(9000, 'b'), "added file served (v/1)");
    }
    // Expiry wakes a waiting list request with 410 (the test client gives up after 5 s).
    {
        while (srv.secondsLeft() > 2) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        Resp res;
        auto t0 = std::chrono::steady_clock::now();
        std::thread t([&] { res = get(port, base + "list?n=2"); });
        t.join();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        check(res.status == 410 && expiredCalls == 1 && ms < 4000,
              "live cap reached: the waiting list request → 410 at the deadline (" + std::to_string(ms) + " ms)");
        Resp e = get(port, base);
        check(e.status == 410 && e.body.find(u8(L"自動傳送已經停止")) != std::string::npos, "expired live page text");
        check(srv.addFile(b.wstring()) == -1, "no addFile after expiry");
    }
    // stop() releases a waiting list request at once.
    {
        o.ttlSeconds = 60;
        srv.start({a.wstring()}, o);
        const uint16_t p2 = srv.port();
        const std::string base2 = pathOf(srv.url());
        Resp res;
        std::thread t([&] { res = get(p2, base2 + "list?n=1"); });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const auto t0 = std::chrono::steady_clock::now();
        srv.stop();
        t.join();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        check(ms < 1500 && res.status != 200, "stop() with a list request waiting returns in " + std::to_string(ms) + " ms");
    }
    // A non-live share: addFile works too (the page shows it on reload), start without a file still fails.
    {
        pm::share::Server::Options s = o;
        s.live = false;
        std::string e2;
        check(!srv.start({}, s, &e2), "non-live share without a file → start fails (" + e2 + ")");
        check(srv.start({a.wstring()}, s) && srv.addFile(b.wstring()) == 1 &&
                  get(srv.port(), pathOf(srv.url()) + "list").body.find("\"n\":2") != std::string::npos,
              "non-live share: addFile → listed");
        srv.stop();
    }
}

static void testQr(const fs::path& dir) {
    section("QR code");
    const std::string url = "http://192.168.1.23:48213/Ab3dE6fGh9Jk2LmN4pQr5StU7vWx8YzA/";
    std::vector<uint8_t> bgra;
    int size = 0;
    const bool qrOk = pm::share::renderQr(url, 6, 4, bgra, size);
    check(qrOk && size > 0, "render " + std::to_string(size) + " px");
    // 24-bit BMP for OpenCV.
    const fs::path bmp = dir / L"qr.bmp";
    {
        const int rowBytes = (size * 3 + 3) & ~3;
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        ih.biSize = sizeof(ih);
        ih.biWidth = size;
        ih.biHeight = size;
        ih.biPlanes = 1;
        ih.biBitCount = 24;
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + rowBytes * size;
        std::ofstream f(bmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
        f.write(reinterpret_cast<const char*>(&ih), sizeof(ih));
        std::vector<char> row(size_t(rowBytes), 0);
        for (int y = size - 1; y >= 0; --y) {
            for (int x = 0; x < size; ++x)
                for (int c = 0; c < 3; ++c) row[size_t(x * 3 + c)] = char(bgra[(size_t(y) * size + x) * 4 + c]);
            f.write(row.data(), rowBytes);
        }
    }
    const std::wstring cmd = L"py \"" + std::wstring(PM_VERIFY_QR) + L"\" \"" + bmp.wstring() + L"\"";
    std::string out;
    if (FILE* p = _wpopen(cmd.c_str(), L"rb")) {
        char b[512];
        for (size_t k; (k = fread(b, 1, sizeof(b), p)) > 0;) out.append(b, k);
        _pclose(p);
    }
    check(out == url, "OpenCV decodes the QR back to the URL (\"" + out + "\")");
}

#if PM_SHARE_TEST_ANDROID
static void testAndroid(const fs::path& dir) {
    section("AndroidSource::pushToGallery vs fake adb");
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const fs::path tools = dir / L"android-tools";
    fs::create_directories(tools);
    fs::copy_file(fs::path(self).parent_path() / L"pm_share_fake_adb.exe", tools / L"adb.exe",
                  fs::copy_options::overwrite_existing);
    std::ofstream(tools / L"scrcpy-server") << "dummy";
    const fs::path logFile = dir / L"fake_adb.log";
    SetEnvironmentVariableW(L"FAKE_ADB_LOG", logFile.wstring().c_str());
    SetEnvironmentVariableW(L"PM_ADB_PORT", L"15096");
    const fs::path png = dir / L"自在投影_20261008_101010.png", mp4 = dir / L"自在投影_20261008_101500.mp4";

    auto runCase = [&](const wchar_t* mode, const fs::path& file, pm::AndroidSource::PushResult& res, bool& called) {
        std::error_code ec;
        fs::remove(logFile, ec);
        fs::remove(fs::path(logFile.wstring() + L".scanned"), ec);
        SetEnvironmentVariableW(L"FAKE_ADB_MODE", mode);
        pm::AndroidSource src;
        std::atomic<bool> connected{false}, done{false};
        src.events.onConnected = [&](const std::wstring& name) { connected = name == L"Pixel 8 測試"; };
        check(src.init(tools.wstring()), std::string("init (") + u8(mode) + ")");
        check(!src.pushToGallery(file.wstring(), nullptr), "no phone yet → pushToGallery false");
        src.connectKnownDevices();
        for (int i = 0; i < 100 && !connected; ++i) std::this_thread::sleep_for(100ms);
        check(connected, "connected to the fake phone 「Pixel 8 測試」");
        check(!src.pushToGallery((dir / L"nope.png").wstring(), nullptr), "missing file → false");
        const bool startedPush = src.pushToGallery(file.wstring(), [&](const pm::AndroidSource::PushResult& r) {
            res = r;
            done = true;
        });
        check(startedPush, "pushToGallery started");
        check(!src.pushToGallery(file.wstring(), nullptr), "second push while busy → false");
        for (int i = 0; i < 300 && !done; ++i) std::this_thread::sleep_for(100ms);
        called = done;
    };
    auto logText = [&] { return fileBytes(logFile); };

    {
        pm::AndroidSource::PushResult r;
        bool called = false;
        runCase(L"ok", png, r, called);
        const std::string log = logText();
        check(called && r.ok && r.inGallery && r.remotePath == "/sdcard/Pictures/ZizaiCast/ZizaiCast_20261008_101010.png",
              "picture: ok, in the gallery, " + r.remotePath);
        check(log.find("-s | 192.168.50.7:41234 | shell | mkdir -p /sdcard/Pictures/ZizaiCast") != std::string::npos,
              "mkdir -p /sdcard/Pictures/ZizaiCast");
        check(log.find("-s | 192.168.50.7:41234 | push | " + u8(png.wstring()) +
                       " | /sdcard/Pictures/ZizaiCast/ZizaiCast_20261008_101010.png") != std::string::npos,
              "adb push <Unicode local path> → Pictures/ZizaiCast (fake adb opened the file)");
        check(log.find("content call --uri content://media --method scan_file --arg "
                       "/storage/emulated/0/Pictures/ZizaiCast/ZizaiCast_20261008_101010.png") != std::string::npos,
              "MediaProvider scan_file");
        check(log.find("am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d "
                       "file:///storage/emulated/0/Pictures/ZizaiCast/ZizaiCast_20261008_101010.png") != std::string::npos,
              "MEDIA_SCANNER_SCAN_FILE broadcast");
        check(log.find("content query --uri content://media/external/images/media --projection _id --where "
                       "\"_display_name='ZizaiCast_20261008_101010.png'\"") != std::string::npos,
              "verified with content query (images)");
        check(log.find("scan_volume") == std::string::npos, "no volume rescan needed");
        const size_t pPush = log.find("| push |"), pScan = log.find("scan_file"), pQuery = log.find("content query");
        check(pPush < pScan && pScan < pQuery, "order: push → scan → query");
    }
    {
        pm::AndroidSource::PushResult r;
        bool called = false;
        runCase(L"lateindex", mp4, r, called);
        const std::string log = logText();
        check(called && r.ok && r.inGallery && r.remotePath == "/sdcard/Movies/ZizaiCast/ZizaiCast_20261008_101500.mp4",
              "video (indexed only after a volume scan): ok, in the gallery, " + r.remotePath);
        check(log.find("content://media/external/video/media") != std::string::npos &&
                  log.find("content call --uri content://media --method scan_volume --arg external_primary") != std::string::npos,
              "video table queried, scan_volume fallback used");
    }
    {
        pm::AndroidSource::PushResult r;
        bool called = false;
        runCase(L"never", png, r, called);
        check(called && r.ok && !r.inGallery, "never indexed: ok (on the phone) but inGallery false");
    }
    {
        pm::AndroidSource::PushResult r;
        bool called = false;
        runCase(L"pushfail", png, r, called);
        check(called && !r.ok && r.error.find("not found") != std::string::npos, "push fails → !ok, error \"" + r.error + "\"");
        check(logText().find("scan_file") == std::string::npos, "no scan after a failed push");
    }
}
#endif

static int serve(int argc, wchar_t** argv) {
    int seconds = 60;
    pm::share::Server::Options o;
    o.bindIp = "127.0.0.1";
    std::vector<std::wstring> files, later;
    int every = 5;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--serve" && i + 1 < argc) seconds = _wtoi(argv[++i]);
        else if (a == L"--later" && i + 1 < argc) later.push_back(argv[++i]);
        else if (a == L"--every" && i + 1 < argc) every = _wtoi(argv[++i]);
        else if (a == L"--en") o.english = true;
        else if (a == L"--lang" && i + 1 < argc) o.lang = _wtoi(argv[++i]);  // 0 zh-TW, 1 en, 2 ja, 3 ko
        else if (a == L"--live") o.live = true;
        else if (a == L"--ttl" && i + 1 < argc) o.ttlSeconds = _wtoi(argv[++i]);
        else if (a == L"--port" && i + 1 < argc) o.port = uint16_t(_wtoi(argv[++i]));
        else if (a == L"--light") {  // 奶茶-like light card
            o.palette = {0xF3E7DA, 0xFFF8F0, 0xF4E6D6, 0x3B2A20, 0x7D6858, 0xB7784A, true};
        } else files.push_back(a);
    }
    pm::share::Server srv;
    srv.log = [](const std::string& s) { std::printf("%s\n", s.c_str()); std::fflush(stdout); };
    if (!srv.start(files, o)) return 1;
    std::printf("URL %s\n", srv.url().c_str());
    std::fflush(stdout);
    // --later FILE (repeatable): added one by one, every --every S seconds (default 5).
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    for (const std::wstring& f : later) {
        std::this_thread::sleep_for(std::chrono::seconds(every));
        std::printf("added %d\n", srv.addFile(f));
        std::fflush(stdout);
    }
    std::this_thread::sleep_until(until);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    for (int i = 1; i < argc; ++i)
        if (std::wstring(argv[i]) == L"--serve") return serve(argc, argv);
    const fs::path dir = tempDir();
    testPure();
    testServer(dir);
    testLive(dir);
    testQr(dir);
#if PM_SHARE_TEST_ANDROID
    testAndroid(dir);
#endif
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
