// pm_remote_spike: stand-alone test of pm_remote (a phone browser controls
// this PC over the LAN or Tailscale). Prints the page URLs + a QR code, runs
// until Ctrl+C or --duration. Input is DRY-RUN (log only) unless --inject is
// given; --inject --test-window injects ONLY into a window of its own (the
// pointer never leaves it, clicks / keys elsewhere are dropped).
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "pm/remote.h"
#include "pm/share_server.h"  // pm::share::renderQr

namespace {

std::atomic<bool> g_quit{false};
std::function<void(const std::string&)> g_log;
std::mutex g_logMu;

void slog(const char* fmt, ...) {
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1200];
    snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [spike] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);
    std::lock_guard<std::mutex> lk(g_logMu);
    fputs(line, stderr);
    if (g_log) g_log(line);
}

BOOL WINAPI onCtrl(DWORD) {
    g_quit = true;
    return TRUE;
}

void usage() {
    fputs(
        "pm_remote_spike [options]\n"
        "  --bind IP          LAN address to serve on (default: first real LAN interface)\n"
        "  --port N           HTTP port (default 8090; 0 = any)\n"
        "  --token T          fixed token (default random)\n"
        "  --no-tailscale     do not also listen on the Tailscale address\n"
        "  --loopback         tests: the \"LAN\" side on 127.0.0.3 only (nothing on the LAN: no firewall prompt)\n"
        "  --quality 0|1|2    picture preset to start with (0 smooth 1080p, 1 standard 1440p, 2 clear native; default 2)\n"
        "  --size WxH --fps N --kbps N   override the preset\n"
        "  --monitor N        monitor index (0 = primary)\n"
        "  --idr S            IDR interval seconds (default 5)\n"
        "  --encoder auto|hw|sw\n"
        "  --inject           REALLY move the mouse / type (SendInput). Default: dry-run, log only\n"
        "  --inject-stub      tests: the whole injection path, SendInput replaced by a logging stub\n"
        "  --test-window      with --inject: open a test window and inject ONLY into it\n"
        "  --approval         new devices must be allowed (the spike answers, see --approve-after / --deny)\n"
        "  --approve-after MS allow a pending device after MS ms (default 1500)\n"
        "  --deny             deny the first request, allow the next one\n"
        "  --kick-after S     end the session from the PC S seconds after it became active\n"
        "  --view-only        picture only, input dropped\n"
        "  --idle-minutes N   end the session after N minutes without a touch (default 30, 0 = never)\n"
        "  --fixed-fps        always the full frame rate (no lower rate when the phone is not touched)\n"
        "  --no-even-pacing   frames up to a quarter slot early when the screen changed (0.8.3.5; A / B)\n"
        "  --sleep-wait       Sleep(1) between capture polls instead of the high-resolution timer (0.8.3.5; A / B)\n"
        "  --idle-after S --idle-fps N   phone idle after S seconds (default 60) -> N fps (default 15)\n"
        "  --no-keep-awake    let the PC sleep while connected\n"
        "  --test-clipboard T use an in-memory clipboard holding T (the PC clipboard is never touched)\n"
        "  --upload-dir DIR   files from the phone go to DIR (0.8.3; default: off)  --upload-max BYTES\n"
        "  --test-files A;B   with --test-clipboard: the files offered as copied in Explorer\n"
        "  --help-file PATH   the guide HTML served at /<token>/help\n"
        "  --privacy-test M   0.8.4 隱私黑屏 test cover: offscreen (default) | tiny (2x2 at the top-left pixel)\n"
        "  --privacy-real     0.8.4: a REAL full-screen cover (by hand only)   --no-privacy: not offered\n"
        "  --pcfs-root ID=DIR 0.8.4 電腦的檔案: this folder instead of the PC's own (repeatable)   --no-pcfiles: off   --pcfs-guest: 臨時帳號 may too\n"
        "  --no-audio         no audio track\n"
        "  --test-tone        play a very quiet (-70 dBFS) 440 Hz tone so the loopback has sound\n"
        "  --tone D@S+N[:A]   play a tone on the playback device whose name contains D, from S s for N s (amplitude A, 0.05)\n"
        "  --list-audio       list the active playback devices (* = Windows default) and exit\n"
        "  --tunnel           also start a cloudflared quick tunnel (外網連結) and print its one-time link + 通行碼\n"
        "  --fixed-password P 常駐外網 routes on 127.0.0.2:<port> with password P (PBKDF2-hashed in memory)\n"
        "  --guest NAME:PW:LIFE:RIGHTS  0.8.3.2 臨時帳號 on the fixed link (with --fixed-password; repeatable):\n"
        "                     LIFE once | <seconds>; RIGHTS control | view, + files (e.g. control+files)\n"
        "  --guest-drop NAME:S  tests: that account is deleted S seconds after its session is connected\n"
        "  --fixed-approval   ... and new phones still need 允許\n"
        "  --policy-fixed P   0.8.3.6 連線時要不要允許 for the 主帳號 on the fixed link: ask | remember | password\n"
        "  --policy-guest P   ... for 臨時帳號 (default ask)\n"
        "  --policy-temp P    ... for the temporary address (default ask)\n"
        "  --approve-remember 允許 with 記住 ticked (the spike keeps that list in memory, per scope)\n"
        "  --funnel N         ... published with Tailscale Funnel on port N (never 443), turned off at the end\n"
        "  --fixed-tunnel     ... published with a cloudflared quick tunnel instead\n"
        "  --cloudflared EXE  cloudflared.exe to use (default: next to this exe, else an installed one)\n"
        "  --audio-source X   sound from: \"\" automatic, default, or a device id\n"
        "  --stamp            capture-time stamp in the top-left corner from the start\n"
        "  --lang N           page language 0 zh-TW 1 en 2 ja 3 ko\n"
        "  --no-playout-delay don't ask Chrome for zero playout delay\n"
        "  --no-cursor        don't draw the mouse cursor\n"
        "  --duration S       quit after S seconds\n"
        "  --log FILE         also append the log to FILE\n"
        "  --snapdir DIR      accept the page's decoded-frame snapshot (?auto) into DIR\n"
        "  --stdin-quit       quit when stdin is closed (test drivers)\n"
        "optimisation switches (defaults are the optimised settings):\n"
        "  --cpu-path --no-skip-static --static-refresh MS --vbv-frames N --inline-send\n"
        "  --tail-frames N --rc cbr|vbr (default vbr) --rtp-payload N --scene desktop|static|motion|synth\n",
        stderr);
}

