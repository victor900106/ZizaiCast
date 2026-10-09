# Video front end (`pm_video`)

Decodes the mirrored iPhone screen (Annex-B H.264 / HEVC from `pm::VideoSink`)
and shows it in a Win32 window. Windows-native only: Media Foundation decoder
MFTs + Direct3D 11 + Direct2D/DirectWrite/WIC (status UI). No FFmpeg/GStreamer.

**Language (0.6.0).** Every text the window draws itself (idle title / default
hint, 「正在連線」, the paused text, the PIN card label, the mascot's speech
bubbles, the 「name」 quotes) comes from the shared string table
`include/pm/i18n_strings.inc` (`pm::i18n::tr`, header-only `include/pm/i18n.h`)
in the current language (`pm::i18n::setLang`, process-wide atomic: the render
thread reads it every frame, so a switch shows at the next frame). The UI font
follows the language (`pm::i18n::uiFont()`: Microsoft JhengHei UI for 繁體中文,
Segoe UI for English, CJK device names fall back automatically); the text
format cache is keyed by language. Texts handed in by the app (hints, options,
actions, toasts, tooltips) are already translated. English bubbles: “Hi
there!”, “Ready to cast?”, “Hey, that tickles!”, “You've got this today!”,
“♡”; asleep: “zzz…”, “Five more minutes…”, “Hmm…?”. A toast too long for one
line breaks after the first 「。」, else after the first ". ".

## API (`video/include/pm/video_window.h`)

```cpp
pm::VideoWindow win;                       // implements pm::VideoSink
if (!win.create(L"PhoneMirror")) return 1; // window + decode/render thread
core.setVideoSink(&win);                   // VideoSink calls: any thread, non-blocking
win.runMessageLoop();                      // until the window is closed
```

