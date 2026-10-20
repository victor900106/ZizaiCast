// 0.8.2 main-window screenshots, per language: the 手機投影 and 遠端控制 pages in
// their states, the tray menu, 進階設定, the password window and the 新功能 note.
// Own --dev --test-offscreen instance + data folder per language (nothing shown
// on the desktop, no real input, no sound: PM_REMOTE_DRYRUN, input dry-run).
// --funnel: also the 固定網址 card when ready (Tailscale Funnel :8443 on and off
// again; 443 checked before / after). --fake-funnel URL: the same card with a made-up
// address (PM_REMOTE_FUNNELFAKE: Tailscale is never run; use this for published shots),
// and made-up Wi-Fi / Tailscale addresses on the 遠端控制 page (PM_REMOTE_SHOWLAN / _SHOWTS).
// --before EXE: the 0.8.1 window for comparison.
//   node ui_shots.mjs <自在投影.exe> <outDir> [--langs zh-TW,en,ja,ko] [--funnel | --fake-funnel URL] [--before <0.8.1 exe>]
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';

const [exe, outDir, ...rest] = process.argv.slice(2);
let langs = ['zh-TW', 'en', 'ja', 'ko'], funnel = false, fakeFunnel = '', before = '';
for (let i = 0; i < rest.length; i++) {
  if (rest[i] === '--langs') langs = rest[++i].split(',');
  else if (rest[i] === '--funnel') funnel = true;
  else if (rest[i] === '--fake-funnel') funnel = true, fakeFunnel = rest[++i];
  else if (rest[i] === '--before') before = rest[++i];
}
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const devCmdPs1 = path.resolve(here, '..', '..', 'share', 'tools', 'dev_cmd.ps1');
const shotsDir = path.join(os.tmpdir(), 'pmshots');
const TS = 'C:\\Program Files\\Tailscale\\tailscale.exe';
const funnelJson = () => { try { return execFileSync(TS, ['funnel', 'status', '--json'], {encoding: 'utf8'}); } catch { return ''; } };
const realFunnel = funnel && !fakeFunnel;
const fBefore = realFunnel ? funnelJson() : '';
if (realFunnel && /:8443"/.test(fBefore)) { console.error('8443 in use: not touching it'); process.exit(2); }
// A real PBKDF2 hash in the app's format (the password itself is never shown).
const salt = crypto.randomBytes(16);
const pwHash = `pbkdf2-sha256$310000$${salt.toString('hex')}$${crypto.pbkdf2Sync('Zizai-Test-2026', salt, 310000, 32, 'sha256').toString('hex')}`;
const C = {Toggle: 330, Persistent: 353, FixedToggle: 356, LanQr: 359, TsQr: 360, TabCast: 382, TabRemote: 383}  // 0.8.3: CmdIdleTabCast / CmdIdleTabRemote (were 379 / 380 before remote-080 added three commands);

async function run(exePath, lang, tag, steps, seed = '') {
  const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-remote082ui');
  fs.mkdirSync(dataDir, {recursive: true});
  for (const f of ['remote_devices.txt', 'phonemirror.log']) { try { fs.rmSync(path.join(dataDir, f)); } catch {} }
  fs.writeFileSync(path.join(dataDir, 'remote_devices.txt'), 'aB3dE5fG7hJ9kL1m\tiPhone · Safari\t2026-10-20\nQ2w3E4r5T6y7U8i9\tAndroid · Chrome\t2026-10-21\n');
  fs.writeFileSync(path.join(dataDir, 'settings.ini'),
    `[settings]\nlanguage=${lang}\nremote_port=${18500 + Math.floor(Math.random() * 300)}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\ntoolbar_hint_shown=1\nwhats_new_seen=0.8.3\n${seed}`);
  const app = spawn(exePath, ['--dev', '--test-offscreen'], {stdio: 'ignore', env: {...process.env, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: 'remote082ui', PM_REMOTE_DRYRUN: '1', PM_REMOTE_TESTCLIP: '1', ...(fakeFunnel ? {PM_REMOTE_FUNNELFAKE: fakeFunnel, PM_REMOTE_SHOWLAN: 'http://192.168.0.10:18571/AbCdEfGhJkMnPqRsTuVwXyZ234567892/', PM_REMOTE_SHOWTS: 'http://100.64.0.10:18571/AbCdEfGhJkMnPqRsTuVwXyZ234567892/'} : {})}});
  const dev = (cmd, lp = 0, wait = 700) => { try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmdPs1, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe'}); } catch {} };
  const readLog = () => { try { return fs.readFileSync(path.join(dataDir, 'phonemirror.log'), 'utf8'); } catch { return ''; } };
  const waitLog = async (re, ms) => { const end = Date.now() + ms; while (Date.now() < end) { const m = readLog().match(re); if (m) return m; await sleep(250); } return null; };
  const title = () => { try { return execFileSync('powershell', ['-NoProfile', '-Command', `[Console]::OutputEncoding=[Text.Encoding]::UTF8; (Get-Process -Id ${app.pid}).MainWindowTitle`], {encoding: 'utf8'}).trim(); } catch { return ''; } };
  const grab = name => {
    for (const f of fs.existsSync(shotsDir) ? fs.readdirSync(shotsDir) : []) {
      const src = path.join(shotsDir, f);
      fs.copyFileSync(src, path.join(outDir, `${lang}_${tag}_${name}_${f}`));
      fs.rmSync(src);
    }
  };
  const titles = [];
  try {
    await sleep(4500);
    await steps({dev, grab, waitLog, title: () => titles.push(title())});
  } catch (e) { console.error(lang, tag, e); }
  dev(104, 0, 500);
  for (let i = 0; i < 100 && app.exitCode === null; i++) await sleep(100);
  if (app.exitCode === null) try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
  return titles;
}

const report = [];
try { fs.rmSync(shotsDir, {recursive: true, force: true}); } catch {}
for (const lang of langs) {
  if (before) {
    await run(before, lang, 'before', async ({dev, grab}) => { dev(910, 0, 1500); grab('idle'); });
  }
  const t1 = await run(exe, lang, 'after', async ({dev, grab, waitLog, title}) => {
    dev(C.TabCast, 0, 1500); dev(910, 0, 1200); grab('1-cast');
    dev(C.TabRemote, 0, 1500); dev(910, 0, 1200); grab('2-remote-off'); title();
    dev(C.Toggle, 0, 2500); dev(C.LanQr, 0, 5500); dev(910, 0, 1200); grab('3-remote-on-lanqr'); title();
    dev(900, 0, 900); grab('4-tray');
    dev(963, 8, 900); grab('5-advanced');
    dev(963, 1, 1200); dev(963, 5, 900); grab('6-password');
    dev(963, 7, 600); dev(963, 5, 900); grab('7-password-short');
  });
  // With the password set: the fixed card first (+ Tailscale QR), optionally ready.
  const t2 = await run(exe, lang, 'after', async ({dev, grab, waitLog, title}) => {
    dev(C.TabRemote, 0, 1200); dev(910, 0, 1200); grab('8-pw-set-off');
    dev(C.Toggle, 0, 2500); dev(C.TsQr, 0, 5500); dev(910, 0, 1200); grab('9-pw-set-on-tsqr'); title();
    if (funnel) {
      dev(C.Persistent, 0, 1000);  // 常駐外網 ✓ (also starts the fixed link; autostart is never touched by --dev)
      const m = await waitLog(/常駐外網 ready at (\S+)/, 30000);
      dev(C.TsQr, 0, 5500);  // (TS QR off again; the toasts gone)
      dev(910, 0, 1500); grab('10-fixed-ready'); title();
      dev(C.Persistent, 0, 3000);  // off again: 8443 removed
      if (realFunnel) await waitLog(/Funnel port 8443 removed/, 15000);
      report.push(`${lang}: fixed ready ${m ? m[1] : 'NO'}`);
    }
    dev(963, 6, 1500); dev(963, 5, 900); grab('11-whats-new'); dev(963, 9, 600);
    if (fakeFunnel) { dev(963, 10, 900); grab('12-banner-example'); }
  }, `remote_pw_hash=${pwHash}\nremote_fixed_secret=${crypto.randomBytes(32).toString('hex')}\n`);
  report.push(`${lang}: titles ${JSON.stringify([...t1, ...t2])}`);
}
if (realFunnel) {
  const fAfter = funnelJson();
  report.push('funnel config identical before / after: ' + (fAfter.replace(/\s/g, '') === fBefore.replace(/\s/g, '')));
}
fs.writeFileSync(path.join(outDir, 'ui-shots.txt'), report.join('\n') + '\n');
console.log(report.join('\n'));
