# Android source (`android/`) — 無線鏡像 + 控制 Android 手機

`pm_android` (static lib) mirrors an Android phone to the PC and lets the
mouse/keyboard control it, scrcpy-style, over Wi-Fi only. The PC shows a QR
code; the phone scans it in 開發人員選項 → 無線偵錯 → 使用 QR 圖碼配對裝置
(Android 11+). After the first pairing the phone reconnects automatically
whenever 無線偵錯 is on and it is on the same LAN.

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
  `adb connect`s each phone found; only paired phones accept.
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
  clipboard_autosync=false` (server log lines → `log` as `[server] …`; the
  server deletes its jar on exit). Connect video socket and wait for the
  dummy byte (adb accepts before the server listens; up to 100×100 ms), then
  audio + control sockets; read the 64-byte device name.
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
  The control socket's device→PC messages are drained and ignored
  (clipboard sync off).
* **End of stream**: video socket EOF → session torn down on the worker
  (sockets closed, server killed if still alive, forward removed),
  `VideoSink::onReset()`, `AudioSink::onFlush()`, state Idle 「手機連線中斷」,
  `onDisconnected()`.

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

195 checks, all passing (2026-10-07):

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
VideoWindow/AudioPlayer, touch accuracy and keyboard on Android.

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
on any failure. Known limits: Android 10 and older have no Wi-Fi pairing
(needs a USB cable once — not supported); audio needs Android 11+; some
phones (Xiaomi/MIUI) also need 「USB 偵錯（安全性設定）」 enabled for touch/
key input; 無線偵錯 turns itself off when the phone changes Wi-Fi.
