// Renderer: per-frame main path (frame timing, PIN, toast, render).
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

double Renderer::nextFrameInMs() const {
    if (width_ == 0 || height_ == 0 || !visible_) return -1;  // minimized / hidden: no animation
    const double now = clockMs();
    constexpr double kTail = 40;  // draw one frame past each animation's end
    // Live toolbar: fading in / out, tooltip appearing, a recording dot
    // pulsing, or (event-driven) the moment it starts to hide.
    bool toolFast = false;
    double toolDue = -1;
    if (toolbarAvailable()) {
        const float ta = toolbarAlpha(now);
        const bool fadingTool = now - toolInAt_ < ms(kToolInMs) + kTail ||
                                (!toolInside_ && now >= toolUntil_ && now - toolUntil_ < ms(kToolOutMs) + kTail);
        const bool tip = ta > 0 && toolHot_ >= 0 && now - toolHotAt_ < kTipDelayMs + kTipFadeMs + kTail;
        toolFast = fadingTool || (tip && now - toolHotAt_ >= kTipDelayMs);
        if (tip && now - toolHotAt_ < kTipDelayMs) toolDue = kTipDelayMs - (now - toolHotAt_);
        if (ta > 0 && !toolInside_ && now < toolUntil_) {
            const double d = toolUntil_ - now;
            toolDue = toolDue < 0 ? d : std::min(toolDue, d);
        }
        if (ta > 0 && !revealNote_.empty()) {  // revealToolbar's callout: fades out at revealUntil_
            if (now >= revealUntil_ && now - revealUntil_ < ms(kToolOutMs) + kTail) toolFast = true;
            else if (now < revealUntil_) toolDue = toolDue < 0 ? revealUntil_ - now : std::min(toolDue, revealUntil_ - now);
        }
    }
    // Toast: frames only while it slides in and fades out; the hold between
    // is a still picture (one wake-up when the fade starts).
    const double toastE = now - toastAt_;
    const bool toastAnim = !toast_.empty() && (toastE < ms(kToastInMs) + kTail ||
                                               (toastE >= toastHold_ && toastE < toastHold_ + ms(kToastOutMs) + kTail));
    if (!toast_.empty() && !toastAnim && toastE < toastHold_) {
        const double d = toastHold_ - toastE;
        toolDue = toolDue < 0 ? d : std::min(toolDue, d);
    }
    bool fast = toolFast || now - fadeInAt_ < ms(kFadeInMs) + kTail || now - fadeOutAt_ < ms(kDropMs) + kTail ||
                now - pinAt_ < ms(kPinFadeMs) + kTail ||
                toastAnim ||
                (scene_ == Scene::Connecting && !paused_) || now - dimAt_ < ms(kDimMs) + kTail ||
                (!reduced_ && now - mascotHotAt_ < kHoverMs + kTail) ||
                (now - reactAt_ < kReactMs + 250 + kTail && (!reduced_ || reactBubble_)) ||
                uiAnimating(now) || !busy_.empty() || now - flashAt_ < kFlashInMs + kFlashOutMs + kTail || (now - zoomAt_ >= kZoomShowMs && now - zoomAt_ < kZoomShowMs + ms(kZoomFadeMs) + kTail);
    for (double at : optAt_) fast = fast || now - at < 70 + 220 + kTail;  // tick drawing
    if (!fast && now - zoomAt_ < kZoomShowMs) {
        // Event-driven until the big zoom indicator starts to fade.
        const double d = kZoomShowMs - (now - zoomAt_);
        toolDue = toolDue < 0 ? d : std::min(toolDue, d);
    }
    // Recording: the REC timer ticks once a second (its dot pulses only on
    // frames drawn anyway: a static phone screen costs 1 frame/s, not 30-60).
    if (recording_) {
        const double into = std::fmod(now - recAt_, 1000.0);  // ms into the timer's current second
        // (Waits up to 120 ms for a picture that redraws it anyway.)
        const double d = lastRender_ < now - into ? std::max(0.0, 120 - into) : 1000 - into;
        toolDue = toolDue < 0 ? d : std::min(toolDue, d);
    }
    // Idle / paused screen: the mascot floats (and blinks) for a while after
    // any activity, then rests on a static frame (at once with reduced motion).
    bool slow = !reduced_ && (scene_ == Scene::Idle || paused_) && now < ambientUntil_ + kTail;
    if (!fast && !slow) return toolDue;
    // Fully covered window (Present reported occlusion): probe slowly.
    const double period = occluded_ ? 500 : fast ? kFastFrameMs : kSlowFrameMs;
    const double next = std::max(0.0, period - (now - lastRender_));
    return toolDue >= 0 ? std::min(next, toolDue) : next;
}

