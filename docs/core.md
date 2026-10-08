# core/ — AirPlay protocol core (`pm_core`)

MSVC port of UxPlay's protocol library (`_ref/uxplay/lib`, upstream commit
`3dbf7ce`, 2026-10-05, GPL-3.0; bundled LGPL-2.1 / MIT parts keep their
headers) plus a small C++ facade, `pm::AirPlayServer`.

## Layout

| Path | What |
|---|---|
| `core/src/*.c,*.h` | UxPlay `lib/` verbatim except `// PM:` changes (raop, rtp, mirror, ntp, pairing, srp, fairplay, httpd, dnssd) |
| `core/src/llhttp/` | bundled llhttp (MIT) → `pm_llhttp` |
| `core/src/playfair/` | bundled FairPlay playfair → `pm_playfair` |
| `core/src/mdnsd/` | UxPlay's internal mDNS responder (no Apple Bonjour needed) |
| `core/src/pm_win/` | PhoneMirror POSIX shim: `pm_posix.h` (force-included into every C file: `usleep`, `clock_gettime`, `ssize_t`), `unistd.h` |
| `core/src/airplay_server.cpp`, `core/include/pm/airplay_server.h` | `pm::AirPlayServer` |
| `tools/pm_probe/` | console harness (`pm_probe.exe`) |

`lib/dns_sd/` (external Bonjour/Avahi backend) is intentionally not copied.
Find every local change with `grep -rn "PM:" core/src`. Upstream files touched:

| File | Change |
|---|---|
| `threads.h` | MSVC: `thread_handle_t` = heap `pthread_t*` (pthreads4w's `pthread_t` is a struct; upstream uses `handle = 0` / `!handle`) |
| `httpd.c` | **bug fix**: `run_mutex` was never `MUTEX_CREATE`d (zeroed mutex works in glibc, crashes pthreads4w) |
| `crypto.c` | key file via `BIO_new_file` instead of `FILE*` (FILE* across OpenSSL DLL needs Applink → abort) |
| `raop_ntp.c` | fallback `recvfrom` passed `&addrlen` (an `int**`) as `socklen_t*` |
| `compat.h` | no `#define snprintf _snprintf` under MSVC (UCRT conflict, and `_snprintf` doesn't NUL-terminate) |
| `compat.c` | `wsa_strerror` → `FormatMessageW` + UTF-8 (localized Windows messages were mojibake) |
| `sockets.h` | `SOL_TCP` → `IPPROTO_TCP` |
| `raop_rtp.c` | `[audio-timing]` INFO logs: first datagram / first real audio packet (with count of "no data" packets) / first packet out of the jitter buffer, in ms after the audio stream start; TEARDOWN; SETUP while already running |
| `raop_rtp_mirror.c` | per-frame receive timestamps (`pm_t_header/payload/decrypted` in `stream.h`); optional immediate TCP ACKs (`pm_mirror_quick_ack`); logs SO_RCVBUF at connect |
| `stream.h` | `video_decode_struct` + the three `pm_t_*` timing fields |
| `raop_buffer.c/.h` | **bug fix (audio silent ~2.8 s after pause→play)**: "no data" seqnums are recorded (`raop_buffer_mark_nodata`) and stepped over; a hole waits for a resend at most `max_wait_ms` (40 ms) after the next packet arrived, then is skipped; counters (`raop_buffer_get_stats`) |
| `raop_rtp.c` (2) | reports no-data seqnums (also resent ones) to the buffer; `[audio-timing]` gap trace (see below); counts resend requests |
| `raop_ntp.c` (2) | **bug fix (no NTP mapping at all)**: `client_time_received` was initialised `false` and never set, so `raop_ntp_convert_remote_time()` returned 0 for every frame/packet (`ntp_time_local` = 0, the `[video-timing]` "lead … 0.0 (some frames without NTP sync)"). Now set when the first NTP exchange produced an offset. Also: the "kernel timestamps Disabled" hint is logged only when they really are unavailable (was logged after every exchange) |
| `mdnsd/mdnsd.c/.h` | **multi-interface IPv4 mDNS** (upstream: one interface): joins 224.0.0.251 on every suitable interface, announces per interface with its own A record, answers a query with the A record of the interface it arrived on (`WSARecvMsg` + `IP_PKTINFO`, subnet match as fallback), ignores queries from non-advertised (VPN/VM) interfaces; `mdnsd_scan_interfaces / mdnsd_get_interfaces / mdnsd_restart` |
| `raop.c`, `raop_handlers.h`, `raop.h`, `httpd.c/.h` (takeover) | **multi-phone takeover** replaces upstream "nohold" (see "Several phones"): a 2nd connection coexists until its first SETUP; then the old session is closed synchronously (`httpd_pm_remove_connection_now`) or the newcomer is refused (409); `raop_pm_set_takeover_policy`, callbacks `pm_takeover` / `pm_conn_end`; a full TEARDOWN ends the session's "active" mark |
| `raop_handlers.h`, `pairing.c/.h` (PIN) | **PIN enforcement**: with PIN on, SETUP needs a completed pair-verify on that connection (upstream accepted a SETUP without any pairing), and a PIN-less `/pair-setup` + pair-verify goes through `check_register` like a returning client (upstream skipped it: PIN bypass) |
| `mdnsd/dnssd_mdnsd.c`, `dnssd.h` | `dnssd_pm_get_interfaces / dnssd_pm_scan_interfaces / dnssd_pm_restart / dnssd_pm_announce` for the facade |

Compile definitions (core/CMakeLists.txt): `WIN32` (UxPlay tests `WIN32`, MSVC
only defines `_WIN32`), `NOHOLD`, `PLIST_210 PLIST_230`, `HAVE_STRUCT_TIMESPEC`,
`OPENSSL_API_COMPAT=0x10101000L`; `/FIpm_posix.h` on every pm_core source.
`pm::AirPlayServer` calls `WSAStartup` before `dnssd_init` (else mdnsd's
`gethostname` fails and the SRV target becomes `UxPlay.local`) and
`ntp_global_init()` (QPC frequency for the Windows kernel-timestamp path; a
division by zero otherwise) — both done in `uxplay.cpp main()` upstream.

## Build

```
cmake -B build-core -G "Visual Studio 18 2026" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=%USERPROFILE%/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build-core --config Release --target pm_probe
```

Builds with 0 warnings / 0 errors at /W3 (a few upstream int-conversion
warnings C4244/4267/4018/4996/4146 are disabled for the C sources only).
For symbolised crash stacks in pm_probe, build `--config RelWithDebInfo`.

vcpkg (classic, x64-windows): `openssl`, `libplist`, `pthreads` (pthreads4w).
libplist/OpenSSL/pthreads are DLLs; vcpkg's applocal step copies them next to
the exe.

## API contract (as implemented)

* `VideoSink::onFrame` gets Annex-B: UxPlay's `raop_rtp_mirror.c` already
  replaces the 4-byte AVCC length prefixes with `00 00 00 01` and prepends the
  SPS/PPS (H.265: VPS/SPS/PPS) from the codec packet in front of the next
  frame. Frames whose decryption/NAL parsing failed (UxPlay marks them with
  `data[0] = 1`) are dropped and logged, never forwarded.
* `onCodec` is called from `video_set_codec` before the first frame of a
  stream. H.265 is only sent by the iPhone if feature bit 42 is advertised
  (`Options::h265`, or automatically when the display height is > 1080; see
  "Picture quality").
* Audio: `audio_get_format` → `onFormat`: ct=8 → `AAC_ELD, 44100, 2, 480`
  (screen mirroring), ct=4 → `AAC_LC, 44100, 2, 1024`, ct=2 → `ALAC, 44100, 2,
  spf (352)`. ct=1 (PCM) is logged and not forwarded. `onPacket` gets one
  decrypted raw access unit (no ADTS). ALAC: the magic cookie is not
  delivered (UxPlay uses a fixed one; see `renderers/audio_renderer.c`).
* Timestamps (`ntpLocalNs`) are on `AirPlayServer::localTimeNs()` =
  wall-clock ns since the Unix epoch (CLOCK_REALTIME; a Windows time sync
  step moves both alike). `ntpLocalNs` = the sender's timestamp (video: the
  frame's capture/presentation time from the 128-byte mirror header; audio:
  RTP time via the last sync packet) mapped to local time through the
  session's NTP clock sync (raop_ntp: one request every 250 ms during the
  first 2 s, then every 3 s; RFC 5905-style filter). So
  `localTimeNs() - ntpLocalNs` at arrival = transport latency, and audio and
  video share one timeline (same session, same offset) → A/V sync: present
  both at `ntpLocalNs + fixedLatency`. Details (`toLocal` in
  airplay_server.cpp): one remote→local offset per session, latched from
  the NTP sync at the first packet and kept (no jitter from NTP updates);
  re-latched only if the NTP estimate moves > 20 ms (drift; log
  `[clock] NTP offset moved …`). If a packet arrives before the first NTP
  exchange, a provisional offset from the arrival time is used and replaced
  at the first synced packet (`[clock] … provisional offset corrected by X
  ms`, a one-time step of both streams). Audio packets without any sender
  timestamp yet (before the first RTP sync, non-ELD) get their arrival time.
  Offset is reset on video reset / connection loss / last connection closed.
  Before 2026-10-07 the NTP mapping never worked (see `raop_ntp.c` (2)
  above) and the timeline was arrival-latched only.
  Offline check: `pm_ntp_test` (core/tests) runs raop_ntp against a fake
  iPhone timing server on 127.0.0.1 (phone clock booted 1 h ago, +40 ppm):
  video and audio timestamps map to the true capture instant within 0.33 ms;
  without the fix every timestamp maps to 0 (test fails).
