// D3D11 flip-model swap chain renderer: NV12 -> RGB in a pixel shader,
// aspect-preserving letterbox, Direct2D/DirectWrite status screens (with the
// 投投 mascot) and overlays (idle / connecting / paused, PIN, toast, idle check boxes).
// All methods must be called on the render thread.
#pragma once

#include <d2d1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_3.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mf_decoder.h"
#include "ui_art.h"

namespace pm::video {

// Monotonic milliseconds (QPC).
double clockMs();

class Renderer {
public:
    struct Option {
        std::wstring label;
        bool checked = false;
    };

    // A decoded picture copied into renderer-owned textures (hw -> one NV12
    // texture with two SRVs; sw -> R8 + R8G8).  Used for the shown picture
    // and for pictures held back by the A/V sync queue.
    struct Picture {
        bool hw = false;
        bool tenBit = false;  // P010 (R16 / R16G16 views)
        bool bgra = false;    // already-decoded BGRA picture (y = the texture, ySrv its view)
        UINT w = 0, h = 0;
        ComPtr<ID3D11Texture2D> nv12, y, uv;
        ComPtr<ID3D11ShaderResourceView> ySrv, uvSrv;
        VideoFormat fmt;
        bool valid() const { return ySrv != nullptr; }
    };

    bool init(HWND hwnd, ID3D11Device* device);
    void shutdown();
    // Device loss: drops every D3D/D2D device object (swap chain, textures,
    // shaders, render targets) but keeps the UI state and the device-
    // independent factories.  attachDevice() rebuilds them on a new device.
    void releaseDevice();
    bool attachDevice(ID3D11Device* device);
    bool hasDevice() const { return swap_ != nullptr; }

    // Window client size changed (0 = minimized).  False if the device was
    // lost while resizing.
    bool resize(UINT width, UINT height);
    void setDpi(UINT dpi) { dpi_ = dpi ? dpi : 96; }
    // Window shown / hidden (SW_HIDE, e.g. minimized to the tray).
    void setVisible(bool v) { visible_ = v; }
    // Copies a decoded picture into renderer-owned textures; the first picture
    // after idle/connecting starts the fade-in.
    bool upload(IMFSample* sample, const VideoFormat& fmt);
    // A/V sync: copies a decoded picture into a pooled texture without
    // showing it; show() makes it the current picture later (cheap swap).
    bool hold(IMFSample* sample, const VideoFormat& fmt, Picture& out);
    // Already-decoded BGRA picture (Miracast): uploaded into a reused BGRA
    // texture and shown like a decoded one.  False on failure (device lost).
    bool uploadBgra(const uint8_t* bgra, UINT w, UINT h, UINT stride);
    void show(Picture&& pic);
    void recycle(Picture&& pic);
    // Frees pooled (not shown) picture textures.
    void trimPool() { pool_.clear(); }
    // Stream ended: fade the last picture out to the idle screen.
    void reset();
    bool idle() const { return scene_ != Scene::Live; }
    // Phone screen off: show a notice instead of a frozen last frame.
    void setPaused(bool paused) {
        if (paused != paused_) poke();  // the sleeping mascot floats for a while
        paused_ = paused;
    }
    bool paused() const { return paused_; }
    void setConnecting(const std::wstring& name);
    // A check box whose state changed (same labels) animates its tick.
    void setOptions(std::vector<Option> options);
    void setHover(int index) {
        hover_ = index;
        hoverTo(UiOption, index);
    }
    // Idle screen: subtitle lines replacing the default hint (empty: default;
    // a tab starts a smaller, muted suffix) and the row of actions under the
    // check boxes (primary: outlined pill button, else an underlined link).
    struct Action {
        std::wstring label;
        bool primary = false;
        int card = -1;  // 0.7.8: the button of idle card #card (drawn in it, not in the row)
    };
    void setHints(std::vector<std::wstring> lines) { hints_ = std::move(lines); }
    // 0.7.8: the idle screen as 「請選你的手機」 + up to three cards (title,
    // one line of how, a muted note; muted: greyed, cannot be used now).  Each
    // card's button is the Action with that card index.  Non-empty: replaces
    // the hints and the default title.
    struct Card {
        std::wstring title, body, note;
        bool muted = false;
    };
    void setCards(std::vector<Card> cards) { cards_ = std::move(cards); }
    void setActions(std::vector<Action> actions) { actions_ = std::move(actions); }
    void setActionHover(int index) {
        actionHot_ = index;
        hoverTo(UiAction, index);
    }
    // ---- Hover / press / keyboard focus feedback ----
    // Kinds: the idle screen's check boxes and actions, the toolbar's buttons.
    // Hover eases in over 120 ms and out over 180 ms; a pressed element
    // shrinks to 96 % (toolbar 92 %); the focused one gets a ring.
    enum UiKind { UiOption = 0, UiAction = 1, UiTool = 2 };
    void setPressed(int kind, int index);  // kind < 0: nothing pressed
    void setFocus(int kind, int index);    // kind < 0: no focus ring
    void setPin(const std::wstring& pin);
    // holdMs: how long the toast stays fully visible, at least its reading
    // time (2.5-8 s by length; <= 0: just that).
    void showToast(const std::wstring& text, double holdMs = 0);
    // Screenshot taken: a short white veil over the picture only (none with
    // reduced motion).
    void flash() {
        if (!reduced_) flashAt_ = clockMs();
    }

