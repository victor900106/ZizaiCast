"""Idle CPU / wake-up measurement of pm_probe (no iPhone needed). GPL-3.0-or-later.

  py -3 tools/pm_probe/idle_check.py --exe PATH [--seconds 60] [--only noclient|session]

Starts `pm_probe <random name> --dynamic-ports` (loopback clients only, no
sound, no window) and measures over --seconds, after a 5 s settle:
  CPU      user + kernel CPU time of the process (psutil), in ms and in % of
           one core
  wakeups  context switches of all its threads (psutil num_ctx_switches: the
           sum of the per-thread ContextSwitches counters on Windows), total
           and per second
Scenarios:
  noclient  listening, advertised, nobody connected (the app's normal state)
  session   one simulated phone (takeover_check.Phone): pair-verify, SETUP 1,
            mirror SETUP + codec packet, audio SETUP (type 96, AAC-ELD) and a
            /feedback heartbeat every 2 s like iOS, but no audio/video data
            (a phone whose screen and sound are idle)
Prints one line per scenario; exit code 0.
"""
import argparse
import os
import plistlib
import socket
import sys
import time

import psutil

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import takeover_check as tc  # noqa: E402


def sample(pid):
    p = psutil.Process(pid)
    t = p.cpu_times()
    return t.user + t.system, sum(p.num_ctx_switches())


def measure(probe, seconds, tick=None):
    c0, w0 = sample(probe.p.pid)
    t0 = time.time()
    while time.time() - t0 < seconds:
        time.sleep(0.5)
        if tick:
            tick()
    c1, w1 = sample(probe.p.pid)
    el = time.time() - t0
    return (c1 - c0) * 1000.0, (w1 - w0), el


def report(name, cpu_ms, wakes, el, threads):
    print('%-9s %5.1f s: CPU %7.1f ms (%.3f %% of one core), wake-ups %6d (%.1f/s), threads %d' %
          (name, el, cpu_ms, cpu_ms / (el * 10.0), wakes, wakes / el, threads))


def audio_setup(ph):
    ctrl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ctrl.bind(('127.0.0.1', 0))
    pl = {'streams': [{'type': 96, 'ct': 8, 'spf': 480, 'audioFormat': 0x1000000, 'usingScreen': True,
                       'isMedia': True, 'controlPort': ctrl.getsockname()[1]}]}
    st, _ = ph.req('SETUP', 'rtsp://127.0.0.1/1', plistlib.dumps(pl, fmt=plistlib.FMT_BINARY),
                   'application/x-apple-binary-plist')
    return st, ctrl


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=tc.DEFAULT_EXE)
    ap.add_argument('--seconds', type=float, default=60)
    ap.add_argument('--only', choices=['noclient', 'session'])
    a = ap.parse_args()
    print('exe: %s' % os.path.abspath(a.exe))
    if a.only in (None, 'noclient'):
        pr = tc.Probe(a.exe, [], seconds=int(a.seconds) + 30)
        try:
            time.sleep(5)
            cpu, wk, el = measure(pr, a.seconds)
            report('noclient', cpu, wk, el, psutil.Process(pr.p.pid).num_threads())
        finally:
            pr.stop()
    if a.only in (None, 'session'):
        pr = tc.Probe(a.exe, [], seconds=int(a.seconds) + 30)
        ph = None
        ctrl = None
        try:
            ph = tc.Phone(pr.port, 'IdlePhone', 'AA:BB:CC:00:00:01')
            ok = ph.pair_verify() and ph.setup1() == 200 and ph.setup_mirror() == 200
            st, ctrl = audio_setup(ph)
            ok = ok and st == 200
            if not ok:
                print('session setup failed')
                print('\n'.join(pr.lines[-40:]))
                return 1
            last = [time.time()]

            def heartbeat():
                if time.time() - last[0] >= 2.0:
                    last[0] = time.time()
                    ph.feedback()
            for _ in range(10):  # settle 5 s with heartbeats
                time.sleep(0.5)
                heartbeat()
            cpu, wk, el = measure(pr, a.seconds, heartbeat)
            report('session', cpu, wk, el, psutil.Process(pr.p.pid).num_threads())
        finally:
            if ph:
                ph.close()
            if ctrl:
                ctrl.close()
            pr.stop()
    return 0


if __name__ == '__main__':
    sys.exit(main())
