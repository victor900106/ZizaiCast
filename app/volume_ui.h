// 音量 / 靜音 UI of 自在投影 (see docs/app.md *音量*): the live toolbar's
// speaker button (mute) + compact slider, Ctrl+M / Ctrl+↑ / Ctrl+↓, the 音量
// submenu (tray and right-click menus) and the 「音量 60%」 toasts, over the
// one remembered PC level of RememberVolumeAudioSink (volume_memory.h; the
// 0..100 % scale is pm::vol there).  main.cpp only forwards to it.
// UI thread only (phone changes arrive through a message-only window).
#pragma once
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "pm/video_window.h"
#include "popup_menu.h"

class RememberVolumeAudioSink;

namespace pm::ui {

// Command ids (toolbar, menus, --dev DevCommand scripts), clear of main.cpp's
// Command enum.
enum VolumeCommand : UINT {
    CmdVolMute = 260,  // toggle 靜音 (Ctrl+M, toolbar speaker, menus)
    CmdVolUp,          // one phone step up (Ctrl+↑)
    CmdVolDown,        // one phone step down (Ctrl+↓)
    CmdVolSlider,      // the toolbar slider's id (no command)
    CmdVolSet100 = 270,  // 100 / 75 / 50 / 25 % (menu presets)
    CmdVolSet75,
    CmdVolSet50,
    CmdVolSet25,
};

class VolumeControl {
public:
    struct Hooks {
        std::function<void(const std::wstring&)> toast;  // the video window's toast
        std::function<void()> changed;                    // refresh the live toolbar
        std::function<void(bool muted)> saveMuted;        // settings.ini mute=
        std::function<void(const std::string&)> log;
    };
    // After the sink exists (its mute already set from settings.ini).
    void attach(RememberVolumeAudioSink* sink, Hooks hooks);
    void detach();  // before the sink goes (no more phone notifications)

    // Toolbar / menu / DevCommand ids above; false if not one of them.
    bool runCommand(UINT cmd);
    // Ctrl+<vk> (the caller checked Ctrl, no Alt): VK_UP / VK_DOWN / 'M'.
    bool ctrlKey(WPARAM vk);
    // The toolbar's speaker button + slider (groupStart: divider before).
    void appendToolbar(std::vector<pm::VideoWindow::ToolbarItem>& v, bool groupStart) const;
    // VideoWindow::setLiveToolbarSlider handlers.
    void slide(float fraction, bool done);
    void wheel(int notches);
    // 「音量：60%」 ▸ 靜音 ✓ · 調大聲 · 調小聲 · 100/75/50/25 % · note.
    MenuItem menuItem() const;

    // What the slider shows: 0 while the phone itself is muted.
    float shownFraction() const;
    int shownPercent() const;
    bool muted() const;

private:
    void setLevel(float db, const char* how);
    void setMuted(bool m, const char* how);
    void toastLevel();
    void onPhoneChanged();
    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

    RememberVolumeAudioSink* sink_ = nullptr;
    Hooks hooks_;
    HWND msgWnd_ = nullptr;
    float dragStartDb_ = 0;
    bool dragging_ = false;
};

}  // namespace pm::ui
