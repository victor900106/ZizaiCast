# docs/tutorial

User-facing guides for 自在投影 / Zizai Cast in four languages. Since 0.8.3 (ux-tutorial) the four
HTML guides have **the same content and the same sections** and are generated from `src/`.

| file | language | what |
|---|---|---|
| `自在投影教學.html` | 繁體中文 | Self-contained guide (inline CSS + SVG, mascot and WebP screenshots as data URIs, ~0.5 MB, works offline). |
| `ZizaiCast-Guide.html` | English | Same guide in English. |
| `ZizaiCast-Guide-ja.html` | 日本語 | Same guide in Japanese (it used to be a shorter one). |
| `ZizaiCast-Guide-ko.html` | 한국어 | Same guide in Korean (it used to be a shorter one). |
| `使用說明.txt` | 繁體中文 | Short plain-text version (UTF-8 BOM, CRLF), meant to replace `installer/使用說明.txt`. |
| `ZizaiCast-ReadMe.txt` | English | English short plain-text version (UTF-8 BOM, CRLF). |

The app opens the guide that matches its UI language (`app/help/tutorial.cpp` `guideName()`, falling
back to English, then Chinese); the remote phone page opens it at `/<token>/help`. The installer puts
the guide + readme of the installation language at the top of the install folder and all guides in `程式\`.

## Structure (every guide)

Each task is written as **你要做什麼 / What you want to do → 3–5 steps → 看到這個就成功 / Success looks
like this → 不成功怎麼辦 / If it doesn't work** (`.goal`, `ol.steps`, `.ok`, `.ng`). The page starts with a
「我想要…」 / “I want to…” table (`#want`) that links to every task.

Anchors (all four guides have exactly the same ids; the ones the app or the phone page open are marked *):

