// cf_addon_test (0.8.4): the cloudflared add-on download against local fake servers. Never runs cloudflared.
//   node app/tools/cf_addon_test.mjs <pm_cf_addon_test.exe> <real cloudflared.exe> <its LICENSE>
// The "real" files are only read and served (their size / SHA-256 are the app's pins); scenarios:
//   1 mirror serves both  2 again: nothing fetched  3 mirror 404 -> GitHub (origin)  4 corrupt bytes on both -> SHA-256 error,
//   nothing installed  5 cancel mid-download (slow server) -> .part kept, then resume with a Range request  6 no server at all.
import http from 'node:http';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawn } from 'node:child_process';

const [testExe, realExe, realLic] = process.argv.slice(2);
const exeBytes = fs.readFileSync(realExe), licBytes = fs.readFileSync(realLic);
const sha = b => crypto.createHash('sha256').update(b).digest('hex');
console.log(`payload: ${exeBytes.length} bytes sha ${sha(exeBytes).slice(0, 12)}…, license ${licBytes.length} bytes`);
let pass = 0, fail = 0;
const check = (name, ok, extra = '') => { console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${extra ? '  -- ' + extra : ''}`); ok ? pass++ : fail++; };

function server(name, routes, opts = {}) {
  const log = [];
  const srv = http.createServer((req, res) => {
    const body = routes[req.url];
    const range = req.headers.range;
    log.push({ url: req.url, range: range || null });
    if (!body) { res.writeHead(404); res.end('no'); return; }
    let start = 0, end = body.length - 1, code = 200;
    const m = range && /bytes=(\d+)-(\d*)/.exec(range);
    if (m) { start = +m[1]; if (m[2]) end = Math.min(+m[2], body.length - 1); code = 206; }
    const headers = { 'Content-Length': end - start + 1, 'Accept-Ranges': 'bytes', 'Content-Type': 'application/octet-stream' };
    if (code === 206) headers['Content-Range'] = `bytes ${start}-${end}/${body.length}`;
    res.writeHead(code, headers);
    if (!opts.slowBytesPerSec) { res.end(body.subarray(start, end + 1)); return; }
    let pos = start;
    const tick = () => {
      if (pos > end || res.destroyed) { res.end(); return; }
      const n = Math.min(opts.slowBytesPerSec / 20, end + 1 - pos);
      res.write(body.subarray(pos, pos + n)); pos += n;
      setTimeout(tick, 50);
    };
    tick();
  });
  return new Promise(r => srv.listen(0, '127.0.0.1', () => r({ name, srv, log, port: srv.address().port, close: () => { srv.closeAllConnections?.(); srv.close(); } })));
}
const MIRROR = '/ZizaiCast/addons/';
const mirrorRoutes = (exe, lic) => ({ [MIRROR + 'cloudflared-2026.7.2-windows-amd64.exe']: exe, [MIRROR + 'cloudflared-2026.7.2-LICENSE.txt']: lic });
const originRoutes = (exe, lic) => ({ '/cloudflare/cloudflared/releases/download/2026.7.2/cloudflared-windows-amd64.exe': exe, '/cloudflare/cloudflared/2026.7.2/LICENSE': lic });
const corrupt = b => { const c = Buffer.from(b); c[1000] ^= 0xff; return c; };

function run(root, mirrorPort, originPort, extra = []) {
  return new Promise(resolve => {
    const p = spawn(testExe, ['--root', root, ...extra], { env: { ...process.env, PM_CF_TEST_MIRROR: `127.0.0.1:${mirrorPort}`, PM_CF_TEST_ORIGIN: `127.0.0.1:${originPort}` }, stdio: ['ignore', 'pipe', 'pipe'] });
    let out = '';
    p.stdout.on('data', d => out += d); p.stderr.on('data', d => out += d);
    p.on('close', code => {
      const m = /RESULT (ok|err) (.*) installed=(\d) steps=(\d+) maxPct=(\d+) ms=(\d+)/.exec(out) || [];
      resolve({ ok: m[1] === 'ok', err: m[2], installed: m[3] === '1', steps: +m[4], maxPct: +m[5], ms: +m[6], out });
    });
  });
}
const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), 'cfaddon-'));
const dest = root => path.join(root, 'cloudflared', 'cloudflared.exe');

// 1 + 2
{
  const root = tmp();
  const mir = await server('mirror', mirrorRoutes(exeBytes, licBytes)), org = await server('origin', originRoutes(exeBytes, licBytes));
  const r = await run(root, mir.port, org.port);
  check('1 mirror serves both: ok, installed, progress to 100 %', r.ok && r.installed && r.maxPct === 100 && r.steps > 1, `steps ${r.steps}, ${r.ms} ms`);
  check('1 installed bytes = the pinned SHA-256', fs.existsSync(dest(root)) && sha(fs.readFileSync(dest(root))) === sha(exeBytes));
  check('1 LICENSE + VERSION.txt written', sha(fs.readFileSync(path.join(root, 'cloudflared', 'LICENSE'))) === sha(licBytes) &&
    /cloudflared 2026\.7\.2/.test(fs.readFileSync(path.join(root, 'cloudflared', 'VERSION.txt'), 'utf8')));
  check('1 the origin (GitHub) was not asked', org.log.length === 0, `origin requests ${org.log.length}`);
  check('1 no .part left', !fs.existsSync(dest(root) + '.part') && !fs.existsSync(dest(root) + '.part.seg'));
  const before = mir.log.length;
  const r2 = await run(root, mir.port, org.port);
  check('2 second run: ok, nothing fetched again', r2.ok && r2.installed && mir.log.length === before, `new requests ${mir.log.length - before}`);
  mir.close(); org.close(); fs.rmSync(root, { recursive: true, force: true });
}
// 3
{
  const root = tmp();
  const mir = await server('mirror', {}), org = await server('origin', originRoutes(exeBytes, licBytes));
  const r = await run(root, mir.port, org.port);
  check('3 mirror 404 (not published yet): the GitHub release is used', r.ok && r.installed && org.log.length > 0 && mir.log.length > 0, `mirror ${mir.log.length} / origin ${org.log.length} requests`);
  mir.close(); org.close(); fs.rmSync(root, { recursive: true, force: true });
}
// 4
{
  const root = tmp();
  const mir = await server('mirror', mirrorRoutes(corrupt(exeBytes), licBytes)), org = await server('origin', originRoutes(corrupt(exeBytes), licBytes));
  const r = await run(root, mir.port, org.port);
  check('4 corrupt bytes on both hosts: error "SHA-256", not installed', !r.ok && !r.installed && /SHA-256/.test(r.err), `err "${r.err}"`);
  check('4 no cloudflared.exe left behind', !fs.existsSync(dest(root)));
  mir.close(); org.close(); fs.rmSync(root, { recursive: true, force: true });
}
// 5
{
  const root = tmp();
  const slow = await server('mirror', mirrorRoutes(exeBytes, licBytes), { slowBytesPerSec: 800_000 }), org = await server('origin', originRoutes(exeBytes, licBytes));
  const r = await run(root, slow.port, org.port, ['--cancel-after-ms', '800']);
  check('5 cancelled mid-download: err "cancelled", not installed', !r.ok && r.err === 'cancelled' && !r.installed, `err "${r.err}", ${r.ms} ms (cancel at 800)`);
  check('5 cancel answered within 1 s', r.ms < 1800, `${r.ms} ms`);
  const part = dest(root) + '.part';
  check('5 the .part is kept for the next try', fs.existsSync(part), fs.existsSync(part) ? `${fs.statSync(part).size} bytes preallocated` : '');
  slow.close();
  const fast = await server('mirror', mirrorRoutes(exeBytes, licBytes));
  const r2 = await run(root, fast.port, org.port);
  check('5 next run resumes (Range requests) and installs', r2.ok && r2.installed && fast.log.some(l => l.range && !/bytes=0-/.test(l.range)), `ranges ${fast.log.filter(l => l.range).length}`);
  fast.close(); org.close(); fs.rmSync(root, { recursive: true, force: true });
}
// 6
{
  const root = tmp();
  const r = await run(root, 1, 1);
  check('6 nothing reachable: error, not installed, no crash', !r.ok && !r.installed && !!r.err && r.err !== '-', `err "${r.err}", ${r.ms} ms`);
  fs.rmSync(root, { recursive: true, force: true });
}
console.log(`${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
