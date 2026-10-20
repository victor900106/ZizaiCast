// Internal to pm_android: timing knobs (tests shorten them), the clipboard
// backend factory (tests use an in-memory clipboard, never the owner's), and
// the pure helpers behind AndroidSource that are unit-tested on their own.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "clipboard.h"

namespace pm::android_internal {

struct Tuning {
    // Stalled stream (scrcpy repeats the last frame every 100 ms, so silence
    // means a hung encoder / server / tunnel): RESET_VIDEO first, then a
    // restart of scrcpy-server (adb still up) or a reconnect (adb gone).
    int monitorMs = 500;
    int stallResetMs = 1500;
    int stallMs = 3000;        // < the app's own 6 s "phone gone" watchdog
    int firstFrameMs = 12000;  // first video packet after the sockets are up
    // In-place restarts of scrcpy-server (same sinks, the window keeps its picture).
    int restartAttempts = 3;
    int restartBaseMs = 500, restartCapMs = 4000;
    int restartForgetMs = 60000;  // a session that ran this long resets the count
    // Reconnecting a phone whose adb transport went away.
    int reconnectAttempts = 8;
    int reconnectBaseMs = 1000, reconnectCapMs = 30000;
    int probeTimeoutMs = 3000;  // `adb shell echo` round trip
    int rediscoverMs = 3000;    // per attempt: mDNS for the phone's new port
    // adb server idle shutdown (no phone, no work for this long; 0 = never).
    int idleKillMs = 15000;
    // USB: how long to wait for 「允許 USB 偵錯？」 on the phone.
    int usbAuthWaitMs = 30000;
    int usbFindMs = 3000;  // no USB device at all for this long: give up
    // pushFile progress poll (remote file size).
    int transferPollMs = 1000;
    // Our own mDNS browser (binds :5353; tests without network turn it off).
    bool mdnsBrowser = true;
    // Where the browser sends its queries: empty = the LAN (224.0.0.251 on
    // every interface); tests use a loopback address so nothing leaves the PC.
    std::vector<std::string> mdnsTargets;
    // connectKnownDevices while the adb server is stopped: our own mDNS looks
    // first and adb is started only if a phone advertises; every n-th such
    // scan is a full one with adb (its own mDNS) anyway.
    int coldScanFullEvery = 5;
    int coldScanMs = 4000;
    // PC clipboard; null = the Windows clipboard.
    std::function<std::unique_ptr<clip::Backend>()> clipboardFactory;
};
// Read by AndroidSource's constructor.
Tuning& tuning();

// scrcpy-server arguments after the version (scid=…, options).
struct ServerOptions {
    std::string scid;            // 8 hex digits
    bool clipboardAutosync = true;
    bool stayAwake = false;      // stay_awake + keep_active (restored by the server's cleanup)
    int maxSize = 1920;
};
std::vector<std::string> serverArgs(const ServerOptions& o);

// What a scrcpy-server log line means for the user (zh-TW), empty if it is
// not an error we explain.  kind: 1 = screen capture / video encoder,
// 2 = audio, 3 = other error.
std::wstring explainServerLine(const std::string& line, int* kind);

// `adb install` output (exit code != 0 or no "Success") → a plain zh-TW sentence.
std::wstring explainInstallFailure(const std::string& adbOutput, int exitCode);
// `adb push` / shell failure output → a plain zh-TW sentence.
std::wstring explainPushFailure(const std::string& adbOutput, int exitCode);

// pushFile's phone-side name: the PC file name with characters Android's
// shared storage refuses (\ / : * ? " < > | and controls) replaced by '_'.
std::string downloadName(const std::wstring& fileName);
// "name (n).ext" for the n-th collision.
std::string numberedName(const std::string& name, int n);
// POSIX shell single-quoting ('…' with ' → '\'').
std::string shellQuote(const std::string& s);

}  // namespace pm::android_internal
