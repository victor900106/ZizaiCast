#pragma once
// The PC's sound for the phone: WASAPI loopback (what a playback device
// plays), 48 kHz stereo, encoded with libopus in 20 ms frames (restricted low
// delay, 128 kbps) and handed to the host, which sends them on the WebRTC
// audio track (OpusRtpPacketizer) or the tunnel's WebSocket.
//
// Source (0.8.1; 0.8.0 took the eConsole default device only, and the
// owner's PC plays through another one: peak 0.006 on the phone):
//   ""         automatic (0.8.2): every active playback device is captured and
//              they are MIXED (0.8.1 sent one and jumped to the HDMI noise in
//              the music's pauses); each device has its own FIFO, a frame every
//              20 ms on the wall clock, then a soft limiter.
//   "default"  the Windows default playback device (eConsole), following it
//              when it changes.
//   <id>       that endpoint (IMMDevice id); the default while it is gone.
// Device list / default changes arrive through IMMNotificationClient. Every
// device is logged with its FriendlyName and which default roles it holds.
// Nothing is captured while nothing plays (WASAPI delivers no packets): no
// packets are sent then; the RTP time follows the wall clock across gaps.
//
// mutePc: while running, the device being sent is muted (restored at stop()
// or when another one is picked). If its loopback goes silent the moment it
// is muted (drivers that mute before the loopback tap), the mute is undone.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pm::remote {

struct PlaybackDevice {
    std::string id;    // UTF-8 IMMDevice id
    std::string name;  // FriendlyName, e.g. "喇叭 (Razer Leviathan V2)"
    bool isDefault = false;  // Windows default (eConsole)
    float volume = -1;       // 0.8.3: master volume scalar (0..1; -1 unknown)
    bool hwVolume = false;   // the volume is applied in hardware (else in Windows' mixer, before the loopback)
    bool muted = false;
    int activeSessions = 0;  // programs playing to it right now (AudioSessionStateActive)
};
std::vector<PlaybackDevice> playbackDevicesList();  // active render endpoints

class LoopbackAudio {
public:
    // One Opus packet (20 ms), its RTP time in seconds since start(), and the
    // capture time (nowUs clock) of its first sample.
    using OutputFn = std::function<void(std::vector<uint8_t>&& packet, double rtpSeconds, int64_t captureUs)>;
    ~LoopbackAudio() { stop(); }
    bool start(OutputFn fn, bool mutePc, const std::string& source);
    void stop();
    bool running() const { return running_; }
    void setSource(const std::string& source);  // while running too
    // 手機播聲音時電腦靜音 (0.8.2: while running too): every captured device is muted
    // (endpoint mute) and restored when this goes off or the capture stops.
    void setMute(bool on);
    // 0.8.3 (set before start()): agc = an automatic gain on the mix (0.8.3.2, level.h: towards
    // -17 dBFS RMS, -6..+30 dB, a noise gate keeps the pauses at x1, a steady noise floor is
    // not followed; 0.8.3 / 0.8.3.1 only followed frames above -34 dBFS RMS, up to +12 dB,
    // and stayed at x1 on real quiet material); compensate = a device whose
    // volume Windows applies in software (no hardware volume: the loopback gets the sound
    // after it) is raised by that volume's attenuation (up to +18 dB), so the phone's level
    // does not follow the PC's volume slider.
    void setLevelOptions(bool agc, bool compensate) { agc_ = agc, comp_ = compensate; }
    float agcGain() const { return agcGain_.load(); }    // the AGC's gain now (1 = none)
    float compGain() const { return compGain_.load(); }  // the largest volume compensation now (1 = none)
    // Tests: "music" / "loud" / "quiet" (0.8.3; 0.8.3.2: music-like) / "speech" (0.8.3.2) = a silent
    // synthetic source instead of WASAPI (set before start()).
    void setSynthetic(const std::string& mode) {
        std::lock_guard<std::mutex> lk(mu_);
        synthetic_ = mode;
    }
    // Statistics since the last call: packets, average capture->encoded ms,
    // peak of what was sent; `others` = the other devices' peaks ("name 0.000, ...").
    void takeStats(int& packets, double& avgMs, double& peak, std::string& device, std::string& others);
    // The level of what the phone gets over the last ~1 s (0 = silence / nothing plays).
    float recentPeak() const { return recentPeak_.load(); }
    // 0.8.3.2: RMS (dBFS) of what was sent since the last call (-120 = nothing sent).
    double takeRmsDb() {
        const int64_t s = sentSqPico_.exchange(0);
        const int n = sentFrames_.exchange(0);
        return n && s > 0 ? 10.0 * std::log10(double(s) / 1e12 / n) : -120.0;
    }
    std::string currentDevice() const {
        std::lock_guard<std::mutex> lk(mu_);
        return curName_;
    }

private:
    void loop();
    OutputFn out_;
    std::thread th_;
    std::atomic<bool> running_{false}, stop_{false}, dirty_{false};
    std::atomic<bool> muteWanted_{false}, dirty_mute_{false};
    mutable std::mutex mu_;
    std::string source_, curName_, others_, synthetic_;
    bool statsTaken_ = false;  // guarded by mu_
    std::atomic<int> packets_{0};
    std::atomic<int64_t> latSumUs_{0};
    std::atomic<int> peakMicro_{0};
    std::atomic<float> recentPeak_{0};
    std::atomic<bool> agc_{true}, comp_{true};
    std::atomic<float> agcGain_{1}, compGain_{1};
    std::atomic<int64_t> sentSqPico_{0};  // 0.8.3.2: the sent frames' mean squares x 1e12
    std::atomic<int> sentFrames_{0};
};

}  // namespace pm::remote
