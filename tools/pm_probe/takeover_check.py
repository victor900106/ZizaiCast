"""Multi-phone takeover check for pm_probe (no iPhone needed). GPL-3.0-or-later.

  py -3 tools/pm_probe/takeover_check.py [--exe PATH] [--only replace|keep|pin|pinreg]

Simulated AirPlay senders speak RTSP to a fresh `pm_probe --dynamic-ports`:
pair-verify (X25519/Ed25519, as an iPhone that already paired; needs the
`cryptography` package), SETUP 1 (deviceID/name/model/timingPort, ekey/eiv),
SETUP 2 (mirror stream type 110) and an unencrypted H.264 codec packet on the
mirror TCP port (-> VideoSink::onCodec). FairPlay is skipped (the core then
logs a decrypt warning only). Scenarios:

  replace  NewReplacesOld (default policy): GET /info from a 2nd client does not
           disturb A; B's SETUP closes A (RTSP + mirror sockets), sinks get
           onReset/onFlush, onTakeover(A->B), onClientConnecting(B), B streams;
           A coming straight back is refused for 3 s (ping-pong guard), later
           takes over again; the same phone reconnecting replaces its own
           session without onTakeover; currentClientName() follows.
  keep     --keep-current: a 2nd host gets 409 at its first request, a 2nd
           client from the same host 409 at SETUP; after A's TEARDOWN B is
           admitted.
  pin      --pin: B without pairing -> 470 at SETUP (A untouched); B asking for
           a PIN and cancelling -> onPin(code) then onPin("") while A keeps
           mirroring; B after pair-verify -> takes over.
  pinreg   --pin --key <temp>: an unregistered client's pair-verify is refused
           and its SETUP gets 470.
Exit code 0 = all checks passed.
"""
import argparse
import hashlib
import os
import plistlib
import random
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519, x25519
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_EXE = os.path.join(HERE, '..', '..', 'build-core', 'bin', 'Release', 'pm_probe.exe')
RAW = dict(encoding=serialization.Encoding.Raw, format=serialization.PublicFormat.Raw)

failures = []
PRINT_LOG = False


def check(cond, what):
    print('  [%s] %s' % ('ok' if cond else 'FAIL', what))
    if not cond:
        failures.append(what)
    return cond


class Probe:
    def __init__(self, exe, extra, seconds=60):
        self.name = 'PMTakeover%04d' % random.randint(0, 9999)
        self.cwd = tempfile.mkdtemp(prefix='pm_takeover_')
        args = [os.path.abspath(exe), self.name, '--dynamic-ports', '--seconds', str(seconds)] + extra
        self.p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                  cwd=self.cwd, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        self.lines = []
        ready = threading.Event()

        def pump():
            for raw in self.p.stdout:
                line = raw.decode('utf-8', 'replace').rstrip()
                self.lines.append(line)
                if 'READY' in line:
                    ready.set()
        threading.Thread(target=pump, daemon=True).start()
        if not ready.wait(15):
            print('\n'.join(self.lines))
            sys.exit('pm_probe did not get READY')
        self.port = int(re.search(r'TCP port (\d+)', [l for l in self.lines if 'READY' in l][0]).group(1))

    def mark(self):
        return len(self.lines)

    def since(self, m):
        return self.lines[m:]

    def wait_for(self, pattern, m=0, timeout=3.0):
        end = time.time() + timeout
        while time.time() < end:
            for i in range(m, len(self.lines)):
                if re.search(pattern, self.lines[i]):
                    return i
            time.sleep(0.05)
        return -1

    def stop(self):
        if PRINT_LOG:
            print('\n'.join('     | ' + l for l in self.lines))
        try:
            self.p.send_signal(subprocess.signal.CTRL_BREAK_EVENT)
        except Exception:
            pass
        try:
            self.p.wait(10)
        except subprocess.TimeoutExpired:
            self.p.kill()
        shutil.rmtree(self.cwd, ignore_errors=True)


