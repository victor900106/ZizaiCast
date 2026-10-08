#include "audio_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace pm::audio {

namespace {

int samplingFrequencyIndex(int rate) {
    static const int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                 22050, 16000, 12000, 11025, 8000,  7350};
    for (int i = 0; i < 13; ++i)
        if (kRates[i] == rate) return i;
    return -1;
}

struct BitWriter {
    std::vector<uint8_t> bytes;
    int bitPos = 0;
    void put(uint32_t value, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            if (bitPos % 8 == 0) bytes.push_back(0);
            if ((value >> i) & 1) bytes.back() |= uint8_t(0x80 >> (bitPos % 8));
            ++bitPos;
        }
    }
};

}  // namespace

// Copies `frames` frames of `srcCh`-channel PCM to `dstCh` channels.
void remixAppend(const int16_t* src, int frames, int srcCh, int dstCh, std::vector<int16_t>& out) {
    size_t base = out.size();
    out.resize(base + size_t(frames) * dstCh);
    int16_t* dst = out.data() + base;
    if (srcCh == dstCh) {
        std::memcpy(dst, src, sizeof(int16_t) * size_t(frames) * dstCh);
        return;
    }
    for (int f = 0; f < frames; ++f) {
        for (int c = 0; c < dstCh; ++c) {
            dst[f * dstCh + c] = src[f * srcCh + std::min(c, srcCh - 1)];
        }
    }
}

namespace {

// AAC-LC / AAC-ELD via FFmpeg's native (LGPL) "aac" decoder. The decoder is
// opened with the same AudioSpecificConfig as extradata and fed one raw
// access unit per AVPacket; its float-planar output is converted to
// interleaved S16. It emits one frame per AU (480 or 1024 samples), first AU
// included, like fdk-aac did: same length and codec delay (ELD identical;
// LC 661 samples earlier than fdk, whose default PCM limiter delayed LC).
class FfmpegAacDecoder final : public AudioDecoder {
public:
    FfmpegAacDecoder(int rate, int ch) : rate_(rate), ch_(ch) {}
    ~FfmpegAacDecoder() override {
        av_frame_free(&frame_);
        av_packet_free(&pkt_);
        avcodec_free_context(&ctx_);
    }

