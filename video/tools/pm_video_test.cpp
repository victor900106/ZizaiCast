// pm_video_test <file.h264|file.h265> [options]
// Splits an Annex-B elementary stream into access units and feeds them
// through pm::VideoSink into a pm::VideoWindow at a fixed rate (default 60 fps),
// then shows the idle screen (onReset) and prints decode/latency statistics.
//   --fps N          feed rate (0 = as fast as possible: drop-policy stress test)
//   --hold MS        keep the last picture this long before onReset (1500)
//   --idle MS        show the idle screen this long before closing (1500)
//   --loops N        play the file N times back to back (timestamps continue)
//   --minutes M      loop the file for M minutes (soak test; overrides --loops)
//   --report S       every S seconds print frames + process private bytes,
//                    handles, GDI/USER objects (default 60 with --minutes)
//   --size W,H       client size in DIPs (1280,720)
//   --demo-ui        scripted tour of the status UI (times in ms after start):
//                    0 idle + two check boxes, 3000 setConnecting, 5000 frames,
//                    6000 saveSnapshot (UI thread) + toast, 8000 PIN, 9500 PIN off,
//                    10000 paused, 11200 resumed, 12000 reset -> idle for --idle ms.
//   --demo-ui2       tour of themes, mascot, device frame, rotation, REC, dim
//                    (540x960; plays the file twice from 7000 ms; timeline in
//                    runDemo2() below, screenshots via testdata/capture.ps1)
// Frame tap:
//   --tap            install a frame tap: counts pictures, checks size / pts
//                    order, prints the render-thread cost (stats.tapAvg/P95)
//   --tap-dump PNG   also convert the last tapped picture to RGB on the CPU,
//                    write it to PNG, saveSnapshot() the same picture to
//                    PNG_snapshot.png and compare the two (mean |diff|, PSNR)
//   --tap-slow MS    the tap sleeps MS per picture (back-pressure test)
// A/V sync (synthesised timestamps; the real ones come from the core):
//   --sync LAT       setSyncMode(true, LAT); frame i gets ntpLocalNs =
//                    start + i/fps (+ --lead) on the --clock clock
//   --clock utc|qpc  clock of the synthesised ntpLocalNs (utc = today's core)
//   --lead MS        timestamp lead over the nominal feed time (0)
//   --jitter MS      deliver each frame 0..MS late (random, in order) while
//                    its timestamp stays on the regular grid (network jitter)
//   --lat-sweep A,B  alternate the audio latency between A and B every 2 s
//   --ntp0-every N   every Nth frame carries ntpLocalNs = 0 (ASAP fallback)
// Robustness:
//   --test-at T:C[:A][,T:C[:A]...]  at T ms after the window appears post the
//                    test hook (lParam A): C=0 simulated device removal
//                    (A=1: seen while uploading the next decoded picture,
//                    i.e. from inside the decoder's output callback),
//                    1 power-saving GPU, 2 WARP, 3 automatic GPU choice;
//                    fault injection (0.6.2 watchdog): 20 the decoder stops
//                    returning pictures (A = more hardware instances that do
//                    too), 21 Present reports DXGI_STATUS_OCCLUDED for A ms,
//                    22 Present calls swallowed (stuck swap chain), 23 render
//                    thread blocked A ms (stalled Present / GPU), 24 drop the
//                    next A non-IDR AUs, 26 display change after which the
//                    power-saving GPU is preferred
//   --display-change-at T[,T...]  post a real WM_DISPLAYCHANGE to the window
//   --fullscreen-check  self-checking (PASS/FAIL, exit code = failures), on a
//                    HIDDEN window (never on screen): F11 covers the nearest
//                    monitor; after its rect is knocked off (a stale rcMonitor
//                    after a resolution / scaling change) WM_DISPLAYCHANGE and
//                    WM_DPICHANGED fit it to the monitor again
//   --cover-at T:MS[,...]  a topmost opaque layered window over the test
//                    window for MS ms (only with the off-screen window: it
//                    is never on a monitor)
//   --offscreen      PM_VIDEO_OFFSCREEN=1: the window is far off the desktop
//   --watchdog off   PM_VIDEO_WATCHDOG=0 (0.6.1 behaviour, for before/after)
//   --freeze-report  sample stats every 50 ms; print every interval in which
//                    AUs arrived but no picture was presented (> 250 ms), the
//                    longest one, and pictures presented after the first fault
//   --churn N [--alt file2]  N cycles of onCodec / setConnecting / showPin /
//                    frames / showToast / showPin("") / onReset (alternating
//                    with file2's codec if given), resources printed every 25
//   --churn-loss K   also simulate a device loss every K churn cycles
//   --decoder-cycles N  open/close the H.264 and HEVC decoder MFTs N times
//                    (no window) and print handle counts per object type
// Android sources (the stream file is ignored):
//   --android        540x960 self-checking test (PASS/FAIL lines, exit code =
//                    failures): idle with two hint lines + check boxes + help
//                    link (hover, click -> callback on the UI thread); BGRA
//                    pictures 720x1280 (padded stride) at 30 fps: snapshot ==
//                    submitted pixels, rotation 90 + mirror snapshot ==
//                    transpose; synthetic mouse / wheel / key messages with
//                    rotation 90 + mirror checked against the expected
//                    picture coordinates; device frame + framed snapshot;
//                    frame tap (BGRA -> NV12) vs the pattern; reset -> idle.
//                    Screenshot times for capture.ps1: 1.3 idle, 1.9 link
//                    hover, 5.4 framed BGRA picture, 6.6 idle again.
// Magnifier / filters / freeze / translation overlay (0.7):
//   --magnifier DIR  540x960 off-screen self-checking test (PASS/FAIL lines,
//                    exit code = failures) fed with --png FILE (a phone
//                    screenshot, e.g. translate/testdata/render.sh output;
//                    default the synthetic pattern) + a moving bar: zoomAt /
//                    indicator / badge / overview, snapshot + grabPicture
//                    unzoomed, pointer mapping, drag / Ctrl+wheel / arrow
//                    input, 8x / 1x clamps, the 4 filters, freeze (picture
//                    still, frame tap continues), text overlay (+ original,
//                    zoomed, mirrored), busy card, region select (also with
//                    rotation 90 + mirror + zoom; Esc), device frame + zoom,
//                    onReset; window shots mag_*.png in DIR
//   --freeze-stream DIR  the stream file decoded (DXVA / NV12) for ~7 s:
//                    freeze (window still, frame tap continues), grabPicture,
//                    zoom 3x + contrast filter shot, unfreeze
// Mascot:
//   --mascot-tour DIR  off-screen window (never on the desktop): every 投投
//                    state (4 themes, hover, click, beam pulse, connecting,
//                    paused, landscape 1280x720, narrow 400x800, 3440x1440)
//                    saved to DIR with saveWindowShot, then process CPU while
//                    the idle screen animates and once it has settled (~80 s)
//   --anim-bench     off-screen: frames drawn per second (+ process CPU) idle
//                    animating / animations off / hidden / minimised / settled,
//                    a static live picture with and without the toolbar,
//                    recording, live video (the stream file is ignored)
//   --anim-shots DIR off-screen: frame strips of the hover / press / focus /
//                    reaction transitions (window shots, names = ms after the trigger)
//   --bgra-bench W,H BGRA pictures W x H at --fps (60) for --seconds (5),
//                    moving pattern; prints call / upload / present / tap
//                    cost and process CPU (add --tap for the frame tap)
#include <windows.h>
#include <psapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <tlhelp32.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <mutex>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include "../src/annexb.h"
#include "../src/mf_decoder.h"
#include "../src/ui_art.h"

#include <mfapi.h>
#include "pm/video_window.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "psapi.lib")

namespace {

// Runs closures on the UI thread (the thread that pumps the window's messages),
// the way the app calls the UI API.
constexpr UINT WM_RUN_ON_UI = WM_APP + 0x3a0;
DWORD g_uiThread = 0;
std::mutex g_uiMutex;
std::deque<std::function<void()>> g_uiQueue;

void runOnUi(std::function<void()> fn) {
    {
        std::lock_guard lk(g_uiMutex);
        g_uiQueue.push_back(std::move(fn));
    }
    PostThreadMessageW(g_uiThread, WM_RUN_ON_UI, 0, 0);
}

int messageLoop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!msg.hwnd && msg.message == WM_RUN_ON_UI) {
            std::deque<std::function<void()>> q;
            {
                std::lock_guard lk(g_uiMutex);
                q.swap(g_uiQueue);
            }
            for (auto& f : q) f();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

struct Stream {
    std::vector<uint8_t> data;
    std::vector<std::pair<size_t, size_t>> aus;
    pm::VideoCodec codec = pm::VideoCodec::H264;
};

bool load(const wchar_t* path, Stream& s) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open %ls\n", path);
        return false;
    }
    s.data.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const wchar_t* ext = wcsrchr(path, L'.');
    const bool hevc = ext && (!_wcsicmp(ext, L".h265") || !_wcsicmp(ext, L".hevc") || !_wcsicmp(ext, L".265"));
    s.codec = hevc ? pm::VideoCodec::H265 : pm::VideoCodec::H264;
    s.aus = pm::annexb::splitAccessUnits(s.codec, s.data.data(), s.data.size());
    return true;
}

int64_t qpcNs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return c.QuadPart / f.QuadPart * 1000000000LL + c.QuadPart % f.QuadPart * 1000000000LL / f.QuadPart;
}

int64_t utcNs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const int64_t t = static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    return (t - 116444736000000000LL) * 100;
}

void printResources(const char* tag, double tSec, const pm::VideoWindow& win) {
    PROCESS_MEMORY_COUNTERS_EX pmc{sizeof(pmc)};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    const DWORD gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    DWORD threads = 0;
    if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{sizeof(te)};
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
            if (te.th32OwnerProcessID == GetCurrentProcessId()) ++threads;
        CloseHandle(snap);
    }
    auto s = win.stats();
    std::printf("%s t=%7.1fs private=%7.1f MB ws=%7.1f MB handles=%5lu threads=%3lu gdi=%4lu user=%4lu in=%lld presented=%lld "
                "dropped=%lld recoveries=%lld\n",
                tag, tSec, pmc.PrivateUsage / 1048576.0, pmc.WorkingSetSize / 1048576.0, handles, threads, gdi, user,
                s.framesIn, s.framesPresented, s.framesDropped, s.deviceRecoveries);
    std::fflush(stdout);
}


// Handle count per object type (leak hunting): NtQuerySystemInformation
// (SystemExtendedHandleInformation) filtered to this process, type names via
// NtQueryObject(ObjectTypeInformation) - safe, it never touches the object.
std::map<std::wstring, int> handleTypes() {
    struct Entry {
        PVOID object;
        ULONG_PTR pid;
        ULONG_PTR handle;
        ULONG access;
        USHORT backTrace;
        USHORT typeIndex;
        ULONG attributes;
        ULONG reserved;
    };
    struct Info {
        ULONG_PTR count;
        ULONG_PTR reserved;
        Entry handles[1];
    };
    struct TypeInfo {
        USHORT length, maxLength;
        PWSTR name;
        ULONG pad[32];
    };
    using QSI = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);
    using QO = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto qsi = reinterpret_cast<QSI>(GetProcAddress(nt, "NtQuerySystemInformation"));
    auto qo = reinterpret_cast<QO>(GetProcAddress(nt, "NtQueryObject"));
    std::map<std::wstring, int> out;
    if (!qsi || !qo) return out;
    std::vector<uint8_t> buf(1 << 24);
    ULONG need = 0;
    LONG st;
    while ((st = qsi(64, buf.data(), static_cast<ULONG>(buf.size()), &need)) == static_cast<LONG>(0xC0000004))
        buf.resize(buf.size() * 2);
    if (st < 0) return out;
    auto* info = reinterpret_cast<Info*>(buf.data());
    std::map<USHORT, std::wstring> names;
    const ULONG_PTR me = GetCurrentProcessId();
    for (ULONG_PTR i = 0; i < info->count; ++i) {
        const Entry& e = info->handles[i];
        if (e.pid != me) continue;
        auto it = names.find(e.typeIndex);
        if (it == names.end()) {
            alignas(8) uint8_t tb[1024]{};
            std::wstring n = L"type#" + std::to_wstring(e.typeIndex);
            if (qo(reinterpret_cast<HANDLE>(e.handle), 2, tb, sizeof(tb), nullptr) >= 0) {
                auto* ti = reinterpret_cast<TypeInfo*>(tb);
                if (ti->name) n.assign(ti->name, ti->length / 2);
            }
            it = names.emplace(e.typeIndex, n).first;
        }
        out[it->second]++;
    }
    return out;
}

// ---- frame tap check helpers ----
struct TapCapture {
    std::mutex m;
    long long count = 0, sizeChanges = 0, ptsBackwards = 0;
    int w = 0, h = 0, stride = 0;
    uint64_t lastPts = 0, firstPts = 0;
    std::vector<uint8_t> last;  // last picture (NV12, stride = w)
    bool onRenderThread = true;
};

