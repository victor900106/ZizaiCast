// 0.8.3.6 連線時要不要允許: before / after screenshots (zh-TW / en / ja / ko), off every monitor.
//   node tools/shots_0836_auth.mjs <自在投影.exe> <outDir> <tag> [--langs zh-TW,en,ja,ko]    (tag: before | after)
// Same harness as shots_0835_files.mjs: own --dev --test-offscreen instance (its own data folder and TEMP),
// loopback only, a made-up fixed address (PM_REMOTE_FUNNELFAKE: no tailscale / cloudflared is ever run), no
// sound, input dry-run, own PID only; the windows are checked to be on no monitor and not in Alt+Tab.
// Made-up data only: a password hash of a test password, one 臨時帳號 "guest-4821", remembered devices with
// made-up ids, the address 203.0.113.24 (documentation range).
// Scenes:
//   A  進階設定 as remote_advanced.png (before: the 外面的新手機也要電腦按允許 row; after: 連線時要不要允許 ▸)
//      and, after: its 連線時要不要允許 ▸ / 已記住的手機 ▸ submenus
//   K  (after only: 963/11..13) the 允許 dialog for a made-up 臨時帳號 / 臨時網址 / 主帳號 phone, with
//      每次問 (= what 0.8.3.5 shows) and with 記住這台裝置 (the 記住 box)
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';

const [exe, outRoot, tag, ...rest] = process.argv.slice(2);
if (!exe || !outRoot || !tag) { console.error('usage: node tools/shots_0836_auth.mjs <exe> <outDir> <tag> [--langs ..]'); process.exit(1); }
let langs = ['zh-TW', 'en', 'ja', 'ko'];
for (let i = 0; i < rest.length; i++) if (rest[i] === '--langs') langs = rest[++i].split(',');
const HERE = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const REPO = path.resolve(HERE, '..');
const INST = process.env.UX_INSTANCE || 'shots0836a';
const WORK = process.env.UX_WORK || path.join(process.env.TEMP, `zizai-${INST}`);
const outDir = path.resolve(outRoot, tag);
fs.mkdirSync(outDir, {recursive: true});
const DEVCMD = path.join(REPO, 'share', 'tools', 'dev_cmd.ps1');
const sleep = ms => new Promise(r => setTimeout(r, ms));
const LAN = 'http://203.0.113.10:18571/AbCdEfGhJkMnPqRsTuVwXyZ234567892/';
const FUNNEL = 'https://example-pc.tail000000.ts.net:8443/';
const TUNNEL = 'https://example-demo.example.com';
const salt = crypto.randomBytes(16);
const pwHash = `pbkdf2-sha256$310000$${salt.toString('hex')}$${crypto.pbkdf2Sync('Zizai-Test-2026', salt, 310000, 32, 'sha256').toString('hex')}`;
const pwSeed = `remote_pw_hash=${pwHash}\nremote_fixed_secret=${crypto.randomBytes(32).toString('hex')}\nremote_persistent=1\n`;
const GUEST_ID = 'a1b2c3d4e5f60708';
const now = Math.floor(Date.now() / 1000);
const guestSeed = `# test\n${GUEST_ID}\tguest-4821\t${pwHash}\t${now - 600}\t${now + 86400}\t0\t0\t1\t0\n`;
const devicesBefore = 'demo0001demo0001demo0001demo0001\tiPhone · Safari\t2026-10-01\n' +
                      'demo0002demo0002demo0002demo0002\tWindows PC · Edge\t2026-10-12\n';
const devicesAfter = devicesBefore.replace(/\n/g, '\tlan\n') +
                     'demo0003demo0003demo0003demo0003\tAndroid · Chrome\t2026-10-20\tfixed\n' +
                     `demo0004demo0004demo0004demo0004\tiPhone · Safari\t2026-10-21\tguest:${GUEST_ID}\n` +
                     'demo0005demo0005demo0005demo0005\tiPad · Safari\t2026-10-22\ttemp\n';
const log = [];
const say = s => { console.log(s); log.push(s); };

function onScreen(pid) {
  const ps = `Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Collections.Generic;
public static class PmWin6 {
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
$w = [PmWin6]::Of(${pid}); $on = 0; $tab = 0
foreach ($r in $w) { $tab += $r[4]; foreach ($s in [System.Windows.Forms.Screen]::AllScreens) { $b = $s.Bounds; if ($r[0] -lt $b.Right -and $r[2] -gt $b.Left -and $r[1] -lt $b.Bottom -and $r[3] -gt $b.Top) { $on++ } } }
Write-Output "$($w.Count) $on $tab"`;
  try { const [n, on, tab] = execFileSync('powershell', ['-NoProfile', '-Command', ps], {encoding: 'utf8', windowsHide: true}).trim().split(/\s+/).map(Number); return {n, on, tab}; }
  catch { return {n: -1, on: -1, tab: -1}; }
}

