// pm_panel_anim_test OUTDIR: the app panels' animations, off the desktop
// (owner window at x = -12000, panels with testOffscreen; posted mouse; no
// network, no sound). Writes frame PNGs named <strip>_<ms>.png:
//   *_open_*   PrintWindow of the opening panel, composited at its window
//              alpha over grey (fade + 97 -> 100 % grow; PairPanel alpha only)
//   *_close_*  the closing panel (110 ms ease-in fade, then destroyed)
//   *_hover_*  renderPng while a button's hover eases in, *_press_* pressed
//   pair_qr_*  the QR reveal (circle from the centre, 320 ms)
// and prints PASS / FAIL lines (exit code = failures).
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "../about_panel.h"
#include "../ask_panel.h"
#include "../pair_panel.h"
#include "../ui_anim.h"
#include "../update_panel.h"

using Microsoft::WRL::ComPtr;

namespace {

int failures = 0;
void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

void pump(int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 2, QS_ALLINPUT);
    }
}

bool savePng(const std::wstring& path, const std::vector<uint8_t>& bgra, int w, int h) {
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    ComPtr<IWICStream> s;
    ComPtr<IWICBitmapEncoder> e;
    ComPtr<IWICBitmapFrameEncode> fr;
    if (FAILED(f->CreateStream(&s)) || FAILED(s->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &e)) || FAILED(e->Initialize(s.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(e->CreateNewFrame(&fr, nullptr)) || FAILED(fr->Initialize(nullptr)))
        return false;
    fr->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    fr->SetPixelFormat(&fmt);
    fr->WritePixels(h, w * 4, static_cast<UINT>(bgra.size()), const_cast<BYTE*>(bgra.data()));
    return SUCCEEDED(fr->Commit()) && SUCCEEDED(e->Commit());
}

// The window as DWM shows it, at its layered alpha over grey.
bool shot(HWND h, const std::wstring& path, BYTE* alphaOut = nullptr) {
    if (!IsWindow(h)) return false;
    RECT r{};
    GetWindowRect(h, &r);
    const int w = r.right - r.left, hh = r.bottom - r.top;
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -hh, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bmp);
    const bool ok = PrintWindow(h, dc, 2 /*PW_RENDERFULLCONTENT*/) != FALSE;
    BYTE alpha = 255;
    DWORD flags = 0;
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_LAYERED) GetLayeredWindowAttributes(h, nullptr, &alpha, &flags);
    if (!(flags & LWA_ALPHA)) alpha = 255;
    std::vector<uint8_t> px(static_cast<size_t>(w) * hh * 4);
    const auto* src = static_cast<const uint8_t*>(bits);
    for (size_t i = 0; i < px.size(); i += 4)
        for (int c = 0; c < 3; ++c) px[i + c] = static_cast<uint8_t>((src[i + c] * alpha + 0x80 * (255 - alpha)) / 255);
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    if (alphaOut) *alphaOut = alpha;
    return ok && savePng(path, px, w, hh);
}

// Frames at these ms after trigger(): fn(name) takes each one.
void strip(const std::wstring& dir, const wchar_t* name, std::function<void()> trigger,
           std::function<void(const std::wstring&)> take, std::initializer_list<int> times) {
    const auto t0 = std::chrono::steady_clock::now();
    trigger();
    for (int t : times) {
        do pump(1);
        while (std::chrono::steady_clock::now() < t0 + std::chrono::milliseconds(t));
        wchar_t n[128];
        swprintf_s(n, L"%ls\\%ls_%03d.png", dir.c_str(), name, t);
        const auto s0 = std::chrono::steady_clock::now();
        take(n);
        std::printf("  %ls %d ms (shot took %.0f ms)\n", name, t,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count());
    }
}

