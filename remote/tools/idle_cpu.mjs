// Idle CPU of the app (no phone connected), in states: remote off / remote on /
// remote on + 免安裝外網 tunnel. Own --dev instance and data folder
// (PM_DEV_INSTANCE=remote08cpu), offscreen. Per state: % of one core over N s
// (process time / wall time), plus the hottest threads.
//   node idle_cpu.mjs <自在投影.exe> <outDir> [--seconds 40] [--visible]
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

const [exe, outDir, ...rest] = process.argv.slice(2);
let seconds = 40, visible = false;
for (let i = 0; i < rest.length; i++) { if (rest[i] === '--seconds') seconds = +rest[++i]; else if (rest[i] === '--visible') visible = true; }
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const devCmdPs1 = path.resolve(here, '..', '..', 'share', 'tools', 'dev_cmd.ps1');
const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-remote08cpu');
fs.mkdirSync(dataDir, {recursive: true});
try { fs.rmSync(path.join(dataDir, 'phonemirror.log')); } catch {}
fs.writeFileSync(path.join(dataDir, 'settings.ini'), `[settings]\nlanguage=zh-TW\nremote_port=${8800 + Math.floor(Math.random() * 90)}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\ntoolbar_hint_shown=1\n`);
const args = ['--dev'];
if (!visible) args.push('--test-offscreen');
const app = spawn(exe, args, {stdio: 'ignore', env: {...process.env, PM_DEV_INSTANCE: 'remote08cpu', PM_REMOTE_DRYRUN: '1', PM_REMOTE_NOTS: '1'}});
const dev = (cmd, lp = 0, wait = 600) => { try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmdPs1, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe'}); } catch {} };
const sample = () => JSON.parse(execFileSync('powershell', ['-NoProfile', '-Command',
  `$p = Get-Process -Id ${app.pid}; @{cpu=$p.TotalProcessorTime.TotalMilliseconds; threads=@($p.Threads | ForEach-Object { @{id=$_.Id; ms=$_.TotalProcessorTime.TotalMilliseconds} })} | ConvertTo-Json -Depth 3 -Compress`], {encoding: 'utf8'}));
async function measure(name) {
  await sleep(8000);
  const a = sample(), t0 = Date.now();
  await sleep(seconds * 1000);
  const b = sample(), wall = Date.now() - t0;
  const before = new Map(a.threads.map(t => [t.id, t.ms]));
  const hot = b.threads.map(t => ({id: t.id, pct: +(((t.ms - (before.get(t.id) || 0)) / wall) * 100).toFixed(2)})).sort((x, y) => y.pct - x.pct).slice(0, 6);
  const r = {state: name, pctOneCore: +(((b.cpu - a.cpu) / wall) * 100).toFixed(2), hot};
  console.log(JSON.stringify(r));
  return r;
}
const out = [];
await sleep(4000);
out.push(await measure('remote off'));
dev(330, 0, 1500);  // remote on (QR panel opens)
out.push(await measure('remote on, no phone'));
dev(352, 0, 800);   // 免安裝外網
await sleep(15000);
out.push(await measure('remote on + tunnel, no phone'));
// A phone session like the owner's (sound on, 手機播聲音時電腦靜音 toggled during it), then gone.
{
  const L = () => { try { return fs.readFileSync(path.join(dataDir, 'phonemirror.log'), 'utf8'); } catch { return ''; } };
  const url = (L().match(/remote: on at (http:\/\/[\d.]+:\d+\/\w+\/)/) || [])[1];
  const os = await import('node:os');
  const profileDir = path.join(os.tmpdir(), 'edge-082-cpu');
  try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
  const cdpPort = 9400 + Math.floor(Math.random() * 400);
  const edge = spawn('C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
    [`--user-data-dir=${profileDir}`, `--remote-debugging-port=${cdpPort}`, '--no-first-run', '--headless=new', '--autoplay-policy=no-user-gesture-required', 'about:blank'], {stdio: 'ignore'});
  let ver;
  for (let i = 0; i < 50 && !ver; i++) { await sleep(200); try { ver = await (await fetch(`http://127.0.0.1:${cdpPort}/json/version`)).json(); } catch {} }
  const ws = new WebSocket(ver.webSocketDebuggerUrl);
  await new Promise(r => ws.addEventListener('open', r));
  let id = 1; const pend = new Map();
  ws.addEventListener('message', e => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m.result); pend.delete(m.id); } });
  const cdp = (method, params = {}, sessionId) => new Promise(r => { const i = id++; pend.set(i, r); ws.send(JSON.stringify({id: i, method, params, ...(sessionId ? {sessionId} : {})})); });
  const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
  const {sessionId} = await cdp('Target.attachToTarget', {targetId, flatten: true});
  await cdp('Page.navigate', {url}, sessionId);
  await sleep(3000);
  dev(961, 3, 800);  // 允許 + 記住
  await sleep(5000);
  await cdp('Runtime.evaluate', {expression: 'soundTap()', userGesture: true}, sessionId);
  await sleep(4000);
  dev(334, 0, 800);  // 手機播聲音時電腦靜音 (0.8.1: restart queued for after the session)
  await sleep(4000);
  dev(334, 0, 800);  // back off
  await sleep(2000);
  await cdp('Browser.close');
  for (let i = 0; i < 50 && edge.exitCode === null; i++) await sleep(100);
  if (edge.exitCode === null) try { execFileSync('taskkill', ['/PID', String(edge.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
  await sleep(15000);
  out.push(await measure('remote on, after a phone session'));
  out.push(await measure('remote on, after a phone session (later)'));
}
fs.writeFileSync(path.join(outDir, 'idle-cpu.json'), JSON.stringify(out, null, 1));
fs.copyFileSync(path.join(dataDir, 'phonemirror.log'), path.join(outDir, 'app.log'));
dev(104, 0, 500);
for (let i = 0; i < 100 && app.exitCode === null; i++) await sleep(100);
if (app.exitCode === null) try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
console.log('my app exited:', app.exitCode !== null);
