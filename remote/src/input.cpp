#include "input.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "pm/remote.h"
#include "util.h"

namespace pm::remote {

namespace {

const char* buttonName(int b) { return b == 2 ? "right" : b == 1 ? "middle" : "left"; }

void sendMouse(DWORD flags, LONG dx = 0, LONG dy = 0, DWORD data = 0) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx, in.mi.dy = dy, in.mi.mouseData = data, in.mi.dwFlags = flags;
    in.mi.dwExtraInfo = kInjectedInputTag;
    if (SendInput(1, &in, sizeof(in)) != 1)
        logf("input: SendInput failed (blocked by UIPI? the window under the cursor may run as administrator)");
}

void sendVk(WORD vk, bool extended) {
    INPUT in[2] = {};
    for (int i = 0; i < 2; ++i) {
        in[i].type = INPUT_KEYBOARD;
        in[i].ki.wVk = vk;
        in[i].ki.wScan = WORD(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        in[i].ki.dwFlags = (extended ? KEYEVENTF_EXTENDEDKEY : 0) | (i ? KEYEVENTF_KEYUP : 0);
        in[i].ki.dwExtraInfo = kInjectedInputTag;
    }
    if (SendInput(2, in, sizeof(INPUT)) != 2) logf("input: SendInput(key) failed (foreground window elevated?)");
}

void sendText(const std::wstring& w) {
    std::vector<INPUT> v;
    auto flush = [&] {
        if (!v.empty() && SendInput(UINT(v.size()), v.data(), sizeof(INPUT)) != v.size())
            logf("input: SendInput(text) failed (foreground window elevated?)");
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

}  // namespace

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
    return true;
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
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const LONG ax = LONG(std::lround((x - vx) * 65535.0 / std::max(1, vw - 1)));
    const LONG ay = LONG(std::lround((y - vy) * 65535.0 / std::max(1, vh - 1)));
    sendMouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, ax, ay);
}

void InputInjector::button(int b, bool down) {
    if (b < 0 || b > 2) return;
    if (down && !allowed(false)) return;  // a release always goes out (never leaves a button held)
    if (!down && !held_[b]) return;
    held_[b] = down;
    if (!inject_) return;
    static const DWORD dn[3] = {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_RIGHTDOWN};
    static const DWORD upf[3] = {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_RIGHTUP};
    sendMouse(down ? dn[b] : upf[b]);
}

bool InputInjector::handle(const std::string& t, double dx, double dy, int b, const std::string& text) {
    if (t == "release") {
        releaseAll();
        return true;
    }
    std::lock_guard<std::mutex> lk(mu_);
    ++events_;
    const char* mode = inject_ ? "" : " (dry-run)";
    const bool haveScreen = screen_.right > screen_.left && screen_.bottom > screen_.top;
    if (t == "move") {
        if (!std::isfinite(dx) || !std::isfinite(dy) || std::fabs(dx) > 1e5 || std::fabs(dy) > 1e5) return false;
        ++moveN_, moveDx_ += dx, moveDy_ += dy;
        if (inject_) {
            fracX_ += dx, fracY_ += dy;
            const LONG ix = LONG(std::trunc(fracX_)), iy = LONG(std::trunc(fracY_));
            fracX_ -= ix, fracY_ -= iy;
            if (ix || iy) {
                POINT p;
                GetCursorPos(&p);
                RECT r = screen_;
                if (!haveScreen) {
                    r.left = GetSystemMetrics(SM_XVIRTUALSCREEN), r.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
                    r.right = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN), r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
                }
                // Stays on the streamed monitor: the phone always sees the cursor.
                moveTo(std::clamp(double(p.x + ix), double(r.left), double(r.right - 1)),
                       std::clamp(double(p.y + iy), double(r.top), double(r.bottom - 1)));
            }
        }
    } else if (t == "abs") {
        if (!std::isfinite(dx) || !std::isfinite(dy) || !haveScreen) return false;
        ++absN_;
        const double x = screen_.left + std::clamp(dx, 0.0, 1.0) * (screen_.right - screen_.left - 1);
        const double y = screen_.top + std::clamp(dy, 0.0, 1.0) * (screen_.bottom - screen_.top - 1);
        fracX_ = fracY_ = 0;
        if (inject_) moveTo(x, y);
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
        if (b < 0 || b > 2) return false;
        lastPressUs_ = nowUs();
        logf("input%s: %s %s", mode, buttonName(b), t == "down" ? "down" : "up");
        button(b, t == "down");
    } else if (t == "click") {
        if (b < 0 || b > 2) return false;
        lastPressUs_ = nowUs();
        POINT p{};
        GetCursorPos(&p);
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
    logf("input%s: combo %s", inject_ ? "" : " (dry-run)", desc.c_str());
    if (allowed(true) && SendInput(UINT(in.size()), in.data(), sizeof(INPUT)) != in.size())
        logf("input: SendInput(combo) failed (foreground window elevated? Ctrl+Alt+Del never works)");
    return true;
}

void InputInjector::releaseAll() {
    std::lock_guard<std::mutex> lk(mu_);
    for (int b = 0; b < 3; ++b)
        if (held_[b]) {
            logf("input%s: releasing held %s button", inject_ ? "" : " (dry-run)", buttonName(b));
            button(b, false);
        }
}

void InputInjector::flushLog(bool force) {
    std::lock_guard<std::mutex> lk(mu_);
    const int64_t now = nowUs();
    if (!force && now - lastFlushUs_ < 1000000) return;
    lastFlushUs_ = now;
    const char* mode = inject_ ? "" : " (dry-run)";
    if (moveN_) logf("input%s: move x%d, total dx=%.0f dy=%.0f", mode, moveN_, moveDx_, moveDy_);
    if (absN_) logf("input%s: pointer placed x%d", mode, absN_);
    if (wheelN_) logf("input%s: wheel x%d, total dx=%.0f dy=%.0f", mode, wheelN_, wheelDx_, wheelDy_);
    moveN_ = wheelN_ = absN_ = 0;
    moveDx_ = moveDy_ = wheelDx_ = wheelDy_ = 0;
}

}  // namespace pm::remote
