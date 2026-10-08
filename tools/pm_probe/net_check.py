"""Offline network checks for pm_probe (no iPhone needed). GPL-3.0-or-later.

  py -3 tools/pm_probe/net_check.py [--exe PATH] [--preset std|high|max] [--size WxH]
                                    [--allow-virtual] [--refresh]

Starts pm_probe --dynamic-ports under a unique name, then
  * GET /info over RTSP -> prints the "displays" plist entry (width/height/
    refreshRate/maxFPS) and the feature bits,
  * sends a raw mDNS PTR query for _airplay._tcp.local out of EVERY local IPv4
    interface (IP_MULTICAST_IF) and prints, per interface, whether our
    instance answered and with which A record,
  * --refresh: presses refreshNetwork() (pm_probe --refresh-at) and prints the
    re-announcement lines from the probe log, plus a second round of queries.
--allow-virtual sets PM_MDNS_ALLOW_VIRTUAL=1 (VPN/VM adapters advertised too)
so the per-interface A record can be checked on a machine with one real NIC.
"""
import argparse
import os
import plistlib
import random
import re
import socket
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_EXE = os.path.join(HERE, '..', '..', 'build-core', 'bin', 'Release', 'pm_probe.exe')


def local_ipv4():
    """(alias, ip) of every IPv4 address (via PowerShell Get-NetIPAddress)."""
    out = subprocess.run(['powershell', '-NoProfile', '-Command',
                          '[Console]::OutputEncoding=[Text.Encoding]::UTF8; Get-NetIPAddress -AddressFamily IPv4 | '
                          'Where-Object { $_.AddressState -eq "Preferred" } | '
                          'ForEach-Object { $_.InterfaceAlias + "|" + $_.IPAddress }'],
                         capture_output=True, text=True, encoding='utf-8').stdout
    res = []
    for line in out.splitlines():
        if '|' in line:
            alias, ip = line.rsplit('|', 1)
            if not ip.startswith('127.'):
                res.append((alias.strip(), ip.strip()))
    return res


def qname(name):
    return b''.join(bytes([len(p)]) + p.encode() for p in name.split('.')) + b'\0'


def read_name(pkt, off):
    labels, jumped, end = [], False, off
    while True:
        n = pkt[off]
        if n == 0:
            off += 1
            break
        if n & 0xC0 == 0xC0:
            ptr = ((n & 0x3F) << 8) | pkt[off + 1]
            if not jumped:
                end = off + 2
            jumped = True
            off = ptr
            continue
        labels.append(pkt[off + 1:off + 1 + n].decode('utf-8', 'replace'))
        off += 1 + n
    return '.'.join(labels), (end if jumped else off)


def parse(pkt):
    qd, an, ns, ar = struct.unpack('>4H', pkt[4:12])
    off = 12
    for _ in range(qd):
        _, off = read_name(pkt, off)
        off += 4
    rrs = []
    for _ in range(an + ns + ar):
        name, off = read_name(pkt, off)
        typ, cls, ttl, rdlen = struct.unpack('>HHIH', pkt[off:off + 10])
        off += 10
        rd = pkt[off:off + rdlen]
        if typ == 1:
            val = socket.inet_ntoa(rd)
        elif typ == 12:
            val = read_name(pkt, off)[0]
        elif typ == 33:
            val = '%s:%d' % (read_name(pkt, off + 6)[0], struct.unpack('>H', rd[4:6])[0])
        else:
            val = rd
        rrs.append((name, typ, ttl, val))
        off += rdlen
    return rrs


