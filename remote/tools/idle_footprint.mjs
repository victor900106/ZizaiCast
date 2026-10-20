// 0.8.3: what 遠端控制 costs while no phone is connected, and the first-picture
// time of the phone that comes in afterwards (before / after "capture only with a
// phone"). Own --dev --test-offscreen instance, own data folder
// (PM_DEV_INSTANCE=remote083idle), input dry-run, no Tailscale listener, the fixed
// link faked (PM_REMOTE_FUNNELFAKE: nothing is run, port 8443 is never touched),
// 常駐外網 seeded on like the owner's PC. A headless Edge (own throw-away profile)
// is the phone, over the LAN address. Nothing is shown, played or typed.
//   node idle_footprint.mjs <自在投影.exe> <outDir> [--idle 60] [--sessions 4]
// Per phase: threads, private bytes, handles, working set, GPU dedicated memory
// (WMI GPU Process Memory), % of one core over the window, log lines written.
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';

const [exe, outDir, ...rest] = process.argv.slice(2);
let idle = 60, sessions = 4, gapSec = 45;
for (let i = 0; i < rest.length; i++) {
  if (rest[i] === '--idle') idle = +rest[++i];
  else if (rest[i] === '--sessions') sessions = +rest[++i];
  else if (rest[i] === '--gap') gapSec = +rest[++i];
}
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const devCmdPs1 = path.resolve(here, '..', '..', 'share', 'tools', 'dev_cmd.ps1');
const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-remote083idle');
fs.mkdirSync(dataDir, {recursive: true});
for (const f of ['remote_devices.txt', 'phonemirror.log', 'phonemirror.old.log']) { try { fs.rmSync(path.join(dataDir, f)); } catch {} }
const salt = crypto.randomBytes(16);
const pwHash = `pbkdf2-sha256$310000$${salt.toString('hex')}$${crypto.pbkdf2Sync('Zizai-Test-2026', salt, 310000, 32, 'sha256').toString('hex')}`;
const token = crypto.randomBytes(24).toString('base64').replace(/[^A-Za-z0-9]/g, '').slice(0, 32).padEnd(32, 'x');
const port = 18600 + Math.floor(Math.random() * 300);
fs.writeFileSync(path.join(dataDir, 'settings.ini'),
  `[settings]\nlanguage=zh-TW\nremote_token=${token}\nremote_port=${port}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\ntoolbar_hint_shown=1\n` +
  `whats_new_seen=0.8.3\nremote_persistent=1\nremote_pw_hash=${pwHash}\nremote_fixed_secret=${crypto.randomBytes(32).toString('hex')}\n`);
const appLog = path.join(dataDir, 'phonemirror.log');
const readLog = () => { try { return fs.readFileSync(appLog, 'utf8'); } catch { return ''; } };
const app = spawn(exe, ['--dev', '--test-offscreen'], {stdio: 'ignore', env: {...process.env, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: 'remote083idle',
  PM_REMOTE_DRYRUN: '1', PM_REMOTE_TESTCLIP: '1', PM_REMOTE_NOTS: '1', PM_REMOTE_FUNNELFAKE: 'https://example-pc.example-tailnet.ts.net:8443/'}});
