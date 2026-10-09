// PhoneMirror AirPlay receiver core: C++ facade over the UxPlay protocol
// library (raop + internal mDNS responder). GPL-3.0-or-later.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pm/media.h"

namespace pm {

class AirPlayServer {
public:
    enum class LogLevel { Error, Warning, Info, Debug };
    using LogCallback = std::function<void(LogLevel, const std::string&)>;

    // What happens when a second phone selects this receiver while one is
    // mirroring (meeting room: phones taking turns). See docs/core.md
    // "Several phones (takeover)".
    enum class TakeoverPolicy {
        // Default. The newcomer takes over as soon as it is admitted (paired,
        // PIN entered if required, first RTSP SETUP): the old phone's session
        // is closed (its iPhone shows mirroring stopped), the sinks get
        // onReset() (+ audio onFlush()), then Events::onTakeover(old, new),
        // onClientConnecting(new) and the new stream. A phone that merely
        // probes the receiver, or fails/cancels the PIN, does not disturb the
        // running session.
        NewReplacesOld,
        // Reject newcomers (RTSP 409 at their first request, before any PIN)
        // while a session is active; the same phone reconnecting is allowed.
        KeepCurrent,
    };

    struct Options {
        // Multi-phone behaviour; may be changed only before start().
        TakeoverPolicy takeoverPolicy = TakeoverPolicy::NewReplacesOld;
        // Advertise H.265/HEVC screen mirroring (AirPlay features bit 42).
        // Off by default: the iPhone then always sends H.264.
        bool h265 = false;
        // Display mode reported to the client in GET /info "displays" (0 =
        // upstream default 1920x1080, or 3840x2160 when h265 is on). The
        // iPhone scales its screen to fit this box, HEIGHT is the controlling
        // value (portrait mirroring at 1080 arrives as ~498x1080, at 2160 as
        // ~996x2160). Only one of width/height set: the other one is derived
        // as 16:9. Range 320..7680 x 240..4320.
        // height > 1080 needs H.265 (the iPhone refuses H.264 above 1080p and
        // sends an unsupported codec packet instead), so start() then turns
        // h265 on by itself (logged). Use applyQualityPreset() for the UI.
        int width = 0;
        int height = 0;
        int refreshRate = 60;  // Hz, 1..255 (/info refreshRate = 1/refreshRate)
        int maxFps = 60;       // 1..255 (/info maxFPS); the phone may send fewer
        // Optional PEM file holding the server's Ed25519 identity (created on
        // first use). Empty = UxPlay default: key derived from the deviceid
        // (MAC), i.e. also stable across runs.
        std::string keyFile;
        // "aa:bb:cc:dd:ee:ff"; empty = MAC of the first up Ethernet/Wi-Fi
        // adapter (random if none). Becomes the AirPlay deviceid.
        std::string macAddress;
        // true: fixed UxPlay "-p" legacy ports (TCP 7000 control, 7100 mirror
        // data; UDP 6000/6001/7011) so port-based firewall rules work.
        // false: let the OS pick ephemeral ports.
        bool legacyPorts = true;
        // Forward the library's LOGGER_DEBUG messages to the log callback.
        bool debugLog = false;
        // Drop the session if the client sends no feedback heartbeat for this
        // many seconds while connected (UxPlay default 15; 0 = never).
        unsigned feedbackTimeoutSec = 15;
        // Volume the phone starts at, AirPlay dB: 0 full, -30 quietest,
        // -144 mute. Reported to the phone as initialVolume.
        float initialVolumeDb = -15.0f;
        // Ask the phone for a 4-digit PIN (shown via Events::onPin) before
        // accepting a new connection (UxPlay "-pin" + "-reg"): mDNS TXT gets
        // pw=true / sf=0x8c; an iPhone that has not paired with this receiver
        // identity yet requests a PIN (a fresh random one per attempt) and must
        // type it. Clients that entered the PIN are remembered in
        // "<keyFile dir>/airplay_pin_clients.txt" and reconnect without a PIN;
        // with an empty keyFile nothing is stored and returning clients are
        // accepted (UxPlay "-pin" without "-reg"). Changing it needs stop()+start().
        bool requirePin = false;
        // Periodic (every 5 s while mirroring) "[video-timing]" Info log line
        // with receive-path latency figures; see docs/core.md.
        bool timingLog = true;
        // Audio latency the receiver reports to the phone (RECORD
        // "Audio-Latency" header; UxPlay "-al"). -1 = library default 250 ms.
        // Experimental knob for A/B tests of the mirroring delay.
        int reportedAudioLatencyMs = -1;
        // Experimental: ACK every TCP segment of the mirror video stream at
        // once (SIO_TCP_SET_ACK_FREQUENCY = 1) instead of Windows' delayed
        // ACKs. Process-wide; takes effect for the next mirror connection.
        bool mirrorQuickAck = false;
        // A/B test (0.7.6, TikTok "AirPlay" cast bar during mirroring): offer
        // this PC as an audio-only AirPlay target too? 1 = yes (UxPlay
        // default: features bit 9 + _raop._tcp), 0 = Screen Mirroring only
        // (bit 9 off, no _raop._tcp), 2 = bit 9 on but no _raop._tcp. The
        // mirroring session's own sound is not affected by the receiver in
        // any mode (core/src/pm_audio_advert.h). Needs stop()+start().
        int advertiseAudio = 1;
    };

