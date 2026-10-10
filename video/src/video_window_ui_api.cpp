// pm::VideoWindow public API: pointer / key handlers, idle screen, status, toast, live toolbar, theme, rotation, device frame, REC, frame tap.
// 拆檔 0.7.9：自 video_window.cpp 原樣搬出。
#include "pm/video_window.h"

#include "video_window_impl.h"

namespace pm {

void VideoWindow::setPointerHandler(std::function<void(const PointerEvent&)> handler) {
    auto fn = handler ? std::make_shared<std::function<void(const PointerEvent&)>>(std::move(handler)) : nullptr;
    std::lock_guard lk(impl_->m);
    impl_->pointerFn = std::move(fn);
}

void VideoWindow::setKeyHandler(std::function<void(unsigned vk, bool down, wchar_t ch)> handler) {
    auto fn = handler ? std::make_shared<std::function<void(unsigned, bool, wchar_t)>>(std::move(handler)) : nullptr;
    std::lock_guard lk(impl_->m);
    impl_->keyFn = std::move(fn);
}

void VideoWindow::setIdleActions(std::vector<IdleAction> actions) {
    std::vector<video::Renderer::Action> ra;
    std::vector<std::function<void()>> fns;
    for (auto& a : actions) {
        ra.push_back({a.label, a.primary, a.card});
        fns.push_back(std::move(a.onClick));
    }
    {
        std::lock_guard lk(impl_->m);
        impl_->actionsReq = std::move(ra);
        impl_->actionFns = std::move(fns);
        impl_->dirty = true;
    }
    impl_->wake();
}

void VideoWindow::setIdleHelpLink(const std::wstring& label, std::function<void()> onClick) {
    std::vector<IdleAction> a;
    if (!label.empty()) a.push_back({label, false, std::move(onClick)});
    setIdleActions(std::move(a));
}

void VideoWindow::setIdleCards(std::vector<IdleCard> cards) {
    std::vector<video::Renderer::Card> rc;
    for (auto& c : cards) rc.push_back({std::move(c.title), std::move(c.body), std::move(c.note), c.muted});
    if (rc.size() > 3) rc.resize(3);
    {
        std::lock_guard lk(impl_->m);
        impl_->cardsReq = std::move(rc);
        impl_->dirty = true;
    }
    impl_->wake();
}

void VideoWindow::setIdleHints(std::vector<std::wstring> lines) {
    if (lines.size() > 3) lines.resize(3);
    {
        std::lock_guard lk(impl_->m);
        impl_->hintsReq = std::move(lines);
        impl_->dirty = true;
    }
    impl_->wake();
}

// ---- status / interaction UI ----

void VideoWindow::setConnecting(const std::wstring& deviceName) {
    {
        std::lock_guard lk(impl_->m);
        impl_->connectingReq = deviceName;
        impl_->dirty = true;
    }
    impl_->wake();
}

void VideoWindow::setIdleOptions(std::vector<IdleOption> options, std::function<void(int id, bool checked)> onToggle) {
    {
        std::lock_guard lk(impl_->m);
        impl_->options = std::move(options);
        impl_->onToggle = std::move(onToggle);
        impl_->optionsChanged = true;
        impl_->dirty = true;
    }
    impl_->wake();
}

void VideoWindow::showPin(const std::wstring& pin) {
    {
        std::lock_guard lk(impl_->m);
        impl_->pinReq = pin;
        impl_->dirty = true;
    }
    impl_->wake();
}

void VideoWindow::showToast(const std::wstring& text) { showToast(text, 0); }

void VideoWindow::flash() {
    {
        std::lock_guard lk(impl_->m);
        impl_->flashReq = true;
    }
    impl_->wake();
}

void VideoWindow::showToast(const std::wstring& text, int holdMs) {
    {
        std::lock_guard lk(impl_->m);
        impl_->toastReq = text;
        impl_->toastHoldReq = holdMs;
        impl_->dirty = true;
    }
    impl_->wake();
}

void VideoWindow::setLiveToolbar(std::vector<ToolbarItem> items, std::function<void(int id)> onClick) {
    std::vector<video::Renderer::ToolItem> ri;
    std::vector<int> ids;
    for (auto& t : items) {
        ri.push_back({t.glyph, std::move(t.tooltip), t.toggled, t.danger, t.groupStart, t.recording, t.optional,
                      t.slider, std::move(t.label)});
        ids.push_back(t.id);
    }
    {
        std::lock_guard lk(impl_->m);
        impl_->toolItemsUi = ri;
        impl_->toolbarReq = std::move(ri);
        impl_->toolIds = std::move(ids);
        impl_->toolFn = std::move(onClick);
        impl_->dirty = true;
    }
    impl_->wake();
}

void VideoWindow::revealToolbar(int holdMs, const std::wstring& note) {
    {
        std::lock_guard lk(impl_->m);
        impl_->toolRevealReq = std::make_pair(static_cast<double>(holdMs), note);
        impl_->dirty = true;
    }
    impl_->wake();
}

void VideoWindow::setLiveToolbarSlider(std::function<void(int id, float value, bool done)> onSlide,
                                       std::function<void(int id, int notches)> onWheel) {
    std::lock_guard lk(impl_->m);
    impl_->toolSlideFn = std::move(onSlide);
    impl_->toolWheelFn = std::move(onWheel);
}

namespace {
constexpr uint32_t hex(const D2D1_COLOR_F& c) {
    return (static_cast<uint32_t>(c.r * 255 + 0.5f) << 16) | (static_cast<uint32_t>(c.g * 255 + 0.5f) << 8) |
           static_cast<uint32_t>(c.b * 255 + 0.5f);
}
}  // namespace

void VideoWindow::setDimmed(bool dimmed) {
    {
        std::lock_guard lk(impl_->m);
        impl_->dimReq = dimmed ? 1 : 0;
    }
    impl_->wake();
}

void VideoWindow::setRotation(int quarterTurnsClockwise) {
    const int r = ((quarterTurnsClockwise % 4) + 4) % 4;
    if (impl_->rotation.exchange(r) == r) return;
    {
        std::lock_guard lk(impl_->m);
        impl_->xformChanged = true;
    }
    impl_->wake();
}

void VideoWindow::setMirrored(bool horizontal) {
    if (impl_->mirrored.exchange(horizontal) == horizontal) return;
    {
        std::lock_guard lk(impl_->m);
        impl_->xformChanged = true;
    }
    impl_->wake();
}

void VideoWindow::desiredClientAspect(int& w, int& h) const {
    w = impl_->srcW.load();
    h = impl_->srcH.load();
    if (impl_->rotation.load() & 1) std::swap(w, h);
}

void VideoWindow::setDeviceFrame(bool enabled) {
    {
        std::lock_guard lk(impl_->m);
        impl_->frameReq = enabled ? 1 : 0;
    }
    impl_->wake();
}

void VideoWindow::setTheme(Theme t) {
    {
        std::lock_guard lk(impl_->m);
        impl_->themeReq = std::clamp(static_cast<int>(t), 0, 3);
    }
    impl_->wake();
}

std::array<uint32_t, 3> VideoWindow::themeSwatch(Theme t) {
    const auto& p = video::Renderer::palette(static_cast<int>(t));
    return {hex(p.bgTop), hex(p.card), hex(p.accent)};
}

void VideoWindow::setRecording(bool active) {
    {
        std::lock_guard lk(impl_->m);
        impl_->recReq = active ? 1 : 0;
    }
    impl_->wake();
}

void VideoWindow::setFrameTap(FrameTap tap) {
    auto fn = tap ? std::make_shared<FrameTap>(std::move(tap)) : nullptr;
    const bool on = fn != nullptr;
    if (GetCurrentThreadId() == impl_->workerTid.load()) {
        impl_->tapFn = std::move(fn);  // from inside the tap: the worker holds tapCallM
    } else {
        std::lock_guard lk(impl_->tapCallM);  // waits for a running tap call
        impl_->tapFn = std::move(fn);
    }
    impl_->tapOn = on;
    {
        std::lock_guard lk(impl_->m);
        impl_->tapChanged = true;
    }
    impl_->wake();
}

// ---- Magnifier / filters / freeze / text overlay ----

}  // namespace pm
