// 0.8.3.6 連線時要不要允許 inside the real app (node 22+), no browser, no phone:
//   node tools/app_auth0836_test.mjs <自在投影.exe> <outDir>
// Own --dev --test-offscreen instance (own data folder and TEMP; windows checked: on no monitor, not in Alt+Tab),
// loopback only, a made-up fixed address (PM_REMOTE_FUNNELFAKE: no tailscale / cloudflared is ever run), no sound,
// input dry-run, own PID only. The 允許 dialog is opened for a made-up phone (DevCommand 963 / 14..16: no session
// behind it, the host ignores the answer; the app's side runs) and answered with DevCommand 961 (1 允許, 2 拒絕,
// 3 記住 + 允許). remote_devices.txt and settings.ini are read after each step.
//  1. settings: a 0.8.3.5 settings.ini with 外面的新手機也要電腦按允許 on -> 主帳號 記住這台裝置; each menu row
//     (DevCommand 470 + way * 3 + policy) is saved as words and logged; the host is told
//  2. 每次問: no 記住 box (記住 + 允許 writes nothing); 記住這台裝置: the box, the entry with the account's scope
//  3. clearing: 拒絕 -> gone; 臨時帳號 重設密碼 -> gone; 臨時帳號 deleted -> gone; 主帳號 password changed -> the
//     fixed-address entry gone (the LAN one stays); one row from the menu; 全部忘記
//  4. 中斷 (kick) while the dialog is open -> that device's entries gone
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';

const [exe, outDir] = process.argv.slice(2);
if (!exe || !outDir) { console.error('usage: node tools/app_auth0836_test.mjs <exe> <outDir>'); process.exit(2); }
fs.mkdirSync(outDir, {recursive: true});
const HERE = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const REPO = path.resolve(HERE, '..');
const DEVCMD = path.join(REPO, 'share', 'tools', 'dev_cmd.ps1');
const INST = 'auth0836';
const sleep = ms => new Promise(r => setTimeout(r, ms));
let failed = 0;
const check = (name, ok, detail = '') => { if (!ok) failed++; console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail !== '' ? '  -- ' + detail : ''}`); };

const salt = crypto.randomBytes(16);
const pwHash = `pbkdf2-sha256$310000$${salt.toString('hex')}$${crypto.pbkdf2Sync('Zizai-Test-2026', salt, 310000, 32, 'sha256').toString('hex')}`;
const now = Math.floor(Date.now() / 1000);
const G1 = 'a1b2c3d4e5f60708', G2 = 'b1b2c3d4e5f60708';
const DEV = 'demo0836device0000', OTHER = 'demo0001demo0001demo0001demo0001';
const dataDir = path.join(process.env.LOCALAPPDATA, `PhoneMirror-dev-${INST}`);
const tmpDir = path.join(process.env.TEMP, `zizai-${INST}`, '_tmp');
fs.rmSync(dataDir, {recursive: true, force: true});
fs.mkdirSync(dataDir, {recursive: true});
fs.rmSync(tmpDir, {recursive: true, force: true});
fs.mkdirSync(tmpDir, {recursive: true});
fs.writeFileSync(path.join(dataDir, 'settings.ini'),
  `[settings]\nlanguage=zh-TW\nremote_port=${18500 + Math.floor(Math.random() * 300)}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\n` +
  `toolbar_hint_shown=1\nright_click_hint_shown=1\nwhats_new_seen=0.8.3.9\nmiracast=0\nandroid_auto=0\nidle_tab=remote\n` +
  `remote_pw_hash=${pwHash}\nremote_fixed_secret=${crypto.randomBytes(32).toString('hex')}\nremote_persistent=1\nremote_fixed_approval=1\n`);
fs.writeFileSync(path.join(dataDir, 'remote_guests.txt'),
  `# test\n${G1}\tguest-4821\t${pwHash}\t${now - 600}\t${now + 86400}\t0\t0\t1\t0\n${G2}\tguest-7310\t${pwHash}\t${now - 600}\t${now + 86400}\t0\t0\t0\t0\n`);
