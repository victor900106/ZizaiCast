# App shell (`app/`) — 自在投影.exe

`app/main.cpp` wires three sources — `pm::AirPlayServer` (core, iPhone),
`pm::MiracastReceiver` (miracast, Android 投放) and `pm::AndroidSource`
(android, wireless debugging + scrcpy) — to `pm::VideoWindow` (video) and
`pm::AudioPlayer` (audio), feeds `pm::Recorder` (recorder) while recording,
and adds the desktop-app behaviour below.
Target `PhoneMirror`, output name **自在投影.exe**, version 0.6.0 (manifest
`app/res/app.manifest`: Common Controls v6 + PerMonitorV2; version
resource in `app/res/app.rc`, numbers from `PM_APP_VERSION` in
app/CMakeLists.txt; `-DPM_APP_VERSION_OVERRIDE=x.y.z` builds a fake
version for updater tests). Links `pm_core pm_video pm_audio
pm_recorder pm_miracast pm_android` (app/CMakeLists.txt adds the miracast /
android subdirectories itself if the top-level list does not).

## Command line

| flag | meaning |
|---|---|
| `--background` | start hidden in the tray (used by the autostart entry) |
| `--name "X"` | display name for phones (AirPlay + Miracast); default `display_name` in settings.ini, else 自在投影 (繁體中文) / Zizai Cast (English) |
| `--h265` | advertise HEVC mirroring |
| `--debug` | forward the protocol library's debug log |
| `--dev` | developer instance: own mutex, OS-chosen ports (`legacyPorts=false`), name "自在投影 (測試)" / "Zizai Cast (Test)", data in `%LOCALAPPDATA%\PhoneMirror-dev` — runs next to an installed copy |
| `--no-hevc` | with `--dev` only: behave as if no HEVC decoder were installed (to test the greyed 畫質 items) |
| `--test-feed F.h264` | with `--dev` only: fake phone 「連線測試機」 1 s after start; the Annex-B H.264 file is looped at 30 fps into the app's video sink (no audio) |
| `--test-seconds N` | with `--test-feed`: after N s the video stops and a disconnect is posted (→ 連線中斷 hold) |
| `--test-takeover S` | with `--test-feed`: at S s the sinks are reset and a takeover by 「第二支 iPhone」 is posted |
| `--test-source android` | with `--test-feed`: the file comes from a fake Android phone 「Galaxy S24」 through the Android path (arbitration, gates, nav menu; the pointer / key handlers log taps and keys instead of sending them); no adb, no auto-reconnect |
| `--test-source miracast` | `--dev` only, no file needed: a fake Miracast cast 「Galaxy S24」 of generated BGRA pictures via `submitBgraFrame` (claimed like a real cast; `--test-seconds` ends it); the real receiver is not started. With `--test-feed F` as well, a fake iPhone mirrors F from 1 s and the cast comes at `--test-delay` (takeover / refusal test) |
| `--test-delay S` | the fake phone connects after S s instead of 1 s (e.g. to try a takeover against a real phone) |
| `--test-stall S` | with `--test-feed`: at S s the fake phone stops sending without a reset (a phone that left the Wi-Fi: exercises the Android watchdog) |
| `--test-no-install` | `--dev` only: an update (local or downloaded + SHA-256 checked) stops right before the installer would run (log `test-no-install: would install …`, toast 「（測試）vX 已就緒，未安裝」) — for testing against the real manifest |
| `--test-pair-timeout S` | `--dev` only: the pairing panel's 「沒有收到手機回應…」 after S s instead of 45 s |
| `--demo-branding` | `--dev` only, for README / site screenshots: the display name, tray-menu header and About version drop the 「 (測試)」 / “ (Test)” / 測試版 / “Dev build” labels (ports, data folder and mutex stay the dev ones) |
| `--test-offscreen` | `--dev` only, for scripted screenshots next to someone using the PC: the window never takes the foreground or the z-order (`bringToFront` only shows it, without activation), the pairing panel / About window are centred over the window even off the desktop and shown without activation, and (0.6.0) no tray icon / balloons, the tutorial, links and folders are only logged (no browser / Explorer), an update installer runs `/VERYSILENT`. Start the process with `STARTUPINFO` `STARTF_USEPOSITION` far off-screen (or `PM_VIDEO_OFFSCREEN=1`, inherited by an update installer and the app it restarts), post input, capture with `PrintWindow(PW_RENDERFULLCONTENT)`; the panels are drawn into PNGs by DevCommand 902 |

