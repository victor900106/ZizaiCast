// PhoneMirror Android source: mirrors + controls an Android phone over Wi-Fi,
// scrcpy-style (Google adb.exe + scrcpy-server v5.0, both downloaded by
// android/third_party/fetch_tools.ps1). Pairing: the PC shows a QR code that
// the phone scans in 開發人員選項 → 無線偵錯 → 使用 QR 圖碼配對裝置 (Android 11+).
//
// Threading: every method may be called from any thread and none blocks for
// long (pairing / connecting / starting run on an internal worker thread).
// Events and `log` are called from internal threads -- marshal to the UI
// thread yourself. Set `events` / `log` before init().
//
// Typical use:
//     pm::AndroidSource src;
//     src.events.onConnected = [&](auto& name) { post(start mirroring) };
//     src.init(exeDir + L"\\android-tools");
//     src.connectKnownDevices();                 // already-paired phones
//     src.beginQrPairing(bgra, size, text);       // show QR; phone scans it
//     ... onConnected → src.start(&videoWindow, &audioPlayer);
//     videoWindow.setPointerHandler([&](auto& e) { src.sendPointer(e); });
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pm/media.h"
#include "pm/video_window.h"

namespace pm {

class AndroidSource {
public:
    enum class State { Idle, WaitingForPairing, Pairing, Connecting, Mirroring, Error };
    struct Events {
        // Every state change; detail is a short user-facing (zh-TW) text.
        std::function<void(State, const std::wstring& detail)> onState;
        // An adb connection to a phone is up (after pairing or reconnect);
        // call start() to begin mirroring it.
        std::function<void(const std::wstring& deviceName)> onConnected;
        // Mirroring ended without stop() (phone disconnected, server died,
        // Wi-Fi lost). The sinks have already been reset/flushed.
        std::function<void()> onDisconnected;
    };

    AndroidSource();
    ~AndroidSource();  // stops mirroring, kills our private adb server
    AndroidSource(const AndroidSource&) = delete;
    AndroidSource& operator=(const AndroidSource&) = delete;

    Events events;

    bool init(const std::wstring& toolsDir);  // dir containing adb.exe + scrcpy-server
    // Pairing QR as BGRA pixels (square, white quiet zone included), plus the
    // raw text; starts listening for the phone (mDNS) in the background.
    // State → WaitingForPairing → Pairing → Connecting → (onConnected).
    bool beginQrPairing(std::vector<uint8_t>& bgra, int& size, std::wstring& qrText);
    // 「使用配對碼配對裝置」: hostPort = the IP:port shown under the 6-digit
    // code. Asynchronous; returns false only for malformed input / not init.
    bool pairWithCode(const std::wstring& hostPort, const std::wstring& code);
    void cancelPairing();
    // Auto-reconnect: browses _adb-tls-connect._tcp for a few seconds and
    // `adb connect`s every phone found (only paired ones succeed).
    // Asynchronous; returns false if not init or a pairing/connect is running.
    bool connectKnownDevices();
    // Starts mirroring the connected device (asynchronous after the checks).
    bool start(VideoSink* video, AudioSink* audio);
    void stop();
    // Input (non-blocking; dropped unless Mirroring).
    void sendPointer(const VideoWindow::PointerEvent& e);
    void sendKey(unsigned vk, bool down, wchar_t ch);
    void pressBack();
    void pressHome();
    void pressAppSwitch();
    State state() const;
    std::function<void(const std::string&)> log;

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

}  // namespace pm
