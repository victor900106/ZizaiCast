"""Checks a pm_recorder_test MP4 with ffprobe/ffmpeg (stdlib only).

Reports: container/stream info, stream durations and their difference, video
frame count + timestamp regularity (CFR), audio frequency (zero crossings),
the audio gap, the frame counter of the first segment (output frame n must show
input frame n), and the A/V offset at every flash+beep marker.

usage: python verify_recording.py out.mp4 [--flash 3,15] [--rotate-at 10]
          [--audio-gap 6:1] [--fps 60] [--no-audio]
"""
import argparse
import json
import math
import subprocess
import sys


def run(args, binary=False):
    r = subprocess.run(args, capture_output=True)
    if r.returncode != 0:
        sys.exit("command failed: %s\n%s" % (" ".join(args), r.stderr.decode(errors="replace")[-2000:]))
    return r.stdout if binary else r.stdout.decode(errors="replace")


def zc_freq(x, rate):
    cross = []
    for i in range(1, len(x)):
        a, b = x[i - 1], x[i]
        if a < 0 <= b:
            cross.append(i - 1 + (-a) / (b - a))
    if len(cross) < 3:
        return float("nan")
    return (len(cross) - 1) * rate / (cross[-1] - cross[0])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mp4")
    ap.add_argument("--flash", default="3,15")
    ap.add_argument("--rotate-at", type=float, default=10.0)
    ap.add_argument("--audio-gap", default="6:1")
    ap.add_argument("--fps", type=int, default=60)
    ap.add_argument("--no-audio", action="store_true")
    ap.add_argument("--tone-at", default="1.0:2.5,16:19", help="windows for the frequency measurement")
    a = ap.parse_args()
    flashes = [float(v) for v in a.flash.split(",") if v]
    gap_at, gap_len = (float(v) for v in a.audio_gap.split(":"))

    info = json.loads(run(["ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", a.mp4]))
    v = next(s for s in info["streams"] if s["codec_type"] == "video")
    au = next(s for s in info["streams"] if s["codec_type"] == "audio")
    print("format: %s, duration %s s, %.2f MB, %.2f Mbps" % (
        info["format"]["format_name"], info["format"]["duration"], int(info["format"]["size"]) / 1e6,
        int(info["format"]["bit_rate"]) / 1e6))
    print("video: %s %s %sx%s %s, r_frame_rate %s avg %s, nb_frames %s, start %s, duration %s" % (
        v["codec_name"], v.get("profile"), v["width"], v["height"], v.get("pix_fmt"), v["r_frame_rate"],
        v["avg_frame_rate"], v.get("nb_frames"), v.get("start_time"), v.get("duration")))
    print("audio: %s %s %s Hz %s ch, nb_frames %s, start %s, duration %s, %s bps" % (
        au["codec_name"], au.get("profile"), au["sample_rate"], au["channels"], au.get("nb_frames"),
        au.get("start_time"), au.get("duration"), au.get("bit_rate")))
    dv, da = float(v["duration"]), float(au["duration"])
    print("stream durations: video %.4f s, audio %.4f s, |diff| %.1f ms -> %s" % (
        dv, da, abs(dv - da) * 1000, "OK" if abs(dv - da) < 0.050 else "FAIL"))

    # Video packet timestamps (CFR check).
    pts = [float(l.split(",")[0]) for l in run([
        "ffprobe", "-v", "error", "-select_streams", "v", "-show_entries", "packet=pts_time", "-of", "csv=p=0",
        a.mp4]).split() if l.strip() and l.split(",")[0] != "N/A"]
    pts.sort()
    d = [pts[i + 1] - pts[i] for i in range(len(pts) - 1)]
    exp = 1.0 / a.fps
    irregular = sum(1 for x in d if abs(x - exp) > 0.0005)
    print("video packets: %d, first pts %.4f, last pts %.4f, frame interval %.5f..%.5f s (expected %.5f), "
          "irregular %d -> %s" % (len(pts), pts[0], pts[-1], min(d), max(d), exp, irregular,
                                  "CFR OK" if irregular == 0 else "NOT CFR"))

    # Per-frame centre luma (flash detection) and frame counter blocks.
    w, h = int(v["width"]), int(v["height"])
    cw, ch = w // 8, h // 8
    centre = run(["ffmpeg", "-v", "error", "-i", a.mp4, "-an", "-vf",
                  "crop=%d:%d:%d:%d,scale=1:1:flags=area,format=gray" % (cw, ch, (w - cw) // 2, (h - ch) // 2),
                  "-f", "rawvideo", "-"], binary=True)
    counter = run(["ffmpeg", "-v", "error", "-i", a.mp4, "-an", "-vf",
                   "crop=512:32:0:0,scale=16:1:flags=area,format=gray", "-f", "rawvideo", "-"], binary=True)
    nframes = len(centre)
    print("decoded frames: %d (%.3f s at %d fps)" % (nframes, nframes / a.fps, a.fps))
    seg = int(a.rotate_at * a.fps)
    bad = 0
    checked = 0
    for n in range(min(seg, len(counter) // 16)):
        bits = counter[n * 16:(n + 1) * 16]
        val = 0
        for b in bits:
            val = (val << 1) | (1 if b > 125 else 0)
        checked += 1
        if val != n:
            bad += 1
            if bad <= 5:
                print("  frame %d shows input frame %d" % (n, val))
    print("frame counter (first segment, %d frames): %d mismatches" % (checked, bad))

    vflash = []
    for f in flashes:
        lo = int((f - 0.3) * a.fps)
        hi = int((f + 0.3) * a.fps)
        on = [n for n in range(max(lo, 0), min(hi, nframes)) if centre[n] > 210]
        vflash.append(on[0] / a.fps if on else None)
        print("flash @%.3f s: video frames %s" % (f, on))

    if a.no_audio:
        raw = run(["ffmpeg", "-v", "error", "-i", a.mp4, "-vn", "-ac", "1", "-f", "s16le", "-"], binary=True)
        n = len(raw) // 2
        peak = max(abs(int.from_bytes(raw[i:i + 2], "little", signed=True)) for i in range(0, len(raw), 2))
        print("audio (silent track expected): %d samples, peak %d -> %s" % (n, peak, "OK" if peak < 10 else "FAIL"))
        return

    rate = int(au["sample_rate"])
    raw = run(["ffmpeg", "-v", "error", "-i", a.mp4, "-vn", "-ac", "1", "-f", "s16le", "-"], binary=True)
    import array
    x = array.array("h")
    x.frombytes(raw)
    print("decoded audio: %d samples = %.4f s" % (len(x), len(x) / rate))
    for win in a.tone_at.split(","):
        lo, hi = (float(v) for v in win.split(":"))
        seg = x[int(lo * rate):int(hi * rate)]
        if len(seg) > rate // 2:
            print("tone frequency (%g-%g s): %.3f Hz" % (lo, hi, zc_freq([s / 32768 for s in seg], rate)))
    # Gap: longest run of near silence between 3.5 s and the end.
    best = (0, 0)
    run_start = None
    for i in range(int(3.5 * rate), len(x)):
        if abs(x[i]) < 300:
            if run_start is None:
                run_start = i
        else:
            if run_start is not None and i - run_start > best[1] - best[0]:
                best = (run_start, i)
            run_start = None
    # Silence inside a 440 Hz sine is at most a few samples, so runs >5 ms are gaps.
    print("audio gap: silence %.4f .. %.4f s (%.1f ms), expected %.3f .. %.3f s" % (
        best[0] / rate, best[1] / rate, (best[1] - best[0]) * 1000 / rate, gap_at, gap_at + gap_len))
    for f, vt in zip(flashes, vflash):
        lo, hi = int((f - 0.3) * rate), int((f + 0.3) * rate)
        on = next((i for i in range(lo, min(hi, len(x))) if abs(x[i]) > 0.5 * 32768), None)
        at = on / rate if on is not None else None
        if at is None or vt is None:
            print("marker @%.3f: video %s audio %s -> not found" % (f, vt, at))
            continue
        # The beep's first sample above threshold is up to 1/4 period after onset (440 Hz).
        print("marker @%.3f s: flash at %.4f s, beep at %.4f s -> A/V offset (audio - video) %+.1f ms" % (
            f, vt, at, (at - vt) * 1000))


if __name__ == "__main__":
    main()
