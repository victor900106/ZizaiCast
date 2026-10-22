// 0.8.3.2 臨時帳號 inside the app (node 22+):
//   node app_guest_test.mjs <自在投影.exe> <outDir>
// Own --dev --test-offscreen instance and data folder, PM_REMOTE_LOOPBACK (no firewall prompt), the fixed
// link made up (PM_REMOTE_FUNNELFAKE: no tailscale run), input dry-run. 複製全部 goes to a test file
// (PM_TEST_GUEST_OUT), never the real clipboard. A headless muted Edge is the phone.
// Checks: two accounts made in the 新增臨時帳號 window (用一次 可操作; 24 小時 僅觀看); remote_guests.txt and
// the log hold no password; a guest's phone needs 允許 and is never remembered (even with 記住 asked);
// 連線紀錄 names the account; the 用一次 account is gone after its session; 刪除 on the PC ends the other
// one's session at once; no window on any monitor.
import {spawn, execFileSync} from 'node:child_process';
import {headlessShell} from './quiet_browser.mjs';  // rel-0836: chrome-headless-shell (no window, no Alt+Tab entry), not Edge / Chrome --headless=new
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';

const [exe, outDir] = process.argv.slice(2);
if (!exe || !outDir) { console.error('usage: node app_guest_test.mjs <自在投影.exe> <outDir>'); process.exit(2); }
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const devCmdPs1 = path.resolve(here, '..', '..', 'share', 'tools', 'dev_cmd.ps1');
const C = {GuestNew: 440, GuestDel0: 450, Exit: 104};
const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-guest0832app');
fs.rmSync(dataDir, {recursive: true, force: true});
fs.mkdirSync(dataDir, {recursive: true});
const tmpDir = path.join(outDir, '_tmp');
fs.rmSync(tmpDir, {recursive: true, force: true});
fs.mkdirSync(path.join(tmpDir, 'pmshots'), {recursive: true});
const OWNER = 'Zizai-Test-2026';
const salt = crypto.randomBytes(16);
const pwHash = `pbkdf2-sha256$310000$${salt.toString('hex')}$${crypto.pbkdf2Sync(OWNER, salt, 310000, 32, 'sha256').toString('hex')}`;
const port = 19500 + Math.floor(Math.random() * 200);
fs.writeFileSync(path.join(dataDir, 'settings.ini'),
  `[settings]\nlanguage=zh-TW\nremote_port=${port}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\ntoolbar_hint_shown=1\n` +
  `whats_new_seen=0.8.3\nidle_tab=remote\nremote_persistent=1\nremote_pw_hash=${pwHash}\nremote_fixed_secret=${crypto.randomBytes(32).toString('hex')}\n`);
const copyFile = path.join(tmpDir, 'copy.txt');
const env = {...process.env, TEMP: tmpDir, TMP: tmpDir, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: 'guest0832app', PM_REMOTE_DRYRUN: '1', PM_REMOTE_SYNTHAUDIO: 'quiet', PM_REMOTE_TESTCLIP: '1',
  PM_REMOTE_NOTS: '1', PM_REMOTE_LOOPBACK: '1', PM_REMOTE_FUNNELFAKE: 'https://example-pc.example.ts.net:8443/', PM_TEST_GUEST_OUT: copyFile};
