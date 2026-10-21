// 0.8.3.4 畫面組 before / after screenshots of the live picture window (toolbar auto-hide, 凍結 / 放大鏡
// badges, the clickable screenshot toast, the 畫筆模式 badge), per language.
//   node tools/shots_0834v.mjs <自在投影.exe> <outDir> <before|after> --feed <portrait.h264> [--land <landscape.h264>]
//        [--langs zh-TW,en,ja,ko] [--only TBSIN]
// Each run: its own --dev --test-offscreen --test-no-network --demo-branding instance (window at x = -12000,
// PM_VIDEO_OFFSCREEN), its own data folder (PhoneMirror-dev-shots0834v) and TEMP; the phone is a looped
// --test-feed stream (a copy of the given file). Mouse input is POSTED to the instance's own window
// (PhoneMirror.Video.Test 12: no leave tracking): the real pointer never moves, no key is sent. Every
// window of the instance is checked against every monitor after start and before quit (the own PID is
// killed if one is on a screen). Opening the screenshot is logged (PM_TEST_OPEN_OUT), never done.
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

const [exe, outRoot, which, ...rest] = process.argv.slice(2);
if (!exe || !outRoot || !/^(before|after)$/.test(which || '')) {
  console.error('usage: node tools/shots_0834v.mjs <exe> <outDir> <before|after> --feed f.h264 [--land l.h264] [--langs ..] [--only ..]');
  process.exit(1);
}
let langs = ['zh-TW', 'en', 'ja', 'ko'], only = 'TBSIN', feedSrc = null, landSrc = null;
for (let i = 0; i < rest.length; i++) {
  if (rest[i] === '--langs') langs = rest[++i].split(',');
  else if (rest[i] === '--only') only = rest[++i];
  else if (rest[i] === '--feed') feedSrc = rest[++i];
  else if (rest[i] === '--land') landSrc = rest[++i];
}
const after = which === 'after';
const HERE = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const REPO = path.resolve(HERE, '..');
const WORK = process.env.UX_WORK || path.join(process.env.TEMP, 'zizai-shots0834v');
const outDir = path.resolve(outRoot, which);
fs.mkdirSync(outDir, {recursive: true});
fs.mkdirSync(WORK, {recursive: true});
const feed = path.join(WORK, 'feed.h264'), land = path.join(WORK, 'land.h264');
if (feedSrc) fs.copyFileSync(feedSrc, feed);
if (landSrc) fs.copyFileSync(landSrc, land);
const DEVCMD = path.join(REPO, 'share', 'tools', 'dev_cmd.ps1');
const sleep = ms => new Promise(r => setTimeout(r, ms));
const log = [];
const say = s => { console.log(s); log.push(s); };
const C = {Snapshot: 107, MagCycle: 183, ZoomReset: 182, Freeze: 190, Ink: 400, ZoomIn: 180};

const PS_WIN = `
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Collections.Generic; using System.Text;
public static class PmWin4 {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern uint RegisterWindowMessage(string s);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  public static List<int[]> Of(uint pid) { var l = new List<int[]>(); EnumWindows((h, x) => { uint p; GetWindowThreadProcessId(h, out p);
    if (p == pid && IsWindowVisible(h)) { RECT r; GetWindowRect(h, out r); l.Add(new int[] { r.L, r.T, r.R, r.B }); } return true; }, IntPtr.Zero); return l; }
  public static IntPtr Video(uint pid) { IntPtr f = IntPtr.Zero; EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); var sb = new StringBuilder(128); GetClassName(h, sb, 128);
    if (p == pid && sb.ToString() == "PhoneMirrorVideoWindow") { f = h; return false; } return true; }, IntPtr.Zero); return f; }
}
"@`;

