// Offline tests for pm_android (no phone needed):
//   pm_android_test [--live-mdns SECONDS] [--no-adb] [--no-python]
// Sections: control-message bytes (scrcpy's own test vectors), stream parser
// (synthesized packets, random chunking), input translation, QR (decoded back
// with OpenCV via tools/verify_qr.py), DNS/mDNS (parser, loopback responder,
// live browse), adb child process on a private port, scrcpy session against
// a fake scrcpy-server.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "adb.h"
#include "mdns.h"
#include "pairing_qr.h"
#include "pm/android_source.h"
#include "scrcpy_proto.h"
#include "scrcpy_session.h"

using namespace pm;
using namespace std::chrono_literals;

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (c) ++g_pass;                                                          \
        else { ++g_fail; std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
    } while (0)

static void section(const char* s) { std::printf("== %s\n", s); }

static std::wstring exeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L'\\'));
}

// ------------------------------------------------------------ control --

static void testControl() {
    section("control messages (scrcpy v5.0 test vectors)");
    using namespace scrcpy;
    {
        auto v = msgKeycode(kKeyUp, 66, 5, kMetaShift);
        std::vector<uint8_t> e = {0, 1, 0, 0, 0, 0x42, 0, 0, 0, 5, 0, 0, 0, 0x41};
        CHECK(v == e);
    }
    {
        auto v = msgText("hello, world!");
        std::vector<uint8_t> e = {1, 0, 0, 0, 0x0d, 'h', 'e', 'l', 'l', 'o', ',', ' ', 'w', 'o', 'r', 'l', 'd', '!'};
        CHECK(v == e);
        auto l = msgText(std::string(400, 'a'));
        CHECK(l.size() == 5 + 300 && l[3] == 0x01 && l[4] == 0x2c);
        // 299 'a' + 3-byte char: must cut before the char (no broken UTF-8)
        auto u = msgText(std::string(299, 'a') + "\xE4\xB8\xAD");
        CHECK(u.size() == 5 + 299);
    }
    {
        auto v = msgTouch(kMotionDown, 0x1234567887654321ull, 100, 200, 1080, 1920, 1.0f, 1, 1);
        std::vector<uint8_t> e = {2, 0, 0x12, 0x34, 0x56, 0x78, 0x87, 0x65, 0x43, 0x21, 0, 0, 0, 0x64, 0, 0, 0, 0xc8,
                                  0x04, 0x38, 0x07, 0x80, 0xff, 0xff, 0, 0, 0, 1, 0, 0, 0, 1};
        CHECK(v == e);
    }
    {
        auto v = msgScroll(260, 1026, 1080, 1920, 16, -16, 1);
        std::vector<uint8_t> e = {3, 0, 0, 1, 4, 0, 0, 4, 2, 0x04, 0x38, 0x07, 0x80, 0x7f, 0xff, 0x80, 0x00, 0, 0, 0, 1};
        CHECK(v == e);
    }
    CHECK((msgBackOrScreenOn(kKeyUp) == std::vector<uint8_t>{4, 1}));
    CHECK((msgSimple(MsgType::ExpandNotificationPanel) == std::vector<uint8_t>{5}));
}

// ------------------------------------------------------------- parser --

struct MockVideo : VideoSink {
    std::vector<VideoCodec> codecs;
    std::vector<std::pair<int, int>> sizes;
    std::vector<std::vector<uint8_t>> frames;
    int resets = 0;
    std::mutex mu;
    void onCodec(VideoCodec c) override { std::lock_guard<std::mutex> l(mu); codecs.push_back(c); }
    void onFrame(const uint8_t* p, size_t n, uint64_t) override {
        std::lock_guard<std::mutex> l(mu);
        frames.emplace_back(p, p + n);
    }
    void onSourceSize(int w, int h) override { std::lock_guard<std::mutex> l(mu); sizes.push_back({w, h}); }
    void onReset() override { ++resets; }
};
struct MockAudio : AudioSink {
    int formats = 0, lastRate = 0, lastCh = 0, lastSpf = 0;
    AudioCodec lastCodec{};
    std::vector<std::vector<uint8_t>> packets;
    int flushes = 0;
    std::mutex mu;
    void onFormat(AudioCodec c, int r, int ch, int spf) override {
        std::lock_guard<std::mutex> l(mu);
        ++formats; lastCodec = c; lastRate = r; lastCh = ch; lastSpf = spf;
    }
    void onPacket(const uint8_t* p, size_t n, uint64_t) override {
        std::lock_guard<std::mutex> l(mu);
        packets.emplace_back(p, p + n);
    }
    void onVolume(float) override {}
    void onFlush() override { ++flushes; }
};

