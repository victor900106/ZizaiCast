# 傳到手機 / Send to phone (`share/`, app wiring in `app/`)

After a screenshot or a recording, send it straight to the phone's photo
album.

| phone | how | user does |
|---|---|---|
| Android **mirroring over wireless debugging** (AndroidSource state Mirroring) | `AndroidSource::pushToGallery` → `adb push` + media scan | nothing: toast 「已傳到手機相簿：name」 |
| iPhone / iPad (AirPlay), Android over Miracast, or nothing connected | `pm::share::Server`: LAN page behind a QR code, 10 minutes | scans the QR with the camera, long-presses the picture → 「加入照片」 / downloads the video |

If the adb push fails (phone gone, adb error) the app falls back to the QR
path for that file and the rest of the queue (toast 「無法直接傳到 Android
手機，改用 QR 碼」).

## Several captures, 自動傳到手機 (0.7.1)

* **This run's captures**: every screenshot / saved recording is remembered
  (`App::captures`, oldest first) with a *sent* flag — set when the file is
  pushed or queued for adb, put on a QR page, or added to the live page.
* **傳到手機** (toolbar button, chip, the new first menu item 「傳到手機」 with
  「N 個未傳」 on the right) sends the captures **not sent yet**: none → the
  newest file as before; exactly one → that file at once (the old one-click
  path); two or more → the **picker**.
* **Picker** (`pm::ui::SharePicker`, app/share_picker.*; themed like the
  other panels): 「傳到手機」 ×, 「這次拍的截圖和錄影，勾選要傳的」, a 4-column
  grid of thumbnails, newest first (two rows visible, wheel scrolls, a thin
  bar shows the position; at most 48), each with a round check box (the
  unsent ones ticked), 「已傳」 on sent ones, ▶ on recordings, the time under
  it; footer 全選 / 全不選, 取消, 「傳送 N 個」 (dim at 0). Enter = send, Esc =
  cancel, Ctrl+A = all / none. Thumbnails come from a below-normal-priority
  worker thread (WIC scaled decode for pictures, the shell's thumbnail —
  Media Foundation's frame — for MP4s), so opening it never waits on a
  decoder. 傳送 → the ticked files go the usual way (adb or QR).
* **截圖／錄影後自動傳到手機** (check item in both menus under 傳到手機…;
  settings.ini `auto_share=1`): every new capture is sent at once, the chip
  is not shown.
  * Android mirroring over adb: queued `pushToGallery` (one at a time on
    AndroidSource's push thread; a capture taken while a push runs waits in
    `pushQueue`), no 「正在傳到手機」 toast, then a short 「已傳到手機相簿」.
    A manual send while pushing is queued too (toast 「已排入傳送（還有 N 個）」;
    the old 「上一個檔案還在傳送中」 refusal is gone).
  * Anything else: a **live share** (below). Turning the option on starts
    it right away (empty page + QR panel) so the phone can open the page
    before the first capture; each new capture is added to it (toast
    「已傳到手機上的頁面」), the panel is not reopened. If no live share runs
    (closed by 停止, expired, the app restarted with the option on), the next
    capture starts a new one and shows its QR.
  * Live QR panel: 「自動傳送中 · 已傳 N 個」 instead of the countdown, the hint
    「頁面開著，新的截圖和錄影就會自動出現。關掉這個視窗也會繼續傳。」, the
    newest file's name, button 「停止自動傳送」 (= the option off: page ends,
    toast 「已關閉自動傳到手機」). × / Esc only hide the panel; 傳到手機 shows it
    again (and adds unsent captures to the page).
  * Turning the option off stops the live share.
* **Never in the way of the picture**: pushes run on AndroidSource's thread,
  the server on its own threads, thumbnails on a worker; the UI thread only
  queues. (The PNG of a screenshot is still written on the UI thread as
  before; the video is rendered on the window's worker thread, so it does
  not stutter.) Measured off-screen with 3 quick screenshots + a recording
  while 4 slow (1.5 s) fake-adb pushes ran: the video's 5 s summaries show
  `drop 0 skip 0`, every picture presented.

## UX (app/main.cpp, app/share_panel.cpp)

* **Chip**: after 截圖 and after a recording is saved (unless 自動傳到手機 is
  on), a small themed 「⇪ 傳到手機 | ×」 button (`pm::ui::ShareChip`) appears centred just above
  the video window's toast for 8 s (stays while hovered, 2.5 s after the
  cursor leaves). Click = 傳到手機 (the unsent captures, see above). The video window's toasts have no actions, so this is an
  owned no-activate popup of the app (follows the window on move / resize,
  never takes the focus).
