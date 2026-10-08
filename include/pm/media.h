// Shared interfaces between the AirPlay protocol core and the Windows
// video/audio front ends. Kept tiny on purpose: each module only depends on
// this header, never on another module's internals.
#pragma once

#include <cstddef>
#include <cstdint>

namespace pm {

enum class VideoCodec { H264, H265 };

// Receives the mirrored screen as Annex-B elementary stream access units
// (start-code prefixed NAL units; SPS/PPS(/VPS) arrive in-band before IDR).
// All methods may be called from the network thread; implementations must be
// thread-safe and must not block for long.
class VideoSink {
public:
    virtual ~VideoSink() = default;
    virtual void onCodec(VideoCodec codec) = 0;
    // ntpLocalNs: local presentation time in nanoseconds (0 = show ASAP).
    virtual void onFrame(const uint8_t* annexB, size_t len, uint64_t ntpLocalNs) = 0;
    // Source dimensions changed (rotation etc.); purely informational.
    virtual void onSourceSize(int width, int height) = 0;
    // Stream ended or connection reset: drop queued frames, show idle screen.
    virtual void onReset() = 0;
    // The phone suspended the stream (e.g. its screen turned off) while staying
    // connected; frames resume after onPaused(false).
    virtual void onPaused(bool /*paused*/) {}
};

// AirPlay audio compression types ("ct" in the RAOP SETUP).
enum class AudioCodec { ALAC = 2, AAC_LC = 4, AAC_ELD = 8 };

class AudioSink {
public:
    virtual ~AudioSink() = default;
    // samplesPerFrame: 480 for AAC-ELD mirroring, 1024 for AAC-LC, 352 for ALAC.
    virtual void onFormat(AudioCodec codec, int sampleRate, int channels, int samplesPerFrame) = 0;
    // One decrypted compressed audio frame (raw AAC access unit, no ADTS).
    virtual void onPacket(const uint8_t* data, size_t len, uint64_t ntpLocalNs) = 0;
    // AirPlay volume in dB: 0.0 = full, -30.0 = quietest, -144.0 = mute.
    virtual void onVolume(float airplayDb) = 0;
    virtual void onFlush() = 0;
};

}  // namespace pm
