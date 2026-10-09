// 音量 / 靜音 UI (volume_ui.h).
#include "volume_ui.h"

#include <atomic>
#include <cmath>
#include <cstdio>

#include "pm/i18n.h"
#include "volume_memory.h"

using pm::i18n::fmt;
using pm::i18n::S;
using pm::i18n::tr;

namespace pm::ui {
namespace {

// Segoe Fluent Icons / Segoe MDL2 Assets
constexpr wchar_t kIcoMute = 0xE74F;     // Mute
constexpr wchar_t kIcoVolume1 = 0xE993;  // Volume1 .. Volume3 (speaker + 1..3 waves)
constexpr wchar_t kIcoVolume2 = 0xE994;
constexpr wchar_t kIcoVolume3 = 0xE995;
constexpr wchar_t kIcoVolUp = 0xE710;    // Add
constexpr wchar_t kIcoVolDown = 0xE738;  // Remove

constexpr UINT kPhoneMsg = WM_APP + 1;
constexpr long long kQuietAfterConnectMs = 3000;  // the phone reports its level on connecting: no toast
std::atomic<bool> g_posted{false};

std::wstring pct(int p) { return std::to_wstring(p); }

}  // namespace

void VolumeControl::attach(RememberVolumeAudioSink* sink, Hooks hooks) {
    sink_ = sink;
    hooks_ = std::move(hooks);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PhoneMirror.Volume";
    RegisterClassW(&wc);
    msgWnd_ = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (msgWnd_) SetWindowLongPtrW(msgWnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    // Phone thread -> UI thread, at most one message in flight.
    const HWND w = msgWnd_;
    sink_->onPhoneChange = [w]() {
        if (w && !g_posted.exchange(true)) PostMessageW(w, kPhoneMsg, 0, 0);
    };
}

void VolumeControl::detach() {
    if (msgWnd_) {
        SetWindowLongPtrW(msgWnd_, GWLP_USERDATA, 0);
        DestroyWindow(msgWnd_);
        msgWnd_ = nullptr;
    }
    sink_ = nullptr;
}

LRESULT CALLBACK VolumeControl::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kPhoneMsg) {
        g_posted = false;
        if (auto* self = reinterpret_cast<VolumeControl*>(GetWindowLongPtrW(h, GWLP_USERDATA))) self->onPhoneChanged();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool VolumeControl::muted() const { return sink_ && sink_->muted(); }

float VolumeControl::shownFraction() const {
    if (!sink_ || sink_->phoneMuted()) return 0.0f;
    return pm::vol::fractionOf(sink_->saved());
}

int VolumeControl::shownPercent() const { return static_cast<int>(std::lround(shownFraction() * 100.0f)); }

void VolumeControl::toastLevel() {
    if (!hooks_.toast || !sink_) return;
    if (sink_->muted()) hooks_.toast(fmt(S::VolToastMutedLevel, {pct(shownPercent())}));
    else if (sink_->phoneMuted()) hooks_.toast(tr(S::VolToastPhoneMuted));
    else hooks_.toast(fmt(S::VolToast, {pct(shownPercent())}));
}

void VolumeControl::setLevel(float db, const char* how) {
    if (!sink_) return;
    const bool wasMuted = sink_->muted();
    sink_->setLevel(db);  // also unmutes (a level change means "I want to hear it")
    if (wasMuted && hooks_.saveMuted) hooks_.saveMuted(false);
    if (how && hooks_.log) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "volume: %d%% (%.2f dB) by %s%s", pm::vol::percentOf(sink_->saved()),
                      sink_->saved(), how, wasMuted ? ", unmuted" : "");
        hooks_.log(buf);
    }
    toastLevel();
    if (hooks_.changed) hooks_.changed();
}

void VolumeControl::setMuted(bool m, const char* how) {
    if (!sink_) return;
    sink_->setMuted(m);
    if (hooks_.saveMuted) hooks_.saveMuted(m);
    if (hooks_.log) hooks_.log(std::string("volume: ") + (m ? "muted" : "unmuted") + " by " + how);
    if (hooks_.toast) {
        if (m) hooks_.toast(tr(S::VolToastMuted));
        else toastLevel();
    }
    if (hooks_.changed) hooks_.changed();
}

void VolumeControl::onPhoneChanged() {
    if (!sink_) return;
    if (hooks_.log) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "volume: phone set %d%% (%.2f dB)%s%s", pm::vol::percentOf(sink_->saved()),
                      sink_->saved(), sink_->phoneMuted() ? ", phone muted" : "", sink_->muted() ? ", app muted" : "");
        hooks_.log(buf);
    }
    if (sink_->msSinceSession() > kQuietAfterConnectMs) toastLevel();
    if (hooks_.changed) hooks_.changed();
}

