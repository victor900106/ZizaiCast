# Audio front end (`audio/`, target `pm_audio`)

Implements `pm::AudioSink` (include/pm/media.h) as `pm::AudioPlayer`
(audio/include/pm/audio_player.h): decodes AirPlay audio and plays it through
WASAPI.

## Pipeline

```
network thread                         render thread (MMCSS "Pro Audio")
onPacket(AU) -> decoder (lavc AAC/ALAC) WASAPI shared, event driven, float32
            -> S16 PCM -> jitter ring  ->  pull + volume ramp -> device
```

* **Decoding** (inline in `onPacket`, ~tens of µs per frame, guarded by a
  decoder mutex that only `onFormat` contends):
  * AAC uses FFmpeg's native `aac` decoder (libavcodec, **LGPL-only** build,
    no gpl/nonfree features; fdk-aac's license is GPL-incompatible and is no
    longer linked into anything shipped). Opened with the ASC below as
    extradata, one raw access unit per `AVPacket`, float-planar output
    converted to interleaved S16 (rounded, clipped). One frame per AU, first
    AU included: same output length and codec delay as fdk-aac (see
    `--bench`). A failed `send_packet`/`receive_frame` counts as one decode
    error, as before. On the first AAC `onFormat` the player logs once
    `AAC decoder: FFmpeg libavcodec 63.1.102 (9.0.2), avcodec license: LGPL
    version 2.1 or later, avutil license: LGPL version 2.1 or later` (a
    warning is appended if either is not LGPL). libav logging is set to
    `AV_LOG_ERROR`.
  * ct=8 AAC-ELD (mirroring; 44.1 kHz/2 ch/480 spf): raw AUs,
    AudioSpecificConfig `f8 e8 50 00` (AOT 39, frameLengthFlag=1, no LD-SBR).
  * ct=4 AAC-LC: ASC `12 10`. An ADTS header, if present, is stripped.
  * ct=2 ALAC: Apple's open-source decoder (vcpkg port `alac`, Apache-2.0),
    magic cookie `00000160 00 10 28 0a 0e 02 00ff 00000000 00000000 0000ac44`
    (same as UxPlay). Lives in `src/alac_decoder.cpp`. Compiled only if the port is installed
    (`PM_HAVE_ALAC`); otherwise `onFormat(ALAC)` logs an error and packets are
    ignored.
  * The ASC/cookie are generated from (codec, rate, channels, spf), see
    `audio/src/audio_decoder.cpp`.
* **Jitter buffer**: interleaved S16 ring, capacity `maxBufferMs` (120 ms).
  Overflow drops the *oldest* PCM (counted in `droppedFrames`). Playback starts
  once `prefillMs` (30 ms) is queued; if it runs dry while playing, an underrun
  is counted, silence is output and it re-primes. `onFlush` empties it.
  `ntpLocalNs` is currently ignored (play ASAP; latency bounded by the ring).
* **Output**: WASAPI shared mode, `EVENTCALLBACK | AUTOCONVERTPCM |
  SRC_DEFAULT_QUALITY`, float32 at the *stream* rate (the engine resamples
  44.1k -> mix rate), requested buffer 20 ms. Total latency ≈ 30 ms prefill +
  device buffer ≈ 50 ms.
* **Volume**: `onVolume(dB)`: -144 (or < -30) → 0, else `10^(dB/20)` with dB
  clamped to [-30, 0]; applied in software with a per-block linear ramp.
* **Device changes**: an `IMMNotificationClient` wakes the render thread on a
  default render device change and the stream is reopened on the new device;
  stream errors (e.g. `AUDCLNT_E_DEVICE_INVALIDATED`) also reopen (retry every
  ~0.5 s). `onFormat` reopens the device only if sample rate or channel
  count change; otherwise (the iPhone re-SETUPs the audio stream on every
  play after a pause) it swaps in a fresh decoder, empties the ring and the
  WASAPI stream keeps running with silence.
* **Logging / start timing**: `AudioPlayerConfig::log` receives the
  diagnostic lines (default stderr + OutputDebugString). On every stream
  (re)start — first packet after `onFormat`/`onFlush` or after >200 ms
  without packets — it logs `stream start: first packet X ms after
  onFormat, first PCM to device Y ms after first packet`; WASAPI opens log
  `open took X ms`. The figures are also in `AudioStats::startGapMs /
  startDelayMs / starts`. The app should route `log` into its log file.
* **Test/tap mode**: `AudioPlayerConfig::pcmTap` replaces WASAPI with a
  real-time paced pull clock driving the same ring, handing out rendered S16
  PCM (used by `pm_audio_test --wav`).