    // ---- Live toolbar (pill of icon buttons at the top of the picture) ----
    struct ToolItem {
        wchar_t glyph = 0;
        std::wstring tip;
        bool toggled = false, danger = false, group = false;
        bool recording = false;  // toggled = REC red + pulsing dot (else the accent)
        bool optional = false;   // hidden when the window is too narrow for every button
        // >= 0: a compact horizontal slider at this position (0..1) instead of a
        // button (glyph unused; toggled = greyed, e.g. muted).  Hidden only when
        // the window is too narrow even without every optional button.
        float slider = -1;
        // 0.7.8: short caption under the icon (all or none: none when the
        // window is too narrow for the buttons they widen).
        std::wstring label;
    };
    // One-time introduction: shown by itself for holdMs (pending until the
    // toolbar is available on a live picture) with note in a callout under it.
    void revealToolbar(double holdMs, std::wstring note) {
        revealMs_ = holdMs;
        revealNote_ = std::move(note);
    }
    // Slider track ends: this fraction of the item's height in from each side
    // (hit rect -> value mapping in VideoWindow uses the same).
    static constexpr float kToolSliderInset = 0.22f;
    void setToolbar(std::vector<ToolItem> items);
    // Mouse moved over the window: show it (fade in) for ~2 s.
    bool toolbarActivity();  // true if that needs a frame (fading in)
    // Mouse left the window: start hiding now (unless it is under the cursor).
    void toolbarLeave();
    // Button under the cursor (-1 none; drives hover + tooltip) and whether
    // the cursor is on the pill at all (keeps it up).
    void setToolbarHover(int index, bool inside);
    // Button / pill hit rectangles in client pixels while the toolbar is up
    // (empty when hidden or mostly faded out).  Updated by render().
    const std::vector<RECT>& toolbarRects() const { return toolRects_; }
    const RECT& toolbarPill() const { return toolPill_; }
    // User activity: restarts the idle screen's ambient animation.
    void poke() { ambientUntil_ = clockMs() + kAmbientMs; }
    // Windows 「顯示動畫」 off (SPI_GETCLIENTAREAANIMATION): no ambient motion
    // (the idle screen is a static picture), no hop / hearts / sparkles /
    // flash, every transition a plain fade of at most 120 ms.
    void setReducedMotion(bool on) { reduced_ = on; }
    bool reducedMotion() const { return reduced_; }

    // ---- Presentation / theme / recording ----
    struct Palette {
        D2D1_COLOR_F bgTop, bgBottom, fg, dim, accent, accent2, ink, card, well;
    };
    static const Palette& palette(int theme);  // 0 Sakura, 1 Mint, 2 Night, 3 MilkTea
    void setTheme(int theme);
    void setDimmed(bool dimmed);
    void setTransform(int quarterTurnsCw, bool mirrored);
    void setDeviceFrame(bool on) { frame_ = on; }
    void setRecording(bool on);
    // Displayed picture size after rotation (0 x 0 without a picture).
    void displaySize(int& w, int& h) const;
    // Live picture viewport in client pixels (empty unless a live picture is
    // on screen) and the rotation / mirroring it was drawn with.  Updated by
    // render().
    const RECT& pictureRect() const { return picRect_; }
    int rotation() const { return rot_; }
    bool mirrored() const { return mirror_; }
    // Screen position t (0..1 in the picture viewport, y down) -> position in
    // the picture (0..1) for a clockwise rotation by rot quarter turns
    // followed by a horizontal mirror: the mapping the vertex shader uses.
    static void screenToPicture(int rot, bool mirror, float tx, float ty, float& sx, float& sy);