LPARAM dip(HWND h, float x, float y) {
    const float s = GetDpiForWindow(h) / 96.f;
    return MAKELPARAM(static_cast<int>(x * s), static_cast<int>(y * s));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: pm_panel_anim_test OUTDIR\n");
        return 2;
    }
    const std::wstring dir = argv[1];
    CreateDirectoryW(dir.c_str(), nullptr);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    using namespace pm::ui;
    std::printf("Windows 「顯示動畫」: %s\n", animationsOn() ? "on" : "off");
    AskPanel::testOffscreen = PairPanel::testOffscreen = AboutPanel::testOffscreen = UpdatePanel::testOffscreen = true;
    // A visible owner far off the desktop: the panels centre over it.
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PmPanelAnimOwner";
    RegisterClassW(&wc);
    HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"owner", WS_POPUP, -12000, 200,
                                 800, 800, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(owner, SW_SHOWNOACTIVATE);
    RECT orc{};
    GetWindowRect(owner, &orc);
    expect(!MonitorFromRect(&orc, MONITOR_DEFAULTTONULL), "owner window is off every monitor");
    shot(owner, dir + L"\\_warmup.png");  // WIC / PrintWindow warm-up (the first shot is slow)

    // ---- AskPanel: open fade + grow, hover, press, close fade.
    AskPanel ask;
    int answer = -1;
    auto askInfo = [&] {
        AskPanel::Info i;
        i.title = L"按 X 時要怎麼做？";
        i.body = L"可以縮到右下角繼續接收手機投影，或直接結束程式。";
        i.primary = L"縮到右下角";
        i.secondary = L"結束程式";
        i.secondaryChoice = 2;
        i.check = L"記住我的選擇";
        i.done = [&](int c) { answer = c; };
        return i;
    };
    HWND askH = nullptr;
    BYTE a0 = 0, a1 = 0;
    strip(dir, L"ask_open", [&] {
        ask.open(owner, askInfo(), false);
        askH = ask.hwnd();
    }, [&](const std::wstring& p) {
        BYTE a = 0;
        shot(askH, p, &a);
        std::printf("  ask_open alpha %d\n", a);
        if (!a0) a0 = a ? a : 1;
        a1 = a;
    }, {0, 40, 80, 120, 160, 240});
    RECT ar{};
    GetWindowRect(askH, &ar);
    expect(!MonitorFromRect(&ar, MONITOR_DEFAULTTONULL), "AskPanel is off every monitor");
    expect(a0 < 200 && a1 == 255, "AskPanel fades in (alpha low at first, 255 at the end)");
    expect(!(GetWindowLongPtrW(askH, GWL_EXSTYLE) & WS_EX_LAYERED), "AskPanel no longer layered after the fade");
    RECT cr{};
    GetClientRect(askH, &cr);
    const float hDip = cr.bottom / (GetDpiForWindow(askH) / 96.f);
    const LPARAM prim = dip(askH, 452 - 26 - 60, hDip - 22 - 19), away = dip(askH, 30, 120);
    strip(dir, L"ask_hover", [&] { PostMessageW(askH, WM_MOUSEMOVE, 0, prim); },
          [&](const std::wstring& p) { ask.renderPng(p); }, {0, 40, 80, 120, 200});
    strip(dir, L"ask_press", [&] { PostMessageW(askH, WM_LBUTTONDOWN, MK_LBUTTON, prim); },
          [&](const std::wstring& p) { ask.renderPng(p); }, {0, 35, 70, 120});
    PostMessageW(askH, WM_LBUTTONUP, 0, away);  // released elsewhere: no answer
    strip(dir, L"ask_unhover", [&] { PostMessageW(askH, WM_MOUSEMOVE, 0, away); },
          [&](const std::wstring& p) { ask.renderPng(p); }, {0, 60, 120, 180, 260});
    BYTE c1 = 255;
    bool gone = false;
    strip(dir, L"ask_close", [&] { ask.close(0); }, [&](const std::wstring& p) {
        BYTE a = 255;
        if (shot(askH, p, &a)) c1 = a;
    }, {0, 30, 60, 90});
    pump(150);
    gone = !IsWindow(askH);
    expect(answer == 0, "AskPanel answered at once on close (not after the fade)");
    expect(c1 < 255, "AskPanel fades out");
    expect(gone, "AskPanel window destroyed after its close fade");

    // ---- PairPanel: alpha-only open, QR reveal.
    PairPanel pair;
    HWND pairH = nullptr;
    strip(dir, L"pair_open", [&] {
        pair.open(owner, {});
        pairH = pair.hwnd();
    }, [&](const std::wstring& p) { shot(pairH, p); }, {0, 60, 120, 200});
    std::vector<uint8_t> qr(static_cast<size_t>(29) * 29 * 4, 255);
    for (int y = 0; y < 29; ++y)
        for (int x = 0; x < 29; ++x) {
            const bool finder = (x < 7 && y < 7) || (x > 21 && y < 7) || (x < 7 && y > 21);
            bool dark = ((x * 7 + y * 13 + x * y) % 5) < 2;
            if (finder) {
                const int fx = x > 21 ? x - 22 : x, fy = y > 21 ? y - 22 : y;
                dark = fx == 0 || fx == 6 || fy == 0 || fy == 6 || (fx >= 2 && fx <= 4 && fy >= 2 && fy <= 4);
            } else if ((x < 8 && y < 8) || (x > 20 && y < 8) || (x < 8 && y > 20)) {
                dark = false;
            }
            uint8_t* p = &qr[(static_cast<size_t>(y) * 29 + x) * 4];
            p[0] = p[1] = p[2] = dark ? 0 : 255;
        }
    strip(dir, L"pair_qr", [&] { pair.setQr(qr, 29); }, [&](const std::wstring& p) { shot(pairH, p); },
          {0, 60, 120, 200, 320, 400});
    pair.close();
    pump(200);
    expect(!IsWindow(pairH), "PairPanel window destroyed after its close fade");

    // ---- About / Update: open fades.
    AboutPanel about;
    HWND aboutH = nullptr;
    AboutPanel::Info ai;
    ai.version = L"0.7.2";
    ai.repoUrl = L"https://github.com/victor900106/ZizaiCast";
    strip(dir, L"about_open", [&] {
        about.open(owner, ai);
        aboutH = about.hwnd();
    }, [&](const std::wstring& p) { shot(aboutH, p); }, {0, 40, 80, 160});
    strip(dir, L"about_hover", [&] { PostMessageW(aboutH, WM_MOUSEMOVE, 0, dip(aboutH, 210, 520 - 22 - 19)); },
          [&](const std::wstring& p) { about.renderPng(p); }, {0, 40, 80, 120, 200});
    about.close();
    UpdatePanel upd;
    HWND updH = nullptr;
    UpdatePanel::Info ui;
    ui.current = L"0.7.2";
    ui.version = L"0.7.3";
    ui.changesZh = {L"按鈕有動畫了", L"投投看到手機會驚訝"};
    strip(dir, L"update_open", [&] {
        upd.open(owner, ui, false);
        updH = upd.hwnd();
    }, [&](const std::wstring& p) { shot(updH, p); }, {0, 40, 80, 160});
    upd.close();
    pump(200);
    expect(!IsWindow(aboutH) && !IsWindow(updH), "About / Update windows destroyed after their close fades");

    DestroyWindow(owner);
    CoUninitialize();
    std::printf("%d failure(s)\n", failures);
    return failures;
}
