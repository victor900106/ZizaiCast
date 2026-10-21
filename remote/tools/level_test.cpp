// 0.8.3.2: the level of the PC's sound for the phone, measured OFFLINE on synthetic PCM
// (nothing is captured or played: no audio device is opened at all).
//   pm_level_test [out.csv]
// Every scenario is the mix as audio.cpp hands it to the level stage (after the volume
// compensation), in 20 ms frames, through
//   before  the 0.8.3 / 0.8.3.1 AGC + limiter (copied verbatim from audio.cpp at 341ed9e)
//   after   pm::remote::LevelChain (remote/src/level.h)
// then Opus (the same settings as audio.cpp) and back, and reports what the phone gets:
// RMS of the frames with sound (all / settled = after 10 s), peak, samples over 0.95 and
// at 1.0 (clipping), the quiet stretches' RMS (noise floor), the gain over time.
// Exit code 1 if an "after" target fails.
#include <opus.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "level.h"
#include "synth_audio.h"

namespace {

constexpr int kRate = 48000, kFrame = 960;
using pm::remote::synth::hashNoise;

// ---- 0.8.3 / 0.8.3.1, verbatim from audio.cpp (the AGC block and the soft limiter) ----
struct Legacy {
    float agcG = 1, agcLoud = 0, limGain = 1;
    bool agcOn = true;
    void process(std::vector<float>& mixed) {
        if (agcOn) {
            double sq = 0;
            for (float x : mixed) sq += double(x) * x;
            const float rms = float(std::sqrt(sq / double(mixed.size())));
            float want = 1.f;
            if (rms > 0.02f) {
                agcLoud = agcLoud > 0 ? agcLoud * 0.95f + rms * 0.05f : rms;
                want = std::clamp(0.125f / std::max(agcLoud, 1e-4f), 1.f, 4.f);
            } else {
                want = std::max(1.f, agcG * 0.996f);  // ~ -0.7 dB/s back to x1
            }
            const float from = agcG;
            agcG = want < agcG ? std::max(want, agcG * 0.9f) : std::min(want, agcG * 1.02f);  // down fast, up ~ +8.6 dB/s
            if (from != 1.f || agcG != 1.f)
                for (size_t k = 0; k < mixed.size(); ++k) mixed[k] *= from + (agcG - from) * float(k) / float(mixed.size());
        }
        float peak = 0;
        for (float x : mixed) peak = std::max(peak, std::fabs(x));
        const float target = peak * limGain > 0.9f ? 0.9f / peak : 1.f;
        const float from = limGain;
        limGain = target < limGain ? target : std::min(1.f, limGain + 0.02f);
        for (size_t k = 0; k < mixed.size(); ++k) {
            const float g = from + (limGain - from) * float(k) / float(mixed.size());
            float x = mixed[k] * g;
            if (std::fabs(x) > 0.95f) x = std::copysign(0.95f + 0.05f * std::tanh((std::fabs(x) - 0.95f) / 0.05f), x);
            mixed[k] = x;
        }
    }
    float gain() const { return agcG; }
};

struct Scenario {
    std::string name, what;
    double seconds;
    std::function<void(int i, float& l, float& r)> gen;  // the mix at sample i (48 kHz)
    double quietFrom = -1, quietTo = -1;                 // a stretch with no material (noise floor only)
};

double db(double x) { return x > 0 ? 20 * std::log10(x) : -120; }

struct Result {
    double inPeak = 0, inRms = 0, outRmsAll = 0, outRmsSettled = 0, outPeak = 0, decPeak = 0, decRmsSettled = 0;
    double quietIn = 0, quietOut = 0;
    long over95 = 0, clip = 0, decClip = 0;
    std::vector<float> gainDb;  // every 100 ms
    std::vector<float> outRms1s;
};

template <class Chain>
Result run(const Scenario& sc, Chain& chain, const std::function<float()>& gainOf) {
    Result r;
    const int frames = int(sc.seconds * kRate) / kFrame;
    std::vector<float> buf(size_t(kFrame) * 2), dec(size_t(kFrame) * 2);
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(kRate, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(128000));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(6));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    OpusDecoder* de = opus_decoder_create(kRate, 2, &err);
    std::vector<unsigned char> pkt(4000);
    double inSq = 0, outSq = 0, outSqSet = 0, decSqSet = 0, qIn = 0, qOut = 0, win = 0;
    long nIn = 0, nSet = 0, nQ = 0, nWin = 0;
    for (int f = 0; f < frames; ++f) {
        const double t = double(f) * kFrame / kRate;
        for (int k = 0; k < kFrame; ++k) sc.gen(f * kFrame + k, buf[size_t(k) * 2], buf[size_t(k) * 2 + 1]);
        double sq = 0;
        for (float x : buf) sq += double(x) * x, r.inPeak = std::max(r.inPeak, double(std::fabs(x)));
        const double frameMs = sq / double(buf.size());
        const bool sound = 10 * std::log10(frameMs + 1e-20) >= -55;  // a frame with material in it
        const bool quiet = t >= sc.quietFrom && t < sc.quietTo;
        chain.process(buf);
        double osq = 0;
        for (float x : buf) {
            osq += double(x) * x;
            r.outPeak = std::max(r.outPeak, double(std::fabs(x)));
            if (std::fabs(x) > 0.95f) ++r.over95;
            if (std::fabs(x) >= 0.9999f) ++r.clip;
        }
        const int len = opus_encode_float(enc, buf.data(), kFrame, pkt.data(), opus_int32(pkt.size()));
        double dsq = 0;
        if (len > 0 && opus_decode_float(de, pkt.data(), len, dec.data(), kFrame, 0) == kFrame)
            for (float x : dec) {
                dsq += double(x) * x;
                r.decPeak = std::max(r.decPeak, double(std::fabs(x)));
                if (std::fabs(x) >= 0.9999f) ++r.decClip;
            }
        if (sound && !quiet) {
            inSq += sq, outSq += osq, ++nIn;
            if (t >= 10) outSqSet += osq, decSqSet += dsq, ++nSet;
        }
        if (quiet && t >= sc.quietFrom + 1.0) qIn += sq, qOut += osq, ++nQ;  // from 1 s after the material stopped (the gate has closed)
        win += osq, ++nWin;
        if (nWin == 50) r.outRms1s.push_back(float(db(std::sqrt(win / (50.0 * buf.size()))))), win = 0, nWin = 0;
        if (f % 5 == 4) r.gainDb.push_back(float(db(gainOf())));
    }
    const double n2 = double(buf.size());
    r.inRms = nIn ? db(std::sqrt(inSq / (nIn * n2))) : -120;
    r.outRmsAll = nIn ? db(std::sqrt(outSq / (nIn * n2))) : -120;
    r.outRmsSettled = nSet ? db(std::sqrt(outSqSet / (nSet * n2))) : -120;
    r.decRmsSettled = nSet ? db(std::sqrt(decSqSet / (nSet * n2))) : -120;
    r.quietIn = nQ ? db(std::sqrt(qIn / (nQ * n2))) : -120;
    r.quietOut = nQ ? db(std::sqrt(qOut / (nQ * n2))) : -120;
    opus_encoder_destroy(enc);
    opus_decoder_destroy(de);
    return r;
}