fs.writeFileSync(path.join(dataDir, 'remote_devices.txt'), `${OTHER}\tWindows PC · Edge\t2026-10-12\n`);  // a 0.8.3.5 line (no scope)

const env = {...process.env, TEMP: tmpDir, TMP: tmpDir, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: INST, PM_REMOTE_DRYRUN: '1',
  PM_REMOTE_TESTCLIP: '1', PM_REMOTE_NOTS: '1', PM_REMOTE_LOOPBACK: '1', PM_SHARE_BIND: '127.0.0.1',
  PM_REMOTE_FUNNELFAKE: 'https://example-pc.tail000000.ts.net:8443/', PM_TEST_GUEST_OUT: path.join(tmpDir, 'copy.txt')};
const app = spawn(exe, ['--dev', '--test-offscreen', '--test-no-network'], {stdio: 'ignore', env, windowsHide: true});
const kill = () => { try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} };
const dev = (cmd, lp = 0, wait = 700) => { try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', DEVCMD, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe', windowsHide: true}); } catch {} };
const readLog = () => { try { return fs.readFileSync(path.join(dataDir, 'phonemirror.log'), 'utf8'); } catch { return ''; } };
const waitLog = async (re, ms) => { const end = Date.now() + ms; while (Date.now() < end) { if (re.test(readLog())) return true; await sleep(200); } return false; };
const devices = () => { try { return fs.readFileSync(path.join(dataDir, 'remote_devices.txt'), 'utf8'); } catch { return ''; } };
const ini = () => fs.readFileSync(path.join(dataDir, 'settings.ini'), 'utf8');
const altTab = () => {
  const ps = `Add-Type @"
using System; using System.Runtime.InteropServices;
public static class PmW7 { public delegate bool P(IntPtr h, IntPtr l);
 [DllImport("user32.dll")] public static extern bool EnumWindows(P cb, IntPtr l);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint c);
 [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
 public static int Count(uint pid) { int n = 0; EnumWindows((h, x) => { uint p; GetWindowThreadProcessId(h, out p);
  if (p == pid && IsWindowVisible(h)) { int ex = GetWindowLong(h, -20); if ((ex & 0x80) == 0 && (GetWindow(h, 4) == IntPtr.Zero || (ex & 0x40000) != 0)) n++; } return true; }, IntPtr.Zero); return n; } }
"@
[PmW7]::Count(${app.pid})`;
  try { return Number(execFileSync('powershell', ['-NoProfile', '-Command', ps], {encoding: 'utf8', windowsHide: true}).trim()); } catch { return -1; }
};
const P = (way, pol) => 470 + way * 3 + pol;
const ask = async (how, answer) => { dev(963, how, 900); dev(961, answer, 900); await sleep(300); };
const has = (id, scope) => devices().split(/\r?\n/).some(l => l.startsWith(id + '\t') && l.endsWith('\t' + scope));
const alt = [];
try {
  check('app started, remote control on (常駐外網)', await waitLog(/remote: on at/, 20000));
  await waitLog(/常駐外網 ready at|固定網址 ready at|fixed address ready at|ready at https:\/\/example-pc/, 15000);
  await sleep(1500);
  alt.push(altTab());

  // 1. settings
  dev(P(1, 1), 0, 600);
  const s1 = ini();
  check('0.8.3.5 settings.ini with 外面的新手機也要電腦按允許 on -> 主帳號 記住這台裝置, 臨時網址 每次問 (saved as words)',
        /remote_policy_fixed=remember/.test(s1) && /remote_policy_temp=ask/.test(s1), (s1.match(/remote_policy_\w+=\w+/g) || []).join(' '));
  check('menu row 臨時帳號 記住這台裝置: saved, logged, the host told',
        /remote_policy_guest=remember/.test(s1) && /連線時要不要允許: 臨時帳號 -> remember/.test(readLog()) && /連線時要不要允許: 主帳號 \(fixed address\) remember, 臨時帳號 remember, 臨時網址 ask/.test(readLog()));
  dev(P(0, 2), 0, 600);
  check('menu row 主帳號 只用密碼: saved (remote_fixed_approval=0 for older versions)', /remote_policy_fixed=password/.test(ini()) && /remote_fixed_approval=0/.test(ini()));
  check('the 0.8.3.5 list line is kept (LAN)', devices().startsWith(OTHER + '\t'));

  // 2. 每次問 vs 記住這台裝置
  dev(P(2, 0), 0, 400);
  await ask(15, 3);  // 臨時網址, 每次問: 記住 + 允許 asked for
  check('臨時網址 每次問: no 記住 box -> nothing written', !devices().includes(DEV), devices().trim());
  await ask(14, 3);  // 臨時帳號, 記住這台裝置
  check('臨時帳號 記住這台裝置: 記住 + 允許 -> the entry with that account\'s scope', has(DEV, `guest:${G1}`), devices().trim());
  await ask(14, 2);  // asked again (a made-up phone), 拒絕
  check('拒絕 -> that device\'s entries gone (the other phone stays)', !devices().includes(DEV) && devices().includes(OTHER), devices().trim());

  // 3. clearing rules
  await ask(14, 3);
  check('臨時帳號 記住 again', has(DEV, `guest:${G1}`));
  dev(460, 0, 2500);  // 重設密碼 of the first account (the panel shows the new one, offscreen)
  check('臨時帳號 重設密碼 -> its entries gone', !has(DEV, `guest:${G1}`) && /臨時帳號 重設密碼: 1 remembered devices forgotten/.test(readLog()), devices().trim());
  dev(984, 0, 600);  // close the panel
  await ask(14, 3);
  check('臨時帳號 記住 again (after 重設密碼)', has(DEV, `guest:${G1}`));
  dev(450, 0, 1200);  // 刪除 the first account
  check('臨時帳號 deleted -> its entries gone', !has(DEV, `guest:${G1}`) && /臨時帳號 gone: 1 remembered devices of it forgotten/.test(readLog()), devices().trim());
  dev(P(2, 1), 0, 400), dev(P(0, 1), 0, 400);
  await ask(15, 3);  // 臨時網址 記住
  await ask(16, 3);  // 主帳號 記住
  check('臨時網址 / 主帳號 記住這台裝置: entries with "temp" / "fixed"', has(DEV, 'temp') && has(DEV, 'fixed'), devices().trim());
  dev(963, 2, 6000);  // 變更密碼 (the password panel, filled and saved)
  await waitLog(/常駐外網 password set/, 8000);
  check('主帳號 password changed -> the fixed-address entry gone, the others stay', !has(DEV, 'fixed') && has(DEV, 'temp') && devices().includes(OTHER), devices().trim());
  dev(363, 0, 600);  // 已記住的手機 ▸ the first row (the 0.8.3.5 phone)
  check('one row forgotten from the menu', !devices().includes(OTHER) && has(DEV, 'temp'), devices().trim());

  // 4. 中斷 while the dialog is open (the bar / Ctrl+D / the menu: kick())
  dev(963, 15, 900);
  dev(340, 0, 900);  // CmdRemoteKick
  check('中斷 -> that device\'s entries gone', !devices().includes(DEV) && /中斷 on the PC: demo0836\.\.\. no longer remembered/.test(readLog()), devices().trim());
  dev(961, 2, 600);  // (the dialog, if still open)
  await ask(15, 3);
  dev(339, 0, 600);  // 全部忘記
  check('全部忘記 -> the file is gone', devices() === '' && !fs.existsSync(path.join(dataDir, 'remote_devices.txt')));
  alt.push(altTab());
  check('no window in Alt+Tab / the taskbar', alt.every(n => n === 0), alt.join(','));
} catch (e) { check('ran', false, String(e && e.stack || e)); }
dev(104, 0, 500);
for (let i = 0; i < 100 && app.exitCode === null; i++) await sleep(100);
if (app.exitCode === null) { kill(); console.log('quit did not end it: own PID killed'); }
fs.writeFileSync(path.join(outDir, 'app_auth0836.log'), readLog().split('\n').filter(l => /remote|dev remote/.test(l)).join('\n'));
fs.rmSync(dataDir, {recursive: true, force: true});
console.log(failed ? `FAILED (${failed} failed)` : 'ALL PASSED');
process.exit(failed ? 1 : 0);