// Visible windows of the instance, how many are on a monitor and how many Alt+Tab / the taskbar would
// show (tools/alttab_check.ps1: visible, top-level, not a tool window, unowned or APPWINDOW, not cloaked).
const ALTTAB = path.join(REPO, 'tools', 'alttab_check.ps1');
function windows(pid) {
  try {
    const out = execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ALTTAB, '-ProcessId', String(pid)], {encoding: 'utf8', windowsHide: true});
    const m = out.match(/alttab \d+ (\d+) visible (\d+) onscreen (\d+)/);
    if (!m) return {n: -1, on: -1, alt: -1, raw: out.trim()};
    return {alt: Number(m[1]), n: Number(m[2]), on: Number(m[3]), raw: out.trim()};
  } catch { return {n: -1, on: -1, alt: -1, raw: ''}; }
}

// Posted mouse messages / test hooks to the instance's own video window. ops: [[kind, a, b], ...]
//   ['synth'] no leave tracking; ['move', fx, fy] WM_MOUSEMOVE at a fraction of the client;
//   ['movepx', x, y]; ['click', x, y] (client px); ['leave']; ['size', w, h] (client DIPs, window stays off-screen);
//   ['toast'] -> prints the clickable toast's centre (test hook 19); ['sleep', ms].
function vw(pid, ops) {
  let body = '';
  for (const [k, a, b] of ops) {
    if (k === 'synth') body += `[PmWin4]::SendMessage($h, $t, [IntPtr]12, [IntPtr]1) | Out-Null\n`;
    else if (k === 'move') body += `[PmWin4]::GetClientRect($h, [ref]$cr) | Out-Null; $x = [int]($cr.R * ${a}); $y = [int]($cr.B * ${b}); [PmWin4]::PostMessage($h, 0x200, [IntPtr]0, [IntPtr](($y -shl 16) -bor $x)) | Out-Null\n`;
    else if (k === 'movepx') body += `[PmWin4]::PostMessage($h, 0x200, [IntPtr]0, [IntPtr](((${b}) -shl 16) -bor (${a}))) | Out-Null\n`;
    else if (k === 'click') body += `$lp = [IntPtr](((${b}) -shl 16) -bor (${a})); [PmWin4]::PostMessage($h, 0x200, [IntPtr]0, $lp) | Out-Null; Start-Sleep -Milliseconds 120; [PmWin4]::PostMessage($h, 0x201, [IntPtr]1, $lp) | Out-Null; Start-Sleep -Milliseconds 80; [PmWin4]::PostMessage($h, 0x202, [IntPtr]0, $lp) | Out-Null\n`;
    else if (k === 'leave') body += `[PmWin4]::PostMessage($h, 0x2A3, [IntPtr]0, [IntPtr]0) | Out-Null\n`;
    else if (k === 'size') body += `[PmWin4]::GetWindowRect($h, [ref]$wr) | Out-Null; [PmWin4]::GetClientRect($h, [ref]$cr) | Out-Null; $fw = ($wr.R - $wr.L) - $cr.R; $fh = ($wr.B - $wr.T) - $cr.B; $k = [PmWin4]::GetDpiForWindow($h) / 96.0; [PmWin4]::SetWindowPos($h, [IntPtr]0, 0, 0, [int](${a} * $k) + $fw, [int](${b} * $k) + $fh, 0x16) | Out-Null\n`;
    else if (k === 'toast') body += `$r = [PmWin4]::SendMessage($h, $t, [IntPtr]19, [IntPtr]0).ToInt64(); Write-Output "toast $r"\n`;
    else if (k === 'sleep') body += `Start-Sleep -Milliseconds ${a}\n`;
  }
  const ps = `${PS_WIN}
[PmWin4]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null  # per-monitor aware: posted coordinates are not rescaled
$h = [PmWin4]::Video(${pid}); if ($h -eq [IntPtr]::Zero) { Write-Output "no window"; exit 1 }
$t = [PmWin4]::RegisterWindowMessage("PhoneMirror.Video.Test"); $cr = New-Object PmWin4+RECT; $wr = New-Object PmWin4+RECT
${body}`;
  try { return execFileSync('powershell', ['-NoProfile', '-Command', ps], {encoding: 'utf8', windowsHide: true}).trim(); }
  catch (e) { return 'error ' + e.message; }
}

