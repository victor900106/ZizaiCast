# Android source (`android/`) — 無線鏡像 + 控制 Android 手機

`pm_android` (static lib) mirrors an Android phone to the PC and lets the
mouse/keyboard control it, scrcpy-style, over Wi-Fi or (0.8.3) a USB cable.
The PC shows a QR code; the phone scans it in 開發人員選項 → 無線偵錯 → 使用
QR 圖碼配對裝置 (Android 11+). After the first pairing the phone reconnects
automatically whenever 無線偵錯 is on and it is on the same LAN. With a
cable, `connectUsb()` uses the phone's 「USB 偵錯」 instead (any Android
version adb supports, no pairing).

## How it works

```
 phone (adbd, Wi-Fi debugging)                         PC (自在投影)
 ─────────────────────────────                         ─────────────
 _adb-tls-pairing._tcp  ──mDNS──►  mdns::Browser + `adb mdns services`
                         ◄── adb pair ip:port <QR password>
 _adb-tls-connect._tcp  ──mDNS──►  adb connect ip:port
 scrcpy-server (app_process) ◄─ adb push + adb shell … Server 5.0 …
 localabstract:scrcpy_<scid> ◄─ adb forward tcp:N ─► 127.0.0.1:N
     video socket  ──H.264 Annex-B──► StreamParser → VideoSink (VideoWindow)
     audio socket  ──AAC-LC raw────► StreamParser → AudioSink (AudioPlayer)
     control socket ◄─ touch/scroll/keys/text ─ InputTranslator ◄─ PointerEvent/keys
```