// NV12 (BT.709 video range, the test streams) -> BGRX with bilinear chroma at
// pixel centres, like the renderer's sampler.
std::vector<uint8_t> nv12ToBgrx(const uint8_t* p, int w, int h, int stride) {
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
    const uint8_t* uv = p + static_cast<size_t>(stride) * h;
    const int cw = w / 2, ch = h / 2;
    auto chroma = [&](float x, float y, int c) {
        x = std::clamp(x, 0.f, cw - 1.f);
        y = std::clamp(y, 0.f, ch - 1.f);
        const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
        const int x1 = std::min(x0 + 1, cw - 1), y1 = std::min(y0 + 1, ch - 1);
        const float fx = x - x0, fy = y - y0;
        auto at = [&](int xx, int yy) { return static_cast<float>(uv[static_cast<size_t>(yy) * stride + xx * 2 + c]); };
        return (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float Y = (p[static_cast<size_t>(y) * stride + x] / 255.f - 16.f / 255) * (255.f / 219);
            const float cx = (x + 0.5f) / 2 - 0.5f, cy = (y + 0.5f) / 2 - 0.5f;
            const float cb = (chroma(cx, cy, 0) / 255.f - 0.5f) * (255.f / 224);
            const float cr = (chroma(cx, cy, 1) / 255.f - 0.5f) * (255.f / 224);
            const float r = Y + 1.5748f * cr, g = Y - 0.187324f * cb - 0.468124f * cr, b = Y + 1.8556f * cb;
            uint8_t* d = out.data() + (static_cast<size_t>(y) * w + x) * 4;
            d[0] = static_cast<uint8_t>(std::clamp(b * 255 + 0.5f, 0.f, 255.f));
            d[1] = static_cast<uint8_t>(std::clamp(g * 255 + 0.5f, 0.f, 255.f));
            d[2] = static_cast<uint8_t>(std::clamp(r * 255 + 0.5f, 0.f, 255.f));
            d[3] = 255;
        }
    return out;
}

bool readPng(const std::wstring& path, std::vector<uint8_t>& bgrx, UINT& w, UINT& h) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)) ||
        FAILED(conv->GetSize(&w, &h)))
        return false;
    bgrx.resize(static_cast<size_t>(w) * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(bgrx.size()), bgrx.data()));
}

void printHandleDiff(const std::map<std::wstring, int>& a, const std::map<std::wstring, int>& b) {
    std::map<std::wstring, int> all = a;
    for (auto& [k, v] : b) all.emplace(k, 0);
    std::printf("handle types (start -> end):");
    for (auto& [k, v] : all) {
        const int x = a.count(k) ? a.at(k) : 0, y = b.count(k) ? b.at(k) : 0;
        std::printf(" %ls %d->%d%s", k.c_str(), x, y, x != y ? "*" : "");
    }
    std::printf("\n");
    std::fflush(stdout);
}
}  // namespace

// --demo-ui2: themes, mascot interaction, device frame, rotation, REC, dim.
// Times in ms after the window appeared (screenshot times for capture.ps1 in
// parentheses).
template <class At, class Feed>
void runDemo2(pm::VideoWindow& win, At at, Feed feed) {
    using Theme = pm::VideoWindow::Theme;
    runOnUi([&] {
        win.setIdleOptions({{1, L"開機時自動啟動", true}, {2, L"連線時需要輸入 PIN 碼", false}},
                           [](int, bool) {});
    });
    auto step = [](const char* what) {
        std::printf("[demo2] %s\n", what);
        std::fflush(stdout);
    };
    // Mouse at a client position given as fractions of the client size.
    auto mouse = [&](float fx, float fy, bool click) {
        HWND h = win.hwnd();
        RECT r{};
        if (!h || !GetClientRect(h, &r)) return;
        const LPARAM lp = MAKELPARAM(static_cast<int>(r.right * fx), static_cast<int>(r.bottom * fy));
        PostMessageW(h, WM_MOUSEMOVE, 0, lp);
        if (click) {
            PostMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, lp);
            PostMessageW(h, WM_LBUTTONUP, 0, lp);
        }
    };
    step("idle, Sakura");                       // (1000)
    at(1200); win.setTheme(Theme::Mint);        // (2000)
    at(2200); win.setTheme(Theme::Night);       // (3000)
    at(3200); win.setTheme(Theme::MilkTea);     // (4000)
    at(4200); win.setTheme(Theme::Sakura);
    at(4400); step("hover + click the mascot"); mouse(0.42f, 0.86f, false);
    at(4700); mouse(0.42f, 0.86f, true);        // (5050 mid-reaction)
    at(6000); mouse(0.42f, 0.86f, true);        // second line
    at(7400); mouse(0.95f, 0.05f, false);
    step("setConnecting: happy hop");
    win.setConnecting(L"Victor 的 iPhone");     // (7750 hop)
    at(9000); step("frames, device frame on");
    win.setDeviceFrame(true);
    std::thread frames([&] { feed(2); });        // 9.0-11 portrait, 11-13 landscape, 13-15 portrait, ...
    at(13000); step("frame off, rotation 90");  // (10200 framed portrait, 12200 framed landscape)
    win.setDeviceFrame(false);
    win.setRotation(1);                          // (14200)
    at(14600); step("rotation 0, recording");
    win.setRotation(0);
    win.setRecording(true);                      // (17200 REC 00:02)
    at(17400);
    runOnUi([&] {
        win.setMirrored(true);
        win.setRotation(1);
        const bool a = win.saveSnapshot(L"demo2_snapshot_rot90_mirror.png");
        win.setMirrored(false);
        win.setRotation(0);
        const bool b = win.saveSnapshotFramed(L"demo2_snapshot_framed.png");
        int w = 0, h = 0;
        win.desiredClientAspect(w, h);
        std::printf("[demo2] snapshots: rot90+mirror %s, framed %s; desiredClientAspect %dx%d\n", a ? "ok" : "FAILED",
                    b ? "ok" : "FAILED", w, h);
        std::fflush(stdout);
    });
    at(19000); win.setRecording(false);
    frames.join();                               // ends at 21000
    at(21300); step("dimmed (connection lost hold)");
    win.setDimmed(true);                         // (21900)
    at(23000); step("reset -> idle, paused, sleepy click");
    win.setDimmed(false);
    win.onReset();
    at(24000); win.onPaused(true);
    at(24500); mouse(0.42f, 0.86f, true);        // (24900)
    at(26000); win.onPaused(false);
    win.onReset();
}

// ---- --mascot-tour DIR: every mascot state, saved with saveWindowShot ----
// The window is off-screen (PM_VIDEO_OFFSCREEN), input is posted, shots come
// from the back buffer: nothing shows on the desktop.  Ends with process CPU
// while the idle screen animates and after it has settled (40 s).
double processCpuMs() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto ms = [](FILETIME f) { return (static_cast<double>(f.dwHighDateTime) * 4294967296.0 + f.dwLowDateTime) / 1e4; };
    return ms(k) + ms(u);
}

template <class At>
void runMascotTour(pm::VideoWindow& win, At at, const std::wstring& dir) {
    using Theme = pm::VideoWindow::Theme;
    CreateDirectoryW(dir.c_str(), nullptr);
    const UINT testMsg = RegisterWindowMessageW(L"PhoneMirror.Video.Test");
    HWND h = win.hwnd();
    SendMessageW(h, testMsg, 12, 1);  // posted mouse only
    auto shot = [&](const wchar_t* name) {
        const std::wstring p = dir + L"\\" + name + L".png";
        const bool ok = win.saveWindowShot(p);
        RECT r{};
        GetClientRect(h, &r);
        std::printf("[tour] %ls %ldx%ld %s\n", name, r.right, r.bottom, ok ? "ok" : "FAILED");
        std::fflush(stdout);
    };
    auto setClient = [&](int w, int hh, bool px = false) {
        const UINT dpi = GetDpiForWindow(h);
        RECT r{0, 0, px ? w : MulDiv(w, dpi, 96), px ? hh : MulDiv(hh, dpi, 96)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, dpi);
        SetWindowPos(h, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    };
    auto mascotPt = [&]() -> LPARAM {
        const LRESULT r = SendMessageW(h, testMsg, 13, 0);
        return r == -1 ? -1 : static_cast<LPARAM>(r);
    };
    auto move = [&](LPARAM lp) { PostMessageW(h, WM_MOUSEMOVE, 0, lp); };
    auto click = [&](LPARAM lp) {
        move(lp);
        PostMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, lp);
        PostMessageW(h, WM_LBUTTONUP, 0, lp);
    };
    const LPARAM away = MAKELPARAM(4, 4);
    runOnUi([&] {
        win.setIdleOptions({{1, L"開機時自動啟動", true}, {2, L"連線時需要輸入 PIN 碼", false}}, [](int, bool) {});
    });
    at(1300); shot(L"idle_sakura");
    win.setTheme(Theme::Mint);
    at(1700); shot(L"idle_mint");
    win.setTheme(Theme::Night);
    at(2100); shot(L"idle_night");
    win.setTheme(Theme::MilkTea);
    at(2500); shot(L"idle_milktea");
    win.setTheme(Theme::Sakura);
    at(2700);
    const LPARAM pt = mascotPt();
    std::printf("[tour] mascot point %s\n", pt == -1 ? "none" : "ok");
    move(pt);
    at(3100); shot(L"hover");
    click(pt);
    at(3330); shot(L"click_react_airborne");
    at(3650); shot(L"click_react_hearts");
    move(away);
    // idle beam pulse: the pulse comes every 6 s; take a strip of frames
    for (int i = 0; i < 12; ++i) {
        at(5000 + i * 500);
        wchar_t n[32];
        swprintf_s(n, L"idle_seq_%02d", i);
        shot(n);
    }
    win.setConnecting(L"Victor 的 iPhone");
    at(11400); shot(L"connect_intro");
    at(12600); shot(L"connecting");
    at(13000); shot(L"connecting2");
    win.onReset();
    win.onPaused(true);
    at(14600); shot(L"paused");
    click(mascotPt());
    at(14900); shot(L"paused_click");
    at(16400);
    win.onPaused(false);
    win.onReset();
    // layouts
    setClient(1280, 720);
    at(17400); shot(L"landscape_idle");
    win.setConnecting(L"Victor 的 iPhone");
    at(19200); shot(L"landscape_connecting");
    win.onReset();
    setClient(400, 800);
    at(20200); shot(L"narrow_idle");
    win.setTheme(Theme::Night);
    win.onPaused(true);
    at(21200); shot(L"narrow_paused_night");
    win.onPaused(false);
    win.onReset();
    win.setTheme(Theme::Sakura);
    setClient(3440, 1440, true);  // pixels: a 3440x1440 monitor in fullscreen
    at(22400); shot(L"ultrawide_idle");
    setClient(540, 960);
    // CPU: animating (fresh activity) vs settled (> 40 s after it)
    move(away);
    at(23000);
    double c0 = processCpuMs();
    auto w0 = std::chrono::steady_clock::now();
    at(33000);
    double c1 = processCpuMs();
    double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
    std::printf("[tour] CPU animating idle: %.2f %% of one core (%.0f ms CPU in %.0f ms)\n", 100 * (c1 - c0) / wall,
                c1 - c0, wall);
    at(66000);
    shot(L"settled");
    c0 = processCpuMs();
    w0 = std::chrono::steady_clock::now();
    at(76000);
    c1 = processCpuMs();
    wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
    std::printf("[tour] CPU settled idle: %.2f %% of one core (%.0f ms CPU in %.0f ms)\n", 100 * (c1 - c0) / wall,
                c1 - c0, wall);
    std::fflush(stdout);
}

// ---- --fullscreen-check: fullscreen re-fit after display changes ----
// The window is hidden first and never shown again (fullscreen is not
// toggled off: SetWindowPlacement would show it).
int runFullscreenCheck(pm::VideoWindow& win) {
    int failures = 0;
    auto expect = [&](bool ok, const char* what, const RECT& r) {
        if (!ok) ++failures;
        std::printf("[fullscreen] %s %s: window %ld,%ld %ldx%ld\n", ok ? "PASS" : "FAIL", what, r.left, r.top,
                    r.right - r.left, r.bottom - r.top);
        std::fflush(stdout);
    };
    auto onUi = [](std::function<void()> fn) {
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        runOnUi([&] {
            fn();
            SetEvent(done);
        });
        WaitForSingleObject(done, 5000);
        CloseHandle(done);
    };
    auto wait = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
    HWND hwnd = win.hwnd();
    wait(500);
    bool hidden = false;
    onUi([&] {
        ShowWindow(hwnd, SW_HIDE);
        hidden = !IsWindowVisible(hwnd);
    });
    if (!hidden) {
        std::printf("[fullscreen] FAIL could not hide the window (check skipped: nothing goes on screen)\n");
        return 1;
    }
    auto state = [&](RECT& wr, RECT& mon) {
        onUi([&] {
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            GetWindowRect(hwnd, &wr);
            mon = mi.rcMonitor;
        });
    };
    // Knocks the window off the monitor rect (still on the same monitor).
    auto knock = [&](RECT& to) {
        onUi([&] {
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            to = {mi.rcMonitor.left + 40, mi.rcMonitor.top + 30, mi.rcMonitor.left + 40 + 700, mi.rcMonitor.top + 30 + 500};
            SetWindowPos(hwnd, nullptr, to.left, to.top, 700, 500, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        });
    };
    RECT wr{}, mon{}, off{};
    PostMessageW(hwnd, WM_KEYDOWN, VK_F11, 0);
    wait(300);
    state(wr, mon);
    expect(EqualRect(&wr, &mon) != 0, "F11 covers the nearest monitor", wr);
    knock(off);
    wait(100);
    state(wr, mon);
    expect(EqualRect(&wr, &off) != 0, "rect knocked off the monitor (stale rcMonitor)", wr);
    onUi([&] {
        SendMessageW(hwnd, WM_DISPLAYCHANGE, 32,
                     MAKELPARAM(GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)));
    });
    wait(100);
    state(wr, mon);
    expect(EqualRect(&wr, &mon) != 0, "WM_DISPLAYCHANGE re-fits it to the monitor", wr);
    knock(off);
    wait(100);
    onUi([&] {
        const UINT dpi = GetDpiForWindow(hwnd);
        SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&off));
    });
    wait(100);
    state(wr, mon);
    expect(EqualRect(&wr, &mon) != 0, "WM_DPICHANGED re-fits it (not the suggested rect)", wr);
    bool stillHidden = false;
    onUi([&] { stillHidden = !IsWindowVisible(hwnd); });
    expect(stillHidden, "the window stayed hidden throughout", wr);
    return failures;
}

