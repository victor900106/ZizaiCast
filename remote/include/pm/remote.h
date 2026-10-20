#pragma once
// pm_remote: 遠端操控 -- a phone browser sees and controls this PC. A monitor
// -> DXGI Desktop Duplication -> Media Foundation H.264 (low latency,
// constrained baseline, zero-copy on the GPU) -> WebRTC (libdatachannel) to
// a page served by a small token HTTP server bound to the LAN address (and
// the Tailscale one when there is one); the PC's sound (WASAPI loopback ->
// Opus) on an audio track; touch input back over a reliable, ordered
// DataChannel -> SendInput.
//
// Security (see docs/remote.md): the token in every path (constant-time
// compared), LAN-subnet / tailnet peers only, connection caps; one phone at a
// time; a new device must be allowed on the PC (onRequest -> approve()), a
// remembered or already allowed one goes straight in; nothing is streamed and
// no input is taken before that. kick() ends the session (the page does not
// reconnect by itself, the next connection asks again). viewOnly: picture
// only, every input message is dropped.
//
// Threading: start / stop / approve / kick / setViewOnly from any thread (the
// app: its UI thread). The callbacks run on the server's threads -- marshal
// them yourself. Set them before start().

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pm::remote {

// Every mouse / keyboard event this module injects carries this dwExtraInfo
// (SendInput). A window that must never be operated from the phone (the
// PC's own 關閉遠端操控 / 中斷 / 僅觀看) checks GetMessageExtraInfo() for it.
constexpr uintptr_t kInjectedInputTag = 0x504D5254;  // 'PMRT'

// Monitors, primary first then left to right (physical pixels).
struct Monitor {
    int index = 0;
    int x = 0, y = 0, w = 0, h = 0;
    bool primary = false;
};
std::vector<Monitor> monitors();

// Picture presets, offered on the phone page: 0 流暢 (fits 1920x1080),
// 1 標準 (2560x1440), 2 清晰 (native, up to 3840x2160).
struct Quality {
    int maxWidth, maxHeight, fps, kbps;
};
Quality qualityPreset(int level);
constexpr int kQualityLevels = 3;

// A Tailscale address of this PC (100.64.0.0/10 on an up adapter), "" if none.
std::string tailscaleIp();

// Active playback devices (for 聲音來源): id, FriendlyName, Windows default.
struct AudioDevice {
    std::string id, name;
    bool isDefault = false;
};
std::vector<AudioDevice> playbackDevices();

struct Peer {
    std::string session;  // id (approve() / kick())
    std::string device;   // the page's own random id (kept in the phone browser)
    std::string ip;
    std::string label;    // "iPhone · Safari", from the user agent
    bool tailscale = false;  // came in over Tailscale (outside the LAN)
    bool tunnel = false;     // came in over the 外網連結 (cloudflared quick tunnel); ip = CF-Connecting-IP
    bool fixed = false;      // came in over the fixed link with the password (常駐外網, 0.8.2)
};

// Password hash for the fixed link: "pbkdf2-sha256$<iterations>$<salt hex>$<key hex>"
// (PBKDF2-HMAC-SHA256, 16-byte random salt); never the password itself.
std::string hashPassword(const std::string& password, int iterations = 310000);
bool verifyPassword(const std::string& password, const std::string& stored);
std::string randomHex(size_t bytes);  // e.g. a token-signing secret

// Tailscale Funnel through the tailscale CLI (常駐外網, 0.8.2). Only the given
// port is ever changed; port 443's configuration is snapshotted before and
// after every change (start / stop fail if it differs).
struct FunnelStatus {
    bool cli = false, running = false, funnelCap = false;
    std::string backend, dnsName, error;  // error: no-tailscale / not-running: ... / ...
    std::string port443;                   // JSON snapshot of port 443's serve / funnel config
    bool hasPort = false, portFunnel = false;
    std::string target;                    // what the port proxies to
};
class TailscaleFunnel {
public:
    static std::wstring findCli();
    static FunnelStatus status(int port = 8443);
    // tailscale funnel --bg --https=<port> <target>; url = https://<name>:<port>/
    static bool start(int port, const std::string& target, std::string& url, std::string& err);
    static bool stop(int port, std::string& err);  // tailscale funnel --https=<port> off
    // 0.8.2.1: does something answer at the Funnel's target ("http://127.0.0.2:<port>")?
    // Then another running 自在投影 serves it, and it is not stale.
    static bool targetAnswers(const std::string& target);
};

struct Options {
    std::string bindIp;      // LAN IPv4 to bind; empty = the first real LAN interface
    bool tailscale = true;   // also listen on the Tailscale address (if any)
    uint16_t port = 8090;    // 0 = any free port
    std::string token;       // empty = random 32 chars
    int quality = 2;         // qualityPreset() level to start with (the page may change it)
    int maxWidth = 0, maxHeight = 0, fps = 0, kbps = 0;  // > 0: override the preset (spike / tests)
    int monitor = 0;         // monitors() index to start with
    double idrSeconds = 10.0;  // of encoded frames; a lost frame is repaired at once (PLI -> IDR)
    std::string encoder = "auto";  // auto | hw | sw
    bool inject = false;     // false = dry-run (input only logged)
    bool viewOnly = false;   // picture only, no input
    bool keepAwake = true;   // no sleep / display off while a phone is connected (SetThreadExecutionState)
    double idleMinutes = 30; // end the session after this long without a touch on the phone (0 = never)
    // Frame rate follows use: full rate while the phone is touched (and 1.5 s
    // after), half rate otherwise, idleFps after idleAfterSec without a
    // touch, nothing while the page is in the background. false = always full.
    bool adaptiveFps = true;
    int idleFps = 15;
    double idleAfterSec = 60;
    bool requireApproval = true;  // false: every page is let in (spike)
    bool audio = true;       // offer the PC's sound (the page turns it on)
    bool mutePc = false;     // mute the PC's speakers while the phone plays the sound
    // Which playback device's sound: "" automatic (the one that plays; the
    // Windows default while all are quiet), "default" the Windows default
    // (follows it), else a device id from playbackDevices().
    std::string audioSource;
    bool stamp = false;      // capture-time stamp in the top-left corner (latency; the page's debug view turns it on)
    bool playoutDelayZero = true;  // RTP playout-delay ext min=max=0 (Chrome renders ASAP)
    bool drawCursor = true;
    // Optimisation switches (defaults = optimised; the others exist to measure the difference).
    bool cpuPath = false;
    bool skipStatic = true;
    int staticRefreshMs = 1000;
    int tailFrames = 3;
    bool vbr = true;         // peak-constrained VBR: a still screen costs almost nothing
    int vbvFrames = 2;
    bool inlineSend = false;
    int rtpPayload = 1200;   // RTP payload bytes per packet (Tailscale sessions: at most 1100, MTU 1280)
    std::string scene = "desktop";  // desktop | static | motion (test scenes)
    bool testClipboard = false;  // tests: an in-memory clipboard instead of the PC's real one
    std::string testClipboardText;
    std::wstring snapDir;    // where POST /<token>/snap PNGs go (empty = disabled)
    // Page: language (0 zh-TW, 1 en, 2 ja, 3 ko), app name (UTF-8), colours (0xRRGGBB).
    int lang = 0;
    std::string appName = "自在投影";
    uint32_t pageBg = 0x1E1416, pageAccent = 0xF5A7A7, pageFg = 0xF6E9EA;
    std::string helpUrl;     // shown by the page for the tutorial (optional)
    // The guide (self-contained HTML, current language): served at /<token>/help so the
    // phone can open its remote-control sections (#remote, #remote-trouble ...). Wins over helpUrl.
    std::wstring helpFile;

    std::function<void(const std::string&)> log;
    std::function<bool(const std::string& device)> remembered;  // allowed without asking
    std::function<void(const Peer&)> onRequest;   // ask the user, then approve()
    std::function<void(const Peer&)> onActive;    // allowed: streaming / controlling from now
    std::function<void(const Peer&, const std::string& why)> onEnded;  // pending or active session gone
    // An address used a wrong link (token) too often and is shut out for a while.
    std::function<void(const std::string& ip, int failures)> onBlocked;
    // The page could not get a picture: a plain reason (ap-isolation / firewall / ...), for the PC's notice.
    std::function<void(const std::string& ip, const std::string& reason)> onTrouble;
    // 外網連結 (0.8.1): the one-time link was used (or expired) and a new one
    // replaces it: show the new QR code ("/t/<token>", after the https address).
    std::function<void(const std::string& linkPath)> onTunnelLink;
    // Public STUN servers for the direct (hole-punched) WebRTC path of a
    // tunnel session; no TURN (the tunnel's WebSocket is the fallback).
    std::vector<std::string> stunServers{"stun:stun.cloudflare.com:3478", "stun:stun.l.google.com:19302"};
    // Tests (before / after): false = the tunnel never drops video (no congestion rule).
    bool tunnelCongestion = true;
    // Tests: "music" / "loud" = the PC sound comes from a silent synthetic source (nothing is played or captured).
    std::string audioSynthetic;
    // 常駐外網 (0.8.2): the fixed link (Tailscale Funnel :8443 or a cloudflared fallback ->
    // 127.0.0.2:<port>), logins with the owner's password.
    std::string fixedPasswordHash;  // hashPassword(); empty = no logins
    std::string fixedSecret;        // signs the login tokens (30 days); a new one ends every login
    bool fixedNeedApproval = false; // 外面的新手機也要電腦按允許
    std::function<void(const std::string& ip, bool ok, int fails, bool locked)> onLogin;
    // Tests: the tunnel's writer sends at most this many kbps (a slow phone link, 0 = no limit).
    int tunnelTestKbps = 0;

    // Tests: inject only into the test window. injectGuard is asked before a
    // click / button / wheel / key (keyboard = keys and text);
    // injectMoveGuard before the pointer is moved to (x, y) (virtual-desktop
    // pixels). false = not injected, only logged.
    std::function<bool(bool keyboard)> injectGuard;
    std::function<bool(int x, int y)> injectMoveGuard;
};

class Host {
public:
    Host();
    ~Host();
    bool start(const Options& o);
    void stop();
    bool running() const;
    std::string url() const;           // http://<lan-ip>:<port>/<token>/
    std::string tailscaleUrl() const;  // http://<100.x>:<port>/<token>/ ("" when not listening there)
    std::string ip() const;
    uint16_t port() const;
    void approve(const std::string& session, bool allow, bool remember = false);
    void kick(const std::string& why);
    void setViewOnly(bool on);
    bool viewOnly() const;
    bool active() const;                // a phone is allowed and connected
    Peer peer() const;                  // the current one (pending or active)
    // Milliseconds since the phone last clicked / pressed a key on this PC
    // (injected), -1 = never. A PC control that a remote click could reach
    // (a menu item) refuses to turn remote control off right after one.
    int64_t msSinceRemoteInput() const;
    // A short notice on the phone page ("pc-only": that switch works on the PC only).
    void notifyPhone(const std::string& what);
    void setAudioSource(const std::string& source);  // Options::audioSource, while running too
    // 0.8.2: these take effect at once (no restart of the host).
    void setMutePc(bool on);         // Options::mutePc: mute the PC while the phone plays its sound
    void setAudioOffered(bool on);   // Options::audio
    void setIdleMinutes(double minutes);

    // 外網連結 (0.8.1): the routes behind a cloudflared quick tunnel, served on
    // 127.0.0.1:<port> (the tunnel's origin; LAN links never work there).
    // Three locks: a one-time link (10 minutes, one use) gives the page its own
    // secret path; the 6-digit 通行碼 shown on the PC unlocks it (5 wrong =
    // that path is dead, 10 wrong from one address = 15 minutes out); then the
    // usual 允許 on the PC (a remembered phone goes straight in). The page tries
    // WebRTC with public STUN first; when that does not connect it gets H.264 +
    // Opus over the tunnel's WebSocket (WebCodecs), its input on the same socket.
    void setTunnel(bool on);  // on: a new 通行碼 and one-time link; off: every tunnel path invalid
    bool tunnelOn() const;
    std::string tunnelPasscode() const;
    std::string tunnelLinkPath() const;  // "/t/<one-time token>"

    // 常駐外網 (0.8.2): serve the fixed-link routes on 127.0.0.2:<port> (the origin of
    // Tailscale Funnel :8443, or of a cloudflared fallback). "/" = the page with the
    // password; POST /login (5 wrong in a row = 15 minutes locked, a growing delay
    // after each) -> a signed token (30 days) -> /k/<token>/. With the right password
    // a phone goes straight in (unless fixedNeedApproval or it was 中斷'd).
    void setFixed(bool on);
    bool fixedOn() const;
    void setFixedOptions(const std::string& passwordHash, const std::string& secret, bool needApproval);
    std::string fixedOrigin() const;  // "http://127.0.0.2:<port>"

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// cloudflared quick tunnel: https://<random>.trycloudflare.com -> 127.0.0.1:port,
// no account. A child process in a job object (it dies with this process).
class QuickTunnel {
public:
    QuickTunnel();
    ~QuickTunnel();
    // <appDir>\cloudflared\cloudflared.exe (shipped), else an installed one; "" = none.
    static std::wstring findExe(const std::wstring& appDir);
    // onUrl: the public address (once, ~3-10 s); onEnd: it failed / stopped by itself. Both on its thread.
    bool start(const std::wstring& exe, uint16_t port, std::function<void(const std::string& url)> onUrl,
               std::function<void(const std::string& why)> onEnd, const std::string& host = "127.0.0.1");
    void stop();
    bool running() const;
    std::string url() const;
    // Set before start(): once the address is known, whether it is reachable from the internet.
    std::function<void(bool ok, int ms)> onCheck;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace pm::remote