    // ---- Magnifier / high-contrast filters / freeze / text overlay ----
    // Coordinates: "viewport" v = 0..1 over pictureRect(); "display" t =
    // 0..1 over the whole displayed picture (rotated + mirrored, unzoomed):
    // t = centre + (v - 0.5) / zoom; "content" d = the rotated, unmirrored
    // picture (what grab() returns): d.x = mirrored ? 1 - t.x : t.x.
    struct View {
        float zoom = 1, cx = 0.5f, cy = 0.5f;  // centre in display coords
        int filter = 0;                         // 0 none, 1 contrast, 2 grey, 3 invert, 4 yellow on black
    };
    static constexpr float kMaxZoom = 8;
    // Clamps zoom to 1..kMaxZoom and the centre so the view stays inside.
    static View clampView(View v);
    void setView(const View& v);
    const View& view() const { return view_; }
    // Freeze: keeps a copy of the current picture on screen while decoding
    // (and the frame tap / recorder) go on with the live stream.  A CPU copy
    // of it is kept too, so a device loss does not swap it for another
    // picture: attachDevice() uploads it again.
    void setFrozen(bool on);
    bool frozen() const { return frozenOn_; }
    // After attachDevice(): true (once) if a freeze ended because its picture
    // could not be restored (no CPU copy); the text overlay was cleared too.
    bool takeFrozenDropped() { return std::exchange(frozenDropped_, false); }
    // Translated text boxes (content coords) drawn over the picture.
    struct TextBox {
        float x0, y0, x1, y1;
        std::wstring text, original;
        int lines = 1;
        uint32_t bg = 0, fg = 0;  // 0xRRGGBB sampled from the picture (colors)
        bool colors = false;
        // Set by setTextOverlay from the marks at the start of text (pm_translate,
        // text_util.h kOverlayRow / kOverlayUncertain): 1 a table row (listed as
        // 「標籤　值」), 2 a translation that failed a check (listed, its checked
        // key facts highlighted, selecting its row shows the original).
        int kind = 0;
        std::wstring facts;  // kind 2: the checked key facts (the text's last line 「⚠ …」)
        bool online = false;  // (part of) it came from online translation: a 「線上」 badge (card corner, list row)
    };
    // In place: each block's area painted in its background colour with the
    // translation in its text colour (dark cards with the 加強對比 / 黃字黑底
    // filters), never overlapping; blocks whose translation does not fit
    // readably get numbered markers and a list panel (text_overlay.cpp).
    void setTextOverlay(std::vector<TextBox> boxes);
    // The boxes whose original is in `originals` get the 「線上」 badge (the others lose it).
    void setTextOverlayOnline(const std::vector<std::wstring>& originals);
    void setTextOverlayOriginal(bool on) { showOriginal_ = on; }
    // 即時翻譯 (live_overlay.cpp): every block in place, never the list
    // panel; the boxes follow the page when it scrolls (live_scroll_tracker)
    // and hide when the picture changed some other way.  liveBoxesSet():
    // after setTextOverlay - the boxes are of the picture of the last grab().
    void setOverlayLive(bool on);
    bool overlayLive() const;
    void liveBoxesSet();
    // List panel / markers: item = list number - 1.  hot: under the cursor
    // (-1 none); selected: clicked (reveal: scroll the list to it); scroll in DIPs.
    void setOverlayHot(int item) { ovHot_ = item; }
    void setOverlaySelected(int item, bool reveal) {
        ovSel_ = item;
        ovReveal_ = reveal && item >= 0;
    }
    void scrollOverlayList(float dips) { ovScroll_ += dips; }
    struct OverlayHits {
        RECT panel{}, zoomBtn{};                          // client px (empty: none)
        std::vector<std::pair<RECT, int>> rows, markers;  // client px, item
        float zoomTo[4] = {0, 0, 0, 0};                   // content rect to magnify ("放大這一塊")
        int inPlace = 0, listed = 0;                      // layout result (tests, logs)
        int notFitting = 0;                               // blocks whose translation did not fit in place
        bool listAll = false;                             // every block listed (清單顯示 or > 30 % not fitting)
        // Checks (tests, logs; all must be 0): pairs of drawn things
        // overlapping (cards, markers, the list panel), cards closer than
        // 4 DIPs, characters cut / hidden (outside their card or the picture,
        // under another card, a marker or the panel), lines starting with
        // a character that must not start a line (、。」ー ッ …).
        int overlaps = 0, tooClose = 0, truncated = 0, kinsoku = 0;
        int shortLast = 0;                                // paragraphs whose last line is a stub (<= 2 characters or < 1/3 of the longest)
        int markerClashes = 0;                            // markers that found no free place
        int fontSizes = 0;                                // distinct in-place text sizes
        float minFont = 0;                                // smallest text drawn (pixels)
        // PM_OVERLAY_070 (tests): the same checks on the 0.7.0 layout.
        int old070Cards = 0, old070Overlaps = 0, old070Cut = 0, old070ShortLast = 0;
    };
    // 0 automatic, 1 in place (whatever fits; the rest listed), 2 list only.
    void setOverlayMode(int mode) {
        if (mode != ovMode_) ovValid_ = false;
        ovMode_ = mode;
    }
    // Dark cards (the 0.7.0 style: theme card colour, light text) instead of
    // the picture's own colours: easier to read for low vision.
    void setOverlayDark(bool on) { ovDark_ = on; }
    const OverlayHits& overlayHits() const { return ovHits_; }
    // Centre pill with a spinner (e.g. 「正在翻譯…」); empty hides.
    void setBusy(const std::wstring& label);
    // Region selection in progress: rectangle in viewport coords (x0 < 0: none
    // dragged yet), plus the hint pill.
    void setSelection(bool active, float x0, float y0, float x1, float y1);
    // The shown (frozen or live) picture as BGRA rows (stride w*4), rotated
    // (and mirrored only if mirror), cropped, unzoomed, unfiltered.
    bool grab(std::vector<uint8_t>& bgra, UINT& w, UINT& h, bool mirror);

    // ---- Mascot interaction ----
    // Hit area of the mascot (its layer canvas at rest) in client pixels
    // (empty when it cannot be clicked: live picture, fading, PIN up); the
    // art's mask() says which parts of it are opaque.  Updated by render().
    const RECT& mascotRect() const { return mascotRect_; }
    void setMascotHover(bool hot);
    void mascotClicked();

