# Recorder (`recorder/`, target `pm_recorder`)

`pm::Recorder` (recorder/include/pm/recorder.h) writes the mirrored picture and
the decoded audio to an MP4: H.264 (Media Foundation, hardware encoder when
available) + AAC-LC stereo, via the Media Foundation Sink Writer
(`MFCreateSinkWriterFromURL`, `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS`).

## API / wiring

```cpp
pm::Recorder rec;
rec.log = [](const std::string& line) { /* app log */ };
if (rec.start(L"C:\\...\\錄影.mp4", 60)) {
    window.setFrameTap([&](const uint8_t* nv12, int w, int h, int stride, uint64_t pts) {
        rec.onVideoFrame(nv12, w, h, stride, pts); });
    player.setPcmMonitor([&](const int16_t* pcm, size_t frames, int ch, int rate, uint64_t when) {
        rec.onPcm(pcm, frames, ch, rate, when); });
}
...
window.setFrameTap(nullptr); player.setPcmMonitor(nullptr);
rec.stop();                                // finalizes; returns when the file is closed
pm::Recorder::Stats s = rec.stats();       // seconds, videoFrames, audioFrames, droppedVideo, bytes
```

| call | notes |
|---|---|
| `start(path, fps)` | Spawns the recorder thread, which creates the file (sink writer) and reports back: false = file cannot be created or a recording is still running (call `stop()` first, also after a failure). Takes a few ms. fps clamped to 1..240. |
| `stop()` | Drains the queues (pending pictures/PCM are written), pads both tracks to a common end, `Finalize()`s, closes. Safe twice / without start. ~50-150 ms. If nothing ever arrived the empty file is deleted. |
| `active()` | true between a successful `start` and `stop`, false after a write error (logged). |
| `stats()` | any thread; `seconds` = recorded media time, `videoFrames` = frames in the file (incl. duplicates), `audioFrames` = PCM frames in the file (incl. silence), `droppedVideo` = pictures dropped because the encoder was behind, `bytes` = current file size. |
| `onVideoFrame` | NV12 8-bit, Y plane then interleaved UV, both `stride` bytes per row (VideoWindow::setFrameTap layout). One copy into a pooled buffer + queue push; never waits on the encoder. 1440p ≈ 1-2 ms (the copy). |
| `onPcm` | interleaved S16, any channel count (first two used; mono duplicated), any rate. Copy + push, ~0.1 ms. |
| `log` | set before `start`; called on the recorder thread. Lines: encoder chosen (name, hardware/software, bitrate), canvas, source size changes, failures, final summary with counts and CPU. |

## Design

**Threading.** Callers only copy into bounded queues. Video: 6 pictures (while
the encoder is being created, up to 256 MB of pictures, see below); when full
the new picture is dropped and counted (`droppedVideo`). Audio: 4 s, oldest
dropped (gap → silence by timestamp). Everything else — scaling, sink writer,
file I/O — is on the recorder thread (COM MTA, `MFStartup`).

**Timestamps / clocks.** `ptsNs` / `whenNs` may be on `steady_clock` (QPC) ns or
the UTC wall clock (Unix-epoch ns, today's `ntpLocalNs`); each stamp is mapped
to the closer "now" at enqueue time and converted to steady ns. 0 or > 30 s
away on both clocks → arrival time. t0 = the first sample of either stream; the
first picture also fills [t0, its own time) if audio started earlier.

**Video: constant frame rate.** Output is CFR at `fps` (MP4 `r_frame_rate` =
`avg_frame_rate`, every frame interval exactly 1/fps). Each picture goes to
slot `round((pts - t0) * fps)`; a picture is held until the next one arrives
and written for every slot up to that one's slot (duplicates while the phone
sends nothing, e.g. a static screen). Two pictures in one slot: newest wins.
Duplicates reuse the same media buffer (no copy; the encoder sees identical
input, so they cost almost no bits).

**Audio: placed by timestamp.** Converted to stereo S16 at the output rate
(44.1 kHz, or 48 kHz if the first audio is 48 kHz; others → linear resampler).
Each block's expected position is `(whenNs - t0) * rate`; if the written audio
is behind by > 40 ms, silence fills the gap; if ahead by > 40 ms, the overlap is
trimmed. Small jitter (< 40 ms) is ignored, so continuous audio is written
sample-exact.

**Live fill.** Every ~50 ms both tracks are extended to `now - lag` (lag = 1 s,
or the largest stamp lateness seen + 0.5 s): video with the held picture, audio
with silence. So recording without audio produces a silent track, a paused
phone (no pictures, no audio) produces still picture + silence in real time,
and the sink writer never has to buffer one stream while the other starves.
At `stop()` both are padded to the common end (last picture lasts one frame;
audio padded to the same frame boundary).

**Rotation / size changes: fixed canvas.** The canvas = the first picture's
size (rounded down to even). Pictures of another size (phone rotated) are
bilinear-scaled to fit, aspect kept, centred, black bars (two passes per row,
striped over up to 4 threads). One file, one size, every player handles it.
Trade-off: after a rotation the picture is smaller (portrait 1170x2532 canvas →
landscape content at 1170x540). Alternative considered: a new `_2.mp4`
segment per size; rejected because the app API is one path → one file.
No picture before audio-only for 5 s (AirPlay audio without mirroring) or stop →
1280x720 black canvas.

