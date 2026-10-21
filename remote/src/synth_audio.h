#pragma once
// 0.8.3.2 tests: silent synthetic material (nothing is ever played: these samples only go
// into the mix -> level -> Opus path, or into remote/tools/level_test.cpp).
//   musicLike   8 s loop, 120 bpm: a chord pad (4 chords, 3 harmonics), bass, kick and
//               hi-hat with per-beat envelopes; about 13 dB between peak and RMS, like music.
//   speechLike  8 s loop: syllables (glottal harmonics 100 - 190 Hz shaped by two moving
//               formants, a few noise fricatives), short gaps, phrase pauses; ~17 dB crest.
// Both are normalised to a peak of 1 (scaled by the caller), deterministic.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace pm::remote::synth {

constexpr int kRate = 48000;
constexpr double kPi = 3.14159265358979323846;

inline float hashNoise(uint32_t i) {  // deterministic white noise in [-1, 1)
    i ^= i >> 16, i *= 0x7feb352dU, i ^= i >> 15, i *= 0x846ca68bU, i ^= i >> 16;
    return float(i >> 8) / 8388608.f - 1.f;
}

inline void normalise(std::vector<float>& v) {
    float p = 0;
    for (float x : v) p = std::max(p, std::fabs(x));
    if (p > 0)
        for (float& x : v) x /= p;
}

inline std::vector<float> musicLike() {
    const int n = 8 * kRate;
    std::vector<float> v(size_t(n), 0.f);
    const double chords[4][3] = {{261.63, 329.63, 392.00}, {220.00, 261.63, 329.63}, {174.61, 220.00, 261.63}, {196.00, 246.94, 293.66}};
    const double roots[4] = {130.81, 110.00, 87.31, 98.00};
    for (int i = 0; i < n; ++i) {
        const double t = double(i) / kRate;
        const int chord = int(t / 2.0) % 4;
        const double tb = std::fmod(t, 0.5);           // time in the beat
        const double th = std::fmod(t + 0.25, 0.5);    // time since the off-beat
        const double att = std::min(1.0, tb / 0.01);
        const double envPad = att * (0.35 + 0.65 * std::exp(-tb / 0.3));
        double pad = 0;
        for (double f : chords[chord])
            for (int h = 1; h <= 3; ++h) pad += std::sin(2 * kPi * f * h * t + h) / (h * h);
        const double bass = std::sin(2 * kPi * roots[chord] * t) * att * std::exp(-tb / 0.4);
        const double kick = std::sin(2 * kPi * (55 + 90 * std::exp(-tb / 0.03)) * tb) * std::exp(-tb / 0.08);
        const double hat = hashNoise(uint32_t(i) * 2654435761u) * std::exp(-th / 0.015);
        v[size_t(i)] = float(0.22 * pad * envPad + 0.45 * bass + 0.55 * kick + 0.12 * hat);
    }
    normalise(v);
    return v;
}

inline std::vector<float> speechLike() {
    const int n = 8 * kRate;
    std::vector<float> v(size_t(n), 0.f);
    uint32_t seed = 20261021;
    auto rnd = [&] {  // [0, 1)
        seed = seed * 1664525u + 1013904223u;
        return double(seed >> 8) / 16777216.0;
    };
    int i = 0, inPhrase = 0;
    double phase = 0;
    while (i < n) {
        if (inPhrase >= 4 + int(rnd() * 3)) {  // phrase pause 400 - 700 ms
            i += int((0.4 + 0.3 * rnd()) * kRate), inPhrase = 0;
            continue;
        }
        ++inPhrase;
        const int len = int((0.12 + 0.13 * rnd()) * kRate);
        const double f0a = 100 + 90 * rnd(), f0b = 100 + 90 * rnd();
        const double f1 = 450 + 400 * rnd(), f2 = 1100 + 1000 * rnd();
        const double amp = 0.5 + 0.5 * rnd();
        const bool fricative = rnd() < 0.25;
        for (int k = 0; k < len && i < n; ++k, ++i) {
            const double u = double(k) / len;
            const double env = std::pow(std::sin(kPi * u), 2) * amp;
            const double f0 = f0a + (f0b - f0a) * u;
            phase += 2 * kPi * f0 / kRate;
            double s = 0;
            for (int h = 1; f0 * h < 3800; ++h) {
                const double fh = f0 * h;
                const double w = 1.0 / (1 + std::pow((fh - f1) / 120, 2)) + 0.6 / (1 + std::pow((fh - f2) / 180, 2)) + 0.02;
                s += w * std::sin(h * phase) / std::sqrt(double(h));
            }
            if (fricative && u < 0.3) s = s * 0.3 + 1.5 * hashNoise(uint32_t(i) * 2246822519u) * (0.3 - u) / 0.3;
            v[size_t(i)] = float(s * env);
        }
        i += int((0.04 + 0.04 * rnd()) * kRate);  // gap between syllables
    }
    normalise(v);
    return v;
}

}  // namespace pm::remote::synth
