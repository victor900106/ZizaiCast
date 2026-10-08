# Miracast receiver (`miracast/`) — Android → 自在投影

`pm_miracast` lets Android phones (Samsung Smart View, Xiaomi / OPPO / vivo
「投放」/「無線投影」, Pixel-era 「投放」 on phones that still speak Miracast,
another Windows PC's 「投影 → 連線到無線顯示器」 …) cast to this PC. The phone
sees the PC as **自在投影**; the picture appears in our own `pm::VideoWindow`.

Windows does all of the protocol work (Wi-Fi Direct group, RTSP/WFD
negotiation, HDCP, RTP/MPEG-TS, H.264 decode, audio) through the Windows
Runtime API `Windows.Media.Miracast` (Windows 10 2004+). We only own
the session and the rendering.

```
phone ──Wi-Fi Direct──► Windows Miracast stack (CastSrv / MiracastReceiver.dll)
                          │ MiracastReceiverSession.MediaSourceCreated
                          ▼
        Windows.Media.Playback.MediaPlayer (frame-server mode, real-time playback)
            │ VideoFrameAvailable (MF thread)        └─► audio: played by the MediaPlayer
            ▼                                              (default output device)
   CopyFrameToVideoSurface → our B8G8R8A8 render target (own D3D11 device)
            → CopyResource into a 3-slot staging ring + event query
   reader thread: newest finished slot → Map → black-bar crop
            → VideoWindow::submitBgraFrame(bgra, w, h, stride, ptsNs) → Unmap
```

## API (`miracast/include/pm/miracast_receiver.h`)

```cpp
pm::MiracastReceiver rx;
rx.log = [](const std::string& s) { ... };          // optional
pm::MiracastReceiver::Events ev;
ev.onStatus = [](Status s, const std::wstring& detail) { ... };
ev.onConnected = [](const std::wstring& phone) { ... };   // picture is flowing
ev.onDisconnected = [] { ... };
ev.onPin = [](const std::wstring& pin) { ... };     // "" = hide the PIN
if (!rx.start(L"自在投影", &window, ev)) { /* onStatus already explained why */ }
...
rx.disconnect();   // end the current cast, keep listening
rx.stop();         // stop listening (also in the destructor)
rx.setVolume(0.5); // cast audio volume 0..1 (extra to the agreed API)
std::wstring why = pm::MiracastReceiver::unsupportedReason(); // "" = supported
```

* `Status`: `Unavailable` (this PC can't: see the reason text), `Disabled`
  (supported but not listening: policy, another receiver owns Miracast, Wi-Fi
  off, or after `stop()`), `Idle` (listening — visible on phones),
  `Connecting` (detail = phone name), `Connected` (detail = phone name).
* All `detail` / reason strings are user-facing zh-TW sentences.
* Threading: `start/stop/disconnect` from any thread. Events and `log` run on
  Windows thread-pool / Media Foundation threads, never under our locks —
  post them to the UI thread. `start()` returns after the session started
  (~0.1–0.5 s, it calls WinRT synchronously; don't call it from inside a
  window procedure that must stay responsive if that matters).
* Video goes to `window->submitBgraFrame()`; when a cast ends (phone stops,
  `disconnect()`, `stop()`) the pump calls `window->onReset()` (only if a
  picture was shown). The app should call `VideoWindow::setConnecting(name)`
  on `Connecting` and `showPin(pin)` on `onPin` itself.
* Settings applied by `start()`: FriendlyName = the given name, ModelName
  「自在投影」, ModelNumber "PhoneMirror", AuthorizationMethod
  `PinDisplayIfRequested` (no prompt; a PIN only if the phone insists — it
  arrives through `onPin`), `RequireAuthorizationFromKnownTransmitters=false`,
  session `MaxSimultaneousConnections=1`, `AllowConnectionTakeover=true` when
  the driver supports it (a second phone replaces the first).

## Rendering details

* Frame server: `MediaPlayer.IsVideoFrameServerEnabled = true`,
  `RealTimePlayback = true` (low latency), `CommandManager.IsEnabled = false`
  (no system media-controls overlay for the cast).
* `VideoFrameAvailable` runs on a Media Foundation thread: `NaturalVideoWidth/
  Height` → (re)create the target + ring on size change →
  `CopyFrameToVideoSurface` (the MediaPlayer blits/converts NV12→BGRA on our
  device) → `CopyResource` into a free staging slot → `End(event query)` →
  `Flush`. It never waits for the GPU; if all three slots are still in use the
  picture is dropped (`framesDropped`).
* Reader thread: polls the slots' event queries (yield, then 250 µs
  high-resolution timer waits — `Sleep(1)` would cost up to 15.6 ms), takes the
  newest finished slot (older finished ones are dropped as superseded), maps it
  and calls `submitBgraFrame` with a pointer into the mapped data (the window
  copies it; no extra CPU copy on our side).
* Pillarbox crop: Android sends its portrait screen inside a 16:9 picture with
  black bars. Each delivered picture is sampled on 17 rows/columns (max(R,G,B)
  > 24 = content); symmetric bars (left≈right / top≈bottom within 8 px, each
  ≥ 2 % of the size) are cropped so the window gets the phone's real aspect
  (rotation of the phone then flips the window like with iPhones). The first
  picture with content sets the crop; it grows immediately when content
  appears outside it and shrinks only after ~30 consecutive agreeing pictures
  (a dark app or a black video frame doesn't make the window jump). All-black
  pictures keep the current crop. `FramePump::setAutoCrop(false)` (internal)
  disables it.
* `ptsNs` = steady_clock (QPC) time when the frame was copied.
* Audio: kept in the MediaPlayer (it plays on the default render device with
  its own A/V sync, which is what keeps lip sync right). `setVolume()` maps to
  `MediaPlayer.Volume`. Consequences: Miracast audio does not go through
  `pm::AudioPlayer`, so the recorder's PCM tap does not see it (recordings of
  Android casts are video-only until an audio frame server path is added) and
  the app's mute/volume must call `rx.setVolume()`.
* UIBC (sending PC keyboard/mouse back to the phone): the API only exposes
  `MiracastReceiverConnection.InputDevices` (keyboard / game-controller
  transmit flags that Windows drives itself, and few phones request UIBC).
  Not wired up.

## Requirements (this is the important part)

### Package identity: not needed so far (final check after a reboot)

On this PC (Windows 11 Pro 26300, unpackaged Win32 exe, no manifest
capabilities; tested from an elevated shell, but nothing in it needs
elevation) these all work without package
identity: `new MiracastReceiver()`, `GetStatus()`, `GetDefaultSettings()` /
`GetCurrentSettings()`, `DisconnectAllAndApplySettings()` (it returns a
*status* — `MiracastNotSupported` here — not `AccessDenied`), and
`CreateSession(nullptr)`. `CreateSession` takes a `CoreApplicationView`, which
desktop apps don't have, and **null is accepted**. No call has returned
`AccessDenied` or `E_ACCESSDENIED`.

**Not yet verified:** `session.Start()` succeeding. Until the feature below is
active it fails with `UnknownFailure` / `E_INVALIDARG (0x80070057)`, because
the receiver isn't available yet (`WiFiStatus = MiracastNotSupported`). The
first `pm_miracast_test --listen` after a reboot settles it:

* `start status 0` / `listening as "自在投影"` → no packaging needed. This is
  the expected result.
* `AccessDenied` (status 3) or `E_ACCESSDENIED` → give the exe package
  identity with a **sparse package (external location)**: a minimal
  `AppxManifest.xml` with `uap10:AllowExternalContent`, the same
  `Identity`/`Publisher` in the exe's `app.manifest` `<msix>` element, a signed
  `.msix` registered by the installer through
  `PackageManager.AddPackageByUriAsync(..., ExternalLocationUri = install dir)`.
  No special capability is documented for `MiracastReceiver`.
* `E_INVALIDARG` again while `WiFiStatus` is Supported → the null view is the
  problem. In that case run the session in a small packaged helper.

WinRT runs on the process MTA (`CoIncrementMTAUsage`), so the app's UI
thread can stay an STA.

### Windows optional feature 「無線顯示器」 (Wireless Display) — REQUIRED

Windows 11 has no Miracast *receiver* until the Feature-on-Demand
**App.WirelessDisplay.Connect** (「無線顯示器」) is installed. Without it, the
`MiracastReceiver` API on capable hardware reports
`WiFiStatus = MiracastNotSupported` (1), `ListeningStatus = NotListening`, and
`ApplySettings` → `MiracastNotSupported` with `0x80070032 ERROR_NOT_SUPPORTED`.
`netsh wlan show drivers` still says `Wireless Display Supported: Yes`.

* Install (user): 設定 → 系統 → 選用功能 → 檢視功能 → 「無線顯示器」 → 新增
  (download from Windows Update, ~250 MB on 26100/26300, a few minutes).
  Admin: `Add-WindowsCapability -Online -Name App.WirelessDisplay.Connect~~~~0.0.1.0`
  or `DISM /Online /Add-Capability /CapabilityName:App.WirelessDisplay.Connect~~~~0.0.1.0`.
  **A reboot is required** (`RestartNeeded: True`; the state stays
  `InstallPending` until then).
* Detection without admin (`src/wireless_display.cpp`): HKLM
  `…\Component Based Servicing\Packages\Microsoft-Windows-WirelessDisplay-FOD-Package~…~~<ver>`
  `CurrentState` — 0x70/0x80 installed, 0x60/0x65 install pending (reboot),
  otherwise not installed (0x40 staged, 0x50 superseded). `unsupportedReason()`
  only checks this when `WiFiStatus` isn't Supported, and returns
  「請先安裝 Windows 選用功能「無線顯示器」…」 or 「…已安裝，請重新開機…」.
* The installer could offer to add the feature (needs admin + internet +
  reboot). That is a decision for the owner and is not done here.

On this PC the feature was **installed during this work (2026-10-07) and is
waiting for a reboot**, so the end-to-end receiver could not be started yet.

### Hardware / drivers

The Wi-Fi adapter and the display driver must both support Miracast (Wi-Fi
Direct + WDDM 1.3 Miracast). `netsh wlan show drivers` →
`Wireless Display Supported: Yes (Graphics Driver: Yes, Wi-Fi Driver: Yes)`.
The Wi-Fi adapter must be enabled (it does not have to be connected to any
network — this PC uses Ethernet and Wi-Fi is disconnected). Desktop PCs
without Wi-Fi cannot receive Miracast at all (`unsupportedReason()` tells the
user to use USB instead).

### Coexistence

* **AirPlay**: no port overlap. AirPlay listens on TCP 7000 / 7100 (+ mDNS
  5353, NTP/timing UDP) on all interfaces. Miracast runs on the Wi-Fi Direct
  (P2P) virtual adapter that Windows creates per session: the *phone* (WFD
  source) is the RTSP server on TCP 7236, the PC connects to it; RTP arrives
  on a UDP port chosen by Windows; MS-MICE (Miracast over infrastructure,
  Windows senders only) uses TCP 7250. Nothing in the Miracast stack touches
  7000/7100, so both receivers can run in one process. Checked here: the
  running 自在投影 holds TCP 7000, and no process listens on 7236/7250. Running
  both at once still needs the post-reboot test. They share one `VideoWindow` — the app must
  decide who owns the window (e.g. treat a Miracast connection like an
  AirPlay takeover).
* **Windows' own receiver** (Settings → 系統 → 投影到此電腦 / the 「無線顯示器」
  (Connect) app): only one Miracast receiver can listen at a time. If the
  Connect app is open, or 「投影到此電腦」 is set to 「隨處可用」 and Windows
  started its own receiver, our receiver reports
  `ListeningStatus = TemporarilyDisabled`. That maps to `Status::Disabled` with
  「…可能是 Windows「連線」應用程式或其他投影接收程式正在使用…」. Any status
  change arrives through `StatusChanged`, so when the other receiver goes away
  we switch back to `Idle`. `DisconnectAllAndApplySettings` also changes the
  *system* receiver settings: the PC name the Connect app advertises becomes
  「自在投影」, and it stays that way after we exit. Recommendation for the app:
  if the status is Disabled/TemporarilyDisabled, show 「請關閉 Windows「連線」
  應用程式」. Leave the user's 「投影到此電腦」 setting alone. (Exact
  behaviour with the Connect app still needs checking after the reboot.)
