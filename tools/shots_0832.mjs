// 0.8.3.2 screenshots of the 遠端控制 page: 允許遠端控制 (本機網址 + 臨時帳號), per language, and the
// 新增臨時帳號 window; with --dpi also the page as if on a 1080p monitor at 125 % and 150 %.
//   node tools/shots_0832.mjs <自在投影.exe> <outDir> [--langs zh-TW,en,ja,ko] [--before] [--dpi]
// --before: an older build (0.8.3.1): only the page at the window's own DPI (it has no 0.8.3.2 hooks).
// Own --dev --test-offscreen instance + data folder + TEMP per run (no window on any monitor, no input,
// no sound), PM_REMOTE_LOOPBACK (127.0.0.3 only: no firewall prompt), the fixed link made up
// (PM_REMOTE_FUNNELFAKE: the tailscale CLI is never run; cloudflared neither), made-up LAN address
// (PM_REMOTE_SHOWLAN, RFC 5737). The 臨時帳號 here are made in the test instance only.
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';

const [exe, outDir, ...rest] = process.argv.slice(2);
let langs = ['zh-TW', 'en', 'ja', 'ko'], before = false, dpi = false;
for (let i = 0; i < rest.length; i++) {
  if (rest[i] === '--langs') langs = rest[++i].split(',');
  else if (rest[i] === '--before') before = true;
  else if (rest[i] === '--dpi') dpi = true;
}
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const devCmdPs1 = path.resolve(here, '..', 'share', 'tools', 'dev_cmd.ps1');
const tmpDir = path.resolve(outDir, '_tmp');
const shotsDir = path.join(tmpDir, 'pmshots');
const LAN = 'http://203.0.113.10:18571/AbCdEfGhJkMnPqRsTuVwXyZ234567892/';
const FUNNEL = 'https://example-pc.example.ts.net:8443/';
const C = {TabRemote: 383, GuestNew: 440};
const salt = crypto.randomBytes(16);
const pwHash = `pbkdf2-sha256$310000$${salt.toString('hex')}$${crypto.pbkdf2Sync('Zizai-Test-2026', salt, 310000, 32, 'sha256').toString('hex')}`;
const log = [];

// Every visible window of a process: how many are on a monitor (must be 0).
function onScreen(pid) {
  const ps = `Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Collections.Generic;
public static class PmWin2 {
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
$w = [PmWin2]::Of(${pid}); $on = 0
foreach ($r in $w) { foreach ($s in [System.Windows.Forms.Screen]::AllScreens) { $b = $s.Bounds; if ($r[0] -lt $b.Right -and $r[2] -gt $b.Left -and $r[1] -lt $b.Bottom -and $r[3] -gt $b.Top) { $on++ } } }
Write-Output "$($w.Count) $on"`;
  try { const [n, on] = execFileSync('powershell', ['-NoProfile', '-Command', ps], {encoding: 'utf8', windowsHide: true}).trim().split(/\s+/).map(Number); return {n, on}; }
  catch { return {n: -1, on: -1}; }
}

