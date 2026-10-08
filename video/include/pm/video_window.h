// PhoneMirror video front end: a Win32 window that decodes and shows the
// mirrored iPhone screen. Windows-native only (Media Foundation + D3D11).
//
// Usage (UI thread):
//     pm::VideoWindow win;
//     if (!win.create(L"PhoneMirror")) return 1;
//     core.setVideoSink(&win);          // any thread may call the VideoSink API
//     win.runMessageLoop();             // returns when the window is closed
//
// Threading:
//   * create(), runMessageLoop() must be called on the same thread (the
//     window's owner thread).  The decoder + renderer run on an internal
//     worker thread started by create().
//   * The VideoSink methods and close()/stats() are thread-safe and never
//     block for long: onFrame() copies the access unit into a bounded queue.
//
// Window controls: double-click or F11 toggles borderless fullscreen, Esc
// leaves fullscreen.  The picture keeps its aspect ratio (letterboxed).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pm/media.h"

struct HWND__;

namespace pm {

class VideoWindow final : public VideoSink {
public:
    struct Stats {
        bool hardwareDecode = false;   // DXVA (D3D11) decoder in use
        long long framesIn = 0;        // access units received via onFrame
        long long framesDecoded = 0;   // pictures produced by the decoder
        long long framesPresented = 0; // pictures actually shown
        long long framesDropped = 0;   // AUs dropped before decode (queue full)
        int width = 0, height = 0;     // current decoded picture size (cropped)
        // Decode latency: AU submitted to MFT -> decoded picture available.
        double decodeAvgMs = 0, decodeP95Ms = 0;
        // End to end: onFrame() call -> Present() returned with that picture
        // (includes the intentional A/V sync delay when sync is on).
        double e2eAvgMs = 0, e2eP95Ms = 0;
        // A/V sync: pictures presented at a scheduled time, and Present()
        // return time minus the target (signed avg, p95 of |error|).
        long long syncPresented = 0;
        double syncErrAvgMs = 0, syncErrAbsP95Ms = 0;
        // Device losses (TDR, driver update, GPU switch) recovered from.
        long long deviceRecoveries = 0;
        std::wstring adapter;  // GPU in use
        // Frame tap: pictures delivered, and render-thread time per picture
        // (GPU convert + copy submission, map, NV12 assembly; excl. the tap).
        long long framesTapped = 0;
        double tapAvgMs = 0, tapP95Ms = 0;
    };

    VideoWindow();
    ~VideoWindow() override;
    VideoWindow(const VideoWindow&) = delete;
    VideoWindow& operator=(const VideoWindow&) = delete;

    // Creates and shows the window and starts the decode/render thread.
    // Returns false (with a log line on stderr) if D3D11/MF are unavailable.
    bool create(const wchar_t* title, int clientWidth = 1280, int clientHeight = 720);

    // Pumps Win32 messages until the window is closed.  Returns the exit code.
    int runMessageLoop();

    // Thread-safe: asks the window to close (runMessageLoop() then returns).
    void close();

    // Native handle (nullptr before create / after close).
    HWND__* hwnd() const;

    // Thread-safe snapshot of decoder/renderer statistics.
    Stats stats() const;

    // A/V sync (thread-safe, cheap; call again whenever the latency changes,
    // e.g. every second).  Disabled (default): every picture is shown as soon
    // as it is decoded (lowest latency).  Enabled: a picture is shown at
    // ntpLocalNs + audioLatencyMs (ntpLocalNs on AirPlayServer::localTimeNs()'s
    // clock); decoded pictures wait in a small queue (at most 500 ms), late
    // ones are shown immediately, and frames with ntpLocalNs == 0 are shown
    // ASAP.  audioLatencyMs: audio buffer + device latency (clamped 0..5000).
    void setSyncMode(bool enabled, int audioLatencyMs);

    // ---- Status / interaction UI (all thread-safe unless noted) ----

    // A phone started connecting; shows "正在連線「name」…" with an animation
    // until the first frame fades in (or onReset() returns to the idle screen).
    void setConnecting(const std::wstring& deviceName);

    // Check boxes drawn on the idle screen (e.g. 開機自動啟動, 連線需要 PIN 碼).
    // Clicking one flips it, redraws, and calls onToggle(id, checked) on the
    // UI thread. Call again to replace the list.
    struct IdleOption {
        int id = 0;
        std::wstring label;
        bool checked = false;
    };
    void setIdleOptions(std::vector<IdleOption> options, std::function<void(int id, bool checked)> onToggle);

    // Large PIN overlay shown while a phone is waiting for the code; empty
    // string hides it.
    void showPin(const std::wstring& pin);

    // Short message at the bottom of the window that fades out after ~2.5 s
    // (holdMs: fully visible that long instead, for longer hints).
    void showToast(const std::wstring& text);
    void showToast(const std::wstring& text, int holdMs);

