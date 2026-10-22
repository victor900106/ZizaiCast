#include "input.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "coords.h"
#include "pm/remote.h"
#include "util.h"

namespace pm::remote {

namespace {

const char* buttonName(int b) { return b == 4 ? "forward" : b == 3 ? "back" : b == 2 ? "right" : b == 1 ? "middle" : "left"; }

bool keyVk(const std::string& k, WORD& vk, bool& ext) {
    struct K {
        const char* name;
        WORD vk;
        bool ext;
    };
    static const K keys[] = {{"Enter", VK_RETURN, false},    {"Backspace", VK_BACK, false},  {"Escape", VK_ESCAPE, false},
                             {"Tab", VK_TAB, false},         {"Delete", VK_DELETE, true},    {"ArrowLeft", VK_LEFT, true},
                             {"ArrowRight", VK_RIGHT, true}, {"ArrowUp", VK_UP, true},       {"ArrowDown", VK_DOWN, true},
                             {"Home", VK_HOME, true},        {"End", VK_END, true},          {"PageUp", VK_PRIOR, true},
                             {"PageDown", VK_NEXT, true},    {"Insert", VK_INSERT, true},    {" ", VK_SPACE, false},
                             {"Space", VK_SPACE, false},     {"Control", VK_CONTROL, false}, {"Alt", VK_MENU, false},
                             {"Shift", VK_SHIFT, false},     {"Meta", VK_LWIN, true},        {"PrintScreen", VK_SNAPSHOT, true},
                             {"ContextMenu", VK_APPS, true}};
    for (const K& x : keys)
        if (k == x.name) return vk = x.vk, ext = x.ext, true;
    ext = false;
    if (k.size() == 1) {  // a letter / digit (combos like Ctrl+C use the key, not the character)
        const char c = k[0];
        if (c >= 'a' && c <= 'z') return vk = WORD(c - 'a' + 'A'), true;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return vk = WORD(c), true;
    }
    if (k.size() >= 2 && k.size() <= 3 && k[0] == 'F') {  // F1..F12
        const int n = atoi(k.c_str() + 1);
        if (n >= 1 && n <= 12) return vk = WORD(VK_F1 + n - 1), true;
    }
    return false;
}

void mouseFlagsName(DWORD f, std::string& out) {
    static const struct {
        DWORD f;
        const char* n;
    } names[] = {{MOUSEEVENTF_MOVE, "MOVE"},         {MOUSEEVENTF_ABSOLUTE, "ABSOLUTE"}, {MOUSEEVENTF_VIRTUALDESK, "VIRTUALDESK"},
                 {MOUSEEVENTF_LEFTDOWN, "LEFTDOWN"}, {MOUSEEVENTF_LEFTUP, "LEFTUP"},     {MOUSEEVENTF_RIGHTDOWN, "RIGHTDOWN"},
                 {MOUSEEVENTF_RIGHTUP, "RIGHTUP"},   {MOUSEEVENTF_MIDDLEDOWN, "MIDDLEDOWN"}, {MOUSEEVENTF_MIDDLEUP, "MIDDLEUP"},
                 {MOUSEEVENTF_XDOWN, "XDOWN"},       {MOUSEEVENTF_XUP, "XUP"},           {MOUSEEVENTF_WHEEL, "WHEEL"},
                 {MOUSEEVENTF_HWHEEL, "HWHEEL"}};
    for (const auto& x : names)
        if (f & x.f) out += (out.empty() ? "" : "|") + std::string(x.n);
}

}  // namespace

// Every event goes out here. stub: logged, never sent (tests).
UINT InputInjector::sendInputs(UINT n, INPUT* in, const char* what) {
    if (!stub_) {
        const UINT sent = SendInput(n, in, sizeof(INPUT));
        if (sent != n) {
            const DWORD err = GetLastError();
            logf("input: SendInput(%s) failed (error %lu; the window may run as administrator)", what, err);
            if (err == ERROR_ACCESS_DENIED && onBlocked) onBlocked("admin", "");
        }
        return sent;
    }
    for (UINT i = 0; i < n; ++i) {
        const INPUT& x = in[i];
        if (x.type == INPUT_MOUSE) {
            std::string f;
            mouseFlagsName(x.mi.dwFlags, f);
            if (x.mi.dwFlags & MOUSEEVENTF_ABSOLUTE) {
                // what Windows does with it: normalised 0..65535 onto the virtual desktop
                const RECT vd = virtualDesk();
                stubCur_.x = fromAbsolute(x.mi.dx, vd.left, vd.right - vd.left), stubCur_.y = fromAbsolute(x.mi.dy, vd.top, vd.bottom - vd.top);
            }
            if (x.mi.dwFlags & ~DWORD(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK))  // not every move: too many lines
                logf("input stub: STUB SendInput mouse %s data %ld at %ld,%ld", f.c_str(), long(x.mi.mouseData), stubCur_.x, stubCur_.y);
        } else if (x.type == INPUT_KEYBOARD) {
            logf("input stub: STUB SendInput key vk 0x%02X scan 0x%02X%s%s%s", x.ki.wVk, x.ki.wScan, x.ki.dwFlags & KEYEVENTF_EXTENDEDKEY ? " ext" : "",
                 x.ki.dwFlags & KEYEVENTF_UNICODE ? " unicode" : "", x.ki.dwFlags & KEYEVENTF_KEYUP ? " up" : " down");
        }
    }
    return n;
}

POINT InputInjector::cursorPos() {
    POINT p{};
    if (stub_) return stubCur_;
    GetCursorPos(&p);
    return p;
}

void InputInjector::sendMouse(DWORD flags, LONG dx, LONG dy, DWORD data) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx, in.mi.dy = dy, in.mi.mouseData = data, in.mi.dwFlags = flags;
    in.mi.dwExtraInfo = kInjectedInputTag;
    sendInputs(1, &in, "mouse");
}

