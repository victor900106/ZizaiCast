// pm_volume_test: the remembered phone volume (app/volume_memory.h) without
// the app, network or audio device.  A temp volume.txt; checks:
//  - a restart (new RememberVolumeAudioSink on the same file) starts at the
//    last audible level the phone set, not at the startup level;
//  - mute (-144) and out-of-range values reach the player but are not kept;
//  - volume steps are debounced (one write) and flushed on destruction;
//  - Android audio starts at the remembered level, not at the last AirPlay gain;
//  - the slider's 0..100 % scale (pm::vol): linear in dB like the iPhone's,
//    0 % silent, phone-step snapping for Ctrl+↑ / Ctrl+↓;
//  - the app's mute: silences AirPlay / Android / Miracast without touching
//    the remembered level, survives phone steps, unmute restores the level,
//    a slider level unmutes; a phone mute only silences its own session.
// No sound device: FakeSink only records the levels the player would get.
#include "../volume_memory.h"

#include <cstdio>
#include <vector>

namespace {

struct FakeSink final : pm::AudioSink {
    std::vector<float> vols;
    int formats = 0;
    void onFormat(pm::AudioCodec, int, int, int) override { ++formats; }
    void onPacket(const uint8_t*, size_t, uint64_t) override {}
    void onVolume(float db) override { vols.push_back(db); }
    void onFlush() override {}
};

int fails = 0;
void expect(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
}

float fileDb(const fs::path& f) { return RememberVolumeAudioSink::load(f, 99.0f); }

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "pm_volume_test";
    fs::create_directories(dir);
    const fs::path file = dir / "volume.txt";
    {
        std::ofstream out(file, std::ios::trunc);
        out << -16.875f;  // the startup level
    }
    FakeSink player;
    {
        RememberVolumeAudioSink v(player, file, -15.0f);
        expect(v.saved() == -16.875f, "starts at volume.txt");
        float followed = 0;
        int changes = 0;
        v.onSavedChange = [&](float db) { followed = db, ++changes; };
        for (float db = -17.0f; db >= -20.625f; db -= 0.5f) v.onVolume(db);  // phone volume steps
        v.onVolume(-20.62f);
        expect(fileDb(file) == -16.875f, "steps are debounced (not written at once)");
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        expect(fileDb(file) == -20.62f, "written ~0.4 s after the last step");
        v.onVolume(-144.0f);  // mute
        v.onVolume(5.0f);     // out of range
        expect(player.vols.back() == 5.0f && v.saved() == -20.62f, "mute / out of range applied, not remembered");
        expect(followed == -20.62f && changes == 9, "onSavedChange follows remembered changes only");
        // What startServer() does on a restart: initialVolumeDb = saved().
        expect(v.saved() == -20.62f, "a restart in this process reports the last level (-20.62), not -16.875");
        // Android audio after a muted AirPlay session.
        SavedVolumeAudioSink android(player, v, nullptr);
        android.onFormat(pm::AudioCodec::AAC_LC, 48000, 2, 1024);
        expect(player.vols.back() == -20.62f && player.formats == 1, "android stream starts at the remembered level");
        v.onVolume(-25.0f);  // last change right before exit
    }
    expect(fileDb(file) == -25.0f, "pending value flushed on exit");
    {
        RememberVolumeAudioSink v(player, file, -15.0f);
        expect(v.saved() == -25.0f, "next app start keeps the last level");
    }
    {
        std::ofstream out(file, std::ios::trunc);
        out << "garbage";
    }
    {
        RememberVolumeAudioSink v(player, file, -15.0f);
        expect(v.saved() == -15.0f, "unreadable volume.txt -> fallback");
    }