* **PCM monitor** (`setPcmMonitor`, for the recorder, docs/recorder.md): every
  decoded block is *also* handed to the monitor, on the `onPacket` thread,
  right after it is pushed into the ring — playback is unchanged. Raw decoder
  output (S16 interleaved, stream rate/channels, **before** the AirPlay
  volume, so recordings do not depend on the volume slider). `whenNs` = the
  packet's `ntpLocalNs` if non-zero and within 30 s of now on the steady clock
  or the UTC wall clock (passed through on the core's clock), else the
  arrival time (steady_clock ns). Guarded by the decoder mutex: once
  `setPcmMonitor(x)` returns the previous monitor is no longer running; the
  callback must not call back into the player. Unset = one null check.

## API

```cpp
pm::AudioPlayer player;                 // or AudioPlayer(AudioPlayerConfig{...})
player.start();                         // spawns render thread
sink = &player;                         // give to core as pm::AudioSink*
pm::AudioStats s = player.stats();      // bufferMs, deviceBufferMs, packets,
                                        // decodeErrors, framesDecoded,
                                        // underruns, droppedFrames, deviceReopens
player.setPcmMonitor([&](const int16_t* pcm, size_t frames, int ch, int rate, uint64_t whenNs) {
    recorder.onPcm(pcm, frames, ch, rate, whenNs); });   // nullptr removes
player.stop();
```

## Build

```
cmake -S audio/standalone -B build-audio -G "Visual Studio 18 2026" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=%USERPROFILE%/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build-audio --config Release
```

(`audio/CMakeLists.txt` is also picked up by the root CMakeLists; existing
build dirs need a reconfigure.) Requires, triplet x64-windows:

```
vcpkg install "ffmpeg[core,avcodec]:x64-windows" alac:x64-windows --overlay-ports=tools/vcpkg-overlay
```

`ffmpeg[core,avcodec]` = libavcodec + libavutil only (0.7.8+: the overlay port
`tools/vcpkg-overlay/ffmpeg` builds just the native `aac` decoder + parser with
`--disable-all ... --enable-small`, avcodec-63.dll ~0.5 MB instead of ~13 MB), configured without
`--enable-gpl`/`--enable-nonfree` (do not add the `gpl`, `nonfree`, `fdk-aac`,
`x264`... features). Runtime DLLs to ship next to the exe: **`avcodec-63.dll`,
`avutil-61.dll`** (imports: only Windows system DLLs + VCRUNTIME140);
`fdk-aac.dll` is no longer needed. LGPL-2.1 obligations: ship the FFmpeg
license text + a source offer/link for FFmpeg 9.0.2; the DLLs are dynamically
linked and replaceable.

`pm_audio_test` (never shipped) still uses fdk-aac's *encoder* to synthesize
the AAC-ELD/LC test vectors and fdk's decoder as the `--bench` reference:
CMake option `PM_TEST_FDK` (default ON, affects only `pm_audio_test`; needs
the `fdk-aac` port, silently off if not installed). With `-DPM_TEST_FDK=OFF`
only `--codec alac` works. `pm_audio` itself never links fdk-aac.

## Test

```
build-audio\bin\Release\pm_audio_test.exe                 # play on default device
build-audio\bin\Release\pm_audio_test.exe --wav out.wav   # capture instead
python audio\tools\verify_wav.py out.wav
```

`pm_audio_test` (with `PM_TEST_FDK`) encodes 3 s of 440 Hz + a 3 s 100 Hz→10 kHz log sweep at
-6 dBFS with the fdk-aac **encoder** configured like AirPlay mirroring
(AAC-ELD, 44.1 kHz stereo, 480 spf, raw AUs; it checks the encoder's ASC
equals the receiver's `f8e85000`), and feeds the AUs to `AudioPlayer` at
real-time pace. Options: `--codec lc`, `--volume dB`, `--jitter MS` (random
per-packet send delay), `--tone S`, `--sweep S`. `--codec alac` uses Apple's
ALAC *encoder* (352 spf), checks the cookie against the receiver's and that a
direct decode is bit-exact. `--monitor` additionally installs a PCM monitor
(odd packets carry a steady-clock `ntpLocalNs` + 100 ms that must come back
unchanged, even packets 0 → arrival time within 5 ms) and checks it saw every
decoded frame: `--monitor --wav` → 552 calls, 264960 frames = decoder output,
0 bad stamps, WAV still 440.000 Hz / 0 gaps / -6.02 dBFS; same with WASAPI.
`--bench [N]` (no playback) decodes the AUs N times with the receiver's
decoder and with fdk-aac's and compares CPU cost, length, alignment and
difference.

Measured (2026-10-07, default config):

| run | result |
|---|---|
| ELD `--wav` | ASC f8e85000 MATCH; 440.000 Hz, -6.02 dBFS, 0 gaps, programme 6.008 s of 6.000 s (codec delay), L==R |
| LC `--wav` | ASC 1210 MATCH; 440.000 Hz, 0 gaps |
| ALAC `--wav` | cookie MATCH, bit-exact, first byte 0x20, 0 gaps, programme 6.0000 s |
| `--volume -12` | tone level -18.02 dBFS (expected -18.02) |
| ELD live (WASAPI) | device buffer 23.5 ms, ring ~25-35 ms, 0 underruns, 0 drops (6 s and 35 s runs, also with `--jitter 20`) |
| onPacket cost | typically 150-400 µs worst case per call (decode + push) |

Re-verified 2026-10-07 after the switch to libavcodec (all device-less:
`--wav`/`--tap`, except the two marked WASAPI runs):

| run | result |
|---|---|
| ELD `--wav` | 440.000 Hz, -6.02 dBFS, 0 gaps, programme 6.0077 s (same as fdk), L==R, decErr 0 |
| LC `--wav` | 440.000 Hz, -6.02 dBFS, 0 gaps, decErr 0 |
| ALAC `--wav` | unchanged: bit-exact, programme 6.0000 s |
| `--volume -12 --wav` | -18.02 dBFS |
| `--monitor --wav` | 552 calls, 264960 frames = decoder output, 0 bad stamps, PASS (WASAPI run: same, 0 underruns) |
| `--bench 30` ELD | lavc 9.2 µs/AU vs fdk 17.4 µs/AU (0.08 % vs 0.16 % of real time); 264960 frames both; best lag 0, max diff 3 LSB (85 dB below signal) |
| `--bench 30 --codec lc` | lavc 7.3 µs/AU vs fdk 22.6-26.5 µs/AU; same length; lag 0 vs fdk with its limiter off, max diff 3 LSB (87 dB) |
| `--flush-test --tap` (6 cycles, first packet → first PCM) | setup 41.1 / flush 43.7 / gap 43.3 ms; fdk build same mode 42.3 / 44.2 / 42.9 ms |

AAC-LC timing note: fdk-aac's decoder enables its PCM limiter by default for
AAC-LC (not for ELD), which delayed LC output by 661 samples (15.0 ms).
libavcodec has no limiter, so ct=4 audio now comes out 15 ms earlier; ELD
(mirroring) timing is identical. (`--tap` resume figures are higher than the
WASAPI ones because the tap pull clock stands in for the device.)

(The single underrun shown in `final` stats is the end of the stream.)

### Pause → play resume latency (`--flush-test`)

```
pm_audio_test --flush-test [--resume setup|flush|gap] [--cycles N] [--play S] [--pause S]
              [--setup-gap MS] [--volume dB] [--tap]
```

Streams a 1 kHz AAC-ELD tone for `--play` s, stops for `--pause` s, resumes;
`setup` (default) calls `onFormat(same format)` at resume like the core does
on the iPhone's re-SETUP, `flush` calls `onFlush` at pause, `gap` neither.
Per cycle it prints the player's "first packet → first PCM handed to the
device" and, via WASAPI loopback capture of the default device (Goertzel
detector at 1 kHz, threshold above the noise of other audio measured during
the pauses), when the tone actually appears in the device mix.

Measured 2026-10-07 on the owner's PC (default device 48 kHz 8 ch), 6 cycles,
`--volume -12`:

| build | resume | first packet → first PCM | first packet → tone in mix | device opens |
|---|---|---|---|---|
| before (onFormat always reopened) | setup | 29.7 ms | 47.9 ms | 7 (WASAPI open 8-28 ms each) |
| after | setup | 28.8 ms | 48.1 ms | 1 |
| after | flush | 28.4 ms | 41.7 ms | 1 |
| after | gap | 29.1 ms | 46.5 ms | 1 |

So the player resumes in ≈ 30 ms prefill + 20 ms device buffer; the reported
~1 s is not spent in `AudioPlayer` (the reopen was real but only ~10 ms here,
and is gone now). The `[audio-timing]` core lines (docs/core.md) show the
network side: SETUP → first real audio packet from the phone. The multi-second
silence after pause → play was in the core jitter buffer (seqnum hole left by
the phone's "no data" packets, ~2.8 s stall), fixed in `raop_buffer.c`; see
docs/core.md "Audio silence after pause → play".

## Known gaps

* No A/V sync: `ntpLocalNs` is not used for scheduling.
* No packet-loss concealment (lost AUs just shorten the buffer).
* Only one decoder instance; ALAC uses fixed AirPlay cookie parameters
  (no `fmtp` parsing; core only passes codec/rate/channels/spf).
* WASAPI path verified on one machine/device only; device hot-swap was not
  exercised physically (code path: IMMNotificationClient -> reopen).
* AAC is decoded by libavcodec's fixed set of features; HE-AAC/LD-SBR
  streams (not used by AirPlay) would also decode, but are untested.
