// Internal: plays a Windows.Media.Core.MediaSource through a
// Windows.Media.Playback.MediaPlayer in frame-server mode and pushes every
// decoded picture as BGRA into pm::VideoWindow::submitBgraFrame().
//
//   VideoFrameAvailable (MF thread)
//     -> CopyFrameToVideoSurface into our B8G8R8A8 render target (own device)
//     -> CopyResource into a free slot of a 3-deep staging ring + event query
//   reader thread
//     -> waits for the newest finished slot (older finished ones are skipped)
//     -> Map, optional black-bar crop, submitBgraFrame, Unmap
// The MF thread never waits for the GPU; if all slots are busy the picture is
// dropped (counted).  Audio is played by the MediaPlayer itself.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace winrt::Windows::Media::Core {
struct MediaSource;
}

namespace pm {

class VideoWindow;

class FramePump {
public:
    using Log = std::function<void(const std::string&)>;

    struct Stats {
        long long framesAvailable = 0;  // VideoFrameAvailable events
        long long framesCopied = 0;     // copied into a staging slot
        long long framesDelivered = 0;  // passed to submitBgraFrame
        long long framesDropped = 0;    // ring full, or superseded before read-back
        long long copyErrors = 0;       // CopyFrameToVideoSurface failures
        int width = 0, height = 0;      // decoded size
        int cropX = 0, cropY = 0, cropW = 0, cropH = 0;  // delivered rectangle
        double copyAvgMs = 0;           // MF-thread cost per frame
        double readbackAvgMs = 0;       // copy submitted -> data mapped
    };

    FramePump(VideoWindow* window, Log log);
    ~FramePump();  // stop()
    FramePump(const FramePump&) = delete;
    FramePump& operator=(const FramePump&) = delete;

    // Creates the D3D11 device and the reader thread.  False if D3D11 fails.
    bool init();

    // Starts playing `source` (replaces the current one).  realTime = low
    // latency mode for live sources (Miracast).
    bool play(const winrt::Windows::Media::Core::MediaSource& source, bool realTime);
    // Stops playback, waits for in-flight pictures, then window->onReset()
    // (only if something was shown).
    void stop();

    void setVolume(double volume01);
    // Crop symmetric black pillar/letterbox bars (Android phones send a
    // portrait screen inside a 16:9 picture).  Default on.
    void setAutoCrop(bool enabled);

    Stats stats() const;

    // Called from MediaPlayer threads.
    std::function<void()> onEnded;
    std::function<void(const std::wstring& error)> onFailed;
    std::function<void(int width, int height)> onFirstFrame;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

}  // namespace pm