* **Menus** (context and tray, under 開始錄影): 「把最後一張截圖傳到手機」,
  「把最後一段錄影傳到手機」 (greyed when there is none; "last" = taken this run,
  else the newest file in 截圖 / 錄影, never the recording in progress) and
  「傳到手機…」 (Windows file dialog, multi-select, starts in 截圖 with 截圖 and
  錄影 pinned as places; png / jpg / mp4 / mov; up to 50 files).
* **Toolbar** (live picture): share button (U+E72D) after 錄影, tooltip
  「傳到手機（最後的截圖／錄影）」 = the newer of the last screenshot / recording.
* **Android auto path**: toast 「正在傳到手機：name…」 while pushing (one file at
  a time), then 「已傳到手機相簿：name」, or 「已傳到手機的 Pictures/ZizaiCast，
  相簿稍後就會出現」 when MediaStore did not list it yet.
* **QR panel** (`pm::ui::SharePanel`, like the pairing panel; follows 主題 and
  語言): 「傳到手機」 ×, 「用手機相機掃描」, 「手機要和這台電腦連在同一個 Wi‑Fi。」,
  the QR (whole-pixel nearest-neighbour on a white card), the file name or
  「N 個檔案」, the URL (ellipsized) + 「複製連結」, countdown 「9:58 後失效」,
  status 「手機已開啟頁面（192.168.1.23）」 / 「手機正在看：name」 /
  「手機正在下載：name」, and 停止分享. ×, Esc and 停止分享 stop the server
  (toast 「已停止分享」). Expired: the QR fades, 「連結已失效」, buttons 關閉 /
  再分享 10 分鐘 (new token, new port, same files). Another share while the
  panel is open replaces the old one (the old URL dies).
* Quitting the app (or an update) stops the server; it never outlives the
  process.

## The LAN server (`pm_share`, share/include/pm/share_server.h)

* **Address**: the first of the AirPlay server's `advertisedInterfaces()` that
  is a current LAN interface (where phones already see the receiver), else the
  best of `pm::share::lanInterfaces()` — same rules as core/src/mdnsd: up,
  IPv4, not loopback / 169.254, real adapters before VPN / VM ones (Hyper-V,
  WSL, Tailscale, WireGuard, …; those only when nothing else is up and only
  with a gateway), gateway first, then route metric. Bound to **that address
  only** (never 0.0.0.0), `SO_EXCLUSIVEADDRUSE`, random port 20000–60999
  (BCryptGenRandom; OS port as last resort).
* **URL**: `http://<ip>:<port>/<token>/`, token = 32 characters [A-Za-z0-9]
  (BCryptGenRandom, no modulo bias, ~190 bits), compared in constant time.
  Routes: `/<token>/` page, `/<token>/v/<i>` inline, `/<token>/d/<i>`
  attachment, `/<token>/zip[?from=F]` all files (index ≥ F) as one ZIP
  (below), `/<token>` → 301 to `/<token>/`. Everything else is a plain
  404 (no listing, files are only addressable by index, `..` means nothing).
  GET and HEAD only (405 otherwise).
* **LAN only**: a peer outside the bound interface's subnet is closed at
  accept (log 「refused (not on this LAN)」); loopback is allowed (this PC).
  At most 24 connections; 16 KB header cap, 20 s receive / 30 s send
  timeouts, HTTP/1.1 keep-alive (≤ 200 requests per connection).
* **Files**: `Content-Type` by extension (png, jpeg, gif, webp, heic, mp4,
  mov, webm), `Content-Disposition: inline|attachment; filename="ZizaiCast_…";
  filename*=UTF-8''%E8%87%AA…` (readable ASCII fallback for 自在投影_… names),
  `Accept-Ranges: bytes`; single byte ranges (`a-b`, `a-`, `-n`; a
  multi-range request gets its first range; past the end → 416 with
  `Content-Range: bytes */size`) — what Safari's video player needs. Files
  are opened per request with full sharing (a recording that is replaced
  later is served as it is then); 256 KB chunks.
