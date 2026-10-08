// Apple ALAC decoder (vcpkg port `alac`). Separate TU: Apple's
// ALACBitUtilities.h and fdk-aac's FDK_audio.h both define ID_SCE etc.
#include "audio_decoder.h"

#include <ALACBitUtilities.h>
#include <ALACDecoder.h>

#include <algorithm>

namespace pm::audio {

namespace {

class AppleAlacDecoder final : public AudioDecoder {
public:
    AppleAlacDecoder(int rate, int ch) : rate_(rate), ch_(ch) {}

    bool init(std::vector<uint8_t> cookie, std::string* err) {
        cookie_ = std::move(cookie);
        if (dec_.Init(cookie_.data(), uint32_t(cookie_.size())) != 0) {
            if (err) *err = "ALACDecoder::Init failed";
            return false;
        }
        decCh_ = std::max<int>(1, dec_.mConfig.numChannels);
        pcm_.resize(size_t(dec_.mConfig.frameLength) * decCh_ * 2);
        return true;
    }

    int decode(const uint8_t* data, size_t len, std::vector<int16_t>& out) override {
        // Apple's BitBuffer reads a little past the end; pad the copy.
        in_.assign(data, data + len);
        in_.resize(len + 16, 0);
        BitBuffer bb;
        BitBufferInit(&bb, in_.data(), uint32_t(len));
        uint32_t got = 0;
        int32_t st = dec_.Decode(&bb, reinterpret_cast<uint8_t*>(pcm_.data()),
                                 dec_.mConfig.frameLength, decCh_, &got);
        if (st != 0 || got == 0) return -1;
        remixAppend(pcm_.data(), int(got), decCh_, ch_, out);
        return int(got);
    }

    int channels() const override { return ch_; }
    int sampleRate() const override { return rate_; }

private:
    ALACDecoder dec_;
    std::vector<uint8_t> cookie_, in_;
    std::vector<int16_t> pcm_;
    int rate_, ch_, decCh_ = 2;
};

}  // namespace

std::unique_ptr<AudioDecoder> createAlacDecoder(int sampleRate, int channels, std::vector<uint8_t> cookie,
                                                std::string* err) {
    auto d = std::make_unique<AppleAlacDecoder>(sampleRate, channels);
    if (!d->init(std::move(cookie), err)) return nullptr;
    return d;
}

}  // namespace pm::audio