* Wi-Fi Direct may force the Wi-Fi adapter onto the phone's channel; on
  single-radio adapters an active Wi-Fi network connection can briefly
  degrade while casting. Not an issue on this Ethernet PC.

## Test tool `pm_miracast_test`

```
pm_miracast_test --status
pm_miracast_test --listen [--name 自在投影] [--seconds N]
pm_miracast_test --file sample.mp4 [--seconds N] [--snapshot out.png] [--no-crop]
```

* `--status`: `unsupportedReason()` + raw `WiFiStatus / ListeningStatus /
  takeover / current settings`. Exit 0 = supported, 2 = not.
* `--listen`: real receiver with a VideoWindow, logs every status/connection
  event with timestamps. Cast from a phone.
* `--file`: plays a local file through the exact same `FramePump`
  (MediaPlayer frame server → ring → `submitBgraFrame`) and prints frame
  counts, drop count, crop rectangle, MF-thread copy cost and read-back
  latency every second. Exit 0 if pictures were delivered.

Sample files (ffmpeg):

```
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 8 -c:v libx264 -pix_fmt yuv420p -c:a aac sample.mp4
ffmpeg -f lavfi -i testsrc2=size=608x1080:rate=60 -t 6 -vf "pad=1920:1080:656:0:black" -c:v libx264 -pix_fmt yuv420p pillar.mp4
```

