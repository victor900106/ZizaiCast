// Renderer: 投投 Toutou mascot (pose, layers, phone, beam, sparkles, bubble, particles, spinner).
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

// ---- 投投 Toutou ----------------------------------------------------------
// Layers (ToutouArt) in the art's frame units, composed with one transform per
// frame: pivot (cloud bottom centre) -> anchor, scaled by u and the squash /
// stretch, lifted by the float.  The phone and the beam are drawn here (they
// take the theme accent and animate).

Renderer::Pose Renderer::mascotPose(const Layout& L, Scene scene, double now) {
    using namespace toutou;
    Pose P;
    const bool paused = paused_ && scene != Scene::Connecting;
    const bool busy = scene == Scene::Connecting && !paused;
    const float env = reduced_ ? 0.f : busy ? 1.f : ease((ambientUntil_ - now) / kAmbientSettleMs);
    const double react = now - reactAt_;
    const bool reacting = react >= 0 && react < kReactMs;
    const double surprise = now - surpriseAt_;
    if (busy && connectIntro_ && surprise >= 0 && surprise < kSurpriseMs) {
        P.face = ToutouArt::FaceSurprised;
    } else if (busy) {
        P.face = connectIntro_ && reacting ? ToutouArt::FaceHappy : ToutouArt::FaceConnecting;
    } else if (paused) {
        P.face = reacting ? ToutouArt::FaceSleepy : ToutouArt::FaceAsleep;
    } else if (reacting) {
        P.face = ToutouArt::FaceHappy;
    } else {
        // Blinks every 4-7 s (now and then twice) while the screen animates.
        if (env > 0.5f && now >= nextBlink_) {
            if (nextBlink_ > 0) blinkAt_ = now;
            ++blinkN_;
            nextBlink_ = now + (blinkN_ % 5 == 0 ? 330.0 : 4000.0 + 3000.0 * std::fmod(blinkN_ * 0.618034, 1.0));
        }
        P.face = now - blinkAt_ < 150 ? ToutouArt::FaceBlink : ToutouArt::FaceIdle;
    }
    P.phonePose = P.face == ToutouArt::FaceAsleep ? kAsleep : P.face == ToutouArt::FaceSleepy ? kSleepy : kHold;
    P.lit = !paused;
    P.opacity = paused ? 0.84f : 1.f;
    // Float: a slow sine (slower asleep, quicker while connecting) with a hint
    // of stretch while rising and squash while sinking; settles to rest.
    const double period = busy ? 1700 : paused ? 5200 : 3000;
    const float ph = static_cast<float>(std::fmod(now, period) / period) * 2 * kPi;
    const float amp = busy ? 5.f : paused ? 3.f : 6.f;  // frame units
    P.lift = env * (0.5f - 0.5f * std::cos(ph));
    P.dy = -amp * P.lift * L.u;
    const float st = 0.016f * env * std::sin(ph);
    P.sx = 1 - 0.8f * st;
    P.sy = 1 + st;
    if (busy && connectIntro_ && !reduced_ && surprise >= 0 && surprise < 850) {
        // A small lift while surprised; the face turns to the phone (160 ms
        // in, held, 350 ms back).
        if (surprise < kSurpriseMs) P.dy -= 3 * L.u * std::sin(kPi * static_cast<float>(surprise / kSurpriseMs));
        const float k = surprise < 160 ? outCubic(surprise / 160) : surprise < 500 ? 1.f : 1 - ease((surprise - 500) / 350);
        P.faceX = 2.5f * k;
        P.faceY = -1.f * k;
    }
    if (!reduced_ && scene_ != Scene::Live && now - fadeOutAt_ < kDropMs)  // back from a picture: drops in 10 px
        P.dy -= 10 * (1 - outCubic((now - fadeOutAt_) / kDropMs));
    float hx, hy, hdy;
    hopTransform(L, now, hx, hy, hdy);
    P.sx *= hx;
    P.sy *= hy;
    P.dy += hdy;
    const float h = static_cast<float>((now - mascotHotAt_) / kHoverMs);
    const bool hot = mascotHot_ && mascotClickable(now);
    const float grow = reduced_ ? 1.f : 1 + 0.04f * (hot ? ease(h) : (mascotHot_ ? 0.f : 1 - ease(h)));
    P.sx *= grow;
    P.sy *= grow;
    P.m = D2D1::Matrix3x2F::Translation(-kPivotX, -kPivotY) * D2D1::Matrix3x2F::Scale(P.sx * L.u, P.sy * L.u) *
          D2D1::Matrix3x2F::Translation(L.anchor.x, L.anchor.y + P.dy);
    // Beam: fast and full while connecting (it grows out first), a calm pulse
    // every 6 s on the idle screen, none asleep.
    if (busy && reduced_) {  // a still beam
        P.beamT1 = 1;
        P.beamK = 1;
    } else if (busy) {
        const double in = now - sceneAt_ - (connectIntro_ ? kConnectIntroMs * 0.6 : 0.0);
        P.beamT1 = ease(in / 450.0);
        P.beamK = P.beamT1 > 0 ? 1.f : 0.f;
        P.beamFlow = now / 600.0;
    } else if (!paused && !reacting && env > 0.003f) {
        const double p = std::fmod(now + 3500.0, 6000.0) / 1700.0;
        if (p < 1) {
            P.beamT1 = ease(p * 1.8);
            P.beamT0 = ease((p - 0.38) / 0.62);
            P.beamK = 0.8f * env;
            P.beamFlow = now / 2400.0;
        }
    }
    return P;
}

