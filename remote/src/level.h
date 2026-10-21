#pragma once
// 0.8.3.2: the level of the PC's sound for the phone, per 20 ms frame (48 kHz stereo,
// interleaved float), after the mix and the volume compensation, before Opus:
//
//   AGC (leveller)  the material's loudness (mean square of the frames above -55 dBFS RMS,
//                   followed up with a 0.8 s time constant, 0.1 s if 12 dB louder; down with
//                   3 s, 1.2 s if 10 dB quieter: another, quieter video) is brought towards
//                   -17 dBFS RMS: at most +30 dB, at least -6 dB; the gain moves at most
//                   +20 dB/s up and -60 dB/s down.
//   steady signals  louder material lowers the gain at once (always safe); what would raise
//                   it is judged 0.5 s late, with the 0.5 s before and after the frame known:
//                   a steady stretch on either side (within 3 dB: a hiss / hum / HDMI noise
//                   floor, a test tone, also just after a level change) teaches nothing, and
//                   while steady the gain drifts back to x1 at 1 dB/s; nothing is learnt in
//                   the first 1 s. So a noise floor alone is never raised.
//   noise gate      frames below -55 dBFS RMS (or 30 dB under the loudness) for longer
//                   than 200 ms are brought back to x1 at 60 dB/s (never above x1): the
//                   pauses between songs / sentences stay as quiet as the PC plays them.
//   limiter         the frame's own peak (a 20 ms look-ahead for free: the gain is chosen
//                   from the whole frame) keeps it under 0.89 (-1 dBFS); release 10 dB/s;
//                   a soft knee above 0.95 so no sample ever reaches 1.0.
//
// 0.8.3 / 0.8.3.1 had an AGC that only followed frames with RMS > 0.02 (-34 dBFS) and at
// most x4: real quiet material (peak 0.03, RMS 0.004 - 0.011: music and speech have
// 10 - 18 dB between peak and RMS) never got there, so it stayed at x1.00 (owner's log,
// 2026-10-21 10:16 - 10:20). The threshold was set above the old synthetic HDMI noise
// (RMS 0.012) and under the synthetic quiet SINE (RMS 0.021): a sine has 3 dB of crest.
// Header-only: remote/src/audio.cpp and the offline test remote/tools/level_test.cpp.

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pm::remote {

class LevelChain {
public:
    // Constants (dB unless said otherwise).
    static constexpr float kTargetDb = -17.f, kMaxGainDb = 30.f, kMinGainDb = -6.f;
    static constexpr float kBiasDb = 1.7f;  // the follower sits this much above the mean power (fast up, slow down)
    static constexpr float kGateDb = -55.f, kExpRelDb = -30.f;
    static constexpr float kSteadyDb = 3.f;
    static constexpr unsigned kHalf = 25, kRing = 2 * kHalf + 1;  // 0.5 s either side of the decision frame
    static constexpr int kHoldFrames = 10, kInitFrames = 15;      // 200 ms, 300 ms
    static constexpr float kUpDbPerS = 20.f, kDownDbPerS = 60.f, kDriftDbPerS = 1.f, kGateDbPerS = 60.f;
    static constexpr float kCeiling = 0.89f, kKnee = 0.95f, kLimReleaseDbPerS = 10.f;
    static constexpr float kFrameS = 0.02f;

    explicit LevelChain(bool agc = true) : agc_(agc) {}
    void setAgc(bool on) { agc_ = on; }

    // x: interleaved stereo, n samples (= 2 x frames). In place.
    void process(float* x, size_t n) {
        if (!n) return;
        double sq = 0;
        for (size_t k = 0; k < n; ++k) sq += double(x[k]) * x[k];
        const float ms = float(sq / double(n));
        const float db = 10.f * std::log10(ms + 1e-20f);
        float applied = 1.f;
        if (agc_) {
            const bool above = db >= kGateDb;
            // Louder material lowers the gain at once (always safe): 0.8 s, or 0.1 s if 12 dB louder.
            if (above && est_ > 0 && ms > est_) est_ += (ms > est_ * 16.f ? kFast : kUp) * (ms - est_);
            // Raising it is decided 0.5 s late, on the frame kHalf ago, when the 0.5 s before it and
            // the 0.5 s after it are known: a steady stretch on either side (a noise floor, a hum, a
            // test tone, also just after a level change) teaches nothing. Nothing in the first 1 s.
            ringDb_[ringIdx_ % kRing] = db, ringMs_[ringIdx_ % kRing] = ms, ++ringIdx_;
            if (ringN_ < kRing) ++ringN_;
            steady_ = false;
            if (ringN_ == kRing) {
                auto flat = [&](unsigned from) {  // kHalf + 1 frames from `from` (oldest first)
                    float lo = 1e9f, hi = -1e9f;
                    for (unsigned i = 0; i <= kHalf; ++i) {
                        const float v = ringDb_[(from + i) % kRing];
                        lo = std::min(lo, v), hi = std::max(hi, v);
                    }
                    return lo >= kGateDb && hi - lo < kSteadyDb;
                };
                const unsigned oldest = ringIdx_ - kRing, d = oldest + kHalf;  // the decision frame
                steady_ = flat(oldest) || flat(d);
                const float dms = ringMs_[d % kRing], ddb = ringDb_[d % kRing];
                if (ddb >= kGateDb && !steady_) {
                    if (est_ <= 0) {
                        initSum_ += dms;
                        if (++initN_ >= kInitFrames) est_ = float(initSum_ / initN_);
                    } else if (dms <= est_) {
                        // down: 3 s; more than 10 dB under it (another, quieter video) 1.2 s, also below the
                        // 20 dB relative gate (so a quieter video is followed, while a short pause is not)
                        est_ += (dms < est_ * 0.1f ? kQuieter : kDown) * (dms - est_);
                    }
                }
            }
            if (steady_) {
                if (g_ > 1.f) g_ = std::max(1.f, g_ * perFrame(-kDriftDbPerS));
            } else if (est_ > 0) {
                const float want = std::clamp(lin(kTargetDb + kBiasDb) / std::sqrt(est_), lin(kMinGainDb), lin(kMaxGainDb));
                g_ = want > g_ ? std::min(want, g_ * perFrame(kUpDbPerS)) : std::max(want, g_ * perFrame(-kDownDbPerS));
            }
            // Noise gate: the pauses go back to x1 (never louder than the PC plays them).
            const bool open = above && (est_ <= 0 || db >= dbOf(est_) + kExpRelDb);
            if (open) hold_ = kHoldFrames;
            const float floorE = g_ > 1.f ? 1.f / g_ : 1.f;
            if (open || hold_ > 0) {
                if (!open) --hold_;
                e_ = 1.f;
            } else {
                e_ = std::max(floorE, e_ * perFrame(-kGateDbPerS));
            }
            gateOpen_ = open || hold_ > 0;
            applied = g_ * e_;
        }
        // Gain (ramped over the frame), then the limiter on the frame's own peak.
        float peak = 0;
        for (size_t k = 0; k < n; ++k) {
            const float g = prev_ + (applied - prev_) * float(k + 1) / float(n);
            x[k] *= g;
            peak = std::max(peak, std::fabs(x[k]));
        }
        prev_ = applied;
        const float target = peak > kCeiling ? kCeiling / peak : 1.f;  // what this frame allows (never above the ceiling)
        const float from = lim_;
        lim_ = target < lim_ ? target : std::min(target, lim_ * perFrame(kLimReleaseDbPerS));
        for (size_t k = 0; k < n; ++k) {  // down: the whole frame at once (its peak is known); up: ramped
            const float g = lim_ < from ? lim_ : from + (lim_ - from) * float(k + 1) / float(n);
            float y = x[k] * g;
            const float a = std::fabs(y);
            if (a > kKnee) y = std::copysign(kKnee + (1.f - kKnee) * std::tanh((a - kKnee) / (1.f - kKnee)), y);
            x[k] = y;
        }
    }

    float gain() const { return g_; }               // the AGC's gain (1 = none)
    float applied() const { return prev_; }         // AGC x noise gate, last frame
    float limiter() const { return lim_; }          // the limiter's gain (1 = not limiting)
    bool gateOpen() const { return gateOpen_; }
    bool steady() const { return steady_; }
    float loudnessDb() const { return est_ > 0 ? dbOf(est_) : -120.f; }  // the material's level it is following

private:
    static float lin(float db) { return std::pow(10.f, db / 20.f); }
    static float dbOf(float ms) { return 10.f * std::log10(ms + 1e-20f); }
    static float perFrame(float dbPerS) { return std::pow(10.f, dbPerS * kFrameS / 20.f); }
    static constexpr float kUp = 0.0247f;    // 1 - exp(-20 ms / 0.8 s)
    static constexpr float kDown = 0.00664f;  // 1 - exp(-20 ms / 3 s)
    static constexpr float kQuieter = 0.0165f;  // 1 - exp(-20 ms / 1.2 s): more than 10 dB under it
    static constexpr float kFast = 0.181f;    // 1 - exp(-20 ms / 0.1 s)

    bool agc_ = true;
    float est_ = 0;  // mean square being followed (0 = not yet)
    double initSum_ = 0;
    int initN_ = 0;
    float ringDb_[kRing] = {}, ringMs_[kRing] = {};
    unsigned ringN_ = 0, ringIdx_ = 0;
    bool steady_ = false, gateOpen_ = false;
    int hold_ = 0;
    float g_ = 1.f, e_ = 1.f, prev_ = 1.f, lim_ = 1.f;
};

}  // namespace pm::remote
