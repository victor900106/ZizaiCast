// 自在投影 app: connect/airplay.cpp — connect（iPhone / Android / Miracast 連線狀態）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "help/help.h"
#include "capture/capture.h"
#include "connect/connect.h"
#include "connect/connect_internal.h"

namespace pm_app {

bool startServer(int attempts) {
    auto server = std::make_unique<pm::AirPlayServer>();
    Log* log = g.log;
    StatusVideoSink* status = g.status;
    server->setLogCallback([log, status](pm::AirPlayServer::LogLevel level, const std::string& msg) {
        static const char* names[] = {"error", "warn", "info", "debug"};
        log->write(names[static_cast<int>(level)], msg);
        if (status) status->noteCoreLog(msg);
    });
    pm::AirPlayServer::Options opts = g.opts;
    opts.requirePin = g.settings.requirePin;
    opts.advertiseAudio = g.settings.advertiseAudio;
    // The remembered volume as of now, not as of app start: a restart (PIN /
    // quality / language / network change) must not go back to the startup level.
    // (AirPlayServer::start() applies it to the player; RememberVolumeAudioSink
    // ignores an unchanged level, so nothing stale is written back.)
    opts.initialVolumeDb = savedVolumeDb();
    // 畫質: display size offered to the phone (High/Highest also turn H.265 on).
    using QP = pm::AirPlayServer::QualityPreset;
    pm::AirPlayServer::applyQualityPreset(
        opts, effectiveQuality() == 0 ? QP::Standard : effectiveQuality() == 2 ? QP::Highest : QP::High);
    opts.h265 = opts.h265 || g.opts.h265;  // --h265 forces HEVC at any size
    // Stable identity: VPN/virtual adapters must not change the deviceid.
    if (!g.settings.deviceId.empty()) opts.macAddress = g.settings.deviceId;
    // 新手機連線時：接手 / 保持目前
    using Takeover = decltype(opts.takeoverPolicy);
    opts.takeoverPolicy = g.settings.takeoverKeep ? Takeover::KeepCurrent : Takeover::NewReplacesOld;
    server->setOptions(opts);
    const HWND h = g.hwnd;
    pm::AirPlayServer::Events ev;
    ev.onClientConnecting = [h, log](const std::string& device, const std::string& model) {
        log->write("info", "client connecting: " + device + " (" + model + ")");
        // A new phone session starts with a fresh audio pipeline (decoder,
        // jitter buffer, WASAPI stream) at the remembered volume: nothing from
        // the previous session (a mute, a stuck stream) can keep it silent.
        // Not while a phone is mirroring (takeover / keep-current) or another
        // source owns the player.
        const int src = g_active.load();
        if (g.audio && (src == SrcNone || src == SrcAirPlay) && !(g.status && g.status->mirroring())) {
            g.audio->resetSession("AirPlay client connecting", savedVolumeDb());
            if (g.volume) {  // no phone mute from the last session; the app's 靜音 / 0 % stay silent
                g.volume->newSession();
                g.audio->onVolume(g.volume->playDb());
            }
        }
        postText(h, EvConnecting, toWide(device.empty() ? model : device));
    };
    ev.onClientDisconnected = [h, log]() {
        log->write("info", "client disconnected");
        postText(h, EvDisconnected, {});
    };
    ev.onPin = [h](const std::string& pin) { postText(h, EvPin, toWide(pin)); };
    ev.onTakeover = [h, log](const std::string& oldName, const std::string& newName) {
        log->write("info", "takeover: \"" + newName + "\" replaced \"" + oldName + "\"");
        postText(h, EvTakeover, toWide(newName));
    };
    server->setEvents(std::move(ev));
    // Ports may linger briefly after a restart; retry for a few seconds.
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (server->start(toUtf8(g.name), g.videoSink, g.audioSink)) {
            g.log->write("info", "ready on port " + std::to_string(server->port()) + ", device id " +
                                     server->deviceId() + ", display " + std::to_string(opts.width) + "x" +
                                     std::to_string(opts.height) + "@" + std::to_string(opts.maxFps) +
                                     (opts.h265 ? ", H.265" : ", H.264") + (opts.requirePin ? ", PIN required" : ""));
            if (g.settings.deviceId.empty() && Settings::validMac(server->deviceId())) {
                g.settings.deviceId = server->deviceId();  // first run: keep the core's choice
                saveSettings();
                g.log->write("info", "device_id stored: " + g.settings.deviceId);
            }
            std::string ifs;
            for (const std::string& i : server->advertisedInterfaces()) ifs += (ifs.empty() ? "" : ", ") + i;
            g.log->write("info", "advertised interfaces: " + (ifs.empty() ? std::string("(none yet)") : ifs));
            g.server = std::move(server);
            return true;
        }
        server->stop();
        if (attempt + 1 < attempts) Sleep(400);
    }
    return false;
}

void restartServer() {
    g.restartPending = false;
    g.applyChip.hide();  // 0.7.8 UX (p9): nothing held back any more
    if (g.testNoNetwork) return;  // --dev --test-no-network: never listen (e.g. after a language switch)
    g.log->write("info", "restarting AirPlay server (requirePin=" + std::to_string(g.settings.requirePin) +
                             ", quality=" + Settings::qualityKey(g.settings.quality) +
                             ", takeover=" + (g.settings.takeoverKeep ? "keep" : "new") +
                             ", airplay_advertise_audio=" + std::to_string(g.settings.advertiseAudio) + ")");
    if (g.server) g.server->stop();
    g.server.reset();
    const bool ok = startServer(10);
    if (!ok) {
        g.log->write("error", "AirPlay server failed to restart");
        g.window->showToast(tr(S::AirPlayRestartFail));
    }
}

}  // namespace pm_app
