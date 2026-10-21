// 0.8.3.4 傳檔 / Android 組 before / after screenshots + checks (C2 傳到手機, B6 配對逾時, F3 重開機提示, D3 小視窗分頁).
//   node tools/shots_0834_share.mjs <自在投影.exe> <outDir> <before|after> [--langs zh-TW,en,ja,ko] [--only CBDF] [--work dir]
// Each run: its own --dev --test-offscreen --test-no-network instance (data folder PhoneMirror-dev-share0834, own TEMP),
// PM_SHARE_BIND 127.0.0.1 (no firewall prompt), PM_REMOTE_* fakes (tailscale / cloudflared never run), no adb ever started.
// Every window of the instance is checked against every monitor after start and before quit; the own PID is killed
// if one is on a screen. Nothing makes a sound; no real keyboard / mouse input is sent (window messages only).
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

const [exe, outRoot, which, ...rest] = process.argv.slice(2);
if (!exe || !outRoot || !/^(before|after)$/.test(which || '')) {
  console.error('usage: node tools/shots_0834_share.mjs <exe> <outDir> <before|after> [--langs ..] [--only CBDF] [--work dir]');
  process.exit(1);
}
let langs = ['zh-TW', 'en', 'ja', 'ko'], only = 'CBDF', work = null;
for (let i = 0; i < rest.length; i++) {
  if (rest[i] === '--langs') langs = rest[++i].split(',');
  else if (rest[i] === '--only') only = rest[++i];
  else if (rest[i] === '--work') work = rest[++i];
}
const HERE = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const REPO = path.resolve(HERE, '..');
const WORK = work || path.resolve(REPO, '..', 'ux0834share_work');
const FEED = path.join(WORK, 'feed');
const outDir = path.resolve(outRoot, which);
fs.mkdirSync(outDir, {recursive: true});
const DEVCMD = path.join(REPO, 'share', 'tools', 'dev_cmd.ps1');
const sleep = ms => new Promise(r => setTimeout(r, ms));
const C = {Shot: 107, ShareToolbar: 160, Panel: 331, Pair: 127};
const log = [];
const say = s => { console.log(s); log.push(s); };

function windows(pid) {
  const ps = `Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Collections.Generic;
public static class PmWin4 {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll", EntryPoint="GetWindowLongPtrW")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint c);
  // Alt+Tab shows a visible, unowned top-level window without WS_EX_TOOLWINDOW (or with WS_EX_APPWINDOW).
  public static bool AltTab(IntPtr h) { long ex = (long)GetWindowLongPtr(h, -20); bool tool = (ex & 0x80) != 0, app = (ex & 0x40000) != 0;
    return GetWindow(h, 4) == IntPtr.Zero && (!tool || app); }
  public static List<int[]> Of(uint pid) { var l = new List<int[]>(); EnumWindows((h, x) => { uint p; GetWindowThreadProcessId(h, out p);
    if (p == pid && IsWindowVisible(h)) { RECT r; GetWindowRect(h, out r); l.Add(new int[] { r.L, r.T, r.R, r.B, AltTab(h) ? 1 : 0 }); } return true; }, IntPtr.Zero); return l; }
}
"@
$w = [PmWin4]::Of(${pid}); $on = 0; $alt = 0
foreach ($r in $w) { $alt += $r[4]; foreach ($s in [System.Windows.Forms.Screen]::AllScreens) { $b = $s.Bounds; if ($r[0] -lt $b.Right -and $r[2] -gt $b.Left -and $r[1] -lt $b.Bottom -and $r[3] -gt $b.Top) { $on++ } } }
Write-Output "$($w.Count) $on $alt"`;
  try { const [n, on, alt] = execFileSync('powershell', ['-NoProfile', '-Command', ps], {encoding: 'utf8', windowsHide: true}).trim().split(/\s+/).map(Number); return {n, on, alt}; }
  catch { return {n: -1, on: -1, alt: -1}; }
}

