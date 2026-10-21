// 0.8.3.2 app check (node 22+): 遠端斷線後自動鎖定電腦 is OFF by default (outside and LAN), and the
// menu option still works when turned on.
//   node app0832_test.mjs <自在投影.exe> <outDir>
// Own --dev --test-offscreen instance and data folder (PM_DEV_INSTANCE=remote0832app), a fresh
// settings.ini WITHOUT remote_lock_* keys, PM_REMOTE_LOOPBACK=1 (the LAN side on 127.0.0.3 only: no
// firewall prompt), no Tailscale listener, the fixed link faked (PM_REMOTE_FUNNELFAKE: nothing is
// run, port 8443 is never touched), input dry-run. --dev never calls LockWorkStation (it logs
// "TEST STUB: would lock the PC now"). A headless, muted Edge (own throw-away profile) is the
// phone; nothing is played, typed or shown (every window of the app is checked to be outside
// every monitor); only the processes started here are closed.
//   1. defaults: a fixed-link phone leaves -> no lock stub, the page shows no lock note,
//      settings.ini then says remote_lock_outside=0 / remote_lock_lan=0
//   2. 遠端控制 ▸ 遠端斷線後自動鎖定電腦 ▸ 從外網連線時 (CmdRemoteLockOutside = 379) turned on:
//      the page shows the note, the phone leaves -> one stub "from outside", settings say 1
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import http from 'node:http';
import crypto from 'node:crypto';

const [exe, outDir] = process.argv.slice(2);
if (!exe || !outDir) { console.error('usage: node app0832_test.mjs <自在投影.exe> <outDir>'); process.exit(2); }
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const devCmdPs1 = path.resolve(here, '..', '..', 'share', 'tools', 'dev_cmd.ps1');
const CmdRemoteLockOutside = 379, CmdExit = 104;

const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-remote0832app');
fs.rmSync(dataDir, {recursive: true, force: true});
fs.mkdirSync(dataDir, {recursive: true});
const PW = 'Zizai-Test-2026';
const salt = crypto.randomBytes(16);
const pwHash = `pbkdf2-sha256$310000$${salt.toString('hex')}$${crypto.pbkdf2Sync(PW, salt, 310000, 32, 'sha256').toString('hex')}`;
const token = crypto.randomBytes(24).toString('base64').replace(/[^A-Za-z0-9]/g, '').slice(0, 32).padEnd(32, 'x');
const port = 19300 + Math.floor(Math.random() * 300);
fs.writeFileSync(path.join(dataDir, 'settings.ini'),
  `[settings]\nlanguage=zh-TW\nremote_token=${token}\nremote_port=${port}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\ntoolbar_hint_shown=1\n` +
  `whats_new_seen=0.8.3\nremote_persistent=1\nremote_pw_hash=${pwHash}\nremote_fixed_secret=${crypto.randomBytes(32).toString('hex')}\n`);
const appLog = path.join(dataDir, 'phonemirror.log');
const readLog = () => { try { return fs.readFileSync(appLog, 'utf8'); } catch { return ''; } };
const iniFlags = () => { const s = fs.readFileSync(path.join(dataDir, 'settings.ini'), 'utf8'); return {outside: (s.match(/remote_lock_outside=(\d)/) || [])[1] ?? null, lan: (s.match(/remote_lock_lan=(\d)/) || [])[1] ?? null}; };
const env = {...process.env, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: 'remote0832app', PM_REMOTE_DRYRUN: '1', PM_REMOTE_TESTCLIP: '1', PM_REMOTE_NOTS: '1',
             PM_REMOTE_LOOPBACK: '1', PM_REMOTE_FUNNELFAKE: 'https://example-pc.example.ts.net:8443/'};