    // ---- Live toolbar ----
    // A themed rounded pill of icon buttons at the top centre of the live
    // picture (below the Dynamic Island with the device frame, below the REC
    // badge if they would touch; over the window while paused).  It fades in
    // whenever the mouse moves over the window while a picture is live and
    // fades out ~2 s after the last movement (it stays while the cursor is on
    // it, and goes at once when the cursor leaves the window).  Hidden, it
    // draws nothing and takes no clicks.  Buttons: hover disc (danger: red
    // disc, white icon), tooltip under the hovered button after ~0.45 s,
    // toggled = red disc + pulsing dot (e.g. recording).  Shrinks (down to
    // 26 DIP buttons) to fit a narrow picture.
    //   * Clicks on the pill never reach the pointer handler (the phone):
    //     left click on a button calls onClick(id) on the UI thread (on
    //     release over the same button); middle / wheel are swallowed; a right
    //     click opens the window's context menu (WM_CONTEXTMENU), even on an
    //     Android picture.  Hover moves over it are not forwarded either.
    //   * glyph: a Segoe Fluent Icons / Segoe MDL2 Assets code point.
    //   * groupStart: a thin divider before this button.
    // Call again to update (e.g. toggled); an empty list removes it.
    struct ToolbarItem {
        int id = 0;
        wchar_t glyph = 0;
        std::wstring tooltip;
        bool toggled = false;
        bool danger = false;
        bool groupStart = false;
    };
    void setLiveToolbar(std::vector<ToolbarItem> items, std::function<void(int id)> onClick);

    // Saves the most recent decoded picture (cropped, no letterbox, with the
    // current rotation / mirroring) as a 24-bit PNG.  Returns false if nothing
    // has been shown yet.
    bool saveSnapshot(const std::wstring& pngPath);
    // Same picture inside the iPhone-style device frame (rounded screen
    // corners, bezel, Dynamic Island) on a transparent background (32-bit
    // PNG), whether or not setDeviceFrame() is on.
    bool saveSnapshotFramed(const std::wstring& pngPath);
    // Test / docs hook: draws the window right now (status screen, mascot,
    // overlays or picture, exactly as shown) and saves the client area as a
    // 24-bit PNG, from the back buffer, so it also works off-screen or
    // covered.  False if nothing could be drawn (minimized, device lost).
    bool saveWindowShot(const std::wstring& pngPath);

    // ---- Presentation of the live picture (thread-safe, cheap) ----

    // Darkens the live picture (~55 % black, 200 ms ease); nothing else
    // changes.  Cleared by setDimmed(false), onReset() or the next decoded
    // picture (used during the app's 3 s "連線中斷" hold).
    void setDimmed(bool dimmed);
    // Rotates the displayed picture clockwise by 0..3 quarter turns (other
    // values wrap) and/or mirrors it horizontally (applied after the
    // rotation, i.e. left/right as seen in the window).  The letterbox
    // follows; snapshots honour both.
    void setRotation(int quarterTurnsClockwise);
    void setMirrored(bool horizontal);
    // Size of the displayed picture after rotation (cropped decoded size,
    // width/height swapped for 90 / 270 degrees); 0 x 0 before the first
    // picture.  Use it as the aspect ratio when sizing the window.
    void desiredClientAspect(int& w, int& h) const;
    // iPhone-style bezel around the picture (off by default).
    void setDeviceFrame(bool enabled);

    // ---- Theme ----
    enum class Theme { Sakura, Mint, Night, MilkTea };
    // Colours of every UI element (idle background, particles, check
    // boxes, toasts, PIN card, spinner, REC badge) and the mascot's accent parts
    // (phone screen, rim light, beam, hearts); its cloud stays white.
    void setTheme(Theme t);
    // { background, card, accent } as 0xRRGGBB, e.g. for matching menus.
    static std::array<uint32_t, 3> themeSwatch(Theme t);

    // ---- Recording ----
    // Pulsing red dot + elapsed mm:ss (counted from activation) at the
    // top-left of the picture while active.
    void setRecording(bool active);

    // Decoded-frame tap (for the recorder).  Every newly decoded picture,
    // cropped to the visible size (rounded down to even width/height), as
    // 8-bit NV12: Y plane (height rows) followed by the interleaved UV plane
    // (height/2 rows), both `stride` bytes per row.  Read back through a
    // staging-texture ring and delivered on the render thread one picture
    // behind decoding (the newest one at the latest ~20 ms later).  The
    // buffer is only valid during the call.  ptsNs = the frame's ntpLocalNs
    // if plausible, else its onFrame() arrival time, on
    // pm::AirPlayServer::localTimeNs()'s clock.  nullptr removes the tap;
    // once setFrameTap() returns the previous tap is no longer called (it
    // may be called from inside the tap).  No cost while unset.  Keep the tap
    // short (copy and hand off): it runs on the render thread.
    using FrameTap = std::function<void(const uint8_t* nv12, int width, int height, int stride, uint64_t ptsNs)>;
    void setFrameTap(FrameTap tap);

