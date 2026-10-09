// Remembered phone volume (volume.txt) for the 自在投影 app: the AirPlay sink
// wrapper that remembers it, the Android sink wrapper that applies it, the
// app's mute and the 0..100 % scale of the volume slider (pm::vol).
// Header-only so pm_volume_test can exercise it without the app.
#pragma once

#include <pm/media.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace fs = std::filesystem;

// One PC playback level for every source (AirPlay, Android, Miracast), in
// AirPlay dB: -30..0 (volume.txt), -144 = mute.
//
// Slider / toast / menu percentage: linear in dB over -30..0, i.e. the same
// scale as the iPhone's own volume slider (its 16 button steps are 1.875 dB
// = 6.25 % each: -28.125, -26.25, ... 0; its bottom sends -144):
//     percent = (dB + 30) / 30 * 100        dB = -30 + 30 * percent / 100
// Equal steps sound about equally loud (loudness follows dB), so this is the
// perceptual mapping; 50 % = -15 dB (the default level) = gain 0.18.
// 0 % (-30 dB, below the phone's lowest step) is silent: played as -144, not
// as 10^(-30/20) = 3 %.  Played gain otherwise 10^(dB/20)
// (AudioPlayer::airplayDbToGain).  Ctrl+↑ / Ctrl+↓ and the wheel move one
// phone step, snapped to the phone's grid.
namespace pm::vol {
constexpr float kMinDb = -30.0f, kMaxDb = 0.0f, kStepDb = 1.875f, kMuteDb = -144.0f;
constexpr int kSteps = 16;
inline float clampDb(float db) { return std::isnan(db) ? kMinDb : std::clamp(db, kMinDb, kMaxDb); }
inline float fractionOf(float db) { return (clampDb(db) - kMinDb) / (kMaxDb - kMinDb); }
inline float dbOfFraction(float f) { return kMinDb + (kMaxDb - kMinDb) * std::clamp(std::isnan(f) ? 0.f : f, 0.f, 1.f); }
inline int percentOf(float db) { return static_cast<int>(std::lround(fractionOf(db) * 100.0f)); }
inline float dbOfPercent(int p) { return dbOfFraction(std::clamp(p, 0, 100) / 100.0f); }
inline bool silent(float db) { return !(db > kMinDb + 0.001f); }
// `steps` phone steps up (+) / down (-) from db, onto the phone's grid (an
// off-grid level goes to the next grid point in that direction first).
inline float stepDb(float db, int steps) {
    const float k = (clampDb(db) - kMinDb) / kStepDb;
    int g = steps > 0 ? static_cast<int>(std::floor(k + 0.01f)) + steps
                      : static_cast<int>(std::ceil(k - 0.01f)) + steps;
    g = std::clamp(g, 0, kSteps);
    return kMinDb + g * kStepDb;
}
// What the player gets: -144 when muted or at 0 %, else the level.
inline float playDb(float db, bool muted) { return muted || silent(db) ? kMuteDb : clampDb(db); }
inline float gainOf(float db, bool muted) { return muted || silent(db) ? 0.0f : std::pow(10.0f, clampDb(db) / 20.0f); }
}  // namespace pm::vol