    // ---- Decoded-frame tap (NV12 8-bit read-back through a staging ring) ----
    static constexpr int kTapRing = 3;
    using TapFn = std::function<void(const uint8_t* nv12, int w, int h, int stride, uint64_t ptsNs)>;
    // Converts pic (nullptr = the current picture) on the GPU (crop, 10 -> 8
    // bit) and queues its read-back.  False on failure (device lost).
    bool tapSubmit(const Picture* pic, uint64_t ptsNs);
    // Maps queued read-backs and calls fn for each: all but the newest, or
    // all when flushAll.  fn may be empty (pending entries are dropped).
    void tapDeliver(const TapFn& fn, bool flushAll);
    // Milliseconds until the newest queued read-back should be flushed
    // (0 = now), < 0 if none is queued.
    double tapFlushInMs() const;
    int tapPending() const {
        int n = 0;
        for (const auto& t : tap_) n += t.pending;
        return n;
    }
    void tapReset();
    // Render-thread time of the last tapSubmit + its delivery (ms, excl. fn).
    double tapCostMs(bool reset) {
        double v = tapCost_;
        if (reset) tapCost_ = 0;
        return v;
    }

    // Signalled when the swap chain can accept a new frame (max latency 1).
    HANDLE frameWaitable() const { return waitable_; }
    // Draws the current picture / status screen + overlays and presents.
    // Returns false when the device was lost.
    bool render();
    // Outcome of the last render(): whether Present was really called and
    // returned S_OK / DXGI_STATUS_OCCLUDED (false: nothing reached DXGI, e.g.
    // minimized, no render target, or a failed Present), and its HRESULT.
    bool lastPresented() const { return presented_; }
    HRESULT lastPresentHr() const { return presentHr_; }
    bool occluded() const { return occluded_; }
    bool minimized() const { return width_ == 0 || height_ == 0; }
    bool visible() const { return visible_; }
    // Watchdog: a new swap chain on the same device and window (keeps the
    // current picture, D2D/UI state).  False on failure (caller re-creates
    // the device).
    bool recreateSwapChain();
    // Fault injection (pm_video_test): Present's result is reported as
    // DXGI_STATUS_OCCLUDED until untilMs (clockMs); swallowPresents: Present
    // is no longer called (a stuck swap chain) until recreateSwapChain().
    void faultOcclude(double untilMs) { faultOccludeUntil_ = untilMs; }
    void faultSwallowPresents() { faultSwallow_ = true; }
    // GPU completion waits (copyIn) that gave up after kGpuWaitMaxMs.
    long long gpuWaitTimeouts() const { return gpuWaitTimeouts_; }
    // Milliseconds until the next animation frame is due (0 = now), or a
    // negative value when nothing animates (event-driven rendering).
    double nextFrameInMs() const;
    // Check-box hit rectangles in client pixels (empty unless shown).
    const std::vector<RECT>& optionRects() const { return optionRects_; }
    // Action hit rectangles in client pixels, one per action while shown
    // (empty otherwise).
    const std::vector<RECT>& actionRects() const { return actionRects_; }

    // Converts the current picture (cropped, rotated / mirrored) to 32-bit
    // rows (stride w*4).  framed: inside the device frame on a transparent
    // background (straight BGRA), else BGRX.
    bool snapshot(std::vector<uint8_t>& bgra, UINT& w, UINT& h, bool framed);
    const ToutouArt& mascotArt() const { return toutou_; }
    // Test / docs hook: draws the current UI now (exactly what render()
    // shows) and copies the back buffer out (BGRX rows, stride w*4) before
    // presenting.  False if nothing could be drawn (device lost, minimized).
    bool renderCapture(std::vector<uint8_t>& bgrx, UINT& w, UINT& h);

private:
    void copyBackBuffer();

public:

private:
    enum class Scene { Idle, Connecting, Live };
    static constexpr double kAmbientMs = 40000;  // idle animation after activity

