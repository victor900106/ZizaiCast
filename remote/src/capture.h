#pragma once
// Monitor capture (the primary one, or another by index), all on the GPU:
//   DXGI Desktop Duplication -> copy (desk_) -> compose (desk_ + mouse
//   pointer drawn with a tiny shader, XOR/monochrome pointers included)
//   -> D3D11 video processor BGRA->NV12 + scale into a pool texture
//   -> capture-time stamp written into the NV12 planes (ClearView).
// The pool texture goes straight to a D3D11-aware hardware encoder (zero
// copy). readback() is the old CPU path (staging copy -> packed NV12) for
// software encoders and for comparison (--cpu-path).

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pm::remote {

using Microsoft::WRL::ComPtr;

// Capture-time stamp (see capture.cpp / web/index.html readStamp).
int stampBlockSize(int streamHeight);
void drawStampCpu(std::vector<uint8_t>& nv12, int w, int h, uint32_t ms);

// Monitors attached to the desktop: the primary one first, then the others
// left to right (top to bottom). Physical pixels, virtual-desktop coordinates.
struct MonitorDesc {
    std::wstring device;  // \\.\DISPLAY2
    RECT rect{};
    bool primary = false;
};
std::vector<MonitorDesc> enumMonitors();

class DesktopCapture {
public:
    // Test scenes (reproducible measurements, independent of what is on the
    // owner's screen): Static = the first desktop image frozen; Motion = the
    // frozen image panned every frame (whole-frame motion, like scrolling).
    enum class Scene { Desktop, Static, Motion };
    struct Options {
        int maxWidth = 1280, maxHeight = 720;  // output fits inside, aspect kept, even sizes
        bool drawCursor = true;
        Scene scene = Scene::Desktop;
        int poolSize = 6;
        int monitor = 0;  // index into enumMonitors() (0 = primary); out of range = primary
    };
    ~DesktopCapture();
    bool open(const Options& o);
    void close();

    // Waits up to timeoutMs for a desktop update. changed = new image or
    // pointer moved / changed shape since the last render(). False only on a
    // fatal error (pipeline must be reopened).
    bool poll(int timeoutMs, bool& changed);
    // Renders the current picture into a free pool texture; returns the slot
    // or -1 when every slot is still held by the encoder.
    int render(bool stamp, uint32_t stampMs);
    ID3D11Texture2D* texture(int slot) const { return pool_[size_t(slot)]->tex.Get(); }
    void release(int slot) { pool_[size_t(slot)]->busy = false; }
    // CPU path: slot -> staging -> packed NV12 (width*height*3/2).
    bool readback(int slot, std::vector<uint8_t>& out);

    ID3D11Device* device() const { return dev_.Get(); }
    LUID adapterLuid() const { return luid_; }
    int width() const { return outW_; }
    int height() const { return outH_; }
    int desktopWidth() const { return deskW_; }
    int desktopHeight() const { return deskH_; }
    // The captured monitor in virtual-desktop coordinates (physical pixels).
    RECT desktopRect() const { return rect_; }
    int monitorIndex() const { return monIndex_; }
    std::string adapterName() const { return adapter_; }
    int busySlots() const;
    bool stampOnGpu() const { return stampGpu_; }  // false: caller draws the stamp after readback
    // 0.8.3.5: a PC browser draws the pointer itself (no lag, no double pointer): leave it out of
    // the picture while set. Any thread.
    void setHideCursor(bool hide) { hideCursor_ = hide; }

private:
    bool createDuplication();
    bool createPipeline();
    bool createShaders();
    void updatePointerShape(const DXGI_OUTDUPL_FRAME_INFO& fi);
    void compose();

    struct Slot {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11VideoProcessorOutputView> vpOut;
        ComPtr<ID3D11RenderTargetView> lumaRtv, chromaRtv;
        std::atomic<bool> busy{false};
    };

    Options opt_;
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11DeviceContext1> ctx1_;
    ComPtr<IDXGIOutput1> output_;
    ComPtr<IDXGIOutputDuplication> dupl_;
    ComPtr<ID3D11Texture2D> desk_;     // clean copy of the newest desktop image
    ComPtr<ID3D11Texture2D> comp_;     // desk_ + pointer (video processor input)
    ComPtr<ID3D11RenderTargetView> compRtv_;
    ComPtr<ID3D11Texture2D> staging_;  // CPU path
    std::vector<std::unique_ptr<Slot>> pool_;
    ComPtr<ID3D11VideoDevice> vdev_;
    ComPtr<ID3D11VideoContext> vctx_;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    ComPtr<ID3D11VideoProcessor> vp_;
    ComPtr<ID3D11VideoProcessorInputView> vpIn_;
    // pointer
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11SamplerState> smp_;
    ComPtr<ID3D11BlendState> blendAlpha_, blendInvert_;
    ComPtr<ID3D11RasterizerState> rs_;
    ComPtr<ID3D11ShaderResourceView> ptrColor_, ptrInvert_;  // invert = XOR part (null if none)
    int ptrW_ = 0, ptrH_ = 0, ptrHotX_ = 0, ptrHotY_ = 0;
    int ptrX_ = 0, ptrY_ = 0;
    bool ptrVisible_ = false;
    std::atomic<bool> hideCursor_{false};
    bool hideApplied_ = false;  // capture thread
    std::vector<uint8_t> shapeBuf_;

    LUID luid_{};
    RECT rect_{};
    int monIndex_ = 0;
    int deskW_ = 0, deskH_ = 0, outW_ = 0, outH_ = 0;
    bool needCompose_ = true, changed_ = true, frozen_ = false, lost_ = false, stampGpu_ = false;
    ComPtr<ID3D10Multithread> mt_;
    int64_t frameNo_ = 0;
    std::string adapter_;
};

}  // namespace pm::remote