### Results on this PC (RTX 3060 Ti, Intel AX201)

* `--file sample.mp4` (1280×720 30 fps H.264 + AAC): 30 pictures/s delivered,
  0 dropped, 0 copy errors, MF-thread cost ≈ 1.5–2 ms/frame
  (CopyFrameToVideoSurface + CopyResource), copy→mapped read-back ≈ 0.7 ms
  (9 ms with `Sleep(1)` polling, which is why the wait uses the
  high-resolution timer), VideoWindow presented every picture, and the PNG
  snapshot has correct colours (BGRA order verified). The 440 Hz tone played
  through the MediaPlayer.
* `--file pillar.mp4` (608×1080 portrait inside 1920×1080, 60 fps): 60
  pictures/s, 0 dropped, crop `654,0 610×1080` from the first picture, window
  shows 610×1080 (portrait).
* `--status` before the feature: `WiFiStatus=1 (NotSupported)
  ListeningStatus=0`, current settings `name="v900106"` (the PC name),
  `auth=2`. After installing the feature (before the reboot): reason
  「…已安裝，請重新開機…」. `--listen` → `Unavailable` with the same reason.
* Still to do after the reboot: `--status` (expect WiFiStatus 3 Supported),
  `--listen` (expect `listening as "自在投影"`, status Idle), then the phone
  test below.