    bool createTargets();
    bool createDeviceObjects();
    bool ensureTextures(Picture& p, bool hw, bool tenBit, UINT w, UINT h);
    bool ensureBgra(Picture& p, UINT w, UINT h);
    bool copyIn(IMFSample* sample, const VideoFormat& fmt, Picture& p);
    void pictureShown();
    // The picture on screen: the frozen copy while frozen, else the live one.
    const Picture& shown() const { return frozenOn_ && frozen_.valid() ? frozen_ : cur_; }
    bool copyPicture(const Picture& from, Picture& to);
    // A picture's planes on the CPU (tightly packed rows), for re-uploading
    // after a device loss: Y (or BGRA) and, unless BGRA, the interleaved UV.
    struct CpuPicture {
        bool bgra = false, tenBit = false;
        UINT w = 0, h = 0, yRow = 0, uvRow = 0, uvH = 0;
        VideoFormat fmt;
        std::vector<uint8_t> y, uv;
    };
    bool readBackPicture(const Picture& p, CpuPicture& c);  // waits for the GPU
    bool restorePicture(const CpuPicture& c, Picture& p);   // as a software (R8/R8G8 or BGRA) picture
    void keepFrozenCopy();      // frozen_ -> frozenCpu_
    void restoreFrozen();       // attachDevice(): frozenCpu_ -> frozen_ (or end the freeze)
    // Draws pic into vp with the zoom of view (identity: whole picture), its
    // filter, rotation and (if mirror) mirroring.
    void drawPicture(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp, const Picture& pic, const View& view,
                     bool mirror);
    void drawPicture(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp) {
        drawPicture(rtv, vp, shown(), view_, mirror_);
    }
    bool renderPicture(std::vector<uint8_t>& out, UINT& w, UINT& h, bool framed, bool mirror);
    // Magnifier / freeze / overlay UI (DIPs; pic = the picture viewport).
    void drawTextOverlay(const D2D1_RECT_F& pic, float radius);
    // 即時翻譯 (live_overlay.cpp): live_ is created on first use.
    struct LiveOverlay;
    std::shared_ptr<LiveOverlay> live_;
    LiveOverlay& live();
    void liveNoteGrab();  // grab(): the signature of the picture handed out
    void liveTrack();     // render(), before 2D: a new picture -> its scroll shift
    void liveReleaseDevice();  // releaseDevice(): its GPU copy belongs to the old device
    void drawLiveOverlay(const D2D1_RECT_F& pic, float radius);
    struct Badges {
        D2D1_RECT_F zoom{}, frozen{};  // DIPs, empty if not shown
    };
    Badges badgeRects(const D2D1_RECT_F& pic, float radius);
    void drawMagnifierUi(const D2D1_RECT_F& pic, float radius, double now);
    D2D1_RECT_F minimapRect(const D3D11_VIEWPORT& vp, float radiusPx) const;  // pixels
    void drawSelection(const D2D1_RECT_F& pic, float radius);
    void drawBusy(const D2D1_RECT_F& pic, double now);
    // Content coords -> DIPs on screen (through mirror + zoom).
    D2D1_POINT_2F contentToDip(const D2D1_RECT_F& pic, float dx, float dy) const;
    // Device frame geometry in pixels: the screen (= picture viewport), its
    // corner radius, bezel and outer edge widths.
    struct FrameGeom {
        D2D1_RECT_F screen;
        float radius, bezel, edge;
        bool landscape;
        float outset() const { return bezel + edge * 2.2f; }  // incl. side buttons
    };
    static FrameGeom frameFor(float dw, float dh, float x0, float y0, float W, float H, bool fit);
    void pictureViewport(D3D11_VIEWPORT& vp, FrameGeom* frame) const;
    void drawDeviceFrame(ID2D1RenderTarget* rt, const FrameGeom& g, float pxToDip);
    float dimLevel(double now) const;
    void drawRecBadge(const D2D1_RECT_F& screenDip, float radiusDip, double now);
    bool ensureD2D();
    void releaseD2D();

