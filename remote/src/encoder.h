#pragma once
// Media Foundation H.264 encoder for the remote stream: low latency, no B
// frames, (constrained) baseline, periodic IDR, CBR. Hardware MFTs (async
// event model) are preferred; the Microsoft software MFT (sync) is the
// fallback. Input is packed NV12 in system memory; output is Annex B with
// SPS/PPS in front of every IDR.

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pm::remote {

using Microsoft::WRL::ComPtr;

class H264Encoder {
public:
    enum class Kind { Auto, Hardware, Software };
    struct Options {
        int width = 1280, height = 720, fps = 30, kbps = 4000;
        double idrSeconds = 2.0;  // periodic IDR interval (in encoded frames: fps * idrSeconds)
        Kind kind = Kind::Auto;
        bool vbr = false;   // peak-constrained VBR instead of CBR
        int vbvFrames = 2;  // CBR buffer = this many frames of bits (caps IDR size); 0 = encoder default
        // Zero-copy input: the capture device (hardware MFT on the same
        // adapter, NV12 textures as input). nullptr = system-memory NV12.
        ID3D11Device* device = nullptr;
        LUID adapterLuid{};
    };
    // Annex B access unit, the capture time it came from (nowUs clock) and
    // whether it is an IDR. Called on the encoder's thread.
    using OutputFn = std::function<void(std::vector<uint8_t>&& au, int64_t captureUs, bool idr)>;

    ~H264Encoder();
    bool open(const Options& o, OutputFn fn);
    void close();
    // Feeds one NV12 frame (width*height*3/2 bytes). False = dropped / error.
    bool encode(const uint8_t* nv12, size_t size, int64_t captureUs);
    // Zero-copy: feeds an NV12 texture of the capture device. onRelease runs
    // (any thread) once the encoder no longer uses it.
    bool encodeTexture(ID3D11Texture2D* tex, int64_t captureUs, std::function<void()> onRelease);
    bool gpuInput() const { return gpu_; }
    void forceKeyFrame() { wantKey_ = true; }
    bool setBitrate(int kbps);

    const std::string& name() const { return name_; }
    bool hardware() const { return hw_; }
    int dropped() const { return dropped_; }
    int64_t takeFillerBytes() { return fillerBytes_.exchange(0); }

private:
    bool openMft(IMFActivate* act, bool hw);
    bool submit(IMFSample* s, int64_t captureUs);
    void configureCodecApi();
    bool setTypes();
    void drainOutput();
    void eventLoop();
    void emit(IMFSample* s);
    void fixup(std::vector<uint8_t>& au, bool& idr);

    Options opt_;
    OutputFn out_;
    ComPtr<IMFActivate> act_;
    ComPtr<IMFTransform> mft_;
    ComPtr<IMFMediaEventGenerator> events_;
    std::string name_;
    bool hw_ = false, async_ = false, providesSamples_ = false, gpu_ = false;
    ComPtr<IMFDXGIDeviceManager> dxgiMgr_;
    UINT resetToken_ = 0;
    DWORD outBufSize_ = 0;
    std::thread evThread_;
    std::atomic<bool> running_{false};
    std::mutex mu_;  // guards needInput_ and MFT calls on the async path
    std::condition_variable cv_;
    int needInput_ = 0;
    std::atomic<bool> wantKey_{false};
    int64_t t0Us_ = -1;
    int64_t frameNo_ = 0;
    int dropped_ = 0;
    std::atomic<int64_t> fillerBytes_{0};
    std::vector<uint8_t> sps_, pps_;
    bool loggedSps_ = false;
};

}  // namespace pm::remote
