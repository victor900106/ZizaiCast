// pm_privacy_test (0.8.4, feat-084-privacy): 隱私黑屏 + 電腦的檔案, without a phone.
//   pm_privacy_test [--no-capture] [--keep]
// 1. paths: cleanRelPath / underRoot / resolvePcPath / listPcDir in a sandbox under %TEMP% (.., backslashes,
//    drive letters, ADS, device names, a junction and a symbolic link pointing out, one pointing in,
//    hidden + system files, a sibling folder whose name starts like the root).
// 2. Esc x3 (EscTriple).
// 3. the cover child (this exe, "--pm-privacy-mask"): window styles and affinity, never focused, quit,
//    watchdog (no heartbeat), the parent killed, "lifted at the PC" (test 'X'). Off-screen 1x1 windows only.
// 4. --capture (default on): the capture path of this program (DesktopCapture: DXGI Desktop Duplication)
//    against a 2x2 window at the primary monitor's top-left pixel for about a second: seen without the
//    affinity (control), not seen with WDA_EXCLUDEFROMCAPTURE. Never a full-screen cover.
// Prints "PASS n / FAIL n" and exits 1 on any failure. No input is sent, nothing is played.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "capture.h"
#include "pc_files.h"
#include "pm/remote.h"
#include "privacy_screen.h"
#include "util.h"

using namespace pm::remote;

namespace {

int g_pass = 0, g_fail = 0;
void check(const std::string& what, bool ok, const std::string& detail = "") {
    (ok ? g_pass : g_fail)++;
    printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : "  -- ", detail.c_str());
    fflush(stdout);
}

std::wstring exePath() {
    wchar_t p[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, p, DWORD(std::size(p)));
    return p;
}

void writeFile(const std::wstring& p, const std::string& body) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD w = 0;
    if (h != INVALID_HANDLE_VALUE) WriteFile(h, body.data(), DWORD(body.size()), &w, nullptr), CloseHandle(h);
}

// mklink /J (no admin needed): a child cmd.exe of ours, waited for.
bool makeJunction(const std::wstring& link, const std::wstring& target) {
    std::wstring cmd = L"cmd.exe /d /c mklink /J \"" + link + L"\" \"" + target + L"\" >nul 2>nul";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return GetFileAttributesW(link.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void removeTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring p = dir + L"\\" + n;
            SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {  // a link: remove the link only, never what it points to
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p.c_str());
                else DeleteFileW(p.c_str());
            } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeTree(p);
            else DeleteFileW(p.c_str());
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryW(dir.c_str());
}

