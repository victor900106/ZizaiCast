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
        int attempts = 100;   // connection attempts (server still starting)
        int delayMs = 100;
    };
    ~Session();
    // Blocking handshake. alive (optional): abort when it returns false.
    bool connect(const Config& cfg, const std::function<bool()>& alive, std::string* err);
    // Starts the threads. Sinks must outlive stop().
    void run(VideoSink* video, AudioSink* audio);
    // Idempotent; joins all threads. Must not be called from a callback.
    void stop();
    const std::string& deviceName() const { return deviceName_; }
    void sendControl(std::vector<uint8_t> msg);

    std::function<void()> onEnded;                 // video stream ended by the peer (once)
    std::function<void(int, int)> onVideoSize;     // session packet (reader thread)
    std::function<void(const std::string&)> log;

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
    std::thread tv_, ta_, tcw_, tcr_;
    std::mutex qmu_;
    std::condition_variable qcv_;
    std::deque<std::vector<uint8_t>> queue_;
};

}  // namespace pm::scrcpy