    // ---- slider scale (pm::vol) ----
    {
        namespace v = pm::vol;
        expect(v::percentOf(-30.0f) == 0 && v::percentOf(-15.0f) == 50 && v::percentOf(0.0f) == 100,
               "percent: -30 dB 0 %, -15 dB (default) 50 %, 0 dB 100 %");
        expect(v::percentOf(-28.125f) == 6 && v::percentOf(-144.0f) == 0 && v::percentOf(5.0f) == 100,
               "percent: the phone's lowest step 6 %, mute 0 %, out of range clamped");
        expect(v::dbOfPercent(50) == -15.0f && v::dbOfPercent(0) == -30.0f && v::dbOfPercent(100) == 0.0f &&
                   v::dbOfPercent(150) == 0.0f,
               "dbOfPercent inverts percentOf (clamped)");
        bool roundTrip = true;
        for (int p = 0; p <= 100; ++p) roundTrip &= v::percentOf(v::dbOfPercent(p)) == p;
        expect(roundTrip, "every whole percent round-trips through dB");
        bool mono = true;
        for (int p = 1; p <= 100; ++p) mono &= v::gainOf(v::dbOfPercent(p), false) > v::gainOf(v::dbOfPercent(p - 1), false);
        expect(mono, "gain strictly increases with the percent");
        expect(v::gainOf(-30.0f, false) == 0.0f && v::playDb(-30.0f, false) == v::kMuteDb, "0 % is silent (-144)");
        expect(std::fabs(v::gainOf(-15.0f, false) - 0.17783f) < 1e-4f && v::gainOf(0.0f, false) == 1.0f,
               "gain = 10^(dB/20): 50 % 0.178, 100 % 1");
        expect(v::gainOf(-6.0f, true) == 0.0f && v::playDb(-6.0f, true) == v::kMuteDb, "muted: gain 0, -144");
        expect(v::stepDb(-15.0f, 1) == -13.125f && v::stepDb(-15.0f, -1) == -16.875f, "a step = one phone step (1.875 dB)");
        expect(v::stepDb(-20.62f, 1) == -18.75f && v::stepDb(-20.62f, -1) == -22.5f,
               "a phone level (log-rounded -20.62) steps to its grid neighbours");
        expect(v::stepDb(-16.0f, 1) == -15.0f && v::stepDb(-16.0f, -1) == -16.875f, "off grid: to the next grid point");
        expect(v::stepDb(0.0f, 1) == 0.0f && v::stepDb(-30.0f, -1) == -30.0f && v::stepDb(-30.0f, 1) == -28.125f,
               "steps stop at 0 % / 100 %");
        expect(v::stepDb(-30.0f, 16) == 0.0f, "16 steps cover the range (like the phone)");
    }

    // ---- app mute / slider level on the sink ----
    {
        std::ofstream(file, std::ios::trunc) << -15.0f;
        FakeSink p2;
        float shared = 99;
        int sharedN = 0, phoneN = 0;
        {
            RememberVolumeAudioSink v(p2, file, -15.0f, /*muted*/ true);
            v.onSharedChange = [&](float db) { shared = db, ++sharedN; };
            v.onPhoneChange = [&] { ++phoneN; };
            expect(v.muted() && v.playDb() == -144.0f && v.sharedDb() == -144.0f && v.saved() == -15.0f,
                   "muted from settings.ini: silent, level kept");
            v.onVolume(-15.0f);  // AirPlayServer::start's initial level
            expect(p2.vols.back() == -144.0f, "muted: the initial AirPlay level plays silent");
            v.onVolume(-13.125f);  // the phone steps up while muted
            expect(v.saved() == -13.125f && p2.vols.back() == -144.0f && v.muted() && phoneN == 1,
                   "phone step while muted: level follows, stays muted, slider told");
            SavedVolumeAudioSink android(p2, v, nullptr);
            android.onFormat(pm::AudioCodec::AAC_LC, 48000, 2, 1024);
            expect(p2.vols.back() == -144.0f, "muted: Android starts silent");
            v.setMuted(false);
            expect(!v.muted() && p2.vols.back() == -13.125f && shared == -13.125f && v.saved() == -13.125f,
                   "unmute restores the level (player and Miracast)");
            v.setMuted(true);
            expect(shared == -144.0f && p2.vols.back() == -144.0f && v.saved() == -13.125f,
                   "mute: Miracast and player silent, level untouched");
            v.setLevel(-6.0f);  // the slider while muted
            expect(!v.muted() && v.saved() == -6.0f && p2.vols.back() == -6.0f && shared == -6.0f,
                   "a slider level unmutes and applies everywhere");
            v.setLevel(-30.0f);
            expect(p2.vols.back() == -144.0f && shared == -144.0f && v.saved() == -30.0f, "slider at 0 %: silent, kept");
            v.setLevel(-12.0f);
            v.onVolume(-144.0f);  // the phone at its bottom
            expect(v.phoneMuted() && p2.vols.back() == -144.0f && v.sharedDb() == -12.0f && v.saved() == -12.0f,
                   "phone mute: its session silent; Android / Miracast level and the remembered level untouched");
            const int before = phoneN;
            v.onVolume(-144.0f);
            expect(phoneN == before, "a repeated phone mute is not a change");
            v.newSession();
            expect(!v.phoneMuted() && v.playDb() == -12.0f, "a new session drops the phone mute");
            v.onVolume(-144.0f);
            v.setMuted(false);  // unmute = sound again, whatever silenced it
            expect(!v.phoneMuted() && p2.vols.back() == -12.0f, "app unmute also lifts a phone mute");
            v.setLevel(5.0f);
            expect(v.saved() == 0.0f, "slider level clamped to 0 dB");
            v.setLevel(-12.0f);
            expect(sharedN >= 6, "onSharedChange on every level / mute change");
        }
        expect(fileDb(file) == -12.0f, "the slider's level is the one remembered in volume.txt");
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    std::printf("RESULT volume-test: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