void printQr(const std::string& text) {
    std::vector<uint8_t> px;
    int n = 0;
    if (!pm::share::renderQr(text, 1, 2, px, n)) return;
    auto light = [&](int x, int y) { return y >= n || px[(size_t(y) * size_t(n) + size_t(x)) * 4] > 128; };
    std::string out;
    for (int y = 0; y < n; y += 2) {
        for (int x = 0; x < n; ++x) {
            const bool a = light(x, y), b = light(x, y + 1);
            out += a && b ? "\xE2\x96\x88" : a ? "\xE2\x96\x80" : b ? "\xE2\x96\x84" : " ";
        }
        out += "\n";
    }
    fputs(out.c_str(), stdout);
}

// ---- test window: the only place injected input may land -------------------

HWND g_testWnd = nullptr;
HWND g_testEdit = nullptr;
std::atomic<int> g_twClicks{0}, g_twRight{0}, g_twWheel{0}, g_twChars{0};

LRESULT CALLBACK testWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_LBUTTONDOWN:
        ++g_twClicks;
        slog("testwin: WM_LBUTTONDOWN at %d,%d (client)", int(short(LOWORD(l))), int(short(HIWORD(l))));
        SetFocus(g_testEdit);
        return 0;
    case WM_RBUTTONDOWN:
        ++g_twRight;
        slog("testwin: WM_RBUTTONDOWN at %d,%d (client)", int(short(LOWORD(l))), int(short(HIWORD(l))));
        return 0;
    case WM_MOUSEWHEEL:
        ++g_twWheel;
        slog("testwin: WM_MOUSEWHEEL delta %d", int(short(HIWORD(w))));
        return 0;
    case WM_ACTIVATE:
        slog("testwin: %s", LOWORD(w) == WA_INACTIVE ? "deactivated" : "activated (foreground)");
        if (LOWORD(w) != WA_INACTIVE) SetFocus(g_testEdit);
        break;
    case WM_COMMAND:
        if (HIWORD(w) == EN_CHANGE) {
            wchar_t buf[512] = {};
            GetWindowTextW(g_testEdit, buf, 511);
            char u8[1600] = {};
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, u8, sizeof(u8) - 1, nullptr, nullptr);
            ++g_twChars;
            slog("testwin: edit text now \"%s\"", u8);
        }
        break;
    case WM_CLOSE:
        return 0;  // only the spike closes it
    case WM_APP:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

RECT testRect() {
    RECT r{};
    if (g_testWnd) GetWindowRect(g_testWnd, &r);
    return r;
}

