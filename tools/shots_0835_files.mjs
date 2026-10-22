// 0.8.3.5 files: the 新增臨時帳號 panel before / after screenshots (zh-TW / en / ja / ko), off every monitor.
//   node tools/shots_0835_files.mjs <自在投影.exe> <outDir> <tag> [--langs zh-TW,en,ja,ko]    (tag: before | after)
// Same harness as shots_0834_panel.mjs (own --dev --test-offscreen instance, loopback, no real tailscale / cloudflared,
// no sound, own PID only, window never on a monitor or in Alt+Tab). Scenes: D = the panel as it opens (default
// rights), F = 可操作 + 可以傳檔 ticked, V = 僅觀看. Output: <outDir>/<tag>/<lang>_<scene>_guest_panel_144.png
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';

const [exe, outRoot, tag, ...rest] = process.argv.slice(2);
if (!exe || !outRoot || !tag) {
  console.error('usage: node tools/shots_0834_panel.mjs <exe> <outDir> <tag> [--langs ..] [--widths ..] [--dpis ..] [--only SOTA]');
  process.exit(1);
}
let langs = ['zh-TW', 'en', 'ja', 'ko'], widths = [420, 510, 680, 900], dpis = [96, 120, 144], only = 'SOTA', height = 760;
let extraEnv = {};
for (let i = 0; i < rest.length; i++) {
  if (rest[i] === '--langs') langs = rest[++i].split(',');
  else if (rest[i] === '--widths') widths = rest[++i].split(',').map(Number);
  else if (rest[i] === '--dpis') dpis = rest[++i].split(',').map(Number);
  else if (rest[i] === '--only') only = rest[++i];
  else if (rest[i] === '--height') height = Number(rest[++i]);
  else if (rest[i] === '--env') { const [k, v] = rest[++i].split('='); extraEnv[k] = v; }
}
const HERE = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const REPO = path.resolve(HERE, '..');
const INST = process.env.UX_INSTANCE || 'shots0835f';  // two runs side by side: each its own instance, data folder and TEMP
const WORK = process.env.UX_WORK || path.join(process.env.TEMP, `zizai-${INST}`);
const outDir = path.resolve(outRoot, tag);
fs.mkdirSync(outDir, {recursive: true});
const DEVCMD = path.join(REPO, 'share', 'tools', 'dev_cmd.ps1');
const SIZE = path.join(REPO, 'tools', 'win_size.ps1');
const sleep = ms => new Promise(r => setTimeout(r, ms));
const LAN = 'http://203.0.113.10:18571/AbCdEfGhJkMnPqRsTuVwXyZ234567892/';
const FUNNEL = 'https://example-pc.tail000000.ts.net:8443/';
const TSURL = 'http://example-pc.tail000000.ts.net:18571/AbCdEfGhJkMnPqRsTuVwXyZ234567892/';
const TUNNEL = 'https://example-demo.example.com';
const C = {Toggle: 330, TabRemote: 383, GuestNew: 440};
const salt = crypto.randomBytes(16);
const pwHash = `pbkdf2-sha256$310000$${salt.toString('hex')}$${crypto.pbkdf2Sync('Zizai-Test-2026', salt, 310000, 32, 'sha256').toString('hex')}`;
const pwSeed = `remote_pw_hash=${pwHash}\nremote_fixed_secret=${crypto.randomBytes(32).toString('hex')}\n`;
const log = [];
let failures = 0;  // scene X
const say = s => { console.log(s); log.push(s); };

