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
#include <string>
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
    void setOptions(std::vector<Option> options) { options_ = std::move(options); }
    void setHover(int index) { hover_ = index; }
    // Idle screen: subtitle lines replacing the default hint (empty: default;
    // a tab starts a smaller, muted suffix) and the row of actions under the
    // check boxes (primary: outlined pill button, else an underlined link).
    struct Action {
        std::wstring label;
        bool primary = false;
    };
    void setHints(std::vector<std::wstring> lines) { hints_ = std::move(lines); }
    void setActions(std::vector<Action> actions) { actions_ = std::move(actions); }
    void setActionHover(int index) { actionHot_ = index; }
    void setPin(const std::wstring& pin);
    // holdMs: how long the toast stays fully visible (<= 0: the default 2.5 s).
    void showToast(const std::wstring& text, double holdMs = 0);

    // ---- Live toolbar (pill of icon buttons at the top of the picture) ----
    struct ToolItem {
        wchar_t glyph = 0;
        std::wstring tip;
        bool toggled = false, danger = false, group = false;
    };
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
    void drawPicture(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp);
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
    Layout layoutFor() const;
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
    D2D1_RECT_F recBadge_{};   // last REC badge (DIPs), empty if none
    std::wstring iconFamily_;  // Segoe Fluent Icons / Segoe MDL2 Assets ("-": none)
    ComPtr<IDWriteTextFormat> iconFmt_;
    float iconFmtSize_ = 0;

    Palette pal_;
    int theme_ = 0;
    bool dimmed_ = false;
    double dimAt_ = -1e9;
    float dimFrom_ = 0;
    int rot_ = 0;
    bool mirror_ = false;
    bool frame_ = false;
    bool recording_ = false;
    double recAt_ = 0;
    // Mascot: hover grow, click / connect reaction (hop + hearts [+ bubble]).
    bool mascotHot_ = false;
    double mascotHotAt_ = -1e9;
    double reactAt_ = -1e9;
    bool reactBubble_ = false;
    int bubbleLine_ = -1;
    bool connectIntro_ = false;  // connecting scene starts with the happy hop
    RECT mascotRect_{};
    float sceneOpacity_ = 1;  // opacity of the scene being drawn (drawTextBlock)

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