static void be32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back(uint8_t(x >> (i * 8)));
}
static void be64(std::vector<uint8_t>& v, uint64_t x) { be32(v, uint32_t(x >> 32)); be32(v, uint32_t(x)); }
static void session(std::vector<uint8_t>& v, uint32_t w, uint32_t h) {
    be32(v, 0x80000000u);
    be32(v, w);
    be32(v, h);
}
static void packet(std::vector<uint8_t>& v, uint64_t ptsFlags, const std::vector<uint8_t>& data) {
    be64(v, ptsFlags);
    be32(v, uint32_t(data.size()));
    v.insert(v.end(), data.begin(), data.end());
}

static const std::vector<uint8_t> kSpsPps = {0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x1f, 0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80};
static const std::vector<uint8_t> kIdr = {0, 0, 0, 1, 0x65, 0x88, 0x84, 0x00, 0x33};
static const std::vector<uint8_t> kP = {0, 0, 0, 1, 0x41, 0x9a, 0x02};
static const std::vector<uint8_t> kSpsPps2 = {0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x28, 0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x81};

static std::vector<uint8_t> videoStream() {
    using namespace scrcpy;
    std::vector<uint8_t> s;
    be32(s, kCodecH264);
    session(s, 1080, 2340);
    packet(s, kFlagConfig, kSpsPps);
    packet(s, kFlagKeyFrame | 0, kIdr);
    packet(s, 16666, kP);
    session(s, 2340, 1080);  // rotation: new session + new config
    packet(s, kFlagConfig, kSpsPps2);
    packet(s, kFlagKeyFrame | 50000, kIdr);
    return s;
}
static std::vector<uint8_t> audioStream() {
    using namespace scrcpy;
    std::vector<uint8_t> s;
    be32(s, kCodecAac);
    packet(s, kFlagConfig, {0x11, 0x90});
    for (int i = 0; i < 3; ++i) packet(s, uint64_t(i) * 21333, std::vector<uint8_t>(size_t(100 + i), uint8_t(0x21 + i)));
    return s;
}

static void checkVideo(MockVideo& mv) {
    CHECK(mv.codecs.size() == 1 && mv.codecs[0] == VideoCodec::H264);
    CHECK((mv.sizes == std::vector<std::pair<int, int>>{{1080, 2340}, {2340, 1080}}));
    CHECK(mv.frames.size() == 3);
    if (mv.frames.size() == 3) {
        std::vector<uint8_t> f0 = kSpsPps;
        f0.insert(f0.end(), kIdr.begin(), kIdr.end());
        std::vector<uint8_t> f2 = kSpsPps2;
        f2.insert(f2.end(), kIdr.begin(), kIdr.end());
        CHECK(mv.frames[0] == f0);
        CHECK(mv.frames[1] == kP);
        CHECK(mv.frames[2] == f2);
    }
}
static void checkAudio(MockAudio& ma) {
    CHECK(ma.formats == 1 && ma.lastCodec == AudioCodec::AAC_LC && ma.lastRate == 48000 && ma.lastCh == 2 &&
          ma.lastSpf == 1024);
    CHECK(ma.packets.size() == 3 && ma.packets[0].size() == 100 && ma.packets[2][0] == 0x23);
}

static void testParser() {
    section("stream parser (synthesized scrcpy v5 streams, random chunking)");
    using namespace scrcpy;
    auto vs = videoStream(), as = audioStream();
    std::mt19937 rng(1234);
    for (int round = 0; round < 50; ++round) {
        MockVideo mv;
        VideoAdapter va(&mv);
        StreamParser p(va.callbacks());
        size_t maxChunk = round == 0 ? 1 : (round == 1 ? vs.size() : 1 + rng() % 40);
        bool ok = true;
        for (size_t i = 0; i < vs.size();) {
            size_t n = std::min(vs.size() - i, round == 0 ? 1 : 1 + size_t(rng() % maxChunk));
            ok = ok && p.feed(&vs[i], n);
            i += n;
        }
        if (round < 3 || round == 49) {
            CHECK(ok);
            checkVideo(mv);
            CHECK(va.width() == 2340 && va.height() == 1080);
        } else if (!ok || mv.frames.size() != 3) {
            CHECK(false);
        }
        MockAudio ma;
        AudioAdapter aa(&ma);
        StreamParser pa(aa.callbacks());
        for (size_t i = 0; i < as.size();) {
            size_t n = std::min(as.size() - i, 1 + size_t(rng() % 7));
            pa.feed(&as[i], n);
            i += n;
        }
        if (round < 3) checkAudio(ma);
    }
    {  // audio disabled by the device (Android < 11): no format, no failure
        MockAudio ma;
        AudioAdapter aa(&ma);
        StreamParser p(aa.callbacks());
        std::vector<uint8_t> s = {0, 0, 0, 0, 1, 2, 3};
        CHECK(p.feed(s.data(), s.size()));
        CHECK(p.disabled() && ma.formats == 0 && !aa.active());
    }
    {  // configuration error code
        StreamParser p({});
        std::vector<uint8_t> s = {0, 0, 0, 1};
        CHECK(!p.feed(s.data(), s.size()) && p.failed());
    }
    {  // zero-length packet is a protocol error
        StreamParser p({});
        std::vector<uint8_t> s;
        be32(s, kCodecH264);
        session(s, 100, 100);
        be64(s, 0);
        be32(s, 0);
        CHECK(!p.feed(s.data(), s.size()));
    }
    {  // PTS / flags
        uint64_t gotPts = 0;
        bool gotKey = false, gotCfg = true;
        StreamParser::Callbacks cb;
        cb.onPacket = [&](const uint8_t*, size_t, uint64_t pts, bool c, bool k) { gotPts = pts, gotKey = k, gotCfg = c; };
        StreamParser p(cb);
        std::vector<uint8_t> s;
        be32(s, kCodecAac);
        packet(s, kFlagKeyFrame | 123456789ull, {1});
        p.feed(s.data(), s.size());
        CHECK(gotPts == 123456789ull && gotKey && !gotCfg);
    }
}