struct LevelAdapter {
    pm::remote::LevelChain c;
    void process(std::vector<float>& b) { c.process(b.data(), b.size()); }
};

}  // namespace

int main(int argc, char** argv) {
    const std::vector<float> music = pm::remote::synth::musicLike(), speech = pm::remote::synth::speechLike();
    const size_t L = music.size();
    auto floorNoise = [](int i) { return 0.002f * hashNoise(uint32_t(i) * 747796405u + 1u); };  // -58.8 dBFS RMS hiss
    auto M = [&](int i, float peak) { return peak * music[size_t(i) % L]; };
    auto Sp = [&](int i, float peak) { return peak * speech[size_t(i) % L]; };
    std::vector<Scenario> sc = {
        {"quiet-music", "music, peak 0.03 (the owner's level after x1.86) + hiss -59 dBFS", 30,
         [&](int i, float& l, float& r) { l = r = M(i, 0.03f) + floorNoise(i); }},
        {"quiet-speech", "speech, peak 0.03 + hiss -59 dBFS", 30, [&](int i, float& l, float& r) { l = r = Sp(i, 0.03f) + floorNoise(i); }},
        {"owner-raw", "music peak 0.0161 on the device x1.86 compensation (= 0.03) + hiss", 30,
         [&](int i, float& l, float& r) { l = r = 1.86f * (M(i, 0.0161f) + floorNoise(i) / 1.86f); }},
        {"very-quiet-music", "music, peak 0.01 (-40 dBFS) + hiss", 30, [&](int i, float& l, float& r) { l = r = M(i, 0.01f) + floorNoise(i); }},
        {"normal-music", "music, peak 0.5 (an ordinary video)", 30, [&](int i, float& l, float& r) { l = r = M(i, 0.5f); }},
        {"loud-music", "music, peak 0.95 (mastered loud)", 30, [&](int i, float& l, float& r) { l = r = M(i, 0.95f); }},
        {"loud-mix", "two devices, music 0.9 + speech 0.9 (the sum is over 1)", 30,
         [&](int i, float& l, float& r) { l = r = M(i, 0.9f) + Sp(i, 0.9f); }},
        {"pause", "quiet music 10 s, hiss only 10 s (10-20 s), quiet music 10 s", 30,
         [&](int i, float& l, float& r) {
             const double t = double(i) / kRate;
             l = r = (t >= 10 && t < 20 ? 0.f : M(i, 0.03f)) + floorNoise(i);
         },
         10, 20},
        {"hdmi-noise", "a steady noise floor only, peak 0.02 (RMS -39 dBFS, like an HDMI device)", 20,
         [&](int i, float& l, float& r) { l = r = 0.02f * hashNoise(uint32_t(i) * 2891336453u + 7u); }, 0, 20},
        {"quiet-then-loud", "quiet music 12 s, then the same music at peak 0.95 (18 s)", 30,
         [&](int i, float& l, float& r) { l = r = M(i, double(i) / kRate < 12 ? 0.03f : 0.95f); }},
        {"loud-then-quiet", "loud music (peak 0.95) 12 s, then quiet music (peak 0.03, another video) 18 s", 30,
         [&](int i, float& l, float& r) { l = r = M(i, double(i) / kRate < 12 ? 0.95f : 0.03f) + floorNoise(i); }},
        {"old-music", "0.8.3's synthetic \"music\": 440 Hz 0.5, 3 s on / 3 s off, + a steady noise floor 0.02 (HDMI)", 30,
         [&](int i, float& l, float& r) {
             const double t = double(i) / kRate;
             l = r = (std::fmod(t, 6.0) < 3.0 ? float(0.5 * std::sin(2 * 3.14159265358979 * 440 * t)) : 0.f) +
                     0.02f * hashNoise(uint32_t(i) * 2891336453u + 7u);
         }},
        {"old-quiet-sine", "0.8.3's synthetic \"quiet\": 440 Hz 0.03, 3 s on / 3 s off, + hiss 0.002", 30,
         [&](int i, float& l, float& r) {
             const double t = double(i) / kRate;
             l = r = (std::fmod(t, 6.0) < 3.0 ? float(0.03 * std::sin(2 * 3.14159265358979 * 440 * t)) : 0.f) + floorNoise(i);
         }},
    };
    FILE* csv = argc > 1 ? std::fopen(argv[1], "w") : nullptr;
    if (csv) std::fprintf(csv, "scenario,chain,t_s,agc_gain_db\n");
    int fails = 0;
    std::printf("%-17s %-6s | %8s %8s | %9s %9s %8s %7s %6s %5s | %9s %8s | %8s %8s | gain dB at 1,2,3,5,10,20,end s\n", "scenario", "chain",
                "in peak", "in RMS", "out RMS", "settled", "peak", ">0.95", "clip", "", "opus RMS", "peak", "quiet in", "out");
    for (const Scenario& s : sc) {
        Legacy lg;
        LevelAdapter la;
        const Result b = run(s, lg, [&] { return lg.gain(); });
        const Result a = run(s, la, [&] { return la.c.gain(); });
        for (int k = 0; k < 2; ++k) {
            const Result& r = k ? a : b;
            auto g = [&](double sec) {
                const size_t i = std::min(r.gainDb.size() - 1, size_t(sec * 10) - 1);
                return r.gainDb[i];
            };
            std::printf("%-17s %-6s | %8.4f %7.1f  | %8.1f  %8.1f  %8.4f %7ld %6ld %5s | %8.1f  %8.4f | %8.1f %8.1f | %+.1f %+.1f %+.1f %+.1f %+.1f %+.1f %+.1f\n",
                        s.name.c_str(), k ? "after" : "before", r.inPeak, r.inRms, r.outRmsAll, r.outRmsSettled, r.outPeak, r.over95, r.clip, "",
                        r.decRmsSettled, r.decPeak, r.quietIn, r.quietOut, g(1), g(2), g(3), g(5), g(10), g(std::min(20.0, s.seconds)), r.gainDb.back());
            if (csv)
                for (size_t i = 0; i < r.gainDb.size(); ++i) std::fprintf(csv, "%s,%s,%.1f,%.2f\n", s.name.c_str(), k ? "after" : "before", (i + 1) * 0.1, r.gainDb[i]);
        }
        std::printf("%-17s   %s\n", "", s.what.c_str());
        // Targets (after): ordinary material lands at -16..-18 dBFS RMS (settled), no clipping anywhere,
        // the noise floor alone is not raised (> 1 dB), very loud material is not boosted.
        auto check = [&](bool ok, const char* what) {
            if (!ok) ++fails, std::printf("  FAIL %s: %s\n", s.name.c_str(), what);
        };
        check(a.clip == 0 && a.decClip == 0, "no sample at 1.0 (before or after Opus)");
        if (s.name == "quiet-music" || s.name == "quiet-speech" || s.name == "owner-raw" || s.name == "normal-music" || s.name == "loud-music")
            check(a.outRmsSettled >= -18.5 && a.outRmsSettled <= -15.5, "settled RMS within -16..-18 dBFS (+-0.5)");
        if (s.name == "pause" || s.name == "hdmi-noise") check(a.quietOut <= a.quietIn + 1.0, "the noise floor is not raised");
        if (s.name == "very-quiet-music") check(a.gainDb.back() >= 29.5, "the full +30 dB for peak 0.01 material");
        const float maxGain = *std::max_element(a.gainDb.begin(), a.gainDb.end());
        if (s.name == "hdmi-noise" || s.name == "old-music") check(maxGain <= 0.5f, "a steady noise floor never raises the gain");
        if (s.name == "loud-music" || s.name == "loud-mix" || s.name == "normal-music") check(a.outPeak <= 0.8901, "peak at or under the -1 dBFS ceiling");
    }
    if (csv) std::fclose(csv);
    std::printf(fails ? "\n%d target(s) FAILED\n" : "\nall targets met\n", fails);
    return fails ? 1 : 0;
}