    bool init(const std::vector<uint8_t>& asc, std::string* err) {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
        if (!codec) return fail(err, "FFmpeg AAC decoder not available");
        ctx_ = avcodec_alloc_context3(codec);
        pkt_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        if (!ctx_ || !pkt_ || !frame_) return fail(err, "FFmpeg allocation failed");
        ctx_->extradata = static_cast<uint8_t*>(av_mallocz(asc.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!ctx_->extradata) return fail(err, "FFmpeg allocation failed");
        std::memcpy(ctx_->extradata, asc.data(), asc.size());
        ctx_->extradata_size = int(asc.size());
        ctx_->sample_rate = rate_;
        av_channel_layout_default(&ctx_->ch_layout, ch_);
        ctx_->thread_count = 1;
        int e = avcodec_open2(ctx_, codec, nullptr);
        if (e < 0) return fail(err, "avcodec_open2(aac) failed: " + avErr(e));
        return true;
    }

    int decode(const uint8_t* data, size_t len, std::vector<int16_t>& out) override {
        // AirPlay AAC-LC has (per UxPlay comments) possibly been seen with
        // ADTS headers; strip one if present since the decoder is in raw mode.
        if (len >= 7 && data[0] == 0xFF && (data[1] & 0xF0) == 0xF0) {
            size_t hdr = (data[1] & 0x01) ? 7 : 9;
            if (len <= hdr) return -1;
            data += hdr;
            len -= hdr;
        }
        // Padded copy: libavcodec may read up to AV_INPUT_BUFFER_PADDING_SIZE
        // bytes past the end of the packet.
        if (in_.size() < len + AV_INPUT_BUFFER_PADDING_SIZE) in_.resize(len + AV_INPUT_BUFFER_PADDING_SIZE);
        std::memcpy(in_.data(), data, len);
        std::memset(in_.data() + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        pkt_->data = in_.data();
        pkt_->size = int(len);
        int e = avcodec_send_packet(ctx_, pkt_);
        pkt_->data = nullptr;
        pkt_->size = 0;
        if (e < 0) {
            drain();
            return -1;
        }
        int total = 0;
        bool bad = false;
        for (;;) {
            e = avcodec_receive_frame(ctx_, frame_);
            if (e == AVERROR(EAGAIN) || e == AVERROR_EOF) break;
            if (e < 0) {
                bad = true;
                break;
            }
            int n = appendFrame(out);
            av_frame_unref(frame_);
            if (n < 0) bad = true;
            else total += n;
        }
        return bad && total == 0 ? -1 : total;
    }

    int channels() const override { return ch_; }
    int sampleRate() const override { return rate_; }

private:
    static std::string avErr(int e) {
        char b[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(e, b, sizeof b);
        return b;
    }
    static bool fail(std::string* err, std::string msg) {
        if (err) *err = std::move(msg);
        return false;
    }
    void drain() {  // drop anything still queued after an error
        while (avcodec_receive_frame(ctx_, frame_) >= 0) av_frame_unref(frame_);
    }
    static int16_t toS16(float x) {
        float v = x * 32768.0f;
        if (!(v > -32768.0f)) return -32768;  // also NaN
        if (v >= 32767.0f) return 32767;
        return int16_t(std::lrintf(v));
    }
    // Appends `frame_` as interleaved S16 with exactly ch_ channels
    // (missing channels repeat the last decoded one, extra ones are dropped).
    int appendFrame(std::vector<int16_t>& out) {
        const int frames = frame_->nb_samples;
        const int srcCh = frame_->ch_layout.nb_channels;
        if (frames <= 0) return 0;
        if (srcCh <= 0) return -1;
        const auto fmt = AVSampleFormat(frame_->format);
        size_t base = out.size();
        out.resize(base + size_t(frames) * ch_);
        int16_t* dst = out.data() + base;
        for (int c = 0; c < ch_; ++c) {
            const int sc = std::min(c, srcCh - 1);
            int16_t* d = dst + c;
            switch (fmt) {
                case AV_SAMPLE_FMT_FLTP: {
                    const float* s = reinterpret_cast<const float*>(frame_->extended_data[sc]);
                    for (int f = 0; f < frames; ++f) d[size_t(f) * ch_] = toS16(s[f]);
                    break;
                }
                case AV_SAMPLE_FMT_FLT: {
                    const float* s = reinterpret_cast<const float*>(frame_->extended_data[0]) + sc;
                    for (int f = 0; f < frames; ++f) d[size_t(f) * ch_] = toS16(s[size_t(f) * srcCh]);
                    break;
                }
                case AV_SAMPLE_FMT_S16P: {
                    const int16_t* s = reinterpret_cast<const int16_t*>(frame_->extended_data[sc]);
                    for (int f = 0; f < frames; ++f) d[size_t(f) * ch_] = s[f];
                    break;
                }
                case AV_SAMPLE_FMT_S16: {
                    const int16_t* s = reinterpret_cast<const int16_t*>(frame_->extended_data[0]) + sc;
                    for (int f = 0; f < frames; ++f) d[size_t(f) * ch_] = s[size_t(f) * srcCh];
                    break;
                }
                default:
                    out.resize(base);
                    return -1;
            }
        }
        return frames;
    }

    int rate_, ch_;
    AVCodecContext* ctx_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frame_ = nullptr;
    std::vector<uint8_t> in_;
};

}  // namespace

std::string aacDecoderInfo() {
    const unsigned v = avcodec_version();
    char buf[256];
    std::snprintf(buf, sizeof buf, "FFmpeg libavcodec %u.%u.%u (%s), avcodec license: %s, avutil license: %s",
                  v >> 16, (v >> 8) & 0xFF, v & 0xFF, av_version_info(), avcodec_license(), avutil_license());
    return buf;
}

bool aacDecoderIsLgpl() {
    return std::strcmp(avcodec_license(), "LGPL version 2.1 or later") == 0 &&
           std::strcmp(avutil_license(), "LGPL version 2.1 or later") == 0;
}

std::vector<uint8_t> makeAacAsc(AudioCodec codec, int sampleRate, int channels, int samplesPerFrame) {
    BitWriter w;
    int sfi = samplingFrequencyIndex(sampleRate);
    auto putAot = [&](int aot) {
        if (aot < 31) {
            w.put(aot, 5);
        } else {
            w.put(31, 5);
            w.put(aot - 32, 6);
        }
    };
    auto putFreq = [&] {
        if (sfi >= 0) {
            w.put(sfi, 4);
        } else {
            w.put(15, 4);
            w.put(uint32_t(sampleRate), 24);
        }
    };
    if (codec == AudioCodec::AAC_ELD) {
        putAot(39);
        putFreq();
        w.put(channels, 4);
        // ELDSpecificConfig
        w.put(samplesPerFrame == 480 ? 1 : 0, 1);  // frameLengthFlag (480 vs 512)
        w.put(0, 1);  // aacSectionDataResilienceFlag
        w.put(0, 1);  // aacScalefactorDataResilienceFlag
        w.put(0, 1);  // aacSpectralDataResilienceFlag
        w.put(0, 1);  // ldSbrPresentFlag
        w.put(0, 4);  // ELDEXT_TERM
    } else {
        putAot(2);  // AAC-LC
        putFreq();
        w.put(channels, 4);
        // GASpecificConfig
        w.put(samplesPerFrame == 960 ? 1 : 0, 1);  // frameLengthFlag
        w.put(0, 1);                               // dependsOnCoreCoder
        w.put(0, 1);                               // extensionFlag
    }
    return w.bytes;
}

std::vector<uint8_t> makeAlacCookie(int sampleRate, int channels, int samplesPerFrame) {
    auto be32 = [](std::vector<uint8_t>& v, uint32_t x) {
        for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s));
    };
    std::vector<uint8_t> c;
    be32(c, uint32_t(samplesPerFrame > 0 ? samplesPerFrame : 352));  // frameLength
    c.push_back(0);                                                   // compatibleVersion
    c.push_back(16);                                                  // bitDepth
    c.push_back(40);                                                  // pb
    c.push_back(10);                                                  // mb
    c.push_back(14);                                                  // kb
    c.push_back(uint8_t(channels));                                   // numChannels
    c.push_back(0);
    c.push_back(255);                // maxRun
    be32(c, 0);                      // maxFrameBytes
    be32(c, 0);                      // avgBitRate
    be32(c, uint32_t(sampleRate));   // sampleRate
    return c;
}

std::unique_ptr<AudioDecoder> createDecoder(AudioCodec codec, int sampleRate, int channels,
                                            int samplesPerFrame, std::string* err) {
    if (channels < 1 || channels > 8 || sampleRate < 8000 || sampleRate > 192000) {
        if (err) *err = "unsupported audio format";
        return nullptr;
    }
    switch (codec) {
        case AudioCodec::AAC_ELD:
        case AudioCodec::AAC_LC: {
            static std::once_flag logOnce;
            std::call_once(logOnce, [] { av_log_set_level(AV_LOG_ERROR); });
            auto d = std::make_unique<FfmpegAacDecoder>(sampleRate, channels);
            if (!d->init(makeAacAsc(codec, sampleRate, channels, samplesPerFrame), err)) return nullptr;
            return d;
        }
        case AudioCodec::ALAC: {
#if PM_HAVE_ALAC
            return createAlacDecoder(sampleRate, channels, makeAlacCookie(sampleRate, channels, samplesPerFrame),
                                     err);
#else
            // TODO(audio): build with vcpkg port `alac` to enable ALAC (ct=2).
            if (err) *err = "ALAC support not compiled in (vcpkg port 'alac' not found)";
            return nullptr;
#endif
        }
    }
    if (err) *err = "unknown codec";
    return nullptr;
}

}  // namespace pm::audio