// --------------------------------------------------------------- input --

static void testInput() {
    section("input translation");
    using namespace scrcpy;
    using K = VideoWindow::PointerEvent::Kind;
    std::vector<std::vector<uint8_t>> out;
    InputTranslator t([&](std::vector<uint8_t> m) { out.push_back(std::move(m)); });
    VideoWindow::PointerEvent e;
    e.kind = K::Down; e.x = 0.5f; e.y = 0.5f;
    t.pointer(e);
    CHECK(out.empty());  // no video size yet
    t.setVideoSize(1080, 2340);
    t.pointer(e);
    CHECK(out.size() == 1 && out[0] == msgTouch(kMotionDown, kPointerGenericFinger, 540, 1170, 1080, 2340, 1.0f, 0, 0));
    e.kind = K::Move; e.x = 0.6f;
    t.pointer(e);
    CHECK(out.size() == 2 && out[1] == msgTouch(kMotionMove, kPointerGenericFinger, 648, 1170, 1080, 2340, 1.0f, 0, 0));
    e.kind = K::Up; e.x = 1.0f; e.y = 1.0f;
    t.pointer(e);
    CHECK(out.size() == 3 && out[2] == msgTouch(kMotionUp, kPointerGenericFinger, 1079, 2339, 1080, 2340, 0.0f, 0, 0));
    e.kind = K::Move;
    t.pointer(e);
    CHECK(out.size() == 3);  // hover: nothing
    out.clear();
    e.kind = K::Down; e.button = 1; t.pointer(e);
    e.kind = K::Up; t.pointer(e);
    CHECK(out.size() == 2 && out[0] == msgBackOrScreenOn(kKeyDown) && out[1] == msgBackOrScreenOn(kKeyUp));
    out.clear();
    e.kind = K::Down; e.button = 2; t.pointer(e);
    e.kind = K::Up; t.pointer(e);
    CHECK(out.size() == 2 && out[0] == msgKeycode(kKeyDown, kKeyHome, 0, 0) && out[1] == msgKeycode(kKeyUp, kKeyHome, 0, 0));
    out.clear();
    e = {}; e.kind = K::Wheel; e.x = 0.25f; e.y = 0.5f; e.wheelY = -1.0f;
    t.pointer(e);
    CHECK(out.size() == 1 && out[0] == msgScroll(270, 1170, 1080, 2340, 0, -1, 0));

    out.clear();
    t.key('A', true, L'a');
    t.key('A', false, 0);
    CHECK(out.size() == 1 && out[0] == msgText("a"));
    out.clear();
    t.key(0x11, true, 0);          // Ctrl down
    t.key('C', true, 0x03);        // Ctrl+C
    t.key('C', false, 0);
    t.key(0x11, false, 0);
    CHECK(out.size() == 2 && out[0] == msgKeycode(kKeyDown, 31, 0, kMetaCtrl) && out[1] == msgKeycode(kKeyUp, 31, 0, kMetaCtrl));
    out.clear();
    t.key(0x0D, true, L'\r');
    t.key(0x0D, false, 0);
    CHECK(out.size() == 2 && out[0] == msgKeycode(kKeyDown, 66, 0, 0) && out[1] == msgKeycode(kKeyUp, 66, 0, 0));
    out.clear();
    t.key(0x08, true, 8);
    t.key(0x08, true, 8);  // auto-repeat
    t.key(0x08, false, 0);
    CHECK(out.size() == 3 && out[1] == msgKeycode(kKeyDown, 67, 1, 0));
    out.clear();
    t.key(0xE5, true, L'中');  // IME commit
    CHECK(out.size() == 1 && out[0] == msgText("\xE4\xB8\xAD"));
    out.clear();
    t.key(0, true, 0xD83D);  // 😀 as a surrogate pair
    t.key(0, true, 0xDE00);
    CHECK(out.size() == 1 && out[0] == msgText("\xF0\x9F\x98\x80"));
    out.clear();
    t.key(0x10, true, 0);
    t.key('B', true, L'B');  // shifted letter: text
    t.key(0x10, false, 0);
    CHECK(out.size() == 1 && out[0] == msgText("B"));
    CHECK(vkToAndroidKeycode(0x25) == 21 && vkToAndroidKeycode(0x2E) == 112 && vkToAndroidKeycode(0x70) == 131);
}

