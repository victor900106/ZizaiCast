// 0.8.3.3 before / after screenshots of every changed screen, per language (docs/remote.md "0.8.3.3").
//   node tools/shots_0833.mjs <自在投影.exe> <outDir> <before|after> [--langs zh-TW,en,ja,ko] [--only ABCDFGP] [--web <remote/web dir>]
// Each run: its own --dev --test-offscreen --test-no-network --demo-branding instance, data folder
// (PhoneMirror-dev-shots0833) and TEMP; PM_REMOTE_LOOPBACK (127.0.0.2 / .3 only: no firewall prompt),
// PM_REMOTE_DRYRUN (the phone's input only logged), made-up fixed / temporary / LAN addresses
// (PM_REMOTE_FUNNELFAKE / TUNNELFAKE / SHOWLAN: tailscale and cloudflared are never run). Every window
// of the instance is checked against every monitor after start and before quit (the own PID is killed
// if one is on a screen). The phone page: headless Chrome (own profile, 127.0.0.x or file:// only).
// Run B (after): the fixed address's TLS check goes to a FAKE local TLS server on 127.0.0.1
// (PM_REMOTE_FUNNELPROBE) that drops every connection for the first 9 s, as Tailscale does while the
// certificate is being issued.
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import net from 'node:net';
import tls from 'node:tls';
import {headlessShell, watchAltTab} from '../remote/tools/quiet_browser.mjs';

const [exe, outRoot, which, ...rest] = process.argv.slice(2);
if (!exe || !outRoot || !/^(before|after)$/.test(which || '')) {
  console.error('usage: node tools/shots_0833.mjs <exe> <outDir> <before|after> [--langs ..] [--only ..] [--web dir]');
  process.exit(1);
}
let langs = ['zh-TW', 'en', 'ja', 'ko'], only = 'ABCDEFGHP', webDir = null;
for (let i = 0; i < rest.length; i++) {
  if (rest[i] === '--langs') langs = rest[++i].split(',');
  else if (rest[i] === '--only') only = rest[++i];
  else if (rest[i] === '--web') webDir = rest[++i];
}
const after = which === 'after';
const HERE = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const REPO = path.resolve(HERE, '..');
const WORK = process.env.UX_WORK || path.join(process.env.TEMP, 'zizai-shots0833');
const FEED = path.join(WORK, 'feed');
const outDir = path.resolve(outRoot, which);
fs.mkdirSync(outDir, {recursive: true});
const DEVCMD = path.join(REPO, 'share', 'tools', 'dev_cmd.ps1');
const CHROME = 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const sleep = ms => new Promise(r => setTimeout(r, ms));
const LAN = 'http://203.0.113.10:18571/AbCdEfGhJkMnPqRsTuVwXyZ234567892/';
const FUNNEL = 'https://example-pc.example.ts.net:8443/';
const TUNNEL = 'https://example-demo.example.com';
const C = {Toggle: 330, Panel: 331, Tunnel: 352, FixedToggle: 356, TabRemote: 383, GuestNew: 440, Pair: 127};
const salt = crypto.randomBytes(16);
const pwHash = `pbkdf2-sha256$310000$${salt.toString('hex')}$${crypto.pbkdf2Sync('Zizai-Test-2026', salt, 310000, 32, 'sha256').toString('hex')}`;
const log = [];
const say = s => { console.log(s); log.push(s); };

function windows(pid) {
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
  try { const [n, on] = execFileSync('powershell', ['-NoProfile', '-Command', ps], {encoding: 'utf8', windowsHide: true}).trim().split(/\s+/).map(Number); return {n, on}; }
  catch { return {n: -1, on: -1}; }
}