With `--dev`, the registered message `PhoneMirror.DevCommand` (wParam = a
`Command` id from main.cpp) runs that menu command — used by test scripts for
screenshots. `wParam` 900 renders the tray and context menus as they would
open right now into `%TEMP%\pmshots\menu_tray.png` / `menu_context.png`
(`pm::ui::renderMenuPng`: no window, no mouse capture; `lParam` − 1 = tray row
to highlight); 901 also the 設定 and 語言 submenus (`menu_settings.png`,
`menu_language.png`); 902 draws the open pairing panel / About window into
`pair.png` / `about.png` (`pm::ui::renderToPng`, a software Direct2D target —
`PrintWindow` sees nothing of their HWND render targets off-screen; the
pairing panel's edit boxes are drawn with their text / cue banner). Command
ids 0.6.0: 138 語言 自動, 139 繁體中文, 140 English, 141 關於.

A second launch does not start another receiver (AirPlay ports are fixed): it
brings the running window to the front (registered message
`PhoneMirror.ShowWindow`), or exits silently with `--background`.

## Window and tray

* Tray icon (投投, `app/res/app.ico`). Double-click / click = show window.
  Tray menu: header 「自在投影」 (app icon, version), [更新到 vX.Y.Z (bold, ↑) +
  separator, while an update is on offer], 顯示視窗, [中斷連線（name）
  Ctrl+D while a phone is live or an iPhone is connecting],
  連接 Android（掃 QR）, 使用教學, 開始錄影 / 停止錄影, 開機自動啟動 ✓, 連線需要
  PIN 碼 ✓, 接受 Android 投放（Miracast） ✓ [+ reason notes], 自動連線已配對的
  Android ✓, 畫質 標準 / 高 · 建議 / 最高 (radio), 新手機連線時 接手 / 保持目前
  (radio), 主題 ▸, 語言 / Language ▸, 開啟截圖資料夾, 開啟錄影資料夾, 檢查更新,
  關於自在投影, 結束.
* Closing the window hides it to the tray (balloon "自在投影仍在背景執行"
  the first time). 結束 in a menu really quits. If the tray is unavailable,
  closing quits.
* Right-click on the window (or Shift+F10 / menu key, or the toolbar's 更多 /
  a right click on the toolbar): [更新到 vX.Y.Z + separator, on offer],
  [while a phone is live: caption 「name（source）」,
  for Android 返回 (右鍵) / 主畫面 (中鍵) / 最近使用, then 中斷連線 (Ctrl+D)],
  全螢幕 (F11), 視窗置頂 (Ctrl+T), 畫面 ▸, 主題 ▸,
  截圖 (Ctrl+S), 開始錄影 / 停止錄影 (Ctrl+R), 開啟截圖資料夾, 開啟錄影資料夾,
  連接 Android（掃 QR）, 使用教學, 設定 ▸ (開機自動啟動, 連線需要 PIN 碼, 接受
  Android 投放（Miracast）, 自動連線已配對的 Android, 畫質, 新手機連線時,
  語言 / Language ▸, 檢查更新), 關於自在投影, 結束.
* Keyboard (window focused, no menu open): F11, Ctrl+D 中斷連線, Ctrl+T,
  Ctrl+S, Ctrl+R 錄影, Ctrl+→ / Ctrl+← rotate, Ctrl+H 左右翻轉, Ctrl+0 還原,
  Ctrl+F iPhone 外框. These Ctrl shortcuts are handled by the subclass before
  the video window, so they never reach an Android phone. **Esc** while an
  Android phone is live and the window is not fullscreen stays forwarded to the
  phone (scrcpy keycode ESCAPE — most apps treat it as back; a PC user expects
  Esc to "go back / dismiss"); in fullscreen Esc leaves fullscreen as before.
  Disconnecting is Ctrl+D, never Esc, so a stray Esc cannot end a session.

## 語言 / Language — 0.6.0

The whole UI exists in 繁體中文 and English.

* **String table**: `include/pm/i18n_strings.inc` — `PM_STR(id, 繁體中文,
  English)`, one line per user-visible string of app/ and video/ (menus,
  toolbar tooltips, toasts, titles, idle hints / check boxes / actions, mascot
  bubbles, PIN card, pairing panel, About, update prompts, the recording
  confirm, message boxes, tray tooltip / balloons). `include/pm/i18n.h`
  (header-only, used by app/ and video/): `enum class S` + table, `tr(id)`,
  `fmt(id, {args})` for `{0}` `{1}` placeholders, `setLang` / `lang` / `en`,
  `uiFont()` (JhengHei UI / Segoe UI) and `localeName()`. Menus, the pairing
  panel and About recreate their text formats when the language changed.
* **Texts from pm_miracast / pm_android** (Miracast reasons, adb pairing
  states) stay Chinese in those modules; `moduleText()` in main.cpp maps them
  through the table's `Mod*` entries (the module's exact text, `{0}` for a
  variable part such as a device name or adb output) when the UI is English.
  `miracastNote()` / `pairFailureText()` recognise them by the same entries.
* **Setting**: settings.ini `language=auto|zh-TW|en` (default auto = Windows
  display language `GetUserDefaultUILanguage`: any Chinese (zh-*) → 繁體中文,
  anything else → English). Menu 設定 → 「語言 / Language」 (also in the tray
  menu): 自動（跟隨 Windows） / 繁體中文 / English (radio; the item's label is
  bilingual in both languages). Read before anything is shown (even the
  "already running" box is in it).
* **Live switch** (`setLanguage`): titles, tray tooltip, idle hints / check
  boxes / actions, toolbar, open pairing panel / About re-labelled at once,
  toast 「語言：English」 / “Language: 繁體中文”; menus are built on open.
* **Display name** (what phones list): `--name`, else settings.ini
  `display_name=` (optional override), else 自在投影 (繁體中文) / Zizai Cast
  (English) (`--dev`: + 「 (測試)」 / " (Test)"). If a switch changes it, the
  AirPlay server is restarted (mDNS re-advertised) and the Miracast receiver
  re-applied; while an iPhone is connected / a cast is live that waits until
  it ends (toast 「投影結束後，手機上的名稱會改成「X」」).
* Product name in the window title, tray, menus' header, About and balloons:
  自在投影 / Zizai Cast. Screenshot / recording files: `自在投影_…` /
  `ZizaiCast_…`; fallback folders `Pictures\自在投影` / `Pictures\Zizai Cast`.