// ------------------------------------------------------------------ QR --

static bool writeBmp(const std::wstring& path, const std::vector<uint8_t>& bgra, int size) {
    int rowBytes = (size * 3 + 3) & ~3;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + DWORD(rowBytes * size);
    ih.biSize = sizeof(ih);
    ih.biWidth = size;
    ih.biHeight = size;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<char*>(&fh), sizeof(fh));
    f.write(reinterpret_cast<char*>(&ih), sizeof(ih));
    std::vector<uint8_t> row(size_t(rowBytes), 0);
    for (int y = size - 1; y >= 0; --y) {
        for (int x = 0; x < size; ++x)
            memcpy(&row[size_t(x) * 3], &bgra[(size_t(y) * size_t(size) + size_t(x)) * 4], 3);
        f.write(reinterpret_cast<char*>(row.data()), rowBytes);
    }
    return bool(f);
}

static std::string runCapture(const std::wstring& cmdLine, int* exitCode) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd, wr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring c = cmdLine;
    std::string out;
    *exitCode = -1;
    if (CreateProcessW(nullptr, c.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(wr);
        char buf[4096];
        DWORD n;
        while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, n);
        WaitForSingleObject(pi.hProcess, 30000);
        DWORD code;
        GetExitCodeProcess(pi.hProcess, &code);
        *exitCode = int(code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        CloseHandle(wr);
    }
    CloseHandle(rd);
    return out;
}

static void testQr(bool python, const std::wstring& verifyScript) {
    section("pairing QR");
    for (int i = 0; i < 20; ++i) {
        auto c = pairing::randomCredentials();
        std::string t = c.qrText();
        CHECK(t.rfind("WIFI:T:ADB;S:zizai-", 0) == 0 && t.size() > 4 && t.substr(t.size() - 2) == ";;");
        CHECK(c.service.size() == 16 && c.password.size() == 12);
        CHECK(c.password.find_first_of(";:,\\\"") == std::string::npos);
    }
    auto c1 = pairing::randomCredentials(), c2 = pairing::randomCredentials();
    CHECK(c1.password != c2.password && c1.service != c2.service);

    std::vector<std::vector<bool>> m;
    CHECK(pairing::qrModules(c1.qrText(), m));
    int n = int(m.size());
    CHECK(n >= 21 && (n - 17) % 4 == 0);
    // finder pattern corners: dark 7x7 ring with a dark 3x3 centre
    auto finder = [&](int ox, int oy) {
        for (int y = 0; y < 7; ++y)
            for (int x = 0; x < 7; ++x) {
                bool ring = x == 0 || y == 0 || x == 6 || y == 6;
                bool core = x >= 2 && x <= 4 && y >= 2 && y <= 4;
                if (m[size_t(oy + y)][size_t(ox + x)] != (ring || core)) return false;
            }
        return true;
    };
    CHECK(finder(0, 0) && finder(n - 7, 0) && finder(0, n - 7));

    std::vector<uint8_t> bgra;
    int size = 0;
    CHECK(pairing::renderQr(c1.qrText(), 8, 4, bgra, size));
    CHECK(size == (n + 8) * 8 && bgra.size() == size_t(size) * size * 4);
    CHECK(bgra[0] == 255 && bgra[3] == 255);  // quiet zone white, opaque
    size_t firstDark = (size_t(32) * size + 32) * 4;
    CHECK(bgra[firstDark] == 0 && bgra[firstDark + 3] == 255);
    std::printf("  QR %dx%d modules, image %dx%d: %s\n", n, n, size, size, c1.qrText().c_str());

    if (python) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring bmp = std::wstring(tmp) + L"pm_android_qr.bmp";
        CHECK(writeBmp(bmp, bgra, size));
        int code = -1;
        std::string decoded = runCapture(L"py \"" + verifyScript + L"\" \"" + bmp + L"\"", &code);
        std::printf("  OpenCV decoded: %s (exit %d)\n", decoded.c_str(), code);
        CHECK(code == 0 && decoded == c1.qrText());
    }
}