// A fake "Funnel" for the TLS check: 127.0.0.1 only, a self-signed certificate made here (the app
// accepts it only for this --dev probe address). Connections are dropped until readyAt.
let fakeTls = null;
function startFakeTls(dropMs) {
  const key = path.join(WORK, 'fake_key.pem'), cert = path.join(WORK, 'fake_cert.pem');
  if (!fs.existsSync(cert))
    execFileSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', cert, '-days', '2', '-subj', '/CN=example-pc.example.ts.net'], {stdio: 'ignore'});
  const ts = tls.createServer({key: fs.readFileSync(key), cert: fs.readFileSync(cert)}, s => {
    s.on('data', () => { s.end('HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nnot found'); });
    s.on('error', () => {});
  });
  const st = {readyAt: Date.now() + dropMs, dropped: 0, served: 0};
  const srv = net.createServer(sock => {
    sock.on('error', () => {});
    if (Date.now() < st.readyAt) { st.dropped++; sock.destroy(); return; }
    st.served++;
    ts.emit('connection', sock);
  }).listen(0, '127.0.0.1');
  return new Promise(r => srv.on('listening', () => { st.port = srv.address().port; st.close = () => srv.close(); r(st); }));
}

async function run(lang, tag, {seed = '', env = {}, args = [], steps}) {
  const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-shots0833');
  const tmpDir = path.join(WORK, '_tmp');
  const shotsDir = path.join(tmpDir, 'pmshots');
  fs.rmSync(dataDir, {recursive: true, force: true});
  fs.mkdirSync(dataDir, {recursive: true});
  fs.rmSync(tmpDir, {recursive: true, force: true});
  fs.mkdirSync(shotsDir, {recursive: true});
  fs.writeFileSync(path.join(dataDir, 'settings.ini'),
    `[settings]\nlanguage=${lang}\nremote_port=${18500 + Math.floor(Math.random() * 300)}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\n` +
    `toolbar_hint_shown=1\nright_click_hint_shown=1\nwhats_new_seen=0.8.3\nmiracast=0\nandroid_auto=0\n${seed}`);
  const fullEnv = {...process.env, TEMP: tmpDir, TMP: tmpDir, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: 'shots0833', PM_REMOTE_DRYRUN: '1',
    PM_REMOTE_TESTCLIP: '1', PM_REMOTE_NOTS: '1', PM_REMOTE_LOOPBACK: '1', PM_SHARE_BIND: '127.0.0.1', PM_REMOTE_FUNNELFAKE: FUNNEL,
    PM_REMOTE_SHOWLAN: LAN, PM_REMOTE_TUNNELFAKE: TUNNEL, PM_TEST_GUEST_OUT: path.join(tmpDir, 'copy.txt'),
    PM_TEST_GUEST_PW: 'ABC123', ...env};  // rel-0835: the made-up 臨時帳號 password in the pictures (「ABC 123」), --dev only
  for (const k of Object.keys(fullEnv)) if (fullEnv[k] === null) delete fullEnv[k];
  const app = spawn(exe, ['--dev', '--test-offscreen', '--test-no-network', '--demo-branding', ...args], {stdio: 'ignore', env: fullEnv, windowsHide: true});
  const kill = () => { try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} };
  const dev = (cmd, lp = 0, wait = 700) => { try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', DEVCMD, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe', windowsHide: true}); } catch {} };
  const readLog = () => { try { return fs.readFileSync(path.join(dataDir, 'phonemirror.log'), 'utf8'); } catch { return ''; } };
  const waitLog = async (re, ms) => { const end = Date.now() + ms; while (Date.now() < end) { const m = readLog().match(re); if (m) return m; await sleep(200); } return null; };
  const grab = name => {
    const got = [];
    for (const f of fs.readdirSync(shotsDir)) {
      if (!/\.png$/.test(f)) continue;
      const dst = path.join(outDir, `${lang}_${tag}${name}_${f}`);
      fs.copyFileSync(path.join(shotsDir, f), dst);
      fs.rmSync(path.join(shotsDir, f));
      got.push(path.basename(dst));
    }
    if (!got.length) say(`${lang} ${tag}${name}: (nothing)`);
    return got;
  };
  const win = async (name, wait = 1000) => { dev(910, 0, wait); return grab(name); };
  const check = when => {
    const w = windows(app.pid);
    say(`${lang} ${tag} ${when}: windows ${w.n}, on a monitor ${w.on}`);
    if (w.on !== 0) { kill(); throw new Error('a window on a monitor: own PID killed'); }
  };
  try {
    await waitLog(/remote: |idle|ready/, 6000);
    await sleep(3500);
    check('start');
    await steps({dev, grab, win, waitLog, readLog, check, pid: app.pid});
    check('end');
  } catch (e) { say(`${lang} ${tag} ERROR ${e.message}`); }
  dev(104, 0, 500);
  for (let i = 0; i < 100 && app.exitCode === null; i++) await sleep(100);
  if (app.exitCode === null) { kill(); say(`${lang} ${tag}: quit did not end it, own PID killed`); }
  fs.mkdirSync(path.join(outRoot, 'logs'), {recursive: true});
  try { fs.copyFileSync(path.join(dataDir, 'phonemirror.log'), path.join(outRoot, 'logs', `${which}_${lang}_${tag}.log`)); } catch {}
  return readLog;
}

