// Renderer internals shared by its .cpp files: UI timings, easing curves, colours and
// palettes, the shader constant buffer, the picture transform and small text helpers.
// 拆檔 0.7.9：自 renderer.cpp 的匿名 namespace 原樣搬出，改放 pm::video::detail
// （函式加 inline、陣列改 inline，避免 /W4 C4505 與多份副本）。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "pm/i18n.h"
#include "renderer.h"

namespace pm::video::detail {

constexpr UINT kSwapFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

// ---- UI timing (ms) ----
// Waiting <-> picture cross-fade with a settle: in, the scene fades over
// 300 ms (standard) while lifting 8 px and the picture grows 98.5 -> 100 %
// over 380 ms (outCubic); out, the picture shrinks to 98.8 % (280 ms), the
// scene fades in over 420 ms (outCubic) and 投投 drops in 10 px (520 ms).
constexpr double kFadeInMs = 380, kSceneOutMs = 300, kFadeOutMs = 420, kDropMs = 520;
constexpr double kPinFadeMs = 160;
constexpr double kToastInMs = 220, kToastHoldMs = 2500, kToastOutMs = 260;  // in: outCubic + 8 px rise
constexpr double kAmbientSettleMs = 1500;  // breathing amplitude eases out
constexpr double kConnectTimeoutMs = 60000;
constexpr double kFastFrameMs = 1000.0 / 60, kSlowFrameMs = 1000.0 / 30;

constexpr double kDimMs = 200;
constexpr float kDimMax = 0.55f;
constexpr double kHoverMs = 160;
constexpr double kReactMs = 1250;      // click reaction (hop, hearts, bubble)
constexpr double kHopMs = 1000;        // squash-and-stretch hop
constexpr double kConnectIntroMs = 1150;  // surprise + happy hop before the spinner scene
constexpr double kSurpriseMs = 260;       // 「found a phone」: surprised face before the hop
constexpr double kTapFlushMs = 20;     // newest tapped picture delivered at the latest after this
// Live toolbar: shown on mouse movement, hidden kToolHoldMs after the last one.
constexpr double kToolHoldMs = 2000, kToolInMs = 160, kToolOutMs = 260;  // in: outCubic
constexpr double kTipDelayMs = 450, kTipFadeMs = 120;
// Magnifier: the big zoom indicator stays this long after a change, then fades.
constexpr double kZoomShowMs = 1200, kZoomFadeMs = 400;
constexpr double kFlashInMs = 40, kFlashOutMs = 200;  // screenshot veil

constexpr D2D1_COLOR_F rgb(uint32_t c) {
    return {((c >> 16) & 0xff) / 255.f, ((c >> 8) & 0xff) / 255.f, (c & 0xff) / 255.f, 1};
}
// Themes: soft pink, mint, lavender night and milk tea.  The mascot's cloud
// stays white; only its phone screen, rim light, beam and hearts take the
// accent.
//                                bgTop     bgBottom  fg        dim       accent    accent2   ink       card      well
inline const Renderer::Palette kPalettes[4] = {
    {rgb(0x2A1C1F), rgb(0x181012), rgb(0xFFF4F1), rgb(0xD1B0B0), rgb(0xF5A7A7), rgb(0xE65C54), rgb(0x5A2A2A), rgb(0x332226), rgb(0x452E32)},
    {rgb(0x123230), rgb(0x0A1D1C), rgb(0xF0FCF8), rgb(0xA6CFC4), rgb(0x8FE3C4), rgb(0xF2C6A0), rgb(0x0F3D33), rgb(0x173D39), rgb(0x1F4E49)},
    {rgb(0x1B1D3D), rgb(0x0D0E23), rgb(0xF4F2FF), rgb(0xB3AFD8), rgb(0xBBA9F7), rgb(0x8FA2FF), rgb(0x2B2366), rgb(0x24264D), rgb(0x2F3260)},
    {rgb(0x30231B), rgb(0x1A120D), rgb(0xFFF6EC), rgb(0xD8C3AC), rgb(0xE3B98A), rgb(0xF4DCC0), rgb(0x4D301B), rgb(0x3B2B21), rgb(0x4C392D)},
};
constexpr D2D1_COLOR_F kRecRed = rgb(0xFF4F55);
constexpr D2D1_COLOR_F kDangerRed = rgb(0xFF6B6B);  // 中斷連線 (readable on every theme's card)

constexpr float kPi = 3.14159265f;
using pm::i18n::S;
using pm::i18n::tr;
// Mascot speech bubbles (click reaction), awake / asleep: pm/i18n_strings.inc.
inline constexpr S kBubbleLines[] = {S::Bubble1, S::Bubble2, S::Bubble3, S::Bubble4, S::Bubble5};
inline constexpr S kSleepyLines[] = {S::Sleepy1, S::Sleepy2, S::Sleepy3};

inline float ease(double t) {  // smoothstep on [0,1]
    float x = static_cast<float>(std::clamp(t, 0.0, 1.0));
    return x * x * (3 - 2 * x);
}
inline float outCubic(double t) {  // 1 - (1 - t)^3: enters fast, settles softly
    const float x = 1 - static_cast<float>(std::clamp(t, 0.0, 1.0));
    return 1 - x * x * x;
}
inline float inCubic(double t) {
    const float x = static_cast<float>(std::clamp(t, 0.0, 1.0));
    return x * x * x;
}
// CSS cubic-bezier(x1, y1, x2, y2) at time t (Newton on x).
inline float bezier(double t, float x1, float y1, float x2, float y2) {
    const float x = static_cast<float>(std::clamp(t, 0.0, 1.0));
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
inline float standard(double t) { return bezier(t, 0.4f, 0, 0.2f, 1); }   // material standard
inline float softOut(double t) { return bezier(t, 0.22f, 1, 0.36f, 1); }  // gentle ease-out, no overshoot
inline D2D1_COLOR_F mixc(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}
inline D2D1_COLOR_F alphaOf(D2D1_COLOR_F c, float a) {
    c.a *= a;
    return c;
}
inline D2D1_RECT_F inflate(D2D1_RECT_F r, float d) { return {r.left - d, r.top - d, r.right + d, r.bottom + d}; }
// Scale about a point, composed with the target's current transform.
inline D2D1::Matrix3x2F scaledAbout(const D2D1_MATRIX_3X2_F& base, float k, D2D1_POINT_2F c) {
    return D2D1::Matrix3x2F::Scale(k, k, c) * *D2D1::Matrix3x2F::ReinterpretBaseType(&base);
}


struct Constants {
    float uvRect[4];
    float range[4];
    float mat[4];
    float xfU[4];
    float xfV[4];
    float view[4];
};

// Screen coords t (0..1, y down) -> picture coords for a clockwise rotation
// of the picture by rot quarter turns, then a horizontal mirror.
inline void pictureTransform(int rot, bool mirror, float u[4], float v[4]) {
    // Mirror on screen first: tx' = mx * tx + mc.
    const float mx = mirror ? -1.f : 1.f, mc = mirror ? 1.f : 0.f;
    // s = R(t'), coefficients of (tx', ty, 1).
    float a[3], b[3];
    switch (rot & 3) {
    default: a[0] = 1; a[1] = 0; a[2] = 0; b[0] = 0; b[1] = 1; b[2] = 0; break;    // (tx, ty)
    case 1: a[0] = 0; a[1] = 1; a[2] = 0; b[0] = -1; b[1] = 0; b[2] = 1; break;    // (ty, 1-tx)
    case 2: a[0] = -1; a[1] = 0; a[2] = 1; b[0] = 0; b[1] = -1; b[2] = 1; break;   // (1-tx, 1-ty)
    case 3: a[0] = 0; a[1] = -1; a[2] = 1; b[0] = 1; b[1] = 0; b[2] = 0; break;    // (1-ty, tx)
    }
    u[0] = a[0] * mx; u[1] = a[1]; u[2] = a[0] * mc + a[2]; u[3] = 0;
    v[0] = b[0] * mx; v[1] = b[1]; v[2] = b[0] * mc + b[2]; v[3] = 0;
}


inline D2D1_POINT_2F polar(D2D1_POINT_2F c, float r, float deg) {
    float a = deg * kPi / 180.f;
    return {c.x + r * std::cos(a), c.y + r * std::sin(a)};
}


constexpr D2D1_COLOR_F kToutouInk = rgb(0x3A2830);

// Korean wraps at spaces, not inside a word (「미러 / 링」, 「선택하 / 세요」):
// a word joiner (U+2060, invisible) between Hangul syllables; DirectWrite
// still breaks a word wider than the line.  0.7.9: hyphenated words
// (Wi-Fi, Right-click) stay whole in every script, and so do katakana words
// (the 日本語 idle card broke 「画面ミラーリ / ング」).
inline std::wstring keepAll(const std::wstring& s) {
    auto hangul = [](wchar_t ch) { return ch >= 0xAC00 && ch <= 0xD7A3; };
    auto kata = [](wchar_t ch) { return ch >= 0x30A1 && ch <= 0x30FC && ch != 0x30FB; };  // not the middle dot
    auto letter = [](wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'); };
    std::wstring o;
    o.reserve(s.size() * 2);
    for (size_t i = 0; i < s.size(); ++i) {
        o += s[i];
        if (i + 1 < s.size() && ((hangul(s[i]) && hangul(s[i + 1])) || (kata(s[i]) && kata(s[i + 1])) ||
                                 (s[i] == L'-' && i > 0 && letter(s[i - 1]) && letter(s[i + 1]))))
            o += L'\x2060';
    }
    return o;
}

}  // namespace pm::video::detail