// ---------------------------------------------------------------- mDNS --

static void testDns() {
    section("DNS message build/parse");
    using namespace mdns;
    auto q = buildQuery(7, {{"_adb-tls-pairing._tcp.local", kTypePtr, true}});
    Message m;
    CHECK(parseMessage(q.data(), q.size(), m) && !m.response && m.id == 7 && m.questions.size() == 1 &&
          m.questions[0].name == "_adb-tls-pairing._tcp.local" && m.questions[0].unicastResponse);
    auto r = buildServiceResponse(9, "zizai-AbC", "_adb-tls-pairing._tcp", "Android-3.local", 0xC0A8014D, 37099);
    CHECK(parseMessage(r.data(), r.size(), m) && m.response && m.records.size() == 3);
    if (m.records.size() == 3) {
        CHECK(m.records[0].type == kTypePtr && m.records[0].target == "zizai-AbC._adb-tls-pairing._tcp.local");
        CHECK(m.records[1].type == kTypeSrv && m.records[1].port == 37099 && m.records[1].target == "Android-3.local");
        CHECK(m.records[2].type == kTypeA && m.records[2].ipv4 == 0xC0A8014D);
    }
    // Hand-made packet with name compression (as Android's responder sends).
    std::vector<uint8_t> c = {0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 1};
    size_t typeOff = c.size();
    for (auto lbl : {"_adb-tls-connect", "_tcp", "local"}) {
        c.push_back(uint8_t(strlen(lbl)));
        c.insert(c.end(), lbl, lbl + strlen(lbl));
    }
    c.push_back(0);
    c.insert(c.end(), {0, 12, 0, 1, 0, 0, 0, 120});
    std::vector<uint8_t> rd = {11};
    for (char ch : std::string("adb-ABC-xyz")) rd.push_back(uint8_t(ch));
    rd.push_back(0xC0);
    rd.push_back(uint8_t(typeOff));
    c.push_back(0);
    c.push_back(uint8_t(rd.size()));
    size_t instOff = c.size();
    c.insert(c.end(), rd.begin(), rd.end());
    // additional: SRV with compressed owner name
    c.insert(c.end(), {0xC0, uint8_t(instOff), 0, 33, 0x80, 1, 0, 0, 0, 120, 0, 8, 0, 0, 0, 0, 0xA4, 0x23, 0xC0, 0x0C});
    // (target points at offset 12 = the type name; only structure matters here)
    CHECK(parseMessage(c.data(), c.size(), m) && m.records.size() == 2);
    if (m.records.size() == 2) {
        CHECK(m.records[0].target == "adb-ABC-xyz._adb-tls-connect._tcp.local");
        CHECK(m.records[1].name == "adb-ABC-xyz._adb-tls-connect._tcp.local" && m.records[1].port == 0xA423);
    }
    // Truncated / looping pointers must fail cleanly.
    std::vector<uint8_t> bad = {0, 0, 0x84, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 12};
    CHECK(!parseMessage(bad.data(), bad.size(), m));
    CHECK(!parseMessage(c.data(), 20, m));
}

static void testMdnsLoopback() {
    section("mDNS browse loop vs loopback responder");
    using namespace mdns;
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    int len = sizeof(a);
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    uint16_t port = ntohs(a.sin_port);
    std::atomic<bool> stop{false};
    std::atomic<int> queries{0};
    std::thread responder([&] {
        DWORD to = 100;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&to), sizeof(to));
        while (!stop) {
            uint8_t buf[2048];
            sockaddr_in from{};
            int fl = sizeof(from);
            int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
            if (n <= 0) continue;
            Message m;
            if (!parseMessage(buf, size_t(n), m) || m.response) continue;
            ++queries;
            bool asksPairing = false;
            for (auto& q : m.questions) asksPairing |= q.name == "_adb-tls-pairing._tcp.local";
            if (!asksPairing) continue;
            // Legacy unicast reply: same id, question echoed.
            auto r = buildServiceResponse(m.id, "zizai-LOOPBACK", "_adb-tls-pairing._tcp", "Android-7.local",
                                          0xC0A80142, 41234, m.questions);
            sendto(s, reinterpret_cast<char*>(r.data()), int(r.size()), 0, reinterpret_cast<sockaddr*>(&from), fl);
        }
    });
    Browser br;
    std::atomic<int> found{0};
    br.onFound = [&](const adb::MdnsService&) { ++found; };
    br.start({"_adb-tls-pairing._tcp", "_adb-tls-connect._tcp"}, {"127.0.0.1:" + std::to_string(port)});
    auto t0 = std::chrono::steady_clock::now();
    std::vector<adb::MdnsService> sv;
    while (std::chrono::steady_clock::now() - t0 < 3s) {
        sv = br.services();
        if (!sv.empty()) break;
        std::this_thread::sleep_for(20ms);
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    br.stop();
    stop = true;
    responder.join();
    closesocket(s);
    CHECK(sv.size() == 1);
    if (!sv.empty()) {
        std::printf("  resolved %s %s %s in %lld ms (%d queries seen)\n", sv[0].instance.c_str(), sv[0].type.c_str(),
                    sv[0].hostPort().c_str(), (long long)ms, queries.load());
        CHECK(sv[0].instance == "zizai-LOOPBACK" && sv[0].type == "_adb-tls-pairing._tcp" &&
              sv[0].hostPort() == "192.168.1.66:41234");
    }
    CHECK(found == 1);
}