// ---- --android: BGRA pictures, remote input, idle hints + help link ----

// 720x1280 test picture (BGRA): x / y gradients in red / green, 64 px
// checker in blue, a white ring; every pixel differs from its transpose.
std::vector<uint8_t> makePattern(int w, int h, int stride, int phase = 0) {
    std::vector<uint8_t> p(static_cast<size_t>(stride) * h, 0xCD);  // padding bytes: garbage
    const float cx = w * 0.5f + phase % 200, cy = h * 0.35f, r = w * 0.22f;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* d = p.data() + static_cast<size_t>(y) * stride + x * 4;
            const bool ring = std::fabs(std::hypot(x - cx, y - cy) - r) < 6;
            d[0] = ring ? 255 : (((x + phase) / 64 + y / 64) % 2 ? 200 : 40);
            d[1] = ring ? 255 : static_cast<uint8_t>(y * 255 / (h - 1));
            d[2] = ring ? 255 : static_cast<uint8_t>(x * 255 / (w - 1));
            d[3] = 255;
        }
    return p;
}

struct AndroidCheck {
    int failures = 0;
    void expect(bool ok, const char* what, const std::string& detail = {}) {
        if (!ok) ++failures;
        std::printf("[android] %s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
        std::fflush(stdout);
    }
};

template <class At>
int runAndroid(pm::VideoWindow& win, At at, TapCapture& cap) {
    using PE = pm::VideoWindow::PointerEvent;
    AndroidCheck chk;
    HWND hwnd = win.hwnd();
    const UINT testMsg = RegisterWindowMessageW(L"PhoneMirror.Video.Test");
    std::atomic<int> helpClicks{0}, helpOffUi{0};
    std::mutex em;
    std::vector<PE> events;
    std::vector<std::tuple<unsigned, bool, wchar_t>> keys;
    std::atomic<int> offUi{0};
    auto wait = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
    // Runs fn on the UI thread and waits for it.
    auto onUi = [](std::function<void()> fn) {
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        runOnUi([&] {
            fn();
            SetEvent(done);
        });
        WaitForSingleObject(done, 5000);
        CloseHandle(done);
    };

    // Phase A: idle screen with two hint lines, check boxes and the help link.
    onUi([&] {
        win.setIdleHints({L"iPhone：控制中心 → 螢幕鏡像輸出", L"Android：設定 → 投放 → 自在投影"});
        win.setIdleOptions({{1, L"開機時自動啟動", true}, {2, L"連線時需要輸入 PIN 碼", false}}, [](int, bool) {});
        win.setIdleHelpLink(L"怎麼連線？", [&] {
            helpClicks++;
            if (GetCurrentThreadId() != g_uiThread) helpOffUi++;
        });
    });
    at(1500);  // (screenshot 1.3 s: idle)
    const LRESULT link = SendMessageW(hwnd, testMsg, 10, 0);
    chk.expect(link != -1, "help link laid out");
    PostMessageW(hwnd, WM_MOUSEMOVE, 0, link);  // hover (screenshot 1.9 s)
    at(2100);
    PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, link);
    PostMessageW(hwnd, WM_LBUTTONUP, 0, link);
    at(2300);
    chk.expect(helpClicks == 1 && helpOffUi == 0, "help link click -> callback on the UI thread",
               "clicks " + std::to_string(helpClicks.load()));
    PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(5, 5));

    // Phase B: BGRA pictures (stride with padding) at 30 fps.
    constexpr int PW = 720, PH = 1280, PS = PW * 4 + 64;
    const auto pattern = makePattern(PW, PH, PS);
    std::atomic<bool> stopFeed{false};
    at(2500);
    std::thread feeder([&] {
        for (int i = 0; !stopFeed; ++i) {
            win.submitBgraFrame(pattern.data(), PW, PH, PS, static_cast<uint64_t>(utcNs()));
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
    });
    auto compare = [&](const wchar_t* file, bool transposed, const char* what) {
        bool ok = false;
        onUi([&] { ok = win.saveSnapshot(file); });
        std::vector<uint8_t> px;
        UINT w = 0, h = 0;
        if (!ok || !readPng(file, px, w, h)) return chk.expect(false, what, "snapshot failed");
        const UINT ew = transposed ? PH : PW, eh = transposed ? PW : PH;
        if (w != ew || h != eh) return chk.expect(false, what, "size " + std::to_string(w) + "x" + std::to_string(h));
        int maxd = 0;
        long long bad = 0;
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x) {
                const UINT sx = transposed ? y : x, sy = transposed ? x : y;
                const uint8_t* s = pattern.data() + static_cast<size_t>(sy) * PS + sx * 4;
                const uint8_t* d = px.data() + (static_cast<size_t>(y) * w + x) * 4;
                for (int c = 0; c < 3; ++c) {
                    const int diff = std::abs(s[c] - d[c]);
                    maxd = std::max(maxd, diff);
                    bad += diff > 1;
                }
            }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%ux%u, max |diff| %d, %lld channel values off by > 1", w, h, maxd, bad);
        chk.expect(maxd <= 1, what, buf);
    };
    at(3200);
    compare(L"android_snapshot.png", false, "BGRA snapshot == submitted pixels");
    auto st = win.stats();
    chk.expect(st.width == PW && st.height == PH && !st.hardwareDecode, "stats: BGRA picture size",
               std::to_string(st.width) + "x" + std::to_string(st.height));

    win.setRotation(1);
    win.setMirrored(true);
    at(3600);
    compare(L"android_snapshot_rot90_mirror.png", true, "BGRA snapshot, rotation 90 + mirror == transposed pixels");

    // Remote input with rotation 90 + mirror: screen (tx, ty) -> picture (ty, tx).
    win.setPointerHandler([&](const PE& e) {
        if (GetCurrentThreadId() != g_uiThread) offUi++;
        std::lock_guard lk(em);
        events.push_back(e);
    });
    win.setKeyHandler([&](unsigned vk, bool down, wchar_t ch) {
        if (GetCurrentThreadId() != g_uiThread) offUi++;
        std::lock_guard lk(em);
        keys.emplace_back(vk, down, ch);
    });
    RECT cr{};
    GetClientRect(hwnd, &cr);
    const int W = cr.right, H = cr.bottom, dw = PH, dh = PW;  // displayed size after rotation
    const float sc = std::min(W / static_cast<float>(dw), H / static_cast<float>(dh));
    const float vw = std::round(dw * sc), vh = std::round(dh * sc);
    const float left = std::floor((W - vw) / 2), top = std::floor((H - vh) / 2);
    auto expectAt = [&](int px, int py, float& x, float& y) {
        const float tx = std::clamp((px + 0.5f - left) / vw, 0.f, 1.f), ty = std::clamp((py + 0.5f - top) / vh, 0.f, 1.f);
        x = ty;  // rotation 90 cw then mirror = transpose
        y = tx;
    };
    struct Exp {
        PE::Kind kind;
        int px, py, button;
        float wx, wy;
    };
    std::vector<Exp> exp;
    auto pt = [&](float fx, float fy) { return POINT{static_cast<int>(left + vw * fx), static_cast<int>(top + vh * fy)}; };
    auto post = [&](UINT msg, WPARAM wp, POINT p) { PostMessageW(hwnd, msg, wp, MAKELPARAM(p.x, p.y)); };
    const POINT p1 = pt(0.30f, 0.40f), p2 = pt(0.10f, 0.20f), p3 = pt(0.55f, 0.70f), p5 = pt(0.80f, 0.25f),
                p6 = pt(0.62f, 0.90f);
    const POINT pOut{p3.x, static_cast<int>(top) - 15};  // above the picture (letterbox)
    const POINT pFar{static_cast<int>(left + vw) + 40, static_cast<int>(top + vh) + 25};
    const POINT pBox{W / 2, std::max(2, static_cast<int>(top) / 2)};  // letterbox, no button down
    at(3800);
    post(WM_MOUSEMOVE, 0, p1);                      exp.push_back({PE::Kind::Move, p1.x, p1.y, -1, 0, 0});
    post(WM_LBUTTONDOWN, MK_LBUTTON, p2);           exp.push_back({PE::Kind::Down, p2.x, p2.y, 0, 0, 0});
    post(WM_MOUSEMOVE, MK_LBUTTON, p3);             exp.push_back({PE::Kind::Move, p3.x, p3.y, 0, 0, 0});
    post(WM_MOUSEMOVE, MK_LBUTTON, pOut);           exp.push_back({PE::Kind::Move, pOut.x, pOut.y, 0, 0, 0});
    post(WM_MOUSEMOVE, MK_LBUTTON, pFar);           exp.push_back({PE::Kind::Move, pFar.x, pFar.y, 0, 0, 0});
    post(WM_LBUTTONUP, 0, pFar);                    exp.push_back({PE::Kind::Up, pFar.x, pFar.y, 0, 0, 0});
    POINT s5 = p5;
    ClientToScreen(hwnd, &s5);
    PostMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-2 * WHEEL_DELTA)), MAKELPARAM(s5.x, s5.y));
    exp.push_back({PE::Kind::Wheel, p5.x, p5.y, 0, 0, -2});
    PostMessageW(hwnd, WM_MOUSEHWHEEL, MAKEWPARAM(0, WHEEL_DELTA / 2), MAKELPARAM(s5.x, s5.y));
    exp.push_back({PE::Kind::Wheel, p5.x, p5.y, 0, 0.5f, 0});
    if (top >= 4) {  // a letterbox to click in (not when the picture fills the window)
        post(WM_MOUSEMOVE, 0, pBox);                // outside, no button: nothing
        post(WM_LBUTTONDOWN, MK_LBUTTON, pBox);     // outside: nothing (no capture)
        post(WM_LBUTTONUP, 0, pBox);
    }
    post(WM_RBUTTONDOWN, MK_RBUTTON, p6);           exp.push_back({PE::Kind::Down, p6.x, p6.y, 1, 0, 0});
    post(WM_RBUTTONUP, 0, p6);                      exp.push_back({PE::Kind::Up, p6.x, p6.y, 1, 0, 0});
    at(4200);
    {
        std::lock_guard lk(em);
        // ReleaseCapture makes Windows synthesise a WM_MOUSEMOVE at the real
        // cursor position: a hover move if the user's cursor is over the
        // picture.  Only the first hover move is part of the script.
        size_t synth = 0;
        for (size_t i = 1; i < events.size();)
            if (events[i].kind == PE::Kind::Move && events[i].button == -1) events.erase(events.begin() + i), ++synth;
            else ++i;
        if (synth) std::printf("[android]   (%zu hover move(s) from the real cursor ignored)\n", synth);
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%zu events (expected %zu), picture rect %.0f,%.0f %.0fx%.0f in %dx%d", events.size(),
                      exp.size(), left, top, vw, vh, W, H);
        chk.expect(events.size() == exp.size(), "pointer event count", buf);
        float maxErr = 0;
        bool kindsOk = true;
        for (size_t i = 0; i < std::min(events.size(), exp.size()); ++i) {
            float ex, ey;
            expectAt(exp[i].px, exp[i].py, ex, ey);
            const PE& e = events[i];
            const float err = std::max(std::fabs(e.x - ex), std::fabs(e.y - ey));
            maxErr = std::max(maxErr, err);
            const bool kindOk = e.kind == exp[i].kind && (e.kind == PE::Kind::Wheel || e.button == exp[i].button) &&
                                e.wheelX == exp[i].wx && e.wheelY == exp[i].wy;
            kindsOk = kindsOk && kindOk;
            static const char* const kNames[] = {"Down", "Move", "Up", "Wheel"};
            std::printf("[android]   %-5s b=%2d client (%4d,%4d) -> (%.4f, %.4f) expected (%.4f, %.4f)%s%s\n",
                        kNames[static_cast<int>(e.kind)], e.button, exp[i].px, exp[i].py, e.x, e.y, ex, ey,
                        e.kind == PE::Kind::Wheel ? (" wheel " + std::to_string(e.wheelX) + "," + std::to_string(e.wheelY)).c_str() : "",
                        kindOk ? "" : "  <-- kind/button/wheel mismatch");
        }
        std::snprintf(buf, sizeof(buf), "max |error| %.6f", maxErr);
        chk.expect(maxErr < 1e-4f && kindsOk, "pointer coordinates (rotation 90 + mirror, letterbox, clamped drag)", buf);
    }
    chk.expect(GetCapture() == nullptr || GetCapture() != hwnd, "capture released");

    // Keys: plain 'A' (character 'a'), Ctrl+S stays with the app, Ctrl+C goes to the phone.
    auto key = [&](UINT msg, WPARAM vk, UINT scan) {
        const LPARAM lp = 1 | (static_cast<LPARAM>(scan) << 16) | (msg == WM_KEYUP ? (3LL << 30) : 0);
        PostMessageW(hwnd, msg, vk, lp);
    };
    // One message at a time, like real input: TranslateMessage's WM_CHAR is
    // then the next message in the queue (as it is for keyboard input).
    auto keyStep = [&](UINT msg, WPARAM vk, UINT scan) {
        key(msg, vk, scan);
        wait(30);
    };
    // This thread's key state (what TranslateMessage / GetKeyState see) is
    // set explicitly: no modifiers held and Caps Lock off, whatever the
    // user's real keyboard is doing (Caps Lock on made 'A' -> 'A').
    BYTE saved[256]{}, clean[256]{};
    onUi([&] {
        GetKeyboardState(saved);
        std::memcpy(clean, saved, sizeof(clean));
        for (int vk : {VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU, VK_LMENU, VK_RMENU,
                       VK_CAPITAL})
            clean[vk] = 0;
        SetKeyboardState(clean);
    });
    keyStep(WM_KEYDOWN, 'A', 0x1E);
    keyStep(WM_KEYUP, 'A', 0x1E);
    onUi([&] {  // Ctrl held
        BYTE ks[256];
        std::memcpy(ks, clean, sizeof(ks));
        ks[VK_CONTROL] = ks[VK_LCONTROL] = 0x80;
        SetKeyboardState(ks);
    });
    keyStep(WM_KEYDOWN, 'S', 0x1F);
    keyStep(WM_KEYUP, 'S', 0x1F);
    keyStep(WM_KEYDOWN, 'C', 0x2E);
    keyStep(WM_KEYUP, 'C', 0x2E);
    onUi([&] { SetKeyboardState(saved); });
    PostMessageW(hwnd, WM_CHAR, 0x4F60, 1);  // IME result 「你」: text only
    at(4600);
    {
        std::lock_guard lk(em);
        const std::vector<std::tuple<unsigned, bool, wchar_t>> want = {
            {'A', true, L'a'}, {'A', false, L'\0'}, {'C', true, L'\0'}, {'C', false, L'\0'}, {0, true, L'\x4F60'}};
        std::string got;
        for (auto& [vk, down, ch] : keys) {
            char b[48];
            std::snprintf(b, sizeof(b), "(0x%02X,%s,U+%04X) ", vk, down ? "down" : "up", static_cast<unsigned>(ch));
            got += b;
        }
        chk.expect(keys == want, "keys: A -> 'a', Ctrl+S not forwarded, Ctrl+C forwarded, IME char", got);
    }
    chk.expect(offUi == 0, "input handlers ran on the UI thread");

    // Phase C: no rotation, device frame (screenshot 5.4 s), framed snapshot.
    win.setPointerHandler(nullptr);
    win.setKeyHandler(nullptr);
    win.setRotation(0);
    win.setMirrored(false);
    win.setDeviceFrame(true);
    at(5600);
    bool framedOk = false;
    onUi([&] { framedOk = win.saveSnapshotFramed(L"android_snapshot_framed.png"); });
    chk.expect(framedOk, "framed snapshot of a BGRA picture");
    stopFeed = true;
    feeder.join();
    at(5900);
    {
        // Frame tap: BGRA -> NV12 (BT.709 video range) back to RGB vs the pattern.
        std::lock_guard lk(cap.m);
        char buf[160];
        if (cap.last.empty() || cap.w != PW || cap.h != PH) {
            std::snprintf(buf, sizeof(buf), "%lld pictures, last %dx%d", cap.count, cap.w, cap.h);
            chk.expect(false, "frame tap of BGRA pictures", buf);
        } else {
            auto rgb = nv12ToBgrx(cap.last.data(), cap.w, cap.h, cap.stride);
            double sq = 0, sum = 0;
            long long n = 0;
            for (int y = 0; y < PH; ++y)
                for (int x = 0; x < PW; ++x)
                    for (int c = 0; c < 3; ++c) {
                        const int d = rgb[(static_cast<size_t>(y) * PW + x) * 4 + c] - pattern[static_cast<size_t>(y) * PS + x * 4 + c];
                        sq += d * d;
                        sum += std::abs(d);
                        ++n;
                    }
            const double psnr = 10 * std::log10(255.0 * 255 / std::max(sq / n, 1e-9));
            std::snprintf(buf, sizeof(buf), "%lld pictures, %dx%d stride %d, mean |diff| %.2f, PSNR %.1f dB (4:2:0)", cap.count,
                          cap.w, cap.h, cap.stride, sum / n, psnr);
            chk.expect(psnr > 30 && cap.onRenderThread && cap.ptsBackwards == 0, "frame tap of BGRA pictures", buf);
        }
    }
    win.setDeviceFrame(false);
    win.onReset();  // back to idle with the hints (screenshot 6.6 s)
    at(7000);
    std::printf("[android] %d failure(s)\n", chk.failures);
    std::fflush(stdout);
    return chk.failures;
}