async function run(lang, scene, steps) {
  const dataDir = path.join(process.env.LOCALAPPDATA, `PhoneMirror-dev-${INST}`);
  const tmpDir = path.join(WORK, '_tmp');
  const shotsDir = path.join(tmpDir, 'pmshots');
  fs.rmSync(dataDir, {recursive: true, force: true});
  fs.mkdirSync(dataDir, {recursive: true});
  fs.rmSync(tmpDir, {recursive: true, force: true});
  fs.mkdirSync(shotsDir, {recursive: true});
  fs.writeFileSync(path.join(dataDir, 'settings.ini'),
    `[settings]\nlanguage=${lang}\nremote_port=${18500 + Math.floor(Math.random() * 300)}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\n` +
    `toolbar_hint_shown=1\nright_click_hint_shown=1\nwhats_new_seen=0.8.3.9\nmiracast=0\nandroid_auto=0\nidle_tab=remote\n${pwSeed}`);
  fs.writeFileSync(path.join(dataDir, 'remote_guests.txt'), guestSeed);
  fs.writeFileSync(path.join(dataDir, 'remote_devices.txt'), tag === 'before' ? devicesBefore : devicesAfter);
  const env = {...process.env, TEMP: tmpDir, TMP: tmpDir, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: INST, PM_REMOTE_DRYRUN: '1',
    PM_REMOTE_TESTCLIP: '1', PM_REMOTE_NOTS: '1', PM_REMOTE_LOOPBACK: '1', PM_SHARE_BIND: '127.0.0.1', PM_REMOTE_FUNNELFAKE: FUNNEL,
    PM_REMOTE_SHOWLAN: LAN, PM_REMOTE_TUNNELFAKE: TUNNEL};
  const app = spawn(exe, ['--dev', '--test-offscreen', '--test-no-network', '--demo-branding'], {stdio: 'ignore', env, windowsHide: true});
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
      const dst = path.join(outDir, `${lang}_${scene}${name}_${f}`);
      fs.copyFileSync(path.join(shotsDir, f), dst);
      fs.rmSync(path.join(shotsDir, f));
      got.push(path.basename(dst));
    }
    say(`${lang} ${scene}${name}: ${got.join(', ') || '(nothing)'}`);
    return got;
  };
  try {
    await waitLog(/remote: on at/, 15000);
    await sleep(2500);
    check('start');
    await steps({dev, grab, waitLog, readLog, check});
    check('end');
  } catch (e) { say(`${lang} ${scene} ERROR ${e.message}`); }
  dev(104, 0, 500);
  for (let i = 0; i < 100 && app.exitCode === null; i++) await sleep(100);
  if (app.exitCode === null) { kill(); say(`${lang} ${scene}: quit did not end it, own PID killed`); }
  fs.mkdirSync(path.join(outRoot, 'logs'), {recursive: true});
  try { fs.copyFileSync(path.join(dataDir, 'phonemirror.log'), path.join(outRoot, 'logs', `${tag}_${lang}_${scene}.log`)); } catch {}
  fs.rmSync(dataDir, {recursive: true, force: true});
}

const P = (way, pol) => 470 + way * 3 + pol;  // CmdRemotePolicy0 + way * 3 + policy
for (const lang of langs) {
  await run(lang, 'A', async ({dev, grab}) => {
    dev(963, 8, 1200);
    grab('');
  });
  if (tag !== 'after') continue;
  await run(lang, 'K', async ({dev, grab, readLog}) => {
    const ask = (name, way, pol, how) => { dev(P(way, pol), 0, 600); dev(963, how, 1200); grab(name); };
    ask('_guest_ask', 1, 0, 11);       // 臨時帳號 每次問 (= 0.8.3.5's dialog)
    ask('_guest_remember', 1, 1, 11);  // 臨時帳號 記住這台裝置: the 記住 box
    ask('_temp_ask', 2, 0, 12);
    ask('_temp_remember', 2, 1, 12);
    ask('_fixed_ask', 0, 0, 13);       // 主帳號 每次問 (no 記住 box)
    ask('_fixed_remember', 0, 1, 13);
    dev(P(0, 2), 0, 400), dev(P(1, 0), 0, 400), dev(P(2, 0), 0, 400);  // back to the defaults
    dev(963, 8, 1200);
    grab('_defaults');
    say(`${lang} K policy lines: ` + (readLog().match(/連線時要不要允許: [^\n]*/g) || []).join(' | '));
  });
}
fs.writeFileSync(path.join(outRoot, `${tag}_shots.txt`), log.join('\n') + '\n');
