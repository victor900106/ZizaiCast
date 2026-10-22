// 0.8.4 隱私黑屏 (see privacy_screen.h): the parent side (PrivacyScreen) and the child (the cover windows).
#include "privacy_screen.h"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "remote_toutou_cloud_png.h"
#include "remote_toutou_face_png.h"
#include "remote_toutou_rim_png.h"
#include "util.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "msimg32.lib")

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace pm::remote {

bool privacyOsSupported() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    static const bool ok = [] {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        auto fn = nt ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(nt, "RtlGetVersion")) : nullptr;
        RTL_OSVERSIONINFOW v{};
        v.dwOSVersionInfoSize = sizeof(v);
        if (!fn || fn(&v) != 0) return false;
        return v.dwMajorVersion > 10 || (v.dwMajorVersion == 10 && v.dwBuildNumber >= 19041);
    }();
    return ok;
}

// ---- parent ------------------------------------------------------------------

bool PrivacyScreen::start(const std::wstring& exe, int lang, const std::string& mode, Event onEvent, std::string& err) {
    stop();
    if (!privacyOsSupported()) return err = "unsupported", false;
    struct Starting {
        std::atomic<bool>& f;
        explicit Starting(std::atomic<bool>& x) : f(x) { f = true; }
        ~Starting() { f = false; }
    } starting(starting_);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0)) {
        for (HANDLE h : {inR, inW, outR, outW})
            if (h) CloseHandle(h);
        return err = "pipe " + std::to_string(GetLastError()), false;
    }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);  // our ends stay ours
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    // Only the two pipe ends are inherited (never the app's sockets / files).
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<char> attrBuf(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    HANDLE inherit[2] = {inR, outW};
    const bool attrOk = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = inR;
    si.StartupInfo.hStdOutput = outW;
    si.StartupInfo.hStdError = outW;
    si.lpAttributeList = attrOk ? attrs : nullptr;
    const std::string m = mode.empty() ? "real" : mode;
    std::wstring cmd = L"\"" + exe + L"\" --pm-privacy-mask " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(std::clamp(lang, 0, 3)) +
                       L" " + widen(m);
    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_SUSPENDED | (attrOk ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr, nullptr,
                                        &si.StartupInfo, &pi);
    const DWORD spawnErr = GetLastError();
    if (attrOk) DeleteProcThreadAttributeList(attrs);
    CloseHandle(inR);
    CloseHandle(outW);
    if (!created) {
        CloseHandle(inW);
        CloseHandle(outR);
        return err = "spawn " + std::to_string(spawnErr), false;
    }
    // Kill-on-close job: if this process dies, Windows ends the child too.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
        if (!AssignProcessToJobObject(job, pi.hProcess)) logf("privacy: the cover is not in a job (%lu); the pipe / parent watch still end it", GetLastError());
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    {
        std::lock_guard<std::mutex> lk(mu_);
        proc_ = pi.hProcess, job_ = job, inW_ = inW, outR_ = outR, pid_ = pi.dwProcessId;
        onEvent_ = std::move(onEvent);
        ready_ = 0, readyErr_.clear();
        stopping_ = false;
    }
    reader_ = std::thread([this] { readLoop(); });
    for (int i = 0; i < 300 && ready_ == 0; ++i) Sleep(10);  // the windows are up in ~50-150 ms
    if (ready_ != 1) {
        err = ready_ == 0 ? "timeout" : readyErr_;
        stop();
        return false;
    }
    running_ = true;
    logf("privacy: cover on (pid %lu, %s)", pid_, m.c_str());
    return true;
}

void PrivacyScreen::readLoop() {
    std::string line;
    char c = 0;
    DWORD got = 0;
    while (ReadFile(outR_, &c, 1, &got, nullptr) && got == 1) {
        if (c != '\n') {
            if (line.size() < 200) line += c;
            continue;
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("R", 0) == 0) ready_ = 1;
        else if (line.rfind("E ", 0) == 0) {
            readyErr_ = line.substr(2);
            ready_ = -1;
        } else if (line == "U") {
            logf("privacy: lifted at the PC (Ctrl+Alt+Shift+B / Esc x3)");
            running_ = false;
            if (!stopping_ && onEvent_) onEvent_("local");
        } else if (!line.empty()) logf("privacy: cover says %s", line.c_str());
        line.clear();
    }
    const bool was = running_.exchange(false);
    if (ready_ == 0) ready_ = -1, readyErr_ = "exited";
    if (was && !stopping_) {
        logf("privacy: the cover ended by itself");
        if (onEvent_) onEvent_("exited");
    }
}