let app = null;
const dev = (cmd, lp = 0, wait = 600) => { try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmdPs1, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe', windowsHide: true}); } catch {} };
// Every visible top-level window of a process, and whether any of it is on a monitor.
function windowsOnScreen(pid) {
  const ps = `Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Collections.Generic;
public static class PmWin {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public static List<int[]> Of(uint pid) { var l = new List<int[]>(); EnumWindows((h, x) => { uint p; GetWindowThreadProcessId(h, out p);
    if (p == pid && IsWindowVisible(h)) { RECT r; GetWindowRect(h, out r); l.Add(new int[] { r.L, r.T, r.R, r.B }); } return true; }, IntPtr.Zero); return l; }
}
"@
$w = [PmWin]::Of(${pid}); $on = 0
foreach ($r in $w) { foreach ($s in [System.Windows.Forms.Screen]::AllScreens) { $b = $s.Bounds; if ($r[0] -lt $b.Right -and $r[2] -gt $b.Left -and $r[1] -lt $b.Bottom -and $r[3] -gt $b.Top) { $on++ } } }
Write-Output "$($w.Count) $on"`;
  try {
    const [n, on] = execFileSync('powershell', ['-NoProfile', '-Command', ps], {encoding: 'utf8', windowsHide: true}).trim().split(/\s+/).map(Number);
    return {visibleWindows: n, onAMonitor: on};
  } catch (e) { return {error: String(e.message || e).slice(0, 200)}; }
}
async function startApp() {
  app = spawn(exe, ['--dev', '--test-offscreen'], {stdio: 'ignore', env, windowsHide: true});
  log('app pid', app.pid);
  for (let i = 0; i < 100 && !/remote: on at http/.test(readLog()); i++) await sleep(200);
  for (let i = 0; i < 100 && !/常駐外網[^\n]*(on|up|ready|https:\/\/example-pc)/.test(readLog()); i++) await sleep(200);
  await sleep(1500);
}
async function stopApp() {
  dev(CmdExit, 0, 500);
  for (let i = 0; i < 100 && app.exitCode === null; i++) await sleep(100);
  if (app.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} log('killed own app'); }
}

// ---- the phone: headless, muted Edge ----
const profileDir = path.join(os.tmpdir(), 'edge-0832-app-' + crypto.randomBytes(3).toString('hex'));
const cdpPort = 9800 + Math.floor(Math.random() * 90);
let edge = null, ws = null, nextId = 1;
const pend = new Map();
const cdp = (method, params = {}, sessionId) => new Promise(r => { const i = nextId++; pend.set(i, r); ws.send(JSON.stringify({id: i, method, params, ...(sessionId ? {sessionId} : {})})); });
async function startEdge() {
  edge = spawn('C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe', [`--user-data-dir=${profileDir}`, `--remote-debugging-port=${cdpPort}`, '--no-first-run',
    '--headless=new', '--mute-audio', '--allow-loopback-in-peer-connection', 'about:blank'], {stdio: 'ignore', windowsHide: true});
  let ver;
  for (let i = 0; i < 60 && !ver; i++) { await sleep(200); try { ver = await (await fetch(`http://127.0.0.1:${cdpPort}/json/version`)).json(); } catch {} }
  ws = new WebSocket(ver.webSocketDebuggerUrl);
  await new Promise(r => ws.addEventListener('open', r));
  ws.addEventListener('message', e => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m.result || m.error); pend.delete(m.id); } });
}
async function tab(url) {
  const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
  const {sessionId} = await cdp('Target.attachToTarget', {targetId, flatten: true});
  await cdp('Emulation.setDeviceMetricsOverride', {width: 390, height: 844, deviceScaleFactor: 3, mobile: true}, sessionId);
  await cdp('Page.navigate', {url}, sessionId);
  const ev = async (expression, awaitPromise = false) => { const r = await cdp('Runtime.evaluate', {expression, returnByValue: true, awaitPromise}, sessionId); return r && r.result ? r.result.value : undefined; };
  return {ev, close: () => cdp('Target.closeTarget', {targetId})};
}
const waitFor = async (fn, ms) => { for (let t = 0; t < ms; t += 200) { const v = await fn(); if (v) return v; await sleep(200); } return null; };
async function fixedSession() {  // a fresh fixed-link login (via 127.0.0.2, as Funnel delivers it), then the phone leaves
  await cdp('Storage.clearDataForOrigin', {origin: `http://127.0.0.2:${port}`, storageTypes: 'local_storage'});
  const t = await tab(`http://127.0.0.2:${port}/`);
  await waitFor(() => t.ev(`$('pass').style.display === 'flex'`), 8000);
  await t.ev(`(() => { $('passIn').value = ${JSON.stringify(PW)}; submitPass(); return 1; })()`);
  const frame = await waitFor(() => t.ev('window.__pmFirstFrame ? 1 : 0'), 20000);
  await sleep(1500);
  const note = await t.ev(`$('lockNote').style.display !== 'none'`);
  await t.close();
  await sleep(8000);
  return {connected: !!frame, lockNoteShown: note};
}
const stubs = () => (readLog().match(/TEST STUB: would lock the PC now \(([^)]*)/g) || []);

