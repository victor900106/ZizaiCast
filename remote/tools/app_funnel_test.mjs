// 0.8.3.2 常駐外網 when Funnel cannot start (node 22+): the app against a FAKE tailscale CLI
// (pm_fake_tailscale.exe, real processes; tailscale itself is never run, nothing is changed).
//   node app_funnel_test.mjs <自在投影.exe> <pm_fake_tailscale.exe> <outDir> [--langs zh-TW,en,ja,ko]
// Own --dev --test-offscreen instance and data folder, PM_REMOTE_LOOPBACK (no firewall prompt), the
// password set and 常駐外網 on, so the fixed link starts at once. For each case: how long until the card
// says what to do, the UI thread's answer time while the CLI runs (SendMessageTimeout every 100 ms),
// the card (PNG, 100 %), no fake CLI process left. Also: 開啟設定頁 (logged, no browser), "enabled on the
// page" then 我開好了，再試一次 -> up; 關閉固定網址 and quitting while the CLI hangs -> at once.
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';

const [exe, fakeCli, outDir, ...rest] = process.argv.slice(2);
if (!exe || !fakeCli || !outDir) { console.error('usage: node app_funnel_test.mjs <自在投影.exe> <pm_fake_tailscale.exe> <outDir> [--langs a,b]'); process.exit(2); }
let langs = ['zh-TW', 'en', 'ja', 'ko'];
for (let i = 0; i < rest.length; i++) if (rest[i] === '--langs') langs = rest[++i].split(',');
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const devCmdPs1 = path.resolve(here, '..', '..', 'share', 'tools', 'dev_cmd.ps1');
const tmpDir = path.resolve(outDir, '_tmp');
const C = {TabRemote: 383, FixedToggle: 356, FixedSetup: 443, Exit: 104};
const salt = crypto.randomBytes(16);
const pwHash = `pbkdf2-sha256$310000$${salt.toString('hex')}$${crypto.pbkdf2Sync('Zizai-Test-2026', salt, 310000, 32, 'sha256').toString('hex')}`;

// The UI thread's answer time: SendMessageTimeout(WM_NULL) to the app's window, every 100 ms for `ms`.
function uiProbe(pid, ms) {
  const ps = `Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class PmUi {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, IntPtr l, uint f, uint t, out IntPtr r);
  public static IntPtr Find(uint pid) { IntPtr f = IntPtr.Zero; EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); var sb = new StringBuilder(64); GetClassName(h, sb, 64);
    if (p == pid && sb.ToString() == "PhoneMirrorVideoWindow") { f = h; return false; } return true; }, IntPtr.Zero); return f; }
}
"@
$h = [PmUi]::Find(${pid}); $max = 0; $n = 0; $sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.ElapsedMilliseconds -lt ${ms}) { $t = [Diagnostics.Stopwatch]::StartNew(); $r = [IntPtr]::Zero
  [void][PmUi]::SendMessageTimeout($h, 0, [IntPtr]::Zero, [IntPtr]::Zero, 2, 10000, [ref]$r); $e = $t.ElapsedMilliseconds; if ($e -gt $max) { $max = $e }; $n++; Start-Sleep -Milliseconds 100 }
Write-Output "$n $max"`;
  return new Promise(res => {
    const p = spawn('powershell', ['-NoProfile', '-Command', ps], {windowsHide: true});
    let o = '';
    p.stdout.on('data', d => { o += d; });
    p.on('close', () => { const [n, max] = o.trim().split(/\s+/).map(Number); res({probes: n, maxMs: max}); });
  });
}
const alive = pidsFile => {
  if (!fs.existsSync(pidsFile)) return 0;
  let n = 0;
  for (const line of fs.readFileSync(pidsFile, 'utf8').split('\n')) {
    const pid = +line.split(' ')[0];
    if (!pid) continue;
    try { process.kill(pid, 0); n++; } catch {}
  }
  return n;
};

