// 自在投影 Miracast receiver: Android phones (Samsung Smart View, Xiaomi /
// OPPO / vivo 投放, Windows 「投影」…) cast to this PC over Wi-Fi Direct and
// the picture is shown in our own pm::VideoWindow.  Built on the Windows
// Runtime API Windows.Media.Miracast (Windows 10 2004+); Windows does the
// Wi-Fi Direct / RTSP / HDCP / decoding, we render the decoded frames.
// See docs/miracast.md (requirements, threading, test procedure).
//
// Threading: start/stop/disconnect from any thread (normally the UI thread).
// Events and `log` are called on Windows thread-pool threads, never while a
// lock of this class is held; marshal to the UI thread yourself.
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace pm {

class VideoWindow;

class MiracastReceiver {
public:
    enum class Status {
        Unavailable,  // this PC cannot receive Miracast (see unsupportedReason())
        Disabled,     // supported, but not listening (policy, another receiver, Wi-Fi off …)
        Idle,         // listening: visible to phones as friendlyName
        Connecting,   // a phone is connecting (PIN / negotiation)
        Connected     // picture is streaming
    };
    struct Events {
        std::function<void(Status, const std::wstring& detail)> onStatus;
        std::function<void(const std::wstring& deviceName)> onConnected;
        std::function<void()> onDisconnected;
        // PIN to show while the phone asks for it; empty string = hide it.
        std::function<void(const std::wstring& pin)> onPin;
    };

    MiracastReceiver();
    ~MiracastReceiver();  // stop()
    MiracastReceiver(const MiracastReceiver&) = delete;
    MiracastReceiver& operator=(const MiracastReceiver&) = delete;

    // Applies the receiver settings (name shown on the phones, no PIN unless
    // the phone asks for one) and starts listening.  Decoded frames go to
    // window->submitBgraFrame(), window->onReset() when a cast ends.
    // Returns false + onStatus(Unavailable or Disabled, reason) on failure.
    bool start(const std::wstring& friendlyName, VideoWindow* window, Events events);
    void stop();
    // Ends the current cast (the receiver keeps listening).
    void disconnect();
    Status status() const;

    // Playback volume of the cast's audio (played by Windows' MediaPlayer on
    // the default output device), 0..1; applies to current and later casts.
    void setVolume(double volume01);

    // Empty if this PC can receive Miracast, else a user-facing (zh-TW)
    // explanation of what is missing.  Takes ~10-50 ms (WinRT status query).
    static std::wstring unsupportedReason();

    std::function<void(const std::string&)> log;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

}  // namespace pm