## Owner test procedure (Samsung / Xiaomi)

0. Once: **reboot the PC** (the 「無線顯示器」 feature installed on 2026-10-07 is
   pending). Then `pm_miracast_test --status` must print
   `unsupportedReason: (none: supported)` and `WiFiStatus=3`. On other PCs,
   install the feature first (see Requirements).

Before: the PC's Wi-Fi must be **on** (connected or not); phone and PC do not
need to be on the same network (Wi-Fi Direct). If Windows' own 「投影到此電腦」
is set to 「隨處可用」, set it to 「一律關閉」 first (so only 自在投影 shows up).

1. On the PC: `build-miracast\bin\Release\pm_miracast_test.exe --listen`
   (or the app once integrated). Expect the log
   `miracast: listening as "自在投影"` and `EVENT status Idle`.
2. **Samsung (One UI)**: swipe down twice → 快速設定 → **Smart View** → wait for
   「自在投影」 in the list → tap → 「立即開始」.
   **Xiaomi / Redmi / POCO (HyperOS / MIUI)**: 快速設定 → **投放** (Cast) (older
   MIUI: 設定 → 連線與共享 → 投放) → enable → choose 「自在投影」.
   **OPPO / realme / OnePlus (ColorOS)**: 快速設定 → **螢幕投放** / 設定 → 連線與共享 →
   螢幕投放. **vivo (OriginOS / Funtouch)**: 快速設定 → **投屏** / 設定 → 其他網路與連線 →
   投屏.
3. Expect: `EVENT status Connecting <phone>` within ~2–5 s, then
   `EVENT status Connected` + `EVENT connected <phone>` and the phone's screen
   in the window (portrait, no black side bars), sound from the PC speakers.
   If the phone asks for a PIN, the window shows it (`EVENT pin "1234"`) —
   type it on the phone.
4. Rotate the phone to landscape (e.g. a video in full screen): the window
   should follow within ~1 s.
5. Stop casting on the phone (Smart View → 中斷連線): `EVENT disconnected`,
   `EVENT status Idle`, window back to the idle screen. Cast again — it
   must reconnect without restarting the tool.
6. Takeover: while one phone casts, cast from a second phone (if the driver
   supports takeover the second replaces the first).
7. Coexistence: run the AirPlay app at the same time and mirror an iPhone
   before/after an Android cast.
8. Send back `pm_miracast_test` console output for any failure (the status
   lines include the WiFi/listening states and HRESULTs).

Known phone-side limits: Google Pixel and stock Android 6+ removed Miracast
(「投放」 there is Google Cast only — those phones will never see 自在投影 via
Miracast; use the USB/scrcpy path). Some Samsung models show the PC only if
「Smart View → 設定 → 手機螢幕畫面 / 裝置」 allows other devices. Netflix /
DRM apps may show black (HDCP / app policy).