void Renderer::drawPin(double now) {
    if (pin_.empty()) return;
    const float t = ease((now - pinAt_) / ms(kPinFadeMs));
    const float a = pinVisible_ ? t : 1 - t;
    if (a <= 0.003f) return;
    const float s = dpi_ / 96.f;
    const float W = width_ / s, H = height_ / s;
    const D2D1_COLOR_F veil{pal_.bgBottom.r * 0.4f, pal_.bgBottom.g * 0.4f, pal_.bgBottom.b * 0.4f, 1};
    d2dTarget_->FillRectangle({0, 0, W, H}, brush(veil, 0.62f * a));

    const size_t n = std::min<size_t>(pin_.size(), 8);
    const float nf = static_cast<float>(n);
    float d = std::clamp(std::min(W * 0.15f, H * 0.11f), 36.f, 120.f);
    float gap = d * 0.22f, pad = d * 0.5f;
    const float maxCard = W - 24;
    if (nf * d + (nf - 1) * gap + 2 * pad > maxCard) {
        d = std::max((maxCard - 2 * pad) / (nf + (nf - 1) * 0.22f), 20.f);
        gap = d * 0.22f;
    }
    const float labelSize = std::clamp(d * 0.27f, 13.f, 28.f);
    auto label = layout(tr(S::VidPinLabel), labelSize, maxCard - 2 * pad);
    const float digitsW = nf * d + (nf - 1) * gap;
    const float cardW = std::min(std::max(digitsW, textW(label.Get())) + 2 * pad, maxCard);
    const float labelH = textH(label.Get());
    const float heart = labelSize * 0.9f;
    const float cardH = pad * 0.8f + heart + labelSize * 0.5f + labelH + d * 0.4f + d + pad;
    const float x0 = std::round((W - cardW) / 2), y0 = std::round((H - cardH) / 2);
    const float lift = pinVisible_ && !reduced_ ? (1 - t) * 8 : 0;  // slight rise while fading in
    const D2D1_RECT_F card{x0, y0 + lift, x0 + cardW, y0 + cardH + lift};
    d2dTarget_->FillRoundedRectangle({card, 18, 18}, brush(pal_.card, a));
    d2dTarget_->DrawRoundedRectangle({card, 18, 18}, brush(pal_.accent, 0.28f * a), 1.25f);
    float y = card.top + pad * 0.8f;
    if (heart_) {
        d2dTarget_->SetTransform(D2D1::Matrix3x2F::Scale(heart, heart) *
                                 D2D1::Matrix3x2F::Translation(W / 2, y + heart / 2));
        d2dTarget_->FillGeometry(heart_.Get(), brush(pal_.accent, a));
        d2dTarget_->SetTransform(D2D1::IdentityMatrix());
    }
    y += heart + labelSize * 0.5f;
    if (label) {
        label->SetMaxWidth(cardW - 2 * pad);
        d2dTarget_->DrawTextLayout({card.left + pad, y}, label.Get(), brush(pal_.dim, a));
    }
    y += labelH + d * 0.4f;
    float x = std::round((W - digitsW) / 2);
    for (size_t i = 0; i < n; ++i, x += d + gap) {
        D2D1_ROUNDED_RECT cell{{x, y, x + d, y + d}, d * 0.22f, d * 0.22f};
        d2dTarget_->FillRoundedRectangle(cell, brush(pal_.well, a));
        d2dTarget_->DrawRoundedRectangle(cell, brush(pal_.accent, 0.55f * a), 1.5f);
        auto digit = layout(std::wstring(1, pin_[i]), d * 0.6f, d, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, true);
        if (!digit) continue;
        digit->SetMaxHeight(d);
        digit->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        d2dTarget_->DrawTextLayout({x, y}, digit.Get(), brush(pal_.fg, a));
    }
}