// Minimal headless browser over CDP (own profile, debugging port on 127.0.0.1). rel-0834: chrome-headless-shell
// (remote/tools/quiet_browser.mjs: no window at all, no Alt+Tab / taskbar entry, never the focus; `chrome
// --headless=new` showed blank windows in Alt+Tab), watched by alttab_watch.ps1: its count is logged, must be 0.
async function browser({width = 412, height = 860, dpr = 2} = {}) {
  const port = 9300 + Math.floor(Math.random() * 400);
  const proc = spawn(headlessShell(), ['--disable-gpu', '--hide-scrollbars', '--no-first-run', '--mute-audio', `--user-data-dir=${WORK}/_chrome_cdp`,
    `--remote-debugging-port=${port}`, '--remote-debugging-address=127.0.0.1', 'about:blank'], {stdio: 'ignore', windowsHide: true});
  const watch = watchAltTab([proc.pid], path.join(WORK, `_alttab_${proc.pid}.txt`));
  let tabs;
  for (let i = 0; i < 60; i++) { try { tabs = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json(); if (tabs.length) break; } catch {} await sleep(250); }
  const ws = new WebSocket(tabs.find(t => t.type === 'page').webSocketDebuggerUrl);
  await new Promise(r => ws.addEventListener('open', r));
  let id = 0; const pending = new Map();
  ws.addEventListener('message', e => { const m = JSON.parse(e.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } });
  const send = (method, params = {}) => new Promise(r => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({id: i, method, params})); });
  await send('Page.enable');
  await send('Emulation.setDeviceMetricsOverride', {width, height, deviceScaleFactor: dpr, mobile: true});
  return {
    send,
    go: async (url, wait = 3000) => { await send('Page.navigate', {url}); await sleep(wait); },
    eval: async expr => { const r = await send('Runtime.evaluate', {expression: expr, awaitPromise: true, returnByValue: true}); return r.result && r.result.result && r.result.result.value; },
    shot: async out => { const r = await send('Page.captureScreenshot', {format: 'png'}); fs.writeFileSync(out, Buffer.from(r.result.data, 'base64')); },
    close: async () => {
      try { await send('Browser.close'); } catch {}
      await sleep(500);
      try { execFileSync('taskkill', ['/PID', String(proc.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
      const w = await watch.stop();
      say(`browser ${proc.pid}: Alt+Tab watcher ${w.samples} samples, max ${w.max}${w.seen.length ? ' ' + w.seen.join(' | ') : ''}`);
    },
  };
}
const BL = {'zh-TW': 'zh-TW', en: 'en-US', ja: 'ja-JP', ko: 'ko-KR'};
const LN = {'zh-TW': 0, en: 1, ja: 2, ko: 3};
const pwSeed = `remote_pw_hash=${pwHash}\nremote_fixed_secret=${crypto.randomBytes(32).toString('hex')}\n`;

for (const lang of langs) {
  if (only.includes('A')) await run(lang, 'A', {seed: pwSeed + 'idle_tab=remote\nremote_persistent=1\n', steps: async ({dev, grab, win, waitLog, readLog}) => {
    await waitLog(/常駐外網 ready at|固定網址 ready at|fixed address ready at/, 20000);
    await sleep(1500);
    dev(985, 96, 1500);
    dev(C.TabRemote, 0, 1500); await win('1-remote-allow');
    dev(963, 8, 900); grab('2-advanced');
    dev(C.GuestNew, 0, 1000); dev(982, 96, 900); grab('3-guest-form');
    dev(981, 2 | 8 | 16, 1200); dev(982, 96, 900); grab('4-guest-made');  // rel-0835: 可以傳檔 ticked, the 0.8.3.5 default
    dev(984, 0, 600);
    dev(C.GuestNew, 0, 1000); dev(981, 0 | 4 | 8 | 16, 1200); dev(984, 0, 900);
    await sleep(1200); await win('5-remote-guests');
    const fixed = (readLog().match(/listening on (127\.0\.0\.2:\d+)/) || [])[1];
    if (fixed) {
      const b = await browser();
      try {
        await b.send('Network.enable'); await b.send('Network.setExtraHTTPHeaders', {headers: {'Accept-Language': BL[lang]}});
        await b.go(`http://${fixed}/`, 3000); await b.shot(path.join(outDir, `${lang}_A6-phone-login.png`));
        await b.go('about:blank', 300);
        await b.go(`http://${fixed}/#u=guest-4821`, 3000); await b.shot(path.join(outDir, `${lang}_A6b-phone-login-guest.png`));
      } finally { await b.close(); }
    } else say(`${lang} A: no fixed origin in the log`);
    dev(C.Panel, 0, 1800); dev(C.Tunnel, 0, 3500); dev(960, 0, 1500); grab('7-panel');
    dev(963, 5, 900); grab('8-panel-fixed');
  }});
  if (only.includes('B')) {
    const fk = after ? await startFakeTls(9000) : null;
    await run(lang, 'B', {seed: pwSeed, env: fk ? {PM_REMOTE_FUNNELPROBE: `https://127.0.0.1:${fk.port}/`} : {}, steps: async ({dev, win, waitLog}) => {
      dev(985, 96, 1500);
      dev(C.TabRemote, 0, 1200); dev(C.Toggle, 0, 2500);
      if (fk) fk.readyAt = Date.now() + 15000;  // dropped for 15 s from the moment the fixed address starts
      dev(C.FixedToggle, 0, 300); await sleep(5000); await win('1-fixed-start', 300);
      if (fk) {
        await waitLog(/固定網址 reachable|fixed address reachable/, 30000);
        await sleep(800); await win('2-fixed-ready', 300);
        say(`${lang} B: fake TLS dropped ${fk.dropped}, served ${fk.served}`);
      } else { await sleep(4000); await win('2-fixed-later'); }
    }});
    if (fk) fk.close();
  }
  if (only.includes('C')) await run(lang, 'C', {seed: pwSeed, env: {PM_REMOTE_FUNNELFAKE: null, PM_REMOTE_FUNNELFAIL: '1'}, steps: async ({dev, win}) => {
    dev(985, 96, 1500);
    dev(C.TabRemote, 0, 1200); dev(C.Toggle, 0, 2500); dev(C.FixedToggle, 0, 4000); await win('1-fixed-failed');
  }});
  if (only.includes('D')) await run(lang, 'D', {steps: async ({dev, grab, win}) => {
    dev(985, 96, 1500);
    dev(950, 1, 900); await win('1-idle');
    dev(C.Pair, 0, 2500); dev(902, 0, 1200); grab('2-pair');
    if (after) { dev(987, 1, 900); dev(902, 0, 1200); grab('3-pair-steps'); }
  }});
  if (only.includes('E')) await run(lang, 'E', {steps: async ({dev, win}) => {  // the page for a new user: off, then 在家 QR
    dev(985, 96, 1500);
    dev(C.TabRemote, 0, 1200); await win('1-remote-off');
    dev(359, 0, 3500); await win('2-remote-lan');
  }});
  // The card when a new tailnet has Funnel off: the FAKE tailscale CLI (pm_fake_tailscale.exe next to the exe;
  // tailscale itself is never run) prints an enable page and waits.
  if (only.includes('H') && after) {
    const fake = path.join(path.dirname(exe), 'pm_fake_tailscale.exe');
    const hdir = path.join(WORK, '_fake_ts');
    fs.mkdirSync(hdir, {recursive: true});
    await run(lang, 'H', {seed: pwSeed + 'remote_persistent=1\n', env: {PM_REMOTE_FUNNELFAKE: null, PM_REMOTE_FAKECLI: fake, PM_FAKE_TS_MODE: 'enable-hang',
      PM_FAKE_TS_PIDS: path.join(hdir, 'pids.txt'), PM_FAKE_TS_STATE: path.join(hdir, 'state.txt')}, steps: async ({dev, win, waitLog}) => {
      await waitLog(/Funnel failed/, 25000);
      dev(985, 96, 1500); dev(C.TabRemote, 0, 1200); await win('1-fixed-enable');
    }});
  }
  // The launch material's remote-control pictures (README / landing page): the window at 150 %.
  if (only.includes('L') && after) await run(lang, 'L', {seed: pwSeed + 'idle_tab=remote\nremote_persistent=1\n', steps: async ({dev, grab, win, waitLog}) => {
    await waitLog(/常駐外網 ready at/, 20000);
    await sleep(1200);
    dev(985, 144, 1500);
    dev(C.TabRemote, 0, 1500); await win('1-launch-allow');
    dev(359, 0, 3500); await win('2-launch-lan');
    dev(C.Panel, 0, 1800); dev(C.Tunnel, 0, 3500); dev(960, 0, 1500); grab('3-launch-panel');
  }});
  if (only.includes('F')) await run(lang, 'F', {args: ['--test-feed', path.join(FEED, 'phone_en.h264')], steps: async ({dev, grab, win}) => {
    await sleep(2500);
    dev(107, 0, 900); await win('1-snapshot', 300);
    await sleep(3500);
    dev(160, 0, 2500); dev(903, 0, 1000); grab('2-picker');
    dev(164, 0, 600);
    dev(191, 0, 4000); await win('3-translate');
  }});
  if (only.includes('G')) await run(lang, 'G', {args: ['--test-feed', path.join(FEED, 'phone_en.h264'), '--test-source', 'android'],
    env: {PM_DROP_TEST: FEED, PM_DROP_FAKE_OK: '1'}, steps: async ({dev, win}) => {
    await sleep(2500);
    dev(977, 0, 700); await win('1-drop-folder', 300);
  }});
}

// The phone page from file:// (a copy of remote/web, a made-up PC picture, no server): the toolbar and
// the settings sheet.
if (only.includes('P')) {
  const src = webDir || path.join(REPO, 'remote', 'web');
  const dir = path.join(WORK, `_phone_${which}`);
  fs.rmSync(dir, {recursive: true, force: true});
  fs.mkdirSync(dir, {recursive: true});
  for (const f of fs.readdirSync(src)) fs.copyFileSync(path.join(src, f), path.join(dir, f));
  fs.copyFileSync(path.join(FEED, 'desk.png'), path.join(dir, 'desk.png'));
  const page = fs.readFileSync(path.join(dir, 'index.html'), 'utf8');
  const b = await browser({width: 412, height: 860});
  try {
    for (const lang of langs) {
      fs.writeFileSync(path.join(dir, `p_${lang}.html`), page.replace('window.PM = /*PM_CONFIG*/{};', `window.PM = {lang: ${LN[lang]}, audio: true};`));
      await b.go(`file:///${dir.replace(/\\/g, '/')}/p_${lang}.html`, 2500);
      await b.eval(`(() => {
        document.getElementById('msg').style.display = 'none';
        for (const id of ['v', 'scr']) document.getElementById(id).style.display = 'none';
        document.body.style.background = '#111 url(desk.png) center 38% / 100% auto no-repeat';
        const bar = document.getElementById('bar'); bar.style.display = 'flex'; bar.style.opacity = '1';
        return 1; })()`);
      await sleep(600);
      await b.shot(path.join(outDir, `${lang}_P1-phone-bar.png`));
      await b.eval(`(() => { document.getElementById('bSet').click(); const s = document.getElementById('set'); s.style.display = 'block'; return 1; })()`);
      await sleep(600);
      await b.shot(path.join(outDir, `${lang}_P2-phone-settings.png`));
    }
  } finally { await b.close(); }
}
fs.writeFileSync(path.join(outRoot, `${which}_shots.txt`), log.join('\n') + '\n');