    // ---- 2D UI (DIP coordinates) ----
    struct Layout {
        float W, H, s;            // client size in DIPs, DIP->pixel scale
        bool landscape;
        float u;                  // DIPs per art frame unit (0: no mascot)
        D2D1_POINT_2F anchor;     // where the art's pivot (cloud bottom centre) sits at rest
        D2D1_RECT_F mascot;       // the layer canvas at rest (no float / squash)
        D2D1_RECT_F text;         // region for the text block
        float title, hint;        // font sizes
    };
    // mascot false: no 投投, the text region is the whole window (see
    // idleNoMascot_); the default follows idleNoMascot_.
    Layout layoutFor() const { return layoutFor(!idleNoMascot_); }
    Layout layoutFor(bool mascot) const;
    // Art frame point -> DIPs at rest (no float / squash).
    static D2D1_POINT_2F framePoint(const Layout& L, float x, float y);
    // The mascot's pose this frame: expression, float, squash / stretch, hover.
    struct Pose {
        ToutouArt::Layer face = ToutouArt::FaceIdle;
        int phonePose = 0;        // toutou::Pose
        float dy = 0;             // float + hop (DIPs, negative = up)
        float lift = 0;           // 0..1 float height (shadow)
        float sx = 1, sy = 1;     // squash / stretch (incl. hover grow)
        float opacity = 1;
        float faceX = 0, faceY = 0;  // face layer offset (frame units): she looks at the phone
        bool lit = true;          // phone screen on
        float beamT0 = 0, beamT1 = 0, beamK = 0;  // visible part of the beam, strength
        double beamFlow = 0;      // beam animation phase
        D2D1::Matrix3x2F m;       // frame units -> DIPs
    };
    Pose mascotPose(const Layout& L, Scene scene, double now);
    void drawMascotFx(const Layout& L, const Pose& P, double now);
    void drawBubble(const Layout& L, const Pose& P, double now);
    void drawTextBlock(Scene scene, const Layout& L, float opacity, double now);
    bool mascotClickable(double now) const;
    void hopTransform(const Layout& L, double now, float& sx, float& sy, float& dy) const;
    void drawBackground(float opacity);
    void drawScene(Scene scene, float opacity, double now);
    void drawMascot(const Layout& L, const Pose& P, double now);
    bool ensureLayerBitmaps(float pxPerUnit);
    void drawLayer(ToutouArt::Layer l, const D2D1_MATRIX_3X2_F& m, float opacity);
    void drawPhone(const Pose& P, float glow);
    // Ribbon of light from the phone top: visible part t0..t1 (0..1 along it),
    // strength k, flow = pixel drift / twist phase (animation).
    void drawBeam(const Pose& P, float t0, float t1, float k, double flow);
    void drawParticles(const Layout& L, double now, float strength);
    void drawSpinner(D2D1_POINT_2F c, float radius, double now);
    void drawZzz(const Layout& L, const Pose& P, double now);
    void drawOptions(float top, float textSize, const D2D1_RECT_F& region);
    // An eased 0..1 level (hover, press, focus) that reverses from where it is.
    struct Fade {
        bool on = false, softOff = false;
        float from = 0;
        double at = -1e9, inMs = 120, outMs = 180;
    };
    static constexpr int kUiKinds = 3, kUiMax = 32;
    Fade uiHot_[kUiKinds][kUiMax], uiPress_[kUiKinds][kUiMax], uiFocus_[kUiKinds][kUiMax];
    float fadeLevel(const Fade& f, double now) const;
    void fadeTo(Fade& f, bool on, double now, double inMs, double outMs, bool softOff = false);
    float uiLevel(const Fade (&set)[kUiKinds][kUiMax], int kind, int index, double now) const {
        return kind >= 0 && kind < kUiKinds && index >= 0 && index < kUiMax ? fadeLevel(set[kind][index], now) : 0.f;
    }
    void hoverTo(int kind, int index);
    bool uiAnimating(double now) const;
    struct ActionLayout {
        ComPtr<IDWriteTextLayout> text;
        float w = 0;  // pill / link width including padding (DIPs)
        bool primary = false;
        int row = 0;
        int index = 0;  // in actions_
    };
    // Lays the actions out in centred rows (wrapping when narrow); returns
    // the number of rows (0: none). rowW: width of each row.
    int layoutActions(std::vector<ActionLayout>& out, std::vector<float>& rowW, float size, float rowH, float maxW,
                      float gap);
    void drawActions(const std::vector<ActionLayout>& acts, const std::vector<float>& rowW, float top, float rowH,
                     float rowGap, float gap, const D2D1_RECT_F& region);
    void drawPin(double now);
    void drawToast(double now);
    float toolbarAlpha(double now) const;
    bool toolbarAvailable() const { return scene_ == Scene::Live && !toolItems_.empty(); }
    // area: the live picture (or the client area without one), DIPs;
    // frame: device frame geometry (pixels) or nullptr.
    void drawToolbar(const D2D1_RECT_F& area, const FrameGeom* frame, double now);
    ComPtr<IDWriteTextFormat> iconFormat(float size);
    ComPtr<IDWriteTextFormat> format(float size, DWRITE_FONT_WEIGHT weight, bool latin = false);
    ComPtr<IDWriteTextLayout> layout(const std::wstring& text, float size, float maxW,
                                     DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL, bool wrap = true,
                                     bool latin = false);
    ID2D1SolidColorBrush* brush(D2D1_COLOR_F c, float alpha = 1.f);
    bool ensureBrushes();
    float textW(IDWriteTextLayout* l);
    float textH(IDWriteTextLayout* l);

    HWND hwnd_ = nullptr;
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain2> swap_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    HANDLE waitable_ = nullptr;
    UINT width_ = 0, height_ = 0;
    UINT dpi_ = 96;

    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_, psBgra_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11Query> evt_;  // GPU completion fence for upload()

    Picture cur_;                // picture on screen
    std::vector<Picture> pool_;  // free textures for held (A/V sync) pictures
    static constexpr size_t kMaxPool = 8;
    bool havePicture_ = false;

    // ---- UI state ----
    Scene scene_ = Scene::Idle;
    Scene fadeFrom_ = Scene::Idle;  // status screen faded out over the first picture
    double fadeInAt_ = -1e9;        // first picture shown
    double fadeOutAt_ = -1e9;       // reset: picture fades to idle
    double sceneAt_ = 0;            // current status scene entered
    double ambientUntil_ = 0;
    double lastRender_ = 0;
    bool paused_ = false;
    std::wstring deviceName_;
    std::vector<Option> options_;
    int hover_ = -1;
    std::vector<RECT> optionRects_;
    std::vector<std::wstring> hints_;
    std::vector<Action> actions_;
    std::vector<Card> cards_;
    // Lays out (and with draw, draws at x / top within maxW) the idle cards;
    // returns their height.  clickable: publishes the buttons' hit rects.
    // mode 0: title, how-to line, note, button; 1 (a short window): no
    // how-to line; 2 (a tiny one): title and button only.
    float drawCards(float x, float top, float maxW, float size, bool draw, bool clickable, int mode = 0);
    // How the 「請選你的手機」 screen fits the text region, most generous
    // first: full cards with the check boxes; without the check boxes (they
    // are in 設定 too); cards without the how-to line; a smaller title; title
    // and button only; no title.  Cards shrink to ~62 % at each step.
    struct IdlePlan {
        int level = -1;  // the step used (-1: none fits; the last one, shrunk)
        bool fits = false, opts = true, title = true;
        int mode = 0;
        float titleSize = 0, cardSize = 0, cardsH = 0, gapCards = 0, block = 0;
    };
    IdlePlan planIdleCards(const Layout& L, bool full);
    bool optionsFit(float size, float regionW);  // every check box label whole (drawOptions trims)
    // A title's size: shrunk up to 25 % so it stays on one line within maxW.
    float oneLineSize(const std::wstring& text, float size, float maxW);
    D2D1_RECT_F cardsRect_{};  // the cards as last drawn (DIPs; empty: none)
    D2D1_RECT_F beamAvoid_{};  // drawBeam: not drawn when it would cross this (the cards)
    bool idleNoMascot_ = false;  // a window too small for 投投 and the cards: cards only
    int actionHot_ = -1;
    std::vector<RECT> actionRects_;
    RECT picRect_{};
    std::wstring pin_;
    bool pinVisible_ = false;
    double pinAt_ = -1e9;
    std::wstring toast_;
    double toastAt_ = -1e9;
    double toastHold_ = 2500;

