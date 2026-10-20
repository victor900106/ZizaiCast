// PhoneMirror Android source: mirrors + controls an Android phone over Wi-Fi
// (or a USB cable, connectUsb()), scrcpy-style (Google adb.exe + scrcpy-server v5.0, both downloaded by
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
        // Wi-Fi lost). The sinks have already been reset/flushed.  Unless
        // stop() is called, a reconnect with exponential backoff follows
        // (State Connecting 「手機連線中斷，正在重新連線（第 n 次）…」); when
        // it succeeds onConnected fires again (call start() as usual), else
        // Idle 「手機連線中斷，重新連線失敗…」.  A stalled stream (no video for
        // ~3 s) or a crashed scrcpy-server while adb is still up is restarted
        // in place instead, without any event (same sinks).
        std::function<void()> onDisconnected;
        // 0.8.3: something the user should know that does not end mirroring
        // (zh-TW sentence), e.g. 「這支手機的 Android 版本低於 11，投影沒有聲音」.
        // Internal thread, like the other events.
        std::function<void(const std::wstring& message)> onNotice;
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
    // The pending QR / code pairing: queued, listening or pairing (not once
    // `adb pair` succeeded; the connect then finishes).  Safe to call always.
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

    // 傳到手機: copies a screenshot / recording into the connected phone's
    // gallery -- `adb push` to /sdcard/Pictures/ZizaiCast/ (pictures) or
    // /sdcard/Movies/ZizaiCast/ (videos; by extension), file name made ASCII
    // (自在投影_… → ZizaiCast_…), then a media scan (MediaProvider scan_file,
    // the MEDIA_SCANNER_SCAN_FILE broadcast, and scan_volume if the file is
    // still not in MediaStore) so it shows in Photos / Gallery at once.
    // Asynchronous on its own thread (works while mirroring); `done` is called
    // from that thread. Returns false (and never calls done) if not init, no
    // phone is connected over adb, the file is missing, or a push is running.
    struct PushResult {
        bool ok = false;         // the file is on the phone
        bool inGallery = false;  // ... and MediaStore lists it (seen in the gallery)
        std::string remotePath;  // e.g. /sdcard/Pictures/ZizaiCast/ZizaiCast_20261008_101010.png
        std::string error;       // adb's output when !ok
    };
    bool pushToGallery(const std::wstring& path, std::function<void(const PushResult&)> done);
    // Pure: the phone-side path pushToGallery uses for `fileName` (tests).
    static std::string galleryPath(const std::wstring& fileName);

    // 0.8.3 features
    //
    // Threads: the setters return at once (any thread).  pushFile /
    // installApk run one at a time, in call order, on one internal transfer
    // thread; `done` is called exactly once on that thread -- never inside
    // the pushFile / installApk call itself, never while a lock of this class
    // is held -- so it may PostMessage to the UI thread right away (do not
    // touch UI objects from it directly).  `msg` is a user-facing zh-TW
    // sentence for success and failure alike.  Pending transfers are
    // cancelled by the destructor (done(false, 「已取消」)).
    //
    // Clipboard sync (default on): the phone's clipboard changes reach the PC
    // clipboard at once (server clipboard_autosync + device CLIPBOARD
    // messages); the PC clipboard reaches the phone (SET_CLIPBOARD) only once
    // the user controls the phone (a click / key since start()) -- while
    // only watching nothing is sent.  Ctrl+V on the mirror = SET_CLIPBOARD
    // with paste; Ctrl+C / Ctrl+X = GET_CLIPBOARD with COPY / CUT.
    void setClipboardSync(bool on);                 // 電腦↔手機剪貼簿雙向同步（scrcpy SET_CLIPBOARD／GET_CLIPBOARD 加裝置剪貼簿訊息；預設開，僅觀看時不送出）
    // adb push to /sdcard/Download/<file name> ("name (1).ext" if taken),
    // then a media scan; progress (bytes on the phone) goes to `log`.
    void pushFile(const std::wstring& path, std::function<void(bool ok, std::wstring msg)> done); // adb push 到 /sdcard/Download/，非同步，有進度記錄
    // adb install -r; INSTALL_FAILED_… codes become plain sentences.
    void installApk(const std::wstring& path, std::function<void(bool ok, std::wstring msg)> done); // adb install -r，失敗訊息轉成白話
    // Off: SET_DISPLAY_POWER(false) while mirroring (the picture continues);
    // remembered for later sessions.  On stop() the panel is switched back on
    // before the sockets close; if the connection is lost, scrcpy-server's
    // cleanup process on the phone switches it on.
    void setPhoneScreenOn(bool on);                 // 操控時關手機螢幕（scrcpy SET_DISPLAY_POWER），斷線時一定恢復亮
    // scrcpy stay_awake (charging) + keep_active (wireless): the phone does
    // not sleep while mirroring; scrcpy-server's cleanup restores the
    // previous stay-on setting when mirroring ends or the connection drops.
    // While mirroring the change restarts scrcpy-server in place (~2 s).
    void setStayAwake(bool on);                     // 連線期間不休眠，斷線還原原本設定
    // A phone on a USB cable (`adb devices`, serial without ':'), USB
    // debugging on.  Same flow as the Wi-Fi paths: Connecting → onConnected
    // → start(); waits up to 30 s for 「允許 USB 偵錯？」 on the phone.
    // Asynchronous; false if not init or a pairing / connect is running.
    bool connectUsb();                              // 用 USB 線連（adb devices 裡的 usb 裝置），和無線共用同一套流程

    std::function<void(const std::string&)> log;

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

}  // namespace pm
