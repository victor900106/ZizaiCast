// 0.8.4 cloudflared as an on-demand add-on, inside the real app (--dev --test-offscreen instance, own data folder).
//   node app_cfaddon_test.mjs <自在投影.exe (built with -DPM_TEST_HTTP_DL=ON)> <fake_cloudflared.exe> <cloudflared LICENSE> <pm_fake_tailscale.exe> <outDir>
// Never runs cloudflared or tailscale: the "add-on" served by the local fake server is fake_cloudflared.exe (prints an error,
// exits 3; the test build takes its size / SHA-256 as the pin through PM_CF_TEST_PIN), PM_CF_ADDON_ONLY=1 keeps the app away from
// the cloudflared installed on this PC, Funnel goes to the fake tailscale CLI. No sound, no visible window, no real input
// (PM_REMOTE_DRYRUN), only the PIDs started here are ended.
//   A 臨時網址 with no cloudflared: download -> SHA-256 -> started from <data>\addons\cloudflared; used again without a download
//   B 常駐外網 when Funnel cannot start -> 改用臨時網址: the fallback fetches it too
//   C 停止 while downloading -> cancelled, .part kept   D server says 404 -> clear error, state failed
import { spawn, execFileSync } from 'node:child_process';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';

const [exe, fakeExe, licPath, fakeCli, outDir] = process.argv.slice(2);
if (!outDir) { console.error('usage: node app_cfaddon_test.mjs <exe> <fake_cloudflared.exe> <LICENSE> <pm_fake_tailscale.exe> <outDir>'); process.exit(2); }
fs.mkdirSync(outDir, { recursive: true });
const sleep = ms => new Promise(r => setTimeout(r, ms));
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const devCmdPs1 = path.resolve(here, '..', '..', 'share', 'tools', 'dev_cmd.ps1');
const tmpDir = path.resolve(outDir, '_tmp');
const C = { Toggle: 330, Tunnel: 352, FixedToggle: 356, FixedTemp: 358, TempShare: 442, Exit: 104 };
const sha = b => crypto.createHash('sha256').update(b).digest('hex');
const fakeBytes = fs.readFileSync(fakeExe), licBytes = fs.readFileSync(licPath);
const pin = `${fakeBytes.length}:${sha(fakeBytes)}`;
let pass = 0, fail = 0;
const check = (name, ok, extra = '') => { console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${extra ? '  -- ' + extra : ''}`); ok ? pass++ : fail++; };
const salt = crypto.randomBytes(16);
const pwHash = `pbkdf2-sha256$310000$${salt.toString('hex')}$${crypto.pbkdf2Sync('Zizai-Test-2026', salt, 310000, 32, 'sha256').toString('hex')}`;

const cloudflaredPids = () => { try { return execFileSync('powershell', ['-NoProfile', '-Command', "(Get-Process cloudflared -ErrorAction SilentlyContinue | ForEach-Object Id) -join ','"], { encoding: 'utf8' }).trim(); } catch { return '?'; } };

function fileServer({ missing = false, slow = 0, payload = fakeBytes } = {}) {
  const log = [];
  const srv = http.createServer((req, res) => {
    log.push(req.url);
    const body = missing ? null : /LICENSE/i.test(req.url) ? licBytes : /cloudflared/.test(req.url) ? payload : null;
    if (!body) { res.writeHead(404); res.end('no'); return; }
    const m = /bytes=(\d+)-(\d*)/.exec(req.headers.range || '');
    let a = 0, e = body.length - 1, code = 200;
    if (m) { a = +m[1]; if (m[2]) e = Math.min(+m[2], e); code = 206; }
    const h = { 'Content-Length': e - a + 1, 'Accept-Ranges': 'bytes' };
    if (code === 206) h['Content-Range'] = `bytes ${a}-${e}/${body.length}`;
    res.writeHead(code, h);
    if (!slow) { res.end(body.subarray(a, e + 1)); return; }
    let p = a;
    const t = () => { if (p > e || res.destroyed) { res.end(); return; } const n = Math.min(slow, e + 1 - p); res.write(body.subarray(p, p + n)); p += n; setTimeout(t, 100); };
    t();
  });
  return new Promise(r => srv.listen(0, '127.0.0.1', () => r({ log, port: srv.address().port, close: () => { srv.closeAllConnections?.(); srv.close(); } })));
}

async function runCase(name, { persistent = false, mode = 'ok', server, steps, payload = fakeBytes }) {
  console.log(`--- ${name}`);
  const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-cf084');
  fs.rmSync(dataDir, { recursive: true, force: true });
  fs.mkdirSync(dataDir, { recursive: true });
  fs.rmSync(tmpDir, { recursive: true, force: true });
  fs.mkdirSync(path.join(tmpDir, 'pmshots'), { recursive: true });
  const modeFile = path.join(tmpDir, 'mode.txt'), pidsFile = path.join(tmpDir, 'pids.txt');
  fs.writeFileSync(modeFile, mode + '\n');
  fs.writeFileSync(path.join(dataDir, 'settings.ini'),
    `[settings]\nlanguage=zh-TW\nremote_port=${18800 + Math.floor(Math.random() * 300)}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\ntoolbar_hint_shown=1\nwhats_new_seen=0.8.3\nidle_tab=remote\nmiracast=0\nandroid_auto=0\n` +
    (persistent ? `remote_persistent=1\nremote_pw_hash=${pwHash}\nremote_fixed_secret=${crypto.randomBytes(32).toString('hex')}\n` : ''));
  const env = { ...process.env, TEMP: tmpDir, TMP: tmpDir, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: 'cf084', PM_REMOTE_DRYRUN: '1', PM_REMOTE_NOTS: '1',
    PM_REMOTE_LOOPBACK: '1', PM_REMOTE_FAKECLI: fakeCli, PM_FAKE_TS_MODEFILE: modeFile, PM_FAKE_TS_PIDS: pidsFile, PM_FAKE_TS_STATE: path.join(tmpDir, 'state.txt'),
    PM_CF_ADDON_ONLY: '1', PM_CF_TEST_MIRROR: `127.0.0.1:${server.port}`, PM_CF_TEST_ORIGIN: `127.0.0.1:${server.port}`, PM_CF_TEST_PIN: `${payload.length}:${sha(payload)}` };
  const app = spawn(exe, ['--dev', '--test-offscreen'], { stdio: 'ignore', env, windowsHide: true });
  const dev = (cmd, wait = 700) => { try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmdPs1, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', '0', '-Wait', String(wait)], { stdio: 'pipe' }); } catch (e) { console.log('dev cmd failed', cmd, String(e.stdout || e.message).trim()); } };
  const logFile = path.join(dataDir, 'phonemirror.log');
  const readLog = () => { try { return fs.readFileSync(logFile, 'utf8'); } catch { return ''; } };
  const waitLog = async (re, ms) => { const end = Date.now() + ms; while (Date.now() < end) { const m = readLog().match(re); if (m) return m; await sleep(150); } return null; };
  const ctx = { dev, waitLog, readLog, dataDir, modeFile, pidsFile, app, server };
  try {
    if (persistent) await waitLog(/常駐外網 starting/, 15000); else await sleep(2500);
    await steps(ctx);
  } catch (e) { check(`${name}: no exception`, false, String(e && e.stack || e)); }
  dev(C.Exit, 300);
  for (let i = 0; i < 100 && app.exitCode === null; i++) await sleep(100);
  if (app.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], { stdio: 'ignore' }); } catch { } }
  await sleep(400);
  fs.copyFileSync(logFile, path.join(outDir, `cfaddon_${name.split(' ')[0]}.log`));
  return readLog();
}
const addonExe = dataDir => path.join(dataDir, 'addons', 'cloudflared', 'cloudflared.exe');

const before = cloudflaredPids();
console.log('cloudflared processes before:', before || '(none)');

// A
{
  const server = await fileServer();
  const log = await runCase('A temporary address', { server, steps: async ({ dev, waitLog, dataDir }) => {
    dev(C.Toggle, 1200);
    dev(C.TempShare, 300);
    check('A: no cloudflared yet -> says it downloads the add-on', !!(await waitLog(/cloudflared not installed yet: downloading the add-on/, 8000)));
    check('A: download finished, SHA-256 checked', !!(await waitLog(/cloudflared add-on: installed \(SHA-256 checked\)/, 30000)));
    check('A: the file is the pinned one', fs.existsSync(addonExe(dataDir)) && sha(fs.readFileSync(addonExe(dataDir))) === sha(fakeBytes));
    const st = await waitLog(/免安裝外網 starting \(([^)]*addons[\\/]cloudflared[\\/]cloudflared\.exe)\)/, 15000);
    check('A: then started from <data>\\addons\\cloudflared', !!st, st ? st[1] : '');
    check('A: the fake exe ended -> shown as failed (not hung)', !!(await waitLog(/免安裝外網 failed/, 15000)));
    dev(C.TempShare, 300);  // again: the add-on is there, no second download
    await sleep(2500);
  } });
  check('A: second use: started again without a second download', (log.match(/免安裝外網 starting/g) || []).length >= 2 && (log.match(/downloading the add-on/g) || []).length === 1);
  check('A: the server saw the exe + LICENSE requests only', server.log.length > 0 && server.log.every(u => /cloudflared|LICENSE/i.test(u)), `${server.log.length} requests`);
  server.close();
}
// B
{
  const server = await fileServer();
  const log = await runCase('B fixed-address fallback', { persistent: true, mode: 'no-attr', server, steps: async ({ dev, waitLog, dataDir }) => {
    check('B: Funnel could not start (fake CLI)', !!(await waitLog(/常駐外網: Funnel failed/, 25000)));
    dev(C.FixedTemp, 300);
    check('B: 改用臨時網址 without cloudflared downloads the add-on', !!(await waitLog(/cloudflared add-on: downloading to/, 8000)) && !!(await waitLog(/cloudflared add-on: installed/, 30000)));
    check('B: the fallback then starts cloudflared (127.0.0.2)', !!(await waitLog(/cloudflared fallback -> 127\.0\.0\.2:\d+/, 15000)));
    check('B: file installed under <data>\\addons', fs.existsSync(addonExe(dataDir)));
  } });
  check('B: the fake exe ending is reported', /cloudflared fallback ended/.test(log));
  server.close();
}
// C
{
  const big = Buffer.concat([fakeBytes, Buffer.alloc(24 << 20)]);  // 24 MB stand-in, served at ~0.6 MB/s a connection: still running when stopped
  const server = await fileServer({ slow: 60000, payload: big });
  const log = await runCase('C stop while downloading', { server, payload: big, steps: async ({ dev, waitLog, dataDir }) => {
    dev(C.Toggle, 1200);
    dev(C.TempShare, 300);
    check('C: progress is logged', !!(await waitLog(/cloudflared add-on: \d+ % of \d+ MB|cloudflared add-on: downloading to/, 8000)));
    await sleep(1500);
    dev(C.Tunnel, 300);  // 停止 while the address is being prepared
    check('C: stopping cancels the download', !!(await waitLog(/cloudflared add-on: download cancelled/, 8000)));
    check('C: no cloudflared.exe installed from a cancelled download', !fs.existsSync(addonExe(dataDir)));
  } });
  check('C: nothing was started', !/免安裝外網 starting/.test(log));
  server.close();
}
// D
{
  const server = await fileServer({ missing: true });
  const log = await runCase('D server says 404', { server, steps: async ({ dev, waitLog }) => {
    dev(C.Toggle, 1200);
    dev(C.TempShare, 300);
    check('D: a clear failure is logged', !!(await waitLog(/cloudflared add-on: download failed: HTTP 404/, 40000)));
  } });
  check('D: nothing was started', !/免安裝外網 starting/.test(log));
  server.close();
}
const after = cloudflaredPids();
check('no (real or fake) cloudflared process is running that was not already', after === before, `before "${before}" after "${after}"`);
try { fs.rmSync(tmpDir, { recursive: true, force: true }); } catch { }
console.log(`${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