function onScreen(pid) {
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
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
  // Visible top-level windows of pid: the rectangle, then 1 when Alt+Tab / the taskbar would list it
  // (no WS_EX_TOOLWINDOW, and unowned or WS_EX_APPWINDOW).
  public static List<int[]> Of(uint pid) { var l = new List<int[]>(); EnumWindows((h, x) => { uint p; GetWindowThreadProcessId(h, out p);
    if (p == pid && IsWindowVisible(h)) { RECT r; GetWindowRect(h, out r); int ex = GetWindowLong(h, -20);
      int tab = (ex & 0x80) == 0 && (GetWindow(h, 4) == IntPtr.Zero || (ex & 0x40000) != 0) ? 1 : 0;
      l.Add(new int[] { r.L, r.T, r.R, r.B, tab }); } return true; }, IntPtr.Zero); return l; }
}
"@
$w = [PmWin4]::Of(${pid}); $on = 0; $tab = 0
foreach ($r in $w) { $tab += $r[4]; foreach ($s in [System.Windows.Forms.Screen]::AllScreens) { $b = $s.Bounds; if ($r[0] -lt $b.Right -and $r[2] -gt $b.Left -and $r[1] -lt $b.Bottom -and $r[3] -gt $b.Top) { $on++ } } }
Write-Output "$($w.Count) $on $tab"`;
  try { const [n, on, tab] = execFileSync('powershell', ['-NoProfile', '-Command', ps], {encoding: 'utf8', windowsHide: true}).trim().split(/\s+/).map(Number); return {n, on, tab}; }
  catch { return {n: -1, on: -1, tab: -1}; }
}

async function run(lang, scene, {seed = '', env = {}, steps}) {
  const dataDir = path.join(process.env.LOCALAPPDATA, `PhoneMirror-dev-${INST}`);
  const tmpDir = path.join(WORK, '_tmp');
  const shotsDir = path.join(tmpDir, 'pmshots');
  fs.rmSync(dataDir, {recursive: true, force: true});
  fs.mkdirSync(dataDir, {recursive: true});
  fs.rmSync(tmpDir, {recursive: true, force: true});
  fs.mkdirSync(shotsDir, {recursive: true});
  fs.writeFileSync(path.join(dataDir, 'settings.ini'),
    `[settings]\nlanguage=${lang}\nremote_port=${18500 + Math.floor(Math.random() * 300)}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\n` +
    `toolbar_hint_shown=1\nright_click_hint_shown=1\nwhats_new_seen=0.8.3\nmiracast=0\nandroid_auto=0\nidle_tab=remote\n${seed}`);
  const fullEnv = {...process.env, TEMP: tmpDir, TMP: tmpDir, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: INST, PM_REMOTE_DRYRUN: '1',
    PM_REMOTE_TESTCLIP: '1', PM_REMOTE_NOTS: '1', PM_REMOTE_LOOPBACK: '1', PM_SHARE_BIND: '127.0.0.1', PM_REMOTE_FUNNELFAKE: FUNNEL,
    PM_REMOTE_SHOWLAN: LAN, PM_REMOTE_TUNNELFAKE: TUNNEL, PM_TEST_GUEST_OUT: path.join(tmpDir, 'copy.txt'), ...extraEnv, ...env};
  for (const k of Object.keys(fullEnv)) if (fullEnv[k] === null) delete fullEnv[k];
  const app = spawn(exe, ['--dev', '--test-offscreen', '--test-no-network', '--demo-branding'], {stdio: 'ignore', env: fullEnv, windowsHide: true});
  const kill = () => { try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} };
  const dev = (cmd, lp = 0, wait = 700) => { try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', DEVCMD, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe', windowsHide: true}); } catch {} };
  const readLog = () => { try { return fs.readFileSync(path.join(dataDir, 'phonemirror.log'), 'utf8'); } catch { return ''; } };
  const waitLog = async (re, ms) => { const end = Date.now() + ms; while (Date.now() < end) { const m = readLog().match(re); if (m) return m; await sleep(200); } return null; };
  const check = when => {
    const w = onScreen(app.pid);
    say(`${lang} ${scene} ${when}: windows ${w.n}, on a monitor ${w.on}, in Alt+Tab ${w.tab}`);
    if (w.on !== 0) { kill(); throw new Error('a window on a monitor: own PID killed'); }
    if (w.tab !== 0) { kill(); throw new Error('a window in Alt+Tab / the taskbar: own PID killed'); }
  };
  const grab = name => {
    const got = [];
    for (const f of fs.readdirSync(shotsDir)) {
      if (!/\.png$/.test(f)) continue;
      const dst = path.join(outDir, f === 'window.png' ? `${lang}_${scene}${name}.png` : `${lang}_${scene}${name}_${f}`);
      fs.copyFileSync(path.join(shotsDir, f), dst);
      fs.rmSync(path.join(shotsDir, f));
      got.push(path.basename(dst));
    }
    if (!got.length) say(`${lang} ${scene}${name}: (nothing)`);
    return got;
  };
  // The window at width x height DIPs at this DPI (985 first: the DPI; then the client size in pixels).
  let curDpi = 0;
  const size = (w, dpi) => {
    if (dpi !== curDpi) { dev(985, dpi, 900); curDpi = dpi; }
    const out = execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', SIZE, '-ProcessId', String(app.pid),
      '-W', String(Math.round(w * dpi / 96)), '-H', String(Math.round(height * dpi / 96))], {encoding: 'utf8', windowsHide: true}).trim();
    const on = Number(out.split(/\s+/).pop());
    say(`${lang} ${scene} size ${w} DIPs @ ${dpi}: window ${out}`);
    if (on !== 0) { kill(); throw new Error(`resized onto a monitor (${out}): own PID killed`); }
  };
  const shot = async name => { await sleep(700); dev(910, 0, 900); return grab(name); };
  const sweep = async name => {
    for (const dpi of dpis)
      for (const w of widths) { size(w, dpi); await shot(`${name}_w${w}_${Math.round(dpi * 100 / 96)}`); check(`w${w}@${dpi}`); }
  };
  try {
    await waitLog(/remote: |idle|ready/, 6000);
    await sleep(3500);
    check('start');
    await steps({dev, grab, shot, sweep, size, waitLog, readLog, check, dataDir});
    check('end');
  } catch (e) { say(`${lang} ${scene} ERROR ${e.message}`); }
  dev(104, 0, 500);
  for (let i = 0; i < 100 && app.exitCode === null; i++) await sleep(100);
  if (app.exitCode === null) { kill(); say(`${lang} ${scene}: quit did not end it, own PID killed`); }
  fs.mkdirSync(path.join(outRoot, 'logs'), {recursive: true});
  try { fs.copyFileSync(path.join(dataDir, 'phonemirror.log'), path.join(outRoot, 'logs', `${tag}_${lang}_${scene}.log`)); } catch {}
}

for (const lang of langs) {
  await run(lang, "G", {seed: pwSeed + "remote_persistent=1\n", env: {PM_REMOTE_SHOWPHONES: "1"}, steps: async ({dev, grab, waitLog}) => {
    await waitLog(/常駐外網 ready at|固定網址 ready at|fixed address ready at/, 20000);
    await sleep(1500);
    dev(C.TabRemote, 0, 1200);
    const scene = (name, lp) => { dev(C.GuestNew, 0, 1000); if (lp !== null) dev(981, lp, 900); dev(982, 144, 900); grab(name); dev(984, 0, 600); };
    scene("D_default", null);            // as it opens
    scene("F_control_files", 2 | 8);     // 24 小時, 可操作, 可以傳檔 ticked
    scene("N_control_nofiles", 2);       // 24 小時, 可操作, box cleared
    scene("V_viewonly", 2 | 4 | 8);      // 僅觀看 (the box asked for, the panel refuses)
  }});
}
fs.writeFileSync(path.join(outRoot, `${tag}_shots.txt`), log.join("\n") + "\n");
