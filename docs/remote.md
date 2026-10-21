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
| `remote/src/tunnel.*` | 臨時網址 (0.8.1, then called 免安裝外網): cloudflared quick tunnel child process (job object), its public address |
| `remote/src/input.*` | DataChannel messages -> SendInput (keys / combos released atomically, buttons released on disconnect) |
| `remote/src/http_server.*` | tiny HTTP/1.1 server bound to the LAN address, the Tailscale address and 127.0.0.1 (the tunnel's origin); WebSocket (RFC 6455) for the tunnel |
| `remote/web/` | the phone page (`index.html`), trackpad gestures (`trackpad.js`, MoonlightWeb, GPL-3.0+), phone keyboard (`vkbd.js`, noVNC, MPL-2.0), `icon.png` |
| `app/remote/remote_actions.cpp` | the app side: menu, settings, approval dialog, banner, Ctrl+D hotkey, remembered phones |
| `app/remote_panel.*` | QR window (tabs 同一個 Wi-Fi / 固定網址 / Tailscale / 臨時網址, the main page's order since 0.8.3.4: `kTabOrder`) and the topmost 「正在被遠端操控」 bar |
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
- **Sound.** `remote_audio_source` = auto | default | device id; menu 遠端操控 ▸ 聲音來源 (the page: 進階設定 ▸ 聲音 ▸ 聲音來源 since 0.8.3.4). Sound
  off keeps the capture 15 s (no reopen churn). The page shows 「電腦目前沒有在播放聲音」 after
  6 s below 0.001. Test: `remote/tools/audio_test.mjs` (pm_remote_spike `--tone`, `--list-audio`).
- **臨時網址 (temporary address; 0.8.1 免安裝外網).** cloudflared quick tunnel -> https://<random>.trycloudflare.com ->
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

## 0.8.3

Measured on the dev PC (Windows 11, NVENC, 3440x1440), the phone = headless Edge on the
same PC (390x844 touch emulation), so the network part is the PC's own loopback / LAN /
Tailscale adapter. Before = e69c71e (0.8.2 + the --dev Funnel fix) built in a separate
worktree, after = this branch. Nothing was played, typed or shown; no tailscale / cloudflared
was run. Tools: `remote/tools/sec083_test.mjs` (spike: http, ws, audio, page, viewonly, fixed,
ts), `remote/tools/app083_test.mjs` (app), `remote/tools/idle_footprint.mjs` (app, CPU / RAM /
GPU). Re-run: `node sec083_test.mjs <spike.exe> <out> [--before]`.

### A. Picture, sound, use

| # | what | before | after |
|---|---|---|---|
| A1 | 觸控直接操作 | page default `trackpad` | default `直接點` (new key `mode083`: a finger on the picture clicks there; 觸控板 one tap away in 設定) |
| A2 | 60 fps | moving screen, nobody touching: 30 fps (log `60->30 (phone not touched)`, 15 fps after 60 s) | 58-61 fps (8 one-second samples); the lower rates only for a still picture (costs nothing either way) |
| A3 | end-to-end latency (page HUD / report `e2e_ms`: ½ RTT + ½ frame slot + capture->shown by the stamp) | capture->shown p50 18 / p95 25 ms at 30 fps; same formula 35 ms | p50 14 / p95 16 ms at 60 fps; e2e 23 ms (p95 33) |
| A4 | first picture (ttff) | spike page 631-720 ms; app, phone again 195-211 ms, PC first frame 148-163 ms after the offer request | spike 625-628 ms; app 140-189 ms, 100-141 ms (the capture opens on the page load, in parallel); first phone 75 s after the last one (capture + encoder released, reopened in 27 ms on the page load): ttff 186 ms vs 185 ms before (capture kept open), PC first frame 137 vs 140 ms |
| A5 | 外網密碼登入：解除僅觀看 | `unlock` ignored, input dropped (0 clicks) | badge 「僅觀看 · 解除僅觀看」 -> password again (same per-address limits as the login): wrong -> `left 4`, right -> 僅觀看 off for this session only, the click goes through (1); 僅觀看 on the PC locks it again; PC toast |
| A6 | 音量 AGC + 補償 | quiet material (synthetic, -30 dBFS) sent at peak 0.032; the phone's volume default 150 % | AGC: peak 0.128 (x4 = +12 dB) towards -18 dBFS RMS, loud material unchanged (0.52 -> 0.52), noise floor not followed (adapts above -34 dBFS only); phone volume default 100 %. Compensation of a software-volume device by its master-volume dB (up to +18 dB): see "not verified" |
| A7 | 閒置關掉擷取 + NVENC (`idle_footprint.mjs`, 固定網址 on, no phone, 60 s) | 134 threads, 265.7 MB private, 135.3 MB GPU dedicated, 4 log lines / min | 105 threads, 111.4 MB, 40.0 MB, 0 log lines (CPU 1.0 vs 1.6 % of a core: the app's own start-up noise, the remote part sleeps) |
| A7 | ... 60 s after the last phone | 149 threads, 287.2 MB, 145.9 MB GPU, 0.67 % | 121 threads, 137.0 MB, 53.6 MB GPU, 0.65 % (released 30 s after the last session; libdatachannel's threads stay) |

### B. Health check (each with its reproduction in `sec083_test.mjs` / `app083_test.mjs`)

| # | problem | reproduction | before | after |
|---|---|---|---|---|
| B1 | 中斷 could be bypassed | fixed link login -> connected -> 中斷 on the PC -> the same login asks again with a new device id | `200` offer (straight back in), `check` 204 | `401 {needLogin, revoked}`, also after an app restart (`remote_revoked` in settings.ini: token id + expiry only) |
| B2 | one stranger locked everybody out (global lock) | 5 wrong passwords from 203.0.113.5, then the right one from 198.51.100.7 | `429 locked, retryAfter 896` | `200` + login; 203.0.113.5 alone is out (429); one try at a time per address; all addresses together: > 20 wrong in 15 min = one try per 3 s (paced, never locked) |
| B3 | CF-Connecting-IP / X-Forwarded-For forged | fixed link (Funnel) request with `CF-Connecting-IP: 9.9.9.9`, `X-Forwarded-For: 1.2.3.4, 203.0.113.10` | logged / counted as `9.9.9.9` | `203.0.113.10` (Funnel: only its last XFF entry; cloudflared: only CF-Connecting-IP; only on the loopback origins, never on the LAN / Tailscale listener) |
| B4 | malformed JSON ended the program | `{"t":"ping","id":"x"}` over the tunnel WebSocket (the fixed page's) | spike gone, exit 0xC0000409 (std::terminate) | alive, `message ignored (...type_error...)` |
| B5 | ... the same over the DataChannel | 9 malformed messages in one batch | libdatachannel `Uncaught exception in callback`: the rest of the batch dropped | each one logged and ignored, the others handled |
| B6 | malformed HTTP JSON | `{"device":12345}`, `{"device":"abcdefgh12","q":"x","m":{}}` on POST session | 500, 500 | 400, 200 (bad q / m = the defaults) |
| B7 | orphan session: two set-ups at once | two POST session at the same moment | 2 offers, 0 closed (the loser stays connected, out of reach of 中斷, its DataChannel still drives the input) | one at a time (`offerMu`); the first is closed `replaced by a new session`; a non-current session's messages are refused and it is closed |
| B8 | orphan session: the page goes away | close the phone's tab | the session (bar, input) stays 29.4 s until ICE failed | 2 ms (`leave` on pagehide / 中斷連線); without it 15 s of silence (10 min in the background) |
| B9 | 僅觀看 could read the clipboard | 僅觀看 on, the page sends `clip-get {watch:true}` | the PC clipboard text sent (and every change pushed) | `clip-denied`; the watch stops when 僅觀看 is turned on; files too (`files-denied`, upload 403) |
| B10 | a slow request held a connection slot | one header line every 3 s | still open at 32 s (no end: 10 s per recv) | closed at 13.5 s (10 s for the whole request) |
| B11 | 8 MB body before any check | POST session announcing 8,000,000 bytes | waits for the body (16 connections x 8 MB) | `413` at once: 64 KB for every request, 4 MB only for a file piece of the current session, 8 MB only for the tests' snapshot route |
| B12 | a reconnecting tunnel WebSocket left the old one open | two WebSockets for the same session | the first stays open | the first is closed |
| B13 | a silent WebSocket held its slot for ever | upgrade, `ws-media on`, then nothing | open at 80 s (no read timeout) | the session ends at 12.8 s (B8's silence rule); 65 s read timeout as a backstop |
| B14 | the wrong-link count could be reset | 9 wrong paths from one address, 257 other addresses once each, then the 10th | not blocked (every count cleared at 257 addresses) | blocked, next request 429 (the address with the fewest tries is dropped; expired blocks pruned) |
| B15 | password counts unbounded per address | (code) | `pwFails` one global counter | per address, at most 512 kept (oldest dropped) |
| B16 | 解除僅觀看 brute force | (code) | - | the same per-address limits as the login, checked off the DataChannel thread, one at a time |
| B17 | LAN Wi-Fi hiccup ended the session | (not reproduced: needs a real network drop) | `disconnected` closed it at once | waits 4 s for `connected` again |
| B18 | the accept thread polled | (code) | `select` every 200 ms with no phone | sleeps on FD_ACCEPT / stop events |
| B19 | the capture + housekeeping thread never slept | A7 | 4 remote log lines / min, capture and encoder open with no phone | housekeeping on its own 1 s thread, capture asleep |

### C. Added on request (files, lock, connection log)

| what | result |
|---|---|
| 傳檔到電腦 (`input type=file multiple`, POST `up` in 4 MB / 1 MB pieces, 2 GB cap, resumable from the PC's offset, 4 at a time per session, disk-space check) | 64 MB: LAN 337-566 ms (113-190 MB/s); Tailscale address 288-318 ms (201-222 MB/s; same PC); saved byte-exact in `%USERPROFILE%\Downloads\自在投影遠端\` (`--dev`: `<data folder>\uploads`); PC toast |
| names from the phone | `../../evil.txt` -> `evil.txt`, `..\..\evil2.txt` -> `evil2.txt`, `CON.txt` -> `_CON.txt`, `nul` -> `_nul`, `a<b>:c?.txt` -> `abc.txt`, `....` -> `file`, the same name twice -> `same (2).txt`; nothing outside the folder |
| 從電腦下載 (CF_HDROP only: files copied in Explorer; one-time key per file, 10 minutes, streamed) | 64 MB: LAN 122-264 ms (242-525 MB/s), Tailscale address 101-215 ms (298-634 MB/s); the same link again `410` |
| 僅觀看 | file sheet hidden, `files-denied`, upload `403` |
| 遠端斷線後自動鎖定電腦 (outside on, LAN / Tailscale off; the page says so next to 中斷) | app test (`--dev` = stub, never LockWorkStation): LAN phone leaves -> no lock; fixed-link phone leaves -> `TEST STUB: would lock the PC now (from outside ... the phone left)`; 中斷 on the PC -> no lock |
| 連線紀錄 (遠端控制 ▸ 連線紀錄…, last 50) | `remote_connections.log` (time, LAN / Tailscale / 固定網址 / 臨時網址 / 外網, masked address `203.0.*.*`, device, 成功 / 密碼錯誤 / 被擋 / 被拒); rotated at 64 KB (`.old.log`); no password / token in it; 3 wrong passwords in a row from outside -> tray note |

### Not verified / left out

- Volume compensation on real devices (it needs sound playing): the loopback level vs the
  master volume of a software-volume device is assumed from the WASAPI behaviour noted in
  0.8.3 step 0 (`--list-audio`); A6's AGC numbers are from the silent synthetic source.
- cloudflared (臨時網址, the fixed link's fallback) was never run: CF-Connecting-IP trust and
  the tunnel WebSocket were tested through the fixed link's own origin (same code paths).
- A real phone over Wi-Fi / mobile data: every number above is PC-local (RTT < 1 ms).
- LockWorkStation itself, the tray balloons (no tray icon in `--test-offscreen`) and the
  連線紀錄 window (a message box; `DevCommand 965 1` writes its text to the log).
- B17 (Wi-Fi hiccup) and uploads through the relays (HTTPS) were not reproduced.

### Protocol / options added

HTTP (behind the link / login): `POST up?id=&f=&n=&o=&t=` (a file piece -> `{ok, got, done, name}`,
409 `{got}` = continue from there, 403 `{viewOnly}`, 413 `{tooBig}`, 507 `{diskFull}`),
`GET dl/<one-time key>`. DataChannel / WebSocket from the page: `leave`, `unlock{pw}`,
`files-get`; from the PC: `unlock{ok,left,locked,retryAfter}`, `files{list:[{n,s,k}]}`,
`files-denied`, `clip-denied`. Offer: `unlockable`, `lockAfter`, `upload`, `uploadMax`.
`Options`: `revokedLogins` / `onLoginRevoked`, `onUnlocked`, `uploadDir`, `uploadMaxBytes`,
`onUploaded`, `testClipboardFiles`, `audioAgc`, `audioVolumeComp`; `Host`:
`viewOnlyUnlocked()`, `setFixedViaCloudflared()`, `setLockAfterSession()`. settings.ini:
`remote_revoked`, `remote_lock_outside` (1; 0.8.3.2: 0), `remote_lock_lan` (0). Spike: `--upload-dir`,
`--upload-max`, `--test-files`, `--no-agc`, `--no-volume-comp`, `--synthetic-audio quiet`.
App `--dev`: `PM_REMOTE_TESTFILES`, `PM_REMOTE_UPLOADDIR`, DevCommand 965 (1 連線紀錄 text,
2 中斷 from the PC).

## 0.8.3.1

### No picture on the owner's iPhone Safari (fixed link, 2026-10-21 07:48)

The page came in with its saved 30-day login (no password typed: no tap on the page at all),
WebRTC connected, the first IDR was decoded (`page-report`: `w` 3440, `dec_ms` 10) but
`video.play()` was refused (`playErr: NotAllowedError`, `paused: true`, `videoW: 0`): this
iPhone refuses even muted autoplay (WebKit does so in Low Power Mode; every session of this
phone in the log has `tapAsk`). Since 0.8.1 the tap prompt was a pill whose text was never set,
under the full-page 「連線中…」 spinner; at 10 s the page blamed the router. 0.8.2 worked only
because the password was typed on that page load (a tap). The decoder was never the problem
(0.8.2, 01:31: the same phone at 3440x1440 level 5.2, 57.8 fps).

- Connected + play refused: 「已連上電腦，點一下就會出現畫面」 with 顯示畫面 (any tap works);
  on the 外網 / fixed links the picture comes over the tunnel's WebCodecs canvas after 1.5 s
  (no autoplay needed) and the first tap moves to direct WebRTC with a fresh IDR
  (`{"t":"idr","why":...}`, logged, at most one per 300 ms).
- No picture and nothing to tap for: IDR request at 3 s; at 6 s the tunnel (外網 / fixed) or,
  when frames arrive but none decodes, quality 0 for this session; the 10 s watchdog keeps the
  tap message when that is the reason.
- 「連線中…」 never spins for ever: 12 s -> 「電腦一直沒有回應」 / 「畫面的連線一直接不起來」 with
  重試 and 看排除方法 (`page-diag stuck-connecting`); the saved-login check gives up after 10 s.

### 固定網址 after a reboot

`TailscaleFunnel::startPatiently`: BackendState NoState / Starting (or the CLI not reaching
tailscaled yet) = wait and try again after 2, 4, 8, 16, 32, 60, 60, 60, 58 s (300 s); the card
and the panel say 「等待 Tailscale 啟動…（n 秒後再試）」, no temporary address is offered.
NeedsLogin / Stopped / no Funnel / 443 changed fail at once as before. 8443 already pointing at
our origin with Funnel on (tailscaled keeps it across restarts) is left as it is (no command).
After 5 minutes: 「Tailscale 等了 5 分鐘還沒啟動完成」.

### Tests (none listens on the LAN: no firewall prompt; tailscale / cloudflared never run)

- `--loopback` (spike) / `PM_REMOTE_LOOPBACK=1` (app `--dev`): the LAN side on 127.0.0.3, no LAN
  or Tailscale listener, no STUN; `edge_test.mjs --loopback`, `app_remote_test.mjs --loopback`
  (Edge with `--allow-loopback-in-peer-connection`).
- `node remote/tools/novideo_test.mjs <spike> <out>`: owner / tap / normal / stuck / nopic, with a
  WebKit-like autoplay block, the saved login, quality 2, static scene.
- `app_remote_test.mjs --block-autoplay`: the same case inside the app.
- `pm_funnel_test.exe`: the retry against a stubbed CLI (`TailscaleFunnel::setRunnerForTest`).
- `node remote/tools/funnel_wait_shots.mjs <自在投影.exe> <out>`: 「等待 Tailscale 啟動」 in the app
  (`PM_REMOTE_FUNNELSTARTING=<n>`: stubbed CLI inside a `--dev` instance).
- The connected-phone banner is placed off every monitor in `--test-offscreen` (it was shown at
  the top of the real screen).

## 0.8.3.2

### The PC's sound was too quiet on the phone (owner's log, 2026-10-21 10:16-10:20, iPhone Chrome, fixed link)

The owner had to set 手機音量 to 300 % and the iPhone to half volume to hear it at a normal level.
The log: `audio: level: AGC on (up to +12 dB towards -18 dBFS RMS)`, one device (自在的喇叭, HD
Audio Driver for Display Audio, software volume -5.4 dB -> loopback x1.86), and every `stats audio`
line `peak 0.02-0.06 (AGC x1.00, volume x1.86)`. The page's own analyser (`page-report rmsIn`,
before its gain) never went above 0.011 (47 reports, mean 0.0019).

Root cause, `remote/src/audio.cpp` (0.8.3, the AGC block of `LoopbackAudio::loop()`): it measured
the RMS of each 20 ms frame **after** the volume compensation and only followed frames with
`rms > 0.02f` (-34 dBFS); quieter frames let the gain drift back to x1, and the gain stopped at x4
(+12 dB). Music and speech have 10-18 dB between peak and RMS, so material with peak 0.03 has RMS
0.004-0.011 and never reached the threshold: x1.00 for ever. The threshold had been put between the
synthetic HDMI noise (RMS 0.012) and the synthetic quiet **sine** (peak 0.03, RMS 0.021: a sine has
3 dB of crest), so the 0.8.3 test passed. Even when it did follow, +12 dB would have left this
material near -32 dBFS RMS.

0.8.3.2, `remote/src/level.h` (`LevelChain`, header-only, also used offline):
- leveller towards **-17 dBFS RMS**, gain -6..**+30 dB** (peak 0.03 material needs +27 dB);
  follows the mean square of frames above -55 dBFS RMS (up: 0.8 s, 0.1 s when 12 dB louder; down:
  3 s, 1.2 s when 10 dB quieter); the gain moves at most +20 dB/s up, -60 dB/s down;
- louder material lowers the gain at once; what would raise it is judged 0.5 s late, with 0.5 s on
  either side known: a steady stretch (within 3 dB for 0.5 s: hiss, hum, an HDMI noise floor, a test
  tone, also just after a level change) teaches nothing and the gain drifts to x1 at 1 dB/s; nothing
  is learnt in the first 1 s;
- noise gate: frames under -55 dBFS RMS (or 30 dB under the level) for 200 ms go back to x1 at
  60 dB/s (never above x1): pauses stay as quiet as the PC plays them;
- limiter on the frame's own peak (the 20 ms frame is a free look-ahead): 0.89 (-1 dBFS), release
  10 dB/s; a soft knee above 0.95 as a last guard. The stats line adds `RMS <dBFS>` of what was sent.

Measured offline (`pm_level_test`, synthetic PCM, the 0.8.3.1 code copied verbatim vs LevelChain,
then Opus 128 kbps and back; settled = after 10 s; gain at 1 / 2 / 3 / 10 s / end):

| material | before RMS | after RMS (Opus) | after peak | gain after | noise floor in pauses |
|---|---|---|---|---|---|
| music, peak 0.03 + hiss -59 dBFS | -44.3 | **-16.9** (-17.0) | 0.75 | 0 / +14 / +28 / +28 / +27.5 dB | |
| speech, peak 0.03 | -45.2 | **-16.8** (-16.8) | 0.84 | 0 / +12 / +29 / +28 / +29 dB | |
| owner case: device peak 0.0161 x1.86 | -44.3 | **-16.9** | 0.75 | +27.5 dB | |
| music, peak 0.01 | -52.0 | -22.0 | 0.38 | +30 dB (the limit) | |
| ordinary video, peak 0.5 | -17.5 | -17.0 | 0.75 | +3.2 dB | |
| loud master, peak 0.95 | -14.4 | -17.0 | 0.86 | -2.4 dB | |
| two loud devices (sum 1.23) | -14.1, 168 samples > 0.95 | -17.0, **0 > 0.95** | 0.89 | -2.9 dB | |
| quiet music, 10 s hiss only, music | -44.3 | -17.0 | 0.75 | +27.5 dB | **-58.7 -> -58.7** |
| steady HDMI-like noise only (RMS -39) | | | 0.02 | **0 dB** throughout | -38.8 -> -38.8 |
| quiet 12 s, then peak 0.95 | | -17.3 | **0.89, 0 clipped** | +28 -> -2.4 dB at once | |
| peak 0.95 12 s, then peak 0.03 | | -20.0 | 0.86 | -2.4 -> +22 dB in 8 s -> +27 | |

No sample reached 1.0 in any case, before or after Opus. End to end (`level_e2e.mjs`: spike
`--loopback --synthetic-audio`, WebRTC, headless muted Edge): quiet music -44.2 -> **-17.0 dBFS RMS**
(AGC +27.2 dB); speech -48.9 -> -20.5 over all frames incl. its pauses (gated at x1); 0.8.3's
"music" (sine on/off over a steady 0.02 noise) and "loud" (two steady sines) stay at 0 dB (steady).

手機音量 (page, Web Audio gain + compressor): on the owner's iPhone Chrome the path works — at 300 %,
`rmsOut / rmsIn` = 3.3-3.5 in the log (x3 and the compressor's make-up gain). With the PC at
-17 dBFS RMS, 100 % is normal (about what a phone plays a video at), so the setting starts again
at **100 %** under a new key (`gain0832`: the 300 % people set for the old AGC would be ~10 dB too
loud), range **0-400 %** (+12 dB for material beyond the AGC's +30 dB or a noisy place; above that the
limiter would only squash).

### 遠端斷線後自動鎖定電腦

Off by default for both (outside and LAN / Tailscale); a saved value is kept. And the two menu items
(and 連線紀錄…) never did anything since 0.8.3: `runCommand()` did not route CmdRemoteLockOutside /
CmdRemoteLockLan / CmdRemoteLog to `remoteCommand()`.

### 允許遠端控制 and 臨時帳號

The 遠端控制 page starts with 允許遠端控制 (in place of 固定網址＋密碼): 本機網址 (the fixed address,
large) with its QR code and 複製網址; 新增臨時帳號; the accounts; 在家 and Tailscale below as before.
Without the fixed address: 產生臨時網址 (the one-time cloudflared address and its 通行碼).

0.8.3.4 (design B): with the fixed address on, the top block is 「本機網址（在外面也能連）」 (`RemoteAllowAddr`): a
small label over the big address, 複製網址 (filled) and 新增臨時帳號, the QR code beside; the accounts are a card of
their own under it; 在家 and Tailscale are compact rows. Off the page, in 進階設定 (`remoteAdvancedItems()`):
變更固定網址的密碼…, 固定網址（在外面也能連） (check item, only with a password: unticking it turns the fixed
address off), 開機自動開啟固定網址, and 聲音 ▸ (電腦聲音傳到手機, 手機播聲音時電腦靜音, 聲音來源). The panel's
固定網址 tab still has 關閉固定網址 / 變更密碼. Phone page 設定: 電腦喇叭靜音 (`mutePc`) = the PC's
手機播聲音時電腦靜音 (kept on the PC; refused in 僅觀看 and for a 臨時帳號: `mutePcOk` / `mutePcWhy`);
手機音量 0–400 % (100 % = the PC's level, above = 響度加強).

- 新增臨時帳號: name (guest-NNNN suggested, editable; letters, digits, - _ .), 有效時間 用一次 (within
  24 h) / 1 h / 24 h / 7 days, 可操作 / 僅觀看, 允許傳檔 (off). The password: 6 characters from
  `ABCDEFGHJKMNPQRSTUVWXYZ23456789` (no 0 O 1 I L), checked upper-cased. Shown once with the address and
  the account, 複製全部; 重設密碼 makes a new one (a connected phone stays). At most 10.
- Kept in `remote_guests.txt` beside settings.ini: id, name, PBKDF2 hash, created, expires, once, view
  only, files, last used. Never the password (not in the file, not in the log).
- Phone page: 帳號 above 密碼 (empty = the owner's own password, the 30-day login as before).
- Host (`Host::setGuests`, `POST /login {"user","password"}`): a guest login lasts at most 12 h and
  never past the account; it always waits for 允許 (never "allowed before" / remembered, the dialog
  has no 記住); 僅觀看 drops all input and offers no 解除僅觀看; without 傳檔 upload / download / list
  are refused (403); 用一次 is spent when its session ends; an account that is deleted or expired
  (pruned every 30 s, not while connected) loses its logins and its session ends at once. Every
  failure answers 401 alike (an unknown name costs a dummy PBKDF2) and counts in the per-address
  limits (5 wrong = 15 minutes). 連線紀錄: `[guest-4821] iPhone · Safari`.

### 固定網址 (then 常駐外網) on a new tailnet froze the app (a friend's PC, 0.8.3.1)

A new tailnet has Funnel (and HTTPS certificates) off: `tailscale funnel --bg` prints "Funnel is not
enabled on your tailnet. To enable, visit: https://login.tailscale.com/f/funnel?node=..." and waits
until that page is clicked. 0.8.3.1 ran it with a 30 s limit (plus two 15 s status calls) on the fix
thread, and `stopFixed()` / `startFixed()` **joined that thread on the UI thread** (stopFixed also ran
`funnel off` there): 關閉固定網址, 再試一次 or quitting froze the window for up to a minute, and
meanwhile the card had no address and no reason.

- `runCapture`: the CLI in a job object (its tree ends with it), at most 15 s, stopped as soon as it
  prints an enable page; `TailscaleFunnel::cancelAll()` for quit. Answers: `needs-enable: funnel|https
  <url>`, `no-funnel-attr: <url>`, `port-taken: <target>` (8443 serving another program is left
  alone), `cli-too-old`, `needs-login`, `timeout: ...`; none is "still starting" (no 0.8.3.1 retry).
- The app: the CLI only on background threads, one job at a time, never waited for by the UI thread
  (a start that finishes after 關閉 undoes itself; at quit 4 s at most, then the CLI is ended).
- The card in every failure: the reason in plain words and what to do; Funnel / HTTPS not on yet:
  開啟設定頁 (the tailscale.com page the CLI named) + 我開好了，再試一次 + 改用臨時網址.

### Other

- `remote/tools/funnel_retry_test.cpp`, `app/remote/remote_actions.cpp`: the made-up name
  `example-pc.example.ts.net`; neutral wording for port 443 and the reboot case.
- `tools/release/publish.mjs manifest` takes x.y.z.w.

### Tests (silent: synthetic PCM only, nothing played; loopback only; tailscale / cloudflared never run)

- `pm_level_test.exe [gain.csv]`: the table above (exit 1 if a target fails).
- `node remote/tools/level_e2e.mjs <spike> <out> [--ab]`: the real capture loop, synthetic source.
- `node remote/tools/app0832_test.mjs <app> <out>`: lock off by default; the menu id turns it on.
- `node remote/tools/guest_test.mjs <spike> <out>`: 17 checks (30-day vs 12 h, 用一次 then 401 for the
  login and the old link, expired 401, 僅觀看 = 0 inputs, no 傳檔 = 403 / files-denied, deleted while
  connected -> session ends in 2 s, no password in the log).
- `node remote/tools/app_guest_test.mjs <app> <out>`: 11 checks in the app (no password in any file
  of the data folder, never remembered, 連線紀錄 name, 用一次 gone, 刪除 ends the session in 0.6 s).
- `pm_funnel_test.exe`: the new answers against the stub, and the fake CLI (`pm_fake_tailscale.exe`,
  never tailscale) as a real process: enable page -> stopped in 0.1 s, silence -> 15 s, no process left.
- `node remote/tools/app_funnel_test.mjs <app> <pm_fake_tailscale.exe> <out>`: each case in the app;
  the UI thread answers within 7 ms while the CLI runs; 關閉 during a hang 0.4 s; quit 4.5 s.
- `node tools/shots_0832.mjs <app> <out> [--before] [--dpi]`: the page per language, the window, and
  the page as on a 1080p monitor at 125 % / 150 % (DevCommand 985: WM_DPICHANGED with a 1080p client).

## 0.8.3.3 (usability, docs/ux-audit-083.md)

### The fixed address before its HTTPS certificate (audit D2, P0)

The first time a tailnet uses Funnel, `tailscale funnel --bg` answers at once (the app said 「常駐外網已開啟」)
while Tailscale issues the HTTPS certificate in the background: for a minute or two every connection
to `https://<pc>.<tailnet>.ts.net:8443/` is closed (the phone: ERR_CONNECTION_CLOSED). Now the address
is said to be on only once it answers:

- `TailscaleFunnel::probeTls(url, detail)` (`remote/src/funnel.cpp`): one HTTPS GET from this PC
  (WinHTTP, no proxy, 5 s; the name resolves to this node, the certificate is the one phones get).
  Ready = a TLS handshake with a valid certificate and an HTTP answer below 500 (502 / 503: Funnel is up
  but cannot reach the app yet). WinHTTP 12030 = connection closed (the certificate case), 12175 =
  certificate.
- The app (`startFixedProbe`, after `RemFixedUp`): fixState stays 1 (starting); a thread checks at once,
  then every 3 s, for up to 5 minutes (it never touches RemoteState; `stopFixed` ends it). After the first
  failure the 允許遠端控制 block shows the address (no QR code, no 臨時帳號) and 「正在準備網址，約 1–2 分鐘…」;
  when it answers: fixState 2, the toast 「固定網址已開啟」, log `常駐外網 ready at <url> (...); 固定網址
  reachable after <ms> ms (HTTP <code>)`; after 5 minutes: failed, 「網址等了 5 分鐘還連不上…」 with 再試一次 /
  先用臨時網址. The cloudflared fallback is not checked (it has its own self-check).
- `--dev`: only `PM_REMOTE_FUNNELPROBE=https://127.0.0.1:<port>/` is ever checked (a fake local TLS server
  with a self-signed certificate, accepted only there); without it a made-up / fake-CLI address is ready at
  once (no DNS query for a made-up name). `PM_REMOTE_FUNNELPROBEMAX=<s>` shortens the 5 minutes.

### One name each: 固定網址 and 臨時網址 (audit D4)

「常駐外網」 is gone: the check box / menu item is 「開機自動開啟固定網址」 (since 0.8.3.4 in 進階設定, not on the page), the toasts 「固定網址已開啟／已關閉」,
the password window 「設定固定網址的密碼」. 「免安裝外網」: the panel's tab and the menu item are 「臨時網址」.
Same in English (fixed address / temporary address; the tab: Temporary), 日本語 (固定アドレス／一時アドレス),
한국어 (고정 주소／임시 주소), and on the phone page (通行碼 page, 解除僅觀看).

### 臨時網址 = always 允許 on the PC (audit D-F1 / #7)

The temporary address is for lending the PC, like a 臨時帳號: `Host` lets no "allowed before" or
remembered phone straight in through it (`viaTempLink`: a tunnel request that is not the fixed link's), the
question has no 記住這支手機 and says the phone came in with the temporary address (`RemoteAskBodyTemp`).
The panel's note: 「每次連線都要有人在電腦旁按「允許」；自己出門用，請用固定網址。」 (it said 「出門前先在電腦旁連一次
並勾記住」).

### The phone's sign-in page: owner / 臨時帳號

- Owner (the fixed address as is): only the password; 「輸入你設定的密碼」; the link 「用臨時帳號登入」.
- 臨時帳號: the address 複製全部 gives (and the 已建立 window shows) is `…:8443/#u=<account>`. A fragment never
  leaves the phone (not in the request to the PC, not at Tailscale's relay, never in a Referer: the page
  already sends `no-referrer`), so the account is in no server log; a query string would be. The page opens
  in this mode with the account shown (「不是這個帳號？」 to type another) and only the password to type:
  「輸入對方給你的密碼」 / 「…連上後，對方要在電腦上按「允許」」.
- Errors: owner 「密碼不對，還可以再試 N 次」; 臨時帳號 「帳號或密碼不對，還可以再試 N 次」 for a wrong password and
  an unknown account alike. `Host` keeps the accounts that went (expired / used once / deleted) for 7 days in
  memory (name, hash, why); only their right password gets `{"gone": "expired|used|deleted"}` (「這個臨時帳號已經
  過期了／只能用一次，已經用過了／已經被刪除了」), with the same single PBKDF2 as any try (a valid account, a gone
  one or the dummy) and counted like a wrong one. A guest login that stops working mid-visit: 「這個臨時帳號已經
  不能用了…」.
- 16 px text, 顯示密碼 / 隱藏密碼.

### Phone page, pairing window, layout (audit #6, #9, #10, layout)

- Toolbar: a word under each icon (聲音 / 放大 / 檔案 / 鍵盤 / 設定; muted: 「開聲音」 in the accent colour), also
  as tooltips; the bar sits 16 px higher. The call to action reads 「點一下開聲音」.
- 設定 ▸ 操作方式: 「直接點：手指點哪裡就點哪裡。觸控板：像筆電的觸控板，手指滑動移動游標。」
- 連接 Android: 「① 還沒開過『開發人員選項』？看步驟」 (docs/app.md).
- The block's big address breaks only between the host name and 「:8443/」 (or the path), after first trying
  it on one line a little smaller; a 臨時帳號 row breaks only between its fields (explicit line breaks between
  the U+3000-separated fields; the row is laid out without wrapping), so 「可 / 操作」 is gone.

### Small ones (audit P2)

A1 the first start opens the guide at `#want`; A2 the idle check box 「開機自動啟動（開機後在背景等手機）」; A3 the
cards' notes in plain words; B2 「截圖已存到「圖片\截圖」資料夾」; B5 USB: 「…在手機的「開發人員選項」打開「USB 偵錯」…」;
C1 a dropped folder: 「…可以先把資料夾壓縮成 zip」; C2 「傳送這 N 個到手機」; D5 「先用臨時網址（每次會變）」; D6 下載
Tailscale on the Tailscale card (opens tailscale.com/download/windows; logged only in `--dev`).

### Tests (tailscale / cloudflared never run; loopback only; nothing is ever allowed on the PC)

- `pm_funnel_test.exe`: + probeTls against local servers (closes every connection / plain HTTP / an http://
  address / the insecure mode outside 127.0.0.1).
- `node remote/tools/app0833_test.mjs <app> <pm_fake_tailscale.exe> <out>` (36 checks): probe (a fake TLS server
  drops every connection for 8 s: 正在準備網址, no "ready at" before it answers), cli (the same through the fake
  CLI), giveup (PM_REMOTE_FUNNELPROBEMAX=6), login (owner page without the account field, `#u=` fills it in and
  never reaches the log, wrong password = unknown account, a deleted account's right password says so), tunnel
  (a phone remembered on this PC still waits for 允許 through the temporary address, no 記住; denied).
- `node tools/shots_0833.mjs <app> <out> before|after`: every changed screen in four languages (off-screen,
  made-up addresses; the phone page in headless Chrome).