void PrivacyScreen::beat() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!inW_) return;
    DWORD w = 0;
    WriteFile(inW_, "H", 1, &w, nullptr);
}

void PrivacyScreen::testUnlock() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!inW_) return;
    DWORD w = 0;
    WriteFile(inW_, "X", 1, &w, nullptr);
}

void PrivacyScreen::stop() {
    HANDLE proc = nullptr, job = nullptr, inW = nullptr, outR = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        stopping_ = true;
        if (inW_) {
            DWORD w = 0;
            WriteFile(inW_, "Q", 1, &w, nullptr);
        }
        proc = proc_, job = job_, inW = inW_, outR = outR_;
        proc_ = job_ = inW_ = outR_ = nullptr;
    }
    if (proc) {
        if (WaitForSingleObject(proc, 1500) != WAIT_OBJECT_0) {
            logf("privacy: the cover did not end, terminating pid %lu", pid_);
            TerminateProcess(proc, 9);
            WaitForSingleObject(proc, 1000);
        }
    }
    if (inW) CloseHandle(inW);
    if (reader_.joinable()) {
        if (outR) CancelIoEx(outR, nullptr);
        if (reader_.get_id() == std::this_thread::get_id()) reader_.detach();  // (never: the host reaps from its housekeeping)
        else reader_.join();
    }
    if (outR) CloseHandle(outR);
    if (proc) CloseHandle(proc);
    if (job) CloseHandle(job);
    if (running_.exchange(false) || proc) logf("privacy: cover off");
}

// ---- child: the cover --------------------------------------------------------

namespace {

const wchar_t* const kTitle[4] = {L"正在遠端操控中", L"Remote control in progress", L"リモート操作中", L"원격 제어 중"};
const wchar_t* const kSub[4] = {
    L"有人正從遠端使用這台電腦，螢幕暫時遮住了",
    L"Someone is using this PC remotely; the screen is covered for now",
    L"このパソコンはリモートで使用中のため、画面を一時的に隠しています",
    L"누군가 이 PC를 원격으로 사용 중이라 화면을 잠시 가렸습니다",
};
const wchar_t* const kHint[4] = {
    L"坐在電腦前？按 Ctrl+Alt+Shift+B，或連按三次 Esc，就能解除黑幕",
    L"At this PC? Press Ctrl+Alt+Shift+B, or Esc three times, to remove this cover",
    L"パソコンの前にいる方へ：Ctrl+Alt+Shift+B か、Esc を 3 回続けて押すと解除できます",
    L"PC 앞에 계신가요? Ctrl+Alt+Shift+B 또는 Esc를 세 번 누르면 가림 화면이 해제됩니다",
};
const wchar_t* const kFont[4] = {L"Microsoft JhengHei UI", L"Segoe UI", L"Yu Gothic UI", L"Malgun Gothic"};

// 投投 (rim + cloud + happy face, the app's own layers), premultiplied BGRA.
struct Art {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
};
Art g_art;

bool decodePng(IWICImagingFactory* f, const unsigned char* data, size_t n, Art& out) {
    IWICStream* st = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    bool ok = SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory(const_cast<BYTE*>(data), DWORD(n))) &&
              SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnLoad, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
              SUCCEEDED(f->CreateFormatConverter(&cv)) &&
              SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    UINT w = 0, h = 0;
    if (ok) ok = SUCCEEDED(cv->GetSize(&w, &h)) && w > 0 && h > 0;
    if (ok) {
        out.w = int(w), out.h = int(h);
        out.px.resize(size_t(w) * h * 4);
        ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, UINT(out.px.size()), out.px.data()));
    }
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    if (st) st->Release();
    return ok;
}

void loadArt() {
    if (!g_art.px.empty()) return;
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return;
    Art layers[3];
    const bool ok = decodePng(f, kRemoteToutouRim, sizeof(kRemoteToutouRim), layers[0]) &&
                    decodePng(f, kRemoteToutouCloud, sizeof(kRemoteToutouCloud), layers[1]) &&
                    decodePng(f, kRemoteToutouFace, sizeof(kRemoteToutouFace), layers[2]);
    f->Release();
    if (!ok || layers[0].w != layers[1].w || layers[0].h != layers[1].h || layers[2].w != layers[1].w || layers[2].h != layers[1].h) return;
    g_art = layers[0];
    for (int L = 1; L < 3; ++L)  // "over", premultiplied
        for (size_t i = 0; i < g_art.px.size(); i += 4) {
            const uint8_t* s = &layers[L].px[i];
            uint8_t* d = &g_art.px[i];
            const int ia = 255 - s[3];
            for (int k = 0; k < 4; ++k) d[k] = uint8_t(std::min(255, s[k] + (d[k] * ia + 127) / 255));
        }
}