**Encoder.** H.264 High, progressive, BT.709 limited range, bitrate =
w·h·fps·0.07 clamped to 3..50 Mbps (2560x1440@60 ≈ 15.5 Mbps, 1170x2532@60 ≈
12.4, 1080p60 ≈ 8.7, 4K60 ≈ 34.8), peak-constrained VBR (peak 2x), GOP 2 s,
passed through `SetInputMediaType`'s encoder parameters (retried with encoder
defaults if rejected). AAC-LC 192 kbps. Order tried: hardware MFT at the canvas
size → Microsoft software encoder → software at ≤ 4096x2304 / 2304x4096 (the
software encoder's limit; canvas scaled down). The chosen encoder is logged.

**Encoder start-up.** Creating the hardware encoder takes ~0.5 s (NVIDIA MFT
on the dev PC: 520-590 ms) and can only start once the first picture gives the
size. Meanwhile pictures queue up to 256 MB (1440p: 45 pictures ≈ 0.75 s; iPhone
1290x2796: 46), then the backlog is encoded at full speed. Above ~2.5K the
budget is shorter than the start-up (4K: ~21 pictures → the first ~0.35 s are
a still picture).

**Memory.** Media buffers are pooled (≤ 40, recycled when the encoder released
them); steady state at 1440p ≈ 10-15 buffers × 5.5 MB.

**Crash safety.** Fragmented MP4 (`MFTranscodeContainerType_FMPEG4`): the
moov comes first, then a moof + mdat about every 0.3 s. After a crash, a kill
or a power cut the file plays up to the last fragment (Windows Media Player /
Photos / ffmpeg; the length shows as unknown and the player may not seek).
`Finalize()` writes the duration and an mfra index, so a finished file plays
and seeks like a plain MP4 (checked with `--readback`: duration, seek, every
frame decoded; `verify_recording.py` unchanged). If this Windows has no
fragmented sink the recorder falls back to a plain MP4 (log: "fragmented MP4
not available"); a plain MP4 cut short has no moov and does not play.
Crash test: start `pm_recorder_test --seconds 30`, kill it after ~8 s, then
`pm_recorder_test --readback t.mp4`.

Known quirk: Media Foundation on this Windows build (10.0.26300) ignores the
moov duration of fragmented MP4s written by an app with a Windows 10
manifest (the sink stamps its OS version in a `uuid` box) and reports the
start of the last fragment instead, so Media Player / Photos may show a
length up to ~0.3 s short. Every frame still decodes and plays (`--readback`
of the app's files), and ffprobe / other players read the right duration.

## Build / test

```
cmake -S recorder/standalone -B build-rec -G "Visual Studio 18 2026" -A x64
cmake --build build-rec --config Release
build-rec\bin\Release\pm_recorder_test.exe --out t.mp4            # 20 s 2560x1440@60
python recorder\tools\verify_recording.py t.mp4
build-rec\bin\Release\pm_recorder_test.exe --readback t.mp4       # decode with Windows' MF decoders
```

(`recorder/CMakeLists.txt` is also in the root module list.) Needs no vcpkg
ports (mfplat, mfreadwrite, mfuuid). The verifier needs ffmpeg/ffprobe on PATH.

`pm_recorder_test` feeds the recorder in real time like the app: synthetic
NV12 pictures (moving 64 px stripes, UV gradient, frame index as 16 binary
blocks top-left) with steady-clock stamps, and a 440 Hz tone (amplitude 0.25)
in 480-frame packets with **UTC** stamps. Defaults: rotation at 10 s
(2560x1440 → 1440x2560), audio gap 6.0-7.0 s, video stall 13.0-13.5 s,
flash+beep markers at 3 s and 15 s (picture white and tone 0.9 for 50 ms).
Options: `--seconds --size WxH --fps --rotate-at --audio-gap S:LEN
--video-stall S:LEN --flash S,S --rate HZ --no-audio --audio-start S`.
It prints call costs, process CPU and the GPU VideoEncode/3D engine load (PDH).
`verify_recording.py` checks with ffprobe/ffmpeg: stream durations, CFR
(packet pts intervals), decoded frame count, frame counter (output frame n
shows input frame n over the first segment), tone frequency (zero crossings),
the gap, and the A/V offset at each marker (first white frame vs first beep
sample).

### Measured (2026-10-07, i7-13700K (24 threads), RTX 3060 Ti → NVIDIA H.264 Encoder MFT)

| run | result |
|---|---|
| 20 s 2560x1440@60, rotation, gap, stall | 1200 frames, CFR 1/60 exact, video 20.000 s / audio 20.016 s (Δ 15.6 ms = AAC frame padding), 0 dropped, 0 frame-counter mismatches, 30 duplicates (= the 0.5 s stall), 440.000 Hz, gap 1.001 s at 6.008 s, A/V offset +0.2 ms at both markers (the beep's 0.2 ms rise to threshold), 13.1 MB (5.25 Mbps on the synthetic content) |
| same, cost | recorder ≈ 37 % of one core (1.6 % of the machine; recorder thread alone 2 s CPU / 20 s, mostly the post-rotation scaling), GPU VideoEncode engine avg 29 % / max 47 %, max `onVideoFrame` 2.0 ms, max `onPcm` 0.15 ms, `stop()` 65 ms, encoder ready 0.52-0.57 s after the first picture |
| 10 s 1170x2532@60 portrait start → 2532x1170, audio 48 kHz starting at 1.5 s | canvas 1170x2532, landscape at 1170x540; Δ duration 7.8 ms, 0 mismatches, tone 440.000 Hz after resampling to 44.1 kHz, gap 4.000-4.500 s exact, A/V +0.2 ms |
| 8 s 1440p, no audio | silent AAC track (peak 0), Δ 10.9 ms, 0 dropped |
| 10 s 3840x2160@60 | CFR, A/V +0.2 ms; 24 pictures dropped during encoder start-up (see above), none after; ~66 % of one core, VideoEncode avg 54 % |
| no data at all | file deleted, `stop()` 2 ms |
| `--readback` (Windows MF demux + decoders) | all four files decode without errors, frame counts/timestamps as above |