void Renderer::drawToast(double now) {
    if (toast_.empty()) return;
    const double e = now - toastAt_;
    // In: 220 ms outCubic, rising 8 px and growing from 98 %; out: 260 ms
    // smoothstep fade in place (reduced motion: 120 ms fades only).
    float a, rise = 0, scale = 1;
    if (e < ms(kToastInMs)) {
        a = reduced_ ? ease(e / ms(kToastInMs)) : outCubic(e / kToastInMs);
        if (!reduced_) rise = (1 - a) * 8, scale = 0.98f + 0.02f * a;
    } else if (e < toastHold_) {
        a = 1;
    } else {
        a = 1 - ease((e - toastHold_) / ms(kToastOutMs));
    }
    if (a <= 0.003f) return;
    const float s = dpi_ / 96.f;
    const float W = width_ / s, H = height_ / s;
    const float size = std::clamp(std::min(H / 24, W / 15) * 0.56f, 12.f, 24.f);
    const float padX = size * 1.2f, padY = size * 0.65f;
    const float maxW = W - 32 - 2 * padX;
    auto l = layout(pm::i18n::keepWords(toast_), size, maxW, DWRITE_FONT_WEIGHT_NORMAL, false);
    if (!l) return;
    // (Measured untrimmed: the trimmed layout never reports more than maxW.)
    if (auto full = layout(toast_, size, 100000.f, DWRITE_FONT_WEIGHT_NORMAL, false); full && textW(full.Get()) > maxW) {
        // Too long for one line: wrap (after the first 「。」 / ". " if there is one).
        std::wstring t = toast_;
        bool split = true;
        if (const size_t dot = t.find(L'。'); dot != std::wstring::npos && dot + 1 < t.size()) t.insert(dot + 1, L"\n");
        else if (const size_t en = t.find(L". "); en != std::wstring::npos && en > 8) t[en + 1] = L'\n';
        else split = false;
        // 0.7.9: a sentence break only when each sentence then fits on its
        // own line; else two sentences that need three lines would (e.g. the
        // first right-click hint in a 540 px window).
        if (split) {
            const size_t nlAt = t.find(L'\n');
            for (const std::wstring& part : {t.substr(0, nlAt), t.substr(nlAt + 1)})
                if (auto p = layout(part, size, 100000.f, DWRITE_FONT_WEIGHT_NORMAL, false); p && textW(p.Get()) > maxW) {
                    t = toast_;
                    split = false;
                    break;
                }
        }
        // No sentence break: lines of even length instead of a full first
        // line and a stub such as 「Wi-Fi）」 (0.7.2).
        float wrapW = maxW;
        UINT32 evenLines = 0;
        if (!split) {
            const float fw = textW(full.Get());
            evenLines = static_cast<UINT32>(std::ceil(fw / maxW));
            wrapW = std::min(maxW, fw / evenLines + size * 1.5f);
        }
        if (auto w = layout(pm::i18n::keepWords(t), size, wrapW, DWRITE_FONT_WEIGHT_NORMAL, true)) l = w;
        // 0.7.9: words kept whole (Shift＋右鍵, Right-click) can push the even
        // lines into one more; then the full width instead.
        DWRITE_TEXT_METRICS tm{};
        if (evenLines && wrapW < maxW && SUCCEEDED(l->GetMetrics(&tm)) && tm.lineCount > evenLines)
            if (auto w = layout(pm::i18n::keepWords(t), size, maxW, DWRITE_FONT_WEIGHT_NORMAL, true)) l = w;
    }
    const float tw = std::min(textW(l.Get()), maxW), th = textH(l.Get());
    const float pillW = tw + 2 * padX, pillH = th + 2 * padY;
    const float x0 = std::round((W - pillW) / 2);
    float y0 = std::round(H - std::max(20.f, H * 0.05f) - pillH + rise);
    // Above the translation list when that panel fills the bottom (0.7.2:
    // the toast was drawn across its rows).
    if (!boxes_.empty() && !showOriginal_ && ovPanel_.bottom > ovPanel_.top && y0 + pillH > ovPanel_.top &&
        x0 < ovPanel_.right && x0 + pillW > ovPanel_.left && ovPanel_.top - pillH - 10 > H * 0.12f)
        y0 = std::round(ovPanel_.top - pillH - 10 + rise);
    const float rad = std::min(pillH / 2, size * 1.25f);  // one line: a pill; more: a rounded card
    const D2D1_ROUNDED_RECT pill{{x0, y0, x0 + pillW, y0 + pillH}, rad, rad};
    D2D1_MATRIX_3X2_F base;
    d2dTarget_->GetTransform(&base);
    if (scale < 1) d2dTarget_->SetTransform(scaledAbout(base, scale, {x0 + pillW / 2, y0 + pillH / 2}));
    d2dTarget_->FillRoundedRectangle(pill, brush(pal_.card, 0.96f * a));
    d2dTarget_->DrawRoundedRectangle(pill, brush(pal_.accent, 0.35f * a), 1.25f);
    l->SetMaxWidth(std::ceil(tw) + 2.f);  // slack: exactly-measured width triggers the ellipsis
    d2dTarget_->DrawTextLayout({x0 + padX - 1.f, y0 + padY}, l.Get(), brush(pal_.fg, a));
    d2dTarget_->SetTransform(base);
}