async function runCase(lang, mode, steps, opt = {}) {
  const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-funnel0832');
  fs.rmSync(dataDir, {recursive: true, force: true});
  fs.mkdirSync(dataDir, {recursive: true});
  fs.rmSync(tmpDir, {recursive: true, force: true});
  fs.mkdirSync(path.join(tmpDir, 'pmshots'), {recursive: true});
  const modeFile = path.join(tmpDir, 'mode.txt'), pidsFile = path.join(tmpDir, 'pids.txt');
  fs.writeFileSync(modeFile, mode + '\n');
  fs.writeFileSync(path.join(dataDir, 'settings.ini'),
    `[settings]\nlanguage=${lang}\nremote_port=${18800 + Math.floor(Math.random() * 300)}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\ntoolbar_hint_shown=1\n` +
    `whats_new_seen=0.8.3\nidle_tab=remote\nremote_persistent=1\nremote_pw_hash=${pwHash}\nremote_fixed_secret=${crypto.randomBytes(32).toString('hex')}\n`);
  const env = {...process.env, TEMP: tmpDir, TMP: tmpDir, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: 'funnel0832', PM_REMOTE_DRYRUN: '1', PM_REMOTE_NOTS: '1',
    PM_REMOTE_LOOPBACK: '1', PM_REMOTE_FAKECLI: fakeCli, PM_FAKE_TS_MODEFILE: modeFile, PM_FAKE_TS_PIDS: pidsFile, PM_FAKE_TS_STATE: path.join(tmpDir, 'state.txt')};
  const t0 = Date.now();
  const app = spawn(exe, ['--dev', '--test-offscreen'], {stdio: 'ignore', env, windowsHide: true});
  const dev = (cmd, lp = 0, wait = 600) => { try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmdPs1, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe', windowsHide: true}); } catch {} };
  const readLog = () => { try { return fs.readFileSync(path.join(dataDir, 'phonemirror.log'), 'utf8'); } catch { return ''; } };
  const waitLog = async (re, ms) => { const end = Date.now() + ms; while (Date.now() < end) { const m = readLog().match(re); if (m) return m; await sleep(100); } return null; };
  const r = {lang, mode};
  try {
    await waitLog(/常駐外網 starting/, 15000);
    const tStart = Date.now();
    if (opt.during) {  // while the CLI hangs (no answer awaited)
      await opt.during({dev, waitLog, app, r, pidsFile});
      throw 'done';
    }
    const probe = uiProbe(app.pid, 4000);
    const fail = await waitLog(/常駐外網: Funnel failed: ([^\n]*)|常駐外網 ready at (\S+)/, 25000);
    r.secondsToAnswer = +((Date.now() - tStart) / 1000).toFixed(1);
    r.answer = fail ? (fail[1] || 'ready ' + fail[2]).replace(/ \(the page offers.*$/, '') : null;
    r.ui = await probe;
    await sleep(800);
    dev(C.TabRemote, 0, 1200); dev(985, 96, 1500); dev(910, 0, 1200);
    const shot = path.join(tmpDir, 'pmshots', 'window.png');
    if (fs.existsSync(shot)) fs.copyFileSync(shot, path.join(outDir, `funnel_${lang}_${mode}.png`));
    if (steps) await steps({dev, waitLog, readLog, modeFile, pidsFile, app, r});
    r.fakeAlive = alive(pidsFile);
  } catch (e) { if (e !== 'done') r.error = String(e && e.stack || e); }
  const tq = Date.now();
  dev(C.Exit, 0, 300);
  for (let i = 0; i < 150 && app.exitCode === null; i++) await sleep(100);
  r.quitSeconds = +((Date.now() - tq) / 1000).toFixed(1);
  if (app.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} r.killed = true; }
  await sleep(500);
  r.fakeAliveAfterQuit = alive(pidsFile);
  fs.copyFileSync(path.join(dataDir, 'phonemirror.log'), path.join(outDir, `funnel_${lang}_${mode}.log`));
  console.log(JSON.stringify(r));
  return r;
}

const all = [];
// The card in every language for the friend's case (Funnel not on for a new tailnet).
for (const lang of langs) all.push(await runCase(lang, 'enable-hang', lang !== 'zh-TW' ? null : async ({dev, waitLog, modeFile, r}) => {
  dev(C.FixedSetup, 0, 800);
  r.setupLogged = !!(await waitLog(/would open https:\/\/login\.tailscale\.com\/f\/funnel\?node=nFAKE0832 in the browser/, 3000));
  fs.writeFileSync(modeFile, 'ok\n');  // "enabled on the web page"
  dev(C.FixedToggle, 0, 500);           // 我開好了，再試一次
  r.afterEnable = !!(await waitLog(/常駐外網 ready at https:\/\/example-pc\.example\.ts\.net:8443\//, 15000));
  dev(985, 96, 1200); dev(910, 0, 1200);
}));
for (const mode of ['https-hang', 'no-attr', 'old', 'needs-login', 'port-taken']) all.push(await runCase('zh-TW', mode));
// A CLI that never answers: the 15 s limit; 關閉固定網址 while it hangs (the UI answers at once); quitting while it hangs.
all.push(await runCase('zh-TW', 'silent-hang'));
all.push(await runCase('zh-TW', 'silent-hang', null, {during: async ({dev, waitLog, app, r}) => {
  await sleep(1500);
  const probe = uiProbe(app.pid, 2500);
  await sleep(200);
  const t = Date.now();
  dev(C.FixedToggle, 0, 100);  // 關閉固定網址 while the CLI hangs
  r.stopLogged = !!(await waitLog(/常駐外網 stopped \(關閉固定網址 on the page\)/, 3000));
  r.stopSeconds = +((Date.now() - t) / 1000).toFixed(1);
  r.uiDuringStop = await probe;
  r.case = 'stop while it hangs';
}}));
all.push(await runCase('zh-TW', 'silent-hang', null, {during: async ({r}) => { await sleep(1500); r.case = 'quit while it hangs'; }}));
fs.writeFileSync(path.join(outDir, 'app_funnel.json'), JSON.stringify(all, null, 1));
try { fs.rmSync(tmpDir, {recursive: true, force: true}); } catch {}
