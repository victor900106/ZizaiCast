// Offline tests for pm_android (no phone needed):
//   pm_android_test [--live-mdns SECONDS] [--no-adb] [--no-python] [--no-mdns]
//                   [--no-window] [--only-083] [--measure-idle SECONDS]
//   (--no-adb --no-mdns: loopback sockets only, so no firewall prompt for a
//   fresh build directory)
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

#include <algorithm>
#include <memory>
#include <mutex>

#include "adb.h"
#include "android_internal.h"
#include "backoff.h"
#include "clipboard.h"
#include "mdns.h"
#include "pairing_qr.h"
#include "pm/android_source.h"
#include "scrcpy_proto.h"
#include "scrcpy_session.h"
#ifdef PM_ANDROID_TEST_VIDEO
#include "pm/video_window.h"
#endif

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
    t.setModifierSource(nullptr);  // only the events below (not this thread's keyboard)
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
    // VideoWindow never sends Ctrl itself: the held state comes from the
    // modifier source (GetKeyState in the app) when the key arrives.
    uint32_t held = 0;
    t.setModifierSource([&] { return held; });
    out.clear();
    held = kMetaCtrl;
    t.key('V', true, 0);
    t.key('V', false, 0);
    held = kMetaCtrl | kMetaShift;
    t.key('Z', true, 0);  // Ctrl+Shift+Z (redo)
    t.key('Z', false, 0);
    held = 0;
    t.key('V', true, L'v');  // released: text again
    CHECK(out.size() == 5 && out[0] == msgKeycode(kKeyDown, 50, 0, kMetaCtrl) && out[1] == msgKeycode(kKeyUp, 50, 0, kMetaCtrl) &&
          out[2] == msgKeycode(kKeyDown, 54, 0, kMetaCtrl | kMetaShift) && out[4] == msgText("v"));
    CHECK(vkToAndroidKeycode(0x25) == 21 && vkToAndroidKeycode(0x2E) == 112 && vkToAndroidKeycode(0x70) == 131);
}

#ifdef PM_ANDROID_TEST_VIDEO
// End to end: real VideoWindow key handling -> InputTranslator (the app's
// path: setKeyHandler -> AndroidSource::sendKey -> translator.key on the UI
// thread).  Ctrl is held in this thread's key state (what GetKeyState reads)
// and never sent as a key event, exactly like real input.  The window is
// created off the desktop and never activated (PM_VIDEO_OFFSCREEN).
static void testKeysThroughWindow() {
    section("keys: VideoWindow -> InputTranslator");
    using namespace scrcpy;
    SetEnvironmentVariableW(L"PM_VIDEO_OFFSCREEN", L"1");
    VideoWindow win;
    if (!win.create(L"pm_android_test keys", 320, 240)) {
        CHECK(!"VideoWindow::create");
        return;
    }
    const HWND hwnd = win.hwnd();
    std::vector<std::vector<uint8_t>> out;
    InputTranslator t([&](std::vector<uint8_t> m) { out.push_back(std::move(m)); });  // default: GetKeyState
    win.setKeyHandler([&](unsigned vk, bool down, wchar_t ch) { t.key(vk, down, ch); });
    auto pump = [] {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    };
    BYTE saved[256]{};
    GetKeyboardState(saved);
    // Set explicitly, whatever the real keyboard does (incl. Caps Lock).
    auto hold = [&](bool ctrl, bool shift, bool caps = false) {
        BYTE ks[256];
        std::memcpy(ks, saved, sizeof(ks));
        ks[VK_CONTROL] = ks[VK_LCONTROL] = ctrl ? 0x80 : 0;
        ks[VK_SHIFT] = ks[VK_LSHIFT] = shift ? 0x80 : 0;
        ks[VK_MENU] = ks[VK_LMENU] = ks[VK_RMENU] = ks[VK_RCONTROL] = ks[VK_RSHIFT] = 0;
        ks[VK_CAPITAL] = caps ? 0x01 : 0;  // toggled
        SetKeyboardState(ks);
    };
    // One message at a time, like real input (TranslateMessage's WM_CHAR is next in the queue).
    auto press = [&](WPARAM vk) {
        const LPARAM scan = static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC)) << 16;
        PostMessageW(hwnd, WM_KEYDOWN, vk, 1 | scan);
        pump();
        PostMessageW(hwnd, WM_KEYUP, vk, 1 | scan | (3LL << 30));
        pump();
    };
    pump();
    hold(false, false);
    press('A');
    CHECK(out.size() == 1 && out[0] == msgText("a"));

    // Ctrl+A / C / V / X / Z: keycodes with the Ctrl meta, no text.
    const std::pair<WPARAM, uint32_t> chords[] = {{'A', 29}, {'C', 31}, {'V', 50}, {'X', 52}, {'Z', 54}};
    for (auto [vk, kc] : chords) {
        out.clear();
        hold(true, false);
        press(vk);
        hold(false, false);
        const bool ok = out.size() == 2 && out[0] == msgKeycode(kKeyDown, kc, 0, kMetaCtrl) && out[1] == msgKeycode(kKeyUp, kc, 0, kMetaCtrl);
        if (!ok) std::printf("  Ctrl+%c: %zu message(s)\n", static_cast<char>(vk), out.size());
        CHECK(ok);
    }
    out.clear();
    hold(true, true);
    press('Z');  // Ctrl+Shift+Z
    hold(false, false);
    CHECK(out.size() == 2 && out[0] == msgKeycode(kKeyDown, 54, 0, kMetaCtrl | kMetaShift));

    // The app's own shortcuts never reach the phone: Ctrl+Up / Down / M
    // (volume), Ctrl+Left / Right (rotate), Ctrl+L (translate), Ctrl+D (disconnect).
    out.clear();
    for (WPARAM vk : {WPARAM(VK_UP), WPARAM(VK_DOWN), WPARAM('M'), WPARAM(VK_LEFT), WPARAM(VK_RIGHT), WPARAM('L'), WPARAM('D')}) {
        hold(true, false);
        press(vk);
        hold(true, true);  // Ctrl+Shift too (Ctrl+Shift+L: translate a region)
        press(vk);
    }
    hold(false, false);
    CHECK(out.empty());

    // Ctrl released: plain keys carry no meta (nothing sticks).
    out.clear();
    press(VK_UP);
    press('C');
    CHECK(out.size() == 3 && out[0] == msgKeycode(kKeyDown, 19, 0, 0) && out[1] == msgKeycode(kKeyUp, 19, 0, 0) &&
          out[2] == msgText("c"));

    // Real Shift+letter and Caps Lock: the uppercase character as text, no meta.
    out.clear();
    hold(false, true);
    press('B');
    hold(false, false, true);
    press('D');
    hold(false, false);
    CHECK(out.size() == 2 && out[0] == msgText("B") && out[1] == msgText("D"));

    SetKeyboardState(saved);
    win.setKeyHandler(nullptr);
    win.close();
    for (int i = 0; i < 20; ++i) {
        pump();
        Sleep(10);
    }
}
#endif

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