    // Picture-quality presets (display size requested from the iPhone):
    //   Standard 標準 1920x1080 @60, H.264
    //   High     高   2560x1440 @60, H.265 (bit 42; needed above 1080p)
    //   Highest  最高 3840x2160 @60, H.265
    // Sets width/height/refreshRate/maxFps/h265 in opts; other fields untouched.
    // Takes effect at the next start() (stop()+start() to change it live).
    enum class QualityPreset { Standard, High, Highest };
    static void applyQualityPreset(Options& opts, QualityPreset preset);

    AirPlayServer();
    ~AirPlayServer();
    AirPlayServer(const AirPlayServer&) = delete;
    AirPlayServer& operator=(const AirPlayServer&) = delete;

    // Must be called before start(). The callback may be invoked from any
    // thread (network threads, mDNS thread, the internal supervisor thread).
    void setLogCallback(LogCallback cb);
    void setOptions(const Options& opts);

    // Session events for the UI. Called from network threads; keep them short.
    struct Events {
        // A phone asked to connect (RTSP SETUP, after pairing; before any
        // video arrives). deviceName as the phone sends it (UTF-8, e.g.
        // "Victor's iPhone"), model e.g. "iPhone15,2".
        std::function<void(const std::string& deviceName, const std::string& model)> onClientConnecting;
        // The last open connection closed (or the session was reset), after
        // an onClientConnecting/onPin. Not sent for bare GET /info probes.
        std::function<void()> onClientDisconnected;
        // requirePin only: "1234" = show this PIN; "" = hide it again (the
        // client paired / sent SETUP, or the PIN timed out). A phone keeps
        // the same PIN for its pairing attempt: a retry shows it again; a new
        // PIN comes after 120 s without a retry or 3 wrong entries.
        std::function<void(const std::string& pin)> onPin;
        // TakeoverPolicy::NewReplacesOld: newName's phone replaced oldName's
        // session (old connection already closed, sinks already reset).
        // Followed by onClientConnecting(newName, ...). onClientDisconnected
        // is NOT sent for the replaced phone.
        std::function<void(const std::string& oldName, const std::string& newName)> onTakeover;
    };
    // Before start().
    void setEvents(Events events);

    // Starts the RAOP/AirPlay HTTP server, the RTP/NTP machinery and the mDNS
    // advertisement of _airplay._tcp and _raop._tcp under displayName.
    // Sinks may be null (data is then dropped) and must outlive stop().
    bool start(const std::string& displayName, VideoSink* video, AudioSink* audio);
    // Withdraws the mDNS advertisement (goodbye packets) and shuts everything
    // down. Safe to call repeatedly; also called by the destructor.
    void stop();

    bool isRunning() const;
    // TCP port of the RAOP/AirPlay server (valid after a successful start()).
    unsigned short port() const;
    // "aa:bb:cc:dd:ee:ff" actually used as deviceid.
    std::string deviceId() const;
    // Name of the phone whose session is admitted right now (set at its
    // onClientConnecting, cleared when it disconnects / tears down, replaced
    // on a takeover); "" if none. Thread-safe.
    std::string currentClientName() const;

    // Re-scan the network interfaces now, re-open the mDNS socket on all
    // suitable ones and re-announce (twice, 1 s apart); restarts the
    // RTSP listener only if it is no longer running and no client is
    // connected. Never drops an active session. Asynchronous (returns at
    // once; the work and its log lines happen on the internal network
    // thread). Network changes (adapter up/down, address change, Wi-Fi
    // reconnect after resume) are also detected automatically (debounced
    // 2 s); call this additionally on WM_POWERBROADCAST / PBT_APMRESUMEAUTOMATIC.
    void refreshNetwork();

    // Interfaces currently advertised by mDNS, e.g. {"Wi-Fi=192.168.1.20",
    // "乙太網路=192.168.1.21"} (UTF-8). Empty when not running / no network.
    std::vector<std::string> advertisedInterfaces() const;

    // The clock used for ntpLocalNs timestamps passed to the sinks:
    // wall-clock nanoseconds since the Unix epoch (UxPlay's "local NTP" time).
    // ntpLocalNs = the sender's capture/presentation timestamp mapped onto
    // this clock through the session's NTP clock sync (one offset per
    // session, shared by audio and video), i.e. localTimeNs() - ntpLocalNs
    // at arrival is the transport latency. See docs/core.md "Timestamps".
    static uint64_t localTimeNs();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace pm