// --bgra-bench W,H: BGRA pictures at --fps for --seconds (moving pattern).
// ---- --freeze-stream DIR: freeze / zoom / filter on a decoded (NV12 / DXVA) stream ----
template <class At, class Feed>
int runFreezeStream(pm::VideoWindow& win, At at, Feed feed, TapCapture& cap, const std::wstring& dir) {
    AndroidCheck chk;
    CreateDirectoryW(dir.c_str(), nullptr);
    std::thread feeder([&] { feed(); });
    auto onUiShot = [&](const std::wstring& p) {
        bool ok = false;
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        runOnUi([&] {
            ok = win.saveWindowShot(p);
            SetEvent(ev);
        });
        WaitForSingleObject(ev, 5000);
        CloseHandle(ev);
        return ok;
    };
    auto shotPx = [&](std::vector<uint8_t>& px) {
        const std::wstring p = dir + L"\\_tmp.png";
        UINT w = 0, h = 0;
        const bool ok = onUiShot(p) && readPng(p, px, w, h);
        DeleteFileW(p.c_str());
        return ok;
    };
    auto diff = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        if (a.size() != b.size() || a.empty()) return -1.0;
        long long s = 0;
        for (size_t i = 0; i < a.size(); ++i) s += std::abs(a[i] - b[i]);
        return static_cast<double>(s) / a.size();
    };
    char buf[160];
    at(1500);
    long long tap0;
    {
        std::lock_guard lk(cap.m);
        tap0 = cap.count;
    }
    win.setFrozen(true);
    at(1800);
    std::vector<uint8_t> a, b;
    shotPx(a);
    at(2400);
    shotPx(b);
    long long tap1;
    {
        std::lock_guard lk(cap.m);
        tap1 = cap.count;
    }
    // The badge does not change between the shots: the whole window must match.
    std::snprintf(buf, sizeof(buf), "mean |diff| %.4f, frame tap +%lld", diff(a, b), tap1 - tap0);
    chk.expect(diff(a, b) == 0 && tap1 - tap0 > 20, "decoded stream frozen, tap continues", buf);
    std::vector<uint8_t> g;
    int gw = 0, gh = 0;
    const bool grabbed = win.grabPicture(g, gw, gh);
    chk.expect(grabbed && gw > 0 && gh > 0, "grabPicture of a frozen decoded picture",
               std::to_string(gw) + "x" + std::to_string(gh));
    // Device lost while frozen (plain, then from inside the decoder
    // callback): the same picture stays frozen, not the newest decoded one.
    const UINT testMsg = RegisterWindowMessageW(L"PhoneMirror.Video.Test");
    const struct { int ms; LPARAM lp; const char* what; } losses[] = {
        {2500, 0, "device loss while frozen: the same picture stays frozen"},
        {3300, 1, "device loss inside the decoder callback while frozen: the same picture stays frozen"}};
    for (const auto& l : losses) {
        at(l.ms);
        PostMessageW(win.hwnd(), testMsg, 0, l.lp);
        at(l.ms + 700);
        std::vector<uint8_t> c, g2;
        int w2 = 0, h2 = 0;
        shotPx(c);
        const bool grabbed2 = win.grabPicture(g2, w2, h2);
        const double dWin = diff(b, c), dPic = grabbed2 && w2 == gw && h2 == gh ? diff(g, g2) : -1;
        std::snprintf(buf, sizeof(buf), "window mean |diff| %.4f, grabPicture %dx%d mean |diff| %.4f, frozen %d, recoveries %lld",
                      dWin, w2, h2, dPic, win.viewState().frozen ? 1 : 0, win.stats().deviceRecoveries);
        chk.expect(dWin >= 0 && dWin < 0.05 && dPic >= 0 && dPic < 0.05 && win.viewState().frozen, l.what, buf);
    }
    win.zoomAt(3, 0.5f, 0.5f);
    win.setFilter(pm::VideoWindow::Filter::Contrast);
    at(4200);
    onUiShot(dir + L"\\stream_frozen_zoom3_contrast.png");
    win.setFilter(pm::VideoWindow::Filter::None);
    win.resetMagnifier();
    win.setFrozen(false);
    at(6200);
    shotPx(a);
    at(6500);
    shotPx(b);
    std::snprintf(buf, sizeof(buf), "mean |diff| %.4f", diff(a, b));
    chk.expect(diff(a, b) > 0.05, "unfrozen decoded stream moves again", buf);
    feeder.join();
    std::printf("[freeze-stream] %d failure(s)\n", chk.failures);
    return chk.failures;
}

// ---- --magnifier DIR: zoom / pan / filters / freeze / text overlay / region select ----

// Self-checking (PASS/FAIL lines, exit code = failures), off-screen 540x960
// window fed with --png (a phone screenshot, e.g. a translate/testdata screen;
// default: the synthetic pattern) at 30 fps with a moving bar at the bottom.
// Window shots of every state go to DIR (looked at by a human).
// ---- --anim-bench: frames drawn per second in the states that matter for
// CPU (idle animating / settled / hidden / minimised, a static live picture
// with and without the toolbar, recording, live video).  Off-screen window,
// posted mouse, no sound, no network.  presentsTotal counts every Present.
void runAnimBench(pm::VideoWindow& win) {
    const UINT testMsg = RegisterWindowMessageW(L"PhoneMirror.Video.Test");
    HWND h = win.hwnd();
    SendMessageW(h, testMsg, 12, 1);  // posted mouse only
    auto sleep = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
    auto onUi = [&](std::function<void()> fn) {
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        runOnUi([&] {
            fn();
            SetEvent(done);
        });
        WaitForSingleObject(done, 5000);
        CloseHandle(done);
    };
    RECT rc{};
    GetClientRect(h, &rc);
    const LPARAM centre = MAKELPARAM(rc.right / 2, rc.bottom * 3 / 5), corner = MAKELPARAM(4, 4);
    // Measures for ms; mover: post a mouse move every 300 ms (keeps the toolbar up).
    auto measure = [&](const char* what, int ms, bool mover = false, LPARAM where = 0) {
        const auto s0 = win.stats();
        const double c0 = processCpuMs();
        const auto w0 = std::chrono::steady_clock::now();
        for (int t = 0; t < ms; t += 100) {
            if (mover && t % 300 == 0) PostMessageW(h, WM_MOUSEMOVE, 0, where + (t / 300 % 2));
            sleep(100);
        }
        const auto s1 = win.stats();
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
        const double cpu = processCpuMs() - c0;
        std::printf("[bench] %-44s frames drawn %6.1f/s  pictures %5.1f/s  CPU %5.2f %% of one core\n", what,
                    (s1.presentsTotal - s0.presentsTotal) / wall, (s1.framesPresented - s0.framesPresented) / wall,
                    cpu / (wall * 10));
        std::fflush(stdout);
    };
    runOnUi([&] {
        win.setIdleHints({L"iPhone：控制中心 → 螢幕鏡像輸出", L"Android：設定 → 投放 → 自在投影"});
        win.setIdleOptions({{1, L"開機時自動啟動", true}, {2, L"連線時需要輸入 PIN 碼", false}}, [](int, bool) {});
        win.setIdleActions({{L"顯示 Android QR 碼", true, [] {}}, {L"怎麼連線？", false, [] {}}});
    });
    sleep(800);
    PostMessageW(h, WM_MOUSEMOVE, 0, corner);  // activity: the ambient animation runs
    sleep(300);
    measure("idle, animating (after activity)", 5000);
    SendMessageW(h, testMsg, 17, 1);  // 「顯示動畫」 off (test override)
    PostMessageW(h, WM_MOUSEMOVE, 0, corner + 1);
    sleep(1500);
    PostMessageW(h, WM_MOUSEMOVE, 0, corner);
    sleep(300);
    measure("idle, animations off (Windows setting)", 5000);
    SendMessageW(h, testMsg, 17, 2);  // back to the system setting
    PostMessageW(h, WM_MOUSEMOVE, 0, corner + 1);
    sleep(300);
    onUi([&] { ShowWindow(h, SW_HIDE); });
    sleep(300);
    measure("idle, window hidden (tray)", 3000);
    onUi([&] { ShowWindow(h, SW_SHOWNOACTIVATE); });
    sleep(300);
    onUi([&] { ShowWindow(h, SW_SHOWMINNOACTIVE); });
    sleep(300);
    measure("idle, minimised", 3000);
    onUi([&] { ShowWindow(h, SW_SHOWNOACTIVATE); });
    PostMessageW(h, WM_MOUSEMOVE, 0, corner);
    sleep(500);
    measure("idle, animating (again, warm)", 5000);
    std::printf("[bench] waiting 42 s for the idle animation to settle...\n");
    std::fflush(stdout);
    sleep(42000);
    measure("idle, settled (> 40 s without activity)", 5000);

    // Live: one BGRA picture, then nothing (a static phone screen, as scrcpy sends it).
    constexpr int PW = 720, PH = 1280, PS = PW * 4;
    const auto pattern = makePattern(PW, PH, PS);
    auto toolbar = [&](bool rec) {
        std::vector<pm::VideoWindow::ToolbarItem> items = {
            {1, 0xE722, L"截圖"}, {2, 0xE7C8, L"錄影", rec, false, false, rec}, {3, 0xE8B9, L"放大鏡", false, false, true},
            {4, 0xE767, L"喇叭", false, false, true, false, true}, {5, 0, L"音量", false, false, false, false, false, 0.6f},
            {6, 0xE711, L"中斷連線", false, true, true}};
        onUi([&, items] { win.setLiveToolbar(items, [](int) {}); });
    };
    toolbar(false);
    win.submitBgraFrame(pattern.data(), PW, PH, PS, static_cast<uint64_t>(utcNs()));
    sleep(3500);  // fade-in done, toolbar (shown by no movement) stays hidden
    measure("live, static screen, toolbar hidden", 5000);
    measure("live, static screen, toolbar shown", 5000, true, centre);
    win.setRecording(true);
    toolbar(true);
    sleep(3000);  // the toolbar hides again
    measure("recording, static screen, toolbar hidden", 5000);
    measure("recording, static screen, toolbar shown", 5000, true, centre);
    std::atomic<bool> stopFeed{false};
    std::thread feeder([&] {
        while (!stopFeed) {
            win.submitBgraFrame(pattern.data(), PW, PH, PS, static_cast<uint64_t>(utcNs()));
            sleep(33);
        }
    });
    sleep(500);
    measure("recording, video 30 fps, toolbar hidden", 5000);
    win.setRecording(false);
    toolbar(false);
    sleep(2500);
    measure("live, video 30 fps, toolbar hidden", 5000);
    stopFeed = true;
    feeder.join();
    win.onReset();
    sleep(600);
}

