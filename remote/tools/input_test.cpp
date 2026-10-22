// 0.8.3.5 unit test (no window, no sound, never a real mouse / key): where a point of the picture lands.
//   pm_input_test.exe
// 1. coords.h on every pixel of single / multi-monitor layouts (3440x1440 ultrawide, monitors left of
//    and above the primary: negative coordinates, mixed sizes): picture point -> pixel, pixel ->
//    SendInput 0..65535 -> back (under Windows' (a * size) >> 16 and the "a * (size - 1) / 65535,
//    rounded" reading); the 0.8.3.4 formulas are counted for comparison.
// 2. InputInjector in stub mode (SendInput replaced by a logging stub) with a made-up virtual
//    desktop: "abs" + "down" lands on the expected pixel of the streamed monitor, back / forward
//    buttons are XBUTTON1 / XBUTTON2, a held button is released by releaseAll.
// 3. (0.8.3.5 / 0.8.4) ③ UIPI with the probe stubbed: once per window, the secure desktop once per visit,
//    SendInput refusals (stubbed) -> one notice, one log line + a count.
// 5. (0.8.4) the real look-up across integrity levels: this test's own hidden tool window and a copy of
//    this test at LOW integrity (--probe-child <hwnd>); nothing is ever sent to a window.
// Exit 1 if anything fails.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "coords.h"
#include "custom_keys.h"
#include "input.h"
#include "util.h"

using namespace pm::remote;

static int g_fail = 0, g_pass = 0;
static void check(const char* name, bool ok, const std::string& detail = "") {
    (ok ? g_pass : g_fail)++;
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : "  -- ", detail.c_str());
}

struct Layout {
    const char* name;
    std::vector<RECT> monitors;  // virtual-desktop pixels (physical: the app is per-monitor DPI aware)
};

static RECT unionOf(const std::vector<RECT>& m) {
    RECT u = m[0];
    for (const RECT& r : m) u.left = std::min(u.left, r.left), u.top = std::min(u.top, r.top), u.right = std::max(u.right, r.right), u.bottom = std::max(u.bottom, r.bottom);
    return u;
}

// 0.8.3.4, for comparison
static LONG oldAbs(LONG p, LONG origin, LONG size) { return LONG(std::lround((p - origin) * 65535.0 / std::max(1L, size - 1))); }
static LONG oldNorm(double n, LONG left, LONG size) { return LONG(std::lround(left + std::clamp(n, 0.0, 1.0) * (size - 1))); }
static LONG roundModel(LONG a, LONG origin, LONG size) { return origin + LONG(std::lround(double(a) * (size - 1) / 65535.0)); }

static std::mutex g_logMu;
static std::vector<std::string> g_log;

// ---- 5. (0.8.4) the real look-up across integrity levels, isolated ----------------------------------
// A window only this test owns: a WS_POPUP with WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, never shown (no
// Alt+Tab / taskbar entry, never the focus), placed off-screen anyway.
static HWND hiddenToolWindow(const wchar_t* cls) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;
    RegisterClassW(&wc);
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls, L"", WS_POPUP, -32000, -32000, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
}

// Child (run by probeAcrossLevels at LOW integrity): makes its own hidden window, looks at the parent's
// window, answers in the exit code, stays until the parent has looked at the child's window (WM_CLOSE).
static int probeChild(HWND parentWin) {
    wchar_t cls[64];
    std::swprintf(cls, 64, L"pm_uipi_child_%lu", GetCurrentProcessId());
    HWND mine = hiddenToolWindow(cls);
    bool denied = false;
    const DWORD own = InputInjector::integrityOf(GetCurrentProcess(), denied);
    const InputInjector::Target t = InputInjector::probeWindow(parentWin, own);
    int code = 20;                                          // 20: the look-up did not see it above us
    if (t.higher) code = t.exe == "pm_input_test.exe" ? 10 : 11;  // 10: above us, program named
    if (own != SECURITY_MANDATORY_LOW_RID) code = 30;       // not low: the test did not set it up
    MSG msg;
    const ULONGLONG until = GetTickCount64() + 15000;
    while (mine && IsWindow(mine) && GetTickCount64() < until) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_CLOSE || (msg.hwnd == mine && msg.message == WM_USER + 7)) return code;
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
    }
    return code;
}