| Member | Notes |
|---|---|
| `create(title, w=1280, h=720)` | Creates/shows the window, starts the worker thread. False if D3D11 unavailable. |
| `runMessageLoop()` | Pumps Win32 messages on the creating thread; returns on close (posts `WM_QUIT`). |
| `close()` | Thread-safe close request. |
| `hwnd()` | Native handle for embedding/positioning. |
| `stats()` | Thread-safe: HW/SW decode, frame counters, decode and onFrame→present latency (avg/p95), A/V sync error, device recoveries, adapter name. |
| `setSyncMode(enabled, audioLatencyMs)` | Any thread, cheap; call again whenever the latency changes (the app feeds the audio buffer + device latency every second). Off (default): present ASAP, as before. On: each picture is presented at `ntpLocalNs + audioLatencyMs` (see *A/V sync*). Latency clamped to 0..5000 ms. |
| `onCodec/onFrame/onSourceSize/onReset/onPaused` | `pm::VideoSink`. `onFrame` copies the AU into a bounded queue and returns. `ntpLocalNs` is only used in sync mode. |
| `setConnecting(name)` | Any thread. Shows a spinner, 「正在連線」 with animated dots and 「name」 on its own line (one line, ellipsis-trimmed) until the first picture fades in (250 ms) or `onReset()`. Ignored while a picture is live; gives up after 60 s without frames (unless a PIN is up). |
| `setIdleOptions(opts, onToggle)` | Any thread. Check boxes under the idle hint; click toggles, redraws and calls `onToggle(id, checked)` **on the UI thread**. Hover highlight + hand cursor. |
| `showPin(pin)` | Any thread. Centred PIN card over whatever is showing; `L""` hides it (both fade 160 ms). |
| `showToast(text)` / `showToast(text, holdMs)` | Any thread. Bottom-centre pill, fully visible ~2.5 s (or `holdMs`) then fades; a new toast replaces the old one. Too long for one line: wraps (after the first 「。」 if there is one) into a rounded card. |
| `setLiveToolbar(items, onClick)` | Any thread. `ToolbarItem { id, glyph, tooltip, toggled, danger, groupStart }`: the live toolbar (see *Live toolbar*); `onClick(id)` **on the UI thread**. `{}` removes it. |
| `setLiveToolbarSlider(onSlide, onWheel)` | Handlers for `ToolbarItem::slider` items (drag: `onSlide(id, value, done)`, wheel: `onWheel(id, notches)`), **on the UI thread**. |
| `saveSnapshot(path)` | Call from the UI thread (any thread works). The worker renders the current picture through the same NV12→RGB shader into an offscreen texture of the visible size (no letterbox; current rotation / mirror applied), the caller encodes a 24-bit PNG with WIC. ~50 ms for 1170x2532 (mostly PNG encoding). While waiting it only services cross-thread *sent* messages (no input re-entrancy). False if no picture was ever shown. |
| `saveSnapshotFramed(path)` | Same, inside the device frame (bezel, rounded screen corners, Dynamic Island, side buttons) on a transparent background: 32-bit PNG with straight alpha, e.g. 2696x1334 for a 2532x1170 picture. Works whether or not `setDeviceFrame` is on. |
| `saveWindowShot(path)` | Test / docs hook: draws the window now (status screen with the mascot, overlays or picture, exactly as shown) and saves the client area as a 24-bit PNG from the back buffer (copied right before `Present`), so it works off-screen or covered. False if minimized / device lost. |
| `setDimmed(bool)` | Any thread. ~55 % black over the live picture, 200 ms ease in/out; nothing else changes. Cleared by `setDimmed(false)`, by the next decoded picture and when the picture leaves the screen (`onReset`, paused). For the app's 3 s 「連線中斷」 hold. |
| `setRotation(q)` / `setMirrored(h)` | Any thread. Rotates the shown picture clockwise by `q` quarter turns (wraps mod 4) / mirrors it left-right (after the rotation). Done in the vertex shader (texture coordinates), so no extra pass; the letterbox / frame follow. Snapshots honour both; the frame tap does not (it delivers the decoded picture). |
| `desiredClientAspect(w, h)` | Any thread, immediate. Size of the displayed picture after rotation (newest decoded picture's visible size, swapped for 90/270°), 0x0 before the first picture. Reflects `setRotation` at once. |
| `setDeviceFrame(bool)` | Any thread. iPhone-style bezel around the picture, fitted to the window with a small margin: titanium edge (faint theme tint) with highlight, black bezel, screen corners rounded (radius 13.5 % of the screen's short side), Dynamic Island at the top in portrait / on the left in landscape, side buttons. The letterbox takes the theme background colour. Off by default. |
| `setTheme(t)` / `themeSwatch(t)` | `Theme::Sakura` (default, the original pink), `Mint`, `Night`, `MilkTea`. Every UI element uses the palette (background gradient, glow, particles, check boxes, toast, PIN card and veil, spinner, speech bubble, hearts, REC badge, frame tint) and so do the mascot's phone screen, rim light and beam; its cloud stays white. `themeSwatch` → `{bg, card, accent}` 0xRRGGBB: Sakura `2A1C1F 332226 F5A7A7`, Mint `123230 173D39 8FE3C4`, Night `1B1D3D 24264D BBA9F7`, MilkTea `30231B 3B2B21 E3B98A`. |
| `setRecording(bool)` | Any thread. Pill at the top-left of the picture (inside the rounded corner when framed; window top-left without a picture): pulsing red dot + elapsed `mm:ss` (`h:mm:ss` after an hour) counted from activation. Redraws at 30 fps while active (a static stream still ticks). |
| `setFrameTap(tap)` | See *Frame tap*. |
| `setZoom(z)` / `zoomAt(z, vx, vy)` / `zoomStep(n)` / `panBy(dx, dy)` / `resetMagnifier()` | Any thread, cheap. Magnifier 1..8x inside the picture viewport (see *Magnifier, high contrast, freeze, text overlay*). `zoomAt` keeps viewport point (vx, vy) fixed; `zoomStep` = x1.25 per step; `panBy` in viewport fractions. |
| `setFilter(f)` | Any thread. `Filter::None / Contrast / Grayscale / Invert / YellowOnBlack` in the pixel shader (picture + overview), not in snapshots / tap. |
| `setFrozen(bool)` | Any thread. Keeps showing a copy of the current picture (badge 「畫面已凍結」); decoding, frame tap and recording go on with the live stream. Cleared by `onReset()`. |
| `viewState()` / `setViewHandler(fn)` | `ViewState { zoom, centerX, centerY, filter, frozen }`; `fn` on the UI thread after the user changed the view with the built-in input (Ctrl+wheel, drag, keys). |
| `setTextOverlay(boxes)` / `setTextOverlayOriginal(bool)` / `setOverlayBusy(label)` | Any thread. Translated-text cards over the picture (content coordinates), outlines only while 「顯示原文」, centre busy card with spinner. Used by pm_translate (docs/translate.md). |
| `beginRegionSelect(done)` / `cancelRegionSelect()` | Any thread; `done(ok, x0, y0, x1, y1)` on the UI thread in content coordinates. |
| `grabPicture(bgra, w, h)` | Any thread (also UI). The shown (frozen) picture as BGRA: cropped, rotated, **not** mirrored, unzoomed, unfiltered — the OCR input. ~6–10 ms for 1170x2532. |
| `post(fn)` | Runs `fn` on the window's UI thread (posted; immediately when already on it). |
| `submitBgraFrame(bgra, w, h, stride, ptsNs)` | Any thread. Already-decoded BGRA picture (Miracast); see *Android sources*. |
| `setPointerHandler(fn)` / `setKeyHandler(fn)` | Any thread; handlers run on the UI thread. Remote control (scrcpy); see *Android sources*. `nullptr` removes. |
| `setIdleActions(actions)` | Any thread. `IdleAction { label, primary, onClick }`: clickable row under the hints / check boxes, centred, wrapping to more rows when narrow. `primary` = pill button (accent outline on a faint accent wash; hover: filled accent, ink text), else underlined accent link (hover: soft pill + foreground text). Hand cursor; click → `onClick` **on the UI thread**. Drawn only on the fully shown idle screen (never while connecting / live / paused). `{}` hides them. |
| `setIdleHelpLink(label, onClick)` | Thin wrapper: `setIdleActions({{label, false, onClick}})`; `L""` hides it. |
| `setIdleHints(lines)` | Any thread. Replaces the idle subtitle with 1–3 lines (each wraps) and the title with 「等待手機連線…」; `{}` restores the iPhone-only default. A tab starts a muted suffix (0.86 × hint size, dim at 62 %), e.g. `L"Android：投放 → 自在投影\t（重新開機後可用）"`; if the line does not fit on one line the suffix moves to its own line as a whole. |

Window controls: double-click or F11 toggles borderless fullscreen, Esc leaves
fullscreen. The picture is letterboxed with its aspect ratio kept. The
`create()` size is in DIPs (scaled by the monitor DPI); `WM_DPICHANGED` is
handled (suggested rect applied, UI re-laid out).

### Status UI

Drawn with Direct2D/DirectWrite on the render thread around the mascot
**投投 Toutou**, an original soft cloud holding a little phone (art, research
and generator in `docs/mascot/toutou/`). Its layers are PNGs generated by
`node docs/mascot/toutou/export.mjs` into `video/res/toutou/` (cloud, rim-light
mask, 7 faces: idle / blink / happy / connecting / surprised / sleepy /
asleep, 3 hands for the phone poses; 840x600 at 4 px per art unit, ~530 KB),
embedded at build time by `res/embed.cmake` (`generated/toutou_png.h`),
decoded with WIC (`ToutouArt`, cropped to their opaque area, resampled with
high-quality cubic to the drawn size in 1/8 px-per-unit steps) and composed
with one transform per frame (art pivot = cloud bottom centre → anchor, scale,
squash / stretch, float). The phone (frame, accent screen gradient, ▶, soft
accent halo) and the **beam** (a ribbon of light leaving the phone top along
its axis and arcing to the upper right: four nested ribbons — glow, band,
white core — whose width grows and breathes with a slow twist, plus eight
square pixel sparkles riding along) are drawn procedurally so they take the
theme accent and animate; the rim light on the cloud is the rim mask filled
with the accent (`FillOpacityMask`). The cloud stays white in every theme.
Geometry shared with the art: `video/res/toutou/toutou_layout.h` (generated).

Layout is in DIPs. Portrait: 投投 floats in the lower part, centred (cloud ≤
64 % of the width / 30 % of the height, ≤ 3.6 DIP per art unit ≈ 570 DIP of
cloud), text centred in the space above. Landscape: 投投 in a column left of
the text column; on very wide windows the pair is centred (text column ≤
max(45 % of the width, 640 DIP)). Titles that are too long shrink (≤ 25 %) to
one line, then wrap.

| State | Look |
|---|---|
| Idle | 投投 floating (3 s sine, a few art units, slight stretch rising / squash sinking), ground shadow that shrinks and fades as it rises, blinks every 4–7 s (sometimes twice), phone screen and rim light breathing in the accent, a calm beam pulse every 6 s (the ribbon grows out of the phone, its tail follows), soft accent glow behind the phone, a few slow floating hearts / sparkles; "等待 iPhone 連線…", hint, check boxes. |
| Connecting | 'connecting' face (eager, looking up the beam), beam on and fast (grows out in 450 ms, a bright pulse travels along it, pixels stream), quicker float; spinner, 「正在連線」 + three pulsing dots, device name in pink below. |
| Live | Picture (fade-in 250 ms over the previous status screen; `onReset()` fades back to idle in 250 ms). |
| Paused (`onPaused(true)`) | 'asleep' face, phone lying down with its screen off, 84 % opacity, slower and smaller float, "z z Z" in the accent drifting; "iPhone 螢幕已關閉 / 解鎖 iPhone 後會自動繼續". |
| PIN / toast | Overlays on top of any state, same palette. |

**Mascot interaction** (idle and paused screens; not while connecting, live,
fading or under a PIN). The worker publishes the layer canvas at rest with
each frame; the UI thread hit-tests it against a 96-cell-wide opacity mask
(cloud + hand + phone), so only opaque parts count. Hover: hand cursor, 投投
grows 4 % (160 ms ease, reversible mid-way). Click (or double-click: no
fullscreen toggle on it): 'happy' face (^^, open smile, a waving nub),
squash-and-stretch hop (anticipation squash → stretched jump → landing squash
→ damped wobble, 1 s, around the cloud's bottom centre), 7 hearts in the
accent colour bursting from the top of the cloud (1.15 s), and a speech bubble
above it that pops in with a small overshoot and fades after ~1.5 s, cycling
「嗨～」「要投影嗎？」「戳我幹嘛」「今天也要加油喔」「♡」 (paused: 'sleepy'
face with a yawn, no hearts, 「zzz…」「再睡五分鐘…」「嗯…？」).
`setConnecting` from the idle screen: the same hop + hearts (no bubble) while
the idle text stays, then the idle text fades out (450–700 ms) and the
spinner scene fades in (600–900 ms) while the beam grows out of the phone. All of it counts as a 60 fps animation
only while it runs; afterwards the idle rules below apply unchanged.

**Rendering policy.** Rendering is event-driven; only while something animates
does the worker wake on a high-resolution waitable timer: 60 fps for fades,
toasts, PIN fade and the connecting spinner, 30 fps for the idle ambience. The
idle ambience runs for 40 s after the last activity (entering idle, mouse
movement over the window — at most one wake per second —, resize, show) and
then eases into a static frame (1.5 s). Nothing animates while minimized or
hidden (`SW_HIDE`), and a window reported occluded by `Present` is probed at
2 fps only.

## Magnifier, high contrast, freeze, text overlay (0.7)

For low-vision users and for reading foreign-language phone screens.
Everything happens on the GPU / in the existing passes: the picture is drawn
by the same vertex / pixel shader with two more constants.

**Coordinates.** *Viewport* v = 0..1 over the picture rectangle on screen
(`pictureRect`); *display* t = 0..1 over the whole displayed picture
(rotated + mirrored as seen): t = centre + (v − 0.5) / zoom; *content* d =
the rotated, **unmirrored** picture (what `grabPicture` returns and what
overlay / selection rectangles use): d.x = mirrored ? 1 − t.x : t.x. The
pointer mapping for remote control goes viewport → display → phone
(`screenToPicture`), so taps land where they are shown at any zoom.

**Magnifier.** `zoom` 1..8 (clamped; snaps to 1 within 6 %) and a centre
clamped so the view never leaves the picture. The vertex shader maps the
quad's corners to `t = v / zoom + (centre − 0.5 / zoom)` before the
rotation / mirror transform — no extra pass, no extra texture. The device
frame, letterbox, REC badge and toolbar stay put (it magnifies inside the
"screen"). UI while zoomed:
* a **big indicator** 「放大 2.5×」 / 「原始大小」 (bold, 26–72 DIP — a 7th of
  the picture's short side, white on 80 % black with a yellow border) for
  1.2 s after every change, fading out in 0.4 s;
* a persistent **zoom badge** bottom left (magnifier glyph + 「2.5×」, 16–30
  DIP, yellow / white on black);
* an **overview** bottom right: the whole picture (second draw of the same
  texture, same filter, 70–220 DIP high, at most 28 % of the width) with the
  magnified part outlined in yellow over black.

Built-in input (no app code): **Ctrl+wheel** zooms ×1.25 per notch at the
cursor; while zoomed, **left drag** pans (with a pointer handler — remote
control — **Ctrl+left drag**, a plain drag still goes to the phone; size-all
cursor), the **wheel** pans vertically / **Shift+wheel** or tilt sideways
(only without a pointer handler: else the phone scrolls), **arrow keys** pan
10 % (only without a key handler), **Alt+arrows** always pan; **Ctrl+= /
Ctrl+-** zoom ×1.25 around the centre and **Ctrl+Shift+0** goes back to 1×
(the app may handle these first). A double click on the zoomed picture
starts a pan instead of toggling full screen. Changes from this input call
`setViewHandler`'s function on the UI thread.

**Filters** (`setFilter`, pixel shader after YUV→RGB, also for BGRA
pictures): `Contrast` — saturation ×1.35 then contrast ×1.7 around mid grey;
`Grayscale` — BT.709 luma, contrast ×1.25; `Invert` — 1 − rgb (a dark-mode UI
becomes black on white and vice versa); `YellowOnBlack` — dark pixels (luma <
0.8, smooth ramp to 0.25) become yellow (1, 0.92, 0.1), light ones black: dark
text on light backgrounds turns into yellow text on black. Snapshots and the
frame tap / recording stay unfiltered.

**Freeze** (`setFrozen(true)`): the renderer copies the current picture into
its own textures (`CopyResource`: NV12 / P010 / R8+R8G8 / BGRA alike) and
shows that copy (`shown()`); decoding, the A/V sync queue, the frame tap and
therefore the recorder keep the live stream. Badge 「畫面已凍結」 (pause glyph)
top right (below the Dynamic Island with the portrait device frame). A frozen
picture stays up while the phone screen is off (paused). Snapshots,
`saveSnapshotFramed`, `grabPicture` and `desiredClientAspect` use the frozen
picture. After a device loss the next decoded picture is frozen again.
`onReset()` (source ended) unfreezes and removes the overlay; zoom and filter
are the user's viewing preference and stay.

**Text overlay** (`setTextOverlay`): one card per box, drawn after the
device frame / dim: the box (content → screen through mirror + zoom) plus a
margin of a quarter line, theme `card` at 97 % with an accent border, text in
the theme foreground, semi-bold, UI font (JhengHei UI for 中文), left-aligned,
uniform line spacing 1.2. Auto-fit: start at the original's glyph height
(`box height / lines × 0.92`), shrink to 70 %, then allow a card up to 1.6×
wider (centred: short labels such as tab names), then shrink to 9 DIP and let
the card grow downwards. Layouts are cached per box size (re-fitted only when
zoom / window size change). `setTextOverlayOriginal(true)`: only accent
outlines of the boxes (the picture's own text shows). Mirroring moves the
boxes, the text stays readable. Clipped to the picture.

**Region selection** (`beginRegionSelect`): crosshair cursor, hint pill
「拖曳框出要翻譯的範圍（Esc 取消）」 at the top, 25 % veil; while dragging a
45 % veil outside the rectangle and a yellow-on-black border. Release →
`done(true, content rect)`; a click / < 8×8 px, Esc, right / middle click,
focus or capture loss, `cancelRegionSelect()` or `onReset()` → `done(false)`.
Nothing is forwarded to the phone meanwhile. Works through rotation, mirror
and zoom.

**Busy card** (`setOverlayBusy`): centred card with the spinner, e.g.
「正在辨識文字…」 / 「正在下載翻譯模型… 42%」 / 「正在翻譯…」; also drawn without a
picture.

Menus, toolbar buttons and shortcuts for the app (放大鏡, 高對比, 凍結畫面,
翻譯畫面): docs/translate.md *Integration*.

## Android sources

### BGRA pictures (`submitBgraFrame`)

* The caller's rows are copied into a one-picture mailbox (newest wins; an
  unshown picture that gets replaced counts as `framesDropped`) and the worker
  is woken. The worker swaps the buffer out (no per-frame allocation) and
  uploads it with `UpdateSubresource` into a reused `B8G8R8A8` texture (new
  texture only when the size changes). From there it is an ordinary current
  picture: letterbox, rotation / mirror (same vertex shader), device frame,
  dim, fade-in from the idle / connecting screen, snapshots (a `ps_bgra`
  pixel shader instead of NV12→RGB: snapshot pixels are the submitted ones),
  `desiredClientAspect`, stats (`framesIn` per submit, `framesDecoded` per
  upload, `hardwareDecode` false, `decodeAvg/P95` = submit → uploaded,
  `e2e` = submit → `Present`).
* Frame tap: two pixel shaders convert to **BT.709 video-range NV12** (what the
  recorder declares; chroma = 2x2 average), then the usual staging ring. `pts` =
  `ptsNs` if within 30 s of now on QPC or the UTC wall clock, else the submit time.
* Always ASAP (no A/V sync queue). `onReset()` drops an unshown picture and
  fades to idle; a picture submitted after `onReset()` shows. A device loss
  re-uploads the last picture (kept on the CPU) on the new device.
* Mixing with the decoder: whichever picture came last is shown.

### Remote control (`setPointerHandler` / `setKeyHandler`)

* The worker publishes the live picture's viewport (client px, letterbox or
  device-frame screen) with the rotation / mirror it was drawn with after each
  frame; the UI thread maps a client pixel centre to the viewport (0..1) and
  through `Renderer::screenToPicture` (the vertex shader's mapping) to the
  phone's picture space. Nothing is mapped while idle, connecting, paused or
  fading.
* Left / right / middle Down on the picture → `Down` (button 0 / 1 / 2) and
  `SetCapture`; `Move` for every `WM_MOUSEMOVE` while a button is down and
  `Up` on release, both clamped to the picture edge when the drag leaves it.
  `WM_CAPTURECHANGED` to another window, or the picture disappearing mid-drag,
  sends `Up` at the last position. Double-clicks on the picture are presses
  (fullscreen toggle only outside the picture). Shift + right click is not
  forwarded (context menu); a forwarded right click produces no `WM_CONTEXTMENU`.
  Nothing on the live toolbar is forwarded (see *Live toolbar*).
* Hover (no button): `Move` with `button = -1` over the picture only, at most
  every 16.7 ms; a throttled last position is delivered by a one-shot timer
  (`0x504D0001`; the app's timers use 1..7).
* `WM_MOUSEWHEEL` / `WM_MOUSEHWHEEL` over the picture → `Wheel` with
  `wheelY` / `wheelX` = delta / 120 (+ = up / right); outside → `DefWindowProc`.
* Keys: `WM_KEYDOWN` / `WM_KEYUP` forwarded when neither Ctrl nor Alt is down,
  except Ctrl+C / V / X / A / Z (copy, paste, cut, select all, undo go to the
  phone; the app's Ctrl+S / T / R / H / F / 0 / ← / → shortcuts are handled by
  its subclass first and never reach the phone). Ctrl / Alt / Win keys, F11 and
  Esc-in-fullscreen are not forwarded; `WM_SYSKEYDOWN` (Alt) never. `ch` = the
  `WM_CHAR` `TranslateMessage` queued for that key (peeked and removed),
  printable only. Text without a forwarded key (IME results — the 注音 /
  倉頡 composition ends as `WM_CHAR` —, dead-key and AltGr characters) →
  `(0, true, ch)`. Every forwarded down gets an up, also on `WM_KILLFOCUS`.
  Modifiers: `GetKeyState` in the handler (it runs synchronously on the UI thread).
* Cursor: a 32x32 white ring + dot with a dark outline (hot spot in the
  centre) over the picture while a pointer handler is set.

### Live toolbar (`setLiveToolbar`)

A themed pill of icon buttons at the top centre of the live picture, for the
things that otherwise hide in the context menu (which a right click on an
Android picture cannot open: that click is 返回).

* **Visibility.** Every `WM_MOUSEMOVE` over the window (the UI thread sends
  at most one request per 200 ms) shows it: 140 ms fade + 6 px slide in, held
  2 s after the last movement, 260 ms fade out. It stays while the cursor is
  on the pill and starts hiding at once on `WM_MOUSELEAVE`. Only in the live
  scene (not idle / connecting / fading); paused or without a picture it sits
  at the top of the window. Hidden it draws nothing and has no hit area; hit
  rectangles are published only while it is more than ~35 % opaque.
  Event-driven: the worker wakes for the hide deadline, the fades, the tooltip
  delay and (only while a button is `toggled`) the pulsing dot.
* **Layout (DIPs).** 36 px buttons, 2 px apart, 5 px padding, an 11 px slot
  with a hairline divider before `groupStart` items; shrinks uniformly (down to
  26 px buttons) to fit the picture width − 16. 10 px below the picture top;
  with the device frame in portrait below the Dynamic Island (screen short
  side × 0.122 + 8); moved below the REC badge when they would overlap.
  Card colour 95 %, 1 px accent hairline at 38 %, soft shadow; icons are
  Segoe Fluent Icons (Windows 11) or Segoe MDL2 Assets at 0.42 × button size.
* **Slider items** (`ToolbarItem::slider` ≥ 0, the app's volume): 72/36 of a
  button wide; track inset 0.22 × height at each end
  (`Renderer::kToolSliderInset`, the same mapping for input), filled in the
  accent up to the knob (grey while `toggled` = muted). A press on it captures
  the mouse and drags: the knob follows at once (UI-side copy of the items)
  and `setLiveToolbarSlider`'s `onSlide(id, value 0..1, done)` runs on the UI
  thread; the toolbar stays up during the drag, a lost capture ends it. The
  wheel over the slider or the button just before it calls `onWheel(id,
  notches)`. Narrow windows: a slider is left out only after every optional
  button, and an optional button right before it (its speaker) only after it.
* **States.** Hover: accent disc (26 %). `danger` (中斷連線): icon `#FF6B6B`,
  hover = solid red disc with a white icon. `toggled` (recording): red disc
  24 % (36 % hovered), red icon and a pulsing red dot at the top-right.
  Tooltip: 450 ms after hovering a button (immediately when moving between
  buttons while one is up), 120 ms fade, a small card centred under the
  button (kept inside the window), text in the danger colour for `danger`.
* **Input.** On the pill nothing reaches the pointer handler (the phone):
  left press → `onClick(id)` on release over the same button (padding:
  swallowed; a fast double click = two clicks, never fullscreen); middle and
  wheel swallowed; right click goes to `DefWindowProc` → `WM_CONTEXTMENU` (the
  menu, even on an Android picture); hover moves are not forwarded. Hand
  cursor over buttons, arrow over the padding. A drag that started on the
  phone keeps going to the phone across the pill.
* Test hooks (`"PhoneMirror.Video.Test"`, `SendMessage`): `wParam` 11,
  `lParam` = button index → its centre (client px, `MAKELRESULT`), -1 if the
  toolbar is not up; `wParam` 12, `lParam` 1 → no `TrackMouseEvent` (scripts
  that post `WM_MOUSEMOVE` without moving the real cursor).

### Idle hints and actions

The hint lines are laid out in the same text block (hint size, dim colour,
`0.45 × hint` between lines; muted suffixes through a second D2D brush set as
the DirectWrite drawing effect), then the check boxes, then the action rows
(hint size × 0.95, semi-bold, row height 2.2 × hint, `0.7 × hint` between
actions; pill padding 0.62 × row height, link 0.42 ×). Rows are filled
greedily within the text region; each row is centred. Every action's hit
rectangle is published like the check boxes' (one per action, in order); they
are clickable only on the fully shown idle screen. Fits 400x800, 540x960 and
1280x720 (screenshots below).

## Design

```
network thread ─onFrame()─► bounded AU queue (32) ─► worker thread:
                                                      MF decoder MFT (sync)
                                                        └ DXVA via IMFDXGIDeviceManager, or SW
                                                      NV12 → copy to own texture
                                                      pixel shader NV12→RGB (BT.601/709, video/full range)
                                                      flip-model swap chain, Present(1), max latency 1
                                                      D2D status UI / overlays on the same back buffer
UI thread: Win32 window, WM_SIZE/paint/fullscreen/DPI/mouse → flags for the worker
           (auto-reset wake event; the worker publishes check-box hit rects back)
```

* **Decoder** (`src/mf_decoder.*`): enumerates sync decoder MFTs (`MFTEnumEx`),
  output NV12, or P010 when the MFT offers no NV12 (HEVC Main10: the HEVC
  Video Extensions offer only P010; before, such a stream failed to open —
  P010 is rendered through R16/R16G16 views by the same shader),
  falls back to `CLSID_CMSH264DecoderMFT`. HEVC uses the *HEVC Video Extensions*
  MFT if installed; if it is missing, a clear log line says so and HEVC frames
  are dropped (the idle screen stays). `MF_LOW_LATENCY` + `CODECAPI_AVLowLatencyMode`
  → one output per input, no reorder delay. Hardware path: `MF_SA_D3D11_AWARE`
  + `MFT_MESSAGE_SET_D3D_MANAGER`; if that is refused or decoding fails, it
  reopens in software and re-feeds the last IDR. `MF_E_TRANSFORM_STREAM_CHANGE`
  (rotation / new SPS) renegotiates NV12 output and reads
  `MF_MT_MINIMUM_DISPLAY_APERTURE` for cropping (e.g. 1184x2544 coded → 1170x2532 shown).
  `PM_VIDEO_FORCE_SW=1` forces software decoding for diagnostics.
* **Renderer** (`src/renderer.*`): HW frames are copied (GPU-side
  `CopySubresourceRegion`) from the decoder's texture array into one NV12
  texture with R8/R8G8 views; SW frames are uploaded to R8 + R8G8 textures.
  A full-screen-quad shader converts to RGB inside a letterboxed viewport.
  Swap chain: `FLIP_DISCARD`, 2 buffers, frame-latency waitable object, max
  frame latency 1.
* **Latency policy**: the worker drains *all* queued AUs, decodes them, waits
  for the swap chain's waitable object, drains again, and presents only the
  newest picture. A slow display therefore drops presented frames, never
  accumulates latency. If the queue holds 32 AUs it skips to the newest IDR
  if one is queued (clean). Without one, AUs are dropped only beyond **600 AUs
  / 128 MB** (~10 s; 0.6.1: 32): then the oldest AU that carries no IDR /
  parameter sets goes (logged with the backlog, and again when caught up). A
  lost reference AU breaks every picture until the next IDR, and iOS sends one
  only every minute or so (the HEVC Video Extensions then output *nothing*),
  so a render thread that was blocked for a while catches up instead
  (~1 ms per AU) — see *Watchdog*.
* **Reset / codec**: `onReset()` clears the queue, flushes the decoder and
  shows the idle screen; decoding resumes at the next IDR. `onCodec()` with a
  new codec re-creates the decoder immediately (MFT creation costs ~100 ms and
  is kept off the first frame).

### A/V sync (`setSyncMode`)

* `ntpLocalNs` is on `pm::AirPlayServer::localTimeNs()`'s clock. Today that is
  `raop_ntp_get_local_time()` = `CLOCK_REALTIME` (Unix-epoch wall-clock ns);
  the window accepts that **or** QPC ns (the core's `CLOCK_MONOTONIC` /
  `steady_clock`): `onFrame` compares the stamp with both "now"s, uses the
  closer one (logged once: `ntpLocalNs is on the UTC wall clock`) and converts
  it to the worker's QPC milliseconds immediately, so a later wall-clock
  adjustment cannot move queued pictures. A stamp more than 30 s from now on
  both clocks, or `ntpLocalNs == 0`, means ASAP.
* Decoded pictures are copied into pooled textures (`Renderer::hold`, pool of
  ≤ 8 free textures) and wait in a queue. Before every present decision the
  worker shows the newest picture whose target `due + audioLatencyMs` has come
  (2 ms slack for the waitable timer); older due pictures are skipped (late →
  shown immediately, never queued behind). The worker sleeps on the
  high-resolution timer until the next target. The latency is read at present
  time, so `setSyncMode(true, newLatency)` takes effect on the next frame
  (lowering it skips the frames that became late; raising it holds the
  picture).
* Caps: a picture waits at most **500 ms** after decoding (an absurd stamp or
  latency cannot freeze the picture), at most 40 pictures (the oldest is shown
  early beyond that). A stamp-less picture queues one frame behind the held
  ones (never jumps ahead). Turning sync off shows the newest held picture
  immediately and frees the pool; `onReset/onCodec` drop the queue.
* Memory: one NV12 texture per held picture (4.5 MB at 1170x2532), i.e.
  ~25 pictures / 110 MB of video memory at 400 ms; ASAP mode allocates none.

### Device loss and GPU choice

* **GPU**: `IDXGIFactory6::EnumAdapterByGpuPreference(HIGH_PERFORMANCE)`
  (first non-software adapter) — on hybrid laptops the discrete GPU renders
  and DWM composes it onto the panel of the integrated GPU, as for games.
  Exception: on a multi-GPU desktop, if the window's monitor is driven by
  another GPU *and* the preferred GPU has outputs of its own, the monitor's GPU
  is used (no cross-adapter copy per frame). Re-evaluated on
  `WM_DISPLAYCHANGE` and when the window moves to another monitor; a different
  choice re-creates everything on the new GPU (same path as a device loss).
  Falls back to the default hardware adapter, then WARP.
* **Detection**: `Present`/`ResizeBuffers` returning `DXGI_ERROR_DEVICE_REMOVED/RESET`,
  a failing decode or GPU copy followed by `GetDeviceRemovedReason() != S_OK`
  (checked after every decode batch — a removed device no longer makes the
  decoder fall back to software).
* **Recovery** (window, UI state, options, PIN/toast and fullscreen stay):
  release the decoder, the renderer's device objects (swap chain, shaders,
  textures, D2D target — the D2D/DWrite factories, geometries and the mascot
  stay) and the device; create a new device (+ `IMFDXGIDeviceManager`), a new
  swap chain on the same HWND, reopen the decoder and **re-feed the access
  units since the last key frame** (kept in a GOP cache, ≤ 1800 AUs / 64 MB
  = 30 s at 60 fps (0.6.1: 300 / 16 MB), else only the key frame — and an HEVC
  stream then stays frozen until its next IDR, logged), copying only the last
  picture to the screen. A
  live stream shows the last frame (DWM keeps it) and then continues; failed
  attempts retry with back-off (100 ms … 2 s, e.g. during a driver update)
  while AUs keep filling the GOP cache.
* **Deferred GPU switch (0.6.2)**: a display / monitor change that makes
  another GPU preferable while a stream is live and the GOP cache is
  truncated waits for the next IDR (switching would lose the references);
  with a complete cache it switches at once and re-feeds. Every check is
  logged (`adapter check (…): keeping …` / `… switching at the next key frame`).
* Test hook: the registered window message `"PhoneMirror.Video.Test"`
  (`wParam` 0 = simulated device removal, 1 = power-saving GPU, 2 = WARP,
  3 = automatic) runs exactly that path; `pm_video_test --test-at` posts it.
  `wParam` 20–26 inject faults (see *Watchdog*).

### Watchdog and diagnostics (0.6.2)

Background: a friend's iPad (AirPlay, HEVC 2560x1440) froze for good after
Win+Shift+S while the sound went on. Measured with an iOS-like stream (HEVC
1440p60, IDR every 60 s, `testdata/ios_like.h265`): the HEVC Video Extensions
(DXVA) **drop every picture whose references are missing**
(`CODECAPI_AVDecVideoDropPicWithMissingRef` is not settable on it; it is
still set to FALSE for MFTs that accept it). So in 0.6.1 *any* event that
restarted the decoder or lost an AU mid-GOP — a render thread blocked
≥ 0.5 s (queue overflow → dropped AUs), a device loss / TDR, a GPU switch on
`WM_DISPLAYCHANGE`, the HW→SW fallback with a GOP longer than 5 s — froze the
picture until iOS's next IDR, i.e. for up to a minute or for the rest of the
session. The likely trigger here: the Snipping overlay (screen capture + DWM
transition) blocked Present / the GPU long enough to overflow the 32-AU
queue. No log existed to tell (the `[video]` lines went only to stderr).

* **Logging**: `VideoWindow::setLogHandler(fn)` gets every `[video]` /
  `[video-watchdog]` line; the app writes them to `phonemirror.log` (levels
  `video` / `video-watchdog`; the log is rotated to `phonemirror.old.log` at
  8 MB on start). While AUs arrive, a monitor thread logs one line per 5 s:

  `5s: AU in 300 drop 0 skip 0 | fed 300 > MFT 300 > pictures 300 (hw) | presented 300 (ok 300, occluded 0, failed 0, last 0x00000000; frame-wait timeouts 0) | queue 0 | last IDR 12.0 s ago, GOP 720`

  (`skip` = AUs ignored while waiting for an IDR, `MFT` = pictures the decoder
  produced, `pictures` = reached the renderer). Event lines: decoder open
  (and its missing-reference policy), output format / stream change, decode
  errors, AU dropping and catching up, GOP cache overflow on a re-feed, device
  loss (+ reason) and re-creation, adapter checks, `Present` failures and
  `DXGI_STATUS_OCCLUDED` ↔ `S_OK` transitions, and — from the monitor thread
  — `render thread busy in <stage> for N s` when the worker is stuck outside
  its idle wait for > 3 s (it cannot report that itself). The monitor sleeps
  while no stream is live (no idle wake-ups).
* **(a) Decoder stall**: AUs are fed (≥ 8) but no picture came out for
  1.5 s → `[video-watchdog] no picture decoded …` (decoder name, MFT in / out /
  errors, last IDR age, GOP cache), reopen the decoder and re-feed the GOP
  cache (only the newest picture is copied); the 2nd time within a minute in
  software (DXVA is tried again at an IDR ≥ 30 s later: SW HEVC cannot do
  1440p60). No key frame kept: wait for the next IDR. If the references are
  known to be lost (AUs dropped, cache truncated) a second restart cannot
  help: logged once (`keeping the last picture until the next IDR`).
  "Too many decode errors" re-feeds the cache instead of waiting for an IDR.
* **(b) Present stall**: pictures decoded but none presented (Present not
  called / failing, no render target) for 1.5 s → re-create the swap chain
  (`Renderer::recreateSwapChain`, same device, the current picture stays) and
  present the newest picture; a second time within 10 s → the whole device.
  A swap chain whose frame-latency waitable times out for 1.5 s while frames
  are presented gets a new swap chain at most every 30 s. Not while minimized,
  hidden, idle or paused.
* **(c) Occlusion**: on `DXGI_STATUS_OCCLUDED` decoding continues at full
  rate, new pictures are presented every 100 ms without waiting for the
  waitable (it may not be signalled while occluded); the first `S_OK` restores
  the full rate. `Present`'s `copyIn` GPU wait is bounded (1 s, logged) so a
  hung GPU cannot block the thread forever.
* `PM_VIDEO_WATCHDOG=0` restores the 0.6.1 behaviour (no watchdog, 32-AU
  drop, 5 s GOP cache, immediate GPU switch, no occlusion pacing); the
  diagnostics stay. Used for the before/after runs below.

## Build

```bat
set CMAKE="C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
:: as part of the whole project
%CMAKE% -S . -B build -G "Visual Studio 18 2026" -A x64 -DCMAKE_TOOLCHAIN_FILE=%USERPROFILE%/vcpkg/scripts/buildsystems/vcpkg.cmake
:: or just the video module (no vcpkg deps)
%CMAKE% -S video/standalone -B build-video -G "Visual Studio 18 2026" -A x64
%CMAKE% --build build-video --config Release
```

Link `pm_video` (static). System libs (d3d11, dxgi, d3dcompiler, d2d1, dwrite,
windowscodecs, mfplat, mf, mfuuid, …) are linked privately by the target. The mascot
layer headers are generated into the build tree (`generated/toutou_*_png.h`,
included by `generated/toutou_png.h`).

### Frame tap (`setFrameTap`)

```cpp
win.setFrameTap([&](const uint8_t* nv12, int w, int h, int stride, uint64_t ptsNs) {
    // render thread; Y: h rows, then UV: h/2 rows, `stride` bytes each; valid only during the call
});
win.setFrameTap(nullptr);  // returns after any running call has finished
```

* Every newly decoded picture (also in sync mode, at decode time; not the
  pictures re-fed after a device loss) is cropped to its visible size, rounded
  down to even width/height, and converted on the GPU by two tiny pixel shaders
  (`Load` of exact texels → R8 and R8G8 render targets; for P010 the UNORM
  targets turn 10 into 8 bits). The two targets are copied into a ring of 3
  staging-texture pairs.
* Read-back is one picture behind: after `Present` the worker maps the previous
  pictures (their GPU work has finished long before — `copyIn` already waited
  for the next decode), copies the planes into one contiguous NV12 buffer
  (stride = width) and calls the tap. The newest picture is delivered at the
  latest 20 ms later if no further picture arrives, and at `onReset` /
  `onCodec`. With several pictures in one decode batch, a slot is kept free by
  delivering early. Mirroring / rotation / dim / frame are not applied.
* `ptsNs` = the AU's `ntpLocalNs` if it is within 30 s of now on the UTC wall
  clock or QPC, else the `onFrame` arrival time on `AirPlayServer::localTimeNs()`'s
  clock (UTC wall-clock ns; QPC if the stamps were detected on QPC).
* Thread safety: `setFrameTap` from any thread (including from inside the tap);
  the worker holds a mutex while calling the tap, so after `setFrameTap` returns
  the old tap is never called again. The tap runs on the render thread: copy
  and hand off; a tap slower than the frame interval stalls the display
  (`--tap-slow 30` on 60 fps: 7 of 100 pictures presented). Never block on the
  UI thread inside it.
  Unset: one relaxed atomic load per AU / picture, nothing else.
* `stats()`: `framesTapped`, `tapAvgMs` / `tapP95Ms` = render-thread time per
  picture (shader + copy submission, map, NV12 assembly; the tap itself excluded).

## Test tool

Magnifier / filters / freeze / overlay (0.7), off-screen, self-checking:

```
pm_video_test video\testdata\ios_like.h264 --magnifier DIR --png build-translate\screens\ja_settings.png
pm_video_test video\testdata\ios_like.h264 --freeze-stream DIR
```

`--magnifier` (BGRA picture + a moving bar, 540x960): grabPicture ==
submitted pixels; zoomAt keeps the cursor point; snapshot unzoomed; pointer
events mapped through the zoom; drag / Ctrl+wheel / arrow input; 8× / 1×
clamps; the 4 filters (+ yellow at 2×); freeze (window still while frames
arrive, frame tap continues, grab stable); overlay (+ original, zoomed,
mirrored); busy card; region select (drag, Esc, through rotation 90 + mirror
+ zoom 2); device frame + zoom 3; unfreeze; onReset ends the freeze. 18
window shots `mag_*.png`. `--freeze-stream` does freeze / grab / zoom 3 +
contrast / unfreeze on the decoded H.264 stream (DXVA NV12 path).

```bat
powershell -ExecutionPolicy Bypass -File video\testdata\make.ps1   :: needs ffmpeg on PATH
build-video\bin\Release\pm_video_test.exe video\testdata\portrait_rotate.h264
build-video\bin\Release\pm_video_test.exe video\testdata\portrait.h265 --fps 60 --hold 1500 --idle 1500
build-video\bin\Release\pm_video_test.exe video\testdata\portrait_rotate.h264 --demo-ui --idle 3000
```

More options (full list in the header of `tools/pm_video_test.cpp`):

```bat
:: A/V sync with synthesised timestamps (UTC like today's core, or --clock qpc),
:: 40 ms network jitter, latency changed every 2 s
pm_video_test.exe portrait_rotate.h264 --sync 150 --jitter 40 --lat-sweep 100,250
:: device loss at 2 s, Intel iGPU at 4 s, WARP at 6.5 s, automatic at 9 s
pm_video_test.exe portrait_rotate.h264 --loops 2 --test-at 2000:0,4000:1,6500:2,9000:3
:: soak: 61 min at 60 fps, resources every 60 s
pm_video_test.exe portrait_rotate.h264 --minutes 61 --report 60 --size 480,640
:: UI churn: 300 x (setConnecting, showPin, 12 frames, showToast, showPin(""), onReset),
:: codec alternating with --alt, optional device loss every K cycles
pm_video_test.exe portrait_rotate.h264 --churn 300 --alt portrait.h265 --churn-loss 10
:: decoder MFT open/close leak isolation (handle types per object type)
pm_video_test.exe portrait_rotate.h264 --decoder-cycles 50
:: watchdog fault injection on the off-screen window, 0.6.1 behaviour vs 0.6.2:
:: decoder output swallowed at 10 s, freeze intervals + pictures after the fault
pm_video_test.exe ios_like.h265 --offscreen --freeze-report --test-at 10000:20:0 [--watchdog off]
```

Fault codes for `--test-at T:C:A` (lParam A): 20 decoder swallows its output
(A more HW instances do too), 21 `Present` reports `DXGI_STATUS_OCCLUDED`
for A ms, 22 `Present` calls swallowed (stuck swap chain), 23 render thread
blocked A ms, 24 next A non-IDR AUs dropped, 26 display change after which
the power-saving GPU is preferred. `--display-change-at T` posts a real
`WM_DISPLAYCHANGE`; `--cover-at T:MS` puts an opaque topmost layered window
over the (off-screen) test window; `--freeze-report` prints every interval in
which AUs arrived but no picture was presented. `ios_like.h265` /
`ios_like.h264`: 75 s testsrc2 at 60 fps, IDR at 0 and 60 s only (`make.ps1`;
gitignored).

`make.ps1` produces `portrait_rotate.h264` (1170x2532 → 2532x1170 → 1170x2532,
2 s each, 60 fps, in-band SPS/PPS per segment) and `portrait.h265` (1170x2532,
3 s). The tool splits the file into access units, feeds them via the
`VideoSink` interface at `--fps` (0 = as fast as possible), then calls
`onReset()` and prints stats. `testdata/capture.ps1` runs it and grabs
screenshots of the window (optional `-ResizeTo w,h`, `-DoubleClickAt s`,
`-ExtraArgs '--demo-ui'`, `-Mouse 't:x:y[:click]'` posts mouse moves/clicks, `-At x,y`).

`--demo-ui2` (540x960): 0–4 s the four themes, 4.4 s hover + click on the
mascot (5.0 s mid-reaction), 6 s second click, 7.4 s `setConnecting` (hop), 9 s
frames ×2 with the device frame (portrait, then landscape), 13 s frame off +
`setRotation(1)`, 14.6 s rotation 0 + `setRecording(true)`, 17.4 s
`saveSnapshot` rotated + mirrored and `saveSnapshotFramed`, 21.3 s
`setDimmed(true)` on the held last frame, 23 s reset, 24 s paused + click
(sleepy line). `--tap` / `--tap-dump out.png` / `--tap-slow MS`: see *Frame tap
measurements*.

`--mascot-tour DIR` (any stream file argument, ignored): the window is created
far off the desktop without a taskbar button and never activated
(`PM_VIDEO_OFFSCREEN=1`), input is posted, and every shot is taken with
`saveWindowShot` (a copy of the back buffer right after drawing), so nothing
appears on screen: idle in the four themes, hover, click mid-hop / hearts, a
12-frame strip over 6 s (beam pulse, blink), connect hop, connecting, paused,
paused click, 1280x720, 400x800, 3440x1440 px, then process CPU for 10 s while
the idle screen animates and 10 s after it settled. Test hook `wParam` 13
returns a point on the mascot's cloud (client px, -1 if not clickable).

`--android` (540x960, any stream file argument, ignored): self-checking test of
the Android hooks, PASS/FAIL lines, exit code = failures (details in the tool's
header). `--bgra-bench W,H [--fps 60] [--seconds 5] [--tap]`: BGRA throughput.
Test hook `wParam` 10 (`SendMessage`, `lParam` = action index) returns that idle action's centre (client px, `MAKELRESULT`), -1 if not shown.

```bat
powershell -File video\testdata\capture.ps1 -Exe build-video\bin\Release\pm_video_test.exe -File video\testdata\portrait_rotate.h264 ^
  -ExtraArgs --android -Times 1.3,1.9,5.4,6.6 -Names a_idle_hints,a_link_hover,a_bgra_frame,a_idle_after -Scale 0.75
```

(The hover shot needs the real cursor over the window: otherwise
`TrackMouseEvent` posts `WM_MOUSELEAVE` right after the synthetic move; use
`-At` to place the window under the cursor.)

`--demo-ui` (540x960 DIP window) runs: idle + two check boxes → 3 s
`setConnecting` → 5 s frames → 6 s `saveSnapshot` on the UI thread
(`demo_snapshot.png`) + toast → 8 s PIN → 9.5 s PIN off → 10 s paused →
11.2 s resumed → 12 s `onReset` → idle. Toggles print `toggle: id=… (ui thread: yes)`.

## Measured (2026-10-07, RTX 3060 Ti, 1170x2532@60)

| Stream | Decoder | decode avg / p95 | onFrame→Present avg / p95 |
|---|---|---|---|
| H.264 rotate | HW (MS H264 MFT, DXVA) | 2.9 / 3.5 ms | 3.1 / 3.8 ms |
| H.264 rotate | SW (forced) | 8.0 / 10.3 ms | 8.5 / 10.7 ms |
| HEVC | HW (HEVCVideoExtension, DXVA) | 1.7–2.0 / 2.5–3.5 ms | 1.8–2.2 / 2.7–4.1 ms |
| HEVC | SW (forced) | 24.9 / 27.7 ms | 25.5 / 28.7 ms |

"decode" = AU submitted → picture copied and GPU-complete (event query).
"onFrame→Present" ends when `Present()` returns; actual glass latency adds
≤ 1 vsync + DWM composition. At `--fps 240` all 360 AUs are decoded, 188 are
presented, latency stays ~4.5 ms avg.

Screenshots: `video/testdata/{h264_portrait,h264_landscape,h264_portrait2,hevc_portrait,resized_landscape,fullscreen_portrait}.png`;
status UI: `ui_{live_toast,pin}.png`, `n_{live_toast,pin}`, `fs_pin`; the 投投 screens are in
`video/testdata/toutou/` (`--mascot-tour`, 175 % DPI, saved at 50 % (3440x1440 at 40 %): `idle_{sakura,mint,night,milktea}`, `idle_blink`, `settled`, `hover`,
`click_react_{airborne,hearts}`, `idle_beam_pulse`, `connect_intro`, `connecting`, `paused`,
`paused_click`, `landscape_{idle,connecting}`, `narrow_{idle,paused_night}`, `ultrawide_idle`; `icon_tray_sizes` = the app.ico 16/20/24/32 entries on dark and light taskbars, 1:1 and ×3).

Status UI CPU (whole process, `--demo-ui`, 540x960, 24-thread CPU):

| State | CPU (one core) |
|---|---|
| idle, ambience animating (30 fps) | 2.3–3.0 % (投投, `--mascot-tour` 540x960 at 175 %: 3.1–3.6 %) |
| connecting (spinner, 60 fps) | 5.2 % |
| idle after the ambience settled | 0.0 ms CPU in 10 s (no wake-ups; `--mascot-tour` shows 0.5 %: its own 10 ms polling thread) |

### A/V sync, recovery, soak (2026-10-07, RTX 3060 Ti + UHD 770)

| Test | Result |
|---|---|
| `--sync 150` (UTC stamps) | 354/360 presented; Present − target avg −1.4 ms, \|p95\| 1.8 ms; onFrame→present 147.6 ms |
| `--sync 150 --jitter 40` | 355/360 presented on the 60 Hz grid, \|p95\| 4.5 ms (ASAP with the same jitter: 279/360, bursty) |
| `--sync 100 --lat-sweep 100,300` | \|p95\| 1.8 ms; lowering the latency skips the frames that became late |
| `--sync 700` | held ≤ 500 ms (cap): onFrame→present 503 ms |
| SW HEVC `--sync 100` | 179/179 presented, \|p95\| 1.8 ms |
| sync off (default) | onFrame→present 3.4 / 5.9 ms (unchanged) |
| Device loss (test hook) | picture back 250–550 ms later: device+swap chain 130–155 ms (NVIDIA/Intel), MFT open 3–45 ms, re-feed of ≤ 103 AUs 90–290 ms; GPU switches NVIDIA → Intel UHD 770 (DXVA) → WARP (SW decode) → NVIDIA all recover. Screenshots `testdata/rec_*.png` (DWM keeps the last frame while recovering). |
| `WM_DISPLAYCHANGE` with the same best GPU | no re-creation |
| Churn 300 cycles (H.264; UI + reset) | handles 661 → 664, private 125 → 126 MB, GDI 12, USER 23–24: flat |
| Churn 300 + device loss every 10 (30 recoveries) | running-state handles 669 → 671, private ~200–210 MB: flat |
| Churn 300 alternating H.264/HEVC | +1 semaphore per HEVC MFT instance (HEVC Video Extensions; see gaps), everything else flat |
| Soak A: 61 min, 60 fps ASAP, H.264 loop | private 141.6 MB (1 min) → 142.4 (11) → 142.1 (21) → 143.7 (31) → 143.9 (41) → 147.0 (51) → 144.0 MB (61): no trend; handles 657 → 665 (flat from 31 min), GDI 12, USER 23–26; 219187/219601 presented, 0 dropped, onFrame→present 4.3 / 7.8 ms |
| Soak B: 61 min, HEVC, sync 150 + latency sweep 100↔250, device loss / GPU switch every 5 min | 12 recoveries; private 214 → 217 → 215 → 209 MB on NVIDIA (450–480 MB while on the Intel iGPU: shared-memory textures, released on switching back); handles 642–684 (+~1 per recovery = the HEVC MFT semaphore), GDI 14–15; Present − target avg −1.4 ms, |p95| 1.8 ms over 211933 pictures |

### Themes, mascot, frame, REC, dim (2026-10-07)

Screenshots (`--demo-ui2`, 540x960 at 75 %): `v2_frame_portrait`,
`v2_frame_landscape`, `v2_l_frame` (1280x720 window, 50 %), `v2_rot90`,
`v2_rec`, `v2_dimmed` (the mascot screens of that run showed the previous
art and were replaced by `testdata/toutou/`).

### Frame tap measurements (2026-10-07, RTX 3060 Ti)

| Stream | Tap correctness / cost |
|---|---|
| H.264 1170x2532 (rotating, `--tap-dump`) | 360/360 pictures, 2 size changes, pts monotonic, last tapped picture converted on the CPU vs `saveSnapshot` of the same picture: mean \|diff\| 0.003, max 8, PSNR 73.1 dB; 0.6–0.7 ms per picture |
| HEVC Main10 1170x2532 (P010 decode) | 120/120, vs snapshot (10-bit GPU path) mean \|diff\| 0.26, max 4, PSNR 54 dB; 0.64 ms |
| H.264 2560x1440@60 | tap 0.78–0.87 ms avg, p95 1.03–1.11 ms; onFrame→present 3.9–4.2 → 5.0–5.1 ms with the tap |
| HEVC 2560x1440@60 | tap 0.78–0.84 ms avg, p95 1.06–1.20 ms; onFrame→present 2.3–2.4 → 3.0–3.2 ms with the tap |

(3 runs each; the e2e increase is the GPU convert + copy queued before
`Present`. Submitting after `Present` was tried and was worse: the next
decode's GPU fence then waits for it.)

### Android sources (2026-10-07, RTX 3060 Ti)

`--android` at 540x960, 400x800 and 1280x720: all 13 checks pass —
BGRA 720x1280 (stride 2944) snapshot vs submitted pixels max |diff| 0;
rotation 90 + mirror snapshot = exact transpose (max |diff| 0); 10 synthetic
pointer events (hover, drag leaving the picture → clamped Move / Up, wheel −2
and +0.5 notches, right click; a press in the letterbox → nothing) mapped with
max error 0.000000 against the analytic transpose; keys `A` → (0x41, 'a'),
Ctrl+S not forwarded, Ctrl+C → (0x43, 0), IME 「你」 → (0, U+4F60); all handlers
on the UI thread; help link click → callback on the UI thread; frame tap 92
pictures, NV12 → RGB vs pattern PSNR 33.4 dB (4:2:0 on hard colour edges).

| `--bgra-bench 1920,1080 --fps 60` (6 s) | without tap | with tap |
|---|---|---|
| presented / submitted | 361 / 361, 0 replaced | 361 / 361, 0 replaced |
| `submitBgraFrame` call (caller copy) avg / p95 | 0.90 / 1.63 ms | 0.68 / 0.92 ms |
| submit → uploaded avg / p95 | 1.76 / 3.01 ms | 1.50 / 2.01 ms |
| submit → `Present` avg / p95 | 1.90 / 3.28 ms | 1.98 / 2.76 ms |
| frame tap (render thread) | — | 0.53 / 0.80 ms |
| process CPU (incl. the synthetic 8 MB/frame source) | 13.4 % of one core | 13.1 % |

Device loss / WARP / back to auto with a BGRA source: picture back after
198 / 22 / 146 ms (last picture re-uploaded).

Screenshots: `a_idle_hints` (two hint lines + check boxes + link),
`a_link_hover`, `a_bgra_frame` (BGRA picture in the device frame),
`a_idle_after` (540x960, 75 %); `a_n_idle_hints`, `a_n_bgra_frame` (400x800);
`a_l_idle_hints`, `a_l_bgra_frame` (1280x720, 50 %).

### Watchdog before / after (2026-10-08, RTX 3060 Ti + UHD 770, off-screen window)

`ios_like.h265` (HEVC 2560x1440@60, IDR at 0 / 60 s), fault at 10 s (40 s
where noted), `--freeze-report`; "freeze" = longest time AUs arrived but no
new picture was presented; 0.6.1 = `--watchdog off`.

| Fault | 0.6.1 | 0.6.2 |
|---|---|---|
| render thread blocked 1.5 s (23) | 58 AUs dropped → **48.8 s** frozen (until the IDR) | no drop, catches up: 1.6 s (= the block) |
| device removal + `WM_DISPLAYCHANGE` (0) | **50.1 s** | 0.9 s (583 AUs re-fed in 633 ms) |
| display change → other GPU (26) | **50.0 s** | 1.0 s (re-feed); at 40 s (cache truncated): switch deferred to the IDR, 0.3 s |
| decoder swallows its output (20) | never recovers (66 s, end of run) | 1.5 s detection + 0.7 s re-feed, then 60 fps |
| … and the next HW instance too (20:1) | never recovers | 2nd restart in software: 9.2 s (4 s SW re-feed, SW HEVC 1440p < 60 fps), DXVA again at the next IDR |
| `Present` swallowed (22) | never recovers | 1.5 s, swap chain re-created, picture 4 ms later |
| `DXGI_STATUS_OCCLUDED` 5 s (21) | no freeze, 60 fps while occluded | no freeze, 10 fps while occluded, full rate at the first `S_OK` |
| covering topmost layered window 3 s | no freeze (DWM reported no occlusion for the off-screen window) | same |
| H.264 1080p, device removal | 0.26 s (the MS H.264 MFT conceals missing references) | 1.2 s (full re-feed) |
| 5 non-IDR AUs lost (24) | 50.3 s | 50.3 s: references lost, logged, waits for the IDR |
| device removal at 40 s (GOP > 30 s cache) | 20.1 s | 18.5 s: same limit, logged |

Regressions: `--android` 12/12 PASS, `--demo-ui2` ok, 3 min soak (private
154–159 MB, handles 656–664 flat, 0 drops), `--sync 150 --jitter 40` |p95|
1.9 ms, GPU switch hooks 0/1/2/3 recover, no watchdog action in any of them.

### Magnifier / freeze / overlay (2026-10-08, RTX 3060 Ti, off-screen)

`--magnifier` with the Japanese settings screen: 0 failures (grabPicture
mean |diff| 0.000 vs the submitted frame; snapshot while zoomed 0.099 — the
moving bar; pointer at zoom 2.5 mapped to (0.4803, 0.2301) as expected;
frozen: two window shots 0.7 s apart identical while the tap delivered +29
pictures; region select (0.100, 0.200)–(0.900, 0.500) for a drag over
exactly that; through rotation + mirror + zoom (0.375…0.625) as computed).
`--freeze-stream` (H.264 1920x1080 DXVA): frozen window identical over 0.6 s
while the tap got +57 pictures; unfrozen mean |diff| 5.3. No measurable
cost: zoom and filter are four constants of the existing draw; the overview
is one more quad; a freeze is one `CopyResource`. The regular stream test
(`--fps 240 --tap`) is unchanged (decode 2.7 ms avg, tap 0.42 ms avg).

## Known gaps

* Lost reference AUs, or a decoder restart more than 30 s (cache) after the
  last IDR, still freeze an HEVC stream until the next IDR (AirPlay has no
  key-frame request); now only after a ≥ 10 s render-thread stall or a real
  device loss, and logged. The real Snipping overlay was not reproduced
  (owner at the PC); its effect is simulated by faults 21–23.

* Device-loss recovery is verified through the test hook (same code path as a
  real removal, including real GPU switches NVIDIA ↔ Intel iGPU ↔ WARP); a
  real TDR was not provoked (it would reset the GPU for the whole desktop).
* The *HEVC Video Extensions* MFT leaks one semaphore handle per decoder
  instance (also with `IMFActivate::ShutdownObject`; H.264 MFT: none).
  Decoders are kept across reconnects with the same codec, so this only
  grows on codec changes / device losses.
* Vendor *async* hardware MFTs are not used; DXVA comes through the Microsoft
  MFTs, which is enough for H.264/HEVC on current GPUs.
* Dropping a non-IDR AU on overflow causes artifacts until the next IDR; the
  sink API has no way to request a key frame from the phone.
* SW HEVC (HEVC extension in software mode) holds one picture (+1 frame latency).
* `runMessageLoop()` posts `WM_QUIT` on close, ending the thread's message loop.
* Remote control: `ch` relies on the host loop calling `TranslateMessage`
  before `DispatchMessage` (both `runMessageLoop` and the app do). X1 / X2
  mouse buttons and touch-screen / pen input (`WM_POINTER`) are not forwarded
  (touch arrives as promoted mouse messages).
* A BGRA picture costs two CPU copies (caller → mailbox, mailbox → upload
  heap); fine at 1080p60 (~1 ms each), not tuned for 4K.