// ---- 1. paths ----
void testPaths(bool keep) {
    std::wstring w;
    struct Case {
        const char* rel;
        bool ok;
    };
    const Case cases[] = {
        {"", true}, {"a.txt", true}, {"sub/b.txt", true}, {"資料夾/檔案 (1).pdf", true}, {"..", false}, {"../x", false},
        {"sub/../a.txt", false}, {"./a.txt", false}, {"...", false}, {". .", false}, {"sub\\..\\a.txt", false},
        {"C:/Windows/win.ini", false}, {"C:", false}, {"/a.txt", false}, {"a.txt/", false}, {"a//b", false}, {"//server/share/x", false},
        {"a.txt::$DATA", false}, {"a.txt:secret", false}, {"CON", false}, {"sub/NUL.txt", false}, {"com1", false}, {"LPT9.log", false},
        {"a.txt.", false}, {"a.txt ", false}, {" a.txt", false}, {"a*.txt", false}, {"a?.txt", false}, {"a\x01.txt", false}, {"\xff\xfe", false},
        {"%2e%2e/x", true},  // a literal name ("%2e%2e"), never decoded: harmless (not found)
    };
    for (const Case& c : cases) {
        const bool got = cleanRelPath(c.rel, w);
        std::string shown = c.rel;
        for (char& ch : shown)
            if (static_cast<unsigned char>(ch) < 32) ch = '?';
        check(std::string("cleanRelPath(\"") + shown + "\") " + (c.ok ? "accepted" : "refused"), got == c.ok);
    }
    std::string deep;
    for (int i = 0; i < 33; ++i) deep += (i ? "/" : "") + std::string("d");
    check("cleanRelPath: 33 parts refused", !cleanRelPath(deep, w));
    check("cleanRelPath: 1025 bytes refused", !cleanRelPath(std::string(1025, 'a'), w));

    check("underRoot: the root itself", underRoot(L"C:\\x\\Root", L"C:\\x\\Root"));
    check("underRoot: below, other case", underRoot(L"C:\\x\\Root", L"c:\\X\\root\\a\\b.txt"));
    check("underRoot: a sibling with the same start (Root2) refused", !underRoot(L"C:\\x\\Root", L"C:\\x\\Root2\\s.txt"));
    check("underRoot: the parent refused", !underRoot(L"C:\\x\\Root", L"C:\\x"));
    check("underRoot: a drive root", underRoot(L"D:\\", L"D:\\a"));
    check("underRoot: another drive refused", !underRoot(L"C:\\x", L"D:\\x\\a"));

    // The sandbox.
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring box = std::wstring(tmp) + L"pm_privacy_test_" + std::to_wstring(GetCurrentProcessId());
    const std::wstring root = box + L"\\Root", sibling = box + L"\\Root2", outside = box + L"\\outside";
    CreateDirectoryW(box.c_str(), nullptr);
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW((root + L"\\sub").c_str(), nullptr);
    CreateDirectoryW((root + L"\\資料夾").c_str(), nullptr);
    CreateDirectoryW(sibling.c_str(), nullptr);
    CreateDirectoryW(outside.c_str(), nullptr);
    writeFile(root + L"\\a.txt", "hello");
    writeFile(root + L"\\sub\\b.txt", "bbbb");
    writeFile(root + L"\\資料夾\\檔案 (1).pdf", "%PDF");
    writeFile(root + L"\\desktop.ini", "[x]");
    SetFileAttributesW((root + L"\\desktop.ini").c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    writeFile(sibling + L"\\s.txt", "sibling secret");
    writeFile(outside + L"\\secret.txt", "outside secret");
    const bool jOut = makeJunction(root + L"\\jout", outside);
    const bool jIn = makeJunction(root + L"\\jin", root + L"\\sub");
    const bool sOut = CreateSymbolicLinkW((root + L"\\slink.txt").c_str(), (outside + L"\\secret.txt").c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
    const bool sDir = CreateSymbolicLinkW((root + L"\\sdir").c_str(), outside.c_str(),
                                          SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
    check("sandbox: junctions made (out / in)", jOut && jIn);
    printf("INFO  symbolic links %s (needs Developer Mode or admin)\n", sOut && sDir ? "made" : "NOT made: those two cases are skipped");

    const std::vector<PcRoot> roots = pcRoots(L"", L"", {}, {L"test=" + root});
    check("pcRoots: the test root only (no folder of this PC)", roots.size() == 1 && roots[0].id == "test");
    std::wstring canon;
    auto res = [&](const char* rel, bool dir) { return resolvePcPath(roots, "test", rel, dir, canon); };
    check("resolve: a.txt", res("a.txt", false) == PcPathErr::Ok && underRoot(roots[0].path, canon));
    check("resolve: sub/b.txt", res("sub/b.txt", false) == PcPathErr::Ok);
    check("resolve: 資料夾/檔案 (1).pdf (UTF-8)", res("資料夾/檔案 (1).pdf", false) == PcPathErr::Ok);
    check("resolve: the root as a folder", res("", true) == PcPathErr::Ok);
    check("resolve: ../outside/secret.txt refused", res("../outside/secret.txt", false) == PcPathErr::BadPath);
    check("resolve: ../Root2/s.txt refused", res("../Root2/s.txt", false) == PcPathErr::BadPath);
    check("resolve: sub\\..\\..\\outside\\secret.txt refused", res("sub\\..\\..\\outside\\secret.txt", false) == PcPathErr::BadPath);
    check("resolve: an absolute path refused", res("C:/Windows/win.ini", false) == PcPathErr::BadPath);
    check("resolve: a.txt::$DATA refused", res("a.txt::$DATA", false) == PcPathErr::BadPath);
    check("resolve: CON refused", res("CON", false) == PcPathErr::BadPath);
    check("resolve: jout/secret.txt (junction out) refused", res("jout/secret.txt", false) == PcPathErr::Escape, pcPathErrName(res("jout/secret.txt", false)));
    check("resolve: jout (junction out, as a folder) refused", res("jout", true) == PcPathErr::Escape);
    check("resolve: jin/b.txt (junction inside the root) allowed", res("jin/b.txt", false) == PcPathErr::Ok);
    if (sOut) check("resolve: slink.txt (symbolic link out) refused", res("slink.txt", false) == PcPathErr::Escape, pcPathErrName(res("slink.txt", false)));
    if (sDir) check("resolve: sdir/secret.txt (directory symlink out) refused", res("sdir/secret.txt", false) == PcPathErr::Escape);
    check("resolve: a folder as a file refused", res("sub", false) == PcPathErr::NotFile);
    check("resolve: a file as a folder refused", res("a.txt", true) == PcPathErr::NotDir);
    check("resolve: missing", res("nope.txt", false) == PcPathErr::NotFound);
    check("resolve: unknown root", resolvePcPath(roots, "desktop", "a.txt", false, canon) == PcPathErr::NoRoot);
    check("resolve: ..\\Root2 (backslashes) refused", res("..\\Root2", true) == PcPathErr::BadPath);

    std::vector<PcEntry> list;
    bool more = false;
    const PcPathErr le = listPcDir(roots, "test", "", list, more);
    auto has = [&](const char* n) {
        for (const PcEntry& e : list)
            if (e.name == n) return true;
        return false;
    };
    std::string names;
    for (const PcEntry& e : list) names += e.name + (e.dir ? "/ " : " ");
    check("list: ok", le == PcPathErr::Ok, names);
    check("list: a.txt, sub, 資料夾, jin shown", has("a.txt") && has("sub") && has("資料夾") && has("jin"));
    check("list: jout (junction out) hidden", !has("jout"));
    check("list: desktop.ini (hidden + system) hidden", !has("desktop.ini"));
    if (sOut) check("list: slink.txt (link out) hidden", !has("slink.txt"));
    check("list: folders first", !list.empty() && list.front().dir);
    check("list: ../ refused", listPcDir(roots, "test", "..", list, more) == PcPathErr::BadPath);
    // The root itself turned into a link after the list was made: refused.
    {
        const std::wstring root3 = box + L"\\Root3";
        CreateDirectoryW(root3.c_str(), nullptr);
        writeFile(root3 + L"\\x.txt", "x");
        std::vector<PcRoot> r3 = pcRoots(L"", L"", {}, {L"r3=" + root3});
        RemoveDirectoryW((root3 + L"\\x.txt").c_str());
        DeleteFileW((root3 + L"\\x.txt").c_str());
        RemoveDirectoryW(root3.c_str());
        const bool swapped = makeJunction(root3, outside);
        check("resolve: the root swapped for a junction out is refused", swapped && resolvePcPath(r3, "r3", "secret.txt", false, canon) == PcPathErr::Escape);
    }
    if (!keep) removeTree(box);
    check("sandbox removed (links only, never their targets)", keep || GetFileAttributesW(box.c_str()) == INVALID_FILE_ATTRIBUTES);
}

// ---- 2. Esc x3 ----
void testEsc() {
    EscTriple e;
    check("Esc x3 within 2 s", !e.press(1000) && !e.press(1500) && e.press(2900));
    EscTriple f;
    check("Esc x3 over 2 s: no", !f.press(1000) && !f.press(2000) && !f.press(3100));
    check("... then the next one within 2 s of the 2nd: yes", f.press(3200) || f.press(3300));
    EscTriple g;
    check("Esc x2: no", !g.press(1) && !g.press(2));
}

// ---- 3. the cover child ----
struct Found {
    DWORD pid;
    std::vector<HWND> wins;
};
BOOL CALLBACK findWins(HWND h, LPARAM lp) {
    auto* f = reinterpret_cast<Found*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (pid == f->pid && wcscmp(cls, L"PmPrivacyCover") == 0) f->wins.push_back(h);
    return TRUE;
}
bool processGone(DWORD pid, int ms) {
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!p) return true;
    const bool gone = WaitForSingleObject(p, DWORD(ms)) == WAIT_OBJECT_0;
    CloseHandle(p);
    return gone;
}

void testCover() {
    const std::wstring exe = exePath();
    if (!privacySupported()) {
        check("Windows 10 2004+ (WDA_EXCLUDEFROMCAPTURE)", false, "this Windows is older: the switch is hidden");
        return;
    }
    check("privacySupported() on this Windows (build 19041+)", true);
    std::atomic<int> local{0}, exited{0};
    auto ev = [&](const std::string& w) { (w == "local" ? local : exited)++; };
    {
        PrivacyScreen ps;
        std::string err;
        const HWND fg = GetForegroundWindow();
        const bool ok = ps.start(exe, 0, "offscreen", ev, err);
        check("cover (offscreen) starts", ok, err);
        Found f{ps.pid(), {}};
        EnumWindows(findWins, reinterpret_cast<LPARAM>(&f));
        HMONITOR dummy = nullptr;
        (void)dummy;
        int monitors = GetSystemMetrics(SM_CMONITORS);
        check("one cover window per monitor", int(f.wins.size()) == monitors, std::to_string(f.wins.size()) + " windows, " + std::to_string(monitors) + " monitors");
        bool styles = !f.wins.empty(), aff = !f.wins.empty(), off = !f.wins.empty();
        for (HWND h : f.wins) {
            const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
            const LONG_PTR need = WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
            styles &= (ex & need) == need && !(ex & WS_EX_APPWINDOW) && GetWindow(h, GW_OWNER) == nullptr;
            DWORD a = 0;
            aff &= GetWindowDisplayAffinity(h, &a) && a == 0x11;
            RECT r{};
            GetWindowRect(h, &r);
            off &= r.right <= -31000 && (r.right - r.left) == 1;
        }
        check("styles: TOPMOST | TRANSPARENT | LAYERED | NOACTIVATE | TOOLWINDOW (no taskbar / Alt+Tab, click-through)", styles);
        check("affinity: WDA_EXCLUDEFROMCAPTURE on every window", aff);
        check("test mode: 1x1 and off every screen", off);
        check("never focused (the foreground window is unchanged)", GetForegroundWindow() == fg);
        ps.beat();
        const DWORD pid = ps.pid();
        ps.stop();
        check("stop: the cover process ends", processGone(pid, 2000));
    }
    {
        PrivacyScreen ps;
        std::string err;
        ps.start(exe, 1, "offscreen", ev, err);
        const DWORD pid = ps.pid();
        const ULONGLONG t0 = GetTickCount64();
        const bool gone = processGone(pid, kPrivacyWatchdogMs + 4000);  // no beat()
        const ULONGLONG ms = GetTickCount64() - t0;
        Sleep(100);
        check("watchdog: no heartbeat -> the cover ends by itself", gone && ms >= 4000, std::to_string(ms) + " ms");
        check("watchdog: the host is told (exited)", exited >= 1);
        ps.stop();
    }
    {
        PrivacyScreen ps;
        std::string err;
        ps.start(exe, 2, "offscreen", ev, err);
        const DWORD pid = ps.pid();
        ps.testUnlock();
        const bool gone = processGone(pid, 3000);
        Sleep(100);
        check("lifted at the PC (as Ctrl+Alt+Shift+B / Esc x3): the cover ends, the host hears \"local\"", gone && local == 1);
        ps.stop();
    }
    {
        // The parent killed (a crash): a helper process of ours starts the cover, then we kill the helper.
        std::wstring cmd = L"\"" + exe + L"\" --as-parent";
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE r = nullptr, w = nullptr;
        CreatePipe(&r, &w, &sa, 0);
        SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = w, si.hStdError = w, si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi{};
        const bool ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != 0;
        CloseHandle(w);
        std::string out;
        char c;
        DWORD got = 0;
        // The helper's log lines come first on the same pipe: wait for its own "cover <pid>" line.
        size_t at = std::string::npos;
        while (ok && (at == std::string::npos || out.find('\n', at) == std::string::npos) && out.size() < 8192 && ReadFile(r, &c, 1, &got, nullptr) && got) {
            out += c;
            if (at == std::string::npos) {
                const size_t p = out.find("\ncover ");
                if (out.rfind("cover ", 0) == 0) at = 0;
                else if (p != std::string::npos) at = p + 1;
            }
        }
        const DWORD coverPid = at == std::string::npos ? 0 : strtoul(out.c_str() + at + 6, nullptr, 10);
        if (coverPid) {  // it must really be a running cover of ours before the kill means anything
            HANDLE cp = OpenProcess(SYNCHRONIZE, FALSE, coverPid);
            check("the helper's cover process is running before the kill", cp && WaitForSingleObject(cp, 0) == WAIT_TIMEOUT);
            if (cp) CloseHandle(cp);
        }
        out = at == std::string::npos ? out.substr(0, 200) : out.substr(at, out.find('\n', at) - at);
        check("helper parent started a cover", ok && coverPid != 0, out);
        if (ok) {
            TerminateProcess(pi.hProcess, 9);  // our own helper
            WaitForSingleObject(pi.hProcess, 3000);
            check("parent killed -> the cover ends within 2 s (job / pipe / parent watch)", coverPid && processGone(coverPid, 2000));
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        CloseHandle(r);
    }
}

int asParent() {
    PrivacyScreen ps;
    std::string err;
    if (!ps.start(exePath(), 0, "offscreen", nullptr, err)) {
        printf("failed %s\n", err.c_str());
        return 1;
    }
    printf("cover %lu\n", ps.pid());
    fflush(stdout);
    for (int i = 0; i < 300; ++i) {  // until killed (at most 30 s)
        ps.beat();
        Sleep(100);
    }
    return 0;
}

// ---- 4. against the capture ----
struct Yuv {
    double y = 0, u = 0, v = 0;
};
bool grab(DesktopCapture& cap, Yuv& out, int waitMs) {
    bool changed = false;
    const ULONGLONG t0 = GetTickCount64();
    do {
        if (!cap.poll(50, changed)) return false;
    } while (!changed && GetTickCount64() - t0 < ULONGLONG(waitMs));
    const int slot = cap.render(false, 0);
    if (slot < 0) return false;
    std::vector<uint8_t> nv12;
    const bool ok = cap.readback(slot, nv12);
    cap.release(slot);
    if (!ok) return false;
    const int w = cap.width(), h = cap.height();
    out.y = (nv12[0] + nv12[1] + nv12[size_t(w)] + nv12[size_t(w) + 1]) / 4.0;
    out.u = nv12[size_t(w) * h], out.v = nv12[size_t(w) * h + 1];
    return true;
}
bool magentaLike(const Yuv& c) { return c.u > 180 && c.v > 190; }
bool closeTo(const Yuv& a, const Yuv& b) { return std::abs(a.y - b.y) <= 14 && std::abs(a.u - b.u) <= 14 && std::abs(a.v - b.v) <= 14; }
std::string str(const Yuv& c) {
    char b[64];
    snprintf(b, sizeof(b), "Y%.0f U%.0f V%.0f", c.y, c.u, c.v);
    return b;
}

void testCapture() {
    const std::wstring exe = exePath();
    const auto mons = enumMonitors();
    if (mons.empty()) return check("capture: a monitor", false);
    DesktopCapture cap;
    DesktopCapture::Options co;
    co.drawCursor = false;
    co.monitor = 0;
    co.outWidth = (mons[0].rect.right - mons[0].rect.left) & ~1, co.outHeight = (mons[0].rect.bottom - mons[0].rect.top) & ~1;
    co.maxWidth = co.outWidth, co.maxHeight = co.outHeight;
    co.poolSize = 2;
    if (!cap.open(co)) return check("capture: DXGI Desktop Duplication opens (the program's own capture class)", false);
    check("capture: DXGI Desktop Duplication opens (the program's own capture class)", true,
          std::to_string(cap.width()) + "x" + std::to_string(cap.height()) + " on " + cap.adapterName());
    Yuv base, ctl, test, after;
    grab(cap, base, 300);
    grab(cap, base, 100);
    auto run = [&](const char* mode, Yuv& got) {
        PrivacyScreen ps;
        std::string err;
        if (!ps.start(exe, 0, mode, nullptr, err)) return check(std::string("cover ") + mode + " starts", false, err), false;
        Sleep(120);  // shown + composed
        bool ok = grab(cap, got, 800);
        Sleep(100);
        ok = grab(cap, got, 300) || ok;
        ps.stop();
        return ok;
    };
    const bool c1 = run("tiny-noexclude", ctl);
    Sleep(250);
    const bool c2 = run("tiny", test);
    Sleep(250);
    grab(cap, after, 400);
    printf("INFO  top-left pixel: before %s | control (no affinity) %s | with WDA_EXCLUDEFROMCAPTURE %s | after %s\n", str(base).c_str(),
           str(ctl).c_str(), str(test).c_str(), str(after).c_str());
    const bool baseMagenta = magentaLike(base);
    check("capture: the desktop corner is not magenta itself (the test can tell)", !baseMagenta);
    check("capture: control -- a normal 2x2 window IS in the captured picture", c1 && magentaLike(ctl), str(ctl));
    check("capture: the cover's window (WDA_EXCLUDEFROMCAPTURE) is NOT in the captured picture", c2 && !magentaLike(test) && (closeTo(test, base) || closeTo(test, after)),
          str(test));
    cap.close();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (const int pc = runPrivacyMaskIfAsked(); pc >= 0) return pc;  // the cover child
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleOutputCP(CP_UTF8);
    bool capture = true, keep = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--as-parent") return asParent();
        if (a == L"--no-capture") capture = false;
        if (a == L"--keep") keep = true;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    setLogHandler([](const std::string&) {});
    printf("== paths ==\n");
    testPaths(keep);
    printf("== Esc x3 ==\n");
    testEsc();
    printf("== cover child ==\n");
    testCover();
    if (capture) {
        printf("== cover vs the capture (2x2 at the primary monitor's top-left, ~1 s) ==\n");
        testCapture();
    }
    printf("\nPASS %d / FAIL %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