static void testMdnsLive(int seconds) {
    section("mDNS live browse on the LAN (informational)");
    mdns::Browser br;
    br.log = [](const std::string& s) { std::printf("  %s\n", s.c_str()); };
    br.start({"_adb-tls-pairing._tcp", "_adb-tls-connect._tcp", "_airplay._tcp", "_googlecast._tcp"});
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    auto sv = br.services();
    br.stop();
    std::printf("  %zu service(s) resolved in %d s\n", sv.size(), seconds);
    for (auto& s : sv) std::printf("    %s | %s | %s\n", s.instance.c_str(), s.type.c_str(), s.hostPort().c_str());
}

// ----------------------------------------------------------------- adb --

static void testAdbParsers() {
    section("adb output parsers");
    auto sv = adb::parseMdnsServices(
        "List of discovered mdns services\r\n"
        "adb-R5CT10ABCDE-Xy1Zab\t_adb-tls-connect._tcp\t192.168.1.23:41827\r\n"
        "zizai-Ab3dE6fGh9\t_adb-tls-pairing._tcp.\t192.168.1.23:37011\r\n\r\n");
    CHECK(sv.size() == 2);
    if (sv.size() == 2) {
        CHECK(sv[0].instance == "adb-R5CT10ABCDE-Xy1Zab" && sv[0].type == "_adb-tls-connect._tcp" &&
              sv[0].host == "192.168.1.23" && sv[0].port == 41827);
        CHECK(sv[1].instance == "zizai-Ab3dE6fGh9" && sv[1].type == "_adb-tls-pairing._tcp" && sv[1].port == 37011);
    }
    auto dv = adb::parseDevices(
        "List of devices attached\r\n192.168.1.23:41827     device product:e3qxxx model:SM_S928B device:e3q "
        "transport_id:1\r\nemulator-5554 offline\r\n");
    CHECK(dv.size() == 2 && dv[0].serial == "192.168.1.23:41827" && dv[0].state == "device" && dv[0].model == "SM_S928B");
    std::string guid;
    CHECK(adb::parsePairResult("Successfully paired to 192.168.1.23:37011 [guid=adb-R5CT10ABCDE-Xy1Zab]\n", &guid) &&
          guid == "adb-R5CT10ABCDE-Xy1Zab");
    CHECK(!adb::parsePairResult("Failed: Unable to start pairing client.\n", &guid));
    CHECK(adb::parseConnectResult("connected to 192.168.1.23:41827\n"));
    CHECK(adb::parseConnectResult("already connected to 192.168.1.23:41827\n"));
    CHECK(!adb::parseConnectResult("failed to connect to '192.168.1.23:41827': Connection refused\n"));
    CHECK(!adb::parseConnectResult("cannot connect to 127.0.0.1:1: (10061)\n"));
    std::string h;
    uint16_t p;
    CHECK(adb::splitHostPort("192.168.0.5:37123", h, p) && h == "192.168.0.5" && p == 37123);
    CHECK(!adb::splitHostPort("192.168.0.5", h, p) && !adb::splitHostPort("1.2.3.4:99999", h, p));
}

static bool portListening(uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    bool ok = connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
    closesocket(s);
    return ok;
}