bool insideTest(int x, int y) {
    const RECT r = testRect();
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

void testWindowThread(RECT mon, std::atomic<bool>* ready) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = testWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(255, 236, 200));
    wc.lpszClassName = L"PmRemoteSpikeTestWindow";
    RegisterClassW(&wc);
    const int w = 720, h = 420;
    const int x = (mon.left + mon.right) / 2 - w / 2, y = (mon.top + mon.bottom) / 2 - h / 2;
    g_testWnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"自在投影 遠端操控 自動測試視窗（測試結束會自動關閉）",
                                WS_OVERLAPPED | WS_CAPTION | WS_VISIBLE, x, y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    g_testEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 20, 20, w - 60, 36,
                                 g_testWnd, nullptr, wc.hInstance, nullptr);
    const RECT r = testRect();
    slog("testwin: rect %ld,%ld,%ld,%ld (monitor %ld,%ld,%ld,%ld) norm-center %.4f,%.4f", r.left, r.top, r.right, r.bottom,
         mon.left, mon.top, mon.right, mon.bottom, ((r.left + r.right) / 2.0 - mon.left) / double(mon.right - mon.left),
         ((r.top + r.bottom) / 2.0 - mon.top) / double(mon.bottom - mon.top));
    *ready = true;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// ---- quiet test tone (so the loopback has something to capture) --------------