bool Renderer::ensureLayerBitmaps(float pxPerUnit) {
    const float q = toutou_.quantize(pxPerUnit);
    if (q == layerScale_ && layerBmp_[0]) return true;
    auto bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    for (int l = 0; l < ToutouArt::kLayers; ++l) {
        layerBmp_[l].Reset();
        IWICBitmap* src = toutou_.scaled(static_cast<ToutouArt::Layer>(l), pxPerUnit);
        if (!src || FAILED(d2dTarget_->CreateBitmapFromWicBitmap(src, &bp, &layerBmp_[l]))) {
            layerScale_ = 0;
            return false;
        }
    }
    layerScale_ = q;
    return true;
}

void Renderer::drawLayer(ToutouArt::Layer l, const D2D1_MATRIX_3X2_F& m, float opacity) {
    if (!layerBmp_[l]) return;
    const auto& p = toutou_.placed(l);
    d2dTarget_->SetTransform(m);
    d2dTarget_->DrawBitmap(layerBmp_[l].Get(), {p.x, p.y, p.x + p.w, p.y + p.h}, opacity,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void Renderer::drawMascot(const Layout& L, const Pose& P, double now) {
    using namespace toutou;
    if (!toutou_.loaded() || L.u <= 0 || !ensureLayerBitmaps(L.u * L.s)) return;
    // Ground shadow: stays on the ground, smaller and fainter as it floats up.
    const D2D1_POINT_2F g = framePoint(L, 135, 195);
    const float k = 1 - 0.22f * P.lift;
    const float rx = 64 * L.u * k * P.sx, ry = 10 * L.u * k;
    shadowBrush_->SetCenter(g);
    shadowBrush_->SetRadiusX(rx);
    shadowBrush_->SetRadiusY(ry);
    shadowBrush_->SetOpacity((0.9f - 0.35f * P.lift) * P.opacity);
    d2dTarget_->FillEllipse({g, rx, ry}, shadowBrush_.Get());

    // The phone screen breathes; the rim light on the cloud follows it.
    const float breath =
        reduced_ ? 1.f : 0.5f - 0.5f * std::cos(static_cast<float>(std::fmod(now, 4200.0) / 4200) * 2 * kPi);
    const float glow = P.lit ? 0.75f + 0.25f * breath : 0.f;
    drawLayer(ToutouArt::Cloud, P.m, P.opacity);
    if (layerBmp_[ToutouArt::Rim]) {
        const auto& r = toutou_.placed(ToutouArt::Rim);
        const D2D1_RECT_F dst{r.x, r.y, r.x + r.w, r.y + r.h};
        d2dTarget_->SetTransform(P.m);
        d2dTarget_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);  // required by FillOpacityMask
        d2dTarget_->FillOpacityMask(layerBmp_[ToutouArt::Rim].Get(),
                                    brush(pal_.accent, (P.lit ? 0.9f * glow : 0.12f) * P.opacity),
                                    D2D1_OPACITY_MASK_CONTENT_GRAPHICS, &dst, nullptr);
        d2dTarget_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }
    drawLayer(P.face, P.faceX || P.faceY ? D2D1::Matrix3x2F::Translation(P.faceX, P.faceY) * P.m : P.m, P.opacity);
    drawPhone(P, glow);
    drawLayer(static_cast<ToutouArt::Layer>(ToutouArt::HandHold + P.phonePose), P.m, P.opacity);
    drawBeam(P, P.beamT0, P.beamT1, P.beamK, P.beamFlow);
    d2dTarget_->SetTransform(D2D1::IdentityMatrix());
}

void Renderer::drawPhone(const Pose& P, float glow) {
    using namespace toutou;
    const Phone& ph = kPhone[P.phonePose];
    const auto m = D2D1::Matrix3x2F::Rotation(ph.rot) * D2D1::Matrix3x2F::Translation(ph.x, ph.y) *
                   D2D1::Matrix3x2F::Rotation(kTilt, {kPivotX, kPivotY}) * P.m;
    d2dTarget_->SetTransform(m);
    const float w = ph.w / 2, h = ph.h / 2;
    if (glow > 0 && phoneGlowBrush_) {  // soft halo in the accent
        const float rx = w + 15, ry = h + 13;
        phoneGlowBrush_->SetCenter({0, 0});
        phoneGlowBrush_->SetRadiusX(rx);
        phoneGlowBrush_->SetRadiusY(ry);
        phoneGlowBrush_->SetOpacity(glow * P.opacity);
        d2dTarget_->FillEllipse({{0, 0}, rx, ry}, phoneGlowBrush_.Get());
    }
    d2dTarget_->FillRoundedRectangle({{-w, -h, w, h}, kPhoneRadius, kPhoneRadius}, brush(kToutouInk, P.opacity));
    const float b = kPhoneBezel, r = kPhoneRadius - b + 0.4f;
    const D2D1_ROUNDED_RECT scr{{-w + b, -h + b, w - b, h - b}, r, r};
    if (P.lit) {
        screenBrush_->SetStartPoint({0, -h});
        screenBrush_->SetEndPoint({0, h});
        screenBrush_->SetOpacity(P.opacity);
        d2dTarget_->FillRoundedRectangle(scr, screenBrush_.Get());
        if (playTri_) {
            d2dTarget_->FillGeometry(playTri_.Get(), brush({1, 1, 1, 1}, 0.95f * P.opacity));
            d2dTarget_->DrawGeometry(playTri_.Get(), brush({1, 1, 1, 1}, 0.95f * P.opacity), 2.2f, round_.Get());
        }
    } else {
        const D2D1_COLOR_F a = pal_.accent, d = rgb(0x2B2226);
        d2dTarget_->FillRoundedRectangle(
            scr, brush({a.r + (d.r - a.r) * 0.62f, a.g + (d.g - a.g) * 0.62f, a.b + (d.b - a.b) * 0.62f, 1}, P.opacity));
    }
    d2dTarget_->FillRoundedRectangle({{-4, -h + 4.4f, 4, -h + 6.4f}, 1, 1}, brush(kToutouInk, 0.55f * P.opacity));
}

void Renderer::drawBeam(const Pose& P, float t0, float t1, float k, double flow) {
    using namespace toutou;
    if (k <= 0.003f || t1 - t0 <= 0.004f || !beamBrush_) return;
    const Phone& ph = kPhone[P.phonePose];
    const float a = ph.rot * kPi / 180;
    // Leaves the phone top along its axis and arcs to the upper right.
    const D2D1_POINT_2F p0{ph.x + std::sin(a) * (ph.h / 2 + 1), ph.y - std::cos(a) * (ph.h / 2 + 1)};
    const D2D1_POINT_2F p1{p0.x + 58, p0.y - 66};
    const float d = std::hypot(p1.x - p0.x, p1.y - p0.y);
    const D2D1_POINT_2F c{p0.x + std::sin(a) * d * 0.55f, p0.y - std::cos(a) * d * 0.55f};
    auto at = [&](float t) {
        const float u = 1 - t;
        return D2D1_POINT_2F{u * u * p0.x + 2 * u * t * c.x + t * t * p1.x, u * u * p0.y + 2 * u * t * c.y + t * t * p1.y};
    };
    auto normal = [&](float t) {  // unit normal of the centre line
        const float dx = 2 * (1 - t) * (c.x - p0.x) + 2 * t * (p1.x - c.x);
        const float dy = 2 * (1 - t) * (c.y - p0.y) + 2 * t * (p1.y - c.y);
        const float l = std::max(std::hypot(dx, dy), 1e-3f);
        return D2D1_POINT_2F{-dy / l, dx / l};
    };
    // Never across the idle cards (0.7.8: the ribbon and its pixel sparkles
    // showed through the iPhone card's corner): the whole path (with its
    // widest band and the sparkles beside it) is tested, so it does not pop
    // in or out half way; it stays off while it would touch them.
    if (beamAvoid_.right > beamAvoid_.left) {
        const auto m = D2D1::Matrix3x2F::Rotation(kTilt, {kPivotX, kPivotY}) * P.m;
        const float reach = 24 * 1.6f * std::abs(P.m._11) + 8;  // 2.4 x half the widest, the sparkles, a gap
        for (int i = 0; i <= 20; ++i) {
            const D2D1_POINT_2F q = m.TransformPoint(at(i / 20.f));
            if (q.x + reach > beamAvoid_.left && q.x - reach < beamAvoid_.right && q.y + reach > beamAvoid_.top &&
                q.y - reach < beamAvoid_.bottom)
                return;
        }
    }
    const float phase = static_cast<float>(std::fmod(flow, 1.0));
    auto width = [&](float t) {  // grows like a projection, breathes with a slow twist
        return (13 + 11 * t) * (0.84f + 0.16f * std::cos(2 * kPi * (t * 1.1f - phase)));
    };
    auto ribbon = [&](float scale, float ta, float tb) {
        ComPtr<ID2D1PathGeometry> g;
        ComPtr<ID2D1GeometrySink> s;
        if (FAILED(d2d_->CreatePathGeometry(&g)) || FAILED(g->Open(&s))) return ComPtr<ID2D1PathGeometry>();
        constexpr int N = 24;
        D2D1_POINT_2F side[N + 1];
        for (int i = 0; i <= N; ++i) {
            const float t = ta + (tb - ta) * i / N;
            const D2D1_POINT_2F p = at(t), n = normal(t);
            const float hw = width(t) * scale * 0.5f;
            side[i] = {p.x - n.x * hw, p.y - n.y * hw};
            if (i == 0) s->BeginFigure({p.x + n.x * hw, p.y + n.y * hw}, D2D1_FIGURE_BEGIN_FILLED);
            else s->AddLine({p.x + n.x * hw, p.y + n.y * hw});
        }
        for (int i = N; i >= 0; --i) s->AddLine(side[i]);
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        s->Close();
        return g;
    };
    d2dTarget_->SetTransform(D2D1::Matrix3x2F::Rotation(kTilt, {kPivotX, kPivotY}) * P.m);
    beamBrush_->SetStartPoint(p0);
    beamBrush_->SetEndPoint(p1);
    beamCoreBrush_->SetStartPoint(p0);
    beamCoreBrush_->SetEndPoint(p1);
    const float op = k * P.opacity;
    struct Band {
        float scale, alpha;
        ID2D1Brush* b;
    };
    const Band bands[] = {{2.4f, 0.10f, beamBrush_.Get()}, {1.6f, 0.20f, beamBrush_.Get()},
                          {1.0f, 1.0f, beamBrush_.Get()}, {0.4f, 1.0f, beamCoreBrush_.Get()}};
    for (const auto& band : bands) {
        if (auto g = ribbon(band.scale, t0, t1)) {
            band.b->SetOpacity(op * band.alpha);
            d2dTarget_->FillGeometry(g.Get(), band.b);
        }
    }
    // A brighter pulse travelling along it while connecting.
    if (k >= 0.99f) {
        const float x = static_cast<float>(std::fmod(flow * 0.55, 1.0));
        const float ta = std::max(t0, x - 0.09f), tb = std::min(t1, x + 0.09f);
        if (tb > ta)
            if (auto g = ribbon(0.8f, ta, tb))
                d2dTarget_->FillGeometry(g.Get(), brush({1, 1, 1, 1}, 0.35f * op * std::sin(kPi * x)));
    }
    // Square pixel sparkles riding along (white, accent, light accent).
    struct Px {
        float t, side, size;
        int c;
    };
    static constexpr Px kPx[] = {{0.16f, 0.9f, 3.4f, 0},  {0.27f, -1.0f, 2.6f, 1}, {0.38f, 1.25f, 4.4f, 2},
                                 {0.50f, -1.35f, 3.2f, 0}, {0.60f, 0.6f, 2.4f, 1},  {0.70f, 1.55f, 5.0f, 2},
                                 {0.82f, -1.2f, 3.6f, 0},  {0.93f, 0.9f, 2.6f, 1}};
    const D2D1_COLOR_F light{(pal_.accent.r + 1) / 2, (pal_.accent.g + 1) / 2, (pal_.accent.b + 1) / 2, 1};
    for (const auto& p : kPx) {
        const float t = static_cast<float>(std::fmod(p.t + flow * 0.25, 1.0));
        if (t < t0 || t > t1) continue;
        const D2D1_POINT_2F q = at(t), n = normal(t);
        const float off = (13 + 11 * t) * 0.5f * p.side;
        const float x = q.x + n.x * off, y = q.y + n.y * off, hs = p.size / 2;
        const float al = std::min(1.f, 1.6f * std::sin(kPi * t)) * op;
        d2dTarget_->FillRectangle({x - hs, y - hs, x + hs, y + hs},
                                  brush(p.c == 0 ? D2D1_COLOR_F{1, 1, 1, 1} : p.c == 1 ? pal_.accent : light, al));
    }
}

// 「Found a phone」: five small sparkles from the phone's top (white and the
// accent), 180-780 ms after the surprise.
void Renderer::drawSparkles(const Pose& P, double now) {
    const double e = now - surpriseAt_ - 180;
    if (reduced_ || e < 0 || e > 600 || !sparkle_ || scene_ != Scene::Connecting) return;
    const float t = static_cast<float>(e / 600);
    const D2D1_POINT_2F o = P.m.TransformPoint({224, 96});
    const float u = P.m._11;  // DIPs per frame unit (incl. squash)
    const float grow = t < 0.3f ? outCubic(t / 0.3f) : 1 - inCubic((t - 0.3f) / 0.7f);
    for (int i = 0; i < 5; ++i) {
        const float ang = (-160.f + i * 32.f) * kPi / 180.f;
        const float dist = u * (7 + 13 * outCubic(t));
        const float size = u * (4.5f + 1.5f * (i % 2)) * grow;
        if (size <= 0.05f) continue;
        d2dTarget_->SetTransform(D2D1::Matrix3x2F::Scale(size, size) * D2D1::Matrix3x2F::Rotation(45 * t + i * 9) *
                                 D2D1::Matrix3x2F::Translation(o.x + std::cos(ang) * dist, o.y + std::sin(ang) * dist));
        d2dTarget_->FillGeometry(sparkle_.Get(), brush(i % 2 ? pal_.accent : D2D1_COLOR_F{1, 1, 1, 1}, 0.95f));
    }
    d2dTarget_->SetTransform(D2D1::IdentityMatrix());
}

// Hearts bursting from the top of the cloud after a click / when a phone
// connects (accent and a lighter accent).
void Renderer::drawMascotFx(const Layout& L, const Pose& P, double now) {
    drawSparkles(P, now);
    const double e = now - reactAt_;
    if (reduced_ || e < 0 || e > 1150 || !heart_ || L.u <= 0 || P.face == ToutouArt::FaceSleepy) return;
    const D2D1_POINT_2F o = P.m.TransformPoint({132, 58});
    const float te = static_cast<float>(e / 1150.0);
    constexpr int kHearts = 7;
    for (int i = 0; i < kHearts; ++i) {
        const float ang = (-90.f + (i - 3) * 25.f + 7.f * std::sin(i * 2.3f)) * kPi / 180.f;
        const float dist = L.u * (34 + 11 * (i % 3)) * (1 - (1 - te) * (1 - te) * (1 - te));
        const float rise = L.u * 9 * te * te;
        const float x = o.x + std::cos(ang) * dist * 1.25f, y = o.y + std::sin(ang) * dist - rise;
        const float pop = te < 0.15f ? 1.2f * ease(te / 0.15f) : 1.2f - 0.2f * ease((te - 0.15f) / 0.2f);
        const float size = L.u * (9 + 2.5f * (i % 3)) * pop;
        const float a = te < 0.55f ? 1.f : 1 - ease((te - 0.55f) / 0.45f);
        const float rot = 14 * std::sin(static_cast<float>(e) * 0.012f + i);
        D2D1_COLOR_F c = pal_.accent;
        if (i % 3 == 1) {  // a few lighter ones
            c.r += (1 - c.r) * 0.35f;
            c.g += (1 - c.g) * 0.35f;
            c.b += (1 - c.b) * 0.35f;
        }
        d2dTarget_->SetTransform(D2D1::Matrix3x2F::Scale(size, size) * D2D1::Matrix3x2F::Rotation(rot) *
                                 D2D1::Matrix3x2F::Translation(x, y));
        d2dTarget_->FillGeometry(heart_.Get(), brush(c, a));
    }
    d2dTarget_->SetTransform(D2D1::IdentityMatrix());
}

void Renderer::hopTransform(const Layout& L, double now, float& sx, float& sy, float& dy) const {
    sx = sy = 1;
    dy = 0;
    const double e = now - reactAt_;
    if (reduced_ || e < 0 || e >= kHopMs) return;
    const float t = static_cast<float>(e / kHopMs);
    const float mh = L.mascot.bottom - L.mascot.top;
    const float hop = mh * 0.06f;
    if (t < 0.12f) {  // anticipation: squash
        const float q = ease(t / 0.12f);
        sy = 1 - 0.06f * q;
        sx = 1 + 0.045f * q;
    } else if (t < 0.52f) {  // airborne: stretched, easing back
        const float p = (t - 0.12f) / 0.40f;
        dy = -hop * 4 * p * (1 - p);
        sy = 1 + 0.045f * (1 - p);
        sx = 1 - 0.03f * (1 - p);
    } else if (t < 0.70f) {  // landing squash
        const float w = std::sin(kPi * (t - 0.52f) / 0.18f);
        sy = 1 - 0.07f * w;
        sx = 1 + 0.05f * w;
    } else {  // settle wobble
        const float q = (t - 0.70f) / 0.30f;
        const float w = std::sin(2 * kPi * q) * (1 - q);
        sy = 1 + 0.02f * w;
        sx = 1 - 0.015f * w;
    }
}

bool Renderer::mascotClickable(double now) const {
    if (!toutou_.loaded() || (!pin_.empty() && (pinVisible_ || now - pinAt_ < ms(kPinFadeMs)))) return false;
    if (paused_) return true;  // the paused screen
    return scene_ == Scene::Idle && !(havePicture_ && now - fadeOutAt_ < ms(kFadeOutMs));
}

void Renderer::setMascotHover(bool hot) {
    if (hot == mascotHot_) return;
    const double now = clockMs();
    // Reverse an unfinished ease from where it is.
    const double e = std::clamp((now - mascotHotAt_) / kHoverMs, 0.0, 1.0);
    mascotHotAt_ = now - (1 - e) * kHoverMs;
    mascotHot_ = hot;
}

void Renderer::mascotClicked() {
    const double now = clockMs();
    if (!mascotClickable(now)) return;
    reactAt_ = now;
    reactBubble_ = true;
    ++bubbleLine_;
    poke();
}

// Speech bubble above the cloud (click reaction).
void Renderer::drawBubble(const Layout& L, const Pose& P, double now) {
    if (!reactBubble_) return;
    const double e = now - reactAt_;
    constexpr double kBubbleMs = kReactMs + 250;
    if (e < 0 || e > kBubbleMs || L.u <= 0) return;
    const wchar_t* text;
    if (paused_) text = tr(kSleepyLines[bubbleLine_ % static_cast<int>(std::size(kSleepyLines))]);
    else text = tr(kBubbleLines[bubbleLine_ % static_cast<int>(std::size(kBubbleLines))]);
    const float size = std::clamp(L.hint * 1.05f, 13.f, 26.f);
    auto l = layout(text, size, L.W * 0.6f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    if (!l) return;
    const float tw = textW(l.Get()), th = textH(l.Get());
    const float padX = size * 0.75f, padY = size * 0.42f, tail = size * 0.55f;
    const float bw = tw + 2 * padX, bh = th + 2 * padY;
    // Tail tip just above the big puff, right of its top; the bubble sits
    // above-right of it.
    D2D1_POINT_2F tip = P.m.TransformPoint({150, 46});
    float x0 = tip.x - bw * 0.18f, y0 = tip.y - tail - bh;
    x0 = std::clamp(x0, 8.f, std::max(8.f, L.W - 8 - bw));
    y0 = std::max(y0, 8.f);
    tip.x = std::clamp(tip.x, x0 + bh * 0.5f, x0 + bw - bh * 0.5f);
    const float te = static_cast<float>(e);
    float sc = 1;  // pop with a small overshoot (back ease-out)
    if (te < 220 && !reduced_) {
        const float q = te / 220 - 1;
        sc = 0.55f + 0.45f * (1 + 2.70158f * q * q * q + 1.70158f * q * q);
    }
    const float out0 = static_cast<float>(kBubbleMs - 250);
    const float a = te < 120 ? ease(te / 120) : te > out0 ? 1 - ease((te - out0) / 250) : 1.f;
    if (a <= 0.003f) return;
    d2dTarget_->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc, tip));
    ComPtr<ID2D1RoundedRectangleGeometry> box;
    ComPtr<ID2D1PathGeometry> tri, shape;
    ComPtr<ID2D1GeometrySink> sink;
    const D2D1_ROUNDED_RECT rr{{x0, y0, x0 + bw, y0 + bh}, bh / 2, bh / 2};
    if (SUCCEEDED(d2d_->CreateRoundedRectangleGeometry(rr, &box)) && SUCCEEDED(d2d_->CreatePathGeometry(&tri)) &&
        SUCCEEDED(tri->Open(&sink))) {
        const float bx = std::clamp(tip.x + tail * 0.3f, x0 + bh * 0.5f, x0 + bw - bh * 0.5f);
        sink->BeginFigure({bx - tail * 0.55f, y0 + bh - 1}, D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine({tip.x, tip.y});
        sink->AddLine({bx + tail * 0.35f, y0 + bh - 1});
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        sink.Reset();
        if (SUCCEEDED(d2d_->CreatePathGeometry(&shape)) && SUCCEEDED(shape->Open(&sink))) {
            box->CombineWithGeometry(tri.Get(), D2D1_COMBINE_MODE_UNION, nullptr, sink.Get());
            sink->Close();
            d2dTarget_->FillGeometry(shape.Get(), brush(pal_.card, 0.97f * a));
            d2dTarget_->DrawGeometry(shape.Get(), brush(pal_.accent, 0.75f * a), 1.5f, round_.Get());
        }
    }
    l->SetMaxWidth(std::ceil(tw) + 2.f);
    d2dTarget_->DrawTextLayout({x0 + padX - 1.f, y0 + padY}, l.Get(), brush(pal_.fg, a));
    d2dTarget_->SetTransform(D2D1::IdentityMatrix());
}

void Renderer::drawParticles(const Layout& L, double now, float strength) {
    if (strength <= 0.003f || !heart_ || !sparkle_) return;
    const float unit = std::clamp(std::min(L.W, L.H) / 700.f, 0.8f, 1.8f);
    constexpr int kCount = 9;
    for (int i = 0; i < kCount; ++i) {
        const double period = 17000 + 2600 * (i % 5);
        const float seed = static_cast<float>(std::fmod(i * 0.381966, 1.0));
        const float ph = static_cast<float>(std::fmod(now / period + seed, 1.0));
        const float x = L.W * static_cast<float>(std::fmod(0.13 + i * 0.6180339, 1.0)) +
                        16 * unit * std::sin(static_cast<float>(std::fmod(now * 0.0004, 2 * kPi)) + i * 1.7f);
        const float y = L.H * (1.04f - 1.12f * ph);
        const float size = (7 + 3 * (i % 3)) * unit;
        float a = strength * 0.32f * std::pow(std::sin(kPi * ph), 1.2f);
        // Nearly gone over the text block (0.7.2: a heart or sparkle drawn
        // across 「Android」 or the 怎麼連線？ link read as a stray glyph).
        if (textBlock_.right > textBlock_.left) {
            const float dx = std::max({textBlock_.left - x, 0.f, x - textBlock_.right});
            const float dy = std::max({textBlock_.top - y, 0.f, y - textBlock_.bottom});
            a *= std::clamp(std::sqrt(dx * dx + dy * dy) / (24 * unit), 0.1f, 1.f);
        }
        if (a <= 0.003f) continue;
        const float angle = 14 * std::sin(static_cast<float>(std::fmod(now * 0.0007, 2 * kPi)) + i);
        d2dTarget_->SetTransform(D2D1::Matrix3x2F::Scale(size, size) * D2D1::Matrix3x2F::Rotation(angle) *
                                 D2D1::Matrix3x2F::Translation(x, y));
        if (i % 3 == 2) d2dTarget_->FillGeometry(sparkle_.Get(), brush(pal_.fg, a * 0.9f));
        else d2dTarget_->FillGeometry(heart_.Get(), brush(i % 4 == 1 ? pal_.accent2 : pal_.accent, a));
    }
    d2dTarget_->SetTransform(D2D1::IdentityMatrix());
}

void Renderer::drawSpinner(D2D1_POINT_2F c, float radius, double now) {
    const float stroke = std::max(radius * 0.24f, 2.f);
    d2dTarget_->DrawEllipse({c, radius, radius}, brush(pal_.fg, 0.10f), stroke);
    const float start = static_cast<float>(std::fmod(now * 0.36, 360.0));  // 1 turn / s
    const float span = 100.f + 30.f * std::sin(static_cast<float>(std::fmod(now * 0.0021, 2 * kPi)));
    ComPtr<ID2D1PathGeometry> g;
    ComPtr<ID2D1GeometrySink> s;
    if (FAILED(d2d_->CreatePathGeometry(&g)) || FAILED(g->Open(&s))) return;
    s->BeginFigure(polar(c, radius, start), D2D1_FIGURE_BEGIN_HOLLOW);
    s->AddArc({polar(c, radius, start + span), {radius, radius}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
               D2D1_ARC_SIZE_SMALL});
    s->EndFigure(D2D1_FIGURE_END_OPEN);
    s->Close();
    d2dTarget_->DrawGeometry(g.Get(), brush(pal_.accent), stroke, round_.Get());
}

// Sleeping: three z rising from the top right of the cloud (accent).
void Renderer::drawZzz(const Layout& L, const Pose& P, double now) {
    if (L.u <= 0) return;
    struct Z {
        float x, y, size, alpha;
        const wchar_t* text;
    };
    static constexpr Z kZ[3] = {{188, 64, 11, 0.55f, L"z"}, {199, 50, 14, 0.72f, L"z"}, {212, 33, 18, 0.9f, L"Z"}};
    const float env = reduced_ ? 0.f : ease((ambientUntil_ - now) / kAmbientSettleMs);
    for (int i = 0; i < 3; ++i) {
        const auto& z = kZ[i];
        const float size = std::max(z.size * L.u, 10.f);
        auto l = layout(z.text, size, size * 2, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, true);
        if (!l) continue;
        // a slow drift up and back while the screen animates
        const float drift = env * 2.5f * std::sin(static_cast<float>(std::fmod(now / 2600.0, 1.0)) * 2 * kPi - i * 0.9f);
        const D2D1_POINT_2F c = framePoint(L, z.x, z.y - drift);
        d2dTarget_->DrawTextLayout({c.x - size / 2, c.y + P.dy - textH(l.Get()) / 2}, l.Get(), brush(pal_.accent, z.alpha));
    }
}

}  // namespace pm::video