const readLog = () => { try { return fs.readFileSync(path.join(dataDir, 'phonemirror.log'), 'utf8'); } catch { return ''; } };
const waitLog = async (re, ms) => { const end = Date.now() + ms; while (Date.now() < end) { const m = readLog().match(re); if (m) return m; await sleep(150); } return null; };
const app = spawn(exe, ['--dev', '--test-offscreen'], {stdio: 'ignore', env, windowsHide: true});
log('app pid', app.pid);
const dev = (cmd, lp = 0, wait = 600) => { try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmdPs1, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe', windowsHide: true}); } catch {} };
function onScreen(pid) {
  const ps = `Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Collections.Generic;
public static class PmWin3 {
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
$w = [PmWin3]::Of(${pid}); $on = 0
foreach ($r in $w) { foreach ($s in [System.Windows.Forms.Screen]::AllScreens) { $b = $s.Bounds; if ($r[0] -lt $b.Right -and $r[2] -gt $b.Left -and $r[1] -lt $b.Bottom -and $r[3] -gt $b.Top) { $on++ } } }
Write-Output "$($w.Count) $on"`;
  try { const [n, on] = execFileSync('powershell', ['-NoProfile', '-Command', ps], {encoding: 'utf8', windowsHide: true}).trim().split(/\s+/).map(Number); return {n, on}; } catch { return {n: -1, on: -1}; }
}
// A new account through the window: lp = life | 4 view | 8 files | 16 create; its 複製全部 text.
async function makeGuest(lp) {
  try { fs.rmSync(copyFile); } catch {}
  dev(C.GuestNew, 0, 1000); dev(981, lp | 16, 1500); dev(983, 0, 600); dev(984, 0, 600);
  const t = fs.existsSync(copyFile) ? fs.readFileSync(copyFile, 'utf8') : '';
  const name = (t.match(/帳號：([^\s（(]+)/) || [])[1], pw = (t.match(/密碼：([^\s（(]+)/) || [])[1];  // rel-0836: 0.8.3.3+ adds （打開網址就已填好）
  return {name, pw};
}

// ---- the phone ----
const profileDir = path.join(os.tmpdir(), 'edge-0832-guestapp-' + crypto.randomBytes(3).toString('hex'));
const cdpPort = 9200 + Math.floor(Math.random() * 90);
let edge = null, ws = null, nextId = 1;
const pend = new Map();
const cdp = (method, params = {}, sessionId) => new Promise(r => { const i = nextId++; pend.set(i, r); ws.send(JSON.stringify({id: i, method, params, ...(sessionId ? {sessionId} : {})})); });
async function startEdge() {
  edge = spawn(headlessShell(), ['--mute-audio', `--user-data-dir=${profileDir}`, `--remote-debugging-port=${cdpPort}`, '--no-first-run',
    '--allow-loopback-in-peer-connection', 'about:blank'], {stdio: 'ignore', windowsHide: true});
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
// The phone logs in with the account; the PC answers 允許 (how: 1 allow, 3 allow + 記住) when asked.
async function guestSession(g, how) {
  await cdp('Storage.clearDataForOrigin', {origin: `http://127.0.0.2:${port}`, storageTypes: 'local_storage'});
  // rel-0836: since 0.8.3.3 the account comes with the address (#u=, as 複製全部 gives it); the user box alone is not read
  const t = await tab(`http://127.0.0.2:${port}/#u=${encodeURIComponent(g.name)}`);
  await waitFor(() => t.ev(`$('pass').style.display === 'flex'`), 8000);
  await t.ev(`(() => { $('userIn').value = ${JSON.stringify(g.name)}; $('passIn').value = ${JSON.stringify(g.pw)}; submitPass(); return 1; })()`);
  const asked = await waitLog(new RegExp(`asks to connect`), 10000);
  await sleep(800);
  dev(960, 0, 1200);  // remote_ask.png (the 允許 dialog)
  for (const f of fs.readdirSync(path.join(tmpDir, 'pmshots'))) if (/ask/.test(f)) fs.copyFileSync(path.join(tmpDir, 'pmshots', f), path.join(outDir, `ask_${g.name}.png`));
  dev(961, how, 800);
  const frame = await waitFor(() => t.ev('window.__pmFirstFrame ? 1 : 0'), 15000);
  return {t, asked: !!asked, connected: !!frame};
}

const out = {};
try {
  await waitLog(/常駐外網 ready at/, 20000);
  await sleep(2000);
  out.windows = onScreen(app.pid);
  const A = await makeGuest(0);          // 用一次, 可操作
  const B = await makeGuest(2 | 4);      // 24 小時, 僅觀看
  out.made = {A: A.name, B: B.name, passwordsShown: !!(A.pw && B.pw), pwFormat: [A.pw, B.pw].every(p => /^[A-HJ-KM-NP-Z2-9]{6}$/.test(p || ''))};
  const file = fs.readFileSync(path.join(dataDir, 'remote_guests.txt'), 'utf8');
  out.file = {rows: file.split('\n').filter(l => l && !l.startsWith('#')).length, hashes: (file.match(/pbkdf2-sha256\$/g) || []).length};
  await startEdge();
  // A: 用一次 -> 允許 asked; "記住" asked for too but never kept; 連線紀錄 has its name; gone after its session
  {
    const s = await guestSession(A, 3);
    out.A = {asked: s.asked, connected: s.connected};
    await sleep(1500);
    out.A.connLog = fs.readFileSync(path.join(dataDir, 'remote_connections.log'), 'utf8').split('\n').filter(l => l.includes(`[${A.name}]`));
    out.A.remembered = fs.existsSync(path.join(dataDir, 'remote_devices.txt')) && fs.readFileSync(path.join(dataDir, 'remote_devices.txt'), 'utf8').trim().length > 0;
    out.A.rememberedLogged = /ALLOWED[^\n]*\(remembered\)/.test(readLog());
    await s.t.close();
    out.A.gone = !!(await waitLog(new RegExp(`臨時帳號 ${A.name} was used once: deleted`), 10000));
    dev(986, 0, 600);
    out.A.after = ((readLog().match(/dev guests: [^\n]*/g) || []).slice(-1)[0] || '');
  }
  // B: 僅觀看; 刪除 on the PC while it is connected -> its session ends at once
  {
    const s = await guestSession(B, 1);
    out.B = {asked: s.asked, connected: s.connected, viewOnly: await s.t.ev('S.viewOnly')};
    await sleep(1500);
    const t0 = Date.now();
    dev(C.GuestDel0, 0, 300);  // row 0 is B now (A is gone)
    const ended = await waitLog(new RegExp(`its 臨時帳號 ${B.name} was deleted`), 8000);
    out.B.endedOnThePc = !!ended;
    out.B.pageBye = await waitFor(() => s.t.ev('S.bye || ""'), 5000);
    out.B.seconds = +((Date.now() - t0) / 1000).toFixed(1);
    await s.t.close();
  }
  dev(986, 0, 600);
  out.guestsAtEnd = ((readLog().match(/dev guests: [^\n]*/g) || []).slice(-1)[0] || '');
  // no password anywhere in the data folder (log, settings, accounts, 連線紀錄)
  const leaks = [];
  for (const f of fs.readdirSync(dataDir)) {
    const p = path.join(dataDir, f);
    if (!fs.statSync(p).isFile()) continue;
    const s = fs.readFileSync(p, 'utf8');
    for (const pw of [A.pw, B.pw, OWNER]) if (pw && (s.includes(pw) || s.includes(pw.toLowerCase()) || s.includes(pw.slice(0, 3) + ' ' + pw.slice(3)))) leaks.push(`${f}: ${pw === OWNER ? 'owner' : 'guest'}`);
  }
  out.leaks = leaks;
  out.windowsAtEnd = onScreen(app.pid);
} catch (e) { out.error = String(e && e.stack || e); }
try { if (ws) await cdp('Browser.close'); } catch {}
for (let i = 0; i < 50 && edge && edge.exitCode === null; i++) await sleep(100);
if (edge && edge.exitCode === null) try { execFileSync('taskkill', ['/PID', String(edge.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
dev(C.Exit, 0, 300);
for (let i = 0; i < 100 && app.exitCode === null; i++) await sleep(100);
if (app.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} out.killed = true; }
await sleep(800);
try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
fs.copyFileSync(path.join(dataDir, 'phonemirror.log'), path.join(outDir, 'app.log'));
const checks = [
  ['no window of the app on any monitor', out.windows && out.windows.on === 0 && out.windowsAtEnd && out.windowsAtEnd.on === 0],
  ['two accounts made in the window, passwords shown once (6, no 0/O/1/I/L)', out.made && out.made.passwordsShown && out.made.pwFormat],
  ['remote_guests.txt: 2 rows, 2 hashes', out.file && out.file.rows === 2 && out.file.hashes === 2],
  ['用一次: 允許 asked, connected', out.A && out.A.asked && out.A.connected],
  ['用一次: never remembered (記住 was asked for)', out.A && out.A.remembered === false && out.A.rememberedLogged === false],
  ['連線紀錄 names the account', out.A && out.A.connLog && out.A.connLog.length > 0],
  ['用一次: deleted after its session', out.A && out.A.gone],
  ['僅觀看 guest: 允許 asked, connected, the page is view-only', out.B && out.B.asked && out.B.connected && out.B.viewOnly === true],
  ['刪除 while connected: its session ends at once (< 3 s)', out.B && out.B.endedOnThePc && out.B.pageBye === 'kicked' && out.B.seconds < 3],
  ['no accounts left', /dev guests: 0/.test(out.guestsAtEnd || '')],
  ['no password in any file of the data folder (log included)', out.leaks && out.leaks.length === 0],
];
out.checks = checks.map(([n, ok]) => `${ok ? 'PASS' : 'FAIL'} ${n}`);
fs.writeFileSync(path.join(outDir, 'app_guest.json'), JSON.stringify(out, null, 1));
try { fs.rmSync(tmpDir, {recursive: true, force: true}); } catch {}
for (const c of out.checks) console.log(c);
process.exit(checks.every(c => c[1]) ? 0 : 1);