* **adb**: Google's `adb.exe` (platform-tools r37.0.1) runs as hidden child
  processes (`CREATE_NO_WINDOW`, stdout/stderr piped, explicit handle list)
  against a **private adb server port 15037** (`-P 15037` +
  `ANDROID_ADB_SERVER_PORT`; `ADB_SERVER_SOCKET` removed) so it never fights
  Android Studio / another scrcpy on 5037. `PM_ADB_PORT` overrides the port
  (tests use 15098/15099). All children — including the adb server daemon
  they fork — live in a kill-on-close job object: if 自在投影 exits or
  crashes, adb goes with it; `~AndroidSource` also runs `adb kill-server`.
  **Idle shutdown (0.8.3)**: the server is stopped (`adb kill-server`) when
  no phone is connected / mirroring and nothing used adb for 15 s; every
  path that needs adb starts it again (~2 s). While it is stopped, the
  app's periodic reconnect scan first asks our own mDNS browser whether any
  phone advertises `_adb-tls-connect._tcp` and only then starts adb (every
  5th such scan is a full one with adb's mDNS anyway). Numbers below.
  `ADB_MDNS_OPENSCREEN=1` (built-in mDNS, no Bonjour), `ADB_MDNS_AUTO_CONNECT=0`
  (we connect explicitly → one transport per phone, serial = `ip:port`).
  adb's RSA key is the normal per-user `%USERPROFILE%\.android\adbkey`, so a
  phone already paired with Android Studio works too.
* **QR pairing** (`pairing_qr.cpp`): `WIFI:T:ADB;S:zizai-<10 random>;P:<12
  random>;;` (BCryptGenRandom, alphabet without 0/O/1/l/I), encoded with
  nayuki qrcodegen (ECC M) → square BGRA, 8 px/module, 4-module white quiet
  zone (33×33 modules → 328×328 px). After scanning, the phone advertises
  `_adb-tls-pairing._tcp` with instance name = `S`. We find it two ways in
  parallel: our own mDNS browser (`mdns.cpp`, below) and polling
  `adb mdns services`. Then `adb pair ip:port <password>` → "Successfully
  paired … [guid=adb-XXXX-yyyy]" → the `_adb-tls-connect._tcp` instance with
  that guid (else: same IP) → `adb connect` → wait for `device` state →
  name from `settings get global device_name` (fallback
  `ro.product.marketname`, `ro.product.model`) → `onConnected(name)`.
* **Pair with code** fallback (「使用配對碼配對裝置」): `pairWithCode(L"ip:port",
  L"123456")` → same flow from `adb pair`.
* **Reconnect** (`connectKnownDevices`): reuses an adb transport still in
  `device` state, else browses `_adb-tls-connect._tcp` for 6 s and
  `adb connect`s each phone found; only paired phones accept. A transport
  adb still lists after the phone left the network (`offline`, or
  `already connected` to a dead one) is `adb disconnect`ed and connected
  once more — before 0.8.3 such a zombie made every reconnect fail until
  the adb server restarted.
* **USB** (`connectUsb`, 0.8.3): `adb devices -l`, a serial without `:`
  (not `emulator-…`) in `device` state → same `connected()` path as Wi-Fi
  (name, `onConnected`, `start()`). `unauthorized` → State Connecting
  「請在手機上允許 USB 偵錯（勾選「一律允許這部電腦」再按「允許」）」, waits up to
  30 s; no USB phone for 3 s → Idle 「沒有找到用 USB 連接的手機…」. Every
  later step (`push`, `forward`, the server, transfers) uses `-s <serial>`.
* **mDNS browser** (`mdns.cpp`): IPv4 on every up, multicast-capable
  interface. Standard queries from a shared `:5353` socket (SO_REUSEADDR,
  joined to 224.0.0.251 per interface) + legacy-unicast queries from an
  ephemeral socket (RFC 6762 §6.7) for responders that only answer those.
  Queries PTR for the service types, then SRV for instances and A for hosts
  that were not in the additional section; 3 rounds at 250 ms, then 1/s.
  Name compression, loops and truncation handled. Independent of adb's own
  mDNS, which on Windows can miss phones with several NICs / VPN adapters.
* **Mirroring** (`android_source.cpp`, `scrcpy_session.cpp`): `adb push
  scrcpy-server /data/local/tmp/zizai-scrcpy-server.jar`; random 31-bit
  `scid`; `adb forward tcp:0 localabstract:scrcpy_<scid>` (adb picks the
  port); `adb shell CLASSPATH=… app_process / com.genymobile.scrcpy.Server
  5.0 scid=… log_level=info audio_codec=aac max_size=1920
  video_bit_rate=8000000 max_fps=60 tunnel_forward=true
  clipboard_autosync=true` (+ `stay_awake=true keep_active=true` with
  `setStayAwake(true)`; built by `android_internal::serverArgs`; server log
  lines → `log` as `[server] …`; the server deletes its jar on exit).
  Connect video socket and wait for the dummy byte (adb accepts before the
  server listens; exponential backoff 20 ms → 400 ms, ≤ 15 s), then audio +
  control sockets; read the 64-byte device name. If the server exits during
  start-up, the error is its own `[server] ERROR:` line in plain words
  (「投影啟動失敗：…」), not "cancelled".
* **Stream protocol** (scrcpy v5, `scrcpy_proto.cpp`, incremental parser):
  codec id (4 bytes BE: `h264`, `aac`, 0 = disabled by device, 1 = config
  error), then 12-byte headers. MSB set = *session packet* (video size
  `w,h`; sent at start and on rotation) → `onSourceSize` + touch mapping.
  Else `pts|flags (8)` (bit 62 config, bit 61 key frame) + `size (4)` +
  payload. Video config packets (SPS/PPS) are prepended to the next frame
  (Annex-B AU into `VideoSink::onFrame`, `ntpLocalNs = 0` → shown ASAP).
  Audio: `onFormat(AAC_LC, 48000, 2, 1024)`, config packet (ASC) skipped,
  raw AAC frames → `onPacket`. Audio needs Android 11+: on older phones (or
  if capture is refused) the server sends codec 0 → logged, video only.
* **Control** (`InputTranslator`): left button = one finger
  (`INJECT_TOUCH_EVENT`, pointer id −2 / generic finger, pressure 1, position
  in the current *video* size — the device drops events for a stale size);
  right click = `BACK_OR_SCREEN_ON` (back, or wake the screen); middle click =
  HOME; wheel = `INJECT_SCROLL_EVENT` (notches, ±16 clamp). Keys ("mixed" mode
  like scrcpy): printable characters without Ctrl/Alt → `INJECT_TEXT` (UTF-8,
  surrogate pairs joined; IME commits with vk 0 / VK_PROCESSKEY too), all
  other keys and Ctrl/Alt chords → `INJECT_KEYCODE` with meta state (VK →
  Android keycode table: letters, digits, F1–F12, arrows, Enter, Backspace →
  DEL, Delete → FORWARD_DEL, Home/End, PgUp/PgDn, Esc, Tab, punctuation,
  numpad, volume, media). `pressBack/Home/AppSwitch` = keycodes 4/3/187.
  Device→PC messages on the control socket are parsed (`DeviceMsgParser`:
  CLIPBOARD, ACK_CLIPBOARD, UHID_OUTPUT; the socket is always drained).
* **Clipboard sync** (0.8.3, `setClipboardSync`, default on): phone → PC:
  every device CLIPBOARD message (server `clipboard_autosync=true`) is
  written to the PC clipboard (CF_UNICODETEXT through a hidden
  message-only window, `clipboard.cpp`). PC → phone: the PC clipboard
  sequence number is checked every 500 ms while mirroring; a change is sent
  as `SET_CLIPBOARD` (paste = false) **only once the user controls the
  phone** (a click / key / nav button since `start()`); while only watching
  nothing leaves the PC. Ctrl+V on the mirror = `SET_CLIPBOARD` with paste
  (the phone pastes the PC clipboard); Ctrl+C / Ctrl+X = `GET_CLIPBOARD`
  with COPY / CUT (the copied text comes back as a device message). Our own
  writes are remembered (text + sequence) so nothing is echoed back. Off:
  device messages are ignored and nothing is sent (works at once both ways:
  the server always autosyncs, the filter is ours).
* **Phone screen** (`setPhoneScreenOn(false)`): `SET_DISPLAY_POWER(0)` to
  the running session (and to every later one while it stays off); the
  picture keeps streaming. `stop()` sends `SET_DISPLAY_POWER(1)` and waits
  until the control writer has written it (`Session::drainControl`, ≤ 0.5 s)
  before closing the sockets. When the connection is lost instead, the
  server's own cleanup process on the phone (`CleanUp`, scrcpy v5) switches
  the panel back on.
* **Stay awake** (`setStayAwake(true)`): server options `stay_awake=true`
  (`stay_on_while_plugged_in`, effective while charging / on USB) +
  `keep_active=true` (simulated user activity, works on Wi-Fi). The
  server's cleanup restores the previous `stay_on_while_plugged_in` on exit,
  also when the connection drops. Changing it while mirroring restarts the
  server in place (~2 s gap in the picture, no events).
* **Stalled stream / crashed server** (0.8.3): a monitor on the worker
  (every 500 ms while a session runs) watches the last video packet (scrcpy
  repeats the last frame every 100 ms, so silence is never normal):
  1.5 s → `RESET_VIDEO` (the server restarts capture); 3 s (12 s before the
  first frame) → the session is restarted. A video EOF / stream error while
  adb still answers (`adb shell echo pm-alive`, ≤ 3 s) is handled the same
  way. Restart = new scrcpy-server on the **same sinks**, no
  `onReset`/state change/event, backoff 0.5 / 1 / 2 s (≤ 4 s), 3 tries per
  minute of good streaming; then State Error with the reason +
  `onDisconnected`. (The app's own 6 s no-picture watchdog may still win
  over a slow restart; it then calls `stop()`.)
* **Capture failure** (encoder / screen capture died on the phone: video
  codec id 1, or `[server] ERROR: Capture/encoding error…`): one restart,
  then State Error 「手機無法錄製畫面（視訊編碼器失敗），請再試一次；若一再發生，請重新啟動手機」
  + `onDisconnected()` — no reconnect loop (not a connection problem).
  Audio that the phone cannot capture (codec 0 = Android < 11, codec 1 /
  `Audio … error`) → `Events::onNotice` once per session (「這支手機的
  Android 版本低於 11，投影沒有聲音」 / 「手機的聲音無法擷取，這次投影沒有聲音」);
  a `SecurityException` / `INJECT_EVENTS` error → onNotice with the
  Xiaomi 「USB 偵錯（安全性設定）」 hint.
* **Phone gone** (EOF or stall and adb does not answer): session torn
  down (sockets closed, server killed, forward removed),
  `VideoSink::onReset()`, `AudioSink::onFlush()`, State Idle 「手機連線中斷」,
  `onDisconnected()`; then the reconnect loop: State Connecting
  「手機連線中斷，正在重新連線（第 n 次）…」, attempts after 1, 2, 4, 8, 16,
  30, 30, 30 s (±20 % jitter), each: probe → `adb disconnect` the dead
  transport → `adb connect ip:port` → mDNS for the same IP with a new port
  (無線偵錯 toggled) for 3 s; USB: wait for the serial to return. Success →
  `onConnected` (the app calls `start()` as for any reconnect); after the
  8th → Idle 「手機連線中斷，重新連線失敗（請確認手機的無線偵錯或 USB 偵錯仍開啟）」.
  `stop()`, a pairing or `connectKnownDevices()` cancels the loop.
* **Retries** all use `android::Backoff` (`backoff.h`: base·2ⁿ capped,
  ±20 % deterministic jitter): reconnect loop, in-place restarts, adb
  device polling (100 → 800 ms), the scrcpy socket connect, USB polling.
* **Send files** (`pushFile`, 0.8.3): FIFO on one transfer thread (shared
  with `installApk`). Name = the PC file name, characters Android's shared
  storage refuses (`\ / : * ? " < > |`, controls) → `_`; taken
  (`ls -d`) → 「name (1).ext」…; `adb push <file> /sdcard/Download/<name>`
  (timeout 60 s + 1 s per 256 KB, ≤ 30 min); progress = the phone-side size
  (`stat -c %s`, once a second) → `log` `pushFile progress 37% (… KB)`, and
  the transfer rate at the end; then MediaProvider `scan_file` so the Files
  app lists it. `done(true, 「已傳送到手機的 Download 資料夾：<name>」)`, or a
  plain reason (「手機的儲存空間不足」, 「手機連線中斷，無法傳送」, 「傳送逾時…」…).
* **Install APK** (`installApk`): `.apk` only (`.apks/.xapk/.aab` → 「這是分割
  安裝包…」); `adb install -r <file>` (timeout 3 min + 1 s per 256 KB);
  success = exit 0 + `Success` → 「已安裝到手機：<file>」; else
  `explainInstallFailure`: INSTALL_FAILED_VERSION_DOWNGRADE, UPDATE_INCOMPATIBLE
  / signatures, INSUFFICIENT_STORAGE, OLDER_SDK, DEPRECATED_SDK_VERSION,
  NO_MATCHING_ABIS, MISSING_SPLIT, INVALID_APK / PARSE_FAILED, USER_RESTRICTED
  (Xiaomi 「USB 安裝」), ABORTED / CANCELED, VERIFICATION_FAILURE (Play Protect),
  DUPLICATE_PERMISSION, TEST_ONLY, … → one sentence each; timeout → 「安裝逾時：
  請看手機畫面，是否在等你按「安裝」或「允許」」; unknown → 「安裝失敗：<code>」.

## API (`android/include/pm/android_source.h`)

As specified, plus the constructor/destructor and a public `Events events;`
member (the spec's `Events` struct had no setter). Set `events`/`log` before
`init()`. Every call returns quickly; slow work runs on one internal worker
thread in order. **Events and `log` come from internal threads** — post to
the UI thread.

| call | effect / states |
|---|---|
| `init(toolsDir)` | checks `adb.exe` + `scrcpy-server`; warms up the adb server in the background (~2 s) |
| `beginQrPairing(bgra, size, text)` | QR now; WaitingForPairing (10 min max) → Pairing → Connecting → `onConnected` |
| `pairWithCode(L"ip:port", L"123456")` | false only for malformed input; Pairing → Connecting → `onConnected` |
| `cancelPairing()` | → Idle 「已取消配對」 |
| `connectKnownDevices()` | Connecting → `onConnected`, or Idle 「…沒有找到…」/「…尚未配對…」 |
| `start(video, audio)` | needs a connected phone; Connecting → Mirroring. Call it from `onConnected` (state stays Connecting until then) |
| `stop()` | synchronous teardown → Idle |
| `sendPointer / sendKey / press*` | non-blocking, ignored unless mirroring |
| `pushToGallery(path, done)` | 傳到手機: `adb push` to `/sdcard/Pictures/ZizaiCast/` (videos: `Movies/ZizaiCast/`, ASCII name `ZizaiCast_…`), then MediaProvider `scan_file`, the `MEDIA_SCANNER_SCAN_FILE` broadcast, a `content query` check and `scan_volume` if still not listed; own thread, one at a time; false without a connected phone / file / while busy. `galleryPath(name)` = the phone path. Details and fake-adb tests: docs/share.md |
| `setClipboardSync(on)` (0.8.3) | default on; phone→PC always, PC→phone only after the user controlled the phone; Ctrl+V / Ctrl+C / Ctrl+X as above |
| `pushFile(path, done)` (0.8.3) | → `/sdcard/Download/<name>`; `done(ok, msg)` on the transfer thread |
| `installApk(path, done)` (0.8.3) | `adb install -r`; `done(ok, msg)` with a plain reason on the transfer thread |
| `setPhoneScreenOn(on)` (0.8.3) | SET_DISPLAY_POWER now and for later sessions; on again at `stop()` / by the server's cleanup |
| `setStayAwake(on)` (0.8.3) | `stay_awake` + `keep_active` for the next server (restart in place while mirroring); restored by the server |
| `connectUsb()` (0.8.3) | like `connectKnownDevices()` for the USB device: Connecting → `onConnected`; false if not init / busy |
| `events.onNotice(text)` (0.8.3) | something to tell the user while mirroring goes on (no audio, permission hint) |

**Threads of the 0.8.3 callbacks**: `pushFile` / `installApk` call `done`
exactly once, on one internal transfer thread (FIFO), never inside the
call and never with a lock of `AndroidSource` held — `PostMessage` it to the
UI thread. Queued transfers are cancelled by the destructor
(`done(false, 「已取消」)`, same thread). `onNotice`, like all events, comes
from internal threads.

Errors → `State::Error` with a zh-TW detail (e.g. 「配對失敗：…」,
「等候逾時：沒有偵測到手機掃描 QR 圖碼」). App wiring: `setPointerHandler →
sendPointer`, `setKeyHandler → sendKey`.

## Third-party components (not in git except qrcodegen)

`android/third_party/fetch_tools.ps1` downloads and verifies (idempotent;
CMake runs it at configure time when files are missing,
`-DPM_ANDROID_FETCH_TOOLS=OFF` to disable). `pm_android_copy_tools(<target>)`
copies them to `<exe dir>\android-tools\` (installer: ship that folder).

| component | version | source | SHA-256 | license |
|---|---|---|---|---|
| adb.exe, AdbWinApi.dll, AdbWinUsbApi.dll, NOTICE.txt | platform-tools **r37.0.1** (adb 1.0.41, 37.0.1-15733141) | `https://dl.google.com/android/repository/platform-tools_r37.0.1-win.zip` | zip `45f4d63113e895ebde0c90f194099a4676b6ac653bd28d54314a9e022bbc1a99` (SHA-1 `e03e78b1…1110` = Google's repository2-3.xml) | adb is AOSP code under Apache-2.0 (NOTICE.txt shipped alongside). The zip is distributed by Google under the Android SDK License; scrcpy's own Windows releases bundle the same files. If the owner prefers zero redistribution, the installer can run `fetch_tools.ps1` at install time instead. |
| scrcpy-server | **v5.0** (versionCode 50000) | `https://github.com/Genymobile/scrcpy/releases/download/v5.0/scrcpy-server-v5.0` | `26cbc9ad0aced6c2282455bef4fb43462605c1f8758c74b4ab1dbf818c229daa` (= release SHA256SUMS.txt) | Apache-2.0 (LICENSE downloaded next to it) |
| qrcodegen (C++) | v1.8.0 | `android/third_party/qrcodegen/` (vendored) | — | MIT (`LICENSE`) |

The client sends `5.0` as the server version; it must match the pushed
server exactly (protocol studied from scrcpy v5.0, commit 19871982, cloned
in `_ref/scrcpy`, gitignored). Upgrading = bump both in `fetch_tools.ps1`
and `scrcpy::kServerVersion`.

## Tests (no phone): `build-android\bin\Release\pm_android_test.exe [--live-mdns N]`

0.8.3 (2026-10-21): `pm_android_test --no-adb --no-mdns --no-window` → **319
checks, all passing** (5 runs of the new section in a row, no flake);
`--only-083` runs just the new parts (+5 checks without `--no-mdns`). New:

* backoff (doubling, cap, ±20 % jitter bounds);
* GET_CLIPBOARD / SET_CLIPBOARD (incl. the 256 KiB limit, UTF-8-safe) /
  SET_DISPLAY_POWER / RESET_VIDEO bytes = scrcpy v5.0's own test vectors;
  device messages (clipboard incl. 「手機」, ack, uhid) in 1/2/7-byte and
  whole chunks, bad type / oversize rejected;
* the plain-language tables (install codes, push errors, server lines),
  Download names (`測試 檔案.txt`, `a:b*c?.pdf`, numbering), shell quoting,
  server arguments;
* **AndroidSource against a fake phone**: `pm_android_fake_adb` scripted by
  files (`offline`, `devices.txt`, `push_fail`, `install_result`,
  `server_error`, …) and its `forward` pointing at an in-test fake
  scrcpy-server on 127.0.0.1 (video + audio + control): mirroring; phone →
  PC clipboard; PC → phone **not** sent while only watching, sent after a
  click, never echoed; sync off both ways; screen off → `[10,0]`, `stop()` →
  `[10,1]` as the last message, re-applied to the next session; stall →
  RESET_VIDEO → restart without `onDisconnected`; server crash → restart;
  stay-awake → restart with `stay_awake=true keep_active=true`; Wi-Fi lost
  → Idle 手機連線中斷 → attempts 1, 2, 3 with growing gaps (e.g. +296 /
  +1188 / +1562 ms with the test's 300 ms base) → `adb disconnect` of the dead
  transport → back to Mirroring; gone for good → 「…重新連線失敗…」 after 4;
  `stop()` cancels the loop; capture error → one restart → Error
  「手機無法錄製畫面…」, no reconnect; `pushFile` with a Unicode name taken →
  「測試 檔案 (1).txt」, progress logged, then install OK / downgrade / not an
  APK / disk full, all five `done` on one non-caller thread in order; USB
  unauthorized → prompt text → device → mirroring with `-s R5CT123ABC`; no
  USB phone → Idle; adb idle shutdown (start → stop after idle → start on
  demand → kept while a phone is connected → stopped after `stop()`); the
  periodic scan with adb stopped and no phone advertising does not start adb.
  Clipboard = an in-memory backend (never the real one), no window, no
  sound, loopback only.

Before 0.8.3 — 195 checks, all passing (2026-10-07):

* control messages byte-identical to scrcpy v5.0's own
  `test_control_msg_serialize.c` vectors (keycode, text + 300-byte UTF-8-safe
  truncation, touch, scroll, back);
* stream parser on synthesized v5 streams (session 1080×2340, SPS/PPS
  config, IDR, P, rotation → session 2340×1080 + new config) fed in 1-byte
  and 49 random chunkings; audio AAC; audio-disabled (0), config error
  (1), zero length, PTS/flag bits;
* input translation (touch down/move/up mapping, back/home buttons, wheel,
  text, Ctrl+C keycode with meta, Enter/Backspace + repeat, IME 中, 😀);
* QR: credential format/randomness, finder patterns, BGRA layout, and the
  image **decoded back by OpenCV** (`android/tools/verify_qr.py`) to the exact
  `WIFI:T:ADB;…;;` text;
* DNS build/parse incl. compression pointers, loops, truncation; **mDNS
  browse loop** against a loopback legacy-unicast responder (resolved in
  ~30 ms); `--live-mdns 4` on the real LAN resolved 5 services (the running
  自在投影 `_airplay._tcp` with its Chinese instance name, a BRAVIA, a Denon,
  a Mi TV `_googlecast._tcp`) → multicast path works on this PC;
* adb on a private port: `version`, `start-server` (~2.1 s) listening on
  :15099 while 5037 stays untouched, `devices -l`, `mdns check`/`services`,
  timeout and cancel kill a hung `wait-for-device`, spawn+kill,
  `kill-server` closes the port; `~AndroidSource` closes its port too;
* scrcpy session vs a **fake scrcpy-server** (first connection closed
  without dummy byte like an un-listened adb forward → retry; dummy byte;
  UTF-8 device name 「Pixel 9 測試」; video/audio streams in 5-byte writes;
  touch + text control bytes received exactly; video EOF → `onEnded` once);
* AndroidSource API without a phone (QR → WaitingForPairing → cancel → Idle,
  input validation).

Not testable without a phone: the real pairing handshake, the phone's mDNS
records, scrcpy-server on a device, H.264/AAC from MediaCodec in
VideoWindow/AudioPlayer, touch accuracy and keyboard on Android, and for
0.8.3: real clipboard text in both directions (incl. Android 10+ clipboard
restrictions), `SET_DISPLAY_POWER` / `keep_active` on real panels, the
server cleanup restoring the screen / stay-on after a cable pull or Wi-Fi
loss, real `adb install` failure texts per brand, USB driver / RSA prompt,
and whether a phone whose screen times out keeps sending repeated frames
(if not, the stall restart wakes it: scrcpy's `power_on` default).

## adb server idle cost — before / after (0.8.3)

Measured on this PC (24 logical CPUs, Windows 11 26300), adb 1.0.41
(platform-tools r37.0.1) on a private port, no phone attached
(`build-droid\measure_adb_idle.ps1`, `measure_after.ps1` with
`pm_android_test --measure-idle 40`):

| | before (≤ 0.8.2: server up for the whole app lifetime) | after (0.8.3) |
|---|---|---|
| adb server while idle | 17.9 MB working set (7.0 MB private), 10 threads, 253 handles, 31 ms CPU per minute (0.05 % of one core), wakes all the time | **none** 15 s after the last use: 0 MB, 0 threads, 0 CPU |
| app started, never paired / auto-reconnect off | server from start to exit | server ~17 s (warm-up + 15 s), then gone |
| paired, idle, auto-reconnect scan every 45 s, no phone around | server always up | adb not started by the scans (our mDNS finds no phone); every 5th scan (~4 min) a full adb scan (~2 s start + 6 s + 15 s) |
| cost of starting it again | — | ~2.1 s until `start-server` returns, ~130 ms CPU |

## Owner test procedure (needs the phone)

PC: build (`cmake --build build-android --config Release --target
pm_android_demo`), PC and phone on the **same Wi-Fi/LAN** (guest Wi-Fi with
client isolation will not work). Allow `adb.exe` through Windows Firewall if
asked (private network).

1. **開發人員選項**: 設定 → 關於手機 → 軟體資訊 → tap 「版本號碼」 7 times
   (Samsung: 關於手機 → 軟體資訊 → 版本號碼; Pixel: 關於手機 → 版本號碼)
   → enter the lock-screen PIN → 「您現在已成為開發人員」.
2. 設定 → 系統 → 開發人員選項 (Samsung: 設定 → 開發人員選項) → turn on
   **無線偵錯** → 「允許在這個網路上使用無線偵錯？」 → 允許.
3. PC: run `build-android\bin\Release\pm_android_demo.exe --qr`. A QR code is
   printed in the console and opened as an image.
4. Phone: tap **無線偵錯** (the text, not the switch) → **使用 QR 圖碼配對裝置**
   → scan the QR. Expected console: `[1] 請用手機掃描…` → `[2] 正在配對
   192.168.x.x…` → `[3] 配對成功，正在連線…` → `connected: <phone name>` →
   `[4] 正在投影：<name>`; the phone shows the PC under 「配對的裝置」.
5. Check: picture appears within ~3 s; rotate the phone (window picture
   follows); play a video/music (sound on the PC — Android 11+; Android 11
   may need the phone unlocked when mirroring starts); click = tap, drag =
   swipe, wheel = scroll, right click = 返回, middle click = 主畫面; click a
   text field and type English and 中文 (Windows IME) → text appears;
   Ctrl+A / Backspace / Enter / arrows work.
6. Close the window → mirroring stops (phone keeps 無線偵錯 on).
7. **Reconnect**: run `pm_android_demo.exe` (no arguments) → it finds the
   paired phone (no QR) and mirrors. Also try: toggle 無線偵錯 off/on on the
   phone, run again → reconnects (the port changes; mDNS finds it).
8. **Disconnect**: while mirroring, turn off 無線偵錯 or Wi-Fi on the phone →
   `disconnected; trying to reconnect`, the window returns to idle.
9. **Code fallback**: phone 無線偵錯 → **使用配對碼配對裝置** → run
   `pm_android_demo.exe --code <IP:port shown> <6-digit code>`.

Please send back the console output (it includes adb and `[server]` lines)
on any failure. Known limits: Android 10 and older have no Wi-Fi pairing —
connect them with a USB cable instead (`connectUsb()`, 「USB 偵錯」 on, allow
the PC on the phone; same mirroring and control, Windows may need the
phone maker's USB driver); audio needs Android 11+; some
phones (Xiaomi/MIUI) also need 「USB 偵錯（安全性設定）」 enabled for touch/
key input (and 「USB 安裝」 for `installApk`); 無線偵錯 turns itself off when
the phone changes Wi-Fi.

10. **USB (0.8.3)**: plug the phone in, 開發人員選項 → **USB 偵錯** on, call
    `connectUsb()` (app menu from feat-ux) → the phone asks 「允許 USB 偵錯？」
    → check 「一律允許這部電腦」 → 允許 → mirroring as over Wi-Fi.
11. **0.8.3 extras** while mirroring: copy text on the phone → paste on the
    PC; click the mirror once, copy on the PC → paste on the phone (Ctrl+V on
    the mirror); screen off → phone panel dark, picture continues; 中斷連線 →
    panel on again; pull the cable / turn Wi-Fi off with the screen off →
    the panel must come back on by itself; stay awake on → the phone does
    not lock while mirroring, and its screen timeout is as before after
    disconnecting; send a file → in 「檔案」 → Download; install an APK (also
    an older version of an installed app → the downgrade sentence).