    // ---- Android sources (declared by the integrator; implemented in video/) ----

    // Already-decoded pictures from sources that do their own decoding
    // (Miracast via Windows.Media.Miracast). BGRA 8-bit; shown like a decoded
    // stream picture (letterbox, rotation, frame, dim, snapshot, frame tap).
    // Call onReset() when the source ends. Thread-safe, non-blocking.
    // The rows are copied before returning (~1 ms for 1920x1080); only the
    // newest unshown picture is kept (a replaced one counts as framesDropped).
    // Always shown ASAP (no A/V sync); ptsNs is only used for the frame tap
    // (same plausibility rule as onFrame's ntpLocalNs).  stride >= width * 4.
    void submitBgraFrame(const uint8_t* bgra, int width, int height, int stride, uint64_t ptsNs);

    // Remote control of the phone (scrcpy): mouse/touch and keyboard input on
    // the picture. Coordinates are normalised 0..1 in the phone's own picture
    // space (rotation/mirroring/letterbox already undone). Handlers run on the
    // UI thread; while a pointer handler is set, clicks on the picture go to it
    // (double-click fullscreen still works outside the picture).
    //   * Down on the picture captures the mouse; Move / Up of that drag are
    //     delivered even outside the picture (clamped to its edge).  Lost
    //     capture / picture gone mid-drag: Up at the last position.
    //   * Move with button -1 = hover (no button down), over the picture
    //     only, at most ~60 Hz (the last position is delivered).
    //   * Wheel: wheelY / wheelX in notches (120 = 1; fractions from
    //     precision touchpads), +wheelY = up (away from the user), +wheelX =
    //     right.  Presses / wheel outside the picture are not delivered.
    //   * Shift + right click keeps the window's context menu (so does a right
    //     click on the live toolbar; nothing on the toolbar is delivered).
    //   * A small ring cursor shows over the picture while a handler is set.
    struct PointerEvent {
        enum class Kind { Down, Move, Up, Wheel } kind = Kind::Move;
        float x = 0, y = 0;
        int button = 0;      // 0 left, 1 right, 2 middle (-1: hover Move)
        float wheelX = 0, wheelY = 0;
    };
    void setPointerHandler(std::function<void(const PointerEvent&)> handler);
    // vk = Win32 virtual key; ch = translated character for down events (0 if none).
    // Forwarded only without Ctrl / Alt (the app's shortcuts), except
    // Ctrl+C / V / X / A / Z; Ctrl / Alt / Win themselves, F11 and Esc in
    // fullscreen stay with the window.  Modifier state: GetKeyState() inside
    // the handler.  ch is printable only (control characters: 0, the vk says
    // it; 0 with Ctrl).  Text without a key (IME result, dead-key / AltGr
    // character) comes as vk 0, down, ch.  Every forwarded down gets its up
    // (also on focus loss).
    void setKeyHandler(std::function<void(unsigned vk, bool down, wchar_t ch)> handler);

    // Clickable actions on the idle screen, in one centred row under the
    // hints / check boxes (wrapping to more rows when the window is narrow).
    // primary: a pill button (accent outline, filled on hover); otherwise an
    // underlined link. Hand cursor over both; clicking calls onClick on the
    // UI thread. Only drawn on the fully shown idle screen (not while
    // connecting / live / paused). Call again to replace; empty list (or
    // empty labels) hides them.
    struct IdleAction {
        std::wstring label;
        bool primary = false;
        std::function<void()> onClick;
    };
    void setIdleActions(std::vector<IdleAction> actions);
    // Shorthand: a single link (e.g. 「怎麼連線？」); empty label hides it.
    // Same as setIdleActions({{label, false, onClick}}).
    void setIdleHelpLink(const std::wstring& label, std::function<void()> onClick);

    // Idle-screen subtitle lines, e.g. both "iPhone：控制中心 → 螢幕鏡像" and
    // "Android：投放 → 自在投影". Replaces the default iPhone-only hint (and
    // the title becomes 「等待手機連線…」); at most 3 lines are shown; an
    // empty list restores the default. A tab in a line starts a smaller,
    // muted suffix (e.g. L"Android：投放 → 自在投影\t（重新開機後可用）").
    void setIdleHints(std::vector<std::wstring> lines);

    // pm::VideoSink (thread-safe, non-blocking)
    void onCodec(VideoCodec codec) override;
    void onFrame(const uint8_t* annexB, size_t len, uint64_t ntpLocalNs) override;
    void onSourceSize(int width, int height) override;
    void onReset() override;
    void onPaused(bool paused) override;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace pm