class Phone:
    """One RTSP connection of a simulated AirPlay sender."""

    def __init__(self, port, name, devid, src='127.0.0.1'):
        self.name, self.devid, self.cseq = name, devid, 0
        self.s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.s.bind((src, 0))
        self.s.settimeout(5)
        self.s.connect(('127.0.0.1', port))
        self.mirror = None
        self.ntp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)  # timing port we announce (NTP requests land here)
        self.ntp.bind(('127.0.0.1', 0))

    def req(self, method, url, body=b'', ctype=None, extra=None):
        self.cseq += 1
        h = ['%s %s RTSP/1.0' % (method, url), 'CSeq: %d' % self.cseq, 'User-Agent: AirPlay/770.8.1',
             'X-Apple-Device-ID: 0x%s' % self.devid.replace(':', '').lower()]
        if ctype:
            h.append('Content-Type: ' + ctype)
        h.append('Content-Length: %d' % len(body))
        h += extra or []
        self.s.sendall(('\r\n'.join(h) + '\r\n\r\n').encode() + body)
        buf = b''
        while b'\r\n\r\n' not in buf:
            d = self.s.recv(4096)
            if not d:
                return None, b''  # connection closed by the receiver
            buf += d
        head, rest = buf.split(b'\r\n\r\n', 1)
        status = int(head.split(b' ', 2)[1])
        m = re.search(rb'Content-Length:\s*(\d+)', head, re.I)
        n = int(m.group(1)) if m else 0
        while len(rest) < n:
            d = self.s.recv(4096)
            if not d:
                break
            rest += d
        return status, rest

    def info(self):
        return self.req('GET', '/info')[0]

    def pair_verify(self, pair_setup=True):
        """pair-setup (PIN-less, as an iPhone does without PIN) + pair-verify; pair_setup=False:
        pair-verify only (a returning client in PIN mode)."""
        ed = ed25519.Ed25519PrivateKey.generate()
        x = x25519.X25519PrivateKey.generate()
        xpub, edpub = x.public_key().public_bytes(**RAW), ed.public_key().public_bytes(**RAW)
        if pair_setup:
            st, body = self.req('POST', '/pair-setup', edpub, 'application/octet-stream')
            if st != 200 or len(body) != 32:
                return False
        st, body = self.req('POST', '/pair-verify', b'\x01\0\0\0' + xpub + edpub, 'application/octet-stream')
        if st != 200 or len(body) != 96:
            return False
        server_x = body[:32]
        secret = x.exchange(x25519.X25519PublicKey.from_public_bytes(server_x))
        key = hashlib.sha512(b'Pair-Verify-AES-Key' + secret).digest()[:16]
        iv = hashlib.sha512(b'Pair-Verify-AES-IV' + secret).digest()[:16]
        ctr = Cipher(algorithms.AES(key), modes.CTR(iv)).encryptor()
        ctr.update(b'\0' * 64)  # the server's signature used the first 64 key-stream bytes
        sig = ctr.update(ed.sign(xpub + server_x))
        st, _ = self.req('POST', '/pair-verify', b'\0\0\0\0' + sig, 'application/octet-stream')
        return st == 200

    def setup1(self):
        pl = {'deviceID': self.devid, 'name': self.name, 'model': 'iPhone15,2', 'ekey': os.urandom(72),
              'eiv': os.urandom(16), 'timingProtocol': 'NTP', 'timingPort': self.ntp.getsockname()[1],
              'sessionUUID': 'TEST', 'sourceVersion': '770.8.1'}
        return self.req('SETUP', 'rtsp://127.0.0.1/1', plistlib.dumps(pl, fmt=plistlib.FMT_BINARY),
                        'application/x-apple-binary-plist')[0]

    def setup_mirror(self):
        pl = {'streams': [{'type': 110, 'streamConnectionID': random.getrandbits(63)}]}
        st, body = self.req('SETUP', 'rtsp://127.0.0.1/1', plistlib.dumps(pl, fmt=plistlib.FMT_BINARY),
                            'application/x-apple-binary-plist')
        if st != 200:
            return st
        port = plistlib.loads(body)['streams'][0]['dataPort']
        self.mirror = socket.create_connection(('127.0.0.1', port), 5)
        # unencrypted codec packet (type 1): avcC with a tiny SPS/PPS -> onCodec(H264)
        sps, pps = bytes.fromhex('6764001facd94050'), bytes.fromhex('68ebe3cb')
        payload = bytes([1, 0x64, 0, 0x1f, 0xff, 0xe1]) + struct.pack('>H', len(sps)) + sps + b'\x01' + \
            struct.pack('>H', len(pps)) + pps
        hdr = bytearray(128)
        struct.pack_into('<I', hdr, 0, len(payload))
        hdr[4], hdr[6] = 1, 0x16
        for off, v in ((16, 1170.0), (20, 2532.0), (40, 1170.0), (44, 2532.0), (56, 498.0), (60, 1080.0)):
            struct.pack_into('<f', hdr, off, v)
        self.mirror.sendall(bytes(hdr) + payload)
        return st

    def feedback(self):
        try:
            return self.req('POST', '/feedback')[0]
        except OSError:
            return None

    def teardown(self):
        return self.req('TEARDOWN', 'rtsp://127.0.0.1/1')[0]

    def closed(self, sock=None, timeout=2.0):
        """True if the receiver closed this RTSP (or the mirror) connection."""
        sock = sock or self.s
        sock.settimeout(timeout)
        try:
            return sock.recv(1) == b''
        except ConnectionResetError:
            return True
        except socket.timeout:
            return False

    def close(self):
        for x in (self.mirror, self.s, self.ntp):
            if x:
                x.close()