// ---- --anim-shots DIR: frame strips of the hover / press / focus / reaction
// transitions (window shots from the back buffer of the off-screen window,
// posted mouse and keys; names carry the ms after the trigger).
void runAnimShots(pm::VideoWindow& win, const std::wstring& dir) {
    CreateDirectoryW(dir.c_str(), nullptr);
    const UINT testMsg = RegisterWindowMessageW(L"PhoneMirror.Video.Test");
    HWND h = win.hwnd();
    SendMessageW(h, testMsg, 12, 1);  // posted mouse only
    using clock = std::chrono::steady_clock;
    auto sleep = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
    auto shot = [&](const std::wstring& name) {
        const bool ok = win.saveWindowShot(dir + L"\\" + name + L".png");
        if (!ok) std::printf("[shots] %ls FAILED\n", name.c_str());
    };
    // Shots at these ms after trigger() (each named prefix_<ms>).
    auto strip = [&](const wchar_t* prefix, std::function<void()> trigger, std::initializer_list<int> times) {
        const auto t0 = clock::now();
        trigger();
        std::printf("[shots] %ls:", prefix);
        for (int t : times) {
            std::this_thread::sleep_until(t0 + std::chrono::milliseconds(t));
            const int real = static_cast<int>(std::chrono::duration<double, std::milli>(clock::now() - t0).count());
            wchar_t n[96];
            swprintf_s(n, L"%ls_%03d", prefix, t);
            shot(n);
            std::printf(" %d(%d)", t, real);
        }
        std::printf("\n");
        std::fflush(stdout);
    };
    auto hook = [&](WPARAM w, LPARAM l) { return SendMessageW(h, testMsg, w, l); };
    auto move = [&](LPARAM lp) { PostMessageW(h, WM_MOUSEMOVE, 0, lp); };
    const LPARAM away = MAKELPARAM(4, 4);
    runOnUi([&] {
        win.setIdleHints({L"iPhone：控制中心 → 螢幕鏡像輸出", L"Android：設定 → 投放 → 自在投影"});
        win.setIdleOptions({{1, L"開機時自動啟動", true}, {2, L"連線時需要輸入 PIN 碼", false}}, [](int, bool) {});
        win.setIdleActions({{L"顯示 Android QR 碼", true, [] {}}, {L"怎麼連線？", false, [] {}}});
    });
    sleep(1500);
    move(away);
    sleep(300);
    const LPARAM opt1 = hook(18, 1), qr = hook(10, 0), link = hook(10, 1);
    std::printf("[shots] option %s, QR %s, link %s\n", opt1 == -1 ? "none" : "ok", qr == -1 ? "none" : "ok",
                link == -1 ? "none" : "ok");
    shot(L"idle_rest");
    const auto hoverTimes = {0, 40, 80, 120, 200};
    const auto outTimes = {0, 60, 120, 180, 260};
    strip(L"option_hover_in", [&] { move(opt1); }, hoverTimes);
    strip(L"option_press", [&] { PostMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, opt1); }, {0, 35, 70, 120});
    strip(L"option_release_tick", [&] { PostMessageW(h, WM_LBUTTONUP, 0, opt1); }, {0, 60, 120, 200, 300, 420});
    strip(L"option_hover_out", [&] { move(away); }, outTimes);
    strip(L"qr_hover_in", [&] { move(qr); }, hoverTimes);
    strip(L"qr_press", [&] { PostMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, qr); }, {0, 35, 70, 120});
    PostMessageW(h, WM_LBUTTONUP, 0, away);  // released elsewhere: no click
    strip(L"qr_hover_out", [&] { move(away); }, outTimes);
    strip(L"link_hover_in", [&] { move(link); }, hoverTimes);
    strip(L"link_hover_out", [&] { move(away); }, outTimes);
    strip(L"focus_tab1", [&] { PostMessageW(h, WM_KEYDOWN, VK_TAB, 0); }, {0, 60, 140});
    strip(L"focus_tab3", [&] {
        PostMessageW(h, WM_KEYDOWN, VK_TAB, 0);
        PostMessageW(h, WM_KEYDOWN, VK_TAB, 0);
    }, {140});
    PostMessageW(h, WM_KEYDOWN, VK_ESCAPE, 0);
    sleep(300);
    strip(L"idle_dots_breath", [] {}, {0, 450, 900, 1350});
    // 「Found a phone」: surprised face, eyes to the phone, sparkles, then the hop.
    strip(L"found_phone", [&] { win.setConnecting(L"Victor 的 Pixel"); },
          {0, 80, 160, 260, 420, 600, 800, 1000, 1300});
    win.onReset();
    sleep(1200);
    move(away);
    sleep(300);
    // Reduced motion (Windows 「顯示動畫」 off): plain 120 ms fades.
    hook(17, 1);
    strip(L"reduced_option_hover_in", [&] { move(opt1); }, {0, 60, 120});
    move(away);
    sleep(300);
    hook(17, 2);

    // Live toolbar over a still picture.
    constexpr int PW = 720, PH = 1280, PS = PW * 4;
    const auto pattern = makePattern(PW, PH, PS);
    std::vector<pm::VideoWindow::ToolbarItem> items = {
        {1, 0xE722, L"截圖"}, {2, 0xE7C8, L"錄影", false, false, false, true}, {3, 0xE8B9, L"放大鏡", false, false, true},
        {4, 0xE767, L"喇叭", false, false, true, false, true}, {5, 0, L"音量", false, false, false, false, false, 0.6f},
        {6, 0xE711, L"中斷連線", false, true, true}};
    runOnUi([&] { win.setLiveToolbar(items, [](int) {}); });
    strip(L"xfade_in", [&] { win.submitBgraFrame(pattern.data(), PW, PH, PS, static_cast<uint64_t>(utcNs())); },
          {0, 60, 120, 200, 300, 420});
    sleep(800);
    RECT rc{};
    GetClientRect(h, &rc);
    move(MAKELPARAM(rc.right / 2, rc.bottom / 2));
    sleep(400);
    const LPARAM b0 = hook(11, 0), b4 = hook(11, 4), b5 = hook(11, 5);
    std::printf("[shots] toolbar buttons %s %s %s\n", b0 == -1 ? "none" : "ok", b4 == -1 ? "none" : "ok",
                b5 == -1 ? "none" : "ok");
    auto keepUp = [&] { move(MAKELPARAM(rc.right / 2, rc.bottom / 2 + 1)); };
    strip(L"tool_hover_in", [&] { move(b0); }, hoverTimes);
    strip(L"tool_press", [&] { PostMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, b0); }, {0, 35, 70, 120});
    strip(L"tool_release", [&] { PostMessageW(h, WM_LBUTTONUP, 0, b0); }, {0, 60, 120});
    strip(L"tool_hover_out", [&] { keepUp(); }, outTimes);
    strip(L"tool_danger_hover_in", [&] { move(b5); }, hoverTimes);
    strip(L"tool_slider_hover_in", [&] { move(b4); }, hoverTimes);
    keepUp();
    strip(L"shutter_toast", [&] {
        win.flash();
        win.showToast(L"已儲存截圖");
    }, {0, 20, 40, 110, 220, 320});
    keepUp();
    sleep(2600);  // the toolbar hides
    strip(L"xfade_out", [&] { win.onReset(); }, {0, 80, 160, 280, 420, 560});
    sleep(800);
}

