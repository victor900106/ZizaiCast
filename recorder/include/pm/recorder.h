// PhoneMirror screen recorder: writes the mirrored picture + the decoded audio
// to an MP4 (H.264 via Media Foundation, hardware encoder when available;
// AAC-LC stereo). See docs/recorder.md.
//
// Threading: onVideoFrame / onPcm copy the data into bounded queues and return
// (never wait on the encoder; video frames that do not fit are dropped and
// counted). All encoding and file I/O happens on the recorder's own thread.
// start/stop/active/stats may be called from any thread.
//
// Timestamps (ptsNs / whenNs) may be on std::chrono::steady_clock (QPC) ns or
// on the UTC wall clock (Unix-epoch ns); each stamp is matched to the closer
// clock. 0 (or a stamp > 30 s from now on both clocks) = "now".
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace pm {

class Recorder {
public:
    struct Stats {
        double seconds = 0;
        uint64_t videoFrames = 0, audioFrames = 0, droppedVideo = 0;
        uint64_t bytes = 0;
    };
    Recorder();
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Starts writing an MP4 (H.264 video via Media Foundation hardware encoder
    // when available, AAC-LC 44.1/48 kHz stereo audio). Returns false if the
    // file cannot be created (or a recording is already active).
    bool start(const std::wstring& mp4Path, int fps = 60);
    void stop();  // finalizes the file; safe to call twice; returns after the file is closed
    bool active() const;
    Stats stats() const;

    // Feed from VideoWindow::setFrameTap (NV12 8-bit, cropped) and
    // AudioPlayer::setPcmMonitor. Cheap (one copy into a queue); safe to call
    // when not recording (ignored).
    void onVideoFrame(const uint8_t* nv12, int width, int height, int stride, uint64_t ptsNs);
    void onPcm(const int16_t* pcm, size_t frames, int channels, int sampleRate, uint64_t whenNs);

    // Diagnostic lines (encoder chosen, canvas, errors, final summary). Called
    // from the recorder thread. Set before start(). Empty = OutputDebugString.
    std::function<void(const std::string&)> log;

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

}  // namespace pm
