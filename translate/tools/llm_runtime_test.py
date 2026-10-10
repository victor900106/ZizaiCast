#!/usr/bin/env python3
"""Local-server cases of the per-CPU runtime download (pm_llm_runtime_test).

  py llm_runtime_test.py EXE ZIP [WORKDIR]

EXE: build\\bin\\Release\\pm_llm_runtime_test.exe (built with PM_DL_TEST_HTTP),
ZIP: llama-b11514-bin-win-cpu-x64.zip (the pinned archive).  Three plain-HTTP
servers on 127.0.0.1 serve ZIP at any path: one with Range, one without
(always 200 + the whole file), one that corrupts every Range answer (a mirror
serving broken bytes; a full GET is good).  Each case downloads the runtime
into its own WORKDIR\\<case> (PM_MODELS_DIR; never the user's data) and checks
how it came.  Exit 0 = every case passed.
"""
import http.server, os, re, shutil, subprocess, sys, tempfile, threading

exe, zpath = sys.argv[1], sys.argv[2]
work = sys.argv[3] if len(sys.argv) > 3 else os.path.join(tempfile.gettempdir(), 'pm-llm-runtime-test')
data = open(zpath, 'rb').read()


def server(mode):
    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            m = re.match(r'bytes=(\d+)-(\d*)', self.headers.get('Range', ''))
            if m and mode != 'norange':
                a = int(m.group(1))
                b = int(m.group(2)) if m.group(2) else len(data) - 1
                body = bytearray(data[a:b + 1])
                if mode == 'badrange' and body:
                    body[len(body) // 2] ^= 0xFF
                self.send_response(206)
                self.send_header('Content-Range', 'bytes %d-%d/%d' % (a, b, len(data)))
            else:
                body = data
                self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(bytes(body))

    s = http.server.ThreadingHTTPServer(('127.0.0.1', 0), H)
    threading.Thread(target=s.serve_forever, daemon=True).start()
    return '127.0.0.1:%d' % s.server_address[1]


RANGE, NORANGE, BAD = server('range'), server('norange'), server('badrange')
NONE = '127.0.0.1:1'  # nothing listens: an unreachable mirror

# name, mirror, origin, variant, expect, run twice (the 2nd: already installed)
cases = [
    ('range', RANGE, NORANGE, 'zen4', 'ranged', True),
    ('no-range-anywhere', NORANGE, NORANGE, '', 'whole', True),
    ('mirror-down-origin-range', NONE, RANGE, 'x64', 'ranged', False),
    ('mirror-broken-origin-range', BAD, RANGE, '', 'ranged', False),
    ('mirror-broken-origin-no-range', BAD, NORANGE, '', 'whole', False),
    ('every-source-broken', BAD, BAD, '', 'fail', False),
]
if os.path.isdir(work):
    shutil.rmtree(work)
failed = 0
for name, mirror, origin, variant, expect, twice in cases:
    env = dict(os.environ, PM_LLM_TEST_MIRROR=mirror, PM_LLM_TEST_ORIGIN=origin, PM_LLM_TEST_VARIANT=variant)
    env.pop('PM_LLAMA_DIR', None)
    d = os.path.join(work, name)
    os.makedirs(d)
    for run, want in [(1, expect)] + ([(2, 'none')] if twice else []):
        p = subprocess.run([exe, '--fetch', d, '--expect', want], env=env, capture_output=True, text=True,
                           creationflags=0x4000 | 0x08000000)  # BELOW_NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW
        line = next((l for l in p.stdout.splitlines() if l.startswith('download')), '?')
        ok = p.returncode == 0
        failed += not ok
        print('%s %s%s: %s' % ('ok  ' if ok else 'FAIL', name, ' (again)' if run == 2 else '', line))
        if not ok:
            print(p.stdout)
print('PASSED' if not failed else 'FAILED (%d)' % failed)
sys.exit(1 if failed else 0)