void InputInjector::sendVk(WORD vk, bool extended) {
    INPUT in[2] = {};
    for (int i = 0; i < 2; ++i) {
        in[i].type = INPUT_KEYBOARD;
        in[i].ki.wVk = vk;
        in[i].ki.wScan = WORD(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        in[i].ki.dwFlags = (extended ? KEYEVENTF_EXTENDEDKEY : 0) | (i ? KEYEVENTF_KEYUP : 0);
        in[i].ki.dwExtraInfo = kInjectedInputTag;
    }
    sendInputs(2, in, "key");
}

void InputInjector::sendText(const std::wstring& w) {
    std::vector<INPUT> v;
    auto flush = [&] {
        if (!v.empty()) sendInputs(UINT(v.size()), v.data(), "text");
        v.clear();
    };
    for (wchar_t c : w) {
        if (c == L'\n' || c == L'\r') {
            flush();
            sendVk(VK_RETURN, false);
            continue;
        }
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = c;
        in.ki.dwFlags = KEYEVENTF_UNICODE;
        in.ki.dwExtraInfo = kInjectedInputTag;
        v.push_back(in);
        in.ki.dwFlags |= KEYEVENTF_KEYUP;
        v.push_back(in);
    }
    flush();
}

void InputInjector::setScreen(const RECT& r) {
    std::lock_guard<std::mutex> lk(mu_);
    screen_ = r;
}

bool InputInjector::allowed(bool keyboard) {
    if (!inject_) return false;
    if (guard && !guard(keyboard)) {
        ++blocked_;
        logf("input: not injected (test guard: %s outside the test window)", keyboard ? "key" : "click");
        return false;
    }
    checkTarget(keyboard);
    return true;
}

// ---- 0.8.3.5: UIPI. Windows silently drops input sent to a window of a higher integrity level
// (a program run as administrator) or to the secure desktop (UAC prompt, lock screen):
// SendInput even reports success. So the target is checked before a click / key: the window
// under the pointer (mouse) or the foreground window (keys), its process' integrity level
// against ours. The phone is told (onBlocked, at most once per 3 s per program).

static DWORD processIntegrity(HANDLE process, bool& denied) {
    denied = false;
    HANDLE tok = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &tok)) {
        denied = GetLastError() == ERROR_ACCESS_DENIED;
        return 0;
    }
    DWORD rid = 0, len = 0;
    GetTokenInformation(tok, TokenIntegrityLevel, nullptr, 0, &len);
    std::vector<BYTE> buf(std::max<DWORD>(len, sizeof(TOKEN_MANDATORY_LABEL)));
    if (len && GetTokenInformation(tok, TokenIntegrityLevel, buf.data(), len, &len)) {
        const auto* tml = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buf.data());
        const UCHAR n = *GetSidSubAuthorityCount(tml->Label.Sid);
        if (n) rid = *GetSidSubAuthority(tml->Label.Sid, DWORD(n - 1));
    }
    CloseHandle(tok);
    return rid;
}