// Forwards to the real player and remembers the phone volume so the next
// connection starts where the user left it instead of at full volume.
// saved() is the remembered level (in memory, always current); volume.txt is
// written by a small writer thread, debounced (the phone sends a message per
// volume step, on the RTP thread), and flushed on exit.  Mute (-144) and
// out-of-range values are applied to the player but never remembered.
//
// The same level is the app's volume slider (setLevel, UI thread): one
// remembered value whether the phone's buttons or the app moved it.  The
// app's mute (setMuted; settings.ini) is a separate flag: it silences every
// source without touching the level, unmuting restores it.  A phone mute
// (-144: the iPhone at its bottom) silences this AirPlay session only
// (phoneMuted, cleared by the next level or newSession()).
//   onSavedChange (any thread): the remembered level changed (phone or app).
//   onSharedChange (any thread): sharedDb() changed (level or app mute) -
//     Android / Miracast follow it.
//   onPhoneChange (phone thread): the phone changed the level or its mute -
//     the app's slider follows.
class RememberVolumeAudioSink final : public pm::AudioSink {
public:
    RememberVolumeAudioSink(pm::AudioSink& inner, fs::path file, float fallback, bool muted = false)
        : inner_(inner), file_(std::move(file)), saved_(load(file_, fallback)), muted_(muted), written_(saved_.load()) {
        writer_ = std::thread([this] { writerMain(); });
    }
    ~RememberVolumeAudioSink() override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        writer_.join();  // writes a pending value first
    }
    static bool valid(float db) { return db >= -30.0f && db <= 0.0f; }
    static float load(const fs::path& file, float fallback) {
        std::ifstream in(file);
        float db = fallback;
        if (in >> db && valid(db)) return db;
        return fallback;
    }
    float saved() const { return saved_.load(); }
    bool muted() const { return muted_.load(); }
    bool phoneMuted() const { return phoneMuted_.load(); }
    // Level for AirPlay (app mute, phone mute and 0 % -> -144).
    float playDb() const { return pm::vol::playDb(saved_.load(), muted_.load() || phoneMuted_.load()); }
    // Level for Android / Miracast (no phone mute of their own).
    float sharedDb() const { return pm::vol::playDb(saved_.load(), muted_.load()); }
    // Milliseconds since newSession() (a phone connecting).
    long long msSinceSession() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
                   .count() -
               sessionAt_.load();
    }
    std::function<void(float db)> onSavedChange;
    std::function<void(float db)> onSharedChange;
    std::function<void()> onPhoneChange;

    // App slider / keys / menu: a new level (clamped to -30..0); unmutes.
    void setLevel(float db) {
        db = pm::vol::clampDb(db);
        muted_ = false;
        phoneMuted_ = false;
        remember(db);
        inner_.onVolume(playDb());
        if (onSharedChange) onSharedChange(sharedDb());
    }
    void setMuted(bool m) {
        muted_ = m;
        if (!m) phoneMuted_ = false;  // unmute = sound again, whatever silenced it
        inner_.onVolume(playDb());
        if (onSharedChange) onSharedChange(sharedDb());
    }
    // A new phone session (AirPlay connecting, an Android stream): a phone
    // mute from the last one does not carry over.
    void newSession() {
        phoneMuted_ = false;
        sessionAt_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    }

    void onFormat(pm::AudioCodec c, int rate, int ch, int spf) override { inner_.onFormat(c, rate, ch, spf); }
    void onPacket(const uint8_t* d, size_t n, uint64_t t) override { inner_.onPacket(d, n, t); }
    void onFlush() override { inner_.onFlush(); }
    void onVolume(float db) override {  // from the phone (and AirPlayServer::start's initial level)
        if (!valid(db)) {
            if (db < pm::vol::kMinDb) {  // phone mute (-144): this session only, not remembered
                const bool was = phoneMuted_.exchange(true);
                inner_.onVolume(pm::vol::kMuteDb);
                if (!was && onPhoneChange) onPhoneChange();
            } else {
                inner_.onVolume(muted_.load() ? pm::vol::kMuteDb : db);  // out of range: passed on, not kept
            }
            return;
        }
        const bool wasMuted = phoneMuted_.exchange(false);
        const bool changed = db != saved_.load();
        if (changed) remember(db);
        inner_.onVolume(playDb());
        if (changed && onSharedChange) onSharedChange(sharedDb());
        if ((changed || wasMuted) && onPhoneChange) onPhoneChange();
    }

private:
    void remember(float db) {
        if (db == saved_.load()) return;
        saved_ = db;
        {
            std::lock_guard<std::mutex> lk(mu_);
            dirtyAt_ = std::chrono::steady_clock::now();
            dirty_ = true;
        }
        cv_.notify_all();
        if (onSavedChange) onSavedChange(db);
    }
    void writerMain() {
        std::unique_lock<std::mutex> lk(mu_);
        for (;;) {
            cv_.wait(lk, [&] { return stop_ || dirty_; });
            // Debounce: write 400 ms after the last step (at once on exit).
            while (!stop_ && dirty_ && std::chrono::steady_clock::now() - dirtyAt_ < std::chrono::milliseconds(400))
                cv_.wait_until(lk, dirtyAt_ + std::chrono::milliseconds(400));
            if (dirty_) {
                dirty_ = false;
                const float db = saved_.load();
                lk.unlock();
                if (db != written_) {
                    std::ofstream out(file_, std::ios::trunc);
                    out << db;
                    written_ = db;
                }
                lk.lock();
            }
            if (stop_ && !dirty_) return;
        }
    }
    pm::AudioSink& inner_;
    fs::path file_;
    std::atomic<float> saved_;
    std::atomic<bool> muted_{false};       // the app's mute (settings.ini)
    std::atomic<bool> phoneMuted_{false};  // the phone sent -144 this session
    std::atomic<long long> sessionAt_{-1000000};
    float written_;  // writer thread only
    std::mutex mu_;
    std::condition_variable cv_;
    bool dirty_ = false, stop_ = false;
    std::chrono::steady_clock::time_point dirtyAt_{};
    std::thread writer_;
};

// Android (scrcpy) audio into the same player: it has no volume of its own,
// so each stream starts at the remembered level (and the app's mute), not at
// whatever the last AirPlay session left (e.g. -144 mute).
class SavedVolumeAudioSink final : public pm::AudioSink {
public:
    SavedVolumeAudioSink(pm::AudioSink& inner, RememberVolumeAudioSink& saved, std::function<void(const std::string&)> log)
        : inner_(inner), saved_(saved), log_(std::move(log)) {}
    void onFormat(pm::AudioCodec c, int rate, int ch, int spf) override {
        saved_.newSession();
        const float db = saved_.sharedDb();
        inner_.onVolume(db);
        if (log_)
            log_("android audio: volume set to the remembered " + std::to_string(saved_.saved()) + " dB" +
                 (saved_.muted() ? " (muted)" : ""));
        inner_.onFormat(c, rate, ch, spf);
    }
    void onPacket(const uint8_t* d, size_t n, uint64_t t) override { inner_.onPacket(d, n, t); }
    void onFlush() override { inner_.onFlush(); }
    void onVolume(float db) override { inner_.onVolume(db); }

private:
    pm::AudioSink& inner_;
    RememberVolumeAudioSink& saved_;
    std::function<void(const std::string&)> log_;
};
