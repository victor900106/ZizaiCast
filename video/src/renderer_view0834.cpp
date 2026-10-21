// Renderer: 0.8.3.4 畫面組 -- the toolbar's grip while it is hidden (B1), where
// the 凍結 badge goes while the toolbar is up and what it says (B3).  The
// clickable toast (B2) is drawn by drawToast (renderer_frame.cpp), the pen
// mode frame and badge (C3) by drawInkBadge (renderer_ink.cpp).
#include "renderer.h"

#include <algorithm>
#include <cmath>

#include "pm/i18n.h"
#include "renderer_internal.h"

namespace pm::video {

using namespace detail;

// The toolbar pill's top edge (DIPs) over `area`: below the Dynamic Island
// when framed in portrait (drawToolbar and the grip share it).
float Renderer::toolbarTop(const D2D1_RECT_F& area, const FrameGeom* frame) const {
    float top = area.top + 10;
    if (frame) {
        const float s = dpi_ / 96.f;
        const float pw = std::min(frame->screen.right - frame->screen.left, frame->screen.bottom - frame->screen.top);
        top = frame->landscape ? area.top + std::max(10.f, frame->radius * 0.30f / s) : area.top + pw * 0.122f / s + 8;
    }
    return top;
}

// B1: the pointer is over the window and the toolbar has faded out: a small
// grip where it lives says "the tools are up here" (fades out as it comes back).
void Renderer::drawToolbarGrip(const D2D1_RECT_F& area, const FrameGeom* frame, float toolbarA) {
    if (!pointerIn_ || !toolbarAvailable() || !d2dTarget_) return;
    const float ga = 1 - toolbarA;
    if (ga <= 0.003f) return;
    float cx = std::round((area.left + area.right) / 2);
    const float top = std::round(toolbarTop(area, frame)) + 2;
    const float w = 44, h = 6;
    // Not under the 凍結 badge (a narrow picture): left of it, or none.
    const D2D1_RECT_F& fb = frozenDip_;
    if (fb.right > fb.left && cx + w / 2 + 6 > fb.left && cx - w / 2 < fb.right && top < fb.bottom && top + h > fb.top) {
        cx = std::round(fb.left - 10 - w / 2);
        if (cx - w / 2 < area.left + 8) return;
    }
    const D2D1_ROUNDED_RECT r{{cx - w / 2, top, cx + w / 2, top + h}, h / 2, h / 2};
    d2dTarget_->FillRoundedRectangle({{r.rect.left - 1.5f, r.rect.top - 0.5f, r.rect.right + 1.5f, r.rect.bottom + 2.5f}, h, h},
                                     brush(D2D1::ColorF(0, 0, 0), 0.22f * ga));
    d2dTarget_->FillRoundedRectangle(r, brush(pal_.accent, 0.92f * ga));
    d2dTarget_->DrawRoundedRectangle(r, brush(pal_.card, 0.85f * ga), 1.f);
}

std::wstring Renderer::frozenLabel() const { return frozenText_.empty() ? std::wstring(tr(S::FrozenBadge)) : frozenText_; }

// B3: the 凍結 badge (top right) moves below the toolbar while that is up over
// it (it slides with the toolbar's fade), instead of hiding under the pill.
// Only where it is drawn: the translation boxes keep clear of its resting place.
D2D1_RECT_F Renderer::frozenBadgeShown(const D2D1_RECT_F& at, double now) const {
    const float a = toolbarAlpha(now);
    if (a <= 0.003f || pillDip_.right <= pillDip_.left || at.right <= at.left) return at;
    const bool over = at.left < pillDip_.right + 4 && at.right > pillDip_.left - 4 && at.top < pillDip_.bottom + 4 &&
                      at.bottom > pillDip_.top - 4;
    if (!over) return at;
    const float dy = std::round((pillDip_.bottom + 8 - at.top) * a);
    return {at.left, at.top + dy, at.right, at.bottom + dy};
}

}  // namespace pm::video
