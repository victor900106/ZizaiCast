// Media Foundation H.264 / HEVC decoder MFT wrapper (synchronous MFT).
// Hardware path: DXVA via IMFDXGIDeviceManager -> output samples wrap
// ID3D11Texture2D (NV12, or P010 for 10-bit streams when the MFT offers no
// NV12).  Software path: system-memory NV12 / P010 samples.
#pragma once

#include <d3d11.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <string>

#include "pm/media.h"

namespace pm::video {

using Microsoft::WRL::ComPtr;

struct VideoFormat {
    UINT codedWidth = 0, codedHeight = 0;  // MF_MT_FRAME_SIZE (buffer size)
    RECT crop{};                           // visible area (display aperture)
    bool bt601 = false;                    // else BT.709
    bool fullRange = false;                // else 16-235 video range
    LONG stride = 0;                       // default stride for SW buffers
    bool tenBit = false;                   // P010 (only if the MFT offers no NV12)
    int visibleWidth() const { return crop.right - crop.left; }
    int visibleHeight() const { return crop.bottom - crop.top; }
};

class MfDecoder {
public:
    // Called for every decoded picture (on the decode thread, synchronously).
    using OutputFn = std::function<void(IMFSample* sample, const VideoFormat& fmt)>;

    MfDecoder() = default;
    ~MfDecoder();
    MfDecoder(const MfDecoder&) = delete;
    MfDecoder& operator=(const MfDecoder&) = delete;

    // dxgiManager may be null (software decode).  Returns false if no decoder
    // MFT for the codec is installed or it refuses the configuration.
    bool open(VideoCodec codec, IMFDXGIDeviceManager* dxgiManager);
    void close();
    bool isOpen() const { return mft_ != nullptr; }
    bool hardware() const { return hw_; }
    VideoCodec codec() const { return codec_; }
    const std::string& name() const { return name_; }

    // Feeds one access unit; emits 0..n pictures through out.  Returns a
    // failed HRESULT only on unrecoverable decoder errors.
    HRESULT decode(const uint8_t* au, size_t len, int64_t time100ns, const OutputFn& out);
    // Drops all internal state (pending pictures); next input should be a key AU.
    void flush();

private:
    HRESULT setOutputType();
    HRESULT drain(const OutputFn& out);

    ComPtr<IMFTransform> mft_;
    // Activation object of an enumerated MFT: ShutdownObject() on close, or
    // MFTs from activation objects (e.g. the HEVC Video Extensions) leak.
    ComPtr<IMFActivate> activate_;
    // Software mode: pending output sample.  A fresh one is allocated after
    // every delivered picture (the MS decoder keeps references to them).
    ComPtr<IMFSample> swSample_;
    VideoCodec codec_ = VideoCodec::H264;
    bool hw_ = false;
    bool providesSamples_ = false;
    DWORD outBufSize_ = 0;
    VideoFormat fmt_;
    std::string name_;
};

}  // namespace pm::video