static void probeAcrossLevels() {
    HWND mine = hiddenToolWindow(L"pm_uipi_parent");
    bool denied = false;
    const DWORD own = InputInjector::integrityOf(GetCurrentProcess(), denied);
    const InputInjector::Target selfT = InputInjector::probeWindow(mine, own);
    check("UIPI real look-up: our own window is not 'above us'", mine && !selfT.higher && selfT.pid == GetCurrentProcessId());
    // a copy of this test at LOW integrity (our own token, lowered: no privilege needed)
    HANDLE tok = nullptr, low = nullptr;
    PROCESS_INFORMATION pi{};
    bool started = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_ADJUST_DEFAULT | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &tok) &&
        DuplicateTokenEx(tok, 0, nullptr, SecurityImpersonation, TokenPrimary, &low)) {
        SID_IDENTIFIER_AUTHORITY ml = SECURITY_MANDATORY_LABEL_AUTHORITY;
        PSID lowSid = nullptr;
        if (AllocateAndInitializeSid(&ml, 1, SECURITY_MANDATORY_LOW_RID, 0, 0, 0, 0, 0, 0, 0, &lowSid)) {
            TOKEN_MANDATORY_LABEL tml{};
            tml.Label.Attributes = SE_GROUP_INTEGRITY;
            tml.Label.Sid = lowSid;
            if (SetTokenInformation(low, TokenIntegrityLevel, &tml, DWORD(sizeof tml + GetLengthSid(lowSid)))) {
                wchar_t self[MAX_PATH] = {}, cmd[600];
                GetModuleFileNameW(nullptr, self, MAX_PATH);
                std::swprintf(cmd, 600, L"\"%ls\" --probe-child %llu", self, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mine)));
                STARTUPINFOW si{};
                si.cb = sizeof si;
                si.dwFlags = STARTF_USESHOWWINDOW;
                si.wShowWindow = SW_HIDE;
                started = CreateProcessAsUserW(low, nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != FALSE;
            }
            FreeSid(lowSid);
        }
    }
    if (low) CloseHandle(low);
    if (tok) CloseHandle(tok);
    check("UIPI real look-up: a copy of this test started at LOW integrity (hidden, no window shown)", started, started ? "" : "error " + std::to_string(GetLastError()));
    if (!started) return;
    // its window: lower than us -> not 'above us' (what 自在投影 sees of a normal program)
    wchar_t cls[64];
    std::swprintf(cls, 64, L"pm_uipi_child_%lu", pi.dwProcessId);
    HWND child = nullptr;
    for (int i = 0; i < 100 && !child; ++i) {
        child = FindWindowW(cls, nullptr);
        if (!child) Sleep(50);
    }
    const InputInjector::Target ct = InputInjector::probeWindow(child, own);
    check("UIPI real look-up: the LOW copy's window is not above us (no false alarm)", child && ct.pid == pi.dwProcessId && !ct.higher,
          child ? "exe " + ct.exe : "window not found");
    if (child) PostMessageW(child, WM_USER + 7, 0, 0);  // to its message queue (we are above it: allowed)
    PostThreadMessageW(pi.dwThreadId, WM_CLOSE, 0, 0);
    WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    if (code == STILL_ACTIVE) TerminateProcess(pi.hProcess, 99);  // only the process this test started
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    check("UIPI real look-up: from LOW integrity our window is seen as above it, program named (the 自在投影 vs administrator case)", code == 10,
          "child exit " + std::to_string(code) + (code == 11 ? " (above, other name)" : code == 20 ? " (not seen above)" : code == 30 ? " (child not low)" : ""));
    DestroyWindow(mine);
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--probe-child")  // 5.: the LOW-integrity copy
        return probeChild(reinterpret_cast<HWND>(static_cast<uintptr_t>(std::strtoull(argv[2], nullptr, 10))));
    setLogHandler([](const std::string& l) {
        std::lock_guard<std::mutex> lk(g_logMu);
        g_log.push_back(l);
    });
    const std::vector<Layout> layouts = {
        {"3440x1440 ultrawide alone", {{0, 0, 3440, 1440}}},
        {"1920x1080 alone", {{0, 0, 1920, 1080}}},
        {"3440x1440 + 1920x1080 on its left (x < 0, lower)", {{0, 0, 3440, 1440}, {-1920, 180, 0, 1260}}},
        {"3440x1440 + 2560x1440 above (y < 0) + 1920x1080 right", {{0, 0, 3440, 1440}, {440, -1440, 3000, 0}, {3440, 200, 5360, 1280}}},
        {"three: 2560x1440 left, 3440x1440, 3840x2160 right (4K at 100 %)", {{-2560, 0, 0, 1440}, {0, 0, 3440, 1440}, {3440, -360, 7280, 1800}}},
    };
    std::mt19937 rng(835);
    for (const Layout& L : layouts) {
        const RECT vd = unionOf(L.monitors);
        const LONG vw = vd.right - vd.left, vh = vd.bottom - vd.top;
        // 1a. every pixel of the virtual desktop through SendInput's 0..65535 and back
        long bad = 0, badRound = 0, oldBad = 0, oldBadRound = 0;
        for (LONG x = vd.left; x < vd.right; ++x) {
            const LONG a = toAbsolute(x, vd.left, vw);
            bad += fromAbsolute(a, vd.left, vw) != x;
            badRound += roundModel(a, vd.left, vw) != x;
            const LONG o = oldAbs(x, vd.left, vw);
            oldBad += fromAbsolute(o, vd.left, vw) != x;
            oldBadRound += roundModel(o, vd.left, vw) != x;
        }
        for (LONG y = vd.top; y < vd.bottom; ++y) {
            const LONG a = toAbsolute(y, vd.top, vh);
            bad += fromAbsolute(a, vd.top, vh) != y;
            badRound += roundModel(a, vd.top, vh) != y;
            const LONG o = oldAbs(y, vd.top, vh);
            oldBad += fromAbsolute(o, vd.top, vh) != y;
            oldBadRound += roundModel(o, vd.top, vh) != y;
        }
        char d[256];
        std::snprintf(d, sizeof d, "virtual desktop %ldx%ld at %ld,%ld: %ld + %ld off (0.8.3.4: %ld + %ld of %ld)", vw, vh, vd.left, vd.top, bad, badRound,
                      oldBad, oldBadRound, vw + vh);
        check((std::string(L.name) + ": every pixel -> SendInput -> the same pixel").c_str(), bad == 0 && badRound == 0, d);
        // 1b. a point of the picture (the middle of pixel i) -> pixel i, on every monitor
        for (size_t mi = 0; mi < L.monitors.size(); ++mi) {
            const RECT m = L.monitors[mi];
            const LONG w = m.right - m.left, h = m.bottom - m.top;
            long nbad = 0, nold = 0;
            for (LONG i = 0; i < w; ++i) {
                const POINT p = normToDesktop((i + 0.5) / w, 0.5, m);
                nbad += p.x != m.left + i;
                nold += oldNorm((i + 0.5) / w, m.left, w) != m.left + i;
            }
            for (LONG i = 0; i < h; ++i) {
                const POINT p = normToDesktop(0.5, (i + 0.5) / h, m);
                nbad += p.y != m.top + i;
                nold += oldNorm((i + 0.5) / h, m.top, h) != m.top + i;
            }
            // the edges: 0 and 1 stay on the monitor
            const POINT tl = normToDesktop(0, 0, m), br = normToDesktop(1, 1, m), out = normToDesktop(-3, 7, m);
            const bool edges = tl.x == m.left && tl.y == m.top && br.x == m.right - 1 && br.y == m.bottom - 1 && out.x == m.left && out.y == m.bottom - 1;
            std::snprintf(d, sizeof d, "monitor %zu %ldx%ld at %ld,%ld: %ld off (0.8.3.4: %ld of %ld), edges %s", mi, w, h, m.left, m.top, nbad, nold, w + h,
                          edges ? "ok" : "WRONG");
            check((std::string(L.name) + ": picture point -> pixel").c_str(), nbad == 0 && edges, d);

            // 2. the injector (stub) end to end: abs + down on this monitor of this desktop
            InputInjector inj(false, true);
            inj.setVirtualDeskForTest(vd);
            inj.setScreen(m);
            long ebad = 0;
            std::string first;
            for (int k = 0; k < 400; ++k) {
                const LONG px = LONG(rng() % w), py = LONG(rng() % h);
                const double nx = (px + 0.5) / w, ny = (py + 0.5) / h;
                inj.handle("abs", nx, ny, 0, "");
                const POINT c = inj.stubCursor();
                if (c.x != m.left + px || c.y != m.top + py) {
                    if (!ebad) first = std::to_string(c.x) + "," + std::to_string(c.y) + " for " + std::to_string(m.left + px) + "," + std::to_string(m.top + py);
                    ++ebad;
                }
            }
            inj.handle("abs", 1.0, 1.0, 0, "");
            const POINT c = inj.stubCursor();
            std::snprintf(d, sizeof d, "400 random pixels: %ld off%s%s; corner (1,1) -> %ld,%ld", ebad, first.empty() ? "" : ", first: ", first.c_str(), c.x, c.y);
            check((std::string(L.name) + ": injector (stub) lands on the pixel, monitor " + std::to_string(mi)).c_str(),
                  ebad == 0 && c.x == m.right - 1 && c.y == m.bottom - 1, d);
        }
    }

    // 3. buttons through the stub
    {
        InputInjector inj(false, true);
        const RECT m{0, 0, 3440, 1440};
        inj.setVirtualDeskForTest(m);
        inj.setScreen(m);
        {
            std::lock_guard<std::mutex> lk(g_logMu);
            g_log.clear();
        }
        inj.handle("abs", (100 + 0.5) / 3440, (200 + 0.5) / 1440, 0, "");
        for (int b = 0; b < 5; ++b) inj.handle("down", 0, 0, b, ""), inj.handle("up", 0, 0, b, "");
        inj.handle("down", 0, 0, 0, "");
        const bool heldBefore = inj.anyHeld();
        inj.releaseAll();
        const bool heldAfter = inj.anyHeld();
        const bool bad5 = !inj.handle("down", 0, 0, 5, "");
        std::string all;
        {
            std::lock_guard<std::mutex> lk(g_logMu);
            for (const auto& l : g_log) all += l;
        }
        auto has = [&](const char* s) { return all.find(s) != std::string::npos; };
        check("left / middle / right down+up at 100,200", has("mouse LEFTDOWN data 0 at 100,200") && has("mouse LEFTUP data 0 at 100,200") &&
                                                              has("mouse MIDDLEDOWN data 0 at 100,200") && has("mouse RIGHTUP data 0 at 100,200"));
        check("back = XBUTTON1, forward = XBUTTON2", has("mouse XDOWN data 1 at 100,200") && has("mouse XUP data 1 at") && has("mouse XDOWN data 2 at") &&
                                                        has("mouse XUP data 2 at"));
        check("a held button is released by releaseAll (disconnect / page hidden)", heldBefore && !heldAfter && has("releasing held left button"));
        check("button 5 is refused", bad5);
        check("nothing was really sent (stub only)", all.find("SendInput(") == std::string::npos);
    }

    // 4. ③ UIPI: a click / key into an administrator window, the secure desktop (probe stubbed)
    //    0.8.4: once per window for the same phone (not every 3 s), SendInput refusals through the same notice
    {
        InputInjector inj(false, true);
        const RECT m{0, 0, 3440, 1440};
        inj.setVirtualDeskForTest(m);
        inj.setScreen(m);
        std::vector<std::string> told;
        inj.onBlocked = [&](const std::string& why, const std::string& exe, const std::string& win) { told.push_back(why + ":" + exe + ":" + win); };
        InputInjector::Target probe;
        inj.targetProbe = [&](bool, POINT) { return probe; };
        inj.resetNotices("phone-A");
        inj.handle("abs", 0.5, 0.5, 0, "");
        inj.handle("down", 0, 0, 0, ""), inj.handle("up", 0, 0, 0, "");
        check("UIPI: a normal window -> nothing said", told.empty());
        probe.pid = 1234, probe.exe = "regedit.exe", probe.higher = true, probe.win = 0x10a2;
        inj.handle("down", 0, 0, 0, ""), inj.handle("up", 0, 0, 0, "");
        inj.handle("down", 0, 0, 2, ""), inj.handle("up", 0, 0, 2, "");
        inj.handle("key", 0, 0, 0, "Enter");
        check("UIPI: an administrator window -> the phone is told once (admin, the program, the window)",
              told.size() == 1 && told[0] == "admin:regedit.exe:10a2", told.empty() ? "nothing" : told[0] + " x" + std::to_string(told.size()));
        Sleep(3300);  // 0.8.3.5 said it again after 3 s; 0.8.4: never again for this window
        for (int i = 0; i < 20; ++i) inj.handle("down", 0, 0, 0, ""), inj.handle("up", 0, 0, 0, "");
        inj.handle("text", 0, 0, 0, "abc");
        check("UIPI 0.8.4: the same window, 3.3 s later and 21 more presses -> still told only once", told.size() == 1, "x" + std::to_string(told.size()));
        probe.win = 0x20b4;  // a second window of the same program (another regedit)
        inj.handle("down", 0, 0, 0, ""), inj.handle("up", 0, 0, 0, "");
        check("UIPI 0.8.4: another administrator window -> told once for it", told.size() == 2 && told[1] == "admin:regedit.exe:20b4",
              told.size() > 1 ? told[1] : "x" + std::to_string(told.size()));
        probe.higher = false, probe.secure = true, probe.win = 0;
        inj.handle("down", 0, 0, 0, ""), inj.handle("up", 0, 0, 0, "");
        inj.handle("key", 0, 0, 0, "Enter");
        check("UIPI: the secure desktop (UAC prompt / lock screen) -> told once (secure)", told.size() == 3 && told[2].rfind("secure:", 0) == 0,
              "x" + std::to_string(told.size()));
        probe.secure = false;
        const size_t before = told.size();
        inj.handle("wheel", 0, -120, 0, "");
        inj.handle("down", 0, 0, 0, ""), inj.handle("up", 0, 0, 0, "");
        check("UIPI: back to a normal window -> nothing more", told.size() == before);
        probe.secure = true;
        inj.handle("down", 0, 0, 0, ""), inj.handle("up", 0, 0, 0, "");
        check("UIPI 0.8.4: the secure desktop again later (a new UAC prompt) -> told again", told.size() == before + 1);
        probe.secure = false, probe.higher = true, probe.win = 0x10a2;
        inj.resetNotices("phone-A");  // the same phone reconnecting (切換畫質): keeps what it was told
        inj.handle("down", 0, 0, 0, ""), inj.handle("up", 0, 0, 0, "");
        const size_t afterSame = told.size();
        inj.resetNotices("phone-B");  // another phone: hears it once
        inj.handle("down", 0, 0, 0, ""), inj.handle("up", 0, 0, 0, "");
        check("UIPI 0.8.4: the same phone reconnecting is not told again; a new phone is told once",
              afterSame == before + 1 && told.size() == afterSame + 1 && told.back() == "admin:regedit.exe:10a2");
        // SendInput refused by Windows (as 0.7.12 logged 248 times on 2026-10-20): one notice, one log line + a count
        {
            std::lock_guard<std::mutex> lk(g_logMu);
            g_log.clear();
        }
        inj.resetNotices("phone-C");
        probe = InputInjector::Target{};
        probe.pid = 777, probe.exe = "Taskmgr.exe", probe.higher = true, probe.win = 0x30c6;
        inj.targetProbe = [&](bool keyboard, POINT) { return keyboard ? probe : InputInjector::Target{}; };  // only the foreground is elevated
        inj.stubRefuse = [] { return true; };
        const size_t t0 = told.size();
        for (int i = 0; i < 248; ++i) inj.handle("move", (i % 2) ? 3 : -3, 1, 0, "");
        inj.flushLog(true);
        inj.stubRefuse = nullptr;
        std::string all;
        int refusedLines = 0;
        {
            std::lock_guard<std::mutex> lk(g_logMu);
            for (const auto& l : g_log) {
                all += l + "\n";
                if (l.find("refused by Windows (error") != std::string::npos) ++refusedLines;
            }
        }
        check("UIPI 0.8.4: 248 refused pointer moves (foreground = Taskmgr.exe) -> the phone told once, with the program",
              told.size() == t0 + 1 && told.back() == "admin:Taskmgr.exe:30c6", "x" + std::to_string(told.size() - t0) + (told.size() > t0 ? " " + told.back() : ""));
        check("UIPI 0.8.4: ... and the log has 1 line + \"247 more\" (not 248 lines)", refusedLines == 1 && all.find("247 more SendInput refused") != std::string::npos &&
              inj.deniedSends() == 248, "lines " + std::to_string(refusedLines) + ", denied " + std::to_string(inj.deniedSends()));
        bool denied = false;
        const DWORD own = InputInjector::integrityOf(GetCurrentProcess(), denied);
        char d[96];
        std::snprintf(d, sizeof d, "RID 0x%04lX%s", own, denied ? " (denied)" : "");
        check("UIPI: our own integrity level is read (medium for a normal run, high when run as administrator)",
              own == SECURITY_MANDATORY_MEDIUM_RID || own == SECURITY_MANDATORY_HIGH_RID, d);
    }

    // 5. 0.8.4: the REAL look-up (probeWindow, no stub) across integrity levels, isolated: this test's own
    //    never-shown, off-screen WS_EX_TOOLWINDOW window (no Alt+Tab / taskbar entry) and a copy of this test
    //    at LOW integrity (the same "lower process vs window above it" pair as 自在投影 at medium vs an
    //    administrator's window). Nothing is sent to any window: only who owns it and its level are read.
    probeAcrossLevels();

    // 6. 0.8.4 自訂快捷鍵: what may be saved (only keys of the table), and what the stub receives for them
    {
        std::string out, why;
        auto ok = [&](const char* j) { return sanitizeCustomKeys(j, out, &why); };
        check("custom keys: the three examples pass and come back unchanged", ok(defaultCustomKeys().c_str()) && out == defaultCustomKeys(), out);
        check("custom keys: an empty list is fine (all deleted)", ok("[]") && out == "[]");
        check("custom keys: modifiers sorted, a lower-case letter made upper-case", ok(R"([{"n":"x","c":["v","Meta","Control"]}])") && out == R"([{"c":["Control","Meta","V"],"n":"x"}])", out);
        check("custom keys: media + punctuation + F13 are in the table",
              ok(R"([{"c":["AudioVolumeUp"]},{"c":["MediaPlayPause"]},{"c":["AudioVolumeDown"]},{"c":["Control","="]},{"c":["Control","Shift","F13"]}])"), why);
        check("custom keys: names in any language are kept (ASCII-escaped on the settings line)", ok(R"([{"n":"剪貼簿 \ud83d\ude00","c":["Meta","V"]}])") && out.find("u526a") != std::string::npos && out.find('\n') == std::string::npos && out.find("剪") == std::string::npos, out);
        check("custom keys: an unknown key is refused", !ok(R"([{"c":["Control","Launch"]}])") && why == "key");
        check("custom keys: Fn / unknown F key (F25) refused", !ok(R"([{"c":["F25"]}])") && !ok(R"([{"c":["Fx"]}])"));
        check("custom keys: two non-modifier keys refused", !ok(R"([{"c":["Control","A","B"]}])") && why == "combo");
        check("custom keys: a doubled modifier refused", !ok(R"([{"c":["Control","Control","A"]}])"));
        check("custom keys: no keys / more than 5 refused", !ok(R"([{"c":[]}])") && !ok(R"([{"c":["Control","Alt","Shift","Meta","A","B"]}])"));
        std::string many = "[";
        for (int i = 0; i < 13; ++i) many += std::string(i ? "," : "") + R"({"c":["A"]})";
        check("custom keys: 13 buttons refused, 12 fine", !ok((many + "]").c_str()) && why == "count" && ok((many.substr(0, many.rfind(',')) + "]").c_str()));
        check("custom keys: a control character / a name over 40 bytes refused", !ok(R"([{"n":"ab","c":["A"]}])") && !ok((std::string(R"([{"n":")") + std::string(41, 'x') + R"(","c":["A"]}])").c_str()));
        check("custom keys: not a list / wrong types refused", !ok("{}") && !ok("nope") && !ok(R"([1])") && !ok(R"([{"c":"A"}])") && !ok(R"([{"c":[1]}])") && !ok(R"([{"c":["A"],"t":"evil"}])"));

        InputInjector inj(false, true);
        {
            std::lock_guard<std::mutex> lk(g_logMu);
            g_log.clear();
        }
        inj.combo({"Meta", "V"});
        inj.combo({"AudioVolumeUp"});
        inj.combo({"MediaPlayPause"});
        inj.combo({"Control", "Shift", "T"});
        const bool bad = !inj.combo({"Control", "Launch"});
        std::string all;
        {
            std::lock_guard<std::mutex> lk(g_logMu);
            for (const auto& l : g_log) all += l;
        }
        auto has = [&](const char* s) { return all.find(s) != std::string::npos; };
        check("stub gets Win+V (LWIN 0x5B down, V 0x56 down, up in reverse)", has("combo Meta+V") && has("vk 0x5B") && has("vk 0x56"));
        check("stub gets the media keys (VOLUME_UP 0xAF, PLAY_PAUSE 0xB3)", has("combo AudioVolumeUp") && has("vk 0xAF") && has("vk 0xB3"));
        check("stub gets Ctrl+Shift+T and refuses an unknown key", has("combo Control+Shift+T") && bad);
        check("nothing was really sent (stub only)", all.find("SendInput(") == std::string::npos);
    }

    std::printf("%s (%d passed, %d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