static void testAdbProcess(const std::wstring& toolsDir) {
    section("adb child process on a private port");
    const uint16_t kPort = 15099;
    adb::Adb a;
    if (!a.init(toolsDir + L"\\adb.exe", kPort)) {
        std::printf("  SKIP: no adb.exe in %ls (run third_party/fetch_tools.ps1)\n", toolsDir.c_str());
        return;
    }
    bool default5037Before = portListening(5037);
    auto v = a.run({"version"}, 10000);
    CHECK(v.ok() && v.output.find("Android Debug Bridge") != std::string::npos);
    std::printf("  %s", v.output.substr(0, v.output.find('\n') + 1).c_str());
    auto t0 = std::chrono::steady_clock::now();
    CHECK(a.startServer());
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(portListening(kPort));
    CHECK(portListening(5037) == default5037Before);  // never touched the default port
    std::printf("  adb server up on :%u in %lld ms (5037 listening: %d, unchanged)\n", kPort, (long long)ms,
                int(default5037Before));
    auto d = a.run({"devices", "-l"}, 10000);
    CHECK(d.ok() && d.output.find("List of devices") != std::string::npos);
    auto mc = a.run({"mdns", "check"}, 10000);
    std::printf("  mdns check: %s", mc.output.c_str());
    CHECK(mc.ok());
    auto ms2 = a.run({"mdns", "services"}, 10000);
    CHECK(ms2.ok());
    // Timeout path: a command that never returns is killed.
    auto t1 = std::chrono::steady_clock::now();
    auto w = a.run({"wait-for-device"}, 800);
    auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
    CHECK(w.exitCode == -2 && waited < 3000);
    // Cancel path.
    std::atomic<bool> cancel{false};
    std::thread ct([&] { std::this_thread::sleep_for(300ms); cancel = true; });
    auto c = a.run({"wait-for-device"}, 20000, &cancel);
    ct.join();
    CHECK(c.exitCode == -2);
    // spawn + kill
    std::atomic<int> lines{0};
    auto p = a.spawn({"track-devices"}, [&](const std::string&) { ++lines; });
    CHECK(p && p->running());
    if (p) {
        p->kill();
        CHECK(p->wait(3000));
    }
    a.killServer();
    std::this_thread::sleep_for(300ms);
    CHECK(!portListening(kPort));
    std::printf("  kill-server: port %u closed\n", kPort);
}

// ---------------------------------------------------- fake scrcpy-server --

static void testSession() {
    section("scrcpy session vs fake scrcpy-server");
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    listen(ls, 8);
    int len = sizeof(a);
    getsockname(ls, reinterpret_cast<sockaddr*>(&a), &len);
    uint16_t port = ntohs(a.sin_port);

    std::vector<uint8_t> controlBytes;
    std::mutex cmu;
    std::atomic<bool> closeVideo{false};
    std::thread server([&] {
        // Like adb forward before the server listens: accept + close at once.
        SOCKET early = accept(ls, nullptr, nullptr);
        closesocket(early);
        SOCKET v = accept(ls, nullptr, nullptr);
        uint8_t dummy = 0;
        send(v, reinterpret_cast<char*>(&dummy), 1, 0);
        SOCKET au = accept(ls, nullptr, nullptr);
        SOCKET c = accept(ls, nullptr, nullptr);
        char name[64] = {};
        strcpy(name, "Pixel 9 \xE6\xB8\xAC\xE8\xA9\xA6");  // "Pixel 9 測試"
        send(v, name, 64, 0);
        auto vs = videoStream(), as = audioStream();
        for (size_t i = 0; i < vs.size(); i += 5) send(v, reinterpret_cast<char*>(&vs[i]), int(std::min<size_t>(5, vs.size() - i)), 0);
        send(au, reinterpret_cast<char*>(as.data()), int(as.size()), 0);
        DWORD to = 100;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&to), sizeof(to));
        while (!closeVideo) {
            char b[256];
            int n = recv(c, b, sizeof(b), 0);
            if (n > 0) {
                std::lock_guard<std::mutex> l(cmu);
                controlBytes.insert(controlBytes.end(), b, b + n);
            } else if (n == 0) break;
        }
        closesocket(v);  // device gone
        std::this_thread::sleep_for(200ms);
        closesocket(au);
        closesocket(c);
    });

    MockVideo mv;
    MockAudio ma;
    scrcpy::Session s;
    std::atomic<int> ended{0};
    std::atomic<int> sizeW{0};
    s.onEnded = [&] { ++ended; };
    s.onVideoSize = [&](int w, int) { sizeW = w; };
    scrcpy::Session::Config cfg;
    cfg.port = port;
    cfg.delayMs = 20;
    std::string err;
    bool ok = s.connect(cfg, nullptr, &err);
    CHECK(ok);
    std::printf("  connected (%s), device name \"%s\"\n", ok ? "ok" : err.c_str(), s.deviceName().c_str());
    CHECK(s.deviceName() == "Pixel 9 \xE6\xB8\xAC\xE8\xA9\xA6");
    s.run(&mv, &ma);
    scrcpy::InputTranslator t([&](std::vector<uint8_t> m) { s.sendControl(std::move(m)); });
    for (int i = 0; i < 100 && sizeW != 2340; ++i) std::this_thread::sleep_for(10ms);
    t.setVideoSize(2340, 1080);
    VideoWindow::PointerEvent e;
    e.kind = VideoWindow::PointerEvent::Kind::Down;
    e.x = 0.5f; e.y = 0.5f;
    t.pointer(e);
    e.kind = VideoWindow::PointerEvent::Kind::Up;
    t.pointer(e);
    t.key('X', true, L'x');
    std::vector<uint8_t> expect;
    for (auto& m : {scrcpy::msgTouch(scrcpy::kMotionDown, scrcpy::kPointerGenericFinger, 1170, 540, 2340, 1080, 1, 0, 0),
                    scrcpy::msgTouch(scrcpy::kMotionUp, scrcpy::kPointerGenericFinger, 1170, 540, 2340, 1080, 0, 0, 0),
                    scrcpy::msgText("x")})
        expect.insert(expect.end(), m.begin(), m.end());
    for (int i = 0; i < 200; ++i) {
        {
            std::lock_guard<std::mutex> l(cmu);
            if (controlBytes.size() >= expect.size()) break;
        }
        std::this_thread::sleep_for(10ms);
    }
    {
        std::lock_guard<std::mutex> l(cmu);
        CHECK(controlBytes == expect);
    }
    {
        std::lock_guard<std::mutex> l(mv.mu);
        checkVideo(mv);
    }
    {
        std::lock_guard<std::mutex> l(ma.mu);
        checkAudio(ma);
    }
    closeVideo = true;
    for (int i = 0; i < 200 && !ended; ++i) std::this_thread::sleep_for(10ms);
    CHECK(ended == 1);
    s.stop();
    server.join();
    closesocket(ls);
    std::printf("  video %llu pkts, audio %llu pkts, control %llu msgs, ended=%d\n",
                (unsigned long long)s.videoPackets.load(), (unsigned long long)s.audioPackets.load(),
                (unsigned long long)s.controlSent.load(), ended.load());
}

