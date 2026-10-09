// Small animation helpers for the app's own Direct2D panels (AskPanel,
// PairPanel, About, Update, Share): Windows 「顯示動畫」, easing curves and
// a per-element hover / press level that a panel repainted with
// InvalidateRect can draw with. UI thread only, header-only.
#pragma once
#include <windows.h>

#include <algorithm>
#include <cmath>

namespace pm::ui {

// Windows 「顯示動畫」 (SPI_GETCLIENTAREAANIMATION); read each time (cheap),
// so a change in Settings applies to the next transition.
inline bool animationsOn() {
    BOOL on = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) on = TRUE;
    return on != FALSE;
}

inline double animNowMs() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
}

inline float animClamp(double t) { return static_cast<float>(std::clamp(t, 0.0, 1.0)); }
inline float animEase(double t) {  // smoothstep
    const float x = animClamp(t);
    return x * x * (3 - 2 * x);
}
inline float animOutCubic(double t) {
    const float x = 1 - animClamp(t);
    return 1 - x * x * x;
}
inline float animInCubic(double t) {
    const float x = animClamp(t);
    return x * x * x;
}
// CSS cubic-bezier(x1, y1, x2, y2) at time t.
inline float animBezier(double t, float x1, float y1, float x2, float y2) {
    const float x = animClamp(t);
    float u = x;
    for (int i = 0; i < 6; ++i) {
        const float v = 1 - u;
        const float fx = 3 * v * v * u * x1 + 3 * v * u * u * x2 + u * u * u - x;
        const float dx = 3 * v * v * x1 + 6 * v * u * (x2 - x1) + 3 * u * u * (1 - x2);
        if (std::fabs(dx) < 1e-5f) break;
        u = std::clamp(u - fx / dx, 0.f, 1.f);
    }
    const float v = 1 - u;
    return 3 * v * v * u * y1 + 3 * v * u * u * y2 + u * u * u;
}
inline float animSoftOut(double t) { return animBezier(t, 0.22f, 1, 0.36f, 1); }
// Colour (D2D1_COLOR_F) a -> b by t, alpha too.
template <class C>
C animMix(C a, C b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

// Hover / press levels (0..1) per element key (a panel's Hit value, 0 =
// none). Hover eases in over 120 ms and out over 180 ms, a press in over
// 70 ms and out over 120 ms; both reverse from where they are. While one
// runs, a 16 ms timer (kTimer) invalidates the window: forward WM_TIMER to
// onTimer(). With 「顯示動畫」 off the changes are 120 ms fades (press: none).
class HoverAnim {
public:
    static constexpr UINT_PTR kTimer = 0x7A41;
    static constexpr int kMax = 64;

    void setHot(HWND h, int key) {
        const double now = animNowMs();
        for (int i = 1; i < kMax; ++i) to(hot_[i], i == key, now, 120, 180, false);
        kick(h);
    }
    void setPressed(HWND h, int key) {
        const double now = animNowMs();
        for (int i = 1; i < kMax; ++i) to(press_[i], i == key, now, 70, 120, true);
        kick(h);
    }
    float hot(int key) const { return key > 0 && key < kMax ? level(hot_[key], animNowMs()) : 0.f; }
    float pressed(int key) const {
        return key > 0 && key < kMax && reducedAt_ == 0 ? level(press_[key], animNowMs()) : 0.f;
    }
    // Scale for a pressed element: 1 .. 1 - depth.
    float pressScale(int key, float depth = 0.04f) const { return 1 - depth * pressed(key); }
    // WM_TIMER: true if it was this timer (the window is invalidated while
    // anything runs; the timer stops itself).
    bool onTimer(HWND h, WPARAM id) {
        if (id != kTimer) return false;
        InvalidateRect(h, nullptr, FALSE);
        if (!running(animNowMs())) {
            KillTimer(h, kTimer);
            timer_ = false;
        }
        return true;
    }
    void reset() {
        for (auto& f : hot_) f = F{};
        for (auto& f : press_) f = F{};
        timer_ = false;
    }

private:
    struct F {
        bool on = false, soft = false;
        float from = 0;
        double at = -1e9, in = 120, out = 180;
    };
    static float level(const F& f, double now) {
        const double e = now - f.at;
        if (f.on) return f.from + (1 - f.from) * animOutCubic(e / f.in);
        return f.from * (1 - (f.soft ? animSoftOut(e / f.out) : animEase(e / f.out)));
    }
    void to(F& f, bool on, double now, double in, double out, bool soft) {
        if (on == f.on) return;
        const bool anim = animationsOn();
        reducedAt_ = anim ? 0 : 1;
        f.from = level(f, now);
        f.on = on;
        f.at = now;
        f.in = anim ? in : std::min(in, 120.0);
        f.out = anim ? out : std::min(out, 120.0);
        f.soft = soft;
    }
    bool running(double now) const {
        for (const auto* set : {&hot_, &press_})
            for (const F& f : *set)
                if (now - f.at < (f.on ? f.in : f.out) + 20) return true;
        return false;
    }
    void kick(HWND h) {
        InvalidateRect(h, nullptr, FALSE);
        if (!timer_ && h && running(animNowMs())) timer_ = SetTimer(h, kTimer, 16, nullptr) != 0;
    }
    F hot_[kMax], press_[kMax];
    int reducedAt_ = 0;
    bool timer_ = false;
};

// Panel open / close (item 8). Open: the window fades in over 160 ms
// (outCubic, window alpha) while its content grows from 97 % about the
// centre (scale(): the panel's paint applies it; PairPanel, which has real
// EDIT children, uses alpha only). 「顯示動畫」 off: a 120 ms fade, no scale.
// The window is layered only while it fades (Windows 11 keeps its rounded
// corners otherwise).
class PanelFade {
public:
    static constexpr UINT_PTR kTimer = 0x7A42;
    // After CreateWindowEx, before ShowWindow.
    void begin(HWND h) {
        full_ = animationsOn();
        t0_ = animNowMs();
        SetWindowLongPtrW(h, GWL_EXSTYLE, GetWindowLongPtrW(h, GWL_EXSTYLE) | WS_EX_LAYERED);
        SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
        running_ = SetTimer(h, kTimer, 16, nullptr) != 0;
        if (!running_) end(h);
    }
    // The panel's first frame is on screen: the fade's clock starts (a slow
    // first paint, e.g. creating the render target, must not eat it).
    void painted() {
        if (running_ && !painted_) t0_ = animNowMs();
        painted_ = true;
    }
    float progress() const {
        if (!running_) return 1.f;
        if (!painted_) return animNowMs() - t0_ > 1000 ? 1.f : 0.f;  // (never painted: give up the fade)
        return animOutCubic((animNowMs() - t0_) / (full_ ? 160.0 : 120.0));
    }
    float scale() const { return full_ ? 0.97f + 0.03f * progress() : 1.f; }
    bool onTimer(HWND h, WPARAM id) {
        if (id != kTimer) return false;
        const float p = progress();
        if (p >= 1) {
            end(h);
        } else {
            SetLayeredWindowAttributes(h, 0, static_cast<BYTE>(std::lround(255 * p)), LWA_ALPHA);
            if (full_) InvalidateRect(h, nullptr, FALSE);
        }
        return true;
    }

private:
    void end(HWND h) {
        KillTimer(h, kTimer);
        running_ = false;
        SetWindowLongPtrW(h, GWL_EXSTYLE, GetWindowLongPtrW(h, GWL_EXSTYLE) & ~WS_EX_LAYERED);
        RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
    }
    double t0_ = 0;
    bool full_ = true, running_ = false, painted_ = false;
};

// Panel close: prepareCloseFade() while the panel still paints (makes it
// layered with its current picture), then closeWithFade() instead of
// DestroyWindow: the window, no longer the panel's (input off), fades out
// over 110 ms (ease-in; 「顯示動畫」 off: at once) and is destroyed.
inline void prepareCloseFade(HWND h) {
    if (!h || !IsWindowVisible(h) || IsIconic(h) || !animationsOn()) return;
    SetWindowLongPtrW(h, GWL_EXSTYLE, GetWindowLongPtrW(h, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
    RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}
inline void CALLBACK panelCloseTick(HWND h, UINT, UINT_PTR id, DWORD) {
    const double t0 = static_cast<double>(reinterpret_cast<uintptr_t>(GetPropW(h, L"PmCloseFadeT0")));
    const double t = (animNowMs() - t0) / 110.0;
    if (t >= 1) {
        KillTimer(h, id);
        RemovePropW(h, L"PmCloseFadeT0");
        DestroyWindow(h);
        return;
    }
    SetLayeredWindowAttributes(h, 0, static_cast<BYTE>(std::lround(255 * (1 - animInCubic(t)))), LWA_ALPHA);
}
inline void closeWithFade(HWND h) {
    if (!h) return;
    if (!(GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_LAYERED) || !IsWindowVisible(h)) {
        DestroyWindow(h);
        return;
    }
    // From here on DefWindowProc handles it (the panel's proc ignores a window that is not its own).
    SetWindowLongPtrW(h, GWLP_USERDATA, 0);
    SetWindowLongPtrW(h, GWL_EXSTYLE,
                      GetWindowLongPtrW(h, GWL_EXSTYLE) | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    EnableWindow(h, FALSE);
    SetPropW(h, L"PmCloseFadeT0", reinterpret_cast<HANDLE>(static_cast<uintptr_t>(animNowMs())));
    if (!SetTimer(h, 0x7A43, 16, panelCloseTick)) DestroyWindow(h);
}

}  // namespace pm::ui
