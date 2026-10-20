// One scrcpy mirroring session over an `adb forward` tunnel (tunnel_forward=
// true): connects the video, audio and control sockets on 127.0.0.1:<port>,
// reads the dummy byte + 64-byte device name, then runs reader threads that
// feed StreamParser → VideoSink / AudioSink, and a writer thread for control
// messages. Independent of adb (tests drive it against a fake server).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pm/media.h"

namespace pm::scrcpy {

class Session {
public:
    struct Config {
        std::string host = "127.0.0.1";
        uint16_t port = 0;
        bool audio = true;
        bool control = true;
        // Connecting while the server starts: exponential backoff from
        // delayMs up to maxDelayMs, at most `attempts` tries / timeoutMs.
        int attempts = 100;
        int delayMs = 20;
        int maxDelayMs = 400;
        int timeoutMs = 15000;
    };
    // Why the session ended by itself (onEnded).
    enum class End { None, Eof, StreamError, CaptureError };
    ~Session();
    // Blocking handshake. alive (optional): abort when it returns false.
    bool connect(const Config& cfg, const std::function<bool()>& alive, std::string* err);
    // Starts the threads. Sinks must outlive stop().
    void run(VideoSink* video, AudioSink* audio);
    // Idempotent; joins all threads. Must not be called from a callback.
    void stop();
    const std::string& deviceName() const { return deviceName_; }
    void sendControl(std::vector<uint8_t> msg);
    // Waits until every queued control message has been written (a graceful
    // stop: "screen back on" must reach the phone before the sockets close).
    bool drainControl(int timeoutMs);

    std::function<void()> onEnded;                 // video stream ended by the peer (once)
    std::function<void(int, int)> onVideoSize;     // session packet (reader thread)
    std::function<void(const std::string&)> log;
    // Device messages (control reader thread): the phone's clipboard changed
    // (clipboard_autosync / GET_CLIPBOARD answer), SET_CLIPBOARD acknowledged.
    std::function<void(const std::string& utf8)> onClipboard;
    std::function<void(uint64_t sequence)> onAckClipboard;
    // Audio stream not available: error = capture/encoder failure on the
    // device, else disabled (Android < 11).  Audio reader thread, once.
    std::function<void(bool error)> onAudioUnavailable;
    End endReason() const { return End(end_.load()); }
    // steady_clock milliseconds of the last media packet (0 = none yet).
    int64_t lastVideoMs() const { return lastVideoMs_.load(); }
    int64_t lastAudioMs() const { return lastAudioMs_.load(); }
    int64_t startedMs() const { return startedMs_.load(); }
    static int64_t nowMs();

    // Stats (for tests / diagnostics).
    std::atomic<uint64_t> videoPackets{0}, audioPackets{0}, controlSent{0};

private:
    void videoLoop(VideoSink* sink);
    void audioLoop(AudioSink* sink);
    void controlWriter();
    void controlReader();
    uintptr_t video_ = ~uintptr_t(0), audio_ = ~uintptr_t(0), control_ = ~uintptr_t(0);
    std::string deviceName_;
    std::atomic<bool> stopping_{false};
    std::atomic<int> end_{0};
    std::atomic<int64_t> lastVideoMs_{0}, lastAudioMs_{0}, startedMs_{0};
    std::thread tv_, ta_, tcw_, tcr_;
    std::mutex qmu_;
    std::condition_variable qcv_;
    std::deque<std::vector<uint8_t>> queue_;
    std::condition_variable drained_;
    bool writing_ = false;  // under qmu_: a message is being written
};

}  // namespace pm::scrcpy