* `#want`, `#before`, `#idle`
* `#iphone`*, `#disconnect`
* `#miracast`* (Android casting)
* `#adb`* (Android control) with `#adb-devopt`, `#adb-pair`, `#adb-usb`, `#adb-control`
* `#features` (empty marker kept for old links), `#record`, `#ink`, `#share`, `#translate`, `#magnifier`, `#update`
* `#remote`*, `#remote-open`, `#remote-home`, `#remote-outside` (fixed address), `#remote-tailscale`*
  (Tailscale from zero), `#remote-funnel` (allow Funnel + HTTPS with `tailscale funnel --bg --https=8443 http://127.0.0.2:8091`),
  `#remote-pwa`, `#remote-friend` with `#remote-guest` (臨時帳號: the address carries the account) and `#remote-nolink`
  (temporary address), `#remote-gestures`, `#remote-pc` (another computer's browser, 0.8.3.5), `#remote-safety`, `#remote-limits`
* `#sound` with `#remote-audio`
* `#faq`, `#faq-notfound`, `#faq-audio`, `#faq-lag`, `#faq-blur`, `#faq-takeover`, `#faq-fw`, `#faq-vpn`,
  `#faq-miracast`*, `#faq-adb`, `#faq-disconnect`, `#faq-translate`, `#remote-trouble`*, `#faq-funnel`,
  `#faq-funnel-wait`, `#remote-faq-noconnect`, `#remote-faq-nosound`, `#remote-faq-forgot`

## Editing and building

* Text: `src/zh.html`, `src/en.html`, `src/ja.html`, `src/ko.html` (the body only). Change all four together.
  App labels must match the string tables (`include/pm/i18n_strings.inc` zh / en columns,
  `include/pm/i18n_strings_jako.inc` ja / ko) and the phone page's table in `remote/web/index.html`.
* Look: `src/base.css` (the 0.8.2 design; `__FONT__` / `__SVGFONT__` / `__MASCOT__` are filled per language)
  and `src/extra.css` (the 0.8.3 task blocks).
* Illustrations: `src/fig/<lang>-<name>.html` (SVG). ja / ko use the English `iphone`, `cast`, `devopt`, `pair`,
  `mouse` figures with their texts translated by `src/fig/ja.json` / `ko.json`. Example addresses in figures
  are documentation ranges (192.0.2.x, 203.0.113.x).
* Build: `node docs/tutorial/src/build.mjs [imageDir]` (default `_shots/tut/web`, not in git). A missing
  image becomes a grey placeholder; `{{todo:…}}` becomes a `TODO-0832` placeholder (none left since 0.8.3.3).

## Screenshots

Only off-screen / headless pictures with made-up data: a fake phone screen (`src/shots/feed/*.html`), the
pairing panel's dummy pattern (not a decodable code), 203.0.113.x and `example` addresses
(`PM_REMOTE_SHOWLAN`, `PM_REMOTE_FUNNELFAKE`), a share link on 127.0.0.1 (`PM_SHARE_BIND`), a made-up update
served on 127.0.0.1. Every QR code was decoded after the build (`rel083_work/qr_scan.py`): only those.

`src/shots/` has the scripts used (0.8.3 ux-tutorial):

* `ux_run.mjs` — one `--dev --test-offscreen --test-no-network --demo-branding` instance per run, own data
  folder (`PhoneMirror-dev-uxaudit`) and TEMP, `PM_REMOTE_LOOPBACK=1` (127.0.0.2 / .3 only: no firewall
  prompt), `PM_REMOTE_DRYRUN=1`, fake Funnel / tunnel (`tailscale` and `cloudflared` are never run); every
  window is checked against every monitor after start and before quit (`offcheck.ps1`), the own PID is
  killed if one is on a screen. `PM_EXE=<自在投影.exe>` is required; `UX_WORK` is the scratch folder.
* `s9_tut.mjs <lang>` — the guide pictures (idle, pairing, toolbar, screenshot, recording, pen with posted
  strokes `stroke.ps1`, translate, Android, drop hint, share, remote tab, fixed address, phone sign-in page
  in headless Chrome, update). `phone_sheet.mjs` — the phone page's settings sheet from a copy of
  `remote/web` opened as a file. `prep.py <langs>` — crop / resize to WebP. `check_guides.mjs` — ids,
  links, TODO count. `guide_shot.mjs` — full-page shot of a guide for checking the layout.
* Feeds: render `feed/phone_en.html` / `phone_ja.html` at 720×1560 with headless Chrome, then
  `ffmpeg -loop 1 -i phone_en.png -t 6 -r 30 -vf format=yuv420p -c:v libx264 -bf 0 -g 30 -x264-params repeat-headers=1 -bsf:v h264_mp4toannexb -f h264 phone_en.h264`.

Screenshots and recordings made by the runs land in the exe's `截圖` / `錄影` folders: delete them after.

## 0.8.3.3

The 0.8.3.2 / 0.8.3.3 screens (the TODO-0832 placeholders, all filled): `node tools/shots_0833.mjs <自在投影.exe>
<repo>/_shots after` (off-screen `--dev` instances, made-up addresses, the fake tailscale CLI for the Funnel
enable card, the phone page in headless Chrome), then `python docs/tutorial/src/shots/prep_0833.py zh-TW en ja ko`
(remote-off, remote-lan, remote-allow, remote-fixed, remote-preparing, remote-enable, guest-new, guest-made,
phone-login, phone-login-guest, phone-bar, phone-settings, pair, pair-steps, idle, snapshot -> `_shots/tut/web`).
Texts follow the string tables: 固定網址 / 臨時網址 only, 手機音量 100 % (up to 400 %), 遠端斷線後自動鎖定電腦 off
by default, 臨時帳號 options (用一次 / 1 小時 / 24 小時 / 7 天, 可操作 / 僅觀看, 允許傳檔).

## 0.8.3.4

Retaken for 0.8.3.4 (rel-0834), same made-up data: `node docs/tutorial/src/shots/s9_tut.mjs <lang> bc`
(toolbar, recording, pen mode with its pink frame, picker, share, translate with 翻譯中 · 畫面已暫停 at the top
right), `node tools/shots_0833.mjs <自在投影.exe> <repo>/_shots after --only ABDEFHP` (the remote page in the
0.8.3.4 layout, guest panels, pairing, the screenshot message with 查看; its phone pages now in
chrome-headless-shell), `node remote/tools/phone_shots_0834.mjs <dir>` (the settings sheet with 電腦喇叭靜音 /
響度加強, copied to `_shots/after/<lang>_P2-settings-0834.png`), then `prep.py`, `prep_0833.py` and
`prep_0834.py zh-TW en ja ko` in that order (prep_0834: the 0.8.3.4 remote page starts right under the tabs).

## 0.8.3.5

Retaken for 0.8.3.5 (rel-0835), same made-up data: `node tools/shots_0833.mjs <自在投影.exe> <repo>/_shots after --only AP`
(guest panels with 可以傳檔 ticked, the 臨時帳號 password the made-up `ABC123` from `PM_TEST_GUEST_PW` (--dev only;
0.8.3.4's picture showed a generated one), the phone bar with six buttons), and for the new part `#remote-pc`
`node remote/tools/pcmouse0835_test.mjs <pm_remote_spike.exe> <out> --only shots --shots <dir> --lang N` (N = 0-3;
chrome-headless-shell, spike `--inject-stub`, a made-up desktop) with its `S1_pc_browser_<lang>.png` /
`S2_phone_first_<lang>.png` copied to `_shots/after/<lang>_S1-pc-bar.png` / `<lang>_S2-phone-mode.png`; then
`prep_0834.py` and `prep_0835.py zh-TW en ja ko` (pc-bar, phone-mode) and `build.mjs`. check_guides: 54 ids, no broken
links, no placeholders, 27 pictures per guide.