def mdns_query(ifaddr, instance, timeout=1.5):
    """PTR query out of interface ifaddr; returns (A records, SRV) of our instance."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.bind((ifaddr, 0))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(ifaddr))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
    q = struct.pack('>6H', random.randint(0, 65535), 0, 1, 0, 0, 0) + qname('_airplay._tcp.local') + struct.pack('>HH', 12, 1)
    s.sendto(q, ('224.0.0.251', 5353))
    s.settimeout(0.2)
    a_recs, srv, seen = set(), None, False
    end = time.time() + timeout
    while time.time() < end:
        try:
            pkt, _ = s.recvfrom(9000)
        except socket.timeout:
            continue
        except OSError:
            break
        try:
            rrs = parse(pkt)
        except Exception:
            continue
        if not any(instance in str(r[3]) or instance in r[0] for r in rrs):
            continue
        seen = True
        for name, typ, ttl, val in rrs:
            if typ == 33 and instance in name:
                srv = val
        host = srv.split(':')[0] if srv else None
        for name, typ, ttl, val in rrs:
            if typ == 1 and (host is None or name == host):
                a_recs.add(val)
    s.close()
    return seen, sorted(a_recs), srv


def get_info(port):
    s = socket.create_connection(('127.0.0.1', port), timeout=3)
    s.sendall(b'GET /info RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: AirPlay/690.7.1\r\n\r\n')
    data = b''
    while b'\r\n\r\n' not in data:
        data += s.recv(4096)
    head, body = data.split(b'\r\n\r\n', 1)
    m = re.search(rb'Content-Length:\s*(\d+)', head, re.I)
    n = int(m.group(1)) if m else 0
    while len(body) < n:
        body += s.recv(65536)
    s.close()
    return head.split(b'\r\n')[0].decode(), plistlib.loads(body[:n])


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=DEFAULT_EXE)
    ap.add_argument('--preset')
    ap.add_argument('--size')
    ap.add_argument('--allow-virtual', action='store_true')
    ap.add_argument('--refresh', action='store_true')
    a = ap.parse_args()

    name = 'PMNetCheck%04d' % random.randint(0, 9999)
    args = [os.path.abspath(a.exe), name, '--dynamic-ports', '--seconds', '14' if a.refresh else '7']
    if a.preset:
        args += ['--preset', a.preset]
    if a.size:
        args += ['--size', a.size]
    if a.refresh:
        args += ['--refresh-at', '5']
    env = dict(os.environ)
    if a.allow_virtual:
        env['PM_MDNS_ALLOW_VIRTUAL'] = '1'
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                         cwd=os.path.dirname(os.path.abspath(a.exe)), creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    lines = []
    ready = threading.Event()

    def pump():
        for raw in p.stdout:
            line = raw.decode('utf-8', 'replace').rstrip()
            lines.append(line)
            if 'READY' in line:
                ready.set()
    threading.Thread(target=pump, daemon=True).start()
    if not ready.wait(10):
        print('\n'.join(lines))
        sys.exit('pm_probe did not get READY')
    port = int(re.search(r'TCP port (\d+)', [l for l in lines if 'READY' in l][0]).group(1))
    time.sleep(0.5)
    print('== pm_probe "%s" on TCP %d' % (name, port))
    for l in lines:
        if any(k in l for k in ('display mode', 'mDNS', 'H.265', 'interface')):
            print('   log:', l.split('] ', 1)[-1] if '] ' in l else l)

    status, info = get_info(port)
    d = info['displays'][0]
    print('== GET /info -> %s' % status)
    print('   displays[0]: width=%d height=%d widthPixels=%d heightPixels=%d refreshRate=%s (=%.1f Hz) maxFPS=%d'
          % (d['width'], d['height'], d['widthPixels'], d['heightPixels'], d['refreshRate'],
             1.0 / d['refreshRate'], d['maxFPS']))
    print('   features=0x%X (bit 42 H.265 = %d)' % (info['features'], (info['features'] >> 42) & 1))

    def query_round(tag):
        print('== mDNS PTR query for _airplay._tcp.local out of each interface (%s)' % tag)
        for alias, ip in local_ipv4():
            seen, arecs, srv = mdns_query(ip, name)
            print('   %-28s %-16s -> %s' % (alias, ip, ('answered, SRV %s, A %s' % (srv, ', '.join(arecs) or '-'))
                                             if seen else 'no answer from %s' % name))
    query_round('initial')
    if a.refresh:
        while time.time() and not any('mDNS announcement repeated' in l for l in lines) and p.poll() is None:
            time.sleep(0.2)
        print('== refreshNetwork() log:')
        for l in lines:
            if 'refreshNetwork' in l or '[network]' in l:
                print('   log:', l.split('] ', 1)[-1] if '] ' in l else l)
        query_round('after refresh')
    p.wait(timeout=30)
    print('== pm_probe exit code %d' % p.returncode)
    for l in lines[-3:]:
        print('   log:', l)


if __name__ == '__main__':
    main()
