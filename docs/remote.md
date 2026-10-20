# 遠端操控 / Remote control (0.8.0, 0.8.1)

A phone browser (iPhone Safari, Android Chrome) sees and controls this PC: picture, the
PC's sound, touch input. No app on the phone, plain http, no certificate.
User guide: `docs/tutorial/*` section `#remote` (0.8.2 layout: `#remote-open #remote-home
#remote-outside` (fixed address + password) `#remote-pwa #remote-tailscale #remote-nolink`
(temporary address) `#remote-gestures #remote-audio #remote-safety #remote-limits #remote-trouble`;
FAQ items `#remote-faq-nosound #remote-faq-noconnect #remote-faq-forgot`).
PWA files for the phone page: `remote/web/manifest.webmanifest`, `apple-touch-icon.png`, `icon-192.png`, `icon-512.png`.

## Pieces

| where | what |
|---|---|
| `remote/include/pm/remote.h` | public API: `pm::remote::Host`, `Options`, `Peer`, `monitors()`, `qualityPreset()`, `tailscaleIp()` |
| `remote/src/host.cpp` | HTTP routes, approval flow, one WebRTC session (H.264 video + Opus audio track + input DataChannel), capture/encode loop, housekeeping |
| `remote/src/capture.*` `encoder.*` | DXGI Desktop Duplication -> D3D11 video processor NV12 -> Media Foundation H.264 (zero-copy to NVENC when possible) |
| `remote/src/audio.*` | WASAPI loopback -> libopus 20 ms frames. 0.8.1: every active playback device captured, the one that plays is sent (or the Windows default / a chosen one); IMMNotificationClient for device changes |
| `remote/src/tunnel.*` | 0.8.1 免安裝外網: cloudflared quick tunnel child process (job object), its public address |
| `remote/src/input.*` | DataChannel messages -> SendInput (keys / combos released atomically, buttons released on disconnect) |
| `remote/src/http_server.*` | tiny HTTP/1.1 server bound to the LAN address, the Tailscale address and 127.0.0.1 (the tunnel's origin); WebSocket (RFC 6455) for the tunnel |
| `remote/web/` | the phone page (`index.html`), trackpad gestures (`trackpad.js`, MoonlightWeb, GPL-3.0+), phone keyboard (`vkbd.js`, noVNC, MPL-2.0), `icon.png` |
| `app/remote/remote_actions.cpp` | the app side: menu, settings, approval dialog, banner, Ctrl+D hotkey, remembered phones |
| `app/remote_panel.*` | QR window (同一個 Wi-Fi / Tailscale / 免安裝外網 tabs) and the topmost 「正在被遠端操控」 bar |
| `remote/tools/` | `pm_remote_spike` (stand-alone host), `edge_test.mjs` (page e2e), `app_remote_test.mjs` (app e2e), `measure.mjs` (before/after numbers) |

Dependencies (vcpkg, classic mode): `libdatachannel[core,srtp]` (+ libjuice, libsrtp,
usrsctp, OpenSSL, plog, nlohmann-json), `opus`. Without them `pm_remote` and the app's
remote menu are simply left out.

## Security model

- Off at every app start; the user turns it on (menu / tray). Not a service, nothing hidden.
- Listens only on the LAN address (peers must be on its subnet) and, if present, this PC's
  Tailscale address (peers must be in 100.64.0.0/10). Never 0.0.0.0, never loopback.
- A 32-character random token in every path (constant-time compared), kept in settings.ini
  (`remote_token`) so a Home Screen icon keeps working; delete it for a new link.
  10 wrong links from one address in 10 minutes -> that address gets 429 for 15 minutes
  (and the app shows a notice).
- A new phone (random device id kept in the page's localStorage) waits until 允許 on the PC;
  the dialog never takes the keyboard focus (a typed Enter cannot answer it). 記住這支手機
  -> `%LOCALAPPDATA%\PhoneMirror\remote_devices.txt`. After a deny the same phone cannot
  ask again for 20 s. After 中斷 the next connection asks again even if remembered.
- One phone at a time. While connected: topmost bar 「● 正在被遠端操控」 with 僅觀看 and
  中斷（Ctrl+D）; Ctrl+D is a global hotkey only during the session (so the window's own
  Ctrl+D 中斷連線 for AirPlay waits until it ends). The PC does not sleep while connected.
- 僅觀看 drops every input message on the PC side. Idle (no touch) for 30 minutes (setting)
  ends the session. Every request is logged with the peer IP.
- Not possible (UIPI): elevated windows, UAC, lock screen, Ctrl+Alt+Del.

## Protocol (page <-> PC)

HTTP: `GET /<token>/` (page with the app's language / colours injected),
`trackpad.js`, `vkbd.js`, `icon.png`, `manifest.webmanifest` (standalone), `help` (the
guide in the UI language), `POST session` (`{device,q,m}` -> 200 offer / 202 pending /
403 denied / 429 blocked), `POST session?id=` (long-poll while pending), `POST answer?id=`
(SDP), `POST diag` (page diagnostics, `reason` -> `onTrouble`).
WebRTC: server offer, non-trickle, host candidates on the address the page came in on;
RTP payload 1200 (1100 over Tailscale, MTU 1280).
DataChannel (JSON): input `move{dx,dy,n} abs{x,y} down/up/click{b} wheel text key combo
release`, page state `act ping report pause{on} audio{on} stamp{on} bitrate{kbps} idr
clip-get clip-set{s,type} clip-stop`; from the PC `pong stats cur{x,y,v,n} mode{viewOnly}
clip bye{why}`.

## Picture and bandwidth

Presets: 流暢 1920x1080, 標準 2560x1440, 清晰 native up to 3840x2160; 60 fps max,
6 / 10 / 14 Mbps peak-constrained VBR, IDR every 10 s of frames (+ immediately on PLI).
Frame rate follows use: 60 fps while the phone is touched (+1.5 s), 30 fps otherwise,
15 fps after 60 s untouched, nothing while the page is in the background. Unchanged frames
are not encoded (1 refresh / s + 3 tail frames after a change). The page lowers the bitrate
(70 % / 45 %) and then one quality step on loss > 4 %, RTT > 300 ms or far fewer frames
shown than sent; it comes back after 10 s / 30 s good.

## Tests

- `node remote/tools/edge_test.mjs <pm_remote_spike.exe> <out> [--via lan|tailscale] [spike args]`
  (input dry-run unless `--inject --test-window`: injection only into the spike's own window).
- `node remote/tools/app_remote_test.mjs <自在投影.exe> <out> [--lang ..] [--no-ts]`
  (`--dev --test-offscreen`, own data folder, `PM_REMOTE_DRYRUN=1`, test clipboard).
- `node remote/tools/measure.mjs <pm_remote_spike.exe> <out> <seconds> [--touch-every ms] [spike args]`.

## 0.8.1

- **Phone taps can't switch it off.** Every injected event carries dwExtraInfo `kInjectedInputTag`;
  the QR panel's 關閉遠端操控 / 停止外網連結 and the bar's 中斷 / 僅觀看 ignore such clicks; menu
  commands that would end it are refused within 0.8 s of a phone click; a Ctrl+D typed on the phone
  does not trigger the global hotkey. The QR panel closes when a phone connects.
- **Turned off on the PC** -> the bye is flushed before the connection closes; the page says
  「電腦端已關閉遠端操控」.
- **First picture.** The page probes autoplay at load and asks for the tap at once (in parallel
  with 允許 / connecting); RTCPeerConnection built while the session request is out; the host warms
  up WebRTC at start; scripts inlined; ICE wait 250 ms + trickle (POST candidate). Phase marks go to
  the PC log (`page-diag first-frame`). Measure: `remote/tools/ttff_test.mjs`.
- **Sound.** `remote_audio_source` = auto | default | device id; menu 遠端操控 ▸ 聲音來源. Sound
  off keeps the capture 15 s (no reopen churn). The page shows 「電腦目前沒有在播放聲音」 after
  6 s below 0.001. Test: `remote/tools/audio_test.mjs` (pm_remote_spike `--tone`, `--list-audio`).
- **免安裝外網 (no-app link).** cloudflared quick tunnel -> https://<random>.trycloudflare.com ->
  127.0.0.1:<port>. Routes there: `/t/<one-time>` (10 min, once) -> 303 `/c/<32 random>/`; the page
  needs the 6-digit 通行碼 (POST pass; 5 wrong kill that path, 10 wrong from one CF-Connecting-IP
  block it 15 min), then 允許. LAN token routes never work on 127.0.0.1. The session first tries
  WebRTC with public STUN (no TURN); if not connected after 2.5 s (or it fails later) the page asks
  for H.264 + Opus over the WebSocket (`/c/<path>/ws?id=<session>`): binary
  [1][flags][seq u32][capture ms f64][Annex B AU] / [2][capture ms f64][Opus], decoded with
  WebCodecs onto a canvas; input and control on the same socket. Congestion: the page acks the
  newest frame every 50 ms; a frame unacked for 0.5 s drops video until a key frame and lowers the
  bitrate (70 %, floor 800 kbps; back up 15 % after 8 s clean). cloudflared is looked for in
  `<app>\cloudflared\cloudflared.exe`, then Program Files, then PATH. Test:
  `remote/tools/tunnel_test.mjs`, `app_tunnel_test.mjs`.