// One tone: on the playback device whose name contains `dev` ("" = the
// Windows default), from startMs after launch for durMs, at amplitude amp.
struct Tone {
    std::string dev;
    int startMs = 0, durMs = 0;
    double amp = 0.0003, freq = 440;
};
std::wstring wideOf(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
bool containsNoCase(const std::string& hay, const std::string& needle) {
    std::wstring h = wideOf(hay), n = wideOf(needle);
    CharLowerW(h.data()), CharLowerW(n.data());
    return h.find(n) != std::wstring::npos;
}

void toneThread(Tone t, ULONGLONG t0) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!g_quit && GetTickCount64() - t0 < ULONGLONG(t.startMs)) Sleep(10);
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* ac = nullptr;
    IAudioRenderClient* rc = nullptr;
    std::string name = "the default device";
    do {
        if (g_quit || FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) break;
        if (!t.dev.empty()) {
            for (const auto& d : pm::remote::playbackDevices())
                if (containsNoCase(d.name, t.dev)) {
                    en->GetDevice(wideOf(d.id).c_str(), &dev);
                    name = d.name;
                    break;
                }
            if (!dev) {
                slog("tone: no playback device matching \"%s\"", t.dev.c_str());
                break;
            }
        } else if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) break;
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ac)))) break;
        WAVEFORMATEX f{};
        f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT, f.nChannels = 2, f.nSamplesPerSec = 48000, f.wBitsPerSample = 32;
        f.nBlockAlign = 8, f.nAvgBytesPerSec = 48000 * 8;
        if (FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                  2000000, 0, &f, nullptr)))
            break;
        if (FAILED(ac->GetService(IID_PPV_ARGS(&rc)))) break;
        UINT32 bufFrames = 0;
        ac->GetBufferSize(&bufFrames);
        ac->Start();
        slog("tone: playing %.0f Hz at amplitude %.4f (%.0f dBFS) on %s for %d ms", t.freq, t.amp, 20 * std::log10(t.amp), name.c_str(),
             t.durMs > 0 ? t.durMs : -1);
        double ph = 0;
        const ULONGLONG begin = GetTickCount64();
        while (!g_quit && (t.durMs <= 0 || GetTickCount64() - begin < ULONGLONG(t.durMs))) {
            UINT32 pad = 0;
            ac->GetCurrentPadding(&pad);
            const UINT32 n = bufFrames - pad;
            BYTE* data = nullptr;
            if (n && SUCCEEDED(rc->GetBuffer(n, &data))) {
                float* p = reinterpret_cast<float*>(data);
                for (UINT32 i = 0; i < n; ++i) {
                    const float v = float(t.amp * std::sin(ph));
                    ph += 2 * 3.14159265358979 * t.freq / 48000;
                    p[2 * i] = p[2 * i + 1] = v;
                }
                rc->ReleaseBuffer(n, 0);
            }
            Sleep(20);
        }
        Sleep(250);  // let the buffered tail play out
        ac->Stop();
        slog("tone: stopped on %s", name.c_str());
    } while (false);
    if (rc) rc->Release();
    if (ac) ac->Release();
    if (dev) dev->Release();
    if (en) en->Release();
    CoUninitialize();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (const int pc = pm::remote::runPrivacyMaskIfAsked(); pc >= 0) return pc;  // 0.8.4: this exe is also the 隱私黑屏 cover
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleOutputCP(CP_UTF8);
    pm::remote::Options o;
    o.requireApproval = false;  // the spike lets every page in unless --approval
    o.privacyTest = "offscreen";  // 0.8.4: the spike never covers the screen unless --privacy-real
    double duration = 0, kickAfter = 0;
    int approveAfterMs = 1500;
    bool denyFirst = false, testWindow = false, tone = false;
    bool approveRemember = false;  // 0.8.3.6
    std::vector<Tone> tones;
    bool tunnel = false;
    std::string fixedPw;
    std::vector<pm::remote::GuestAccount> guests;  // 0.8.3.2 --guest
    std::string guestDrop;
    int guestDropSec = 0;
    int funnelPort = 0;
    bool fixedTunnel = false;
    std::wstring cloudflared;
    std::wstring logFile, keysFile;
    auto u8 = [](const wchar_t* w) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
        return s;
    };
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&]() -> const wchar_t* {
            if (i + 1 >= argc) {
                usage();
                exit(2);
            }
            return argv[++i];
        };
        if (a == L"--bind") o.bindIp = u8(next());
        else if (a == L"--port") o.port = uint16_t(_wtoi(next()));
        else if (a == L"--token") o.token = u8(next());
        else if (a == L"--no-tailscale") o.tailscale = false;
        else if (a == L"--loopback") o.loopbackTest = true, o.tailscale = false;
        else if (a == L"--quality") o.quality = _wtoi(next());
        else if (a == L"--monitor") o.monitor = _wtoi(next());
        else if (a == L"--size") {
            if (swscanf(next(), L"%dx%d", &o.maxWidth, &o.maxHeight) != 2) return usage(), 2;
        } else if (a == L"--fps") o.fps = std::max(1, std::min(120, _wtoi(next())));
        else if (a == L"--kbps") o.kbps = std::max(100, _wtoi(next()));
        else if (a == L"--idr") o.idrSeconds = std::max(0.2, _wtof(next()));
        else if (a == L"--encoder") o.encoder = u8(next());
        else if (a == L"--inject") o.inject = true;
        else if (a == L"--inject-stub") o.injectStub = true;  // 0.8.3.5: SendInput replaced by a logging stub
        else if (a == L"--inject-stub-elevated") o.injectStubElevated = u8(next());  // 0.8.3.5: ... and every window "elevated"
        else if (a == L"--test-window") testWindow = true;
        else if (a == L"--approval") o.requireApproval = true;
        else if (a == L"--approve-after") approveAfterMs = _wtoi(next());
        else if (a == L"--deny") denyFirst = true;
        else if (a == L"--kick-after") kickAfter = _wtof(next());
        else if (a == L"--view-only") o.viewOnly = true;
        else if (a == L"--idle-minutes") o.idleMinutes = _wtof(next());
        else if (a == L"--test-clipboard") o.testClipboard = true, o.testClipboardText = u8(next());
        else if (a == L"--upload-dir") o.uploadDir = next();  // 0.8.3: the phone's files go there
        else if (a == L"--upload-max") o.uploadMaxBytes = _wcstoui64(next(), nullptr, 10);
        else if (a == L"--test-files") {  // 0.8.3: "a;b" = the files "copied in Explorer" (with --test-clipboard)
            std::wstring list = next();
            for (size_t p = 0; p <= list.size();) {
                const size_t q = list.find(L';', p);
                if (q != p) o.testClipboardFiles.push_back(list.substr(p, q == std::wstring::npos ? std::wstring::npos : q - p));
                if (q == std::wstring::npos) break;
                p = q + 1;
            }
        }
        else if (a == L"--privacy-test") o.privacyTest = u8(next());  // 0.8.4: offscreen (default) | tiny
        else if (a == L"--privacy-real") o.privacyTest.clear();         // 0.8.4: a real full-screen cover (by hand only)
        else if (a == L"--no-privacy") o.privacyAllowed = false;
        else if (a == L"--pcfs-root") o.pcFilesTestRoots.push_back(next());  // 0.8.4: "id=dir" (replaces every default folder)
        else if (a == L"--no-pcfiles") o.pcFiles = false;
        else if (a == L"--pcfs-guest") o.pcFilesGuest = true;  // 0.8.4: 臨時帳號 may browse / download PC files
        else if (a == L"--help-file") o.helpFile = next();
        else if (a == L"--fixed-fps") o.adaptiveFps = false;
        else if (a == L"--no-even-pacing") o.evenPacing = false;  // 0.8.3.6 A / B: frames up to a quarter slot early (0.8.3.5)
        else if (a == L"--wait-ahead") o.waitAhead = true;  // 0.8.3.6 A / B: one wait to just before the beat (off by default)
        else if (a == L"--sleep-wait") o.preciseWait = false;  // 0.8.3.6 A / B: Sleep(1) between polls (0.8.3.5)
        else if (a == L"--idle-after") o.idleAfterSec = _wtof(next());
        else if (a == L"--idle-fps") o.idleFps = _wtoi(next());
        else if (a == L"--no-keep-awake") o.keepAwake = false;
        else if (a == L"--no-audio") o.audio = false;
        else if (a == L"--test-tone") tone = true;
        else if (a == L"--tone") {  // --tone "<device name part>@<start s>+<seconds>[:amplitude]"
            const std::string spec = u8(next());
            Tone t;
            const size_t at = spec.rfind('@'), plus = spec.find('+', at), colon = spec.find(':', plus);
            if (at == std::string::npos || plus == std::string::npos) return usage(), 2;
            t.dev = spec.substr(0, at);
            t.startMs = int(atof(spec.substr(at + 1, plus - at - 1).c_str()) * 1000);
            t.durMs = int(atof(spec.substr(plus + 1).c_str()) * 1000);
            if (colon != std::string::npos) t.amp = atof(spec.substr(colon + 1).c_str());
            else t.amp = 0.05;
            t.freq = 440.0 + 220.0 * double(tones.size());
            tones.push_back(t);
        } else if (a == L"--list-audio") {
            for (const auto& d : pm::remote::playbackDevices())
                printf("%s%s\tvolume %.2f (%s)%s, %d playing\t%s\n", d.isDefault ? "* " : "  ", d.name.c_str(), d.volume,
                       d.hwVolume ? "hardware volume" : "software volume: applied before the loopback", d.muted ? ", MUTED" : "",
                       d.activeSessions, d.id.c_str());
            return 0;
        } else if (a == L"--audio-source") o.audioSource = u8(next());
        else if (a == L"--tunnel") tunnel = true;
        else if (a == L"--fixed-password") fixedPw = u8(next());
        else if (a == L"--guest") {  // NAME:PW:LIFE:RIGHTS (0.8.3.2)
            const std::string spec = u8(next());
            std::vector<std::string> f;
            for (size_t p = 0; p <= spec.size();) {
                const size_t q = spec.find(':', p);
                f.push_back(spec.substr(p, q == std::string::npos ? std::string::npos : q - p));
                if (q == std::string::npos) break;
                p = q + 1;
            }
            if (f.size() != 4) return usage(), 2;
            pm::remote::GuestAccount g;
            g.id = pm::remote::randomHex(8);
            g.name = f[0];
            g.pwHash = pm::remote::hashPassword(pm::remote::guestPasswordKey(f[1]));
            g.once = f[2] == "once";
            g.expires = static_cast<long long>(time(nullptr)) + (g.once ? 24 * 3600 : atoll(f[2].c_str()));
            g.viewOnly = f[3].find("view") != std::string::npos;
            g.files = f[3].find("files") != std::string::npos;
            guests.push_back(std::move(g));
        } else if (a == L"--guest-drop") {
            const std::string spec = u8(next());
            const size_t c = spec.rfind(':');
            if (c == std::string::npos) return usage(), 2;
            guestDrop = spec.substr(0, c), guestDropSec = atoi(spec.substr(c + 1).c_str());
        }
        else if (a == L"--fixed-approval") o.fixedNeedApproval = true;
        else if (a == L"--policy-fixed" || a == L"--policy-guest" || a == L"--policy-temp") {  // 0.8.3.6
            const std::wstring which = a;
            const std::wstring v = next();
            pm::remote::ApprovalPolicy pol{};
            std::string narrow;
            for (wchar_t c : v) narrow += static_cast<char>(c);
            if (!pm::remote::parsePolicy(narrow, pol)) return usage(), 2;
            (which == L"--policy-fixed" ? o.policies.fixed : which == L"--policy-guest" ? o.policies.guest : o.policies.temp) = pol;
        } else if (a == L"--approve-remember") approveRemember = true;
        else if (a == L"--funnel") funnelPort = _wtoi(next());
        else if (a == L"--fixed-tunnel") fixedTunnel = true;
        else if (a == L"--tunnel-no-cc") o.tunnelCongestion = false;
        else if (a == L"--mute-pc") o.mutePc = true;
        else if (a == L"--keys-file") {  // 0.8.4 自訂快捷鍵: the list is read from / saved to this file (the app keeps it in settings.ini)
            keysFile = next();
            std::ifstream in(keysFile);
            std::stringstream ss;
            ss << in.rdbuf();
            o.customKeys = ss.str();
        }
        else if (a == L"--synthetic-audio") o.audioSynthetic = u8(next());  // music | loud | quiet: silent test source
        else if (a == L"--no-agc") o.audioAgc = false;               // 0.8.3 A / B
        else if (a == L"--no-volume-comp") o.audioVolumeComp = false;  // 0.8.3 A / B
        else if (a == L"--tunnel-test-kbps") o.tunnelTestKbps = _wtoi(next());
        else if (a == L"--cloudflared") cloudflared = next();
        else if (a == L"--stamp") o.stamp = true;
        else if (a == L"--no-stamp") o.stamp = false;
        else if (a == L"--lang") o.lang = _wtoi(next());
        else if (a == L"--no-playout-delay") o.playoutDelayZero = false;
        else if (a == L"--no-cursor") o.drawCursor = false;
        else if (a == L"--duration") duration = _wtof(next());
        else if (a == L"--log") logFile = next();
        else if (a == L"--snapdir") o.snapDir = next();
        else if (a == L"--cpu-path") o.cpuPath = true;
        else if (a == L"--no-skip-static") o.skipStatic = false;
        else if (a == L"--static-refresh") o.staticRefreshMs = _wtoi(next());
        else if (a == L"--vbv-frames") o.vbvFrames = std::max(0, _wtoi(next()));
        else if (a == L"--inline-send") o.inlineSend = true;
        else if (a == L"--rc") o.vbr = std::wstring(next()) == L"vbr";
        else if (a == L"--tail-frames") o.tailFrames = std::max(0, _wtoi(next()));
        else if (a == L"--rtp-payload") o.rtpPayload = _wtoi(next());
        else if (a == L"--scene") o.scene = u8(next());
        else if (a == L"--test-cursor-cycle") o.testCursorCycle = true;  // 0.8.4: the stub pointer's shapes in turn
        else if (a == L"--stdin-quit") {
            // Test drivers: closing our stdin pipe = Ctrl+C.
            std::thread([] {
                char b[256];
                DWORD n = 0;
                while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), b, sizeof(b), &n, nullptr) && n > 0) {
                }
                g_quit = true;
            }).detach();
        } else {
            usage();
            return a == L"--help" || a == L"-h" ? 0 : 2;
        }
    }
    if (testWindow && !o.inject) {
        fputs("--test-window needs --inject\n", stderr);
        return 2;
    }
    if (o.inject && !testWindow) fputs("*** --inject without --test-window: the phone really controls this PC ***\n", stderr);
    if (!logFile.empty()) {
        FILE* f = _wfopen(logFile.c_str(), L"ab");
        if (f) fclose(f);
        static FILE* lf = _wfopen(logFile.c_str(), L"ab");
        g_log = [](const std::string& line) {
            if (lf) fputs(line.c_str(), lf), fflush(lf);
        };
        o.log = [](const std::string& line) {
            std::lock_guard<std::mutex> lk(g_logMu);
            if (lf) fputs(line.c_str(), lf), fflush(lf);
        };
    }
    SetConsoleCtrlHandler(onCtrl, TRUE);

    // Test window on the streamed monitor; injected input only lands in it.
    std::thread tw;
    POINT cursorAtStart{};
    GetCursorPos(&cursorAtStart);
    if (testWindow) {
        const auto mons = pm::remote::monitors();
        const int mi = std::clamp(o.monitor, 0, std::max(0, int(mons.size()) - 1));
        RECT mon{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        if (!mons.empty()) mon = {mons[size_t(mi)].x, mons[size_t(mi)].y, mons[size_t(mi)].x + mons[size_t(mi)].w,
                                  mons[size_t(mi)].y + mons[size_t(mi)].h};
        std::atomic<bool> ready{false};
        tw = std::thread(testWindowThread, mon, &ready);
        while (!ready) Sleep(10);
        o.injectMoveGuard = [](int x, int y) { return insideTest(x, y); };
        o.injectGuard = [](bool keyboard) {
            if (keyboard) return GetForegroundWindow() == g_testWnd;
            POINT p{};
            GetCursorPos(&p);
            HWND under = WindowFromPoint(p);
            return insideTest(p.x, p.y) && under && GetAncestor(under, GA_ROOT) == g_testWnd;
        };
    }
    std::thread toneT;
    if (tone) toneT = std::thread(toneThread, Tone{}, GetTickCount64());
    std::vector<std::thread> toneTs;
    for (const Tone& t : tones) toneTs.emplace_back(toneThread, t, GetTickCount64());

    pm::remote::Host host;
    std::atomic<int> requests{0};
    std::atomic<ULONGLONG> activeAt{0};
    // 0.8.3.6: the 記住 list (device + scope), in memory: --approve-remember adds to it; 拒絕 / 中斷 drop the device.
    static std::mutex remMu;
    static std::set<std::string> remList;
    o.remembered = [](const std::string& device, const std::string& scope) {
        std::lock_guard<std::mutex> lk(remMu);
        const bool on = remList.count(device + "\t" + scope) > 0;
        slog("remembered? device %s scope %s -> %s", device.substr(0, 8).c_str(), scope.rfind("guest:", 0) == 0 ? "guest" : scope.c_str(), on ? "yes" : "no");
        return on;
    };
    o.onRequest = [&](const pm::remote::Peer& p) {
        const int n = ++requests;
        const bool allow = !(denyFirst && n == 1);
        slog("approval: request #%d from %s (%s%s, device %s) -> will %s in %d ms", n, p.label.c_str(), p.ip.c_str(),
             p.tailscale ? ", Tailscale" : "", p.device.substr(0, 8).c_str(), allow ? "ALLOW" : "DENY", approveAfterMs);
        const std::string key = p.device + "\t" + pm::remote::approvalScope(p.fixed, p.tunnel, p.guestId);
        std::thread([&host, id = p.session, allow, approveAfterMs, key, rem = approveRemember && allow] {
            Sleep(DWORD(std::max(0, approveAfterMs)));
            {
                std::lock_guard<std::mutex> lk(remMu);
                if (rem) remList.insert(key);
                if (!allow)
                    for (auto it = remList.begin(); it != remList.end();) it = it->rfind(key.substr(0, key.find('\t') + 1), 0) == 0 ? remList.erase(it) : std::next(it);
            }
            host.approve(id, allow, rem);
        }).detach();
    };
    o.onPrivacy = [](bool on, const std::string& why) { slog("event: PRIVACY %s (%s)", on ? "on" : "off", why.c_str()); };  // 0.8.4
    o.onDownload = [](const pm::remote::Peer& p, const std::string& what, uint64_t bytes, const std::string& kind) {
        slog("event: DOWNLOAD %s %s (%llu bytes) by %s", kind.c_str(), what.c_str(), (unsigned long long)bytes, p.label.c_str());
    };
    o.onActive = [&](const pm::remote::Peer& p) {
        activeAt = GetTickCount64();
        slog("event: ACTIVE %s (%s) session %s%s%s policy %s admit %s", p.label.c_str(), p.ip.c_str(), p.session.substr(0, 6).c_str(),
             p.guest.empty() ? "" : " guest ", p.guest.c_str(), p.policy.c_str(), p.admit.c_str());
        if (!guestDrop.empty() && p.guest == guestDrop) {  // tests: the account is deleted while its phone is connected
            std::thread([&host, &guests, name = guestDrop, sec = guestDropSec] {
                Sleep(DWORD(std::max(0, sec) * 1000));
                std::vector<pm::remote::GuestAccount> left;
                for (const auto& g : guests)
                    if (g.name != name) left.push_back(g);
                slog("test: 臨時帳號 %s deleted now (--guest-drop)", name.c_str());
                host.setGuests(left);
            }).detach();
        }
    };
    o.onBlocked = [](const std::string& ip, int n) { slog("event: BLOCKED %s after %d wrong links", ip.c_str(), n); };
    o.onTrouble = [](const std::string& ip, const std::string& why) { slog("event: TROUBLE %s: %s", ip.c_str(), why.c_str()); };
    if (!fixedPw.empty()) {
        const ULONGLONG h0 = GetTickCount64();
        o.fixedPasswordHash = pm::remote::hashPassword(fixedPw);
        o.fixedSecret = pm::remote::randomHex(32);
        slog("fixed: password hashed in %llu ms: %s...", GetTickCount64() - h0, o.fixedPasswordHash.substr(0, 24).c_str());
    }
    o.onLogin = [](const std::string& ip, bool ok, int fails, bool locked) {
        slog("event: LOGIN %s %s (fails %d%s)", ip.c_str(), ok ? "OK" : "WRONG", fails, locked ? ", LOCKED" : "");
    };
    o.guests = guests;
    o.onKeysChanged = [keysFile](const std::string& json, const std::string& label) {  // 0.8.4: what the app writes to settings.ini
        slog("event: KEYS saved by %s: %s", label.c_str(), json.c_str());
        if (!keysFile.empty()) std::ofstream(keysFile, std::ios::binary | std::ios::trunc) << json;
    };
    o.onGuestLogin = [](const std::string& ip, const std::string& user, bool ok, int fails, bool locked) {
        slog("event: GUEST LOGIN %s user %s %s (fails %d%s)", ip.c_str(), user.c_str(), ok ? "OK" : "WRONG", fails, locked ? ", LOCKED" : "");
    };
    std::string tunnelUrl;
    std::mutex tunnelMu;
    o.onTunnelLink = [&](const std::string& path) {
        std::lock_guard<std::mutex> lk(tunnelMu);
        slog("tunnel: new one-time link %s%s", tunnelUrl.c_str(), path.c_str());
    };
    o.onEnded = [&](const pm::remote::Peer& p, const std::string& why) {
        if (!p.guest.empty()) slog("event: guest %s session ended", p.guest.c_str());

        slog("event: ENDED %s (%s) session %s: %s", p.label.c_str(), p.ip.c_str(), p.session.substr(0, 6).c_str(), why.c_str());
    };
    if (!host.start(o)) {
        fputs("start failed\n", stderr);
        g_quit = true;
        if (g_testWnd) PostMessageW(g_testWnd, WM_APP, 0, 0);
        if (tw.joinable()) tw.join();
        if (toneT.joinable()) toneT.join();
        for (auto& t : toneTs)
            if (t.joinable()) t.join();
        return 1;
    }
    const std::string url = host.url(), ts = host.tailscaleUrl();
    printf("\n自在投影 遠端操控（測試程式） %s\n",
           o.inject ? (testWindow ? "[--inject --test-window：只會操作測試視窗]" : "*** 真的會操控滑鼠鍵盤 (--inject) ***")
                    : "[dry-run：只記錄觸控事件，不會動滑鼠]");
    printf("同一個 Wi-Fi：\n  %s\n", url.c_str());
    if (!ts.empty()) printf("外面（Tailscale）：\n  %s\n", ts.c_str());
    printf("\n");
    printQr(url);
    printf("\nCtrl+C 結束\n");
    fflush(stdout);
    slog("urls: lan %s tailscale %s", url.c_str(), ts.empty() ? "-" : ts.c_str());
    pm::remote::QuickTunnel qt;
    if (tunnel) {
        if (cloudflared.empty()) {
            wchar_t me[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, me, MAX_PATH);
            std::wstring dir = me;
            dir = dir.substr(0, dir.find_last_of(L"\\/"));
            cloudflared = pm::remote::QuickTunnel::findExe(dir);
        }
        host.setTunnel(true);
        const bool ok = qt.start(cloudflared, host.port(),
                                 [&](const std::string& u) {
                                     {
                                         std::lock_guard<std::mutex> lk(tunnelMu);
                                         tunnelUrl = u;
                                     }
                                     slog("tunnel: ready %s link %s%s passcode %s", u.c_str(), u.c_str(), host.tunnelLinkPath().c_str(),
                                          host.tunnelPasscode().c_str());
                                 },
                                 [&](const std::string& why) { slog("tunnel: ended: %s", why.c_str()); });
        if (!ok) slog("tunnel: could not start cloudflared");
    }
    pm::remote::QuickTunnel fqt;
    bool funnelOn = false;
    if (!fixedPw.empty()) {
        host.setFixed(true);
        slog("fixed: origin %s", host.fixedOrigin().c_str());
        if (funnelPort > 0 && funnelPort != 443) {
            std::string u, err;
            funnelOn = pm::remote::TailscaleFunnel::start(funnelPort, host.fixedOrigin(), u, err);
            if (funnelOn) slog("fixed: ready %s", u.c_str());
            else slog("fixed: funnel failed: %s", err.c_str());
        } else if (fixedTunnel) {
            wchar_t me[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, me, MAX_PATH);
            std::wstring dir = me;
            dir = dir.substr(0, dir.find_last_of(L"\\/"));
            const std::wstring exe = cloudflared.empty() ? pm::remote::QuickTunnel::findExe(dir) : cloudflared;
            fqt.start(exe, host.port(), [](const std::string& u) { slog("fixed: ready %s/ (cloudflared)", u.c_str()); },
                      [](const std::string& why) { slog("fixed: cloudflared ended: %s", why.c_str()); }, "127.0.0.2");
        }
    }
    const ULONGLONG t0 = GetTickCount64();
    bool kicked = false;
    while (!g_quit && (duration <= 0 || double(GetTickCount64() - t0) < duration * 1000)) {
        Sleep(100);
        if (kickAfter > 0 && !kicked && activeAt && double(GetTickCount64() - activeAt) > kickAfter * 1000) {
            kicked = true;
            slog("kick: ending the session from the PC (--kick-after)");
            host.kick("spike --kick-after");
        }
    }
    qt.stop();
    host.setTunnel(false);
    if (funnelOn) {
        std::string err;
        const bool off = pm::remote::TailscaleFunnel::stop(funnelPort, host.fixedOrigin(), err);  // 0.8.3.5: only ours
        slog("fixed: funnel %d %s %s", funnelPort, off ? "OFF" : "still on!", err.c_str());
    }
    fqt.stop();
    host.setFixed(false);
    host.stop();
    g_quit = true;
    if (testWindow) {
        slog("testwin: totals left clicks %d, right clicks %d, wheel %d, text changes %d", int(g_twClicks), int(g_twRight),
             int(g_twWheel), int(g_twChars));
        POINT p{};
        GetCursorPos(&p);
        const bool inside = insideTest(p.x, p.y);
        PostMessageW(g_testWnd, WM_APP, 0, 0);

        if (tw.joinable()) tw.join();
        // Put the pointer back where it was, unless someone moved it out of the test window meanwhile.
        if (inside) SetCursorPos(cursorAtStart.x, cursorAtStart.y), slog("testwin: pointer restored to %ld,%ld", cursorAtStart.x, cursorAtStart.y);
    }
    if (toneT.joinable()) toneT.join();
    for (auto& t : toneTs)
        if (t.joinable()) t.join();
    fputs("stopped\n", stderr);
    return 0;
}