    // Live toolbar
    std::vector<ToolItem> toolItems_;
    double toolInAt_ = -1e9;   // fade-in started
    double toolUntil_ = -1e9;  // fade-out starts (unless the cursor is on it)
    bool toolInside_ = false;
    int toolHot_ = -1;
    double toolHotAt_ = -1e9;  // hover started (tooltip delay)
    std::vector<RECT> toolRects_;
    RECT toolPill_{};
    double revealMs_ = 0;        // revealToolbar() pending (0: none)
    std::wstring revealNote_;    // its callout text
    double revealUntil_ = -1e9;  // callout shown until (fades with the toolbar)
    void startReveal(double now);
    void drawRevealNote(const D2D1_RECT_F& pill, float a, double now);
    D2D1_RECT_F recBadge_{};   // last REC badge (DIPs), empty if none
    D2D1_RECT_F textBlock_{};  // last idle / connecting text block (DIPs): the hearts fade there
    std::wstring iconFamily_;  // Segoe Fluent Icons / Segoe MDL2 Assets ("-": none)
    ComPtr<IDWriteTextFormat> iconFmt_;
    float iconFmtSize_ = 0;

    Palette pal_;
    int theme_ = 0;
    bool reduced_ = false;  // setReducedMotion
    // A transition's duration: as designed, or a plain 120 ms fade with reduced motion.
    double ms(double normal) const { return reduced_ && normal > 120 ? 120 : normal; }
    double lastPictureAt_ = -1e9;  // newest picture shown (REC dot pulses only while pictures flow)
    float recPulse(double now) const;
    bool dimmed_ = false;
    double dimAt_ = -1e9;
    float dimFrom_ = 0;
    int rot_ = 0;
    bool mirror_ = false;
    bool frame_ = false;
    bool recording_ = false;
    double recAt_ = 0;
    // Magnifier / filter / freeze / overlay
    View view_;
    double zoomAt_ = -1e9;  // zoom level changed (big indicator)
    bool frozenOn_ = false;
    Picture frozen_;
    CpuPicture frozenCpu_;        // CPU copy of frozen_ (survives releaseDevice)
    bool frozenWasUp_ = false;    // releaseDevice() dropped a frozen picture
    bool frozenDropped_ = false;  // takeFrozenDropped()
    std::vector<TextBox> boxes_;
    bool showOriginal_ = false;
    std::wstring busy_;
    double busyAt_ = -1e9;
    bool selecting_ = false;
    float sel_[4] = {-1, -1, -1, -1};
    D2D1_RECT_F miniRect_{};  // magnifier overview (pixels), empty if none
    // Text overlay layout (text_overlay.cpp).
    struct FitCand {          // one way to set a box's translation
        float size = 0, maxW = 0, w = 0, h = 0;
        bool wide = false;    // wider than the original box
        int lines = 0, shortLast = 0, kinsoku = 0;
        ComPtr<IDWriteTextLayout> text;  // explicit line breaks (balanced, 禁則), not wrapped
    };
    struct Fit {              // candidates, cached per box size on screen
        float w = 0, h = 0, min = 0;
        std::vector<FitCand> c;
    };
    std::vector<Fit> fit_;
    struct OvItem {
        int box = 0;
        D2D1_RECT_F r{};      // the block on screen (DIPs)
        float lineH = 0;
        int cand = -1;        // in place: >= 0, laid out as fc in the card rect
        FitCand fc;
        D2D1_RECT_F card{}, text{};
        int number = 0;       // listed: 1..n, marker centre
        D2D1_POINT_2F marker{};
    };
    struct OvRow {
        int item = 0;         // index into ovItems_
        float y = 0, h = 0;   // in the scrolled content
        FitCand fc;
        FitCand facts, alt;   // kind 2: the checked key facts (below fc), the original (shown when selected)
        float factsY = 0;     // facts' top, relative to fc's
        float badgeY = -1;    // online: the 「線上」 badge's top, relative to fc's (-1 none)
    };
    std::vector<OvItem> ovItems_;
    std::vector<OvRow> ovRows_;
    std::vector<int> ovListed_;  // ovItems_ index per list number - 1
    D2D1_RECT_F ovPanel_{}, ovRowsArea_{}, ovZoomBtn_{};
    ComPtr<IDWriteTextLayout> ovTitle_, ovSub_, ovZoomText_;
    float ovFs_ = 15, ovMarkerD_ = 18, ovRowsH_ = 0;
    bool ovValid_ = false, ovDark_ = false;
    float ovKey_[14] = {};
    std::vector<D2D1_RECT_F> ovBlocked_;  // badges and the overview the overlay keeps clear of (DIPs)
    int ovHot_ = -1, ovSel_ = -1, ovMode_ = 0;
    bool ovReveal_ = false;
    float ovScroll_ = 0;
    OverlayHits ovHits_;
    void layoutOverlay(const D2D1_RECT_F& pic, float radius);
    const std::vector<FitCand>& fitsFor(size_t box, float bw, float bh, float lineH, float pw);
    // The text set at `size` in lines of at most maxW DIPs: whole words,
    // 禁則 (no 、。」ー at a line start, no 「（ at a line end), lines of
    // even length (the narrowest width that keeps the line count), left
    // aligned.  force: break a word longer than maxW (the list); otherwise
    // such a text fails (false).
    bool setText(const std::wstring& text, float size, float maxW, DWRITE_FONT_WEIGHT weight, bool force, FitCand& c);
    void overlayChecks(float s);
    void legacyChecks(const D2D1_RECT_F& pic);
    void drawOverlayList(const D2D1_RECT_F& pic);
    // The 「線上」 pill (TrOnlineBadge) at x, y (top-left; right: x is its right
    // edge), text size fs; returns its size (DIPs). draw false: measure only.
    D2D1_SIZE_F onlineBadge(float x, float y, float fs, bool right, bool yellowMode, bool draw = true);
    // Mascot: hover grow, click / connect reaction (hop + hearts [+ bubble]).
    bool mascotHot_ = false;
    double mascotHotAt_ = -1e9;
    double reactAt_ = -1e9;
    double surpriseAt_ = -1e9;  // a phone was found: surprised face, eyes to the phone, sparkles
    double flashAt_ = -1e9;     // flash(): screenshot veil
    std::vector<double> optAt_; // per check box: when its state last changed (tick draw-on)
    void drawSparkles(const Pose& P, double now);
    bool reactBubble_ = false;
    int bubbleLine_ = -1;
    bool connectIntro_ = false;  // connecting scene starts with the happy hop
    RECT mascotRect_{};
    float sceneOpacity_ = 1;  // opacity of the scene being drawn (drawTextBlock)
    float sceneDy_ = 0;       // layoutFor(): the status scene shifted (DIPs; the cross-fade to a picture lifts it)

