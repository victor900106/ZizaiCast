// Internal: compressed AirPlay audio frame -> interleaved int16 PCM.
#pragma once

#include <pm/media.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pm::audio {

class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;
    // Decodes one access unit and appends interleaved S16 PCM (exactly
    // `channels()` channels) to `out`. Returns sample frames appended, or -1.
    virtual int decode(const uint8_t* data, size_t len, std::vector<int16_t>& out) = 0;
    virtual int channels() const = 0;
    virtual int sampleRate() const = 0;
};

// Returns nullptr (and sets *err) if the codec/config is unsupported.
std::unique_ptr<AudioDecoder> createDecoder(AudioCodec codec, int sampleRate, int channels,
                                            int samplesPerFrame, std::string* err);

// AAC backend (FFmpeg libavcodec, LGPL build) version + avcodec_license() /
// avutil_license(), for a one-time log line. aacDecoderIsLgpl() is true iff
// both report "LGPL version 2.1 or later".
std::string aacDecoderInfo();
bool aacDecoderIsLgpl();

// MPEG-4 AudioSpecificConfig AirPlay implies for raw AAC access units.
// AAC-ELD 44100/2/480 -> f8 e8 50 00 ; AAC-LC 44100/2 -> 12 10.
std::vector<uint8_t> makeAacAsc(AudioCodec codec, int sampleRate, int channels, int samplesPerFrame);

// 24-byte ALACSpecificConfig ("magic cookie") AirPlay implies for ct=2
// (same values UxPlay / shairport-sync use: 352 spf, 16 bit, 40/10/14, 255).
std::vector<uint8_t> makeAlacCookie(int sampleRate, int channels, int samplesPerFrame);

// Internal helpers.
void remixAppend(const int16_t* src, int frames, int srcCh, int dstCh, std::vector<int16_t>& out);
#if PM_HAVE_ALAC
std::unique_ptr<AudioDecoder> createAlacDecoder(int sampleRate, int channels, std::vector<uint8_t> cookie,
                                                std::string* err);
#endif

}  // namespace pm::audio