def idx(probe, pattern, m):
    return probe.wait_for(pattern, m, 3.0)


def scenario_replace(exe):
    print('== replace (NewReplacesOld)')
    pr = Probe(exe, [])
    try:
        A = Phone(pr.port, 'PhoneA', 'AA:AA:AA:AA:AA:01')
        check(A.pair_verify(), 'A pair-verify')
        check(A.setup1() == 200 and A.setup_mirror() == 200, 'A SETUP + mirror stream')
        check(idx(pr, r'onCodec\(H264\)', 0) >= 0, 'A: sink got onCodec(H264)')
        check(idx(pr, r'onClientConnecting\("PhoneA".*current="PhoneA"', 0) >= 0,
              'onClientConnecting(PhoneA), currentClientName()="PhoneA"')

        m = pr.mark()
        probe = Phone(pr.port, 'Probe', 'AA:AA:AA:AA:AA:99')
        check(probe.info() == 200, 'GET /info from another client: 200')
        probe.close()
        check(not A.closed(timeout=0.5) and A.feedback() == 200, 'A still connected after the /info probe')
        check(not any('onReset' in l for l in pr.since(m)), 'no onReset from the probe')

        B = Phone(pr.port, 'PhoneB', 'BB:BB:BB:BB:BB:02')
        check(B.pair_verify(), 'B pair-verify (while A mirrors)')
        check(not A.closed(timeout=0.3), 'A still connected while B pairs')
        m = pr.mark()
        t0 = time.time()
        check(B.setup1() == 200, 'B SETUP: 200')
        dt = (time.time() - t0) * 1000
        check(A.closed(), 'A RTSP connection closed by the receiver')
        check(A.closed(A.mirror), 'A mirror TCP connection closed')
        r, f, t, c = (idx(pr, p, m) for p in (r'onReset\(\)', r'onFlush\(\)', r'onTakeover\("PhoneA" -> "PhoneB"\)',
                                              r'onClientConnecting\("PhoneB".*current="PhoneB"'))
        check(0 <= r < t < c and f >= 0, 'order: onReset/onFlush -> onTakeover(PhoneA->PhoneB) -> '
              'onClientConnecting(PhoneB) (current="PhoneB")')
        check(B.setup_mirror() == 200, 'B mirror stream SETUP: 200 (ports free)')
        check(idx(pr, r'onCodec\(H264\)', c) > c, 'B: sink got onCodec(H264) after the reset')
        check(not any('onClientDisconnected' in l for l in pr.since(m)), 'no onClientDisconnected for A')
        print('     takeover SETUP round trip %.1f ms' % dt)
        A.close()

        m = pr.mark()
        A2 = Phone(pr.port, 'PhoneA', 'AA:AA:AA:AA:AA:01')
        check(A2.pair_verify() and A2.setup1() == 409, 'A straight back (< 3 s): 409 (ping-pong guard)')
        check(not B.closed(timeout=0.3) and B.feedback() == 200, 'B unaffected')
        A2.close()
        time.sleep(3.2)
        A3 = Phone(pr.port, 'PhoneA', 'AA:AA:AA:AA:AA:01')
        check(A3.pair_verify() and A3.setup1() == 200, 'A again after 3 s: 200')
        check(B.closed(), 'B closed')
        check(idx(pr, r'onTakeover\("PhoneB" -> "PhoneA"\)', m) >= 0, 'onTakeover(PhoneB->PhoneA)')
        check(A3.setup_mirror() == 200, 'A mirror stream again')
        B.close()

        m = pr.mark()
        A4 = Phone(pr.port, 'PhoneA', 'AA:AA:AA:AA:AA:01')
        check(A4.pair_verify() and A4.setup1() == 200, 'same phone reconnects: 200')
        check(A3.closed(), 'its stale connection closed')
        check(idx(pr, r'onReset\(\)', m) >= 0 and not any('onTakeover' in l for l in pr.since(m)),
              'onReset, but no onTakeover for the same phone')
        A3.close()
        m = pr.mark()
        A4.close()
        check(idx(pr, r'onClientDisconnected\(\) current=""', m) >= 0, 'last close: onClientDisconnected, current=""')
    finally:
        pr.stop()