async function run(lang, tag, {seed = '', env = {}, args = [], steps}) {
  const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-shots0834v');
  const tmpDir = path.join(WORK, '_tmp');
  const shotsDir = path.join(tmpDir, 'pmshots');
  fs.rmSync(dataDir, {recursive: true, force: true});
  fs.mkdirSync(dataDir, {recursive: true});
  fs.rmSync(tmpDir, {recursive: true, force: true});
  fs.mkdirSync(shotsDir, {recursive: true});
  fs.writeFileSync(path.join(dataDir, 'settings.ini'),
    `[settings]\nlanguage=${lang}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\n` +
    `toolbar_hint_shown=1\nright_click_hint_shown=1\nwhats_new_seen=0.8.3\nmiracast=0\nandroid_auto=0\nremote_enabled=0\n${seed}`);
  const openOut = path.join(tmpDir, 'opened.txt');
  const fullEnv = {...process.env, TEMP: tmpDir, TMP: tmpDir, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: 'shots0834v', PM_REMOTE_DRYRUN: '1',
    PM_REMOTE_NOTS: '1', PM_REMOTE_LOOPBACK: '1', PM_SHARE_BIND: '127.0.0.1', PM_TEST_OPEN_OUT: openOut, ...env};
  const app = spawn(exe, ['--dev', '--test-offscreen', '--test-no-network', '--demo-branding', '--test-feed', feed, ...args],
    {stdio: 'ignore', env: fullEnv, windowsHide: true});
  const kill = () => { try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} };
  const dev = (cmd, lp = 0, wait = 700) => { try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', DEVCMD, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe', windowsHide: true}); } catch {} };
  const readLog = () => { try { return fs.readFileSync(path.join(dataDir, 'phonemirror.log'), 'utf8'); } catch { return ''; } };
  const waitLog = async (re, ms) => { const end = Date.now() + ms; while (Date.now() < end) { const m = readLog().match(re); if (m) return m; await sleep(200); } return null; };
  const grab = name => {
    const got = [];
    for (const f of fs.readdirSync(shotsDir)) {
      if (!/\.png$/.test(f)) continue;
      const dst = path.join(outDir, `${lang}_${tag}${name}.png`);
      fs.copyFileSync(path.join(shotsDir, f), dst);
      fs.rmSync(path.join(shotsDir, f));
      got.push(path.basename(dst));
    }
    if (!got.length) say(`${lang} ${tag}${name}: (nothing)`);
    return got;
  };
  const win = async (name, wait = 600) => { dev(910, 0, wait); return grab(name); };
  const check = when => {
    const w = windows(app.pid);
    say(`${lang} ${tag} ${when}: windows ${w.n}, on a monitor ${w.on}, Alt+Tab visible ${w.alt}`);
    if (w.on !== 0 || w.alt !== 0) { say(w.raw); kill(); throw new Error('a window on a monitor / in Alt+Tab: own PID killed'); }
  };
  try {
    await waitLog(/mirroring|first picture|picture/, 8000);
    await sleep(3500);
    check('start');
    vw(app.pid, [['synth']]);
    await steps({dev, grab, win, waitLog, readLog, check, pid: app.pid, v: ops => vw(app.pid, ops), openOut});
    check('end');
  } catch (e) { say(`${lang} ${tag} ERROR ${e.message}`); }
  dev(104, 0, 500);
  for (let i = 0; i < 100 && app.exitCode === null; i++) await sleep(100);
  if (app.exitCode === null) { kill(); say(`${lang} ${tag}: quit did not end it, own PID killed`); }
  fs.mkdirSync(path.join(outRoot, 'logs'), {recursive: true});
  try { fs.copyFileSync(path.join(dataDir, 'phonemirror.log'), path.join(outRoot, 'logs', `${which}_${lang}_${tag}.log`)); } catch {}
  fs.rmSync(dataDir, {recursive: true, force: true});
}