bool VolumeControl::runCommand(UINT cmd) {
    if (!sink_) return false;
    switch (cmd) {
    case CmdVolMute: setMuted(!sink_->muted(), "command"); return true;
    case CmdVolUp:
    case CmdVolDown: {
        // From what is heard: a phone mute counts as 0 %.
        const float from = sink_->phoneMuted() ? pm::vol::kMinDb : sink_->saved();
        setLevel(pm::vol::stepDb(from, cmd == CmdVolUp ? 1 : -1), cmd == CmdVolUp ? "step up" : "step down");
        return true;
    }
    case CmdVolSet100:
    case CmdVolSet75:
    case CmdVolSet50:
    case CmdVolSet25: setLevel(pm::vol::dbOfPercent(100 - 25 * static_cast<int>(cmd - CmdVolSet100)), "menu"); return true;
    case CmdVolSlider: return true;  // not a command (the slider reports through slide())
    }
    return false;
}

bool VolumeControl::ctrlKey(WPARAM vk) {
    switch (vk) {
    case VK_UP: return runCommand(CmdVolUp);
    case VK_DOWN: return runCommand(CmdVolDown);
    case 'M': return runCommand(CmdVolMute);
    }
    return false;
}

void VolumeControl::slide(float fraction, bool done) {
    if (!sink_) return;
    // Whole percents while dragging; logged once, on release.
    const float db = pm::vol::dbOfPercent(static_cast<int>(std::lround(std::clamp(fraction, 0.f, 1.f) * 100.f)));
    if (!dragging_) {
        dragging_ = true;
        dragStartDb_ = sink_->saved();
    }
    if (db != sink_->saved() || sink_->muted() || sink_->phoneMuted()) setLevel(db, nullptr);
    if (done) {
        dragging_ = false;
        if (hooks_.log) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "volume: %d%% (%.2f dB) by slider (from %.2f dB)",
                          pm::vol::percentOf(sink_->saved()), sink_->saved(), dragStartDb_);
            hooks_.log(buf);
        }
    }
}

void VolumeControl::wheel(int notches) {
    if (!sink_ || !notches) return;
    const float from = sink_->phoneMuted() ? pm::vol::kMinDb : sink_->saved();
    setLevel(pm::vol::stepDb(from, notches), "wheel");
}

void VolumeControl::appendToolbar(std::vector<pm::VideoWindow::ToolbarItem>& v, bool groupStart) const {
    if (!sink_) return;
    using TI = pm::VideoWindow::ToolbarItem;
    const bool m = sink_->muted();
    const float f = shownFraction();
    const bool silentNow = m || f <= 0.0f;
    const wchar_t glyph = silentNow ? kIcoMute : f < 0.34f ? kIcoVolume1 : f < 0.67f ? kIcoVolume2 : kIcoVolume3;
    TI speaker{static_cast<int>(CmdVolMute), glyph, tr(m ? S::VolTipUnmute : S::VolTipMute)};
    speaker.toggled = m;
    speaker.groupStart = groupStart;
    speaker.optional = true;  // left out only after the slider, in the narrowest windows (Ctrl+M / menus)
    v.push_back(speaker);
    TI slider{static_cast<int>(CmdVolSlider), 0, fmt(S::VolTipSlider, {pct(shownPercent())})};
    slider.slider = f;
    slider.toggled = m;  // greyed while muted (the level is kept)
    v.push_back(slider);
}

MenuItem VolumeControl::menuItem() const {
    std::vector<MenuItem> sub;
    const bool m = muted();
    MenuItem mute = MenuItem::command(CmdVolMute, tr(S::MenuVolMute), kIcoMute, L"Ctrl+M");
    mute.checkable = true;
    mute.checked = m;
    sub.push_back(std::move(mute));
    sub.push_back(MenuItem::separator());
    const int p = shownPercent();
    MenuItem up = MenuItem::command(CmdVolUp, tr(S::MenuVolUp), kIcoVolUp, L"Ctrl+↑");
    up.enabled = sink_ && (m || p < 100);
    sub.push_back(std::move(up));
    MenuItem down = MenuItem::command(CmdVolDown, tr(S::MenuVolDown), kIcoVolDown, L"Ctrl+↓");
    down.enabled = sink_ && (m || p > 0);
    sub.push_back(std::move(down));
    sub.push_back(MenuItem::separator());
    for (int i = 0; i < 4; ++i) {
        const int preset = 100 - 25 * i;
        MenuItem r = MenuItem::command(CmdVolSet100 + i, std::to_wstring(preset) + L"%");
        r.radio = true;
        r.checked = !m && p == preset;
        sub.push_back(std::move(r));
    }
    sub.push_back(MenuItem::note(tr(S::MenuVolNote)));
    const float f = shownFraction();
    const wchar_t icon = m || f <= 0.0f ? kIcoMute : f < 0.34f ? kIcoVolume1 : f < 0.67f ? kIcoVolume2 : kIcoVolume3;
    return MenuItem::submenu(m ? std::wstring(tr(S::MenuVolumeMuted)) : fmt(S::MenuVolume, {pct(p)}), icon, std::move(sub));
}

}  // namespace pm::ui