const out = {};
try {
  await startApp();
  out.windows = windowsOnScreen(app.pid);
  await startEdge();
  out.defaults = await fixedSession();
  out.defaults.lockStubs = stubs().length;
  // every page closed before the restart (a closed tab's page could still reconnect to the new instance)
  for (const t of ((await cdp('Target.getTargets')) || {}).targetInfos || []) if (t.type === 'page') await cdp('Target.closeTarget', {targetId: t.targetId});
  await stopApp();
  out.defaults.ini = iniFlags();
  await startApp();
  out.windowsAfterRestart = windowsOnScreen(app.pid);
  dev(CmdRemoteLockOutside, 0, 800);
  out.toggleLogged = (readLog().match(/remote: 遠端斷線後自動鎖定電腦: [^\n]*/g) || []).slice(-1)[0] || null;
  out.turnedOn = await fixedSession();
  out.turnedOn.lockStubs = stubs().length;
  out.turnedOn.allFromOutside = stubs().every(s => /from outside/.test(s));
  out.turnedOn.lastStub = stubs().slice(-1)[0] || null;
  await stopApp();
  out.turnedOn.ini = iniFlags();
} catch (e) { out.error = String(e && e.message || e); }
try { if (ws) await cdp('Browser.close'); } catch {}
for (let i = 0; i < 50 && edge && edge.exitCode === null; i++) await sleep(100);
if (edge && edge.exitCode === null) try { execFileSync('taskkill', ['/PID', String(edge.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
await sleep(800);
try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
if (app && app.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} }
const checks = [
  ['no window of the app on any monitor', out.windows && out.windows.onAMonitor === 0 && out.windowsAfterRestart && out.windowsAfterRestart.onAMonitor === 0],
  ['defaults: the fixed-link phone connected', out.defaults && out.defaults.connected],
  ['defaults: no lock note on the page', out.defaults && out.defaults.lockNoteShown === false],
  ['defaults: the phone left -> the PC is not locked', out.defaults && out.defaults.lockStubs === 0],
  ['defaults: settings.ini remote_lock_outside=0, remote_lock_lan=0', out.defaults && out.defaults.ini && out.defaults.ini.outside === '0' && out.defaults.ini.lan === '0'],
  ['menu: 從外網連線時 turned on (logged)', /outside on, LAN \/ Tailscale off/.test(out.toggleLogged || '')],
  ['turned on: the page shows the lock note', out.turnedOn && out.turnedOn.lockNoteShown === true],
  // (each session the phone side ended counts: the page may reconnect once before its first frame)
  ['turned on: the phone left -> the PC locks (stub, from outside)', out.turnedOn && out.turnedOn.lockStubs >= 1 && out.turnedOn.allFromOutside],
  ['turned on: settings.ini remote_lock_outside=1', out.turnedOn && out.turnedOn.ini && out.turnedOn.ini.outside === '1'],
];
out.checks = checks.map(([n, ok]) => `${ok ? 'PASS' : 'FAIL'} ${n}`);
fs.writeFileSync(path.join(outDir, 'app0832.json'), JSON.stringify(out, null, 1));
fs.writeFileSync(path.join(outDir, 'app.log'), readLog().replace(/\/[A-Za-z0-9]{32}\//g, '/<token>/').replace(/\/k\/[0-9a-f.]+\//g, '/k/<login>/'));
for (const c of out.checks) console.log(c);
process.exit(checks.every(c => c[1]) ? 0 : 1);