bool Renderer::render() {
    presented_ = false;
    presentHr_ = S_OK;
    if (!swap_) return false;  // device lost and not re-created yet
    if (width_ == 0 || height_ == 0) return true;
    if (!rtv_) {  // a failed ResizeBuffers / GetBuffer: nothing can be drawn (the watchdog re-creates the chain)
        presentHr_ = E_HANDLE;
        return true;
    }
    const double now = clockMs();
    lastRender_ = now;
    if (scene_ == Scene::Connecting && !pinVisible_ && now - sceneAt_ > kConnectTimeoutMs) {
        scene_ = Scene::Idle;  // nothing arrived: don't spin forever
        sceneAt_ = now;
        connectIntro_ = false;
        poke();
    }
    const bool fadingIn = scene_ == Scene::Live && now - fadeInAt_ < ms(kFadeInMs);
    const bool fadingOut = scene_ != Scene::Live && havePicture_ && now - fadeOutAt_ < ms(kFadeOutMs);
    // A frozen picture stays up while the phone screen is off.
    if (frozenOn_ && !frozen_.valid() && cur_.valid() && havePicture_ && copyPicture(cur_, frozen_)) keepFrozenCopy();
    const bool frozenUp = frozenOn_ && frozen_.valid();
    // (A frozen picture restored after a device loss shows before the
    // decoder has delivered again.)
    const bool video = (havePicture_ || frozenUp) && (!paused_ || frozenUp) && (scene_ == Scene::Live || fadingOut);
    // Device re-created while live: black until the decoder delivers again
    // (the re-fed key frame, a few ms later).
    const bool blank = scene_ == Scene::Live && !havePicture_ && !paused_;
    if (!video && dimmed_) {  // the dim belongs to the picture only
        dimmed_ = false;
        dimFrom_ = 0;
        dimAt_ = -1e9;
    }
    FrameGeom fg{};
    const bool framed = video && frame_;
    if (video || blank) {
        // Device frame: the letterbox takes the theme's background colour.
        const D2D1_COLOR_F& b = pal_.bgBottom;
        const float bg[4] = {framed ? b.r : 0.f, framed ? b.g : 0.f, framed ? b.b : 0.f, 1};
        ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
        ctx_->ClearRenderTargetView(rtv_.Get(), bg);
    }
    D3D11_VIEWPORT vp{};
    picRect_ = {};
    if (video) {
        pictureViewport(vp, framed ? &fg : nullptr);
        if (vp.Width > 0 && vp.Height > 0) {
            // Settle: the picture grows in from 98.5 % / shrinks to 98.8 % while
            // cross-fading (not inside a device frame: the bezel stays put).
            D3D11_VIEWPORT pv = vp;
            float k = 1;
            if (!reduced_ && !framed && fadingIn) k = 0.985f + 0.015f * outCubic((now - fadeInAt_) / kFadeInMs);
            if (!reduced_ && !framed && fadingOut) k = 1 - 0.012f * standard((now - fadeOutAt_) / 280);
            if (k < 1) {
                pv.TopLeftX += pv.Width * (1 - k) / 2;
                pv.TopLeftY += pv.Height * (1 - k) / 2;
                pv.Width *= k;
                pv.Height *= k;
            }
            drawPicture(rtv_.Get(), pv);
        }
        if (scene_ == Scene::Live && vp.Width > 0 && vp.Height > 0)
            picRect_ = {static_cast<LONG>(vp.TopLeftX), static_cast<LONG>(vp.TopLeftY),
                        static_cast<LONG>(vp.TopLeftX + vp.Width), static_cast<LONG>(vp.TopLeftY + vp.Height)};
        // Magnifier overview: the whole picture, small, bottom right.
        miniRect_ = {};
        if (view_.zoom > 1.001f && vp.Width > 0 && vp.Height > 0) {
            miniRect_ = minimapRect(vp, framed ? fg.radius : 0.f);
            const D3D11_VIEWPORT mv{miniRect_.left, miniRect_.top, miniRect_.right - miniRect_.left,
                                    miniRect_.bottom - miniRect_.top, 0, 1};
            View whole;
            whole.filter = view_.filter;
            drawPicture(rtv_.Get(), mv, shown(), whole, mirror_);
        }
    }
    if (video && live_) liveTrack();  // 即時翻譯: the scroll since the boxes' picture (live_overlay.cpp)
    optionRects_.clear();
    actionRects_.clear();
    const float dim = video ? dimLevel(now) : 0.f;
    const bool overlays = (!pin_.empty() && (pinVisible_ || now - pinAt_ < ms(kPinFadeMs))) ||
                          (!toast_.empty() && now - toastAt_ < toastHold_ + ms(kToastOutMs));
    startReveal(now);  // a pending revealToolbar(), once the toolbar is up on a live picture
    const float toolA = toolbarAlpha(now);
    const bool magUi = video && (view_.zoom > 1.001f || now - zoomAt_ < kZoomShowMs + ms(kZoomFadeMs) || frozenUp ||
                                 !boxes_.empty() || selecting_);
    const bool flashing = video && now - flashAt_ < kFlashInMs + kFlashOutMs;
    const bool need2D = (!video && !blank) || fadingIn || fadingOut || overlays || framed || dim > 0.003f || flashing ||
                        recording_ || toolA > 0.003f || magUi || !busy_.empty();
    toolRects_.clear();
    toolPill_ = {};
    recBadge_ = {};
    bool clickable = false;
    if (need2D && ensureD2D()) {
        const float s = dpi_ / 96.f;
        d2dTarget_->BeginDraw();
        d2dTarget_->SetTransform(D2D1::IdentityMatrix());
        if (blank) {
        } else if (!video) {
            drawBackground(1);
            drawScene(scene_, 1, now);
            clickable = mascotClickable(now);
        } else {
            if (framed) drawDeviceFrame(d2dTarget_.Get(), fg, 1 / s);
            if (dim > 0.003f) d2dTarget_->FillRectangle({0, 0, width_ / s, height_ / s}, brush(D2D1::ColorF(0, 0, 0), dim));
            if (fadingIn) {
                const double e = now - fadeInAt_;
                const float a = reduced_ ? 1 - ease(e / ms(kFadeInMs)) : 1 - standard(e / kSceneOutMs);
                drawBackground(a);
                sceneDy_ = reduced_ ? 0.f : -8 * outCubic(e / kSceneOutMs);
                drawScene(fadeFrom_, a, now);
                sceneDy_ = 0;
            } else if (fadingOut) {
                const double e = now - fadeOutAt_;
                const float a = reduced_ ? ease(e / ms(kFadeOutMs)) : outCubic(e / kFadeOutMs);
                drawBackground(a);
                drawScene(Scene::Idle, a, now);
            }
        }
        if (flashing && vp.Width > 0) {  // screenshot: a white veil over the picture only
            const double fe = now - flashAt_;
            const float fa = fe < kFlashInMs ? 0.28f * outCubic(fe / kFlashInMs)
                                             : 0.28f * (1 - standard((fe - kFlashInMs) / kFlashOutMs));
            d2dTarget_->FillRectangle({vp.TopLeftX / s, vp.TopLeftY / s, (vp.TopLeftX + vp.Width) / s,
                                       (vp.TopLeftY + vp.Height) / s},
                                      brush(D2D1::ColorF(1, 1, 1), fa));
        }
        if (video && vp.Width > 0 && !fadingIn && !fadingOut) {
            const D2D1_RECT_F pr{vp.TopLeftX / s, vp.TopLeftY / s, (vp.TopLeftX + vp.Width) / s,
                                 (vp.TopLeftY + vp.Height) / s};
            if (overlayLive()) drawLiveOverlay(pr, framed ? fg.radius / s : 0.f);  // 即時翻譯: in place, follows scrolling
            else drawTextOverlay(pr, framed ? fg.radius / s : 0.f);
            drawMagnifierUi(pr, framed ? fg.radius / s : 0.f, now);
            if (selecting_) drawSelection(pr, framed ? fg.radius / s : 0.f);
        }
        if (!busy_.empty()) {
            if (video && vp.Width > 0)
                drawBusy({vp.TopLeftX / s, vp.TopLeftY / s, (vp.TopLeftX + vp.Width) / s, (vp.TopLeftY + vp.Height) / s}, now);
            else
                drawBusy({0, 0, width_ / s, height_ / s}, now);
        }
        if (recording_) {
            if (video && vp.Width > 0) {
                const D2D1_RECT_F r{vp.TopLeftX / s, vp.TopLeftY / s, (vp.TopLeftX + vp.Width) / s,
                                    (vp.TopLeftY + vp.Height) / s};
                drawRecBadge(r, framed ? fg.radius / s : 0.f, now);
            } else {
                drawRecBadge({0, 0, width_ / s, height_ / s}, 0.f, now);
            }
        }
        if (toolA > 0.003f && scene_ == Scene::Live) {
            // Over the picture; without one (paused, re-created device) over the window.
            if (video && vp.Width > 0)
                drawToolbar({vp.TopLeftX / s, vp.TopLeftY / s, (vp.TopLeftX + vp.Width) / s, (vp.TopLeftY + vp.Height) / s},
                            framed ? &fg : nullptr, now);
            else
                drawToolbar({0, 0, width_ / s, height_ / s}, nullptr, now);
        }
        drawPin(now);
        drawToast(now);
        if (d2dTarget_->EndDraw() == D2DERR_RECREATE_TARGET) releaseD2D();
    }
    // Mascot hit area (client pixels, at rest) for hover / click.
    mascotRect_ = {};
    if (clickable) {
        const Layout L = layoutFor();
        mascotRect_ = {static_cast<LONG>(L.mascot.left * L.s), static_cast<LONG>(L.mascot.top * L.s),
                       static_cast<LONG>(L.mascot.right * L.s), static_cast<LONG>(L.mascot.bottom * L.s)};
    }
    if (capture_) copyBackBuffer();
    if (faultSwallow_) {  // fault injection: a swap chain that no longer presents
        presentHr_ = S_FALSE;
        return true;
    }
    HRESULT hr = swap_->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        log("device lost on Present (hr=0x%08lx, reason=0x%08lx)", hr, dev_->GetDeviceRemovedReason());
        return false;
    }
    if (SUCCEEDED(hr) && faultOccludeUntil_ > now) hr = DXGI_STATUS_OCCLUDED;  // fault injection
    presentHr_ = hr;
    presented_ = hr == S_OK || hr == DXGI_STATUS_OCCLUDED;
    if (FAILED(hr) && hr != presentErrLogged_) {
        presentErrLogged_ = hr;
        log("Present failed hr=0x%08lx", hr);
    } else if (SUCCEEDED(hr)) {
        presentErrLogged_ = S_OK;
    }
    occluded_ = hr == DXGI_STATUS_OCCLUDED;
    return true;
}

}  // namespace pm::video