template <class At>
int runMagnifier(pm::VideoWindow& win, At at, TapCapture& cap, const std::wstring& dir, const std::wstring& png) {
    using PE = pm::VideoWindow::PointerEvent;
    const ULONGLONG t0ms = GetTickCount64();
    AndroidCheck chk;
    HWND hwnd = win.hwnd();
    const UINT testMsg = RegisterWindowMessageW(L"PhoneMirror.Video.Test");
    SendMessageW(hwnd, testMsg, 12, 1);  // posted mouse messages only
    auto onUi = [](std::function<void()> fn) {
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        runOnUi([&] {
            fn();
            SetEvent(done);
        });
        WaitForSingleObject(done, 5000);
        CloseHandle(done);
    };
    auto shot = [&](const wchar_t* name) {
        const std::wstring p = dir + L"\\" + name;
        bool ok = false;
        onUi([&] { ok = win.saveWindowShot(p); });
        std::printf("[magnifier] %6.2f s shot %ls%s\n", (GetTickCount64() - t0ms) / 1000.0, name, ok ? "" : " (FAILED)");
        std::fflush(stdout);
    };
    CreateDirectoryW(dir.c_str(), nullptr);

    // Source picture.
    std::vector<uint8_t> base;
    UINT PW = 720, PH = 1280;
    if (png.empty() || !readPng(png, base, PW, PH)) {
        if (!png.empty()) std::printf("[magnifier] cannot read %ls: synthetic pattern\n", png.c_str());
        PW = 720;
        PH = 1280;
        base = makePattern(PW, PH, PW * 4);
    }
    const int barH = std::max<int>(8, PH / 60);
    auto frameAt = [&](int i) {  // the picture + a red bar moving along the bottom edge
        std::vector<uint8_t> f = base;
        const int bx = (i * 23) % std::max<int>(1, PW - PW / 5), bw = PW / 5;
        for (int y = PH - barH; y < static_cast<int>(PH); ++y)
            for (int x = bx; x < bx + bw; ++x) {
                uint8_t* d = f.data() + (static_cast<size_t>(y) * PW + x) * 4;
                d[0] = 40, d[1] = 40, d[2] = 230, d[3] = 255;
            }
        return f;
    };
    std::atomic<bool> stopFeed{false};
    std::atomic<int> frameNo{0};
    std::mutex fm;
    std::vector<uint8_t> lastSent;
    std::thread feeder([&] {
        for (int i = 0; !stopFeed; ++i) {
            auto f = frameAt(i);
            {
                std::lock_guard lk(fm);
                win.submitBgraFrame(f.data(), PW, PH, PW * 4, static_cast<uint64_t>(utcNs()));
                lastSent = std::move(f);
                frameNo = i;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
    });
    // Mean |diff| of two BGRA pictures (same size), or -1.
    auto meanDiff = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        if (a.size() != b.size() || a.empty()) return -1.0;
        long long s = 0;
        for (size_t i = 0; i < a.size(); i += 4)
            for (int c = 0; c < 3; ++c) s += std::abs(a[i + c] - b[i + c]);
        return static_cast<double>(s) / (a.size() / 4 * 3);
    };
    auto shotPixels = [&](std::vector<uint8_t>& px, UINT& w, UINT& h) {
        const std::wstring p = dir + L"\\_tmp.png";
        bool ok = false;
        onUi([&] { ok = win.saveWindowShot(p); });
        ok = ok && readPng(p, px, w, h);
        DeleteFileW(p.c_str());
        return ok;
    };
    char buf[256];

    at(1200);
    shot(L"mag_01_normal.png");
    // grabPicture == the submitted picture (unzoomed, unfiltered).
    {
        std::vector<uint8_t> g;
        int gw = 0, gh = 0;
        bool ok = false;
        onUi([&] { ok = win.grabPicture(g, gw, gh); });  // also from the UI thread
        std::vector<uint8_t> sent;
        {
            std::lock_guard lk(fm);
            sent = lastSent;
        }
        const double d = ok && gw == static_cast<int>(PW) && gh == static_cast<int>(PH) ? meanDiff(g, sent) : -1;
        std::snprintf(buf, sizeof(buf), "%dx%d, mean |diff| vs a recent frame %.3f (moving bar only)", gw, gh, d);
        chk.expect(ok && d >= 0 && d < 1.5, "grabPicture == submitted picture", buf);
    }

    // Zoom 2.5x at the upper left area: big indicator, then badge + overview.
    win.zoomAt(2.5f, 0.3f, 0.25f);
    at(1500);
    shot(L"mag_02_zoom_indicator.png");
    {
        const auto v = win.viewState();
        std::snprintf(buf, sizeof(buf), "zoom %.2f centre (%.3f, %.3f)", v.zoom, v.centerX, v.centerY);
        // The point under (0.3, 0.25) stays: c = 0.5 + (v - .5) * (1 - 1/2.5).
        chk.expect(std::fabs(v.zoom - 2.5f) < 1e-3f && std::fabs(v.centerX - 0.38f) < 2e-3f &&
                       std::fabs(v.centerY - 0.35f) < 2e-3f,
                   "zoomAt keeps the point under the cursor", buf);
    }
    at(3400);
    shot(L"mag_03_zoom25.png");
    // Snapshot unaffected by the zoom.
    {
        bool ok = false;
        const std::wstring p = dir + L"\\_snap.png";
        onUi([&] { ok = win.saveSnapshot(p); });
        std::vector<uint8_t> px;
        UINT w = 0, h = 0;
        ok = ok && readPng(p, px, w, h);
        DeleteFileW(p.c_str());
        std::vector<uint8_t> sent;
        {
            std::lock_guard lk(fm);
            sent = lastSent;
        }
        const double d = ok && w == PW && h == PH ? meanDiff(px, sent) : -1;
        std::snprintf(buf, sizeof(buf), "%ux%u, mean |diff| %.3f", w, h, d);
        chk.expect(d >= 0 && d < 1.5, "saveSnapshot while zoomed == whole picture", buf);
    }
    // Pointer mapping through the zoom: the viewport centre is the view centre.
    {
        std::mutex em;
        std::vector<PE> events;
        win.setPointerHandler([&](const PE& e) {
            std::lock_guard lk(em);
            events.push_back(e);
        });
        RECT cr{};
        GetClientRect(hwnd, &cr);
        const float sc = std::min(cr.right / static_cast<float>(PW), cr.bottom / static_cast<float>(PH));
        const float vw = std::round(PW * sc), vh = std::round(PH * sc);
        const float left = std::floor((cr.right - vw) / 2), top = std::floor((cr.bottom - vh) / 2);
        const int px = static_cast<int>(left + vw * 0.75f), py = static_cast<int>(top + vh * 0.2f);
        PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(px, py));
        PostMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(px, py));
        at(3700);
        const auto v = win.viewState();
        const float vx = (px + 0.5f - left) / vw, vy = (py + 0.5f - top) / vh;
        const float ex = v.centerX + (vx - 0.5f) / v.zoom, ey = v.centerY + (vy - 0.5f) / v.zoom;
        std::lock_guard lk(em);
        const bool ok = events.size() == 2 && std::fabs(events[0].x - ex) < 2e-3f && std::fabs(events[0].y - ey) < 2e-3f;
        std::snprintf(buf, sizeof(buf), "%zu events, got (%.4f, %.4f) expected (%.4f, %.4f)", events.size(),
                      events.empty() ? -1.f : events[0].x, events.empty() ? -1.f : events[0].y, ex, ey);
        chk.expect(ok, "pointer events map through the magnifier", buf);
        win.setPointerHandler(nullptr);
    }
    // Built-in input: drag pans, Ctrl+wheel zooms at the cursor, arrows pan.
    {
        RECT cr{};
        GetClientRect(hwnd, &cr);
        const auto v0 = win.viewState();
        const int x0 = cr.right / 2, y0 = cr.bottom / 2;
        PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x0, y0));
        PostMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x0 + 120, y0 + 120));
        PostMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(x0 + 60, y0 + 40));
        at(3900);
        const auto v1 = win.viewState();
        std::snprintf(buf, sizeof(buf), "centre (%.3f, %.3f) -> (%.3f, %.3f)", v0.centerX, v0.centerY, v1.centerX, v1.centerY);
        chk.expect(v1.centerX < v0.centerX - 0.01f && v1.centerY < v0.centerY - 0.01f && v1.zoom == v0.zoom,
                   "left drag pans (content follows the mouse)", buf);
        POINT sp{x0, y0};
        ClientToScreen(hwnd, &sp);
        PostMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, WHEEL_DELTA), MAKELPARAM(sp.x, sp.y));
        at(4000);
        const auto v2 = win.viewState();
        std::snprintf(buf, sizeof(buf), "zoom %.3f -> %.3f", v1.zoom, v2.zoom);
        chk.expect(std::fabs(v2.zoom - v1.zoom * 1.25f) < 1e-3f, "Ctrl+wheel zooms in x1.25", buf);
        PostMessageW(hwnd, WM_KEYDOWN, VK_RIGHT, 0);
        PostMessageW(hwnd, WM_KEYUP, VK_RIGHT, 0);
        at(4100);
        const auto v3 = win.viewState();
        std::snprintf(buf, sizeof(buf), "centre x %.3f -> %.3f", v2.centerX, v3.centerX);
        chk.expect(v3.centerX > v2.centerX + 0.01f, "Right arrow pans right", buf);
    }
    // Zoom limits.
    win.setZoom(20);
    at(4200);
    chk.expect(std::fabs(win.viewState().zoom - 8) < 1e-4f, "zoom clamps at 8x");
    shot(L"mag_04_zoom8.png");
    win.zoomStep(-100);
    at(4300);
    chk.expect(std::fabs(win.viewState().zoom - 1) < 1e-4f, "zoom clamps at 1x");

    // Filters (whole picture, then zoomed 2x on the lower half).
    struct F {
        pm::VideoWindow::Filter f;
        const wchar_t* name;
    } filters[] = {{pm::VideoWindow::Filter::Contrast, L"mag_05_filter_contrast.png"},
                   {pm::VideoWindow::Filter::Grayscale, L"mag_06_filter_gray.png"},
                   {pm::VideoWindow::Filter::Invert, L"mag_07_filter_invert.png"},
                   {pm::VideoWindow::Filter::YellowOnBlack, L"mag_08_filter_yellow.png"}};
    int t = 4600;
    for (const auto& f : filters) {
        win.setFilter(f.f);
        at(t += 400);
        shot(f.name);
    }
    win.zoomAt(2, 0.5f, 0.8f);
    at(t += 2000);
    shot(L"mag_09_yellow_zoom2.png");
    win.setFilter(pm::VideoWindow::Filter::None);

    // Freeze: the picture stops while frames keep arriving; the tap goes on.
    win.resetMagnifier();
    at(t += 1800);  // the "原始大小" indicator has faded
    long long tap0;
    {
        std::lock_guard lk(cap.m);
        tap0 = cap.count;
    }
    win.setFrozen(true);
    at(t += 300);
    std::vector<uint8_t> s1, s2;
    UINT w1 = 0, h1 = 0, w2 = 0, h2 = 0;
    shotPixels(s1, w1, h1);
    at(t += 700);
    shotPixels(s2, w2, h2);
    shot(L"mag_10_frozen.png");
    {
        // Compare the window shots outside the badge (top 12 %).
        double d = -1;
        if (w1 == w2 && h1 == h2 && !s1.empty()) {
            const size_t skip = static_cast<size_t>(h1 * 0.12) * w1 * 4;
            std::vector<uint8_t> a(s1.begin() + skip, s1.end()), b(s2.begin() + skip, s2.end());
            d = meanDiff(a, b);
        }
        long long tap1;
        {
            std::lock_guard lk(cap.m);
            tap1 = cap.count;
        }
        std::snprintf(buf, sizeof(buf), "window shots 0.7 s apart: mean |diff| %.4f; frame tap +%lld pictures", d,
                      tap1 - tap0);
        chk.expect(d >= 0 && d < 0.01 && tap1 - tap0 >= 15, "frozen: picture still, frame tap continues", buf);
    }
    // While frozen, grabPicture returns the frozen picture (not the newest frame).
    {
        std::vector<uint8_t> g1, g2;
        int a = 0, b = 0;
        win.grabPicture(g1, a, b);
        at(t += 300);
        win.grabPicture(g2, a, b);
        const double d = meanDiff(g1, g2);
        std::snprintf(buf, sizeof(buf), "mean |diff| %.4f", d);
        chk.expect(d == 0, "grabPicture while frozen is stable", buf);
    }

    // Translated text overlay over the (frozen) picture.
    using TB = pm::VideoWindow::TextBox;
    std::vector<TB> boxes = {
        {0.03f, 0.085f, 0.97f, 0.135f, L"LINE・田中先生：明天在車站剪票口前 10 點見面可以嗎？", L""},
        {0.17f, 0.195f, 0.40f, 0.230f, L"飛航模式", L"機内モード"},
        {0.17f, 0.250f, 0.40f, 0.285f, L"Wi-Fi", L"Wi-Fi"},
        {0.60f, 0.250f, 0.93f, 0.285f, L"家用網路", L"自宅のネットワーク"},
        {0.17f, 0.360f, 0.45f, 0.395f, L"行動網路", L"モバイル通信"},
        {0.05f, 0.925f, 0.95f, 0.985f, L"電池電量不足。開啟低耗電模式可以減少電池的消耗。", L""},
    };
    win.setTextOverlay(boxes);
    at(t += 400);
    shot(L"mag_11_overlay.png");
    win.setTextOverlayOriginal(true);
    at(t += 300);
    shot(L"mag_12_overlay_original.png");
    win.setTextOverlayOriginal(false);
    win.zoomAt(2, 0.3f, 0.25f);
    at(t += 1800);
    shot(L"mag_13_overlay_zoom2.png");
    win.resetMagnifier();
    win.setMirrored(true);
    at(t += 300);
    shot(L"mag_14_overlay_mirrored.png");
    win.setMirrored(false);
    win.setOverlayBusy(L"正在翻譯…");
    at(t += 300);
    shot(L"mag_15_busy.png");
    win.setOverlayBusy(L"");
    win.setTextOverlay({});

    // Region selection: a posted drag -> callback with content coordinates.
    {
        std::atomic<int> calls{0};
        std::atomic<bool> okSel{false}, onUiThread{true};
        float r[4] = {};
        win.beginRegionSelect([&](bool ok, float x0, float y0, float x1, float y1) {
            if (GetCurrentThreadId() != g_uiThread) onUiThread = false;
            okSel = ok;
            r[0] = x0, r[1] = y0, r[2] = x1, r[3] = y1;
            calls++;
        });
        at(t += 200);
        RECT cr{};
        GetClientRect(hwnd, &cr);
        const float sc = std::min(cr.right / static_cast<float>(PW), cr.bottom / static_cast<float>(PH));
        const float vw = std::round(PW * sc), vh = std::round(PH * sc);
        const float left = std::floor((cr.right - vw) / 2), top = std::floor((cr.bottom - vh) / 2);
        auto pt = [&](float fx, float fy) { return MAKELPARAM(static_cast<int>(left + vw * fx), static_cast<int>(top + vh * fy)); };
        PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pt(0.10f, 0.20f));
        PostMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, pt(0.60f, 0.45f));
        at(t += 300);
        shot(L"mag_16_select.png");
        PostMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, pt(0.90f, 0.50f));
        PostMessageW(hwnd, WM_LBUTTONUP, 0, pt(0.90f, 0.50f));
        at(t += 300);
        std::snprintf(buf, sizeof(buf), "%d call(s), ok %d, (%.3f, %.3f)-(%.3f, %.3f)", calls.load(), okSel.load(), r[0],
                      r[1], r[2], r[3]);
        chk.expect(calls == 1 && okSel && onUiThread && std::fabs(r[0] - 0.10f) < 0.01f && std::fabs(r[1] - 0.20f) < 0.01f &&
                       std::fabs(r[2] - 0.90f) < 0.01f && std::fabs(r[3] - 0.50f) < 0.01f,
                   "region select: drag -> content rectangle on the UI thread", buf);
        // Esc cancels.
        calls = 0;
        win.beginRegionSelect([&](bool ok, float, float, float, float) {
            okSel = ok;
            calls++;
        });
        at(t += 150);
        PostMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        at(t += 150);
        chk.expect(calls == 1 && !okSel, "region select: Esc cancels");
    }
    // Rotation 90 + mirror + zoom: selection maps back to content coordinates.
    {
        win.setRotation(1);
        win.setMirrored(true);
        win.zoomAt(2, 0.5f, 0.5f);
        at(t += 1600);
        shot(L"mag_17_rot90_mirror_zoom2.png");
        std::atomic<int> calls{0};
        float r[4] = {};
        win.beginRegionSelect([&](bool ok, float x0, float y0, float x1, float y1) {
            r[0] = x0, r[1] = y0, r[2] = x1, r[3] = y1;
            calls += ok ? 1 : 100;
        });
        at(t += 150);
        RECT cr{};
        GetClientRect(hwnd, &cr);
        const float dw = static_cast<float>(PH), dh = static_cast<float>(PW);  // displayed after rotation
        const float sc = std::min(cr.right / dw, cr.bottom / dh);
        const float vw = std::round(dw * sc), vh = std::round(dh * sc);
        const float left = std::floor((cr.right - vw) / 2), top = std::floor((cr.bottom - vh) / 2);
        auto pt = [&](float fx, float fy) { return MAKELPARAM(static_cast<int>(left + vw * fx), static_cast<int>(top + vh * fy)); };
        PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pt(0.25f, 0.25f));
        PostMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, pt(0.75f, 0.75f));
        PostMessageW(hwnd, WM_LBUTTONUP, 0, pt(0.75f, 0.75f));
        at(t += 300);
        // viewport 0.25..0.75 at zoom 2 around the centre = display 0.375..0.625;
        // content x = 1 - display x (mirror): 0.375..0.625 as well.
        std::snprintf(buf, sizeof(buf), "(%.3f, %.3f)-(%.3f, %.3f)", r[0], r[1], r[2], r[3]);
        chk.expect(calls == 1 && std::fabs(r[0] - 0.375f) < 0.01f && std::fabs(r[2] - 0.625f) < 0.01f &&
                       std::fabs(r[1] - 0.375f) < 0.01f && std::fabs(r[3] - 0.625f) < 0.01f,
                   "region select through rotation + mirror + zoom", buf);
        win.setRotation(0);
        win.setMirrored(false);
    }
    // Device frame + zoom + frozen.
    win.setDeviceFrame(true);
    win.zoomAt(3, 0.5f, 0.3f);
    at(t += 1800);
    shot(L"mag_18_frame_zoom3.png");
    win.setDeviceFrame(false);
    win.resetMagnifier();
    // Unfreeze: the moving bar is back.
    win.setFrozen(false);
    at(t += 300);
    shotPixels(s1, w1, h1);
    at(t += 500);
    shotPixels(s2, w2, h2);
    {
        const double d = meanDiff(s1, s2);
        std::snprintf(buf, sizeof(buf), "mean |diff| %.4f", d);
        chk.expect(d > 0.01, "unfrozen: picture moves again", buf);
    }
    // onReset clears the freeze and the overlay, keeps zoom / filter.
    win.setFrozen(true);
    win.setTextOverlay(boxes);
    stopFeed = true;
    feeder.join();
    win.onReset();
    at(t += 600);
    chk.expect(!win.viewState().frozen, "onReset ends the freeze");
    std::printf("[magnifier] %d failure(s)\n", chk.failures);
    std::fflush(stdout);
    return chk.failures;
}