static void testSourceApi(const std::wstring& toolsDir) {
    section("AndroidSource API (no phone)");
    SetEnvironmentVariableW(L"PM_ADB_PORT", L"15098");  // never the app's 15037
    AndroidSource src;
    std::vector<std::string> logs;
    std::mutex lmu;
    src.log = [&](const std::string& s) { std::lock_guard<std::mutex> l(lmu); logs.push_back(s); };
    std::vector<AndroidSource::State> states;
    src.events.onState = [&](AndroidSource::State s, const std::wstring&) { std::lock_guard<std::mutex> l(lmu); states.push_back(s); };
    CHECK(!src.init(L"C:\\nonexistent"));
    if (!src.init(toolsDir)) {
        std::printf("  SKIP: tools missing\n");
        return;
    }
    std::vector<uint8_t> bgra;
    int size = 0;
    std::wstring text;
    CHECK(src.beginQrPairing(bgra, size, text));
    CHECK(size > 0 && bgra.size() == size_t(size) * size * 4 && text.rfind(L"WIFI:T:ADB;S:", 0) == 0);
    for (int i = 0; i < 100 && src.state() != AndroidSource::State::WaitingForPairing; ++i) std::this_thread::sleep_for(50ms);
    CHECK(src.state() == AndroidSource::State::WaitingForPairing);
    CHECK(!src.pairWithCode(L"bad", L"123456"));
    CHECK(!src.pairWithCode(L"192.168.1.2:4000", L"12a456"));
    CHECK(!src.start(nullptr, nullptr));  // nothing connected
    src.cancelPairing();
    for (int i = 0; i < 100 && src.state() == AndroidSource::State::WaitingForPairing; ++i) std::this_thread::sleep_for(50ms);
    CHECK(src.state() == AndroidSource::State::Idle);
    src.sendKey('A', true, L'a');  // ignored, no crash
    src.pressHome();
    std::printf("  states:");
    for (auto s : states) std::printf(" %d", int(s));
    std::printf("\n");
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    int liveSeconds = 0;
    bool doAdb = true, python = true;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--live-mdns" && i + 1 < argc) liveSeconds = atoi(argv[++i]);
        else if (a == "--no-adb") doAdb = false;
        else if (a == "--no-python") python = false;
    }
    std::wstring dir = exeDir();
    std::wstring tools = dir + L"\\android-tools";
    // Source tree location of verify_qr.py (exe is in <build>/bin/<cfg>).
    std::wstring verify = PM_ANDROID_TOOLS_DIR L"/verify_qr.py";

    testControl();
    testParser();
    testInput();
    testQr(python, verify);
    testDns();
    testMdnsLoopback();
    testAdbParsers();
    testSession();
    if (doAdb) {
        testAdbProcess(tools);
        testSourceApi(tools);
        std::this_thread::sleep_for(300ms);
        CHECK(!portListening(15098));  // ~AndroidSource killed its private adb server
    }
    if (liveSeconds > 0) testMdnsLive(liveSeconds);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