// Session::stop() while touch traffic keeps the control writer cycling
// (dragging on the phone while pressing 中斷連線): it must always return.
static void testSessionStopUnderTraffic() {
    section("scrcpy session: stop() during control traffic");
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
    const uint16_t port = ntohs(a.sin_port);
    constexpr int kRounds = 25;
    std::thread server([&] {
        for (int i = 0; i < kRounds; ++i) {
            SOCKET v = accept(ls, nullptr, nullptr);
            if (v == INVALID_SOCKET) return;
            uint8_t dummy = 0;
            send(v, reinterpret_cast<char*>(&dummy), 1, 0);
            SOCKET c = accept(ls, nullptr, nullptr);
            char name[64] = "fake";
            send(v, name, 64, 0);
            char b[4096];
            while (recv(c, b, sizeof(b), 0) > 0) {
            }
            closesocket(c);
            closesocket(v);
        }
    });
    int connected = 0;
    double worstMs = 0;
    for (int i = 0; i < kRounds; ++i) {
        MockVideo mv;
        scrcpy::Session s;
        scrcpy::Session::Config cfg;
        cfg.port = port;
        cfg.audio = false;
        cfg.delayMs = 10;
        if (!s.connect(cfg, nullptr, nullptr)) break;
        ++connected;
        s.run(&mv, nullptr);
        std::atomic<bool> go{true};
        std::thread touch([&] {
            const auto m = scrcpy::msgTouch(scrcpy::kMotionMove, scrcpy::kPointerGenericFinger, 10, 10, 100, 100, 1, 0, 0);
            while (go) s.sendControl(m);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(5 + i % 7));
        const auto t0 = std::chrono::steady_clock::now();
        s.stop();
        worstMs = std::max(worstMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        go = false;
        touch.join();
    }
    closesocket(ls);
    server.join();
    std::printf("  %d stop()s under traffic, slowest %.0f ms\n", connected, worstMs);
    CHECK(connected == kRounds && worstMs < 1000);
}

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

// AndroidSource's state machine against pm_android_fake_adb (no sockets, no
// phone; one "connected" phone, `push` takes 4 s):
//   * a stop() while connected or during start-up (an iPhone taking over)
//     ends in Idle: auto-reconnect only runs from Idle / Error;
//   * a QR pairing cancelled while still queued behind the busy worker never
//     runs (it used to reset the shared cancel flag and listen for 10 min).
static void testSourceStates(const std::wstring& dir) {
    section("AndroidSource states vs fake adb");
    using S = AndroidSource::State;
    const std::wstring tools = dir + L"\\fake-android-tools";
    CreateDirectoryW(tools.c_str(), nullptr);
    if (!CopyFileW((dir + L"\\pm_android_fake_adb.exe").c_str(), (tools + L"\\adb.exe").c_str(), FALSE)) {
        CHECK(!"copy pm_android_fake_adb.exe");
        return;
    }
    std::ofstream(tools + L"\\scrcpy-server", std::ios::binary) << "fake";
    SetEnvironmentVariableW(L"PM_ADB_PORT", L"15099");
    AndroidSource src;
    std::mutex lmu;
    std::vector<std::string> logs;
    std::vector<std::pair<S, std::wstring>> states;
    std::atomic<int> connected{0};
    src.log = [&](const std::string& s) { std::lock_guard<std::mutex> l(lmu); logs.push_back(s); };
    src.events.onState = [&](S s, const std::wstring& d) { std::lock_guard<std::mutex> l(lmu); states.emplace_back(s, d); };
    src.events.onConnected = [&](const std::wstring&) { ++connected; };
    if (!src.init(tools)) {
        CHECK(!"init with the fake adb");
        return;
    }
    auto waitFor = [](auto pred, int ms) {
        for (int t = 0; t < ms && !pred(); t += 20) std::this_thread::sleep_for(20ms);
        return pred();
    };
    auto sawDetail = [&](const wchar_t* prefix, size_t from) {
        std::lock_guard<std::mutex> l(lmu);
        for (size_t i = from; i < states.size(); ++i)
            if (states[i].second.rfind(prefix, 0) == 0) return true;
        return false;
    };
    auto sawLog = [&](const char* text) {
        std::lock_guard<std::mutex> l(lmu);
        for (auto& s : logs)
            if (s.find(text) != std::string::npos) return true;
        return false;
    };
    auto mark = [&] { std::lock_guard<std::mutex> l(lmu); return states.size(); };
    auto connect = [&] {
        const int n = connected;
        CHECK(src.connectKnownDevices());
        CHECK(waitFor([&] { return connected > n; }, 8000));
        std::this_thread::sleep_for(200ms);  // the reconnect task returns
        CHECK(src.state() == S::Connecting);
    };

    // Connected, but the window is busy (onAndroidConnected keeps the iPhone): stop() -> Idle.
    connect();
    src.stop();
    CHECK(src.state() == S::Idle);

    // An iPhone takes over during the start-up (claimSource -> stop()).
    connect();
    size_t m = mark();
    CHECK(src.start(nullptr, nullptr));
    CHECK(waitFor([&] { return sawDetail(L"正在啟動投影", m); }, 5000));
    std::this_thread::sleep_for(300ms);  // inside the 4 s push
    src.stop();
    CHECK(waitFor([&] { return src.state() == S::Idle; }, 6000));
    std::this_thread::sleep_for(500ms);
    CHECK(src.state() == S::Idle);  // the cancelled start left nothing behind

    // Pairing panel opened and closed while the worker is busy with a start-up.
    connect();
    m = mark();
    CHECK(src.start(nullptr, nullptr));
    CHECK(waitFor([&] { return sawDetail(L"正在啟動投影", m); }, 5000));
    std::vector<uint8_t> bgra;
    int size = 0;
    std::wstring text;
    CHECK(src.beginQrPairing(bgra, size, text));  // queued behind the push
    src.cancelPairing();                          // panel closed
    src.stop();
    CHECK(waitFor([&] { return sawLog("pairing cancelled before it ran"); }, 8000));
    std::this_thread::sleep_for(300ms);
    {
        std::lock_guard<std::mutex> l(lmu);
        bool waited = false;
        for (size_t i = m; i < states.size(); ++i) waited = waited || states[i].first == S::WaitingForPairing;
        CHECK(!waited);
    }
    CHECK(src.state() == S::Idle);
    CHECK(src.connectKnownDevices());  // the worker is free again (not "busy")
    CHECK(waitFor([&] { return connected >= 4; }, 8000));
    std::lock_guard<std::mutex> l(lmu);
    std::printf("  states:");
    for (auto& [s, d] : states) std::printf(" %d", int(s));
    std::printf("\n");
}

// ------------------------------------------------------- 0.8.3 (droid) --

static void testBackoff() {
    section("exponential backoff");
    android::Backoff b(100, 1000, 0);
    std::vector<int> d;
    for (int i = 0; i < 6; ++i) d.push_back(b.next());
    CHECK((d == std::vector<int>{100, 200, 400, 800, 1000, 1000}));
    CHECK(b.attempts() == 6);
    b.reset();
    CHECK(b.next() == 100);
    // Jitter stays within ±20 % of the nominal delay, and is not constant.
    android::Backoff j(1000, 30000, 0.2, 12345);
    bool varied = false;
    int prev = -1;
    for (int i = 0; i < 8; ++i) {
        const int nominal = j.nominal();
        const int v = j.next();
        CHECK(v >= int(nominal * 0.8) - 1 && v <= int(nominal * 1.2) + 1);
        if (i == 0) prev = v;
        else if (nominal == 30000 && v != prev) varied = true;
        if (nominal == 30000) prev = v;
    }
    CHECK(varied);
}

static void testControl083() {
    section("control messages 0.8.3 (scrcpy v5.0 test vectors)");
    using namespace scrcpy;
    CHECK((msgGetClipboard(kCopyKeyCopy) == std::vector<uint8_t>{8, 1}));
    {
        auto v = msgSetClipboard(0x0102030405060708ull, true, "hello, world!");
        std::vector<uint8_t> e = {9, 1, 2, 3, 4, 5, 6, 7, 8, 1, 0, 0, 0, 0x0d,
                                  'h', 'e', 'l', 'l', 'o', ',', ' ', 'w', 'o', 'r', 'l', 'd', '!'};
        CHECK(v == e);
        // test_serialize_set_clipboard_long: exactly the maximum fits.
        auto l = msgSetClipboard(1, true, std::string(kClipboardTextMax + 10, 'a'));
        CHECK(l.size() == (size_t(1) << 18));
        // never cut inside a UTF-8 character
        auto u = msgSetClipboard(1, false, std::string(kClipboardTextMax - 1, 'a') + "\xE4\xB8\xAD");
        CHECK(u.size() == 14 + kClipboardTextMax - 1);
    }
    CHECK((msgSetDisplayPower(true) == std::vector<uint8_t>{10, 1}));
    CHECK((msgSetDisplayPower(false) == std::vector<uint8_t>{10, 0}));
    CHECK((msgResetVideo() == std::vector<uint8_t>{17}));
}

static void testDeviceMsg() {
    section("device messages (clipboard / ack / uhid)");
    using namespace scrcpy;
    // test_device_msg_deserialize.c vectors + a UTF-8 clipboard, fed in every chunking.
    std::vector<uint8_t> s = {0, 0, 0, 0, 3, 'A', 'B', 'C'};
    const std::string zh = "\xE6\x89\x8B\xE6\xA9\x9F";  // 手機
    s.insert(s.end(), {0, 0, 0, 0, uint8_t(zh.size())});
    s.insert(s.end(), zh.begin(), zh.end());
    s.insert(s.end(), {1, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08});
    s.insert(s.end(), {2, 0, 42, 0, 5, 'h', 'e', 'l', 'l', 'o'});
    for (size_t chunk : {size_t(1), size_t(2), size_t(7), s.size()}) {
        std::vector<std::string> clips;
        uint64_t ack = 0;
        int uhid = -1;
        DeviceMsgParser::Callbacks cb;
        cb.onClipboard = [&](const std::string& t) { clips.push_back(t); };
        cb.onAckClipboard = [&](uint64_t q) { ack = q; };
        cb.onUhidOutput = [&](uint16_t id, const uint8_t*, size_t n) { uhid = int(id) * 100 + int(n); };
        DeviceMsgParser p(cb);
        bool ok = true;
        for (size_t i = 0; i < s.size(); i += chunk) ok = ok && p.feed(&s[i], std::min(chunk, s.size() - i));
        CHECK(ok && (clips == std::vector<std::string>{"ABC", zh}) && ack == 0x0102030405060708ull && uhid == 4205);
    }
    DeviceMsgParser bad({});
    const uint8_t junk[] = {7, 0, 0};
    CHECK(!bad.feed(junk, sizeof(junk)) && bad.failed());
    const uint8_t huge[] = {0, 0x7f, 0xff, 0xff, 0xff};
    DeviceMsgParser big({});
    CHECK(!big.feed(huge, sizeof(huge)));
}

static void testExplain() {
    section("plain-language messages / names / server args");
    namespace ai = android_internal;
    CHECK(ai::explainInstallFailure("Performing Streamed Install\nadb: failed to install a.apk: Failure "
                                    "[INSTALL_FAILED_VERSION_DOWNGRADE: Downgrade detected: Update version code 1 is older than current 9]",
                                    1)
              .find(L"較新的版本") != std::wstring::npos);
    CHECK(ai::explainInstallFailure("Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE: Package x signatures do not match]", 1)
              .find(L"簽章") != std::wstring::npos);
    CHECK(ai::explainInstallFailure("Failure [INSTALL_FAILED_INSUFFICIENT_STORAGE]", 1).find(L"儲存空間") != std::wstring::npos);
    CHECK(ai::explainInstallFailure("Failure [INSTALL_FAILED_NO_MATCHING_ABIS: Failed to extract native libraries, res=-113]", 1)
              .find(L"處理器") != std::wstring::npos);
    CHECK(ai::explainInstallFailure("Failure [INSTALL_PARSE_FAILED_NOT_APK: Failed to parse]", 1).find(L"不是完整的 APK") !=
          std::wstring::npos);
    CHECK(ai::explainInstallFailure("Failure [INSTALL_FAILED_USER_RESTRICTED: Install canceled by user]", 1)
              .find(L"USB 安裝") != std::wstring::npos);
    CHECK(ai::explainInstallFailure("", -2).find(L"逾時") != std::wstring::npos);
    CHECK(ai::explainInstallFailure("adb: device offline", 1) == L"手機連線中斷，無法安裝");
    CHECK(ai::explainInstallFailure("Failure [INSTALL_FAILED_SOMETHING_NEW]", 1) == L"安裝失敗：INSTALL_FAILED_SOMETHING_NEW");
    CHECK(ai::explainPushFailure("adb: error: failed to copy 'a' to '/sdcard/Download/a': remote No space left on device", 1) ==
          L"手機的儲存空間不足");
    CHECK(ai::explainPushFailure("", -2).find(L"逾時") != std::wstring::npos);
    int kind = -1;
    CHECK(ai::explainServerLine("[server] ERROR: Capture/encoding error: android.media.MediaCodec$CodecException: Error 0xfffffff4",
                                &kind)
              .find(L"錄製畫面") != std::wstring::npos &&
          kind == 1);
    CHECK(ai::explainServerLine("[server] WARN: Audio disabled: it is not supported before Android 11", &kind)
              .find(L"低於 11") != std::wstring::npos &&
          kind == 2);
    CHECK(ai::explainServerLine("[server] ERROR: Audio capture error", &kind).find(L"聲音") != std::wstring::npos && kind == 2);
    CHECK(ai::explainServerLine("[server] ERROR: java.lang.SecurityException: Injecting input events requires the caller "
                                "to have the INJECT_EVENTS permission.",
                                &kind)
              .find(L"USB 偵錯（安全性設定）") != std::wstring::npos &&
          kind == 3);
    CHECK(ai::explainServerLine("[server] INFO: Device: [samsung] SM-S928B (Android 14)", &kind).empty() && kind == 0);
    CHECK(ai::downloadName(L"C:\\x\\測試 檔案.txt") == "\xE6\xB8\xAC\xE8\xA9\xA6 \xE6\xAA\x94\xE6\xA1\x88.txt");
    CHECK(ai::downloadName(L"a:b*c?.pdf") == "a_b_c_.pdf" && ai::downloadName(L"x\\ .") == "file");
    CHECK(ai::numberedName("a.txt", 1) == "a (1).txt" && ai::numberedName("README", 2) == "README (2)" &&
          ai::numberedName(".bashrc", 3) == ".bashrc (3)");
    CHECK(ai::shellQuote("it's") == "'it'\\''s'");
    ai::ServerOptions o;
    o.scid = "0000abcd";
    auto a = ai::serverArgs(o);
    auto has = [&](const char* s) { return std::find(a.begin(), a.end(), s) != a.end(); };
    CHECK(a[0] == "scid=0000abcd" && has("clipboard_autosync=true") && has("tunnel_forward=true") && !has("stay_awake=true"));
    o.stayAwake = true;
    a = ai::serverArgs(o);
    CHECK(has("stay_awake=true") && has("keep_active=true"));
}

// A scrcpy-server stand-in on 127.0.0.1 for AndroidSource (behind the fake
// adb's `forward`): one session per video+audio+control connection triple,
// streams a frame + an audio packet every 30 ms.  mode (one-shot per session):
// 1 = stall (sockets stay open, nothing sent), 2 = close now (scrcpy died).
// captureErrorNext: the next session answers with video codec 1.
struct FakeScrcpy {
    SOCKET ls = INVALID_SOCKET;
    uint16_t port = 0;
    std::atomic<int> mode{0};
    std::atomic<bool> captureErrorNext{false};
    std::atomic<int> sessions{0};
    std::atomic<bool> quit{false};
    std::thread th;
    std::mutex m;
    std::vector<uint8_t> control;     // client → device bytes, all sessions
    std::vector<size_t> starts;       // offset where each session began
    SOCKET ctl = INVALID_SOCKET;

    bool start() {
        WSADATA wd;
        WSAStartup(MAKEWORD(2, 2), &wd);
        ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(ls, 8) != 0) return false;
        int len = sizeof(a);
        getsockname(ls, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        th = std::thread([this] { run(); });
        return true;
    }
    void stop() {
        quit = true;
        closesocket(ls);
        if (th.joinable()) th.join();
    }
    static bool sendAll(SOCKET s, const std::vector<uint8_t>& v) {
        return send(s, reinterpret_cast<const char*>(v.data()), int(v.size()), 0) == int(v.size());
    }
    static bool peerClosed(SOCKET s) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(s, &r);
        timeval tv{0, 0};
        if (select(0, &r, nullptr, nullptr, &tv) <= 0) return false;
        char b;
        return recv(s, &b, 1, MSG_PEEK) <= 0;
    }
    void run() {
        while (!quit) {
            SOCKET v = accept(ls, nullptr, nullptr);
            if (v == INVALID_SOCKET) return;
            uint8_t dummy = 0;
            send(v, reinterpret_cast<char*>(&dummy), 1, 0);
            SOCKET au = accept(ls, nullptr, nullptr);
            SOCKET c = accept(ls, nullptr, nullptr);
            if (au == INVALID_SOCKET || c == INVALID_SOCKET) return;
            char name[64] = "Fake Phone";
            send(v, name, 64, 0);
            {
                std::lock_guard<std::mutex> l(m);
                starts.push_back(control.size());
                ctl = c;
            }
            ++sessions;
            std::thread reader([this, c] {
                char b[4096];
                int n;
                while ((n = recv(c, b, sizeof(b), 0)) > 0) {
                    std::lock_guard<std::mutex> l(m);
                    control.insert(control.end(), b, b + n);
                }
            });
            if (captureErrorNext.exchange(false)) {
                std::vector<uint8_t> e;
                be32(e, scrcpy::kCodecError);
                sendAll(v, e);
                std::this_thread::sleep_for(100ms);
            } else {
                std::vector<uint8_t> hv, ha;
                be32(hv, scrcpy::kCodecH264);
                session(hv, 1080, 2340);
                packet(hv, scrcpy::kFlagConfig, kSpsPps);
                packet(hv, scrcpy::kFlagKeyFrame, kIdr);
                be32(ha, scrcpy::kCodecAac);
                packet(ha, scrcpy::kFlagConfig, {0x11, 0x90});
                bool ok = sendAll(v, hv) && sendAll(au, ha);
                for (uint64_t pts = 1; ok && !quit; ++pts) {
                    std::this_thread::sleep_for(30ms);
                    const int md = mode.load();
                    if (md == 2) break;
                    if (peerClosed(v)) break;  // the client tore the session down
                    if (md == 1) continue;     // stalled: nothing on the wire
                    std::vector<uint8_t> f, s;
                    packet(f, pts * 33333, kP);
                    packet(s, pts * 21333, std::vector<uint8_t>(64, 0x21));
                    ok = sendAll(v, f) && sendAll(au, s);
                }
            }
            mode = 0;  // one-shot
            {
                std::lock_guard<std::mutex> l(m);
                ctl = INVALID_SOCKET;
            }
            closesocket(v);
            closesocket(au);
            shutdown(c, SD_BOTH);
            reader.join();
            closesocket(c);
        }
    }
    void deviceClipboard(const std::string& t) {
        std::vector<uint8_t> msg = {0};
        be32(msg, uint32_t(t.size()));
        msg.insert(msg.end(), t.begin(), t.end());
        std::lock_guard<std::mutex> l(m);
        if (ctl != INVALID_SOCKET) sendAll(ctl, msg);
    }
    // Client control messages of session `from` and later, split by type.
    std::vector<std::vector<uint8_t>> messages(size_t fromSession = 0) {
        std::lock_guard<std::mutex> l(m);
        std::vector<std::vector<uint8_t>> out;
        size_t i = fromSession < starts.size() ? starts[fromSession] : control.size();
        auto u32 = [&](size_t p) {
            return (uint32_t(control[p]) << 24) | (uint32_t(control[p + 1]) << 16) | (uint32_t(control[p + 2]) << 8) |
                   control[p + 3];
        };
        while (i < control.size()) {
            size_t n = 0;
            switch (control[i]) {
                case 0: n = 14; break;
                case 1: n = i + 5 <= control.size() ? 5 + u32(i + 1) : 0; break;
                case 2: n = 32; break;
                case 3: n = 21; break;
                case 4: case 8: case 10: n = 2; break;
                case 5: case 6: case 7: case 11: case 17: n = 1; break;
                case 9: n = i + 14 <= control.size() ? 14 + u32(i + 10) : 0; break;
                default: n = 0;
            }
            if (n == 0 || i + n > control.size()) break;
            out.emplace_back(control.begin() + ptrdiff_t(i), control.begin() + ptrdiff_t(i + n));
            i += n;
        }
        return out;
    }
    int count(uint8_t type, size_t fromSession = 0) {
        int n = 0;
        for (auto& x : messages(fromSession)) n += x[0] == type;
        return n;
    }
};

// The PC clipboard of the tests: memory only (never the owner's clipboard).
struct SharedClip : clip::Backend {
    std::shared_ptr<clip::MemoryBackend> mem;
    explicit SharedClip(std::shared_ptr<clip::MemoryBackend> m) : mem(std::move(m)) {}
    uint32_t sequence() override { return mem->sequence(); }
    bool getText(std::string& t) override { return mem->getText(t); }
    uint32_t setText(const std::string& t) override { return mem->setText(t); }
};

static std::string readText(const std::wstring& p) {
    // The fake adb appends under an exclusive lock: retry while it writes.
    for (int i = 0; i < 200; ++i) {
        std::ifstream f(p, std::ios::binary);
        if (f) return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
        std::this_thread::sleep_for(5ms);
    }
    return {};
}

// AndroidSource end to end against pm_android_fake_adb + FakeScrcpy: no
// phone, no network (loopback only), no window, no sound, no real input.
static void testSourceMirror(const std::wstring& dir, bool mdnsLoopback) {
    section("AndroidSource 0.8.3 vs fake phone (reconnect, stall, clipboard, screen, transfers, USB, idle adb)");
    using S = AndroidSource::State;
    namespace ai = android_internal;
    const std::wstring tools = dir + L"\\fake-android-tools";
    const std::wstring st = dir + L"\\fake-phone-state";
    CreateDirectoryW(tools.c_str(), nullptr);
    CreateDirectoryW(st.c_str(), nullptr);
    for (const wchar_t* f : {L"calls.log", L"devices.txt", L"offline", L"exists.txt", L"stat.txt", L"push_fail",
                             L"install_result", L"server_error"})
        DeleteFileW((st + L"\\" + f).c_str());
    if (!CopyFileW((dir + L"\\pm_android_fake_adb.exe").c_str(), (tools + L"\\adb.exe").c_str(), FALSE)) {
        CHECK(!"copy pm_android_fake_adb.exe");
        return;
    }
    std::ofstream(tools + L"\\scrcpy-server", std::ios::binary) << "fake";
    auto touch = [&](const wchar_t* f, const std::string& content = "") {
        std::ofstream(st + L"\\" + f, std::ios::binary) << content;
    };
    auto rm = [&](const wchar_t* f) { DeleteFileW((st + L"\\" + f).c_str()); };
    auto calls = [&] { return readText(st + L"\\calls.log"); };
    auto countCalls = [&](const std::string& what) {
        std::string c = calls();
        int n = 0;
        for (size_t p = 0; (p = c.find(what, p)) != std::string::npos; ++p) ++n;
        return n;
    };

    FakeScrcpy fake;
    if (!fake.start()) {
        CHECK(!"fake scrcpy-server listen");
        return;
    }
    SetEnvironmentVariableW(L"PM_ADB_PORT", L"15099");
    SetEnvironmentVariableW(L"PM_FAKE_ADB_DIR", st.c_str());
    SetEnvironmentVariableW(L"PM_FAKE_SCRCPY_PORT", std::to_wstring(fake.port).c_str());
    SetEnvironmentVariableW(L"PM_FAKE_PUSH_MS", L"400");

    auto mem = std::make_shared<clip::MemoryBackend>();
    ai::Tuning saved = ai::tuning();
    ai::Tuning& T = ai::tuning();
    T.monitorMs = 100;
    T.stallResetMs = 500;
    T.stallMs = 1000;
    T.firstFrameMs = 4000;
    T.restartBaseMs = 100;
    T.restartCapMs = 400;
    T.reconnectAttempts = 4;
    T.reconnectBaseMs = 300;
    T.reconnectCapMs = 2400;
    T.rediscoverMs = 300;
    T.probeTimeoutMs = 2000;
    T.idleKillMs = 0;  // its own check at the end
    T.usbAuthWaitMs = 4000;
    T.usbFindMs = 800;
    T.transferPollMs = 100;
    T.mdnsBrowser = false;
    T.clipboardFactory = [mem] { return std::make_unique<SharedClip>(mem); };

    std::mutex lmu;
    std::vector<std::string> logs;
    std::vector<std::pair<S, std::wstring>> states;
    std::vector<std::wstring> notices;
    std::atomic<int> connected{0}, disconnected{0};
    MockVideo mv;
    MockAudio ma;
    std::atomic<bool> autoStart{true};
    {
        AndroidSource src;
        const bool verbose = GetEnvironmentVariableW(L"PM_TEST_VERBOSE", nullptr, 0) > 0;
        src.log = [&, verbose](const std::string& s) {
            std::lock_guard<std::mutex> l(lmu);
            logs.push_back(s);
            if (verbose) std::printf("    | %s\n", s.c_str());
        };
        src.events.onState = [&](S s, const std::wstring& d) { std::lock_guard<std::mutex> l(lmu); states.emplace_back(s, d); };
        src.events.onNotice = [&](const std::wstring& n) { std::lock_guard<std::mutex> l(lmu); notices.push_back(n); };
        src.events.onConnected = [&](const std::wstring&) {
            ++connected;
            if (autoStart) src.start(&mv, &ma);  // like the app: start from onConnected
        };
        src.events.onDisconnected = [&] { ++disconnected; };
        if (!src.init(tools)) {
            CHECK(!"init with the fake adb");
            fake.stop();
            ai::tuning() = saved;
            return;
        }
        auto waitFor = [](auto pred, int ms) {
            for (int t = 0; t < ms && !pred(); t += 20) std::this_thread::sleep_for(20ms);
            return pred();
        };
        auto mark = [&] { std::lock_guard<std::mutex> l(lmu); return states.size(); };
        auto sawState = [&](S s, const wchar_t* prefix, size_t from) {
            std::lock_guard<std::mutex> l(lmu);
            for (size_t i = from; i < states.size(); ++i)
                if (states[i].first == s && (!prefix || states[i].second.rfind(prefix, 0) == 0)) return true;
            return false;
        };
        auto sawLog = [&](const char* t) {
            std::lock_guard<std::mutex> l(lmu);
            for (auto& s : logs)
                if (s.find(t) != std::string::npos) return true;
            return false;
        };
        auto frames = [&] { std::lock_guard<std::mutex> l(mv.mu); return mv.frames.size(); };
        auto mirroring = [&] { return src.state() == S::Mirroring && fake.sessions > 0; };

        // 1. Mirror.
        size_t m0 = mark();
        CHECK(src.connectKnownDevices());
        CHECK(waitFor(mirroring, 8000));
        CHECK(waitFor([&] { return frames() > 5; }, 3000));
        CHECK(waitFor([&] { return countCalls("clipboard_autosync=true") == 1; }, 3000) && countCalls("stay_awake=true") == 0);
        std::printf("  mirroring through the fake adb forward (port %u): %zu frames\n", fake.port, frames());

        // 2. Phone → PC clipboard (device CLIPBOARD message).
        const std::string phoneText = "\xE6\x89\x8B\xE6\xA9\x9F\xE6\x96\x87\xE5\xAD\x97";  // 手機文字
        fake.deviceClipboard(phoneText);
        CHECK(waitFor([&] { std::string t; return mem->getText(t) && t == phoneText; }, 2000));

        // 3. Only watching: a PC copy is not sent.  Then the user clicks: it is.
        mem->userCopy("pc text 1");
        std::this_thread::sleep_for(600ms);
        CHECK(fake.count(9) == 0);
        VideoWindow::PointerEvent e;
        e.kind = VideoWindow::PointerEvent::Kind::Down;
        e.x = 0.5f;
        e.y = 0.5f;
        src.sendPointer(e);
        e.kind = VideoWindow::PointerEvent::Kind::Up;
        src.sendPointer(e);
        CHECK(waitFor([&] { return fake.count(9) == 1; }, 2000));
        {
            auto msgs = fake.messages();
            std::string sent;
            for (auto& x : msgs)
                if (x[0] == 9) sent.assign(x.begin() + 14, x.end());
            CHECK(sent == "pc text 1");
            bool echoed = false;  // the phone's own text never goes back
            for (auto& x : msgs)
                if (x[0] == 9 && std::string(x.begin() + 14, x.end()) == phoneText) echoed = true;
            CHECK(!echoed);
        }
        // Sync off: neither way.
        src.setClipboardSync(false);
        fake.deviceClipboard("from phone while off");
        mem->userCopy("pc text 2");
        std::this_thread::sleep_for(500ms);
        {
            std::string t;
            mem->getText(t);
            CHECK(t == "pc text 2" && fake.count(9) == 1);
        }
        src.setClipboardSync(true);

        // 4. Phone screen off while mirroring; stop() switches it back on.
        src.setPhoneScreenOn(false);
        CHECK(waitFor([&] {
            for (auto& x : fake.messages()) if (x == std::vector<uint8_t>{10, 0}) return true;
            return false;
        }, 2000));
        int sessionsBefore = fake.sessions;
        src.stop();
        CHECK(src.state() == S::Idle);
        CHECK(waitFor([&] {
            auto msgs = fake.messages();
            return !msgs.empty() && msgs.back() == std::vector<uint8_t>{10, 1};
        }, 1000));
        // ... and is applied again to the next session (still wanted off).
        CHECK(src.connectKnownDevices());
        CHECK(waitFor([&] { return mirroring() && fake.sessions > sessionsBefore; }, 8000));
        CHECK(waitFor([&] { return fake.count(10, size_t(fake.sessions - 1)) == 1; }, 2000));
        src.setPhoneScreenOn(true);
        CHECK(waitFor([&] {
            auto msgs = fake.messages(size_t(fake.sessions - 1));
            return !msgs.empty() && msgs.back() == std::vector<uint8_t>{10, 1};
        }, 2000));

        // 5. Stalled stream: RESET_VIDEO, then scrcpy-server restarted in place.
        int disc = disconnected;
        sessionsBefore = fake.sessions;
        m0 = mark();
        fake.mode = 1;
        CHECK(waitFor([&] { return fake.count(17, size_t(sessionsBefore - 1)) >= 1; }, 3000));
        CHECK(waitFor([&] { return fake.sessions > sessionsBefore; }, 6000));
        size_t f0 = frames();
        CHECK(waitFor([&] { return frames() > f0 + 5; }, 3000));
        CHECK(disconnected == disc && src.state() == S::Mirroring && sawLog("stream stalled"));
        CHECK(!sawState(S::Idle, nullptr, m0));
        std::printf("  stall: RESET_VIDEO sent, scrcpy-server restarted (%d sessions), no disconnect event\n",
                    fake.sessions.load());

        // 6. scrcpy-server died, adb fine: restarted in place too.
        sessionsBefore = fake.sessions;
        fake.mode = 2;
        CHECK(waitFor([&] { return fake.sessions > sessionsBefore; }, 6000));
        CHECK(waitFor([&] { return frames() > f0 + 20; }, 3000));
        CHECK(disconnected == disc && src.state() == S::Mirroring);

        // 7. Stay awake while mirroring: server restarted with the options.
        sessionsBefore = fake.sessions;
        src.setStayAwake(true);
        CHECK(waitFor([&] { return fake.sessions > sessionsBefore; }, 6000));
        CHECK(waitFor([&] { return countCalls("stay_awake=true keep_active=true") == 1; }, 2000));
        CHECK(disconnected == disc && src.state() == S::Mirroring);

        // 8. Wi-Fi lost: Idle 手機連線中斷 + onDisconnected, then reconnect
        //    attempts with growing gaps; the phone comes back on attempt 3.
        m0 = mark();
        const int probesBefore = countCalls("echo pm-alive");
        const int conn0 = connected;
        touch(L"offline");
        fake.mode = 2;
        CHECK(waitFor([&] { return disconnected > disc; }, 6000));
        CHECK(sawState(S::Idle, L"手機連線中斷", m0));
        CHECK(waitFor([&] { return sawState(S::Connecting, L"手機連線中斷，正在重新連線（第 3 次）", m0); }, 8000));
        rm(L"offline");
        CHECK(waitFor([&] { return connected > conn0; }, 8000));
        CHECK(waitFor(mirroring, 8000));
        {
            // Gaps between the attempts' probes (`shell echo pm-alive`).
            std::vector<unsigned long long> t;
            std::string c = calls();
            size_t pos = 0;
            int seen = 0;
            while ((pos = c.find("echo pm-alive", pos)) != std::string::npos) {
                size_t ls = c.rfind('\n', pos);
                ls = ls == std::string::npos ? 0 : ls + 1;
                if (++seen > probesBefore) t.push_back(std::stoull(c.substr(ls, c.find('\t', ls) - ls)));
                pos += 5;
            }
            // t[0] = the probe right after the drop; then attempts 1, 2, 3.
            std::printf("  reconnect probes after the drop:");
            for (size_t i = 1; i < t.size(); ++i) std::printf(" +%llu ms", t[i] - t[i - 1]);
            std::printf("\n");
            CHECK(t.size() >= 4);
            if (t.size() >= 4) CHECK(t[3] - t[2] > t[2] - t[1]);  // exponential: the gaps grow
        }
        CHECK(sawLog("disconnect 10.9.8.7:5555"));  // the dead transport was dropped before connecting

        // 9. Gone for good: gives up after T.reconnectAttempts, Idle with the reason.
        m0 = mark();
        disc = disconnected;
        touch(L"offline");
        fake.mode = 2;
        CHECK(waitFor([&] { return sawState(S::Idle, L"手機連線中斷，重新連線失敗", m0); }, 20000));
        rm(L"offline");
        // stop() during a reconnect cancels it.
        CHECK(src.connectKnownDevices());
        CHECK(waitFor(mirroring, 8000));
        m0 = mark();
        touch(L"offline");
        fake.mode = 2;
        CHECK(waitFor([&] { return sawState(S::Idle, L"手機連線中斷", m0); }, 6000));
        src.stop();
        rm(L"offline");
        std::this_thread::sleep_for(1500ms);
        CHECK(sawLog("reconnect 10.9.8.7:5555 cancelled") && !sawState(S::Mirroring, nullptr, m0));

        // 10. Capture error (encoder died on the phone): one restart, then Error with the reason.
        CHECK(src.connectKnownDevices());
        CHECK(waitFor(mirroring, 8000));
        m0 = mark();
        disc = disconnected;
        touch(L"server_error", "[server] ERROR: Capture/encoding error: android.media.MediaCodec$CodecException: Error 0xfffffff4");
        fake.captureErrorNext = true;
        std::this_thread::sleep_for(200ms);
        fake.mode = 2;
        CHECK(waitFor([&] { return sawState(S::Error, L"手機無法錄製畫面", m0); }, 8000));
        CHECK(disconnected == disc + 1);
        std::this_thread::sleep_for(800ms);
        CHECK(!sawState(S::Connecting, L"手機連線中斷，正在重新連線", m0));  // not a connection problem

        // 11. pushFile: Unicode name, name taken → "(1)", progress logged, callback thread.
        CHECK(src.connectKnownDevices());
        CHECK(waitFor(mirroring, 8000));
        const std::wstring local = dir + L"\\測試 檔案.txt";
        std::ofstream(local, std::ios::binary) << std::string(300 * 1024, 'x');
        touch(L"exists.txt", "/sdcard/Download/\xE6\xB8\xAC\xE8\xA9\xA6 \xE6\xAA\x94\xE6\xA1\x88.txt\n");
        std::mutex dm;
        std::vector<std::pair<bool, std::wstring>> done;
        std::vector<DWORD> doneThreads;
        auto cb = [&](bool ok, std::wstring msg) {
            std::lock_guard<std::mutex> l(dm);
            done.emplace_back(ok, msg);
            doneThreads.push_back(GetCurrentThreadId());
        };
        const DWORD me = GetCurrentThreadId();
        src.pushFile(local, cb);
        {
            std::lock_guard<std::mutex> l(dm);
            CHECK(done.empty());  // never called inside pushFile()
        }
        // FIFO behind it: an APK install, a failing install, a non-APK, a failing push.
        const std::wstring apk = dir + L"\\fake.apk";
        std::ofstream(apk, std::ios::binary) << std::string(1024, 'a');
        src.installApk(apk, cb);
        CHECK(waitFor([&] { std::lock_guard<std::mutex> l(dm); return done.size() == 2; }, 8000));
        touch(L"install_result", "adb: failed to install fake.apk: Failure [INSTALL_FAILED_VERSION_DOWNGRADE: Downgrade detected]");
        src.installApk(apk, cb);
        src.installApk(local, cb);
        touch(L"push_fail", "adb: error: failed to copy 'x' to '/sdcard/Download/x': remote No space left on device");
        src.pushFile(local, cb);
        CHECK(waitFor([&] { std::lock_guard<std::mutex> l(dm); return done.size() == 5; }, 8000));
        rm(L"push_fail");
        rm(L"install_result");
        {
            std::lock_guard<std::mutex> l(dm);
            if (done.size() == 5) {
                CHECK(done[0].first && done[0].second == L"已傳送到手機的 Download 資料夾：測試 檔案 (1).txt");
                CHECK(done[1].first && done[1].second == L"已安裝到手機：fake.apk");
                CHECK(!done[2].first && done[2].second.find(L"較新的版本") != std::wstring::npos);
                CHECK(!done[3].first && done[3].second == L"只能安裝 .apk 檔");
                CHECK(!done[4].first && done[4].second == L"手機的儲存空間不足");
                for (auto& [ok, msg] : done) std::printf("  done(%d): %s\n", int(ok), adb::narrow(msg).c_str());
            }
            bool otherThread = !doneThreads.empty();
            for (DWORD t : doneThreads) otherThread = otherThread && t != me;
            CHECK(otherThread && doneThreads.size() == 5 &&
                  std::all_of(doneThreads.begin(), doneThreads.end(), [&](DWORD t) { return t == doneThreads[0]; }));
        }
        CHECK(countCalls("push " + adb::narrow(local) + " /sdcard/Download/\xE6\xB8\xAC\xE8\xA9\xA6 \xE6\xAA\x94\xE6\xA1\x88 (1).txt") == 1);
        CHECK(countCalls("install -r " + adb::narrow(apk)) == 2);
        CHECK(sawLog("pushFile progress") && sawLog("pushFile progress 100%"));
        src.stop();

        // 12. USB: unauthorized first (the phone asks), then allowed.
        autoStart = false;
        touch(L"devices.txt", "R5CT123ABC     unauthorized usb:1-4 transport_id:3\n");
        m0 = mark();
        const int conn1 = connected;
        CHECK(src.connectUsb());
        CHECK(waitFor([&] { return sawState(S::Connecting, L"請在手機上允許 USB 偵錯", m0); }, 3000));
        touch(L"devices.txt", "R5CT123ABC     device usb:1-4 product:e3q model:SM_S928B device:e3q transport_id:3\n");
        CHECK(waitFor([&] { return connected > conn1; }, 4000));
        CHECK(src.start(&mv, &ma));
        CHECK(waitFor([&] { return src.state() == S::Mirroring; }, 8000));
        CHECK(waitFor([&] { return countCalls("-s R5CT123ABC push") >= 1 && countCalls("-s R5CT123ABC shell CLASSPATH=") >= 1; }, 3000));
        src.stop();
        // No USB phone at all.
        touch(L"devices.txt", "10.9.8.7:5555   device product:fake model:Fake_Phone device:fake\n");
        m0 = mark();
        CHECK(src.connectUsb());
        CHECK(waitFor([&] { return sawState(S::Idle, L"沒有找到用 USB 連接的手機", m0); }, 4000));
        rm(L"devices.txt");
        autoStart = true;
        std::lock_guard<std::mutex> l(lmu);
        std::printf("  notices: %zu, states: %zu, sessions: %d\n", notices.size(), states.size(), fake.sessions.load());
    }
    std::wstring localName = dir + L"\\測試 檔案.txt";
    DeleteFileW(localName.c_str());
    DeleteFileW((dir + L"\\fake.apk").c_str());

    // 13. adb server idle shutdown: started on demand, stopped when idle, started again.
    {
        T.idleKillMs = 800;
        rm(L"calls.log");
        std::vector<std::string> ilogs;
        std::mutex im;
        AndroidSource src;
        src.log = [&](const std::string& s) { std::lock_guard<std::mutex> l(im); ilogs.push_back(s); };
        CHECK(src.init(tools));
        auto waitFor = [](auto pred, int ms) {
            for (int t = 0; t < ms && !pred(); t += 20) std::this_thread::sleep_for(20ms);
            return pred();
        };
        CHECK(waitFor([&] { return countCalls("start-server") == 1; }, 3000));
        CHECK(waitFor([&] { return countCalls("kill-server") == 1; }, 4000));
        CHECK(src.connectKnownDevices());  // needs adb again
        CHECK(waitFor([&] { return countCalls("start-server") == 2; }, 3000));
        CHECK(waitFor([&] { return src.state() == AndroidSource::State::Connecting; }, 4000));
        std::this_thread::sleep_for(1500ms);
        CHECK(countCalls("kill-server") == 1);  // a connected phone keeps it
        src.stop();
        CHECK(waitFor([&] { return countCalls("kill-server") == 2; }, 4000));
        std::lock_guard<std::mutex> l(im);
        bool said = false;
        for (auto& s : ilogs) said = said || s.find("adb server stopped (idle") != std::string::npos;
        CHECK(said);
    }
    // 14. The app's periodic scan while adb is stopped: our own mDNS (loopback
    //     target here, nothing leaves the PC) finds no phone -> adb not started.
    //     (Its socket binds 0.0.0.0 like testMdnsLoopback: skipped by --no-mdns.)
    if (mdnsLoopback) {
        T.idleKillMs = 800;
        T.mdnsBrowser = true;
        T.mdnsTargets = {"127.0.0.1:9"};  // discard port: never answers
        T.coldScanMs = 500;
        rm(L"calls.log");
        AndroidSource src;
        std::mutex im;
        std::vector<std::pair<S, std::wstring>> ist;
        src.events.onState = [&](S s, const std::wstring& d) { std::lock_guard<std::mutex> l(im); ist.emplace_back(s, d); };
        CHECK(src.init(tools));
        auto waitFor = [](auto pred, int ms) {
            for (int t = 0; t < ms && !pred(); t += 20) std::this_thread::sleep_for(20ms);
            return pred();
        };
        CHECK(waitFor([&] { return countCalls("kill-server") == 1; }, 4000));  // warm-up, then idle
        CHECK(src.connectKnownDevices());
        CHECK(waitFor([&] {
            std::lock_guard<std::mutex> l(im);
            return !ist.empty() && ist.back().first == S::Idle && ist.back().second == L"區域網路上沒有找到開啟無線偵錯的手機";
        }, 4000));
        CHECK(countCalls("start-server") == 1);  // only the warm-up
        std::printf("  periodic scan with adb stopped: no phone advertised, adb not started\n");
    }
    ai::tuning() = saved;
    fake.stop();
    SetEnvironmentVariableW(L"PM_FAKE_ADB_DIR", nullptr);
    SetEnvironmentVariableW(L"PM_FAKE_SCRCPY_PORT", nullptr);
    SetEnvironmentVariableW(L"PM_FAKE_PUSH_MS", nullptr);
}

// Measures the real adb server's idle cost with AndroidSource (private port
// 15097, ADB_MDNS_AUTO_CONNECT=0: no phone is contacted): working set / CPU
// of the adb.exe processes while idle, and whether they are gone after the
// idle timeout.  --measure-idle SECONDS
static void measureIdle(const std::wstring& toolsDir, int seconds) {
    section("adb server idle cost (real adb.exe, private port 15097)");
    SetEnvironmentVariableW(L"PM_ADB_PORT", L"15097");
    AndroidSource src;
    src.log = [](const std::string& s) { std::printf("  %s\n", s.c_str()); };
    if (!src.init(toolsDir)) {
        std::printf("  SKIP: no adb.exe\n");
        return;
    }
    for (int s = 1; s <= seconds; ++s) {
        std::this_thread::sleep_for(1s);
        if (s % 5 == 0) std::printf("  t=%2ds\n", s);
    }
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    int liveSeconds = 0;
    bool doAdb = true, python = true, mdnsLoop = true, only083 = false, window = true;
    int measureSeconds = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--live-mdns" && i + 1 < argc) liveSeconds = atoi(argv[++i]);
        else if (a == "--no-adb") doAdb = false;
        else if (a == "--no-python") python = false;
        else if (a == "--no-mdns") mdnsLoop = false;  // its browser binds 0.0.0.0
        else if (a == "--measure-idle" && i + 1 < argc) measureSeconds = atoi(argv[++i]);
        else if (a == "--only-083") only083 = true;
        else if (a == "--no-window") window = false;  // skips the off-desktop VideoWindow key test
    }
    std::wstring dir = exeDir();
    std::wstring tools = dir + L"\\android-tools";
    // Source tree location of verify_qr.py (exe is in <build>/bin/<cfg>).
    std::wstring verify = PM_ANDROID_TOOLS_DIR L"/verify_qr.py";

    if (measureSeconds > 0) {
        measureIdle(tools, measureSeconds);
        return 0;
    }
    testBackoff();
    testControl083();
    testDeviceMsg();
    testExplain();
    if (only083) {
        testSourceMirror(dir, mdnsLoop);
        std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    testControl();
    testParser();
    testInput();
#ifdef PM_ANDROID_TEST_VIDEO
    if (window) testKeysThroughWindow();
#else
    (void)window;
#endif
    testQr(python, verify);
    testDns();
    if (mdnsLoop) testMdnsLoopback();
    testAdbParsers();
    testSession();
    testSessionStopUnderTraffic();
    testSourceStates(dir);
    testSourceMirror(dir, mdnsLoop);
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