* **Intentional exceptions** (non-ASCII literals left in app/ and video/, all
  identifiers or data, not UI text): install-layout folder names 程式 / 截圖 /
  錄影 / 安裝檔 (shared with the installer, same in both languages), the Run
  value name 自在投影 (shared with the installer), the installer / temp file
  patterns `自在投影-安裝程式-*.exe` / `自在投影-更新-`, the guide file name
  自在投影教學.html, the installer VERSIONINFO product name check (自在投影 or
  Zizai Cast, app/updater.cpp), settings.ini comments, log lines, symbols
  (×, →, ←, …). Brand / model names (iPhone, Android, AirPlay, Miracast, the
  test feed's 「Galaxy S24」) are not translated. Test tools under
  video/tools, miracast/tools, android/tools and the module-internal texts of
  pm_miracast / pm_android (translated by `moduleText`) are outside the
  table. Proof: `grep -nP '"[^"]*[^\x00-\x7F]' app/*.cpp video/src/*.cpp`
  lists only these.

## 關於自在投影 / About Zizai Cast — 0.6.0

`app/about_panel.cpp` (`pm::ui::AboutPanel`, built like the pairing panel:
owned popup, Direct2D, menu palette, draggable top, per-monitor DPI, Esc /
Enter closes): app icon, product name, 「版本 0.6.0 · GPL-3.0」 (dev: 「(測試版)」),
tagline, licence sentence (GPL-3.0, no warranty), 「原始碼與新版本」 link
github.com/victor900106/ZizaiCast (default browser), 「開啟授權資訊資料夾」 (→
`<exe dir>\licenses`, installed `程式\licenses`; dev build: copied there by
app/CMakeLists.txt, else `docs\licenses`), credits (UxPlay, scrcpy, Android
adb, FFmpeg, OpenSSL, libplist, ALAC, pthreads4w, Nayuki QR Code generator)
and 「吉祥物「投投」為本專案原創。」, 關閉. Menu item 「關於自在投影 / About Zizai
Cast」 in both menus (Info icon U+E946).

## Live toolbar and 中斷連線 — 0.5.2

Friends testing Android pairing could not find how to disconnect: a right
click on an Android picture is 返回, so the menu with 中斷 Android 連線 never
opened, and Shift+right click is undiscoverable.

* **Toolbar** (`refreshToolbar()` → `VideoWindow::setLiveToolbar`, drawn by
  video/, see docs/video.md *Live toolbar*): appears at the top centre of the
  picture whenever the mouse moves over the window, hides ~2 s later.
  Buttons: [Android: 返回 · 主畫面 · 最近使用 |] 截圖 · 錄影 (red + pulsing
  dot while recording; tooltip 開始 / 停止錄影) · 旋轉 90° · 全螢幕 / 離開全螢幕
  · 更多 (the full context menu at the cursor) | 中斷連線 (red). Tooltips carry
  the shortcuts. Clicks are posted back as `WM_PM_TOOL` (→ `runCommand`; the
  window is inside its own click handling) and never reach the phone.
  `appProc` re-evaluates the items after every app message, `WM_SIZE`,
  `WM_KEYDOWN`, `WM_TIMER`, `WM_CONTEXTMENU` and dev command (a key string
  avoids redundant updates), so recording / fullscreen / source changes show
  at once. Empty (no toolbar) when nothing is live.
* **中斷連線** (`disconnectLive()`, `CmdDisconnect`; toolbar, Ctrl+D, context
  menu, tray 「中斷連線（name）」): the live source is `g_active`, or AirPlay
  while an iPhone is connecting / asking for its PIN (`g.sessionActive`). A
  recording is saved first (toast 「已中斷連線：name（錄影已儲存）」, else
  「已中斷連線：name」); the window goes to idle at once (no 連線中斷 hold).
  - AirPlay: the core has no per-client disconnect, so — like refusals —
    `releaseSource`, `StatusVideoSink::forceIdle()` *first* (mirroring off,
    so the reset caused by the restart is not taken for a lost connection),
    then `restartServer()`; the iPhone's mirroring stops.
  - Miracast: `releaseSource`, `MiracastReceiver::disconnect()`, `onReset`.
  - Android: `releaseSource`, input handlers off, `AndroidSource::stop()`;
    `androidUserStopped` → no auto-reconnect until the next pairing (or
    restart). The old 中斷 Android 連線 command is an alias.
  Nothing live: toast 「目前沒有連線中的手機」.
* **Android hint**: the first picture of an Android connection shows
  「已連線：name。滑鼠移到上方有工具列；右鍵＝返回」 for 5 s (instead of the
  plain 已連線 toast; once per connection).
* The window follows the picture's orientation: the shape comes from
  `VideoWindow::desiredClientAspect` (after 畫面 rotation, with the frame),
  polled every 300 ms; a portrait/landscape flip swaps the client
  width/height around the window centre. After a 畫面 change by the user
  (rotate / reset / frame toggle) the window is fitted to the exact aspect,
  keeping its long side (shrunk to fit the work area). Not while fullscreen /
  maximized.

## 錄影 (recording)

開始錄影 (Ctrl+R; enabled while a picture is shown) →
`<install>\錄影\自在投影_YYYYMMDD_HHMMSS.mp4` (`_2`, … if taken; falls back to
`Videos\自在投影` if not writable; the folder is created on demand and by the
installer, which keeps it on uninstall). `pm::Recorder::start(path)` (60 fps),
then `VideoWindow::setFrameTap` → `Recorder::onVideoFrame` and
`AudioPlayer::setPcmMonitor` → `Recorder::onPcm`; `setRecording(true)` shows
the REC badge; toast 「開始錄影：file」. Stopping detaches both taps first,
then `Recorder::stop()` (finalizes the MP4), badge off, toast
「錄影已儲存：file」, log line with the size. Recorder log lines go to
`phonemirror.log` (level `recorder`).

* Auto-stop: picture ends (idle / phone stopped → 「投影結束，錄影已儲存：…」),
  unexpected disconnect (「連線中斷，錄影已儲存：…」), another phone takes over
  (「「B」接手投影（錄影已儲存）」), app exit, and before an update installs.
  Pausing (phone screen off) keeps recording.
* Failures: start refused below 300 MB free (「磁碟空間不足，無法錄影」) or if
  `start()` fails (「無法開始錄影」). Every second while recording
  (`kRecordTimer`): `Recorder::active()` false (encoder / write error) →
  stop + 「錄影發生錯誤，已停止並儲存：…」; free space < 300 MB → stop +
  「磁碟空間不足，已停止錄影：…」.

## 畫面 (rotation, flip, iPhone frame)

Submenu 畫面: 向右旋轉 90° (Ctrl+→), 向左旋轉 90° (Ctrl+←), 左右翻轉 ✓ (Ctrl+H),
還原 (Ctrl+0, rotation 0 + no flip; greyed when nothing to reset), iPhone 外框 ✓
(Ctrl+F). Calls `VideoWindow::setRotation / setMirrored / setDeviceFrame`;
stored as `rotation` (0/90/180/270), `mirror`, `device_frame`, applied at start.
截圖 uses `saveSnapshotFramed` (picture in the device frame, transparent PNG)
while the frame is on, else `saveSnapshot`; both honour rotation and flip.

## 主題 (themes)

Submenu 主題: 櫻花粉 / 薄荷綠 / 夜空藍 / 奶茶 (radio, each with a colour swatch:
accent disc, card ring, background centre from `VideoWindow::themeSwatch`).
`setTheme` re-themes the video window (idle screen, toasts, PIN card, REC
badge …) and `pm::ui::setPalette(bg, card, accent)` the popup menus (card
gradient from the card colour toward the background; light text on dark
cards, dark text on light ones; accent darkened on light cards). Stored as
`theme` (`sakura|mint|night|milktea`), applied at start.

## 多支 iPhone (takeover)

設定 / tray: 「新手機連線時：」 接手 (`takeover=new`, default) / 保持目前
(`takeover=keep`) → `Options::takeoverPolicy` (NewReplacesOld / KeepCurrent).
It is read at `start()`, so a change restarts the server like the PIN option
(deferred until the phone disconnects, with a toast). `Events::onTakeover`
(old, new) → toast 「「B」接手投影」, title / peer name updated, any recording
of the old phone is saved first. The core resets the sinks just before that
event, which `StatusVideoSink` sees as an unexpected loss: the takeover
handler ends that hold at once (no dimmed frame / 連線中斷 toast) and the next
「已連線」 toast is suppressed for 4 s.

## Source arbitration (iPhone / Miracast / Android) — 0.5.0

One window, one picture: `g_active` (atomic: none / AirPlay / Miracast /
Android) is the source that owns it. Title and toasts name the phone and
the source: 「自在投影 — 投影中：Galaxy S24（Android）」, 「已連線：Galaxy
S24（Miracast）」, 「…（AirPlay）」.

* **Claiming** (`claimSource`, any thread): a free window is always taken; a
  busy one only with 新手機連線時 = 接手 (`takeover=new`, `g_takeoverNew`).
  The claimer stops a Miracast owner (`disconnect()`, which resets the
  window) or an Android owner (`stop()`) before its own first picture, then
  posts `WM_PM_SOURCE` → UI: recording saved (「投影來源已切換，錄影已儲存：…」),
  `StatusVideoSink::switchSource()` (so the newcomer's first frame gives
  「已連線」), Android input handlers removed, and an AirPlay owner is dropped
  by restarting the AirPlay server (the core has no "disconnect client").
* **Gates**: AirPlay and Android video go through `GateVideoSink` (forwards
  only while its source owns the window; `onCodec` claims; the codec is
  replayed if a blocked stream gets the window) into the shared
  `StatusVideoSink` → window; their audio through `GateAudioSink` (plays
  while its source owns the window or nobody does; replays the format). So
  recording, snapshots, 畫面 rotation / frame, themes and the 連線中斷 hold
  work the same for every source. Miracast draws into the window itself
  (`submitBgraFrame`), so it is arbitrated in its `onStatus(Connecting)`
  callback, before its media starts.
* **When:** AirPlay at `onClientConnecting` / `onPin` (UI) and at the
  stream's `onCodec`; Miracast at `onStatus(Connecting)`; Android at
  `onConnected` (UI, before `start()`).
* **takeover=keep** while a picture is shown: an iPhone is refused by
  restarting the AirPlay server (toast 「「A」投影中，已拒絕「B」（新手機連線時：
  保持目前）」, at most every 5 s); a Miracast cast is `disconnect()`ed in its
  Connecting callback (toast 「…已拒絕「B」的投放…」); an Android phone that
  connects from the pairing panel is not started (toast 「…「B」已配對，稍後
  再連…」). iPhone vs iPhone stays the core's own `takeoverPolicy`.
* An Android **auto-reconnect** never takes over a shown picture (with
  either policy); it is retried while the window is free.
* The window is released at the source's end: `WM_PM_STATE` idle / lost
  (AirPlay, Android), `onDisconnected` (Miracast). During the 3 s
  連線中斷 hold the window is already free. An Android phone that goes away
  by itself skips the hold (0.5.2, see *Android*).

## Android 投放 (Miracast) — 0.5.0

`pm::MiracastReceiver` (docs/miracast.md) with the AirPlay name (自在投影).
設定 / tray 「接受 Android 投放（Miracast）」 (`miracast=1` default): at start
(and on toggle) `start()` / `stop()` run on a helper thread (`g.miracastOp`;
WinRT calls take up to a few hundred ms). With the option off only
`unsupportedReason()` is queried. `onStatus` → log, menu and idle hints:
Unavailable → the item is greyed and the reason shown under it as note rows
(split at ～22 characters / punctuation); Disabled (Wi-Fi off, another
receiver …) → reason shown, item stays enabled. While unavailable a
「如何啟用 Miracast」 item follows, opening the tutorial at `#miracast` (the app
does not install the Windows 「無線顯示器」 optional feature itself). Connecting → claim (see
above), 「正在連線」 screen, window raised; onConnected → 已連線 toast, title;
onDisconnected → idle; onPin → `showPin` (the receiver asks for a PIN only if
the phone insists: `PinDisplayIfRequested`; the app's 連線需要 PIN 碼 option
applies to AirPlay only — MiracastReceiver has no PIN-mode setting).
Miracast audio is played by Windows (MediaPlayer), not by pm_audio, so it is
not in recordings.

## Android (wireless debugging) — 0.5.0

`pm::AndroidSource` (android/include/pm/android_source.h) with
`<exe dir>\android-tools` (installed: `程式\android-tools`: adb.exe,
AdbWinApi.dll, AdbWinUsbApi.dll, NOTICE.txt, scrcpy-server, LICENSE-scrcpy.txt,
LICENSE-qrcodegen.txt; a dev build gets the same folder from
`pm_android_copy_tools` + app/CMakeLists.txt). `init()` failing (tools
missing) greys 「連接 Android（掃 QR）」 and 自動連線.

* **Pairing panel** (`app/pair_panel.cpp`, 連接 Android（掃 QR））: an owned
  popup window drawn with Direct2D in the menus' palette
  (`pm::ui::currentColors()`, follows 主題): accent bar, title 「連接 Android」 +
  ×, step text 「手機：開發人員選項 → 無線偵錯 → 使用 QR 圖碼配對裝置…」, the QR
  from `beginQrPairing` on a white rounded card (nearest-neighbour at a whole
  multiple of the bitmap, ≥ 4 modules of quiet zone, pixel-aligned), status
  line (accent; errors in a warm red), link 「怎麼開啟無線偵錯？」 (tutorial
  `#adb`), buttons 用配對碼 / 取消. 用配對碼 switches to a form: 「IP 位址與連接埠」
  and 「配對碼」 (6 digits) — real EDIT controls coloured via
  `WM_CTLCOLOREDIT`, Enter = 配對, Tab, Esc = close — buttons 改用 QR 圖碼 /
  配對 (→ `pairWithCode`; input checked first). Draggable by its top, per-monitor
  DPI. `onState` updates the status (等待手機掃描… / 正在配對… / 配對成功，正在
  連線… / errors); closing cancels a pairing in progress.
* **Connected** (`onConnected`): arbitration, then the panel closes,
  「正在連線：name（Android）」, window raised, `setPointerHandler` /
  `setKeyHandler` forward to `sendPointer` / `sendKey`, `start(gated video,
  gated audio → AudioPlayer)`. Input handlers are removed when the picture
  ends or another source takes over.
* **While shown**: the live toolbar's 返回 / 主畫面 / 最近使用 (`pressBack /
  pressHome / pressAppSwitch`) and 中斷連線 (also in the menu; stop, no
  auto-reconnect until the next pairing). A right click on the picture is the
  phone's 返回 (scrcpy BACK_OR_SCREEN_ON); the menu is the toolbar's 更多, a
  right click on the toolbar or outside the picture, or Shift+right click.
* **Auto-reconnect** (`android_auto=1`, 設定 「自動連線已配對的 Android」): at
  start and every 45 s (`kAndroidTimer`) while the window is free, no panel
  is open and the source is Idle / Error: `connectKnownDevices()`.
* **Phone gone** (0.5.2): straight back to idle with the toast 「Android
  已中斷（手機關閉了無線偵錯或離開 Wi‑Fi）」 (4 s; a recording is saved with
  「Android 已中斷，錄影已儲存：…」) — no dimmed 3 s hold, a reconnect takes
  longer than that. Two ways to notice:
  - 無線偵錯 turned off: adbd drops the connection, the video socket ends,
    the sinks reset (`StateLost` → handled as above) and `onDisconnected`
    follows (nothing left to do).
  - Wi-Fi left / out of range: the sockets may stay open for minutes. A 1 s
    watchdog (`kAndroidWatchTimer`, started with the first picture) counts
    frames passed through `StatusVideoSink` plus Android audio packets
    (`GateAudioSink::packets`): scrcpy repeats the last video frame every
    100 ms and its audio capture never pauses, so 6 s (`kAndroidStallMs`)
    without either → log 「no video or audio for N s」, `stop()`, idle + the
    same toast. Auto-reconnect stays allowed (`--test-stall S` simulates it).
* **Pairing panel feedback** (0.5.2): it closes itself on `onConnected` (before
  mirroring starts). `kPairTimer` starts when the QR is being listened for
  (state WaitingForPairing, QR mode): no answer within 45 s → status (error
  tint) 「沒有收到手機回應：確認手機和電腦在同一個 Wi‑Fi，或改用配對碼」; the
  panel keeps listening. Errors get next steps (`pairFailureText`): 配對失敗 in
  QR mode → 「在手機上重新點「使用 QR 圖碼配對裝置」再掃一次，或改用配對碼」 and a
  fresh QR is generated at once (the failed one is spent), in code mode →
  「確認 IP 位址:連接埠和 6 位數配對碼…」; the Android side's 10-minute timeout →
  the no-answer text; adb not starting → 「請重新開啟自在投影」. An error text is
  not overwritten by the next 等待手機掃描….

## 使用教學 (tutorial) and idle hints — 0.5.0

* The guide of the UI language — `docs/tutorial/自在投影教學.html` /
  `ZizaiCast-Guide.html` (0.6.0; the other one if it is missing) — is looked
  for at the top of the install folder (the installer puts the setup
  language's guide there), next to the exe (the installer puts both in
  `程式\`; dev build: both copied next to the exe), then in the source tree.
  Opened in the default browser by 使用教學 (both menus), the idle screen's
  「怎麼連線？」 link (an idle action) and, once, at the first start that is
  not `--background` (`tutorial_shown`). The pairing panel's link opens
  `#adb`: the browser from the `.html` association (`AssocQueryString`) gets a
  `file:///…#adb` URL; otherwise the file is opened plainly.
* `setIdleHints` (0.5.1: both Android ways always listed, `refreshIdleHints()`):
  「iPhone：控制中心 → 螢幕鏡像 → 自在投影」, 「Android：投放／Smart View →
  自在投影」 and 「Android：無線偵錯（可用電腦操控）」. A way that cannot be used
  right now gets a muted suffix (tab in the hint line): Miracast unavailable →
  from the `unsupportedReason()` text 「（重新開機後可用）」 (reboot after
  installing 無線顯示器), 「（需啟用無線顯示器）」, 「（需更新 Windows）」,
  「（已被系統原則停用）」, else 「（這台電腦不支援）」; turned off in the menu →
  「（已關閉）」 (an unusable PC wins); Disabled / not listening →
  「（目前無法使用）」; no android-tools → 「（缺少連線工具）」 on the 無線偵錯 line.
  Refreshed on every Miracast status change and the 投放 switch.
* `setIdleActions`: pill button 「顯示 Android QR 碼」 (primary; opens the pairing
  panel like 連接 Android（掃 QR）, only when android-tools are usable) and the
  「怎麼連線？」 link. The video window draws actions only on the idle screen, so
  neither shows while a phone is connecting / live / held after a drop.

## 自動更新 (updates)

Two sources; the **offer** is the higher version of the two (a tie goes to
the local installer: nothing to download). Never installed by itself: the
user clicks 更新到 vX.Y.Z.

### Online manifest

* Manifest URL: `update_url` in settings.ini if present (empty = never
  check), else the build's `PM_UPDATE_URL` (CMake cache variable, default
  `https://github.com/victor900106/ZizaiCast/releases/latest/download/update.json`
  — a GitHub release asset; `-DPM_UPDATE_URL=` disables). Build trees that
  cached an earlier default (the placeholder
  `https://zizai-update.example.com/zizai/update.json` or the repo's old name
  `…/zizai-touying/…`) are moved to the new default at configure time, and
  an `update_url` in settings.ini equal to one of those is ignored (treated
  as missing); any other explicit value is kept.
* Manifest: JSON `{"version": "0.5.2", "url": "https://…/zizai-setup-0.5.2.exe",
  "sha256": "<64 hex>", "notes": "…"}` (`app/updater.cpp`: WinHTTP,
  TLS 1.2+, no cache, 64 KB cap, small JSON string reader). Redirects are
  followed (github.com 302 → release-assets.githubusercontent.com, for the
  manifest and the installer; never HTTPS → HTTP).
* When: 30 s after start, then every 24 h (`kUpdateTimer`), and 檢查更新 in
  設定 / tray (toasts 正在檢查更新… / 已是最新版本（v0.5.3） / 檢查更新失敗).
  Automatic checks are silent unless a newer version exists. 檢查更新 first
  rescans 安裝檔: a newer local installer is announced at once.
* 更新到 vX.Y.Z (manifest): worker thread downloads to
  `%TEMP%\自在投影-更新-X.Y.Z.exe` (WinHTTP; Content-Length checked; ≤ 512 MB),
  SHA-256 (BCrypt) must match (else the file is deleted, toast
  「更新檔驗證失敗（SHA-256 不符），已取消更新」), then *Install* below.
* Verified 2026-10-07 against the live manifest (then serving 0.5.2): a
  0.5.3 build said 已是最新版本（v0.5.3）; a 0.5.1 override build offered 0.5.2,
  downloaded `zizai-setup-0.5.2.exe` through the redirect and logged
  `sha256 ok` (`--test-no-install`).

### 本機更新 (local installers) — 0.5.3

The integrator drops new installers into `<install>\安裝檔\` (install dir =
parent of the exe's `程式` folder, like 截圖; created by the installer and,
if missing, by the app; a dev build only watches `<exe dir>\安裝檔` if it
exists).

* Watched with `FindFirstChangeNotification` (file name / size / last write)
  on a thread → `WM_PM_LOCALDIR` → scan 0.5 s later; also a scan at start and
  every 10 min (`kLocalTimer`, which also restarts the watcher if the folder
  was deleted and re-created).
* Candidates: `自在投影-安裝程式-*.exe`. A file is used only when its size and
  write time have not changed for 2 s **and** it opens exclusively (share
  mode 0); otherwise the folder is scanned again a second later. Its version
  comes from VERSIONINFO (`pm::update::installerVersion`: ProductName must be
  「自在投影」 — 0.6.0 also accepts 「Zizai Cast」; the 0.6.0+ installers keep
  「自在投影」 in every language, so 0.5.x apps still recognise them (Inno pads
  values with spaces: trimmed); ProductVersion, else
  FileVersion, else the fixed version; `0.5.3.0` → `0.5.3`). The highest
  version newer than the running app is offered; several files: the highest
  wins; the file gone: the offer goes.
* 更新到 vX.Y.Z (local): the file's version is read again, it is copied to
  `%TEMP%\自在投影-更新-X.Y.Z.exe` (the 安裝檔 copy is never locked), then
  *Install*.

### Offer UX

* Toast 「有新版本 vX.Y.Z，點工具列或選單的「更新」」 (5 s) + tray balloon
  「自在投影有新版本 vX.Y.Z」 (manifest notes, or 「在選單選「更新到 vX.Y.Z」即可一鍵更新。」),
  once per version per run (檢查更新 says it again).
* Bold 「更新到 vX.Y.Z」 (↑ glyph U+E74A) at the top of the tray and context
  menus; on the idle screen a primary pill 「更新到 vX.Y.Z」 first among the
  idle actions; while a phone is live a toolbar button (↑, tooltip
  「更新到 vX.Y.Z」, before 更多). Mirroring / recording are never interrupted
  by the offer itself.

### Install

* Recording: a themed confirm (custom menu over the window: 「正在錄影」,
  「更新會先停止並儲存錄影，再關閉自在投影」, **停止錄影並更新到 vX.Y.Z** / 取消).
* `beginInstall`: recording saved, `updated_to=X.Y.Z` written to
  settings.ini, toast 正在更新到 vX.Y.Z…, the app quits (sources stopped by
  the normal shutdown), and after the message loop (mutex closed) starts the
  installer:
  `/SILENT /SUPPRESSMSGBOXES /NORESTART /NOCANCEL /UPDATE /DIR="<install>"
  /LANG=english|chinesetrad [/APPARGS="--dev --background --test-offscreen"]`
  (`/LANG`: the app's UI language, so shortcuts / readme follow it;
  `--background` if the window was hidden; `--test-offscreen` only for
  scripted --dev tests, which also get `/VERYSILENT` instead of `/SILENT`).
  The installer waits (≤ 30 s) for the app's mutex to go, installs, and its
  `[Run]` entry with `Check: IsUpdate` starts the app again with `APPARGS`.
  Not an installed layout (exe not in `程式\`, e.g. a dev build): the installer
  is started normally with its UI. Leftover `%TEMP%\自在投影-更新-*.exe` files
  are deleted at the next start.
* Next start with `updated_to` set: the key is removed; running version ≥ it
  → toast 「已更新到 vX.Y.Z」 (5 s; tray balloon too if started hidden), else
  「更新到 vX.Y.Z 沒有完成，請再試一次」.
* End-to-end test 0.5.3 → 0.6.0 (2026-10-08, entirely off-screen): a 0.5.3
  test install (the 0.5.3 `zizai.iss`, `/DTestAppId /DNoAppMutex`) in
  `%TEMP%`, its app (a `PM_APP_VERSION_OVERRIDE=0.5.3` build) running `--dev
  --test-offscreen` with `PM_VIDEO_OFFSCREEN=1`; the 0.6.0 test installer
  copied into 安裝檔 in 1 MB chunks → detected 2.3 s after the copy ended
  (`local installer …: version 0.6.0`, toast, idle pill, menus); DevCommand
  126 → app exited in 0.7 s, `/VERYSILENT /UPDATE /LANG=english`, 0.6.0
  running 4.1 s after the click with 「Updated to v0.6.0」; fdk-aac.dll gone,
  English shortcuts, `程式\licenses` present.
* End-to-end test (2026-10-07): 0.5.3 test install (`/DTestAppId`
  `/DNoAppMutex`) in `%TEMP%\pmlocal\手機投影` running `--dev
  --test-offscreen`; a 0.5.4 build (`PM_APP_VERSION_OVERRIDE` +
  `/DAppVersion=0.5.4`) copied into its 安裝檔 in 1 MB chunks over ~4 s →
  detected 2 s after the copy ended, toast / menus / idle pill shown;
  DevCommand 126 (更新到) → app exited in < 1 s, installer ran, 0.5.4 started
  again 4 s later and logged `updated to 0.5.4`.

### Menus (`app/popup_menu.cpp`)

Both menus are custom-drawn (0.3.1), not Win32 HMENUs:
`pm::ui::trackMenu(owner, items, pt, options)` takes a `MenuItem` tree
(command / submenu / separator / caption / note / header; icon code point,
right-aligned shortcut text, check / radio, disabled, bold) and runs a modal
loop like `TrackPopupMenu(TPM_RETURNCMD)`; `main.cpp` passes the id to
`runCommand`.

* Look: the idle screen's palette — warm dark card (#332326 → #2A1C1F),
  hairline pink border, 8 DIP rounded corners, soft drawn shadow; hover =
  translucent #F5A7A7 fill with a small pink bar; check marks, radio dots, the
  畫質 caption and a checked item's icon in pink; disabled items at 38 %
  alpha. Text Microsoft JhengHei UI 14 DIP, shortcuts Segoe UI 12, icons
  Segoe Fluent Icons (Windows 11) → Segoe MDL2 Assets (Windows 10) → none.
  The tray menu has a header (app icon + 「自在投影」 / “Zizai Cast” + version, 「測試版」 /
  “Dev build” with `--dev`) and a thin pink accent bar along the top. Text
  font: the UI language's (`pm::i18n::uiFont()`; Segoe UI in English). Without HEVC, 高 / 最高
  are greyed with the note 「高、最高需安裝 HEVC 影片延伸模組」.
* Theme: `pm::ui::setPalette(bg, card, accent)` replaces the colours above
  for every menu opened afterwards (see 主題). `MenuItem::hasSwatch` +
  `swatch[3]` draws a colour swatch in the icon column instead of a glyph.
* Windows: one `WS_POPUP` per level with `WS_EX_LAYERED | WS_EX_TOOLWINDOW |
  WS_EX_TOPMOST | WS_EX_NOACTIVATE`, unowned. Software Direct2D DC render
  target → premultiplied DIB → `UpdateLayeredWindow` (per-pixel alpha for the
  shadow and anti-aliased corners; DWM does not round layered windows, so
  `DWMWA_WINDOW_CORNER_PREFERENCE` is set to `DWMWCP_DONOTROUND`). Factories,
  text formats and the window class are created at startup
  (`pm::ui::warmUp()`); first open measured ~10–14 ms (logged once:
  `menu opened in … ms`).
* Placement: top-left at the point, flipped left / up when it would leave the
  monitor's work area, then clamped (so a tray menu sits above the taskbar).
  Submenus open beside the parent row (right, else left), first item level
  with the parent row. Each level uses the DPI of its monitor
  (`GetDpiForMonitor`, PerMonitorV2); layout is in DIPs, card size rounded to
  whole pixels. Checked at 100 % and 175 %.
* Input: the root window takes the mouse capture; a click outside any menu
  closes it (the click is eaten, like native menus). Items fire on button
  up (left or right). Hovering a submenu row opens it after 250 ms (another
  row closes it after 250 ms); clicking opens it at once. Keyboard: Up / Down
  (wrap, skip separators and disabled items), Home / End, Right / Enter
  opens a submenu with its first item focused, Left / Esc closes one level,
  Esc at the root / Alt / F10 / Win closes all, Enter / Space runs the item.
  While open, keys are not passed to the window (F11 / Ctrl+T / Ctrl+S act
  only when the menu is closed). It also closes when another app becomes the
  foreground window (if ours was foreground at open), on loss of capture,
  `WM_CANCELMODE`, or a click elsewhere seen by a 50 ms poll (backstop when
  capture / foreground could not be had). A menu keyboard-opened (Shift+F10)
  starts with the first item focused.
* Tray: `SetForegroundWindow(main window)` before tracking (works with the
  window hidden), `WM_NULL` posted after — the usual tray-menu dance.
* Not re-entrant: a request while a menu is open is ignored.

## Session events (`AirPlayServer::Events`, posted to the UI thread)

| event | UI |
|---|---|
| `onClientConnecting(name, model)` | `setConnecting(name)`, window shown + raised, title "正在連線：name" |
| first video frame | title "投影中：name", toast "已連線：name" |
| `onPin(pin)` | `showPin(pin)`, window raised; `""` hides. The PIN is also hidden on `onClientConnecting` and `onClientDisconnected` |
| `onClientDisconnected` | PIN hidden, idle title; applies a pending PIN/quality restart |

### Unexpected disconnect

The core does not report why a session ended, so the app decides: the phone's
own 停止鏡像輸出 is logged by the core as `video_reset: RTP shutdown` right
before `VideoSink::onReset` (`StatusVideoSink::noteCoreLog` watches for it) and
goes straight to the idle screen. Any other end while a picture is shown
(httpd relaunch after a feedback timeout / conn_reset, or
`onClientDisconnected` while video was still flowing) keeps the last frame
for 3 s (`StateLost`, timer `kLostTimer`) with the toast
「連線中斷，在 iPhone 重新選擇螢幕鏡像即可」, then calls `onReset()` on the
window. New video (or `onClientConnecting`) ends the hold at once. If paused
(phone screen off) it goes to idle directly. The held frame is dimmed during
the hold (`VideoWindow::setDimmed(true)` on `StateLost`, `setDimmed(false)`
when the hold ends; video also clears it on `onReset()` / the next picture).

### Sleep / network

`WM_POWERBROADCAST`: on `PBT_APMRESUMEAUTOMATIC` / `PBT_APMRESUMESUSPEND`
(deduplicated within 5 s) the app calls `AirPlayServer::refreshNetwork()`
and logs `advertisedInterfaces()`. If
a picture was showing, it checks 4 s later whether any new frame arrived
(`VideoWindow::stats().framesIn`); if not, the frozen picture is dropped for
the idle screen (toast as above). Network changes while awake are handled by
the core.

## Settings

* `%LOCALAPPDATA%\PhoneMirror\settings.ini` (UTF-8 `key=value`):
  `require_pin`, `topmost`, `tray_hint_shown`, `quick_ack` (default 1 →
  `Options::mirrorQuickAck`), `audio_latency_ms` (default -1 →
  `Options::reportedAudioLatencyMs`), `quality` (`standard|high|max`,
  default `high`), `av_sync` (ignored, always written as 0 — see 影音同步), `device_id`,
  `rotation` (0/90/180/270), `mirror`, `device_frame`, `theme`
  (`sakura|mint|night|milktea`), `takeover` (`new|keep`), `miracast` (default 1),
  `android_auto` (default 1), `tutorial_shown`, `language`
  (`auto|zh-TW|en`, 0.6.0), `display_name` (optional, only written once set),
  `update_url` (only
  written once set; see 自動更新), `updated_to` (only between starting an
  update and the next start). Unknown/missing keys take defaults; the
  file is rewritten when a setting changes. Phone volume stays in `volume.txt`.
* pm_audio's diagnostic lines go to `phonemirror.log` (`AudioPlayerConfig::log`, level `audio`).
* Autostart = `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, value
  `自在投影` = `"<exe>" --background`. The registry is the source of truth
  (the installer's optional task writes the same value). If the value points
  to an exe that no longer exists it is repointed at the running exe.
* The idle-screen check boxes (`VideoWindow::setIdleOptions`), the tray menu
  and the 設定 submenu show the same two options and stay in sync.
* 連線需要 PIN 碼 maps to `Options::requirePin`, which is read at `start()`:
  toggling it restarts the AirPlay server (new `AirPlayServer` instance, retried
  for ~4 s while ports are released). During an active session the restart is
  deferred until the phone disconnects (toast says so).

## AirPlay identity

`device_id` (`aa:bb:cc:dd:ee:ff`) is passed as `Options::macAddress` on every
start, so a VPN / virtual adapter appearing first cannot change the deviceid
(paired iPhones and the PIN client list depend on it). It is empty on first
run: the core picks the MAC of the first Ethernet/Wi-Fi adapter (random if
none) and the app stores `deviceId()` after the first successful start, so an
existing install keeps its current id. `advertisedInterfaces()` is logged at
every start and after resume.

## 畫質 (quality)

`quality` sets the display mode offered to the phone via
`AirPlayServer::applyQualityPreset` (all 60 fps): standard 1920×1080 H.264,
high 2560×1440 (default), max 3840×2160 — both H.265, which iOS needs above
1080p (`--h265` forces H.265 for standard too).

At startup the app checks for an HEVC decoder MFT (`MFTEnumEx`,
`MFT_CATEGORY_VIDEO_DECODER` / `MFVideoFormat_HEVC`, hardware or software,
e.g. the Store's HEVC Video Extensions). Without one, 標準 is used whatever
`quality` says (the stored value is kept), 高 / 最高 are greyed out with
「（需要 HEVC 影片延伸模組）」, and a `warn` line is logged. The iPhone scales its screen into that box, so a higher setting
means a sharper (and heavier) stream. Changing it restarts the AirPlay server
like the PIN option (deferred until the phone disconnects, with a toast).

## 影音同步 (A/V sync) — disabled since 0.3.1

The 「影音同步（看影片用）」 menu item is gone and `av_sync` in settings.ini is
ignored: the owner found that it made the video lag *behind* the audio. Cause:
with sync on, video is scheduled at its pts + audio latency
(`VideoWindow::setSyncMode(true, bufferMs + deviceBufferMs)`), but the audio
player plays packets as soon as they arrive (ASAP), not at their pts — so
delaying the picture by the audio buffer depth over-corrects. It needs audio
scheduled against the same clock (play at pts + fixed latency) before it can
come back. The code path is kept (`Settings::avSync`, `applySyncMode`,
`setAvSync`, `kSyncTimer`, `CmdAvSync`) but `avSync` is forced to false, so at
start the app calls `setSyncMode(false, 0)`: the picture is shown as soon as
it is decoded.

## Screenshots

`VideoWindow::saveSnapshot` (or `saveSnapshotFramed` while iPhone 外框 is on)
→ `<install>\截圖\自在投影_YYYYMMDD_HHMMSS.png` (falls back to
`Pictures\自在投影` if the folder is not writable), toast "截圖已儲存".

## Icon

`app/res/app.ico` is the original 投投 Toutou icon (cloud built from two
circles and a pill, two flat tones, a cast-signal mark, on a pink gradient
tile that reads on dark and light taskbars), generated with every size
(16 20 24 32 40 48 64 256) hand-tuned on its own pixel grid by
`node docs/mascot/toutou/export.mjs` (`docs/mascot/toutou/icon.mjs`; headless
Chrome only). The same .ico is used for the exe, window, tray and installer.

## Installer (`installer/`)

Inno Setup 6 script `installer/zizai.iss`, two languages (0.6.0): English
(`compiler:Default.isl`, first = fallback) and 繁體中文 (the bundled
unofficial `ChineseTraditional.isl`). The language follows the Windows display
language (`LanguageDetectionMethod=uilanguage`: exact, then primary language,
so zh-CN / zh-HK get 繁體中文; anything else English; no language dialog);
`/LANG=english|chinesetrad` forces one (the app passes its own UI language
on updates).

```
"%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" installer\zizai.iss
:: -> installer\Output\自在投影-安裝程式-0.6.0.exe   (file name pattern used by 本機更新)
```

* Per-user, no admin (`PrivilegesRequired=lowest`); default folder
  `%USERPROFILE%\Desktop\手機投影` (繁體中文) / `…\Desktop\Zizai Cast`
  (English) (`{cm:DefaultFolder}`; an upgrade keeps the previous folder).
  `AppName` / uninstall entry / shortcuts: 自在投影 / Zizai Cast
  (`{cm:AppName}`); the VERSIONINFO ProductName stays 「自在投影」 in both (本機更新).
* Installs 自在投影.exe, avcodec-63.dll + avutil-61.dll (FFmpeg, LGPL build;
  0.6.0 — the old fdk-aac.dll is deleted on upgrade), libcrypto-3-x64.dll,
  plist-2.0.dll, pthreadVC3.dll and the app-local VC++ runtime (msvcp140,
  vcruntime140, vcruntime140_1) — the exe's non-system imports, checked with
  `dumpbin /dependents` (nothing imports fdk-aac), LICENSE.txt,
  `程式\licenses\` (第三方授權.txt, THIRD_PARTY_NOTICES.txt, SOURCE.md), both
  guides in `程式\` and `程式\android-tools\*` (adb.exe,
  AdbWinApi.dll, AdbWinUsbApi.dll, NOTICE.txt, scrcpy-server, LICENSE-scrcpy.txt,
  LICENSE-qrcodegen.txt from `<BuildDir>\android-tools`); at the top level the
  setup language's guide + readme (自在投影教學.html + 使用說明.txt, or
  ZizaiCast-Guide.html + "Read Me.txt" from `ZizaiCast-ReadMe.txt`) and the
  shortcuts 自在投影 / 解除安裝自在投影 / 授權資訊 (→ `程式\licenses`), or Zizai
  Cast / Uninstall Zizai Cast / Licenses; the other language's top-level
  files and shortcuts are deleted (`[InstallDelete] … Languages:`); creates
  `截圖`, `錄影` and `安裝檔` (same names in both languages; 本機更新; each
  removed on uninstall only if empty). Start menu also gets
  「自在投影 使用說明 / 使用教學」 or “Zizai Cast Read Me / User Guide”.
* The app's private adb server (`程式\android-tools\adb.exe`, matched by path)
  is stopped before installing (`PrepareToInstall`) and uninstalling, so its
  files are never locked; other adb servers are left alone.
* Start-menu shortcuts, optional task 開機自動啟動, uninstaller (removes the
  Run value if it points into the install folder; keeps screenshots and
  recordings: `截圖` / `錄影` are removed only if empty).
* Running copy: `[Code]` `InitializeSetup` / `InitializeUninstall` check the
  mutex `Local\PhoneMirror.SingleInstance` (`CheckForMutexes`, the standard
  "is running" OK / Cancel box). Not `AppMutex=`, so that an update can wait.
* `/UPDATE` (started by 自動更新 with `/SILENT /SUPPRESSMSGBOXES`): waits up to
  30 s for the mutex to go (+1.5 s), installs, and the `[Run]` entry with
  `Check: IsUpdate` starts the app again with `{param:APPARGS|}` (e.g.
  `--dev --background`); the normal postinstall launch has `Check: not IsUpdate`.
* Test builds: `/DAppVersion=x.y.z` (fake newer installer for the updater),
  `/DNoAppMutex` (install next to a running copy; `/UPDATE` then waits for the
  `--dev` copy's mutex `Local\PhoneMirror.Dev`) and
  `/DTestAppId={{GUID}` (own uninstall entry and no Start-menu shortcuts, so a
  test install/uninstall does not touch the real one).
* Silent test: `setup.exe /VERYSILENT /SUPPRESSMSGBOXES /TASKS="" /DIR="%TEMP%\x"`,
  uninstall with `"%TEMP%\x\程式\unins000.exe" /VERYSILENT`.
* 0.6.0 tests (2026-10-08, TestAppId builds, `%TEMP%`, both `/LANG`s): fresh
  install → every file / folder / shortcut / uninstall entry per language, no
  fdk-aac.dll; upgrade over a 0.5.3 (zh) test install → fdk-aac.dll removed,
  shortcuts / readme / guide switched to the setup language, a user file in
  截圖 kept on uninstall; uninstall → 程式 and the entry gone. Without `/LANG`
  on this zh-TW Windows: 繁體中文.
