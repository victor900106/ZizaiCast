// 0.8.3 shell screenshots, per language: the main window pages, every menu (tray, right-click,
// 設定, 語言 and the other submenus), the remote panels, the password window, the 新功能 note, the
// banner and (with --tunnel) the temporary-address panel. Own --dev --test-offscreen instance +
// data folder + TEMP folder per run (nothing shown on the desktop, no real input, no sound,
// PM_REMOTE_DRYRUN; the tailscale CLI and cloudflared are never run: PM_REMOTE_FUNNELFAKE /
// PM_REMOTE_TUNNELFAKE show made-up addresses, and PM_REMOTE_SHOWLAN / _SHOWTS made-up LAN ones;
// only 203.0.113.x and example hosts, RFC 5737 / 2606).
//   node tools/shell_shots.mjs <自在投影.exe> <outDir> [--langs zh-TW,en,ja,ko] [--tunnel] [--shift]
//   --tunnel: also the temporary-address panel (needs a build with PM_REMOTE_TUNNELFAKE)
//   --shift:  menus as if Shift were held (PM_SHIFT_MENU=1: the experimental rows)
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';

const [exe, outDir, ...rest] = process.argv.slice(2);
let langs = ['zh-TW', 'en', 'ja', 'ko'], tunnel = false, shift = false;
for (let i = 0; i < rest.length; i++) {
  if (rest[i] === '--langs') langs = rest[++i].split(',');
  else if (rest[i] === '--tunnel') tunnel = true;
  else if (rest[i] === '--shift') shift = true;
}
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const devCmdPs1 = path.resolve(here, '..', 'share', 'tools', 'dev_cmd.ps1');
const tmpDir = path.resolve(outDir, '_tmp');
const shotsDir = path.join(tmpDir, 'pmshots');
const LAN = 'http://203.0.113.10:18571/AbCdEfGhJkMnPqRsTuVwXyZ234567892/';
const TS = 'http://203.0.113.11:18571/AbCdEfGhJkMnPqRsTuVwXyZ234567892/';
const FUNNEL = 'https://example-pc.example.ts.net:8443/';
const TUNNEL = 'https://example-demo.example.com';
const salt = crypto.randomBytes(16);
const pwHash = `pbkdf2-sha256$310000$${salt.toString('hex')}$${crypto.pbkdf2Sync('Zizai-Test-2026', salt, 310000, 32, 'sha256').toString('hex')}`;
const C = {Toggle: 330, Tunnel: 352, Persistent: 353, FixedToggle: 356, LanQr: 359, TsQr: 360, TabCast: 382, TabRemote: 383}  // 0.8.3: CmdIdleTabCast / CmdIdleTabRemote (were 379 / 380 before remote-080 added three commands);
const started = [];

async function run(lang, tag, steps, seed = '') {
  const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-shellshots');
  fs.mkdirSync(dataDir, {recursive: true});
  fs.mkdirSync(shotsDir, {recursive: true});
  for (const f of ['remote_devices.txt', 'phonemirror.log']) { try { fs.rmSync(path.join(dataDir, f)); } catch {} }
  fs.writeFileSync(path.join(dataDir, 'remote_devices.txt'), 'aB3dE5fG7hJ9kL1m\tiPhone · Safari\t2026-10-20\nQ2w3E4r5T6y7U8i9\tAndroid · Chrome\t2026-10-21\n');
  fs.writeFileSync(path.join(dataDir, 'settings.ini'),
    `[settings]\nlanguage=${lang}\nremote_port=${18500 + Math.floor(Math.random() * 300)}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\ntoolbar_hint_shown=1\nwhats_new_seen=0.8.3\n${seed}`);
  const env = {...process.env, TEMP: tmpDir, TMP: tmpDir, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: 'shellshots', PM_REMOTE_DRYRUN: '1', PM_REMOTE_TESTCLIP: '1',
    PM_REMOTE_FUNNELFAKE: FUNNEL, PM_REMOTE_SHOWLAN: LAN, PM_REMOTE_SHOWTS: TS, ...(tunnel ? {PM_REMOTE_TUNNELFAKE: TUNNEL} : {}), ...(shift ? {PM_SHIFT_MENU: '1'} : {})};
  const app = spawn(exe, ['--dev', '--test-offscreen', '--demo-branding'], {stdio: 'ignore', env});
  started.push(app.pid);
  const dev = (cmd, lp = 0, wait = 700) => { try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmdPs1, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe'}); } catch {} };
  const readLog = () => { try { return fs.readFileSync(path.join(dataDir, 'phonemirror.log'), 'utf8'); } catch { return ''; } };
  const waitLog = async (re, ms) => { const end = Date.now() + ms; while (Date.now() < end) { const m = readLog().match(re); if (m) return m; await sleep(250); } return null; };
  const grab = name => {
    for (const f of fs.readdirSync(shotsDir)) {
      fs.copyFileSync(path.join(shotsDir, f), path.join(outDir, `${lang}_${tag}_${name}_${f}`));
      fs.rmSync(path.join(shotsDir, f));
    }
  };
  try {
    await sleep(4500);
    await steps({dev, grab, waitLog});
  } catch (e) { console.error(lang, tag, e); }
  dev(104, 0, 500);  // quit
  for (let i = 0; i < 100 && app.exitCode === null; i++) await sleep(100);
  if (app.exitCode === null) try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
}

for (const lang of langs) {
  await run(lang, 'a', async ({dev, grab}) => {
    dev(C.TabCast, 0, 1500); dev(910, 0, 1200); grab('01-cast');
    dev(901, 0, 4000); grab('02-menus');  // tray, right-click, 設定, 語言 and every submenu
    dev(912, 0, 1500); grab('03-shortcuts');
    dev(C.TabRemote, 0, 1500); dev(910, 0, 1200); grab('04-remote-off');
    dev(C.Toggle, 0, 2500); dev(C.LanQr, 0, 5500); dev(910, 0, 1200); grab('05-remote-on-lanqr');
    dev(963, 8, 900); grab('06-advanced');
    dev(963, 1, 1200); dev(963, 5, 900); grab('07-password');
    dev(963, 7, 600); dev(963, 5, 900); grab('08-password-short');
  });
  await run(lang, 'b', async ({dev, grab, waitLog}) => {
    dev(C.TabRemote, 0, 1200); dev(910, 0, 1200); grab('10-pw-set-off');
    dev(C.Toggle, 0, 2500); dev(C.TsQr, 0, 5500); dev(910, 0, 1200); grab('11-pw-set-on-tsqr');
    dev(C.Persistent, 0, 1000);  // 常駐外網 ✓ (also starts the fixed address; made-up: nothing is run)
    await waitLog(/常駐外網 ready at (\S+)/, 20000);
    await sleep(4800);  // the toast is gone
    dev(C.TsQr, 0, 3000); dev(910, 0, 1500); grab('12-fixed-ready');
    dev(963, 5, 1500); grab('13-fixed-tab');
    if (tunnel) { dev(C.Tunnel, 0, 3000); dev(960, 0, 1200); grab('14-tunnel'); }
    dev(963, 6, 1500); dev(963, 5, 900); grab('15-whats-new'); dev(963, 9, 600);
    dev(963, 10, 900); grab('16-banner');
    dev(C.Persistent, 0, 2500);  // off again
  }, `remote_pw_hash=${pwHash}\nremote_fixed_secret=${crypto.randomBytes(32).toString('hex')}\n`);
}
try { fs.rmSync(tmpDir, {recursive: true, force: true}); } catch {}
console.log('started pids:', started.join(','));