def scenario_keep(exe):
    print('== keep (KeepCurrent)')
    pr = Probe(exe, ['--keep-current'])
    try:
        A = Phone(pr.port, 'PhoneA', 'AA:AA:AA:AA:AA:01')
        check(A.pair_verify() and A.setup1() == 200 and A.setup_mirror() == 200, 'A mirrors')
        m = pr.mark()
        B = Phone(pr.port, 'PhoneB', 'BB:BB:BB:BB:BB:02', src='127.0.0.2')
        check(B.info() == 409, 'B (other host) GET /info: 409 at the first request')
        B.close()
        B2 = Phone(pr.port, 'PhoneB', 'BB:BB:BB:BB:BB:02')
        check(B2.info() == 200 and B2.pair_verify() and B2.setup1() == 409, 'B (same host as A): 409 at SETUP')
        B2.close()
        check(not A.closed(timeout=0.3) and A.feedback() == 200, 'A unaffected')
        check(not any(k in l for l in pr.since(m) for k in ('onReset', 'onTakeover')), 'no reset/takeover')
        check(A.teardown() == 200, 'A full TEARDOWN (keeps the RTSP connection open)')
        B3 = Phone(pr.port, 'PhoneB', 'BB:BB:BB:BB:BB:02', src='127.0.0.2')
        check(B3.info() == 200 and B3.pair_verify() and B3.setup1() == 200, 'after A\'s TEARDOWN B is admitted')
        check(not any('onTakeover' in l for l in pr.since(m)), 'no onTakeover (A had ended)')
        A.close()
        B3.close()
    finally:
        pr.stop()


def scenario_pin(exe):
    print('== pin (--pin, no register file)')
    pr = Probe(exe, ['--pin'])
    try:
        A = Phone(pr.port, 'PhoneA', 'AA:AA:AA:AA:AA:01')
        check(A.pair_verify(pair_setup=False) and A.setup1() == 200 and A.setup_mirror() == 200,
              'A (returning client: pair-verify only) mirrors')
        m = pr.mark()
        B = Phone(pr.port, 'PhoneB', 'BB:BB:BB:BB:BB:02')
        check(B.setup1() == 470, 'B without pairing: SETUP 470')
        B.close()
        check(not A.closed(timeout=0.3) and A.feedback() == 200, 'A unaffected')
        B2 = Phone(pr.port, 'PhoneB', 'BB:BB:BB:BB:BB:02')
        check(B2.req('POST', '/pair-pin-start')[0] == 200, 'B pair-pin-start')
        check(idx(pr, r'onPin\("\d{4}"\) -> show PIN', m) >= 0, 'onPin(code) shown')
        m2 = pr.mark()
        B2.close()  # user cancels the PIN prompt
        check(idx(pr, r'onPin\(""\) -> hide PIN', m2) >= 0, 'B cancelled: onPin("") although A is still connected')
        check(not A.closed(timeout=0.3) and A.feedback() == 200, 'A unaffected')
        check(not any(k in l for l in pr.since(m) for k in ('onReset', 'onTakeover')), 'no reset/takeover so far')
        B3 = Phone(pr.port, 'PhoneB', 'BB:BB:BB:BB:BB:02')
        check(B3.pair_verify() and B3.setup1() == 200, 'B paired: SETUP 200')
        check(A.closed() and idx(pr, r'onTakeover\("PhoneA" -> "PhoneB"\)', m) >= 0, 'takeover A->B')
        A.close()
        B3.close()
    finally:
        pr.stop()


def scenario_pinreg(exe):
    print('== pinreg (--pin --key: register active, empty)')
    d = tempfile.mkdtemp(prefix='pm_takeover_key_')
    pr = Probe(exe, ['--pin', '--key', os.path.join(d, 'airplay.key')])
    try:
        B = Phone(pr.port, 'PhoneB', 'BB:BB:BB:BB:BB:02')
        check(not B.pair_verify(pair_setup=False), 'unregistered returning client: pair-verify refused')
        check(B.setup1() == 470, 'its SETUP: 470')
        B.close()
        B2 = Phone(pr.port, 'PhoneB', 'BB:BB:BB:BB:BB:02')
        check(not B2.pair_verify(pair_setup=True), 'PIN-less pair-setup + pair-verify (PIN bypass): refused')
        check(B2.setup1() == 470, 'its SETUP: 470')
        B2.close()
    finally:
        pr.stop()
        shutil.rmtree(d, ignore_errors=True)


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=DEFAULT_EXE)
    ap.add_argument('--only', choices=['replace', 'keep', 'pin', 'pinreg'])
    ap.add_argument('--log', action='store_true', help='print the pm_probe log of each scenario')
    a = ap.parse_args()
    global PRINT_LOG
    PRINT_LOG = a.log
    for n, f in (('replace', scenario_replace), ('keep', scenario_keep), ('pin', scenario_pin),
                 ('pinreg', scenario_pinreg)):
        if not a.only or a.only == n:
            f(a.exe)
    print('== %s' % ('ALL PASSED' if not failures else '%d FAILED: %s' % (len(failures), '; '.join(failures))))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