for (const lang of langs) {
  // T: the toolbar: up after a move at the top, after 3 s still, after a move in the middle / at the bottom.
  if (only.includes('T')) await run(lang, 'T', {steps: async ({v, win}) => {
    v([['move', 0.5, 0.05]]); await sleep(500); await win('1-toolbar-near-top', 200);
    await sleep(3500); await win('2-toolbar-idle-3s', 200);
    v([['move', 0.5, 0.6], ['sleep', 120], ['move', 0.52, 0.62]]); await sleep(400); await win('3-move-middle', 200);
    await sleep(3500);
    v([['leave']]); await sleep(300);
    v([['move', 0.5, 0.8]]); await sleep(400); await win('4-enter-bottom', 200);
    await sleep(3500); await win('5-entered-idle', 200);
  }});
  // B: 凍結 / 放大鏡 badges: frozen + 2x, then the toolbar up over them, then a narrow window, then 4x + landscape.
  if (only.includes('B')) await run(lang, 'B', {steps: async ({v, dev, win}) => {
    dev(C.Freeze, 0, 600); dev(C.MagCycle, 0, 2600);
    await win('1-frozen-zoom2', 200);
    v([['move', 0.5, 0.05]]); await sleep(500); await win('2-frozen-zoom2-toolbar', 200);
    await sleep(3000);
    v([['size', 420, 700]]); await sleep(1200); dev(C.MagCycle, 0, 2600); await win('3-narrow-frozen-zoom4', 200);
    v([['move', 0.5, 0.05]]); await sleep(500); await win('4-narrow-toolbar', 200);
    await sleep(3000);
    dev(936, 1, 800); await win('5-narrow-boxes', 200);
  }});
  // R (after only): the picture frozen by a translation (DevCommand 8342: no models needed) + 2x, then the toolbar.
  if (only.includes('R') && after) await run(lang, 'R', {steps: async ({v, dev, win}) => {
    dev(8342, 1, 900); dev(C.MagCycle, 0, 2600); dev(8343, 0, 300);
    await win('1-trfreeze-zoom2', 200);
    v([['move', 0.5, 0.05]]); await sleep(500); await win('2-trfreeze-toolbar', 200);
    await sleep(3000);
    dev(C.Freeze, 0, 900); await win('3-user-freeze-back', 200);
  }});
  // S: the screenshot toast, hovered, clicked.
  if (only.includes('S')) await run(lang, 'S', {steps: async ({v, dev, win, readLog, openOut, check}) => {
    dev(C.Snapshot, 0, 300); await sleep(300); await win('1-shot-toast', 100);
    const r = v([['toast']]);
    say(`${lang} S: ${r}`);
    const m = r.match(/toast (-?\d+)/);
    const val = m ? Number(m[1]) : -1;
    if (val > 0) {
      const x = val & 0xFFFF, y = (val >> 16) & 0xFFFF;
      v([['movepx', x, y]]); await sleep(400); await win('2-shot-toast-hover', 100);
      await sleep(after ? 9000 : 0);  // past its 8 s: still up while the pointer is on it
      if (after) await win('2b-hover-held-9s', 100);
      v([['click', x, y]]); await sleep(800);
      check('after the toast click (chip up)');
      const opened = fs.existsSync(openOut) ? fs.readFileSync(openOut, 'utf8').trim() : '';
      say(`${lang} S: toast click -> ${opened || '(nothing opened)'}`);
      await win('3-after-click', 100);
    }
  }});
  // I: 畫筆模式 on: the badge (+ the toolbar), then zoomed and frozen together with the badge.
  if (only.includes('I')) await run(lang, 'I', {steps: async ({v, dev, win}) => {
    dev(C.Ink, 0, 900); await sleep(2600); await win('1-ink-on', 200);
    v([['move', 0.5, 0.05]]); await sleep(500); await win('2-ink-toolbar', 200);
    await sleep(3000);
    dev(C.Freeze, 0, 600); dev(C.MagCycle, 0, 2600); await win('3-ink-frozen-zoom', 200);
    v([['size', 420, 700]]); await sleep(1500); await win('4-ink-narrow', 200);
  }});
}
fs.writeFileSync(path.join(outRoot, `${which}_shots.txt`), log.join('\n') + '\n');
