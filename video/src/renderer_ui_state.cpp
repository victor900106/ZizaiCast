// Renderer: UI state setters (connecting, options, PIN, toast, fades, hover / focus, theme, dim, transform, REC).
// 拆檔 0.7.9：自 renderer.cpp 原樣搬出。
#include "renderer.h"

#include <d3dcompiler.h>
#include <mfapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "log.h"
#include "pm/i18n.h"
#include "renderer_internal.h"

namespace pm::video {

using namespace detail;

void Renderer::setConnecting(const std::wstring& name) {
    deviceName_ = name;
    if (scene_ == Scene::Live) return;  // a picture is already up: nothing to announce
    const double now = clockMs();
    if (scene_ == Scene::Idle) {
        // She is happy about it: a hop + hearts, then the spinner scene.
        connectIntro_ = !paused_;
        if (connectIntro_) {
            // Surprised for a moment (eyes to the phone, sparkles at its top),
            // then the happy hop + hearts.
            surpriseAt_ = now;
            reactAt_ = now + kSurpriseMs;
            reactBubble_ = false;
        }
    }
    scene_ = Scene::Connecting;
    sceneAt_ = now;
}

void Renderer::setOptions(std::vector<Option> options) {
    // Same check boxes, one changed: its tick draws itself (or is undrawn).
    const bool same = options.size() == options_.size() &&
                      std::equal(options.begin(), options.end(), options_.begin(),
                                 [](const Option& a, const Option& b) { return a.label == b.label; });
    optAt_.resize(options.size(), -1e9);
    if (same) {
        const double now = clockMs();
        for (size_t i = 0; i < options.size(); ++i)
            if (options[i].checked != options_[i].checked) optAt_[i] = now;
    } else {
        std::fill(optAt_.begin(), optAt_.end(), -1e9);
    }
    options_ = std::move(options);
}

void Renderer::setPin(const std::wstring& pin) {
    const double now = clockMs();
    if (!pin.empty()) {
        pin_ = pin;
        if (!pinVisible_) {
            pinVisible_ = true;
            pinAt_ = now;
        }
    } else if (pinVisible_) {
        pinVisible_ = false;
        pinAt_ = now;
    }
}

void Renderer::showToast(const std::wstring& text, double holdMs) {
    toast_ = text;
    toastAt_ = clockMs();
    // Long enough to read (0.7.2; was 2.5 s or the caller's fixed time, so a
    // two-line message with the next step was gone half read): about 130 ms
    // per CJK / kana / Hangul character and 55 ms per other one, 2.5-8 s; a
    // caller's longer time (a sticky 「正在傳送…」) still wins.
    double read = 1200;
    for (wchar_t c : text) read += c >= 0x2E80 ? 130 : 55;
    read = std::clamp(read, kToastHoldMs, 8000.0);
    toastHold_ = holdMs > 0 ? std::max(holdMs, read) : read;
}

// ---------------------------------------------------------------------------
// Hover / press / focus levels.

float Renderer::fadeLevel(const Fade& f, double now) const {
    const double e = now - f.at;
    if (f.on) return f.from + (1 - f.from) * outCubic(e / f.inMs);
    return f.from * (1 - (f.softOff ? softOut(e / f.outMs) : ease(e / f.outMs)));
}

void Renderer::fadeTo(Fade& f, bool on, double now, double inMs, double outMs, bool softOff) {
    if (on == f.on) return;
    f.from = fadeLevel(f, now);
    f.on = on;
    f.at = now;
    f.inMs = std::max(1.0, ms(inMs));
    f.outMs = std::max(1.0, ms(outMs));
    f.softOff = softOff;
}

void Renderer::hoverTo(int kind, int index) {
    if (kind < 0 || kind >= kUiKinds) return;
    const double now = clockMs();
    for (int i = 0; i < kUiMax; ++i) fadeTo(uiHot_[kind][i], i == index, now, 120, 180);
}

void Renderer::setPressed(int kind, int index) {
    const double now = clockMs();
    for (int k = 0; k < kUiKinds; ++k)
        for (int i = 0; i < kUiMax; ++i) fadeTo(uiPress_[k][i], k == kind && i == index, now, 70, 120, true);
}

void Renderer::setFocus(int kind, int index) {
    const double now = clockMs();
    for (int k = 0; k < kUiKinds; ++k)
        for (int i = 0; i < kUiMax; ++i) fadeTo(uiFocus_[k][i], k == kind && i == index, now, 120, 120);
}

bool Renderer::uiAnimating(double now) const {
    for (const auto* set : {&uiHot_, &uiPress_, &uiFocus_})
        for (const auto& kind : *set)
            for (const Fade& f : kind)
                if (now - f.at < (f.on ? f.inMs : f.outMs) + 40) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Presentation: theme, dim, rotation / mirror, device frame, REC badge

const Renderer::Palette& Renderer::palette(int theme) { return kPalettes[std::clamp(theme, 0, 3)]; }

void Renderer::setTheme(int theme) {
    theme = std::clamp(theme, 0, 3);
    if (theme == theme_) return;
    theme_ = theme;
    pal_ = kPalettes[theme];
    bgBrush_.Reset();  // gradients carry the colours: rebuilt by ensureBrushes()
    glowBrush_.Reset();
    beamBrush_.Reset();
}

float Renderer::dimLevel(double now) const {
    const float t = ease((now - dimAt_) / ms(kDimMs));
    const float target = dimmed_ ? kDimMax : 0.f;
    return dimFrom_ + (target - dimFrom_) * t;
}

void Renderer::setDimmed(bool d) {
    if (d == dimmed_) return;
    const double now = clockMs();
    dimFrom_ = dimLevel(now);
    dimmed_ = d;
    dimAt_ = now;
}

void Renderer::setTransform(int rot, bool mirror) {
    rot_ = rot & 3;
    mirror_ = mirror;
}

void Renderer::setRecording(bool on) {
    if (on == recording_) return;
    recording_ = on;
    if (on) recAt_ = clockMs();
}

}  // namespace pm::video
