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
        // Watchdog actions: decoder restarts (no picture decoded although
        // AUs were fed) and swap-chain re-creations (nothing presented).
        long long watchdogRecoveries = 0;
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

    // Process-wide: every "[video] ..." / "[video-watchdog] ..." log line
    // (no trailing newline) also goes to fn, from any thread (the app writes
    // them to its log file).  Lines always go to stderr and the debugger.
    static void setLogHandler(std::function<void(const char* line)> fn);

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
    //   * recording: toggled in the red REC style (pulsing dot); other
    //     toggled buttons (放大鏡, 翻譯, 凍結) get the theme's accent (0.7.2).
    //   * optional: left out when the window is too narrow for every button
    //     at the smallest size (the command is also in 更多 / the menu).
    // Call again to update (e.g. toggled); an empty list removes it.
    struct ToolbarItem {
        int id = 0;
        wchar_t glyph = 0;
        std::wstring tooltip;
        bool toggled = false;
        bool danger = false;
        bool groupStart = false;
        bool recording = false;
        bool optional = false;
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

    // ---- Magnifier, high-contrast view, freeze (thread-safe, cheap) ----
    // Zoom 1..8x inside the picture viewport (the letterbox / device frame
    // stay), around a centre point; a large "放大 2.5×" indicator shows for
    // ~1.2 s after every change, then a persistent zoom badge (bottom left)
    // and an overview of the whole picture with the magnified part outlined
    // (bottom right) stay while zoomed.  Pointer input for the phone, region
    // selection and overlays follow the zoom.  Snapshots, the frame tap and
    // recordings are unaffected (always the whole, unfiltered picture).
    // Built-in input (no app code needed): Ctrl+wheel zooms at the cursor;
    // while zoomed, a left drag pans (Ctrl+left drag when a pointer handler
    // is set, i.e. remote control), the wheel / Shift+wheel pan (without a
    // pointer handler), arrow keys pan (without a key handler) and
    // Alt+arrows always pan; Ctrl+= / Ctrl+- zoom, Ctrl+Shift+0 back to 1x
    // (unless the app handles those keys first).
    enum class Filter { None, Contrast, Grayscale, Invert, YellowOnBlack };
    struct ViewState {
        float zoom = 1;                     // 1..8
        float centerX = 0.5f, centerY = 0.5f;  // shown point at the viewport centre, 0..1 of the displayed picture
        Filter filter = Filter::None;
        bool frozen = false;
    };
    // Zoom around the current centre (clamped to 1..8; 1 = whole picture).
    void setZoom(float zoom);
    // Zoom keeping the viewport point (vx, vy) (0..1 of the picture viewport) fixed.
    void zoomAt(float zoom, float vx, float vy);
    // One step in (+) / out (-): x1.25 per step (snaps to 1 near 1).
    void zoomStep(int steps);
    // Pan by a fraction of the viewport (dx = 0.1: a tenth of its width to the right).
    void panBy(float dx, float dy);
    void resetMagnifier();  // zoom 1, centred (filter / freeze unchanged)
    // Colour filter of the picture (pixel shader; also in the overview):
    // Contrast = steeper contrast + saturation, Grayscale, Invert (black <->
    // white: a dark-mode UI becomes black on white), YellowOnBlack (dark text
    // on a light background becomes yellow on black).
    void setFilter(Filter f);
    // Freeze: keep showing the current picture (badge 「畫面已凍結」) while
    // the stream goes on in the background (frame tap / recording keep the
    // live stream).  Also stays up while the phone screen is off.  Cleared by
    // onReset() (source ended).
    void setFrozen(bool frozen);
    ViewState viewState() const;
    // Called on the UI thread after the user changed the view with the
    // built-in input (wheel / drag / keys), e.g. to update menus.  Changes
    // made through the setters above are not reported.
    void setViewHandler(std::function<void(const ViewState&)> fn);

    // ---- Text overlay (on-screen translation; drawn by video/, filled by pm_translate) ----
    // Boxes in "content" coordinates: 0..1 of the picture as grabPicture()
    // returns it (rotated, not mirrored, unzoomed).  Each box: a themed
    // semi-opaque card over the original text with `text` auto-fitted
    // (largest font that fits, wrapping; at 9 DIP the card grows downwards).
    // Follows zoom / pan / rotation; mirroring moves the boxes, the text stays
    // readable.  Empty list removes them.  Cleared by onReset().
    //
    // 0.7.1: the block's area is painted in its own background colour (bg)
    // and the translation drawn in its text colour (fg), left-aligned, as
    // large as fits (at least 11 DIP); cards never overlap.  Blocks too small
    // for that get numbered markers and a list panel (scrollable; hover /
    // click a row: its block is highlighted; click a marker: its row; the
    // panel's 「放大這一塊」 magnifies the listed blocks).  With the
    // 加強對比 / 黃字黑底 filters the cards are high-contrast (theme card /
    // yellow on black).  The panel and markers take the mouse (nothing goes
    // to the phone there).
    struct TextBox {
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        std::wstring text;      // translated
        std::wstring original;  // recognised source text (for the app / tests)
        int lines = 1;          // text lines of the original in the box (font size ~ box height / lines)
        uint32_t bg = 0, fg = 0;  // 0xRRGGBB: background / text colour around the original (if colors)
        bool colors = false;
    };
    void setTextOverlay(std::vector<TextBox> boxes);
    // 翻譯 ▸ 顯示方式: mode 0 automatic (listed when > 30 % do not fit), 1
    // 原位顯示 (whatever fits in place, the rest listed), 2 清單顯示 (all
    // listed); dark: 深色方框 (the 0.7.0 dark cards, low vision) instead of
    // the picture's own colours.  Kept across overlays.
    void setTextOverlayStyle(int mode, bool dark);
    // Layout of the shown overlay (tests / logs): blocks in place, listed,
    // and the checks (overlapping pairs, cut characters, lines starting with
    // 、。」 etc., stub last lines: all 0 when the layout is right).
    struct TextOverlayInfo {
        int inPlace = 0, listed = 0, notFitting = 0;
        bool zoomButton = false, listAll = false;
        int overlaps = 0, tooClose = 0, truncated = 0, kinsoku = 0, shortLast = 0, markerClashes = 0, fontSizes = 0;
        float minFontPx = 0;
        int old070Cards = 0, old070Overlaps = 0, old070Cut = 0, old070ShortLast = 0;  // PM_OVERLAY_070 (tests)
    };
    TextOverlayInfo textOverlayInfo() const;
    // true: hide the translations, only outline their areas (the picture's
    // own text shows).
    void setTextOverlayOriginal(bool showOriginal);
    // Centre card with a spinner, e.g. 「正在翻譯…」; empty string hides it.
    void setOverlayBusy(const std::wstring& label);
    // Lets the user drag a rectangle on the picture (crosshair cursor, hint
    // 「拖曳框出要翻譯的範圍（Esc 取消）」, veil outside the rectangle).
    // done(ok, x0, y0, x1, y1) on the UI thread with the rectangle in content
    // coordinates (ok false: Esc, right click, focus loss, cancelRegionSelect()
    // or a tiny rectangle).  Nothing reaches the phone meanwhile.
    void beginRegionSelect(std::function<void(bool ok, float x0, float y0, float x1, float y1)> done);
    void cancelRegionSelect();
    // The shown picture (the frozen one while frozen) as 32-bit BGRA rows
    // (stride w*4, alpha 255): cropped, rotated, NOT mirrored, unzoomed,
    // unfiltered — the content coordinate space.  Any thread (also the UI
    // thread); blocks until the render thread copied it (a few ms).  False
    // if nothing is shown.
    bool grabPicture(std::vector<uint8_t>& bgra, int& width, int& height);
    // Runs fn on the window's UI thread (posted; at once when called on it).
    // Dropped if the window is gone.  For modules working on their own
    // threads (pm_translate) that must show UI.
    void post(std::function<void()> fn);

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