// The cover's picture into dc (rc: the monitor, dpi: its scale).
void paintCover(HDC dc, const RECT& rc, int dpi, int lang) {
    const int W = rc.right - rc.left, H = rc.bottom - rc.top;
    const double s = std::max(0.5, dpi / 96.0);
    // Warm dark rose, a little lighter at the top (one band per 4 px).
    for (int y = 0; y < H; y += 4) {
        const double t = H > 1 ? double(y) / (H - 1) : 0;
        const COLORREF c = RGB(int(52 - 22 * t), int(30 - 12 * t), int(36 - 14 * t));
        HBRUSH b = CreateSolidBrush(c);
        RECT r{rc.left, rc.top + y, rc.right, rc.top + std::min(H, y + 4)};
        FillRect(dc, &r, b);
        DeleteObject(b);
    }
    loadArt();
    const int artW = int(std::min(W * 0.28, 260 * s));
    int y = rc.top + int(H * 0.30);
    if (!g_art.px.empty()) {
        const int artH = artW * g_art.h / g_art.w;
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), g_art.w, -g_art.h, 1, 32, BI_RGB};
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp && bits) {
            memcpy(bits, g_art.px.data(), g_art.px.size());
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, bmp);
            SetStretchBltMode(dc, HALFTONE);
            BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            AlphaBlend(dc, rc.left + (W - artW) / 2, y - artH, artW, artH, mem, 0, 0, g_art.w, g_art.h, bf);
            SelectObject(mem, old);
            DeleteDC(mem);
        }
        if (bmp) DeleteObject(bmp);
        y += int(12 * s);
    }
    lang = std::clamp(lang, 0, 3);
    SetBkMode(dc, TRANSPARENT);
    auto text = [&](const wchar_t* t, int px, int weight, COLORREF col, int gap) {
        HFONT f = CreateFontW(-int(px * s), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, kFont[lang]);
        HGDIOBJ old = SelectObject(dc, f);
        SetTextColor(dc, col);
        RECT r{rc.left + int(W * 0.08), y, rc.right - int(W * 0.08), rc.bottom};
        RECT m = r;
        DrawTextW(dc, t, -1, &m, DT_CENTER | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        r.bottom = r.top + (m.bottom - m.top);
        DrawTextW(dc, t, -1, &r, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        y = r.bottom + int(gap * s);
        SelectObject(dc, old);
        DeleteObject(f);
    };
    text(kTitle[lang], 44, FW_BOLD, RGB(0xF5, 0xA7, 0xA7), 14);
    text(kSub[lang], 20, FW_NORMAL, RGB(0xF6, 0xE9, 0xEA), 40);
    // The way out, in a soft pink frame.
    {
        HFONT f = CreateFontW(-int(17 * s), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, kFont[lang]);
        HGDIOBJ old = SelectObject(dc, f);
        RECT m{0, 0, int(W * 0.8), 0};
        DrawTextW(dc, kHint[lang], -1, &m, DT_CENTER | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        const int pw = (m.right - m.left) + int(40 * s), ph = (m.bottom - m.top) + int(22 * s);
        RECT box{rc.left + (W - pw) / 2, y, rc.left + (W - pw) / 2 + pw, y + ph};
        HBRUSH fill = CreateSolidBrush(RGB(0x4A, 0x2C, 0x33));
        HPEN pen = CreatePen(PS_SOLID, std::max(1, int(1.5 * s)), RGB(0xF5, 0xA7, 0xA7));
        HGDIOBJ ob = SelectObject(dc, fill), op = SelectObject(dc, pen);
        RoundRect(dc, box.left, box.top, box.right, box.bottom, int(ph * 0.9), int(ph * 0.9));
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(fill);
        DeleteObject(pen);
        SetTextColor(dc, RGB(0xFF, 0xD9, 0xDC));
        RECT tr{box.left + int(20 * s), box.top + int(11 * s), box.right - int(20 * s), box.bottom};
        DrawTextW(dc, kHint[lang], -1, &tr, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
        DeleteObject(f);
    }
}

struct Child {
    int lang = 0;
    std::string mode;  // real | offscreen | tiny | tiny-noexclude
    HANDLE parent = nullptr, in = nullptr, out = nullptr;
    DWORD mainThread = 0;
    std::vector<HWND> wins;
    std::atomic<int64_t> lastBeatMs{0};
    std::atomic<bool> quit{false}, rebuild{false};
    HHOOK hook = nullptr;
    EscTriple esc;
    bool escDown = false;
};
Child g_c;
constexpr UINT kMsgUnlock = WM_APP + 41;

void say(const char* line) {
    if (!g_c.out) return;
    DWORD w = 0;
    WriteFile(g_c.out, line, DWORD(strlen(line)), &w, nullptr);
}

LRESULT CALLBACK coverProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_DISPLAYCHANGE: g_c.rebuild = true; return 0;
    case WM_CLOSE: return 0;  // only the child itself ends it (Alt+F4 never reaches it: never focused)
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        if (g_c.mode.rfind("tiny", 0) == 0) {
            HBRUSH b = CreateSolidBrush(RGB(255, 0, 255));  // the colour the test looks for
            FillRect(dc, &rc, b);
            DeleteObject(b);
        } else {
            // Double-buffered: no flicker while it paints a 4K screen.
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            paintCover(mem, rc, int(GetDpiForWindow(h)), g_c.lang);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

LRESULT CALLBACK keyHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        // Only keys typed at this PC: the phone's keys come in through SendInput (injected).
        const bool injected = (k->flags & (LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED)) != 0;
        const bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN, up = wp == WM_KEYUP || wp == WM_SYSKEYUP;
        if (!injected && k->vkCode == VK_ESCAPE) {
            if (down && !g_c.escDown && g_c.esc.press(int64_t(GetTickCount64()))) PostThreadMessageW(g_c.mainThread, kMsgUnlock, 0, 0);
            if (down) g_c.escDown = true;
            if (up) g_c.escDown = false;
        } else if (!injected && down && k->vkCode == 'B' && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) &&
                   (GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
            PostThreadMessageW(g_c.mainThread, kMsgUnlock, 0, 0);
            return 1;  // not typed into the window below
        } else if (!injected && down && k->vkCode != VK_ESCAPE && k->vkCode != VK_CONTROL && k->vkCode != VK_LCONTROL && k->vkCode != VK_RCONTROL &&
                   k->vkCode != VK_MENU && k->vkCode != VK_LMENU && k->vkCode != VK_RMENU && k->vkCode != VK_SHIFT && k->vkCode != VK_LSHIFT &&
                   k->vkCode != VK_RSHIFT) {
            g_c.esc = EscTriple{};  // another key between the Escs starts the count again
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

BOOL CALLBACK addMonitor(HMONITOR, HDC, LPRECT r, LPARAM lp) {
    reinterpret_cast<std::vector<RECT>*>(lp)->push_back(*r);
    return TRUE;
}

void destroyWindows() {
    for (HWND h : g_c.wins) DestroyWindow(h);
    g_c.wins.clear();
}

// One cover per monitor (or the test's tiny / off-screen ones). False: the affinity could not be set
// (nothing is shown then).
bool buildWindows(std::string& err) {
    destroyWindows();
    std::vector<RECT> rects;
    if (g_c.mode == "tiny" || g_c.mode == "tiny-noexclude") {
        rects.push_back(RECT{0, 0, 2, 2});  // the primary monitor's top-left pixels (2x2: one NV12 chroma sample)
    } else {
        EnumDisplayMonitors(nullptr, nullptr, addMonitor, reinterpret_cast<LPARAM>(&rects));
        if (g_c.mode == "offscreen")
            for (size_t i = 0; i < rects.size(); ++i) rects[i] = RECT{-32000 + int(i) * 4, -32000, -32000 + int(i) * 4 + 1, -31999};
    }
    const DWORD ex = WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    for (const RECT& r : rects) {
        HWND h = CreateWindowExW(ex, L"PmPrivacyCover", L"", WS_POPUP, r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
        if (!h) continue;
        SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
        if (g_c.mode != "tiny-noexclude") {
            DWORD aff = 0;
            if (!SetWindowDisplayAffinity(h, WDA_EXCLUDEFROMCAPTURE) || !GetWindowDisplayAffinity(h, &aff) || aff != WDA_EXCLUDEFROMCAPTURE) {
                err = "affinity " + std::to_string(GetLastError());
                DestroyWindow(h);
                destroyWindows();
                return false;
            }
        }
        g_c.wins.push_back(h);
    }
    if (g_c.wins.empty()) return err = "no-window", false;
    for (HWND h : g_c.wins) {
        ShowWindow(h, SW_SHOWNOACTIVATE);
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        UpdateWindow(h);
    }
    return true;
}

DWORD WINAPI stdinLoop(LPVOID) {
    char buf[64];
    DWORD got = 0;
    while (ReadFile(g_c.in, buf, sizeof(buf), &got, nullptr) && got > 0) {
        for (DWORD i = 0; i < got; ++i) {
            if (buf[i] == 'H') g_c.lastBeatMs = int64_t(GetTickCount64());
            else if (buf[i] == 'Q') g_c.quit = true;
            else if (buf[i] == 'X' && g_c.mode != "real") PostThreadMessageW(g_c.mainThread, kMsgUnlock, 0, 0);  // tests: as if the keys were pressed
        }
        if (g_c.quit) break;
    }
    g_c.quit = true;  // the pipe broke: the parent is gone
    PostThreadMessageW(g_c.mainThread, WM_NULL, 0, 0);
    return 0;
}

}  // namespace

int privacyMaskMain(unsigned long parentPid, int lang, const std::string& mode) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_c.lang = std::clamp(lang, 0, 3);
    g_c.mode = mode;
    g_c.in = GetStdHandle(STD_INPUT_HANDLE);
    g_c.out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_c.mainThread = GetCurrentThreadId();
    if (mode != "real" && mode != "offscreen" && mode != "tiny" && mode != "tiny-noexclude") return say("E mode\n"), 2;
    if (!privacyOsSupported()) return say("E unsupported\n"), 2;
    g_c.parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (!g_c.parent) return say("E parent\n"), 4;  // nobody to watch: never cover the screen
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);  // the thread's queue, before the stdin thread posts to it
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = coverProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"PmPrivacyCover";
    RegisterClassExW(&wc);
    std::string err;
    if (!buildWindows(err)) return say(("E " + err + "\n").c_str()), 3;
    g_c.lastBeatMs = int64_t(GetTickCount64());
    HANDLE t = CreateThread(nullptr, 0, stdinLoop, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    g_c.hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyHook, GetModuleHandleW(nullptr), 0);
    // Without the hook (should not happen): the key combination still works as a hotkey.
    if (!g_c.hook) RegisterHotKey(nullptr, 1, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'B');
    char ready[64];
    snprintf(ready, sizeof(ready), "R %zu%s\n", g_c.wins.size(), g_c.hook ? "" : " nohook");
    say(ready);
    SetTimer(nullptr, 0, 250, nullptr);
    int code = 0;
    int64_t lastTop = 0;
    bool unlocked = false;
    while (!g_c.quit) {
        const BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r <= 0) break;
        if (msg.message == kMsgUnlock || (msg.message == WM_HOTKEY && msg.hwnd == nullptr)) {
            unlocked = true;
            break;
        }
        if (msg.message == WM_TIMER && msg.hwnd == nullptr) {
            const int64_t now = int64_t(GetTickCount64());
            if (WaitForSingleObject(g_c.parent, 0) == WAIT_OBJECT_0) break;  // the parent ended (a crash too)
            if (now - g_c.lastBeatMs > kPrivacyWatchdogMs) {  // the parent hangs: never keep the screen covered
                say("W\n");
                code = 5;
                break;
            }
            if (g_c.rebuild.exchange(false) && !buildWindows(err)) {
                code = 3;
                break;
            }
            if (now - lastTop > 2000) {  // stay above other always-on-top windows
                lastTop = now;
                for (HWND h : g_c.wins) SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            }
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    destroyWindows();
    if (g_c.hook) UnhookWindowsHookEx(g_c.hook);
    if (unlocked) say("U\n");
    return code;
}

int privacyRenderPng(const std::wstring& out, int w, int h, int lang, int dpi) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    w = std::clamp(w, 64, 7680), h = std::clamp(h, 64, 4320);
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!bmp || !bits) return 1;
    HGDIOBJ old = SelectObject(mem, bmp);
    paintCover(mem, RECT{0, 0, w, h}, dpi, lang);
    GdiFlush();
    auto* p = static_cast<uint8_t*>(bits);
    for (size_t i = 0; i < size_t(w) * h; ++i) p[i * 4 + 3] = 255;
    IWICImagingFactory* f = nullptr;
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) && SUCCEEDED(f->CreateStream(&st)) &&
              SUCCEEDED(st->InitializeFromFilename(out.c_str(), GENERIC_WRITE)) && SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) &&
              SUCCEEDED(fr->SetSize(UINT(w), UINT(h))) && SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
              SUCCEEDED(fr->WritePixels(UINT(h), UINT(w * 4), UINT(size_t(w) * h * 4), p)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    for (IUnknown* u : std::initializer_list<IUnknown*>{fr, enc, st, f})
        if (u) u->Release();
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    return ok ? 0 : 2;
}

}  // namespace pm::remote
