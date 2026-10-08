// PhoneMirror audio front end: decodes AirPlay audio (AAC-ELD / AAC-LC / ALAC)
// and plays it through WASAPI shared mode with a small bounded jitter buffer.
//
// Threading: every pm::AudioSink method may be called from any thread (the
// network thread in practice). onPacket decodes inline (a few tens of
// microseconds per frame) and pushes PCM into a ring buffer; it never waits on
// the audio device. Playback runs on a dedicated MMCSS thread started by
// start().
#pragma once

#include <pm/media.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace pm {

struct AudioStats {
    double bufferMs = 0;        // PCM currently queued in the jitter buffer
    double deviceBufferMs = 0;  // WASAPI endpoint buffer size (0 if no device)
    uint64_t packets = 0;       // compressed packets received
    uint64_t decodeErrors = 0;  // packets the decoder rejected
    uint64_t framesDecoded = 0; // PCM sample frames produced by the decoder
    uint64_t underruns = 0;     // times playback ran dry while playing
    uint64_t droppedFrames = 0; // PCM sample frames discarded on overrun
    uint64_t deviceReopens = 0; // WASAPI (re)initialisations
    bool deviceOpen = false;
    // Timing of the most recent stream (re)start = first packet after
    // onFormat / onFlush / the buffer running dry:
    //   startGapMs:   onFormat/onFlush -> that first packet (-1 after an underrun)
    //   startDelayMs: that first packet -> first decoded PCM handed to the device
    double startGapMs = -1;
    double startDelayMs = -1;
    uint64_t starts = 0;        // number of (re)starts measured
};

struct AudioPlayerConfig {
    // Playback starts (and restarts after an underrun/flush) once this much
    // PCM is queued. Total latency ~= prefillMs + device buffer (~20 ms).
    int prefillMs = 30;
    // Hard cap on the jitter buffer; on overflow the oldest PCM is dropped.
    int maxBufferMs = 120;
    // Requested WASAPI buffer duration (shared mode, event driven).
    int deviceBufferMs = 20;
    // If set, no audio device is used: a real-time paced pull clock drives the
    // same jitter buffer and every rendered 16-bit PCM block (post volume) is
    // passed here (interleaved, at the stream rate). Used by tests / --wav.
    std::function<void(const int16_t* pcm, size_t frames, int channels, int sampleRate)> pcmTap;
    // Diagnostic log lines (device open/close, format changes, stream start
    // timing in ms). Called from the network or the render thread; keep it
    // cheap. Empty = stderr + OutputDebugString.
    std::function<void(const std::string& line)> log;
};

class AudioPlayer : public AudioSink {
public:
    explicit AudioPlayer(AudioPlayerConfig cfg = {});
    ~AudioPlayer() override;
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // Starts the playback thread. Returns false only on fatal setup failure;
    // a missing audio device is retried in the background.
    bool start();
    void stop();

    AudioStats stats() const;
    // Linear gain currently applied (derived from the last onVolume).
    float gain() const;

    // AirPlay dB -> linear gain: -144 (or below -30) -> 0, else 10^(dB/20)
    // with dB clamped to [-30, 0].
    static float airplayDbToGain(float airplayDb);

    // Monitor of the decoded stream, e.g. for recording. Unlike
    // AudioPlayerConfig::pcmTap this does not replace the device: every
    // decoded block is passed here AND played normally. The PCM is the raw
    // decoder output (interleaved S16 at the stream rate, *before* the AirPlay
    // volume is applied), one call per decoded packet, on the thread calling
    // onPacket (the network thread). whenNs = the packet's ntpLocalNs if it is
    // non-zero and within 30 s of now on either std::chrono::steady_clock
    // (QPC) or the UTC wall clock (Unix-epoch ns) -- passed through on
    // whatever clock the core uses -- else the arrival time on steady_clock
    // ns. Thread-safe; nullptr removes. Once setPcmMonitor returns, no call to
    // the previous monitor is in flight. The callback must be quick and must
    // not call back into this AudioPlayer. Zero cost when unset.
    using PcmMonitor = std::function<void(const int16_t* pcm, size_t frames, int channels, int sampleRate,
                                          uint64_t whenNs)>;
    void setPcmMonitor(PcmMonitor fn);

    // pm::AudioSink
    void onFormat(AudioCodec codec, int sampleRate, int channels, int samplesPerFrame) override;
    void onPacket(const uint8_t* data, size_t len, uint64_t ntpLocalNs) override;
    void onVolume(float airplayDb) override;
    void onFlush() override;

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

}  // namespace pm
