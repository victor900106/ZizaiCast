// Before/after measurements for pm_remote (node 22+).
//   node measure.mjs <spike.exe> <outDir> <seconds> [--touch-every MS] [--warmup S] [spike args...]
// Starts the spike (dry-run), a headless Edge (own throw-away profile) on the
// page through the LAN IP, optionally taps the page every MS ms (a touch =
// "the phone is in use"), and after <seconds> summarises the PC's 5-second
// stats lines (after the warm-up): fps sent, kbps, max frame, IDRs, CPU.
// Closes only what it started.
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';

const [spikeExe, outDir, secArg, ...rest] = process.argv.slice(2);
let touchEvery = 0, warmup = 6;
const spikeArgs = [];
for (let i = 0; i < rest.length; i++) {
  if (rest[i] === '--touch-every') touchEvery = +rest[++i];
  else if (rest[i] === '--warmup') warmup = +rest[++i];
  else spikeArgs.push(rest[i]);
}
if (spikeArgs.includes('--inject')) { console.error('measurements never inject'); process.exit(2); }
const seconds = +secArg;
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const token = crypto.randomBytes(16).toString('hex');
const port = 8600 + Math.floor(Math.random() * 400);
const logFile = path.join(outDir, 'spike.log');
fs.writeFileSync(logFile, '');
const spike = spawn(spikeExe, ['--token', token, '--port', String(port), '--log', logFile, '--no-tailscale', '--duration', String(seconds + 60),
                               '--stdin-quit', ...spikeArgs], {stdio: ['pipe', 'pipe', 'pipe']});
let out = '';
spike.stdout.on('data', d => { out += d; });
spike.stderr.on('data', d => { out += d; });
let url = '';
for (let i = 0; i < 100 && !url; i++) { await sleep(100); const m = out.match(/urls: lan (http:\/\/[\d.]+:\d+\/[0-9a-f]+\/)/); if (m) url = m[1]; }
const profileDir = path.join(os.tmpdir(), 'edge-080-measure-' + crypto.randomBytes(3).toString('hex'));
const cdpPort = 9400 + Math.floor(Math.random() * 400);
const edge = spawn('C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
  [`--user-data-dir=${profileDir}`, `--remote-debugging-port=${cdpPort}`, '--no-first-run', '--no-default-browser-check', '--disable-extensions',
   '--headless=new', 'about:blank'], {stdio: 'ignore'});
let ws, nextId = 1;
const pending = new Map();
const cdp = (method, params = {}, sessionId) => { const id = nextId++; ws.send(JSON.stringify({id, method, params, ...(sessionId ? {sessionId} : {})}));
  return new Promise((res, rej) => pending.set(id, {res, rej})); };
let code = 0;
const t0 = Date.now();
try {
  let ver;
  for (let i = 0; i < 50 && !ver; i++) { await sleep(200); try { ver = await (await fetch(`http://127.0.0.1:${cdpPort}/json/version`)).json(); } catch {} }
  ws = new WebSocket(ver.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  ws.onmessage = e => { const m = JSON.parse(e.data); if (m.id && pending.has(m.id)) { const p = pending.get(m.id); pending.delete(m.id); m.error ? p.rej(new Error(m.error.message)) : p.res(m.result); } };
  const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
  const {sessionId: S} = await cdp('Target.attachToTarget', {targetId, flatten: true});
  await cdp('Emulation.setDeviceMetricsOverride', {width: 844, height: 390, deviceScaleFactor: 3, mobile: true}, S);
  await cdp('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5}, S);
  await cdp('Page.navigate', {url}, S);
  const end = Date.now() + seconds * 1000;
  let n = 0;
  while (Date.now() < end) {
    if (touchEvery > 0) {
      // a short one-finger drag (moves the dry-run pointer; counts as use)
      const y = 150 + (n++ % 5) * 10;
      await cdp('Input.dispatchTouchEvent', {type: 'touchStart', touchPoints: [{x: 300, y}]}, S);
      for (let i = 1; i <= 4; i++) await cdp('Input.dispatchTouchEvent', {type: 'touchMove', touchPoints: [{x: 300 + i * 6, y}]}, S);
      await cdp('Input.dispatchTouchEvent', {type: 'touchEnd', touchPoints: []}, S);
      await sleep(touchEvery);
    } else await sleep(500);
  }
  try { await Promise.race([cdp('Browser.close'), sleep(3000)]); } catch {}
} catch (e) { console.error(e); code = 1; }
try { spike.stdin.end(); } catch {}
for (let i = 0; i < 150 && spike.exitCode === null; i++) await sleep(100);
for (const p of [edge, spike]) if (p.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(p.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} }
const mine = () => { try { return execFileSync('powershell', ['-NoProfile', '-Command',
  `Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" | Where-Object { $_.CommandLine -and (Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue | Where-Object { -not $_.HasExited }) -and $_.CommandLine.Contains('${profileDir}') } | ForEach-Object { $_.ProcessId }`],
  {encoding: 'utf8'}).split(/\s+/).filter(Boolean).map(Number); } catch { return []; } };
let left = mine();
for (let i = 0; i < 20 && left.length; i++) { await sleep(1000); left = mine(); }
for (const pid of left) { try { execFileSync('taskkill', ['/PID', String(pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} }

// ---- summary of the stats lines after the warm-up ----
const L = fs.readFileSync(logFile, 'utf8').split('\n');
const first = L.find(l => /stats: \d+x\d+/.test(l));
const ts = l => { const m = l.match(/^(\d\d):(\d\d):(\d\d)\.(\d\d\d)/); return m ? ((+m[1] * 60 + +m[2]) * 60 + +m[3]) * 1000 + +m[4] : 0; };
const startT = first ? ts(first) + warmup * 1000 : 0;
const rows = L.filter(l => /stats: \d+x\d+/.test(l) && ts(l) >= startT).map(l => {
  const g = re => { const m = l.match(re); return m ? +m[1] : NaN; };
  return {fps: g(/ ([\d.]+) fps sent/), kbps: g(/, (\d+) kbps/), maxKB: g(/max frame ([\d.]+) KB/), idr: g(/KB, (\d+) IDR/), cpu1: g(/\(([\d.]+)% of one core\)/)};
});
const avg = k => rows.length ? rows.reduce((a, r) => a + r[k], 0) / rows.length : NaN;
const summary = {args: spikeArgs.join(' '), touchEvery, windows: rows.length, fps: +avg('fps').toFixed(1), kbps: Math.round(avg('kbps')),
  maxKB: rows.length ? Math.max(...rows.map(r => r.maxKB)) : NaN, idrs: rows.reduce((a, r) => a + r.idr, 0), cpu1: +avg('cpu1').toFixed(1),
  tiers: L.filter(l => /frame rate \d+ -> \d+/.test(l)).map(l => l.replace(/^.*frame rate /, '').trim()).slice(0, 6), secs: Math.round((Date.now() - t0) / 1000)};
fs.writeFileSync(path.join(outDir, 'summary.json'), JSON.stringify(summary, null, 1));
console.log(JSON.stringify(summary));
try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
process.exit(code);