    // Frame tap
    struct TapSlot {
        ComPtr<ID3D11Texture2D> y, uv;  // staging
        UINT w = 0, h = 0;
        uint64_t pts = 0;
        double at = 0;
        bool pending = false;
        uint64_t seq = 0;
    };
    TapSlot tap_[kTapRing];
    uint64_t tapSeq_ = 0;
    ComPtr<ID3D11Texture2D> tapY_, tapUV_;  // GPU conversion targets
    ComPtr<ID3D11RenderTargetView> tapYRtv_, tapUVRtv_;
    ComPtr<ID3D11PixelShader> psTapY_, psTapUV_, psTapRgbY_, psTapRgbUV_;
    ComPtr<ID3D11Buffer> tapCb_;
    UINT tapW_ = 0, tapH_ = 0;
    std::vector<uint8_t> tapBuf_;
    double tapCost_ = 0;
    bool ensureTap(UINT w, UINT h);

    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID2D1RenderTarget> d2dTarget_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1SolidColorBrush> mutedBrush_;  // drawing effect of muted hint suffixes
    ComPtr<ID2D1LinearGradientBrush> bgBrush_;
    ComPtr<ID2D1RadialGradientBrush> glowBrush_;
    ComPtr<ID2D1Layer> layer_;
    ComPtr<ID2D1StrokeStyle> round_;
    ComPtr<ID2D1PathGeometry> heart_, sparkle_;  // unit-size particle shapes
    ComPtr<ID2D1PathGeometry> playTri_;          // ▶ on the mascot's phone (frame units)
    ToutouArt toutou_;
    ComPtr<ID2D1Bitmap> layerBmp_[ToutouArt::kLayers];  // per render target, at layerScale_
    float layerScale_ = 0;
    // Theme-coloured mascot brushes (rebuilt by ensureBrushes on theme change).
    ComPtr<ID2D1LinearGradientBrush> beamBrush_, beamCoreBrush_, screenBrush_;
    ComPtr<ID2D1RadialGradientBrush> shadowBrush_, phoneGlowBrush_;
    double blinkAt_ = -1e9, nextBlink_ = 0;  // idle blinking
    int blinkN_ = 0;
    std::vector<uint8_t>* capture_ = nullptr;  // renderCapture() target
    UINT captureW_ = 0, captureH_ = 0;
    bool occluded_ = false;
    bool presented_ = false;
    HRESULT presentHr_ = S_OK;
    double faultOccludeUntil_ = 0;
    bool faultSwallow_ = false;
    long long gpuWaitTimeouts_ = 0;
    HRESULT presentErrLogged_ = S_OK;
    static constexpr double kGpuWaitMaxMs = 1000;
    bool createSwapChain();
    bool visible_ = true;
    struct CachedFormat {
        float size;
        DWRITE_FONT_WEIGHT weight;
        bool latin;
        int lang;  // pm::i18n::Lang: UI font + locale of that language
        ComPtr<IDWriteTextFormat> fmt;
    };
    std::vector<CachedFormat> formats_;
};

}  // namespace pm::video