async function run(lang, tag, {seed = '', env = {}, args = [], steps}) {
  const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-share0834');
  const tmpDir = path.join(WORK, '_tmp');
  const shotsDir = path.join(tmpDir, 'pmshots');
  fs.rmSync(dataDir, {recursive: true, force: true});
  fs.mkdirSync(dataDir, {recursive: true});
  fs.rmSync(tmpDir, {recursive: true, force: true});
  fs.mkdirSync(shotsDir, {recursive: true});
  fs.writeFileSync(path.join(dataDir, 'settings.ini'),
    `[settings]\nlanguage=${lang}\nremote_port=${18800 + Math.floor(Math.random() * 300)}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\n` +
    `toolbar_hint_shown=1\nright_click_hint_shown=1\nwhats_new_seen=0.8.3\nmiracast=0\nandroid_auto=0\n${seed}`);
  const fullEnv = {...process.env, TEMP: tmpDir, TMP: tmpDir, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: 'share0834', PM_REMOTE_DRYRUN: '1',
    PM_REMOTE_TESTCLIP: '1', PM_REMOTE_NOTS: '1', PM_REMOTE_LOOPBACK: '1', PM_SHARE_BIND: '127.0.0.1',
    PM_REMOTE_FUNNELFAKE: 'https://example-pc.example.ts.net:8443/', PM_REMOTE_SHOWLAN: 'http://203.0.113.10:18571/AbCdEfGhJkMnPqRsTuVwXyZ234567892/',
    PM_REMOTE_TUNNELFAKE: 'https://example-demo.example.com', ...env};
  for (const k of Object.keys(fullEnv)) if (fullEnv[k] === null) delete fullEnv[k];
  const app = spawn(exe, ['--dev', '--test-offscreen', '--test-no-network', '--demo-branding', ...args], {stdio: 'ignore', env: fullEnv, windowsHide: true});
  const kill = () => { try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} };
  const dev = (cmd, lp = 0, wait = 700) => { try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', DEVCMD, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe', windowsHide: true}); } catch {} };
  const readLog = () => { try { return fs.readFileSync(path.join(dataDir, 'phonemirror.log'), 'utf8'); } catch { return ''; } };
  const waitLog = async (re, ms) => { const end = Date.now() + ms; while (Date.now() < end) { const m = readLog().match(re); if (m) return m; await sleep(200); } return null; };
  const grab = (name, only) => {
    const got = [];
    for (const f of fs.readdirSync(shotsDir)) {
      if (!/\.png$/.test(f) || (only && !only.test(f))) continue;
      const dst = path.join(outDir, `${lang}_${tag}${name}_${f}`);
      fs.copyFileSync(path.join(shotsDir, f), dst);
      fs.rmSync(path.join(shotsDir, f));
      got.push(path.basename(dst));
    }
    if (!got.length) say(`${lang} ${tag}${name}: (nothing)`);
    return got;
  };
  const win = async (name, wait = 1000) => { dev(910, 0, wait); return grab(name, /window\.png$/); };
  const check = when => {
    const w = windows(app.pid);
    say(`${lang} ${tag} ${when}: windows ${w.n}, on a monitor ${w.on}, Alt+Tab-visible ${w.alt}`);
    if (w.on !== 0) { kill(); throw new Error('a window on a monitor: own PID killed'); }
  };
  try {
    await waitLog(/idle|ready|remote: /, 6000);
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

const count = (s, re) => (s.match(re) || []).length;
for (const lang of langs) {
  // C2: one new capture -> 傳到手機 goes straight to the QR panel; two or more -> the picker.
  if (only.includes('C')) await run(lang, 'C', {args: ['--test-feed', path.join(FEED, 'phone_en.h264')], steps: async ({dev, grab, win, waitLog, readLog}) => {
    dev(C.Shot, 0, 1500);
    dev(C.ShareToolbar, 0, 1800);
    dev(903, 0, 900); grab('1-one-capture');
    const l1 = readLog();
    say(`${lang} C one capture: QR page ${count(l1, /share: 1 file\(s\) → QR page/g)}, picker ${count(l1, /share: picker with/g)}`);
    dev(C.Shot, 0, 1500); dev(C.Shot, 0, 1500);
    dev(C.ShareToolbar, 0, 1800);
    dev(903, 0, 900); grab('2-two-captures');
    const l2 = readLog();
    say(`${lang} C two captures: picker ${count(l2, /share: picker with \d+ capture/g)}`);
  }});
  // B6: the QR waits; after --test-pair-timeout S the hint, later the wait ends with a retry button.
  if (only.includes('B')) await run(lang, 'B', {args: ['--test-pair-timeout', '4'], steps: async ({dev, grab, readLog}) => {
    dev(C.Pair, 0, 1500);
    dev(902, 0, 700); grab('1-waiting', /pair\.png$/);
    await sleep(3500);
    dev(902, 0, 700); grab('2-hint', /pair\.png$/);
    await sleep(9500);
    dev(902, 0, 700); grab('3-ended', /pair\.png$/);
    dev(991, 0, 900); dev(902, 0, 700); grab('4-retried', /pair\.png$/);
    const l = readLog();
    say(`${lang} B log: ${(l.match(/pairing: [^\n]*/g) || []).join(' | ')}`);
  }});
  // D3: the 遠端控制 small window.
  if (only.includes('D')) await run(lang, 'D', {args: ['--test-feed', path.join(FEED, 'phone_en.h264')], steps: async ({dev, grab}) => {
    dev(C.Panel, 0, 1800); dev(960, 0, 1500); grab('1-remote-panel', /^remote_panel/);
  }});
  // F3: a paired phone is not found -> the toast about 無線偵錯 after a reboot (990: a made-up "none found" report).
  if (only.includes('F')) await run(lang, 'F', {seed: 'android_paired=1\n', steps: async ({dev, grab, win, readLog}) => {
    dev(990, 0, 1200); await win('1-hint');
    dev(990, 0, 800);
    const l = readLog();
    say(`${lang} F hint shown ${count(l, /reboot hint shown/g)} time(s) after 2 reports`);
  }});
}
fs.writeFileSync(path.join(outRoot, `${which}_shots_0834_share.txt`), log.join('\n') + '\n');