InputInjector::Target InputInjector::realTarget(bool keyboard, POINT at) {
    Target t;
    // the secure desktop (UAC prompt, lock screen, Ctrl+Alt+Del screen): no input reaches it
    if (HDESK d = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS)) {
        wchar_t name[64] = {};
        DWORD need = 0;
        if (GetUserObjectInformationW(d, UOI_NAME, name, sizeof(name), &need) && _wcsicmp(name, L"Default") != 0) t.secure = true;
        CloseDesktop(d);
    } else if (GetLastError() == ERROR_ACCESS_DENIED) {
        t.secure = true;  // cannot even open it: Winlogon's desktop
    }
    if (t.secure) return t;
    HWND w = keyboard ? GetForegroundWindow() : WindowFromPoint(at);
    if (!w) return t;
    w = GetAncestor(w, GA_ROOT);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    t.pid = pid;
    if (!pid || pid == GetCurrentProcessId()) return t;
    const int64_t now = nowUs();
    for (auto& c : ilCache_)
        if (c.pid == pid && now - c.atUs < 2000000) {
            t.higher = c.higher, t.exe = c.exe;
            return t;
        }
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p) {
        wchar_t path[MAX_PATH] = {};
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &n)) {
            const wchar_t* base = wcsrchr(path, L'\\');
            t.exe = narrow(base ? base + 1 : path);
        }
        bool denied = false;
        const DWORD rid = processIntegrity(p, denied);
        t.higher = denied || (rid && ownIntegrity_ && rid > ownIntegrity_);
        CloseHandle(p);
    } else {
        t.higher = GetLastError() == ERROR_ACCESS_DENIED;  // a protected / elevated process
    }
    if (ilCache_.size() > 16) ilCache_.erase(ilCache_.begin());
    ilCache_.push_back({pid, now, t.higher, t.exe});
    return t;
}

DWORD InputInjector::integrityOf(HANDLE process, bool& denied) { return processIntegrity(process, denied); }

void InputInjector::checkTarget(bool keyboard) {
    if (!ownIntegrity_) {
        bool denied = false;
        ownIntegrity_ = processIntegrity(GetCurrentProcess(), denied);
        if (!ownIntegrity_) ownIntegrity_ = SECURITY_MANDATORY_MEDIUM_RID;
    }
    const Target t = targetProbe ? targetProbe(keyboard, cursorPos()) : realTarget(keyboard, cursorPos());
    if (!t.secure && !t.higher) return;
    const int64_t now = nowUs();
    const std::string key = t.secure ? std::string("#secure") : t.exe + "#" + std::to_string(t.pid);
    if (key == lastBlockedKey_ && now - lastBlockedUs_ < 3000000) return;
    lastBlockedKey_ = key, lastBlockedUs_ = now;
    ++uipiBlocked_;
    if (t.secure)
        logf("input: the PC shows the secure desktop (UAC prompt / lock screen): %s not delivered (Windows blocks it)", keyboard ? "key" : "click");
    else
        logf("input: %s runs as administrator (integrity above ours): %s not delivered (UIPI)", t.exe.empty() ? "the window" : t.exe.c_str(),
             keyboard ? "key" : "click");
    if (onBlocked) onBlocked(t.secure ? "secure" : "admin", t.exe);
}