template <class Secs>
void runBgraBench(pm::VideoWindow& win, int w, int h, double fps, double seconds, Secs secs) {
    const int stride = w * 4;
    std::vector<std::vector<uint8_t>> frames;
    for (int i = 0; i < 8; ++i) frames.push_back(makePattern(w, h, stride, i * 24));
    std::vector<float> callMs;
    FILETIME c0, e0, k0, u0, c1, e1, k1, u1;
    GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k0, &u0);
    const double t0 = secs();
    const auto start = std::chrono::steady_clock::now();
    long long i = 0;
    for (; secs() - t0 < seconds && win.hwnd(); ++i) {
        std::this_thread::sleep_until(start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                  std::chrono::duration<double>(i / fps)));
        const auto a = std::chrono::steady_clock::now();
        win.submitBgraFrame(frames[i % frames.size()].data(), w, h, stride, static_cast<uint64_t>(utcNs()));
        callMs.push_back(std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - a).count());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const double wall = secs() - t0;
    GetProcessTimes(GetCurrentProcess(), &c1, &e1, &k1, &u1);
    auto ticks = [](FILETIME f) { return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    const double cpu = (ticks(k1) - ticks(k0) + ticks(u1) - ticks(u0)) / 1e4;  // ms
    std::sort(callMs.begin(), callMs.end());
    double avg = 0;
    for (float v : callMs) avg += v;
    avg /= std::max<size_t>(callMs.size(), 1);
    auto s = win.stats();
    std::printf("bgra bench %dx%d @ %.0f fps, %.1f s: %lld submitted, presented %lld, replaced (dropped) %lld\n", w, h, fps,
                wall, i, s.framesPresented, s.framesDropped);
    std::printf("  submitBgraFrame call (caller thread, copy into the mailbox): avg %.2f ms, p95 %.2f ms\n", avg,
                callMs.empty() ? 0.0 : callMs[callMs.size() * 95 / 100]);
    std::printf("  submit -> uploaded (render thread): avg %.2f ms, p95 %.2f ms; submit -> Present: avg %.2f ms, p95 %.2f ms\n",
                s.decodeAvgMs, s.decodeP95Ms, s.e2eAvgMs, s.e2eP95Ms);
    if (s.framesTapped)
        std::printf("  frame tap: %lld pictures, %.3f ms avg, %.3f ms p95 per picture\n", s.framesTapped, s.tapAvgMs, s.tapP95Ms);
    std::printf("  process CPU %.1f %% of one core (incl. the synthetic source)\n", cpu / (wall * 1000) * 100);
    std::fflush(stdout);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: pm_video_test <file.h264|file.h265> [options] (see the source header)\n");
        return 2;
    }
    const wchar_t* path = argv[1];
    double fps = 60, minutes = 0, reportSec = 0;
    int holdMs = 1500, idleMs = 1500, loops = 1, width = 1280, height = 720;
    bool demo = false, demo2 = false, sync = false, utcClock = true, tap = false, android = false;
    int benchW = 0, benchH = 0;
    double benchSec = 5;
    const wchar_t* tapDump = nullptr;
    const wchar_t* tourDir = nullptr;
    const wchar_t* magDir = nullptr;
    const wchar_t* freezeDir = nullptr;
    const wchar_t* pngPath = L"";
    int tapSlowMs = 0;
    int syncLat = 0, latA = -1, latB = -1, ntp0Every = 0, churn = 0, churnLoss = 0, decoderCycles = 0;
    double leadMs = 0, jitterMs = 0;
    const wchar_t* altPath = nullptr;
    struct TestEv {
        int ms;
        int kind;  // 0 test hook, 1 WM_DISPLAYCHANGE, 2 cover on, 3 cover off
        int code;
        long arg;
    };
    std::vector<TestEv> tests;
    bool freezeReport = false, watchdogOff = false, offscreen = false, fullscreenCheck = false;
    bool animBench = false;
    const wchar_t* shotsDir = nullptr;
    for (int i = 2; i < argc; ++i) {
        const bool hasValue = i + 1 < argc;
        if (!wcscmp(argv[i], L"--demo-ui")) demo = true;
        else if (!wcscmp(argv[i], L"--demo-ui2")) demo2 = true;
        else if (!wcscmp(argv[i], L"--tap")) tap = true;
        else if (!wcscmp(argv[i], L"--android")) android = tap = true;
        else if (!wcscmp(argv[i], L"--mascot-tour") && hasValue) tourDir = argv[++i];
        else if (!wcscmp(argv[i], L"--anim-bench")) animBench = offscreen = true;
        else if (!wcscmp(argv[i], L"--anim-shots") && hasValue) shotsDir = argv[++i], offscreen = true;
        else if (!wcscmp(argv[i], L"--magnifier") && hasValue) magDir = argv[++i], tap = true;
        else if (!wcscmp(argv[i], L"--png") && hasValue) pngPath = argv[++i];
        else if (!wcscmp(argv[i], L"--freeze-stream") && hasValue) freezeDir = argv[++i], tap = true, offscreen = true;
        else if (!wcscmp(argv[i], L"--bgra-bench") && hasValue) swscanf_s(argv[++i], L"%d,%d", &benchW, &benchH);
        else if (!wcscmp(argv[i], L"--seconds") && hasValue) benchSec = _wtof(argv[++i]);
        else if (!wcscmp(argv[i], L"--tap-dump") && hasValue) tap = true, tapDump = argv[++i];
        else if (!wcscmp(argv[i], L"--tap-slow") && hasValue) tapSlowMs = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--fps") && hasValue) fps = _wtof(argv[++i]);
        else if (!wcscmp(argv[i], L"--hold") && hasValue) holdMs = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--idle") && hasValue) idleMs = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--loops") && hasValue) loops = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--minutes") && hasValue) minutes = _wtof(argv[++i]);
        else if (!wcscmp(argv[i], L"--report") && hasValue) reportSec = _wtof(argv[++i]);
        else if (!wcscmp(argv[i], L"--size") && hasValue) swscanf_s(argv[++i], L"%d,%d", &width, &height);
        else if (!wcscmp(argv[i], L"--sync") && hasValue) sync = true, syncLat = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--clock") && hasValue) utcClock = !!_wcsicmp(argv[++i], L"qpc");
        else if (!wcscmp(argv[i], L"--lead") && hasValue) leadMs = _wtof(argv[++i]);
        else if (!wcscmp(argv[i], L"--jitter") && hasValue) jitterMs = _wtof(argv[++i]);
        else if (!wcscmp(argv[i], L"--lat-sweep") && hasValue) swscanf_s(argv[++i], L"%d,%d", &latA, &latB);
        else if (!wcscmp(argv[i], L"--ntp0-every") && hasValue) ntp0Every = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--churn") && hasValue) churn = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--churn-loss") && hasValue) churnLoss = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--alt") && hasValue) altPath = argv[++i];
        else if (!wcscmp(argv[i], L"--decoder-cycles") && hasValue) decoderCycles = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--test-at") && hasValue) {
            const wchar_t* p = argv[++i];
            while (*p) {
                int t = 0, c = 0, n = 0;
                long a = 0;
                if (swscanf_s(p, L"%d:%d%n", &t, &c, &n) < 2) break;
                p += n;
                if (*p == L':' && swscanf_s(p + 1, L"%ld%n", &a, &n) == 1) p += 1 + n;
                tests.push_back({t, 0, c, a});
                if (*p == L',') ++p;
            }
        } else if (!wcscmp(argv[i], L"--display-change-at") && hasValue) {
            for (const wchar_t* p = argv[++i]; *p;) {
                int t = 0, n = 0;
                if (swscanf_s(p, L"%d%n", &t, &n) < 1) break;
                tests.push_back({t, 1, 0, 0});
                p += n;
                if (*p == L',') ++p;
            }
        } else if (!wcscmp(argv[i], L"--cover-at") && hasValue) {
            for (const wchar_t* p = argv[++i]; *p;) {
                int t = 0, d = 0, n = 0;
                if (swscanf_s(p, L"%d:%d%n", &t, &d, &n) < 2) break;
                tests.push_back({t, 2, 0, d});
                tests.push_back({t + d, 3, 0, 0});
                p += n;
                if (*p == L',') ++p;
            }
        } else if (!wcscmp(argv[i], L"--freeze-report")) freezeReport = true;
        else if (!wcscmp(argv[i], L"--offscreen")) offscreen = true;
        else if (!wcscmp(argv[i], L"--fullscreen-check")) fullscreenCheck = offscreen = true;
        else if (!wcscmp(argv[i], L"--watchdog") && hasValue) watchdogOff = !_wcsicmp(argv[++i], L"off");
    }
    std::stable_sort(tests.begin(), tests.end(), [](const TestEv& a, const TestEv& b) { return a.ms < b.ms; });
    if (watchdogOff) SetEnvironmentVariableW(L"PM_VIDEO_WATCHDOG", L"0");
    if (offscreen) SetEnvironmentVariableW(L"PM_VIDEO_OFFSCREEN", L"1");
    if (minutes > 0 && reportSec <= 0) reportSec = 60;
    if (decoderCycles > 0) {
        // Leak isolation: open/close the decoder MFT (software, no window).
        std::thread([&] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            MFStartup(MF_VERSION, MFSTARTUP_LITE);
            for (auto c : {pm::VideoCodec::H264, pm::VideoCodec::H265}) {
                const auto a = handleTypes();
                for (int i = 0; i < decoderCycles; ++i) {
                    pm::video::MfDecoder d;
                    d.open(c, nullptr);
                }
                std::printf("%s x%d: ", c == pm::VideoCodec::H264 ? "H.264" : "HEVC", decoderCycles);
                printHandleDiff(a, handleTypes());
            }
            MFShutdown();
            CoUninitialize();
        }).join();
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_uiThread = GetCurrentThreadId();

    Stream main, alt;
    if (!load(path, main)) return 1;
    if (altPath && !load(altPath, alt)) return 1;
    const auto& aus = main.aus;
    const auto& data = main.data;
    const pm::VideoCodec codec = main.codec;
    std::printf("%ls: %zu bytes, %zu access units, codec %s, %.0f fps%s\n", path, data.size(), aus.size(),
                codec == pm::VideoCodec::H265 ? "HEVC" : "H.264", fps, demo ? ", UI demo" : "");

    pm::VideoWindow win;
    const bool tall = demo || demo2 || android || tourDir || magDir || animBench || shotsDir;
    if (tourDir || magDir) SetEnvironmentVariableW(L"PM_VIDEO_OFFSCREEN", L"1");
    if (!win.create(L"PhoneMirror - pm_video_test", tall ? 540 : width, tall ? 960 : height)) return 1;
    TapCapture cap;
    const DWORD mainThread = GetCurrentThreadId();
    if (tap) {
        win.setFrameTap([&cap, mainThread, tapSlowMs, keep = tapDump != nullptr || android](const uint8_t* nv12, int w, int h,
                                                                                  int stride, uint64_t pts) {
            {
                std::lock_guard lk(cap.m);
                if (GetCurrentThreadId() == mainThread) cap.onRenderThread = false;
                if (cap.count && (w != cap.w || h != cap.h)) cap.sizeChanges++;
                if (cap.count && pts < cap.lastPts) cap.ptsBackwards++;
                if (!cap.count) cap.firstPts = pts;
                cap.count++;
                cap.w = w;
                cap.h = h;
                cap.stride = stride;
                cap.lastPts = pts;
                if (keep) cap.last.assign(nv12, nv12 + static_cast<size_t>(stride) * h * 3 / 2);
            }
            if (tapSlowMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(tapSlowMs));
        });
    }
    if (sync) {
        win.setSyncMode(true, syncLat);
        std::printf("A/V sync on: latency %d ms, %s clock, lead %.0f ms, jitter %.0f ms\n", syncLat,
                    utcClock ? "UTC" : "QPC", leadMs, jitterMs);
    }

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    auto secs = [&] { return std::chrono::duration<double>(clock::now() - t0).count(); };
    auto at = [&](int ms) { std::this_thread::sleep_until(t0 + std::chrono::milliseconds(ms)); };
    std::atomic<bool> done{false};

    // Feeds `count` AUs of a stream (all if count < 0) at --fps, looping as
    // configured.  Timestamps are on a regular grid even when delivery jitters.
    auto feedStream = [&](const Stream& s, int count, int loopsN, double untilSec) {
        win.onCodec(s.codec);
        std::mt19937 rng(1234);
        std::uniform_real_distribution<double> jit(0, jitterMs);
        const auto start = clock::now();
        const int64_t ntpStart = (utcClock ? utcNs() : qpcNs()) + static_cast<int64_t>(leadMs * 1e6);
        const double period = fps > 0 ? 1.0 / fps : 0;
        long long i = 0;
        for (int l = 0; untilSec > 0 || l < loopsN; ++l) {
            for (size_t k = 0; k < s.aus.size() && (count < 0 || static_cast<int>(k) < count); ++k, ++i) {
                if (!win.hwnd()) return;
                if (untilSec > 0 && secs() >= untilSec) return;
                if (fps > 0) {
                    const double late = jitterMs > 0 ? jit(rng) / 1000 : 0;
                    std::this_thread::sleep_until(start + std::chrono::duration_cast<clock::duration>(
                                                              std::chrono::duration<double>(i * period + late)));
                }
                uint64_t ntp = 0;
                if (sync && !(ntp0Every > 0 && i % ntp0Every == ntp0Every - 1))
                    ntp = static_cast<uint64_t>(ntpStart + static_cast<int64_t>(i * period * 1e9));
                win.onFrame(s.data.data() + s.aus[k].first, s.aus[k].second, ntp);
            }
        }
    };
    auto printStats = [&] {
        auto s = win.stats();
        std::printf("adapter: %ls\n", s.adapter.c_str());
        std::printf("decoder: %s\n", s.hardwareDecode ? "HARDWARE (DXVA/D3D11)" : "software");
        std::printf("frames: in=%lld decoded=%lld presented=%lld dropped=%lld last=%dx%d\n", s.framesIn,
                    s.framesDecoded, s.framesPresented, s.framesDropped, s.width, s.height);
        std::printf("decode latency: avg %.2f ms, p95 %.2f ms\n", s.decodeAvgMs, s.decodeP95Ms);
        std::printf("onFrame->present: avg %.2f ms, p95 %.2f ms\n", s.e2eAvgMs, s.e2eP95Ms);
        if (sync)
            std::printf("A/V sync: %lld scheduled pictures, present - target: avg %+.2f ms, |p95| %.2f ms\n",
                        s.syncPresented, s.syncErrAvgMs, s.syncErrAbsP95Ms);
        std::printf("device recoveries: %lld, watchdog recoveries: %lld\n", s.deviceRecoveries, s.watchdogRecoveries);
        if (tap) {
            std::lock_guard lk(cap.m);
            std::printf("frame tap: %lld pictures (stats %lld), last %dx%d stride %d, size changes %lld, pts backwards %lld, "
                        "pts span %.3f s, render thread: %s\n",
                        cap.count, s.framesTapped, cap.w, cap.h, cap.stride, cap.sizeChanges, cap.ptsBackwards,
                        (cap.lastPts - cap.firstPts) / 1e9, cap.onRenderThread ? "yes" : "NO");
            std::printf("frame tap cost (render thread, per picture): avg %.3f ms, p95 %.3f ms\n", s.tapAvgMs, s.tapP95Ms);
        }
        std::fflush(stdout);
    };

    // Freeze report: (t, AUs in, pictures presented) every 50 ms.
    struct Sample {
        double t;
        long long in, presented, decoded;
    };
    std::vector<Sample> samples;
    double firstFaultT = -1;
    HWND cover = nullptr;

    // Test hooks and latency sweep run on their own timer thread.
    std::thread aux([&] {
        const UINT testMsg = RegisterWindowMessageW(L"PhoneMirror.Video.Test");
        size_t ti = 0;
        double nextReport = reportSec, nextSweep = 2, nextSample = 0;
        bool sweepB = false;
        while (!done) {
            const double t = secs();
            while (ti < tests.size() && t * 1000 >= tests[ti].ms) {
                const TestEv& e = tests[ti++];
                HWND h = win.hwnd();
                if (e.kind != 3 && firstFaultT < 0) firstFaultT = t;
                if (e.kind == 0) {
                    std::printf("[test] t=%.2fs post test hook %d (lParam %ld)\n", t, e.code, e.arg);
                    if (h) PostMessageW(h, testMsg, e.code, e.arg);
                } else if (e.kind == 1) {
                    std::printf("[test] t=%.2fs post WM_DISPLAYCHANGE\n", t);
                    if (h) PostMessageW(h, WM_DISPLAYCHANGE, 32, MAKELPARAM(GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)));
                } else if (e.kind == 2) {
                    // Opaque topmost layered window exactly over the test window,
                    // only if neither touches a monitor (nothing on screen).
                    runOnUi([&, h, ms = e.arg] {
                        RECT r{};
                        if (!h || !GetWindowRect(h, &r) || MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
                            std::printf("[test] cover refused: the test window is on a monitor (use --offscreen)\n");
                            return;
                        }
                        static const ATOM cls = [] {
                            WNDCLASSW wc{};
                            wc.lpfnWndProc = DefWindowProcW;
                            wc.hInstance = GetModuleHandleW(nullptr);
                            wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
                            wc.lpszClassName = L"pm_video_test_cover";
                            return RegisterClassW(&wc);
                        }();
                        cover = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                                L"pm_video_test_cover", L"", WS_POPUP, r.left - 8, r.top - 8,
                                                r.right - r.left + 16, r.bottom - r.top + 16, nullptr, nullptr,
                                                GetModuleHandleW(nullptr), nullptr);
                        if (cover) {
                            SetLayeredWindowAttributes(cover, 0, 255, LWA_ALPHA);
                            ShowWindow(cover, SW_SHOWNOACTIVATE);
                        }
                        std::printf("[test] cover window %s over the test window for %ld ms\n", cover ? "up" : "FAILED", ms);
                        std::fflush(stdout);
                        (void)cls;
                    });
                } else {
                    runOnUi([&] {
                        if (cover) DestroyWindow(cover);
                        cover = nullptr;
                        std::printf("[test] cover window removed\n");
                        std::fflush(stdout);
                    });
                }
                std::fflush(stdout);
            }
            if (freezeReport && t >= nextSample) {
                const auto s = win.stats();
                samples.push_back({t, s.framesIn, s.framesPresented, s.framesDecoded});
                nextSample = t + 0.05;
            }
            if (reportSec > 0 && t >= nextReport) {
                printResources("[soak]", t, win);
                nextReport += reportSec;
            }
            if (sync && latA >= 0 && t >= nextSweep) {
                sweepB = !sweepB;
                win.setSyncMode(true, sweepB ? latB : latA);
                nextSweep += 2;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    int exitCode = -1;
    std::thread feeder([&] {
        timeBeginPeriod(1);
        if (animBench) {
            runAnimBench(win);
        } else if (shotsDir) {
            runAnimShots(win, shotsDir);
        } else if (tourDir) {
            runMascotTour(win, at, tourDir);
        } else if (fullscreenCheck) {
            exitCode = runFullscreenCheck(win);
        } else if (android) {
            exitCode = runAndroid(win, at, cap);
        } else if (magDir) {
            exitCode = runMagnifier(win, at, cap, magDir, pngPath);
        } else if (freezeDir) {
            exitCode = runFreezeStream(win, at, [&] { feedStream(main, -1, 1, 7.0); }, cap, freezeDir);
            win.onReset();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        } else if (benchW > 0 && benchH > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            runBgraBench(win, benchW, benchH, fps > 0 ? fps : 60, benchSec, secs);
            win.onReset();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        } else if (churn > 0) {
            const UINT testMsg = RegisterWindowMessageW(L"PhoneMirror.Video.Test");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            printResources("[churn]", secs(), win);
            std::map<std::wstring, int> htStart;  // after warm-up (cycle 50)
            for (int c = 1; c <= churn && win.hwnd(); ++c) {
                const Stream& s = (altPath && c % 2 == 0) ? alt : main;
                win.setConnecting(L"Churn iPhone #" + std::to_wstring(c));
                win.showPin(std::to_wstring(1000 + c % 9000));
                feedStream(s, 12, 1, 0);  // 200 ms of frames
                win.showToast(L"churn " + std::to_wstring(c));
                win.showPin(L"");
                if (churnLoss > 0 && c % churnLoss == 0)
                    if (HWND h = win.hwnd()) PostMessageW(h, testMsg, 0, 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                win.onReset();
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                if (c % 25 == 0) printResources("[churn]", secs(), win);
                if (c == 50) htStart = handleTypes();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
            printResources("[churn]", secs(), win);
            printHandleDiff(htStart, handleTypes());
            printStats();
        } else if (demo2) {
            runDemo2(win, at, [&](int n) { feedStream(main, -1, n, 0); });
            printStats();
            std::this_thread::sleep_for(std::chrono::milliseconds(idleMs));
        } else if (!demo) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let the idle screen show
            feedStream(main, -1, loops, minutes > 0 ? 0.3 + minutes * 60 : 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
            if (reportSec > 0) printResources("[soak]", secs(), win);
            printStats();
            if (tapDump) {
                // The last tapped picture is the one on screen (ASAP mode):
                // compare our CPU conversion of the tap with saveSnapshot().
                std::vector<uint8_t> nv12;
                int w = 0, h = 0, stride = 0;
                {
                    std::lock_guard lk(cap.m);
                    nv12 = cap.last;
                    w = cap.w;
                    h = cap.h;
                    stride = cap.stride;
                }
                const std::wstring snapPath = std::wstring(tapDump) + L"_snapshot.png";
                bool snapOk = false;
                HANDLE snapDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                runOnUi([&] {
                    snapOk = win.saveSnapshot(snapPath);
                    SetEvent(snapDone);
                });
                WaitForSingleObject(snapDone, 5000);
                CloseHandle(snapDone);
                if (nv12.empty() || !snapOk) {
                    std::printf("tap dump: FAILED (tap %zu bytes, snapshot %s)\n", nv12.size(), snapOk ? "ok" : "failed");
                } else {
                    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    auto rgb = nv12ToBgrx(nv12.data(), w, h, stride);
                    pm::video::writePng(tapDump, rgb.data(), w, h);
                    std::vector<uint8_t> snap;
                    UINT sw = 0, sh = 0;
                    if (readPng(snapPath, snap, sw, sh)) {
                        double sum = 0, sq = 0;
                        int maxd = 0;
                        long long n = 0;
                        for (int y = 0; y < std::min<int>(h, sh); ++y)
                            for (int x = 0; x < std::min<int>(w, sw); ++x)
                                for (int c = 0; c < 3; ++c) {
                                    const int d = std::abs(rgb[(static_cast<size_t>(y) * w + x) * 4 + c] -
                                                           snap[(static_cast<size_t>(y) * sw + x) * 4 + c]);
                                    sum += d;
                                    sq += d * d;
                                    maxd = std::max(maxd, d);
                                    ++n;
                                }
                        const double mse = sq / n;
                        std::printf("tap dump: %ls (%dx%d) vs snapshot (%ux%u): mean |diff| %.3f, max %d, PSNR %.1f dB\n",
                                    tapDump, w, h, sw, sh, sum / n, maxd, mse > 0 ? 10 * std::log10(255.0 * 255 / mse) : 99.0);
                    }
                    CoUninitialize();
                }
            }
            win.onReset();
            std::this_thread::sleep_for(std::chrono::milliseconds(idleMs));
        } else {
            runOnUi([&] {
                win.setIdleOptions({{1, L"開機時自動啟動", true}, {2, L"連線時需要輸入 PIN 碼", false}},
                                   [](int id, bool checked) {
                                       std::printf("toggle: id=%d checked=%d (ui thread: %s)\n", id, checked ? 1 : 0,
                                                   GetCurrentThreadId() == g_uiThread ? "yes" : "NO");
                                       std::fflush(stdout);
                                   });
            });
            at(3000);
            win.setConnecting(L"Victor 的 iPhone");
            at(5000);
            std::thread frames([&] { feedStream(main, -1, loops, 0); });
            at(6000);
            runOnUi([&] {
                const auto s0 = clock::now();
                bool ok = win.saveSnapshot(L"demo_snapshot.png");
                const double ms = std::chrono::duration<double, std::milli>(clock::now() - s0).count();
                std::printf("saveSnapshot (ui thread): %s in %.1f ms\n", ok ? "ok" : "FAILED", ms);
                std::fflush(stdout);
                win.showToast(ok ? L"已儲存截圖 demo_snapshot.png" : L"截圖失敗");
            });
            at(8000);
            win.showPin(L"4821");
            at(9500);
            win.showPin(L"");
            at(10000);
            win.onPaused(true);
            at(11200);
            win.onPaused(false);
            frames.join();
            at(12000);
            printStats();
            win.onReset();
            std::this_thread::sleep_for(std::chrono::milliseconds(idleMs));
        }
        timeEndPeriod(1);
        done = true;
        win.close();
    });
    int rc = messageLoop();
    done = true;
    feeder.join();
    aux.join();
    if (freezeReport && !samples.empty()) {
        // Intervals in which AUs kept arriving but no picture was presented.
        double longest = 0;
        int n = 0;
        for (size_t i = 0; i < samples.size();) {
            size_t j = i;
            while (j + 1 < samples.size() && samples[j + 1].presented == samples[i].presented) ++j;
            const bool recovered = j + 1 < samples.size();
            const double t1 = recovered ? samples[j + 1].t : samples[j].t;
            const long long in = samples[j].in - samples[i].in;
            if (in > 5 && t1 - samples[i].t > 0.25) {
                ++n;
                longest = std::max(longest, t1 - samples[i].t);
                std::printf("[freeze] %.2f s -> %.2f s: %.2f s without a new picture while %lld AUs arrived%s\n",
                            samples[i].t, t1, t1 - samples[i].t, in, recovered ? "" : " (NOT recovered by the end)");
            }
            i = j + 1;
        }
        const Sample& last = samples.back();
        long long p0 = 0, i0 = 0;
        for (const Sample& s : samples)
            if (firstFaultT >= 0 && s.t <= firstFaultT) p0 = s.presented, i0 = s.in;
        std::printf("[freeze] %d freeze(s), longest %.2f s; after the first fault (t=%.2f s): %lld AUs in, %lld pictures "
                    "presented\n",
                    n, longest, firstFaultT, last.in - i0, last.presented - p0);
        std::fflush(stdout);
    }
    return exitCode >= 0 ? exitCode : rc;
}