* **Headers**: `Cache-Control: no-store`, `Referrer-Policy: no-referrer`
  (the token never leaks to another site), `X-Content-Type-Options: nosniff`,
  `X-Robots-Tag: noindex`, `X-Frame-Options: DENY`, page CSP `default-src
  'none'; img-src 'self'; media-src 'self'; style-src 'unsafe-inline';
  script-src 'self'; connect-src 'self'`: the only script is
  `/<token>/app.js` (same origin, no inline script, no eval); the page's
  texts are a `<script type="application/json">` data block whose JSON never
  contains a raw `<`.
* **ZIP** (0.7.3, `GET /<token>/zip[?from=F]`, for 「全部下載」): the files
  with index ≥ F, in index order, as one classic ZIP, **stored** (no
  compression: pictures / MP4s do not shrink, and the phone gets bytes at
  once), streamed straight from the files. Each file's CRC-32 is computed
  on first use and cached (keyed by size + last-write time), so the
  response carries a `Content-Length` (Safari shows progress) and needs no
  data descriptors; names are UTF-8 (flag bit 11; a repeated name becomes
  `name (2).png`), times are the files' local last-write times.
  `Content-Type: application/zip`, `Content-Disposition: attachment;
  filename="ZizaiCast_YYYYMMDD_HHMMSS.zip"`. A file that is gone is left out;
  files that would push the archive past 4 GB are left out (no ZIP64; they
  keep their own 下載 button, logged `zip: left out file i`); a file that
  vanishes mid-stream cuts the connection (a failed download, never wrong
  bytes). HEAD gives the length without the CRC pass. `from` past the end /
  any other query → 404. `onAccess` fires per file as it streams (the panel
  shows 「手機正在下載：name」). Log: `… GET zip from 0 200 (3 file(s), N bytes)`.
* **Live share** (`Options::live`, 自動傳到手機): may start with no file;
  `addFile(path)` adds one while serving (same path → same index; up to
  `kMaxLiveFiles` 500; `addFile` works on a normal share too). Files are
  guarded by a mutex, indices never change. `GET /<token>/list?n=K` (JSON
  `{"live","n","files":[{i,name,video,size,sizeText,type}]}`) is held up to
  20 s until file K+1 exists (live only; a normal share answers at once),
  and is woken at once by `addFile`, expiry (→ 410) and `stop()`.
* **Live expiry** — same security model, longer life: same LAN-only bind,
  same 32-character token, but the link stays valid **while 自動傳到手機 is on
  and the app runs**, capped at **12 hours** (`kLiveShareHours`, the
  `ttlSeconds` of a live share). It ends earlier when the option is turned
  off, 停止自動傳送, the app quits / updates. After the cap the page gets 410
  「這個分享已結束」 with 「電腦上的自動傳送已經停止…」; the next capture starts a
  new share (new token, new port, the panel shows the new QR). Rationale: the
  phone keeps the page open for a whole session; 12 h covers a working day
  without a forgotten token living for days.
* **Expiry**: `ttlSeconds` (600). At the deadline `onExpired` fires once;
  from then on every request with the right token gets 410 and the
  「這個分享已結束」 page (wrong token: still 404); transfers already running
  finish. The listening socket closes 5 minutes after expiry; `stop()` cuts
  everything at once (shutdown of every connection; measured 0.26 s with a
  64 MB download in flight).
* **Log** (`phonemirror.log`, level `share`): start (address, port,
  interface, files), every request — `share: 192.168.1.23 GET page 200 (ios)`,
  `… GET v/1 "自在投影_….mp4" 206 bytes 1000-1999/300000`, `… GET 404`,
  `… 410 (expired)`, `… refused (not on this LAN)` — expiry and stop. The
  token is never logged.

### The page (share/src/page.cpp)

Mobile-first HTML in the app's language (texts: `Pg*` in
include/pm/i18n_strings.inc), coloured from the current theme (background,
card gradient, accent; light themes get a light page). Header 「從電腦傳來的
檔案」 · 「自在投影 · N 個檔案」. One card per file:

* picture: the image itself (`-webkit-touch-callout: default`, not wrapped in
  a link, so iOS Safari's long-press menu has 「加入照片」 / “Save to Photos”),
  name + size, the tip, and 下載 (on iPhone a secondary button: a download
  goes to Files, not Photos);
* video: `<video controls playsinline preload=metadata src="v/i#t=0.1">`
  (first frame instead of a black box), 下載影片, then the steps — iPhone:
  1 點「下載影片」再點「下載」, 2 打開「檔案」App →「下載項目」→ 點影片,
  3 「分享」→「儲存影片」; Android: the Download folder / album.

**全部下載（ZIP，N 個）** (0.7.3; sticky at the top, a plain `<a href="zip"
download>` rendered by the server, so it works without the script) and
under it one sentence on getting the files into Photos — iPhone: 「存進「照片」：
點上面的按鈕 →「下載」，打開「檔案」App →「下載項目」，點 ZIP 解開成資料夾，進資料夾點
「選取」→「全選」→「分享」→「儲存…」（例如「儲存 3 張影像」）。只要一張的話，長按圖片 →
「加入照片」最快。」; Android: 「下載完成後，在「檔案」App 的 Download 點這個 ZIP →
「解壓縮」，圖片和影片就會出現在相簿。…」; other browsers: both, under small
headings.

*Why a ZIP (0.7.1's 全部儲存 never showed on a real phone):* the Web Share
API (`navigator.share` / `canShare`) exists only in a **secure context**
(https, or http on localhost). The page is `http://<LAN IP>:<port>/…`, so
iPhone Safari (and Android Chrome) have no `navigator.share` there and the
script kept the 全部儲存 bar hidden — the owner's iPhone showed only the
per-file buttons. The 0.7.1 live test stubbed `navigator.share` on
`127.0.0.1` (a secure context) and so could not see it. Options weighed:
* ZIP (chosen): one tap, works in every browser over http; iOS saves it to
  Files → 下載項目, tapping it unzips, then one multi-select share → 「儲存 N
  張影像」 puts all in Photos. Android: Files → Extract → the gallery.
* Several downloads from one tap: iOS Safari asks per file and blocks
  scripted downloads after the first; Chrome asks for "multiple downloads".
  Not reliable.
* A self-signed https server (would make Web Share work): Safari shows a
  full-page 「此連線不是私人連線」 warning that must be clicked through
  (「顯示詳細資訊」→「瀏覽此網站」→ confirm) for each new address; trusting it
  properly means installing a root certificate profile on the phone
  (Settings → 已下載描述檔 → 憑證信任設定) whose private key sits on the PC —
  scary and a real security risk. A public certificate for a LAN name would
  need the private key shipped in the app (revoked) or our own DNS/ACME
  service. Rejected.
* 「長按 → 加入照片」 per picture stays (the cards already show every picture
  large with that tip): the quickest way for one picture.

**Script (app.js, progressive — without it the page works as before):**

* Keeps the ZIP button's count current; on a live page, after a tap on it,
  new files make it 「下載新的 N 個（ZIP）」 → `zip?from=K` (K = one past the
  newest file the tapped ZIP had); with nothing new it offers all again.
* **全部儲存（N）** (replaces the ZIP button) and a per-file **儲存** where the
  browser can share files (`navigator.canShare({files})`: a secure context
  only — https / localhost, never the LAN page): `navigator.share({files:[…]})` with every unsaved file →
  iOS's sheet offers 「儲存 N 張影像」 / 「儲存影片」 and puts them all in Photos at
  once. The share must start inside the tap (iOS refuses it after an
  `await`), so files are fetched ahead into memory, one at a time (≤ 200 MB
  each; bigger ones keep the download button); until then the button reads
  「準備中…（2/3）」. Shared files turn 「已儲存 ✓」, the button 「都已儲存」; new
  ones make it 「全部儲存（1）」 again. A failed share says 「無法開啟分享選單…」.
  When sharing works the long-press / Files-app steps are hidden (the
  button does it); otherwise everything is as before (fallback).
* **Live**: header 「● 即時更新：電腦上新的截圖和錄影會自動出現」, the newest file
  first, an empty-state card 「還沒有檔案。在電腦上截圖或錄影，就會自動出現在這裡。」,
  long poll `list?n=K` → new cards are inserted at the top (briefly
  highlighted) without a reload; 410 → 「分享已結束。這頁上已有的檔案還是可以
  儲存。」; network errors → 「暫時連不到電腦，正在重試…」 and a back-off retry.
  Footer: 「電腦上的「截圖／錄影後自動傳到手機」開著時，這個連結一直有效（最長 12
  小時）…」.

The User-Agent picks the instructions: iPhone / iPad / iPod → iOS only,
Android → Android only, anything else → both under small headings. Footer:
「連結到 HH:MM 前有效（10 分鐘），只有掃描電腦上 QR 碼的人能開啟。」

### Windows Firewall

The server lives inside 自在投影.exe. The app's inbound rule is a *program*
rule — created when the user answers Windows' first-run prompt for the
AirPlay sockets — with protocol TCP/UDP and **LocalPort Any** (checked on
this PC: `Get-NetFirewallApplicationFilter` → the installed
`…\手機投影\程式\自在投影.exe` rules, `Get-NetFirewallPortFilter` → TCP Any /
UDP Any, Private + Public), so it covers the share server's random port too;
no new rule and no installer change are needed. If the user refused that
prompt (a block rule), AirPlay does not work either and the page cannot be
reached; the fix is the same (allow 自在投影 in Windows Security → Firewall →
Allow an app). The installer does not add firewall rules.

## Android: `AndroidSource::pushToGallery` (android/, docs/android.md)

1. `adb -s <serial> shell mkdir -p /sdcard/Pictures/ZizaiCast` (videos:
   `/sdcard/Movies/ZizaiCast`);
2. `adb push <local> /sdcard/…/ZizaiCast/<name>` — the name made ASCII
   (`自在投影_20261008_101010.png` → `ZizaiCast_20261008_101010.png`; shell
   metacharacters dropped), timeout 60 s + 1 s per 256 KB (≤ 30 min);
3. media scan: `content call --uri content://media --method scan_file --arg
   /storage/emulated/0/…` (MediaProvider's own single-file scan, Android 10+;
   shell holds WRITE_MEDIA_STORAGE) and the classic `am broadcast -a
   android.intent.action.MEDIA_SCANNER_SCAN_FILE -d file:///storage/emulated/0/…`
   (Android ≤ 10 and many OEM builds);
4. verified with `content query --uri content://media/external/images|video/media
   --projection _id --where "_display_name='<name>'"` (4 × 0.5 s); still
   missing → `content call … --method scan_volume --arg external_primary`
   and 6 more tries. On Android 11+ the FUSE layer usually indexes a file
   written through /sdcard on close already, so step 4 tends to succeed at
   once.

Runs on its own thread (not the pairing worker; works while mirroring),
one push at a time, `done(PushResult{ok, inGallery, remotePath, error})`
from that thread; the app posts it to the UI thread.

## Tests

`build-app-share\bin\Release\pm_share_test.exe` — **146 checks, all passing
(2026-10-09)**, everything on 127.0.0.1. New in 0.7.3 (ZIP): the page shows
「全部下載（ZIP，3 個）」 without the script (全部儲存 hidden), the iPhone /
Android hint sentences; `zip` → 200 application/zip, attachment
`ZizaiCast_….zip`, Content-Length = body; the archive read back by an
independent parser (end record → central directory → local headers): 3
entries in index order, UTF-8 names, a repeated name → `… (2).png`, bytes
identical, CRC-32 right (bitwise CRC, not the server's table), stored, no
data descriptor; a second download identical (cached CRCs); `zip?from=1`;
HEAD; `from` past the end / garbage → 404; token required; onAccess per
file; a deleted file left out. Also checked by hand: Windows `tar.exe`
(libarchive) and PowerShell `Expand-Archive` list / extract it. (The
「stop() during a 64 MB download」 check failed once in 4 runs with a 30 s
send timeout — an old timing race in that test, not the ZIP code.) New in 0.7.1: script tags only
`app.js` + the JSON block, CSP `script-src 'self'`, JSON has no raw `<`,
`app.js` / `list` need the token; live share: starts empty, `list?n=0` held
until `addFile` (answered ~0 ms after it), same path → same index, newest
first + hidden empty card, expiry wakes a waiting `list` with 410, `stop()`
releases it in < 0.3 s, `addFile` refused after expiry, non-live `addFile`.
Earlier checks:

* helpers: range parsing (a-b, a-, -n, clamp, past end → 416, reversed /
  garbage / other units ignored, multi-range → first), content types, HTML
  escaping, `galleryPath` (自在投影_ → ZizaiCast_, Movies for videos,
  non-ASCII and shell metacharacters);
* server: URL shape / 32-char token / random port, missing files skipped,
  404 for `/`, favicon, wrong / short token, bad index, traversal attempts,
  unknown routes; 405 for POST; 301 for `/<token>`; page 200 + headers
  (no-store, no-referrer, nosniff, CSP, no script), iPhone vs Android tips,
  only shared files listed; whole files byte-identical, inline vs attachment
  + RFC 5987 file names; ranges 0-1, 1000-1999, open end, suffix, 416; HEAD;
  keep-alive (two requests on one connection); oversized header dropped;
  expiry after a 4 s TTL (onExpired once, 410 page + file, wrong token still
  404); stop → connection refused; access log lines (IP, file, status,
  range) and the token absent from the log; restart = new token, old token
  dead, English page; `stop()` during a 64 MB download returns in 0.26 s;
* QR: rendered and **decoded back by OpenCV** (`android/tools/verify_qr.py`)
  to the exact URL;
* **pushToGallery against a fake adb** (`share/tools/fake_adb.cpp`, copied as
  `adb.exe` into a temp `android-tools`; it answers like a phone 「Pixel 8
  測試」 at 192.168.50.7:41234 and logs every invocation): connect via
  `connectKnownDevices`, false without a phone / for a missing file / while
  busy; picture → mkdir, push with the **Unicode local path** (the fake opens
  it), scan_file, broadcast, content query, order push → scan → query, no
  volume scan needed; video indexed only after `scan_volume` → Movies, video
  table, fallback used; never indexed → ok but `inGallery` false; push
  failure → `!ok` with adb's error and no scan.

Page screenshots (headless Chrome, mobile emulation 390 px, iPhone / Android
user agents): `pm_share_test --serve 120 [--en] [--light] FILE…` then
`node share/tools/page_shot.mjs <url> out.png ios|android`.

App, off-screen (`share/tools/app_share_test.ps1`, `dev_cmd.ps1`):
`自在投影.exe --dev --test-offscreen --test-no-network --test-feed F
[--test-source android]` with `PM_VIDEO_OFFSCREEN=1 PM_SHARE_BIND=127.0.0.1
[PM_SHARE_TTL=6]`; DevCommand 107 (截圖) → chip shown; 900 → menus; 160
(toolbar 傳到手機) → QR panel; 903 → `share.png`, `chip.png`,
`share_url.txt` in `%TEMP%\pmshots`. Verified 2026-10-08: the app's QR
decodes (OpenCV) to its URL; curl: page 200, image 200 with the PNG's
size, no token → 404, video `Range: bytes=0-1` → 206, `d/1` attachment;
the panel status turned to 「手機正在看：…」 after the fetch; 164 (停止分享)
→ connection refused; TTL 6 s → 410 and the panel's expired state; quit →
`share: stopped`. With `--test-source android` the auto path was taken
(log `test android: would push …\自在投影_….png → /sdcard/Pictures/ZizaiCast/ZizaiCast_….png`,
toast 「（測試）已傳到手機相簿」). 傳到手機… in `--test-offscreen` skips the
dialog and shares the last screenshot + last recording (2 files).

Several captures / 自動傳到手機, off-screen (`share/tools/app_share_batch_test.ps1
-Mode picker|autopush|live`; restores the dev settings.ini). Verified
2026-10-08:

* **picker**: 2 screenshots, a recording, a screenshot → chip, menus show
  「傳到手機 4 個未傳」; toolbar 傳到手機 → picker with 4 thumbnails (the MP4's
  from the shell), all ticked, rendered in 繁中 / English / 日本語 / 한국어
  (`batch_picker_3-picker-*.png`); untick the recording → 「傳送 3 個」 → QR
  page `list` has exactly those 3; 傳到手機 again → the one unsent file (the
  recording) goes at once.
* **autopush** (`--test-source android`, `PM_SHARE_FAKE_ADB=<dir with
  pm_share_fake_adb.exe as adb.exe>`, `FAKE_ADB_PUSH_MS=1500`): option on,
  3 screenshots 0.3 s apart + a recording → log 「queued (1 waiting)」,
  「(2 waiting)」, four pushes in capture order (fake adb log: 3 ×
  Pictures/ZizaiCast, 1 × Movies/ZizaiCast), all 「in the gallery」; the
  window shot shows 「已傳到手機相簿」; video `drop 0 skip 0` throughout.
* **live** (`share/tools/live_page_test.mjs`, headless Chrome, iPhone UA;
  0.7.3: two tabs). **http tab** (`http://zizai.test:<port>/…`, mapped to
  127.0.0.1 by `--host-resolver-rules`, `--disable-features=HttpsUpgrades`:
  a real non-secure origin like the phone's LAN address, nothing stubbed):
  `isSecureContext` false and `navigator.share` undefined; with 3 files
  「全部下載（ZIP，3 個）」 and the Files → Photos sentence are visible, 全部儲存
  is not; a real tap downloads `ZizaiCast_….zip` (Browser.setDownloadBehavior
  into a temp dir), unzipped in node: 3 files = the server's (names in
  order, sizes, bytes, CRC-32, UTF-8 flag); after one more 截圖 the button
  reads 「下載新的 1 個（ZIP）」 → `zip?from=3` with just that file. Verified
  2026-10-09, all 29 checks. **share tab** (`127.0.0.1`, `navigator.share`
  stubbed): option on → live panel + empty page; app 截圖 →
  the open page shows the card by itself; another 截圖 + a recording → 3
  cards, recording on top, 「全部儲存（3）」; a real mouse tap → `share()` once,
  inside the user activation, with 3 Files whose names / sizes / types equal
  the server's list; cards 「已儲存 ✓」; one more 截圖 → 「全部儲存（1）」.
  Page shots in 4 languages: `pm_share_test --serve 60 --lang 0..3 FILE…`
  + `page_shot.mjs <url> out.png ios 390 stub` (`none` = a browser without
  file sharing: the fallback page).

Dev-only hooks: `PM_SHARE_FAKE_ADB` (adb pushes through a fake adb.exe in
`--test-source android`), DevCommand 908 (picker 傳送), 909 (tick item lp /
-1 all), 910 (video window with its toast → `window.png`), 903 also writes
`picker.png`, command 165 (CmdShareAuto). `--test-no-network` (no AirPlay / Miracast / adb, so a fresh
build path never triggers a Windows Firewall prompt — also after a language
switch), `PM_SHARE_BIND` (bind address), `PM_SHARE_TTL` (seconds),
DevCommand 903, command ids 160–164 (CmdShareLast, …LastShot, …LastRec,
…Pick, …Stop).

## Needs the owner's phones

* **0.7.3, iPhone** (http page, no Web Share): scan the QR → 「全部下載（ZIP，N
  個）」 at the top → 下載 → 檔案 App → 下載項目 → tap the ZIP (unzips to a
  folder) → 選取 → 全選 → 分享 → 「儲存 N 張影像」 → all in Photos. Check the
  Files wording on the owner's iOS version and a ZIP with a recording
  (「儲存 N 個項目」).
* **0.7.1, iPhone**: turn on 截圖／錄影後自動傳到手機 with the iPhone mirroring
  over AirPlay, scan the QR, keep Safari open; take screenshots / a
  recording → they appear on the page by themselves; 全部儲存 → the share
  sheet offers 「儲存 N 張影像」/「儲存 N 個項目」 → all in Photos. Check that a
  locked / backgrounded Safari picks up again when reopened, and how large a
  recording iOS Safari still prefetches (cap 200 MB per file).
* **0.7.1, Android over wireless debugging**: option on, several quick
  screenshots + a long recording → all in the gallery in order; watch the
  mirror while a big recording is pushed (adb shares the Wi-Fi with scrcpy:
  no drops expected at screenshot sizes, a several-hundred-MB push may
  lower the mirror's bitrate for its duration).

* **iPhone / iPad**: scan the panel's QR with the Camera app (same Wi-Fi) →
  Safari opens the page; long-press the picture → 「加入照片」 → it is in
  Photos; for a recording: 下載影片 → 下載 → 檔案 App → 下載項目 → 分享 →
  儲存影片. Also check the inline video plays (range requests) and that the
  link answers 410 after 停止分享 / 10 minutes.
* **Android over wireless debugging** (Android 11–15, ideally a Pixel and a
  Samsung): while mirroring, 截圖 → chip 傳到手機 → the picture appears in
  Google Photos / Samsung Gallery (Pictures/ZizaiCast) within seconds; same
  for a recording (Movies/ZizaiCast). The log shows which scan made it
  visible (`content query` row after scan_file / broadcast, or after
  scan_volume). Unverified without a device: whether `content call …
  scan_file` is permitted for shell on each version (the broadcast and
  scan_volume are the fallbacks) and OEM gallery refresh timing.
* **Android over Miracast**: QR path (Chrome long-press → 下載圖片).
* A PC with a VPN / Hyper-V adapter: the QR must show the Wi-Fi / Ethernet
  address the phone can reach.