// Absolute target in virtual-desktop pixels -> SendInput (normalised to the
// whole virtual desktop): no Windows pointer acceleration on top of the
// page's own.
void InputInjector::moveTo(double x, double y) {
    if (moveGuard && !moveGuard(int(std::lround(x)), int(std::lround(y)))) {
        ++blocked_;
        if (blocked_ < 20 || blocked_ % 100 == 0)
            logf("input: pointer move to %.0f,%.0f not injected (test guard: outside the test window)", x, y);
        return;
    }
    // 0.8.3.5: the middle of the pixel (coords.h), exact on every pixel of any layout
    const RECT vd = virtualDesk();
    const LONG ax = toAbsolute(LONG(std::lround(x)), vd.left, vd.right - vd.left);
    const LONG ay = toAbsolute(LONG(std::lround(y)), vd.top, vd.bottom - vd.top);
    sendMouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, ax, ay);
}

RECT InputInjector::virtualDesk() const {
    if (testDesk_.right > testDesk_.left && testDesk_.bottom > testDesk_.top) return testDesk_;
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    return RECT{vx, vy, vx + GetSystemMetrics(SM_CXVIRTUALSCREEN), vy + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

// 0 left, 1 middle, 2 right, 3 back (XBUTTON1), 4 forward (XBUTTON2)
void InputInjector::button(int b, bool down) {
    if (b < 0 || b >= kButtons) return;
    if (down && !allowed(false)) return;  // a release always goes out (never leaves a button held)
    if (!down && !held_[b]) return;
    held_[b] = down;
    if (!inject_) return;
    static const DWORD dn[kButtons] = {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_XDOWN, MOUSEEVENTF_XDOWN};
    static const DWORD upf[kButtons] = {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_XUP, MOUSEEVENTF_XUP};
    sendMouse(down ? dn[b] : upf[b], 0, 0, b == 3 ? XBUTTON1 : b == 4 ? XBUTTON2 : 0);
}

bool InputInjector::handle(const std::string& t, double dx, double dy, int b, const std::string& text) {
    if (t == "release") {
        releaseAll();
        return true;
    }
    std::lock_guard<std::mutex> lk(mu_);
    ++events_;
    const char* mode = stub_ ? " (stub)" : inject_ ? "" : " (dry-run)";
    const bool haveScreen = screen_.right > screen_.left && screen_.bottom > screen_.top;
    if (t == "move") {
        if (!std::isfinite(dx) || !std::isfinite(dy) || std::fabs(dx) > 1e5 || std::fabs(dy) > 1e5) return false;
        ++moveN_, moveDx_ += dx, moveDy_ += dy;
        if (inject_) {
            fracX_ += dx, fracY_ += dy;
            const LONG ix = LONG(std::trunc(fracX_)), iy = LONG(std::trunc(fracY_));
            fracX_ -= ix, fracY_ -= iy;
            if (ix || iy) {
                const POINT p = cursorPos();
                const RECT r = haveScreen ? screen_ : virtualDesk();
                // Stays on the streamed monitor: the phone always sees the cursor.
                moveTo(std::clamp(double(p.x + ix), double(r.left), double(r.right - 1)),
                       std::clamp(double(p.y + iy), double(r.top), double(r.bottom - 1)));
            }
        }
    } else if (t == "abs") {
        if (!std::isfinite(dx) || !std::isfinite(dy) || !haveScreen) return false;
        ++absN_;
        const POINT px = normToDesktop(dx, dy, screen_);  // 0.8.3.5: floor(x * w), see coords.h
        lastAbs_ = px;
        fracX_ = fracY_ = 0;
        if (inject_) moveTo(px.x, px.y);
    } else if (t == "wheel") {
        if (!std::isfinite(dx) || !std::isfinite(dy) || std::fabs(dx) > 1e5 || std::fabs(dy) > 1e5) return false;
        ++wheelN_, wheelDx_ += dx, wheelDy_ += dy;
        wheelY_ += dy, wheelX_ += dx;
        const LONG iy = LONG(std::trunc(wheelY_)), ix = LONG(std::trunc(wheelX_));
        wheelY_ -= iy, wheelX_ -= ix;
        if ((iy || ix) && allowed(false)) {
            if (iy) sendMouse(MOUSEEVENTF_WHEEL, 0, 0, DWORD(iy));
            if (ix) sendMouse(MOUSEEVENTF_HWHEEL, 0, 0, DWORD(ix));
        }
    } else if (t == "down" || t == "up") {
        if (b < 0 || b >= kButtons) return false;
        lastPressUs_ = nowUs();
        const POINT p = cursorPos();
        logf("input%s: %s %s at %ld,%ld", mode, buttonName(b), t == "down" ? "down" : "up", p.x, p.y);
        button(b, t == "down");
    } else if (t == "click") {
        if (b < 0 || b >= kButtons) return false;
        lastPressUs_ = nowUs();
        const POINT p = cursorPos();
        logf("input%s: %s click at %ld,%ld", mode, buttonName(b), p.x, p.y);
        if (held_[b]) button(b, false);
        button(b, true);
        button(b, false);
    } else if (t == "text") {
        if (text.empty() || text.size() > 4096) return false;
        lastPressUs_ = nowUs();
        logf("input%s: text (%zu bytes)", mode, text.size());
        if (allowed(true)) sendText(widen(text));
    } else if (t == "key") {
        WORD vk = 0;
        bool ext = false;
        lastPressUs_ = nowUs();
        if (!keyVk(text, vk, ext)) return false;
        logf("input%s: key %s", mode, text.c_str());
        if (allowed(true)) sendVk(vk, ext);
    } else if (t == "rel") {  // 0.8.3.5 遊戲模式 (pointer lock): raw relative motion, as a real mouse sends it
        if (!std::isfinite(dx) || !std::isfinite(dy) || std::fabs(dx) > 1e4 || std::fabs(dy) > 1e4) return false;
        ++relN_, relDx_ += dx, relDy_ += dy;
        relFx_ += dx, relFy_ += dy;
        const LONG ix = LONG(std::trunc(relFx_)), iy = LONG(std::trunc(relFy_));
        relFx_ -= ix, relFy_ -= iy;
        if ((ix || iy) && inject_) {
            if (moveGuard) {
                const POINT p = cursorPos();
                if (!moveGuard(p.x + ix, p.y + iy)) return true;
            }
            if (stub_) {
                const RECT vd = virtualDesk();
                stubCur_.x = std::clamp(stubCur_.x + ix, vd.left, vd.right - 1), stubCur_.y = std::clamp(stubCur_.y + iy, vd.top, vd.bottom - 1);
            }
            sendMouse(MOUSEEVENTF_MOVE, ix, iy);
        }
    } else if (t == "kd" || t == "ku") {  // 0.8.3.5: a physical key (KeyboardEvent.code) of a PC's keyboard
        return physKey(text, t == "kd");
    } else if (t == "kh") {  // keys still held on the page (sent every 0.4 s while any is)
        lastKeyMsgUs_ = nowUs();
        --events_;
    } else {
        --events_;
        return false;
    }
    return true;
}

// Every key down in order, then up in reverse, in ONE SendInput call: the
// keys can never stay stuck, whatever happens to the connection.
bool InputInjector::combo(const std::vector<std::string>& keys) {
    if (keys.empty()) return false;
    {
        auto has = [&](const char* k) { return std::find(keys.begin(), keys.end(), k) != keys.end(); };
        if (has("Control") && has("Alt") && has("Delete")) {  // 0.8.3.5: the secure attention sequence
            std::lock_guard<std::mutex> lk(mu_);
            logf("input: Ctrl+Alt+Del cannot be sent by any program (Windows' secure attention sequence)");
            if (onBlocked) onBlocked("sas", "");
            return true;
        }
    }
    std::vector<INPUT> in;
    std::string desc;
    for (const auto& k : keys) {
        WORD vk = 0;
        bool ext = false;
        if (!keyVk(k, vk, ext)) return false;
        INPUT x{};
        x.type = INPUT_KEYBOARD;
        x.ki.wVk = vk;
        x.ki.wScan = WORD(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        x.ki.dwFlags = ext ? KEYEVENTF_EXTENDEDKEY : 0;
        x.ki.dwExtraInfo = kInjectedInputTag;
        in.push_back(x);
        desc += (desc.empty() ? "" : "+") + k;
    }
    for (size_t i = keys.size(); i-- > 0;) {
        INPUT x = in[i];
        x.ki.dwFlags |= KEYEVENTF_KEYUP;
        in.push_back(x);
    }
    lastPressUs_ = nowUs();
    std::lock_guard<std::mutex> lk(mu_);
    ++events_;
    logf("input%s: combo %s", stub_ ? " (stub)" : inject_ ? "" : " (dry-run)", desc.c_str());
    if (allowed(true)) sendInputs(UINT(in.size()), in.data(), "combo");
    return true;
}

void InputInjector::releaseAll() {
    std::lock_guard<std::mutex> lk(mu_);
    for (int b = 0; b < kButtons; ++b)
        if (held_[b]) {
            logf("input%s: releasing held %s button", stub_ ? " (stub)" : inject_ ? "" : " (dry-run)", buttonName(b));
            button(b, false);
        }
    releaseKeysLocked("released");
}

// ---- 0.8.3.5 (b): physical keys of a PC's keyboard -----------------------------------------
// KeyboardEvent.code (the key's place, whatever the layout) -> its scan code (set 1; 0xE0xx =
// extended): the PC's own layout / input method then makes the character, games get the scan
// code. A key may be held (Shift + click, games, auto-repeat = repeated downs); it is never left
// down: released on "ku", "release", page hidden / gone (the host's releaseAll) and when the
// page's heartbeat ("kh", 0.4 s) stops for 1.5 s.
static bool codeScan(const std::string& c, WORD& scan, WORD& vk) {
    struct C {
        const char* code;
        WORD scan;
        WORD vk;  // != 0: sent as a virtual key (its scan code is not enough)
    };
    static const C t[] = {
        {"Escape", 0x01, 0},       {"Digit1", 0x02, 0},       {"Digit2", 0x03, 0},       {"Digit3", 0x04, 0},       {"Digit4", 0x05, 0},
        {"Digit5", 0x06, 0},       {"Digit6", 0x07, 0},       {"Digit7", 0x08, 0},       {"Digit8", 0x09, 0},       {"Digit9", 0x0A, 0},
        {"Digit0", 0x0B, 0},       {"Minus", 0x0C, 0},        {"Equal", 0x0D, 0},        {"Backspace", 0x0E, 0},    {"Tab", 0x0F, 0},
        {"KeyQ", 0x10, 0},         {"KeyW", 0x11, 0},         {"KeyE", 0x12, 0},         {"KeyR", 0x13, 0},         {"KeyT", 0x14, 0},
        {"KeyY", 0x15, 0},         {"KeyU", 0x16, 0},         {"KeyI", 0x17, 0},         {"KeyO", 0x18, 0},         {"KeyP", 0x19, 0},
        {"BracketLeft", 0x1A, 0},  {"BracketRight", 0x1B, 0}, {"Enter", 0x1C, 0},        {"ControlLeft", 0x1D, 0},  {"KeyA", 0x1E, 0},
        {"KeyS", 0x1F, 0},         {"KeyD", 0x20, 0},         {"KeyF", 0x21, 0},         {"KeyG", 0x22, 0},         {"KeyH", 0x23, 0},
        {"KeyJ", 0x24, 0},         {"KeyK", 0x25, 0},         {"KeyL", 0x26, 0},         {"Semicolon", 0x27, 0},    {"Quote", 0x28, 0},
        {"Backquote", 0x29, 0},    {"ShiftLeft", 0x2A, 0},    {"Backslash", 0x2B, 0},    {"KeyZ", 0x2C, 0},         {"KeyX", 0x2D, 0},
        {"KeyC", 0x2E, 0},         {"KeyV", 0x2F, 0},         {"KeyB", 0x30, 0},         {"KeyN", 0x31, 0},         {"KeyM", 0x32, 0},
        {"Comma", 0x33, 0},        {"Period", 0x34, 0},       {"Slash", 0x35, 0},        {"ShiftRight", 0x36, 0},   {"NumpadMultiply", 0x37, 0},
        {"AltLeft", 0x38, 0},      {"Space", 0x39, 0},        {"CapsLock", 0x3A, 0},     {"F1", 0x3B, 0},           {"F2", 0x3C, 0},
        {"F3", 0x3D, 0},           {"F4", 0x3E, 0},           {"F5", 0x3F, 0},           {"F6", 0x40, 0},           {"F7", 0x41, 0},
        {"F8", 0x42, 0},           {"F9", 0x43, 0},           {"F10", 0x44, 0},          {"NumLock", 0x45, VK_NUMLOCK}, {"ScrollLock", 0x46, 0},
        {"Numpad7", 0x47, 0},      {"Numpad8", 0x48, 0},      {"Numpad9", 0x49, 0},      {"NumpadSubtract", 0x4A, 0}, {"Numpad4", 0x4B, 0},
        {"Numpad5", 0x4C, 0},      {"Numpad6", 0x4D, 0},      {"NumpadAdd", 0x4E, 0},    {"Numpad1", 0x4F, 0},      {"Numpad2", 0x50, 0},
        {"Numpad3", 0x51, 0},      {"Numpad0", 0x52, 0},      {"NumpadDecimal", 0x53, 0}, {"IntlBackslash", 0x56, 0}, {"F11", 0x57, 0},
        {"F12", 0x58, 0},          {"F13", 0x64, 0},          {"F14", 0x65, 0},          {"F15", 0x66, 0},          {"F16", 0x67, 0},
        {"F17", 0x68, 0},          {"F18", 0x69, 0},          {"F19", 0x6A, 0},          {"F20", 0x6B, 0},          {"F21", 0x6C, 0},
        {"F22", 0x6D, 0},          {"F23", 0x6E, 0},          {"F24", 0x76, 0},          {"KanaMode", 0x70, 0},     {"IntlRo", 0x73, 0},
        {"Convert", 0x79, 0},      {"NonConvert", 0x7B, 0},   {"IntlYen", 0x7D, 0},      {"NumpadComma", 0x7E, 0},  {"Lang1", 0xF2, VK_HANGUL},
        {"Lang2", 0xF1, VK_HANJA}, {"NumpadEnter", 0xE01C, 0}, {"ControlRight", 0xE01D, 0}, {"NumpadDivide", 0xE035, 0}, {"PrintScreen", 0xE037, 0},
        {"AltRight", 0xE038, 0},   {"Pause", 0x45, VK_PAUSE}, {"Home", 0xE047, 0},       {"ArrowUp", 0xE048, 0},    {"PageUp", 0xE049, 0},
        {"ArrowLeft", 0xE04B, 0},  {"ArrowRight", 0xE04D, 0}, {"End", 0xE04F, 0},        {"ArrowDown", 0xE050, 0},  {"PageDown", 0xE051, 0},
        {"Insert", 0xE052, 0},     {"Delete", 0xE053, 0},     {"MetaLeft", 0xE05B, 0},   {"MetaRight", 0xE05C, 0},  {"ContextMenu", 0xE05D, 0},
        {"OSLeft", 0xE05B, 0},     {"OSRight", 0xE05C, 0},
    };
    for (const C& x : t)
        if (c == x.code) return scan = x.scan, vk = x.vk, true;
    return false;
}

void InputInjector::sendScan(WORD scan, WORD vk, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.dwExtraInfo = kInjectedInputTag;
    const bool ext = (scan & 0xFF00) == 0xE000;
    if (vk) {
        in.ki.wVk = vk;
        in.ki.wScan = WORD(scan & 0xFF);
        in.ki.dwFlags = (vk == VK_NUMLOCK ? KEYEVENTF_EXTENDEDKEY : 0);
    } else {
        in.ki.wScan = WORD(scan & 0xFF);
        in.ki.dwFlags = KEYEVENTF_SCANCODE | (ext ? KEYEVENTF_EXTENDEDKEY : 0);
    }
    if (up) in.ki.dwFlags |= KEYEVENTF_KEYUP;
    sendInputs(1, &in, "key");
}

bool InputInjector::physKey(const std::string& code, bool down) {
    WORD scan = 0, vk = 0;
    if (code.size() > 24 || !codeScan(code, scan, vk)) return false;
    const int64_t now = nowUs();
    lastKeyMsgUs_ = now;
    auto it = std::find_if(keysHeld_.begin(), keysHeld_.end(), [&](const HeldKey& k) { return k.code == code; });
    if (!down) {
        if (it == keysHeld_.end()) return true;  // never pressed here (or already released)
        keysHeld_.erase(it);
        if (inject_) sendScan(scan, vk, true);
        return true;
    }
    lastPressUs_ = now;
    ++keyDowns_;
    auto held = [&](const char* a, const char* b) {
        return std::any_of(keysHeld_.begin(), keysHeld_.end(), [&](const HeldKey& k) { return k.code == a || k.code == b; });
    };
    // Ctrl+Alt+Del is Windows' secure attention sequence: no program can send it (SendInput is
    // ignored), so say so instead of pretending.
    if (code == "Delete" && held("ControlLeft", "ControlRight") && held("AltLeft", "AltRight")) {
        logf("input: Ctrl+Alt+Del cannot be sent by any program (Windows' secure attention sequence)");
        if (onBlocked) onBlocked("sas", "");
        return true;
    }
    if (it == keysHeld_.end()) {
        if (keysHeld_.size() >= 16) return true;  // a page that never sends ups: at most 16 held
        if (!allowed(true)) return true;
        keysHeld_.push_back({code, scan, vk});
    } else if (!inject_ || (guard && !guard(true))) {
        return true;  // auto-repeat of a key that was not let through
    }
    if (inject_) sendScan(scan, vk, false);  // a second down = the key's auto-repeat
    if (stub_ && onStubCopy && code == "KeyC" && held("ControlLeft", "ControlRight")) onStubCopy();
    return true;
}

void InputInjector::releaseKeysLocked(const char* why) {
    if (keysHeld_.empty()) return;
    logf("input%s: %zu key(s) let go (%s)", stub_ ? " (stub)" : inject_ ? "" : " (dry-run)", keysHeld_.size(), why);
    for (size_t i = keysHeld_.size(); i-- > 0;)
        if (inject_) sendScan(keysHeld_[i].scan, keysHeld_[i].vk, true);
    keysHeld_.clear();
}

void InputInjector::tick() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!keysHeld_.empty() && nowUs() - lastKeyMsgUs_ > 1500000) releaseKeysLocked("the page went quiet for 1.5 s");
}

void InputInjector::flushLog(bool force) {
    std::lock_guard<std::mutex> lk(mu_);
    const int64_t now = nowUs();
    if (!force && now - lastFlushUs_ < 1000000) return;
    lastFlushUs_ = now;
    const char* mode = stub_ ? " (stub)" : inject_ ? "" : " (dry-run)";
    if (moveN_) logf("input%s: move x%d, total dx=%.0f dy=%.0f", mode, moveN_, moveDx_, moveDy_);
    if (relN_) logf("input%s: relative (遊戲模式) x%d, total dx=%.0f dy=%.0f", mode, relN_, relDx_, relDy_);
    relN_ = 0, relDx_ = relDy_ = 0;
    if (keyDowns_) logf("input%s: keys x%d", mode, keyDowns_);  // never which (passwords)
    keyDowns_ = 0;
    if (absN_) logf("input%s: pointer placed x%d (last at %ld,%ld)", mode, absN_, lastAbs_.x, lastAbs_.y);
    if (wheelN_) logf("input%s: wheel x%d, total dx=%.0f dy=%.0f", mode, wheelN_, wheelDx_, wheelDy_);
    moveN_ = wheelN_ = absN_ = 0;
    moveDx_ = moveDy_ = wheelDx_ = wheelDy_ = 0;
}

}  // namespace pm::remote