log('app pid', app.pid);
const dev = (cmd, lp = 0, wait = 600) => { try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmdPs1, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe'}); } catch {} };
const sample = () => JSON.parse(execFileSync('powershell', ['-NoProfile', '-Command',
  `$p = Get-Process -Id ${app.pid}; $g = 0; try { $g = (Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory | Where-Object { $_.Name -like 'pid_${app.pid}_*' } | Measure-Object -Property DedicatedUsage -Sum).Sum } catch {}; ` +
  `@{cpu=$p.TotalProcessorTime.TotalMilliseconds; threads=$p.Threads.Count; priv=$p.PrivateMemorySize64; ws=$p.WorkingSet64; handles=$p.HandleCount; gpu=[double]$g} | ConvertTo-Json -Compress`], {encoding: 'utf8'}));
const logLines = () => readLog().split('\n').filter(l => l.includes('[remote]')).length;
async function phase(name, seconds) {
  const a = sample(), t0 = Date.now(), l0 = logLines();
  await sleep(seconds * 1000);
  const b = sample(), wall = Date.now() - t0;
  const r = {phase: name, seconds: Math.round(wall / 1000), threads: b.threads, privateMB: +(b.priv / 1048576).toFixed(1), workingSetMB: +(b.ws / 1048576).toFixed(1),
             handles: b.handles, gpuDedicatedMB: +(b.gpu / 1048576).toFixed(1), pctOneCore: +(((b.cpu - a.cpu) / wall) * 100).toFixed(2), remoteLogLines: logLines() - l0};
  log(JSON.stringify(r));
  return r;
}

// ---- the phone: headless Edge over CDP ----
const profileDir = path.join(os.tmpdir(), 'edge-083-idle-' + crypto.randomBytes(3).toString('hex'));
const cdpPort = 9500 + Math.floor(Math.random() * 400);
const edgeExe = 'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe';
let edge = null, ws = null, nextId = 1;
const pend = new Map();
const cdp = (method, params = {}, sessionId) => new Promise(r => { const i = nextId++; pend.set(i, r); ws.send(JSON.stringify({id: i, method, params, ...(sessionId ? {sessionId} : {})})); });
async function startEdge() {
  edge = spawn(edgeExe, [`--user-data-dir=${profileDir}`, `--remote-debugging-port=${cdpPort}`, '--no-first-run', '--headless=new', '--mute-audio',
    '--autoplay-policy=no-user-gesture-required', 'about:blank'], {stdio: 'ignore'});
  let ver;
  for (let i = 0; i < 60 && !ver; i++) { await sleep(200); try { ver = await (await fetch(`http://127.0.0.1:${cdpPort}/json/version`)).json(); } catch {} }
  ws = new WebSocket(ver.webSocketDebuggerUrl);
  await new Promise(r => ws.addEventListener('open', r));
  ws.addEventListener('message', e => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m.result || m.error); pend.delete(m.id); } });
}
const evalIn = async (sid, expression) => { const r = await cdp('Runtime.evaluate', {expression, returnByValue: true}, sid); return r && r.result ? r.result.value : undefined; };
async function session(url, tag, allow) {
  const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
  const {sessionId: sid} = await cdp('Target.attachToTarget', {targetId, flatten: true});
  await cdp('Emulation.setDeviceMetricsOverride', {width: 390, height: 844, deviceScaleFactor: 3, mobile: true}, sid);
  await cdp('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5}, sid);
  const t0 = Date.now();
  await cdp('Page.navigate', {url}, sid);
  if (allow) { await sleep(2500); dev(961, 3, 600); }
  let first = null;
  for (let i = 0; i < 100 && !first; i++) { await sleep(150); first = await evalIn(sid, 'window.__pmFirstFrame ? JSON.stringify({ttff: Math.round(window.__pmFirstFrame), ph: S.ph}) : null'); }
  const r = first ? JSON.parse(first) : {ttff: null};
  r.navToFirstMs = first ? Date.now() - t0 : null;
  r.tag = tag;
  await sleep(4000);
  await cdp('Target.closeTarget', {targetId});
  log('session', JSON.stringify(r));
  return r;
}

const out = {exe, phases: [], sessions: [], serverFirstFrame: []};
try {
  await sleep(5000);
  const url = (readLog().match(/remote: on at (http:\/\/[\d.]+:\d+\/\w+\/)/) || [])[1];
  if (!url) throw new Error('remote did not come on (persistent seed)');
  log('LAN page', url.replace(/\/\w{32}\//, '/<token>/'));
  out.phases.push(await phase('常駐外網 on, no phone yet', idle));
  await startEdge();
  for (let n = 0; n < sessions; n++) {
    out.sessions.push(await session(url, n === 0 ? 'first phone (allow + remember)' : `phone again after ${gapSec} s`, n === 0));
    out.phases.push(await phase(`${gapSec} s after phone #${n + 1} left`, gapSec));
  }
  out.phases.push(await phase('long idle after the phones', idle));
  out.serverFirstFrame = [...readLog().matchAll(/first frame sent (\d+) ms after the offer request/g)].map(m => +m[1]);
  out.captureOpens = (readLog().match(/capture: monitor \d+ at/g) || []).length;
} catch (e) { log('ERROR', e.message); out.error = e.message; }
try { if (ws) await cdp('Browser.close'); } catch {}
for (let i = 0; i < 50 && edge && edge.exitCode === null; i++) await sleep(100);
if (edge && edge.exitCode === null) try { execFileSync('taskkill', ['/PID', String(edge.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
fs.writeFileSync(path.join(outDir, 'footprint.json'), JSON.stringify(out, null, 1));
fs.writeFileSync(path.join(outDir, 'app.log'), readLog().replace(/\/[A-Za-z0-9]{32}\//g, '/<token>/'));
dev(104, 0, 500);
for (let i = 0; i < 100 && app.exitCode === null; i++) await sleep(100);
if (app.exitCode === null) try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
log('my app exited:', app.exitCode !== null);