* Volume: `onVolume(dB)` raw AirPlay value (0 … -30, -144 = mute).
* Resets: `video_reset` (mirroring stopped, client takeover) → `onReset()`;
  takeover additionally `audio->onFlush()` (see "Several phones"). `conn_reset` (network loss) and the
  feedback watchdog (no heartbeat for `feedbackTimeoutSec`, default 15 s)
  relaunch the HTTP server on the same port from an internal supervisor
  thread (like uxplay's main loop) and call `onReset()` + `onFlush()`.
* Callbacks arrive on library threads (httpd, mirror TCP, audio UDP, NTP);
  sinks must be thread-safe and quick.
* Audio pause/play: during mirroring the iPhone TEARs DOWN the audio stream
  (type 96) when playback pauses and sends a new SETUP (→ `audio_get_format`
  → `onFormat`, same format) when it resumes. There is no FLUSH. The sink
  must therefore treat an `onFormat` with an unchanged format as "a new
  stream starts", not as a device change (see docs/audio.md).

## Session events and PIN (`setEvents`, `Options::requirePin`)

`Events` (set before `start()`, called on network threads):

| Event | When |
|---|---|
| `onClientConnecting(name, model)` | `report_client_request` = first RTSP SETUP (after pairing/FairPlay). `name` is the phone's own UTF-8 name ("Victor的iPhone"), `model` e.g. `iPhone15,2`. |
| `onClientDisconnected()` | open connections dropped to 0 (`conn_destroy`; also after `conn_reset`/watchdog relaunch and `stop()`, which close all connections), only if a client was announced or a PIN shown since the last one. Bare `GET /info` probes from other Apple devices don't produce it. |
| `onPin("1234")` | `requirePin`: the phone sent `POST /pair-pin-start` (UxPlay `display_pin`). |
| `onPin("")` | PIN no longer needed: SETUP arrived (pairing done), the connection that asked for the PIN closed (user cancelled on the phone, also while another phone mirrors) or all connections closed. |
| `onTakeover(oldName, newName)` | `NewReplacesOld`: a 2nd phone was admitted and replaced the running session (old connection already closed, sinks already reset). Followed by `onClientConnecting(newName, …)`; no `onClientDisconnected` for the old phone. |

`currentClientName()` (thread-safe): name of the phone holding the admitted
session; set at `onClientConnecting`, replaced on a takeover, cleared on its
disconnect or full TEARDOWN; `""` if none.

PIN works exactly like UxPlay `-pin -reg`:

1. `requirePin` → `dnssd_init(pin_pw = 1)`: TXT `pw=true` (both records) and
   `_raop` `sf=0x8c` (instead of `0x4`); features unchanged (`0x5A7FFEE6`,
   bit 27 "legacy pairing" is on in both modes here); `raop_set_plist("pin", 0)`.
2. A phone that never paired with this receiver identity (Ed25519 key =
   `keyFile`) sends `pair-pin-start` → the library draws a fresh random PIN
   (0001-9999, new on every attempt) → `onPin`. The phone shows a PIN
   prompt; `pair-setup-pin` (SRP6a, 3 steps) checks it; a wrong PIN answers
   470 and the phone retries (new PIN).
3. On the first SETUP the client's Ed25519 public key is appended to
   `<keyFile dir>/airplay_pin_clients.txt` (`pk,deviceid,name`). Next time the
   phone skips the PIN (pair-verify only) and `check_register` admits it if
   its key is listed. A phone not in the list is refused (UxPlay behaviour) →
   delete `airplay.key` (new identity: all phones re-pair) to recover. With an
   empty `keyFile` nothing is stored and returning phones are accepted.
4. PIN off (default): `display_pin`/`register_client`/`check_register` are
   not installed, TXT/plist are byte-identical to before.

UI: show the 4 digits big while `onPin` holds a non-empty PIN, hide on `""`,
`onClientConnecting` or `onClientDisconnected`. Toggling `requirePin` needs
`stop()` + `start()` (mDNS records are rebuilt).

Verified offline (pm_probe `--pin`, Python RTSP client): `GET /info`
200 with `features=0x5A7FFEE6`; qualifier `txtAirPlay` → `pw=true`
(`pw=false` without `--pin`), `txtRAOP` → `pw=true sf=0x8c` (`pw=false
sf=0x4`); `POST /pair-pin-start` → `onPin("3503")`, closing the connection →
`onPin("")`; without `--pin` no event. Pairing with a real phone not yet tested.

## Several phones (takeover; `Options::takeoverPolicy`)

Meeting-room use: phones take turns. Upstream UxPlay `-nohold` (`NOHOLD`,
`raop_init2(nohold=1)`) dropped the running session at the newcomer's
**first** RTSP request (`conn_request`) — often just `GET /info`, before any
pairing or PIN — so any Apple device probing the receiver, or a phone that
then cancelled/failed the PIN, ended the current mirroring; the kicked
connections were only marked `pending_remove` and closed on the next httpd
loop pass. Without nohold, a 2nd RAOP connection got 409 whenever any RAOP
connection existed (even an idle one). httpd allows 12 connections
(`MAX_CONNECTIONS`), so coexistence is not limited there.

Now (`raop.c` / `raop_handlers.h`, `// PM:`):

* The newcomer's connection coexists with the running session through
  `/info`, pair-setup/verify, PIN and FairPlay. The decision is taken at its
  first SETUP (where `report_client_request` admits it), i.e. only after it
  passed pairing and, if required, the PIN.
* `NewReplacesOld` (default): the old session's connection is closed
  **synchronously** inside that SETUP (`conn_destroy`: audio/mirror/NTP
  threads joined, ports closed; the old iPhone sees its RTSP and mirror TCP
  connections closed = "mirroring stopped"), then `video_reset(NOHOLD)` →
  `onReset()` + `onFlush()`, `onTakeover(old, new)`,
  `onClientConnecting(new)`; the SETUP continues on the same (fixed) ports.
  No relaunch, no port change.
* The same phone (same `deviceID`) reconnecting replaces its own stale
  session (`onReset`, no `onTakeover`).
* Ping-pong guard: the phone that was just replaced cannot take the
  receiver back within 3 s (409; protects against an automatic reconnect of
  the old phone). A deliberate re-selection afterwards works.
* `KeepCurrent`: while a session is active, a connection from another IP
  gets `409 Conflict` at its first request (no PIN is shown); a 2nd client
  from the same IP is refused at SETUP unless it is the same `deviceID`.
  A session ends at disconnect or full TEARDOWN (then the next phone is
  admitted even if the old RTSP connection lingers).
* PIN: a newcomer must pass the PIN (or be a registered returning phone)
  before it can take over; if the PIN is shown while another phone mirrors
  and the newcomer cancels → `onPin("")`, running session untouched.
* If the old phone vanished without closing (Wi-Fi off), `NewReplacesOld`
  replaces it at once; with `KeepCurrent` newcomers wait for the feedback
  watchdog (`feedbackTimeoutSec`, 15 s) to drop it.

Log lines: `new client connection from <ip> while "<A>" is mirroring: it
takes over once it has paired and sends SETUP`, `takeover: "<B>" (<id>)
replaces "<A>": closing the old session`, `[takeover] "<B>" replaced "<A>"`,
`refusing "<B>" (…): "<A>" is mirroring; takeover policy KeepCurrent`.

Verified offline (2026-10-07, `py -3 tools/pm_probe/takeover_check.py`,
simulated senders: pair-setup/pair-verify with real X25519/Ed25519, SETUP 1
+ mirror SETUP, unencrypted H.264 codec packet on the mirror port; FairPlay
skipped), all checks pass:

* replace: a `/info` probe and B's pairing do not disturb A; B's SETUP
  closes A's RTSP + mirror sockets, order `onReset` / `onFlush` →
  `onTakeover("PhoneA" -> "PhoneB")` → `onClientConnecting("PhoneB")`
  (`currentClientName()` = "PhoneB"), B's mirror SETUP on the freed ports
  and `onCodec(H264)` follow; no `onClientDisconnected` for A; A straight
  back → 409, after 3 s → takes over again; same phone reconnecting → no
  `onTakeover`; last close → `onClientDisconnected`, current "".
  Takeover SETUP round trip 16 ms (up to ~0.5 s when the old session's NTP
  thread is inside its 300 ms receive timeout — the fake phone never answers
  NTP; a real phone answers within milliseconds).
* keep: other host → 409 at `GET /info`; same host, other deviceID → 409 at
  SETUP; A untouched; after A's TEARDOWN B is admitted.
* pin (`--pin`): B without pairing → 470 at SETUP; B `pair-pin-start` →
  `onPin("NNNN")`, B closes → `onPin("")` while A mirrors; B paired →
  takeover.
* pinreg (`--pin --key`, empty register): unregistered pair-verify refused
  (also after a PIN-less `/pair-setup`, the former bypass) → SETUP 470.

Owner test with two phones (A, B; same Wi-Fi), PhoneMirror running:

1. A: Control Center → Screen Mirroring → 自在投影. Mirroring runs.
2. B: same. Expected within ~1-2 s: A shows mirroring stopped (its
   mirroring indicator disappears), the PC window shows B; log:
   `takeover: "<B>" … replaces "<A>"`, `[takeover] "<B>" replaced "<A>"`.
3. Wait > 3 s, A selects 自在投影 again: A is back, B stopped.
4. B opens the Screen Mirroring list but does not pick the receiver (or
   picks another one): A keeps mirroring (no `onReset` in the log).
5. PIN on (phones not paired yet, or delete `airplay_pin_clients.txt`):
   while A mirrors, B selects → PIN appears on the PC; B cancels → PIN
   disappears, A unaffected; B again and enters the PIN → takeover as in 2.
6. Audio: play a video with sound on A, take over with B: A's sound stops,
   B's sound plays; stop B → window idle, no restart needed.
7. Optional `KeepCurrent` (pm_probe `--keep-current`): B gets "unable to
   connect" while A mirrors; after A stops, B connects.

Send the log lines around `takeover:` if A does not stop, if A reconnects by
itself (would log `replaced less than 3 s ago`), or if B stays black.

## Latency diagnostics

`[audio-timing]` lines (always on, Info): SETUP (`audio_get_format`),
TEARDOWN, FLUSH, first datagram / first real audio packet (+ count of
"no data" packets the phone sends first) / first packet out of the jitter
buffer, each in ms after the audio stream start, and "first audio packet to
the sink X ms after SETUP". Together with the player's "stream start" line
(docs/audio.md) this shows where the time goes after play is pressed.

### Audio silence after pause → play (fixed 2026-10-07)

Owner log (build 622912a): pause/play of a video during mirroring sends **no**
new SETUP/FLUSH; the player logged `stream start: first packet 3481.9 ms
after the previous packet (gap), first PCM to device 1.3 ms` (also 3761.8,
4434.9 ms), so the time was lost before the sink. Cause: while nothing plays
the iPhone keeps the RTP sequence running with 16-byte "no data" packets
(`00 68 34 00`; 104 of them at stream start). `raop_rtp.c` drops those
before the jitter buffer, so after the pause the buffer sees a seqnum hole.
In resend mode (the phone sends a `controlPort`, as iOS does) upstream
`raop_buffer_dequeue` then returns nothing ("hope resend gets on time") until
256 entries are queued, and afterwards skips only ONE missing seqnum per
arriving packet, so the first audio after any pause shorter than 2.8 s comes
out after exactly 256 packets ≈ 2.8 s (resend requests for no-data seqnums
can't fill the hole). Pauses longer than 2.8 s overflow the buffer and flush,
which is why it looked erratic. UxPlay has the same code.

Fix: no-data seqnums (also resent ones) are marked in the buffer and stepped
over; a real hole is waited for at most 40 ms after the packet behind it
arrived. `pm_buffer_test` (core/tests, real-time paced) proves it:

| scenario | first post-gap packet out |
|---|---|
| upstream, 1 s pause (92 no-data) | 2775.5 ms |
| upstream, 0.4 s pause (37 no-data) | 2775.5 ms |
| upstream, 3 s pause (276 no-data) | 0 ms (overflow flush) |
| fixed, 0.4 s / 1 s / 3 s pause | 0 ms |
| fixed, seq jump of 92 with no packets / first packet lost | 43.5 ms (hole timeout) |

Gap trace in the log (Info): `[audio-timing] audio datagrams resume after X
ms of socket silence` (phone sent nothing), `audio data resumes after X ms
without audio: N datagrams in the gap (M "no data"); first seq/rtp; buffer
first/last/empty`, then `first packet after the gap leaves the jitter buffer
X ms after it arrived: holds, no-data skipped, hole timeouts, late discards,
duplicates, overflow flushes, overrun skips, resend requests`.

`[video-timing]` (Options::timingLog, default on): every 5 s while mirroring

```
[video-timing] 5.0 s: 300 frames (60.0 fps), avg 12.3 KB max 80.1 KB, interval avg 16.7 max 40.0 ms |
 lead = pts(NTP->local) - arrival: avg -45.0 min -60.2 max -30.1 ms | lead vs session timeline (ntpLocalNs): ... |
 recv payload avg 0.40 max 3.0 ms, decrypt avg 0.10 max 0.5 ms, sink onFrame avg 2.00 max 6.0 ms
```

* `lead` = the frame's sender timestamp converted to PC time via the NTP
  clock sync, minus the time its 128-byte header was read from the socket.
  `-lead` ≈ capture/encode → network → our socket latency. If it is around
  -500…-1000 ms, the delay is on the phone / Wi-Fi side; if it is small
  (tens of ms) but the picture still lags, look downstream (video window).
* `lead vs session timeline (ntpLocalNs)`: same with the session's latched
  offset (= the `ntpLocalNs` handed to the sinks); it drifting more negative
  over time means a queue is building (TCP backlog). `(NO NTP clock sync)` /
  `(some frames before NTP sync)` flag frames whose NTP mapping was missing.
* `recv payload` / `decrypt` / `sink onFrame`: our own per-frame cost; there
  is no queue in raop_rtp_mirror (frames go synchronously socket → decrypt
  → sink on the mirror thread), so a slow `onFrame` back-pressures TCP.

Receive path facts (checked): blocking `recv` on the mirror socket (UxPlay's
`SO_RCVTIMEO` timeval is read by Windows as a DWORD of 0 ms = no timeout, so
reads return as soon as data is there; no polling sleeps; `select` timeouts of
5 ms only matter while idle); nothing waits for NTP sync before delivering
video; SO_RCVBUF is logged at connect (Windows autotunes).

Experimental knobs (Options; pm_probe flags): `mirrorQuickAck` (`--quickack`,
immediate TCP ACKs on the mirror socket), `reportedAudioLatencyMs` (`--al MS`,
RECORD `Audio-Latency`, default 250 ms like UxPlay), `width/height`
(`--size WxH`), `maxFps` (`--maxfps N`).

## Picture quality (display size offered to the iPhone)

The iPhone scales its screen into the display box advertised in `GET /info`
→ `displays[0]` (`width`, `height`, `widthPixels`, `heightPixels`,
`refreshRate` = 1/Hz as a real, `maxFPS`; `raop_handlers.h`). The **height**
controls the stream size: with 1080 a portrait phone arrives as ~498x1080,
with 2160 as ~996x2160. Those plist keys are the only places UxPlay uses
`-s wxh@r` / `-fps`; the one interplay is the codec: iOS mirrors above 1080p
only in H.265 and needs feature bit 42 ("ScreenMultiCodec", UxPlay `-h265`)
for that — without it the phone sends a type-0x01 codec packet without
payload (`raop_rtp_mirror.c`: "non-h264 video but … bit 42 … not set") and
nothing plays. UxPlay's other "4K" items (`HEVC:2160` profile strings) are
HLS-only. Hence `start()` turns bit 42 on by itself when height > 1080
(Warning in the log). With bit 42 the phone may still send H.264 at ≤ 1080
(older devices / small box), so the video sink must handle both (`onCodec`).

Presets (`AirPlayServer::applyQualityPreset(opts, QualityPreset::…)`,
pm_probe `--preset std|high|max`):

| Preset | UI | width x height | refresh / maxFPS | H.265 (bit 42) |
|---|---|---|---|---|
| `Standard` | 標準 | 1920x1080 | 60 / 60 | off (H.264) |
| `High` | 高 | 2560x1440 | 60 / 60 | on |
| `Highest` | 最高 | 3840x2160 | 60 / 60 | on |

Notes: the phone decides the final size and frame rate (it may send 30 fps
at 4K, and mirrors at the phone's own aspect); higher presets need more
Wi-Fi bandwidth and a working HEVC decoder in the video window (Media
Foundation HEVC needs a GPU decoder or the "HEVC Video Extensions"). Changing
the preset needs `stop()` + `start()` (the phone reads `/info` per
connection; the TXT `features` change is re-announced at start). Validation:
width 320..7680, height 240..4320 (clamped, logged), one of them 0 → 16:9
from the other, refreshRate/maxFps 1..255 (0 → 60). Start log:
`display mode offered to the client: 2560x1440 @60 Hz, maxFPS 60, H.265 on`.

Verified with `tools/pm_probe/net_check.py --preset std|high|max` (GET /info
over RTSP, binary plist decoded):

| run | displays[0] | features |
|---|---|---|
| `--preset std` | 1920x1080, widthPixels 1920, heightPixels 1080, refreshRate 0.01667 (60 Hz), maxFPS 60 | 0x5A7FFEE6 (bit 42 = 0) |
| `--preset high` | 2560x1440 (pixels 2560x1440), 60 Hz, maxFPS 60 | 0x4005A7FFEE6 (bit 42 = 1) |
| `--preset max` | 3840x2160 (pixels 3840x2160), 60 Hz, maxFPS 60 | 0x4005A7FFEE6 (bit 42 = 1) |
| `--size 2560x1440` (no h265) | 2560x1440 + log "display height 1440 > 1080 requires H.265" | 0x4005A7FFEE6 |

Owner test procedure (video lag):
1. Run PhoneMirror (or `pm_probe`) and mirror; open a stopwatch app with
   milliseconds on the iPhone, hold the phone next to the PC window, take a
   photo of both (repeat 3-5 times). Lag = phone time − PC window time.
2. In `%LOCALAPPDATA%\PhoneMirror\phonemirror.log` read the `[video-timing]`
   lines from the same minute: compare `-lead avg` with the photographed lag.
   lag ≈ -lead → upstream (phone/Wi-Fi); lag ≫ -lead → after `onFrame`.
3. A/B one knob at a time (e.g. `pm_probe --quickack`, `--al 0`,
   `--size 1280x720`, `--maxfps 30`) and repeat 1-2.

## Network / Windows Firewall

Ports with the default `Options::legacyPorts = true` (UxPlay `-p`):

| Proto | Port | Use |
|---|---|---|
| UDP | 5353 | mDNS (multicast 224.0.0.251 / ff02::fb) |
| TCP | 7000 | RTSP/HTTP control (advertised port) |
| TCP | 7100 | mirroring video stream |
| UDP | 6000, 6001, 7011 | audio data, audio control, NTP timing |

Windows asks for firewall permission the first time `pm_probe.exe` listens; tick
**Private networks**. If that prompt was dismissed or the network profile is
Public, add rules yourself from an elevated PowerShell (not done by any tool in
this repo):

```powershell
# program-based (works with dynamic ports too); adjust path
New-NetFirewallRule -DisplayName "PhoneMirror pm_probe" -Direction Inbound -Action Allow `
  -Program "$PWD\build-core\bin\Release\pm_probe.exe" -Profile Private
# or port-based for the legacy fixed ports
New-NetFirewallRule -DisplayName "PhoneMirror TCP" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 7000,7100 -Profile Private
New-NetFirewallRule -DisplayName "PhoneMirror UDP" -Direction Inbound -Action Allow -Protocol UDP -LocalPort 5353,6000,6001,7011 -Profile Private
```

Check the network profile with `Get-NetConnectionProfile` (must be `Private`
for the rules above; or use `-Profile Any`). Remove with
`Remove-NetFirewallRule -DisplayName "PhoneMirror*"`.

Also: the iPhone and PC must be on the same L2 subnet (mDNS is link-local);
"client isolation"/"AP isolation" on Wi-Fi breaks discovery. Another mDNS
responder (Bonjour service, iTunes) can coexist because the socket uses
SO_REUSEADDR, but see "Known gaps".

### mDNS on several interfaces

`mdnsd_scan_interfaces()` (mdnsd.c) picks every IPv4 interface that is up,
multicast-capable, has a preferred address that is not 127.x / 169.254.x
(link-local = no DHCP), and is not a VPN/VM adapter: IfType PROP_VIRTUAL /
TUNNEL / PPP, or description/alias containing Hyper-V, vEthernet, WSL,
Virtual(Box), VMware/VMnet, Tailscale, WireGuard, Wintun, Surfshark, TAP-,
OpenVPN, ZeroTier, NordLynx, ProtonVPN, Npcap, Loopback, Bluetooth, Docker,
Teredo, isatap. Exception: "Wi-Fi Direct"/"Hosted Network" (Windows Mobile
Hotspot — a phone can join the PC's hotspot) counts as real. Order: real
before virtual, with gateway first, then route metric. Virtual adapters are
used only if no real interface is up (and then only with a gateway = the
upstream fallback). One UDP socket on *:5353 joins 224.0.0.251 on each
picked interface; announcements/goodbyes go out of every interface
(`IP_MULTICAST_IF`) with that interface's own A record; a query is answered
on the interface it came in on (`WSARecvMsg` + `IP_PKTINFO`) with that
interface's address, so Ethernet and Wi-Fi on different subnets both work.
Queries arriving on non-advertised interfaces (VPN) are ignored. The start
log lists them: `mDNS advertising on 1 interface(s): 乙太網路=192.168.1.125`.
IPv6 mDNS remains off on Windows (upstream stub).

### Network changes, sleep/resume (`refreshNetwork()`)

`NotifyIpInterfaceChange` + `NotifyUnicastIpAddressChange` (AF_INET) feed a
2 s debounce on an internal network thread. After it settles the interface
list is re-scanned; if it changed, or an interface/address was added or
removed (resume and Wi-Fi reconnects produce these), the mDNS socket is
re-opened on the new set and the services are announced, and again 1 s
later. Mere parameter notifications with an unchanged list are ignored (Debug
log). The RTSP/mirror listeners bind the wildcard address and survive
interface changes; the listener is relaunched only if `raop_is_running()`
says it stopped and no client is connected. An active session is never
dropped by this (a session whose interface really went away ends through
the existing feedback watchdog / `conn_reset`). The app calls
`refreshNetwork()` additionally on `WM_POWERBROADCAST`/`PBT_APMRESUMEAUTOMATIC`
(asynchronous, returns at once). Log lines (`[network]`):

```
[network] refreshNetwork(): interfaces unchanged: before [乙太網路=192.168.1.125, …], now […]
[network] mDNS re-opened and re-announced on 3 interface(s): 乙太網路=192.168.1.125, Tailscale=100.101.153.4, 乙太網路 4=192.168.56.1
[network] AirPlay listener on TCP port 65316 still up (wildcard bind); 0 open connection(s) kept
[network] mDNS announcement repeated
```

If start() happens with no network at all, the mDNS socket is opened
without interfaces (`no usable IPv4 interface … waiting for a network
change`) and the first network change brings the advertisement up.

Verified (2026-10-07, `py -3 tools/pm_probe/net_check.py`, pm_probe
`--dynamic-ports` next to the installed app): this PC has Ethernet
192.168.1.125 (real), VirtualBox host-only 192.168.56.1, Tailscale
100.101.153.4 (Wi-Fi disconnected). Default: advertised on Ethernet only;
a PTR query sent out of each interface is answered on Ethernet with
`A 192.168.1.125` and not on the VirtualBox/Tailscale ones. With
`--allow-virtual` (test hook env `PM_MDNS_ALLOW_VIRTUAL=1`, all three
advertised) each interface answers with its own A record (192.168.56.1 /
100.101.153.4 / 192.168.1.125). `--refresh` (pm_probe `--refresh-at 5`, or
press R in pm_probe) shows the `[network]` lines above and the same answers
afterwards. Automatic notifications could not be exercised without
toggling adapters (not allowed on this PC); they share the refresh path.

## pm_probe (iPhone test procedure)

```
cd build-core\bin\Release
pm_probe.exe "PhoneMirror"          # add --debug for library debug logs, --h265 to offer HEVC
                                    # --dynamic-ports, --key FILE, --seconds N (auto-stop), --pin
                                    # latency A/B: --quickack --al MS --size WxH --maxfps N
                                    # --preset std|high|max (picture quality)
                                    # --refresh-at S, or key R: refreshNetwork()
                                    # --keep-current (TakeoverPolicy::KeepCurrent); key C: currentClientName()
```

Offline checks: `py -3 tools/pm_probe/net_check.py [--preset …] [--size WxH]
[--allow-virtual] [--refresh]` (GET /info + per-interface mDNS query),
`py -3 tools/pm_probe/takeover_check.py [--only replace|keep|pin|pinreg]`
(multi-phone takeover; needs the `cryptography` package),
`pm_ntp_test.exe` (NTP clock mapping), `pm_buffer_test.exe` (audio jitter
buffer).

1. Wait for `READY`. iPhone (same Wi-Fi): Control Center → Screen Mirroring →
   "PhoneMirror".
2. Expected log sequence: `connection request from "<iPhone>"`, `conn_init`,
   pairing/fairplay info lines, `audio_get_format: ct=8 spf=480 …`,
   `[AUDIO] onFormat(AAC-ELD ct=8, 44100 Hz, 2 ch, 480 …)`,
   `[VIDEO] onCodec(H264)`, `video_report_size`, `[VIDEO] onFrame #1 … nal=[7,8,5]`
   (SPS, PPS, IDR), then frames with `nal=[1]`; `[AUDIO] onPacket …`.
3. Stop mirroring → `video_reset: RTP shutdown`, `[VIDEO] onReset()`.
4. Ctrl+C. Check: `ffplay -f h264 probe.h264` (or VLC) shows the screen.
   `probe_audio.bin` = sequence of `uint32 LE length` + raw AAC-ELD AU.

Interesting failure signatures: no device in the iPhone list → mDNS/firewall;
listed but connecting fails → TCP 7000/7100 blocked or pairing error in log;
`dropping undecryptable/invalid video frame` → FairPlay/key problem;
`no feedback heartbeat` → UDP/TCP traffic from phone blocked.

## Verification done without an iPhone (2026-10-07)

* `pm_probe` binds TCP 7000 (v4+v6) and UDP 5353; `netstat -ano` confirms.
* Raw mDNS PTR query on the LAN interface (Python script, multicast
  224.0.0.251) returns `PhoneMirrorTest._airplay._tcp.local` /
  `A036BC2AA0D5@PhoneMirrorTest._raop._tcp.local`, SRV port 7000 →
  `<hostname>.local`, A record, and full TXT (`features=0x5A7FFEE6,0x0`;
  `0x5A7FFEE6,0x400` with `--h265`), alongside other AirPlay devices on the LAN.
* RTSP `GET /info` → 200 + binary plist (1920x1080, maxFPS 60);
  `POST /pair-setup` → 200 + server Ed25519 pk (= TXT `pk`);
  `POST /fp-setup` phase 1 → 200 + 142-byte FairPlay reply.
* Feedback watchdog: an idle client connection is dropped after 16 s, the
  server relaunches on the same port and accepts new connections.
* `--seconds N` clean shutdown (mDNS goodbye, threads joined) exits 0.

## Known gaps / risks

* Not yet tested against a real iPhone (owner to do): FairPlay key
  exchange, mirror decryption, audio decryption and NTP are unexercised.
* mDNS: IPv4 on all real interfaces (see "mDNS on several interfaces"); an
  unusual VPN/VM adapter name not in the list would be advertised too
  (harmless: wrong-subnet phones never see it). IPv6 mDNS on Windows is not
  implemented upstream (`mdns_get_default_ipv6` stub).
* deviceid = MAC of the first up Ethernet/Wi-Fi adapter (`findMac`, as
  UxPlay) unless `Options::macAddress` is set; a TAP adapter that is up
  before the NIC could be picked and the deviceid would then change with
  the VPN state. Not changed here (it would re-identify existing installs);
  the app may pin `macAddress`.
* Any open TCP connection without heartbeat counts for the 15 s watchdog
  (same as UxPlay), so a stray LAN client can trigger a relaunch.
* HLS (YouTube "AirPlay video"), password access and client
  allow/deny lists of UxPlay are not exposed (code is compiled but disabled).
* Kernel RX timestamps: UxPlay's Windows path uses `WSARecvMsg` +
  `SIO_TIMESTAMPING`; MSVC/UCRT has it, so it is compiled in; if the adapter
  does not support it UxPlay logs a hint once and falls back to user-space
  receive times (fine: Wi-Fi jitter dominates; the NTP filter keeps the
  lowest-delay of 8 samples). Enabling "Software Timestamp = RxAll" is not
  required. NTP mapping with a real iPhone is not yet re-checked after the
  `client_time_received` fix: expect `[clock] timeline offset latched from
  the NTP clock sync` and a `[video-timing]` `lead` without "(NO NTP clock
  sync)".
