"""Checks a pm_audio_test --wav capture (stdlib only, no numpy needed).

Reports: duration, leading/trailing silence, interior gaps (runs of digital
silence > 2 ms inside the programme), 440 Hz tone frequency by zero-crossing
interpolation, tone level, L/R equality, and sweep frequency at a few points.

usage: python verify_wav.py out.wav [--tone 3] [--sweep 3] [--gain-db 0]
"""
import argparse
import math
import struct
import sys
import wave


def zc_freq(x, rate):
    """Frequency from linearly interpolated rising zero crossings."""
    cross = []
    for i in range(1, len(x)):
        a, b = x[i - 1], x[i]
        if a < 0 <= b:
            cross.append(i - 1 + (-a) / (b - a))
    if len(cross) < 3:
        return float("nan")
    return (len(cross) - 1) * rate / (cross[-1] - cross[0])


def rms(x):
    return math.sqrt(sum(v * v for v in x) / max(1, len(x)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--tone", type=float, default=3.0)
    ap.add_argument("--sweep", type=float, default=3.0)
    ap.add_argument("--gain-db", type=float, default=0.0)
    a = ap.parse_args()

    w = wave.open(a.wav, "rb")
    rate, ch, n = w.getframerate(), w.getnchannels(), w.getnframes()
    raw = w.readframes(n)
    s = struct.unpack("<%dh" % (n * ch), raw)
    left = s[0::ch]
    right = s[1::ch] if ch > 1 else left
    print(f"file: {rate} Hz, {ch} ch, {n} frames = {n / rate:.3f} s")

    thr = 8  # |sample| <= thr counts as silence
    first = next(i for i, v in enumerate(left) if abs(v) > thr)
    last = len(left) - next(i for i, v in enumerate(reversed(left)) if abs(v) > thr)
    prog = last - first
    exp = (a.tone + a.sweep) * rate
    print(f"leading silence {first / rate * 1000:.1f} ms, programme {prog / rate:.4f} s "
          f"(expected {exp / rate:.4f} s, diff {(prog - exp) / rate * 1000:+.1f} ms), "
          f"trailing {(len(left) - last) / rate * 1000:.1f} ms")

    # Interior gaps: runs of silence longer than 2 ms. A sine never stays
    # within +-thr for more than a couple of samples at these levels.
    gaps, run = [], 0
    for i in range(first, last):
        if abs(left[i]) <= thr:
            run += 1
        else:
            if run > rate * 0.002:
                gaps.append((i - run, run))
            run = 0
    print(f"interior gaps > 2 ms: {len(gaps)}" + (f" first: {gaps[:5]}" if gaps else ""))

    lr = max(abs(l - r) for l, r in zip(left, right))
    print(f"max |L-R| = {lr}")

    t0 = first + int(0.1 * rate)
    t1 = first + int((a.tone - 0.1) * rate)
    tone = [v / 32768.0 for v in left[t0:t1]]
    f = zc_freq(tone, rate)
    lvl = 20 * math.log10(rms(tone) * math.sqrt(2))
    print(f"tone: {f:.3f} Hz (expected 440, err {f - 440:+.3f} Hz), peak-equivalent level "
          f"{lvl:.2f} dBFS (expected {-6.02 + a.gain_db:.2f})")

    k = math.log(10000 / 100)
    for frac in (0.2, 0.5, 0.8):
        c = first + int((a.tone + frac * a.sweep) * rate)
        seg = [v / 32768.0 for v in left[c - 1000:c + 1000]]
        exp_f = 100 * math.exp(frac * k)
        print(f"sweep @ {frac:.1f}: {zc_freq(seg, rate):8.1f} Hz (expected ~{exp_f:.1f})")

    ok = not gaps and abs(f - 440) < 0.5 and abs(prog - exp) < 0.05 * rate
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