for (const lang of langs) {
  const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-shots0832');
  fs.rmSync(dataDir, {recursive: true, force: true});
  fs.mkdirSync(dataDir, {recursive: true});
  fs.rmSync(tmpDir, {recursive: true, force: true});
  fs.mkdirSync(shotsDir, {recursive: true});
  fs.writeFileSync(path.join(dataDir, 'settings.ini'),
    `[settings]\nlanguage=${lang}\nremote_port=${18500 + Math.floor(Math.random() * 300)}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\ntoolbar_hint_shown=1\n` +
    `whats_new_seen=0.8.3\nidle_tab=remote\nremote_persistent=1\nremote_pw_hash=${pwHash}\nremote_fixed_secret=${crypto.randomBytes(32).toString('hex')}\n`);
  const env = {...process.env, TEMP: tmpDir, TMP: tmpDir, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: 'shots0832', PM_REMOTE_DRYRUN: '1', PM_REMOTE_TESTCLIP: '1',
    PM_REMOTE_NOTS: '1', PM_REMOTE_LOOPBACK: '1', PM_REMOTE_FUNNELFAKE: FUNNEL, PM_REMOTE_SHOWLAN: LAN, PM_TEST_GUEST_OUT: path.join(tmpDir, 'copy.txt')};
  const app = spawn(exe, ['--dev', '--test-offscreen', '--demo-branding'], {stdio: 'ignore', env, windowsHide: true});
  const dev = (cmd, lp = 0, wait = 700) => { try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmdPs1, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe', windowsHide: true}); } catch {} };
  const readLog = () => { try { return fs.readFileSync(path.join(dataDir, 'phonemirror.log'), 'utf8'); } catch { return ''; } };
  const grab = name => {
    for (const f of fs.readdirSync(shotsDir)) {
      fs.copyFileSync(path.join(shotsDir, f), path.join(outDir, `${before ? 'before' : 'after'}_${lang}_${name}_${f}`));
      fs.rmSync(path.join(shotsDir, f));
    }
  };
  try {
    for (let i = 0; i < 100 && !/常駐外網 ready at/.test(readLog()); i++) await sleep(200);
    await sleep(3500);
    const win = onScreen(app.pid);
    log.push(`${lang}: app windows ${win.n}, on a monitor ${win.on}`);
    // 0.8.3.2 builds: the page at 100 % first (96 DPI, the same 540 x 960 DIPs); an older build stays at the window's own DPI
    if (!before) dev(985, 96, 1500);
    dev(C.TabRemote, 0, 1500); dev(910, 0, 1200); grab('1-page');
    if (!before) {
      // two 臨時帳號: 24 小時 可操作 (as suggested), and 用一次 僅觀看 + 傳檔
      dev(C.GuestNew, 0, 1000); dev(982, 96, 900); grab('2-guest-form');
      dev(981, 2 | 16, 1200); dev(982, 96, 900); grab('3-guest-made');
      dev(983, 0, 600); dev(984, 0, 600);
      dev(C.GuestNew, 0, 1000); dev(981, 0 | 4 | 8 | 16, 1200); dev(984, 0, 900);
      await sleep(1500);
      dev(910, 0, 1200); grab('4-page-guests');
      if (dpi) {
        for (const [d, h] of [[120, 985], [144, 985]]) {
          dev(985, d | (h << 16), 1500); dev(910, 0, 1500); grab(`5-dpi${d}`);
          { dev(C.GuestNew, 0, 1000); dev(982, d, 900); grab(`6-guest-form-dpi${d}`); dev(984, 0, 600); }
        }
      }
      const copied = fs.existsSync(path.join(tmpDir, 'copy.txt')) ? fs.readFileSync(path.join(tmpDir, 'copy.txt'), 'utf8') : '';
      log.push(`${lang}: 複製全部 text: ${copied.replace(/\n/g, ' / ').replace(/(密碼|Password|パスワード|비밀번호)[：: ]+\S+/, '$1 <hidden>')}`);
      log.push(...(readLog().match(/dev dpi: [^\n]*/g) || []).map(x => `${lang}: ${x}`));
      const after = onScreen(app.pid);
      log.push(`${lang}: at the end: windows ${after.n}, on a monitor ${after.on}`);
    }
  } catch (e) { log.push(`${lang}: ERROR ${e}`); }
  dev(104, 0, 500);
  for (let i = 0; i < 100 && app.exitCode === null; i++) await sleep(100);
  if (app.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} log.push(`${lang}: killed own app`); }
}
try { fs.rmSync(tmpDir, {recursive: true, force: true}); } catch {}
fs.writeFileSync(path.join(outDir, `${before ? 'before' : 'after'}_shots.txt`), log.join('\n') + '\n');
console.log(log.join('\n'));
