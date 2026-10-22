// 0.8.3.5 unit test (no window, no sound, never a real mouse / key): where a point of the picture lands.
//   pm_input_test.exe
// 1. coords.h on every pixel of single / multi-monitor layouts (3440x1440 ultrawide, monitors left of
//    and above the primary: negative coordinates, mixed sizes): picture point -> pixel, pixel ->
//    SendInput 0..65535 -> back (under Windows' (a * size) >> 16 and the "a * (size - 1) / 65535,
//    rounded" reading); the 0.8.3.4 formulas are counted for comparison.
// 2. InputInjector in stub mode (SendInput replaced by a logging stub) with a made-up virtual
//    desktop: "abs" + "down" lands on the expected pixel of the streamed monitor, back / forward
//    buttons are XBUTTON1 / XBUTTON2, a held button is released by releaseAll.
// Exit 1 if anything fails.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "coords.h"
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

int main() {
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
    {
        InputInjector inj(false, true);
        const RECT m{0, 0, 3440, 1440};
        inj.setVirtualDeskForTest(m);
        inj.setScreen(m);
        std::vector<std::string> told;
        inj.onBlocked = [&](const std::string& why, const std::string& exe) { told.push_back(why + ":" + exe); };
        InputInjector::Target probe;
        inj.targetProbe = [&](bool, POINT) { return probe; };
        inj.handle("abs", 0.5, 0.5, 0, "");
        inj.handle("down", 0, 0, 0, ""), inj.handle("up", 0, 0, 0, "");
        check("UIPI: a normal window -> nothing said", told.empty());
        probe.pid = 1234, probe.exe = "regedit.exe", probe.higher = true;
        inj.handle("down", 0, 0, 0, ""), inj.handle("up", 0, 0, 0, "");
        inj.handle("down", 0, 0, 2, ""), inj.handle("up", 0, 0, 2, "");  // again within 3 s: said once
        inj.handle("key", 0, 0, 0, "Enter");
        check("UIPI: an administrator window -> the phone is told once (admin, the program)", told.size() == 1 && told[0] == "admin:regedit.exe",
              told.empty() ? "nothing" : told[0] + " x" + std::to_string(told.size()));
        probe.higher = false, probe.secure = true;
        inj.handle("down", 0, 0, 0, ""), inj.handle("up", 0, 0, 0, "");
        check("UIPI: the secure desktop (UAC prompt / lock screen) -> told (secure)", told.size() == 2 && told[1].rfind("secure:", 0) == 0);
        probe.secure = false;
        const size_t before = told.size();
        inj.handle("wheel", 0, -120, 0, "");
        check("UIPI: back to a normal window -> nothing more", told.size() == before);
        bool denied = false;
        const DWORD own = InputInjector::integrityOf(GetCurrentProcess(), denied);
        char d[96];
        std::snprintf(d, sizeof d, "RID 0x%04lX%s", own, denied ? " (denied)" : "");
        check("UIPI: our own integrity level is read (medium for a normal run, high when run as administrator)",
              own == SECURITY_MANDATORY_MEDIUM_RID || own == SECURITY_MANDATORY_HIGH_RID, d);
    }

    std::printf("%s (%d passed, %d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
