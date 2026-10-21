// 0.8.3.1: 常駐外網 right after a reboot, inside the real app: Tailscale still starting.
//   node funnel_wait_shots.mjs <自在投影.exe> <outDir> [--langs zh-TW,en,ja,ko] [--starting N]
// A --dev --test-offscreen --test-no-network instance (own data folder, PM_DEV_INSTANCE=remote0831fw),
// PM_REMOTE_LOOPBACK=1 (nothing listens on the LAN: no firewall prompt), input dry-run and
// PM_REMOTE_FUNNELSTARTING=N: the tailscale CLI is a stub inside the app (BackendState NoState for N
// checks, then Running with 8443 kept by tailscaled under a made-up name); no process is run, the
// real Funnel (443 / 8443) is never asked or touched. Turns 常駐外網 on, shoots the 遠端控制 page
// while it waits (「等待 Tailscale 啟動」, no 改用臨時網址) and once it is up; checks the log.
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';

const [exe, outDir, ...rest] = process.argv.slice(2);
let langs = ['zh-TW', 'en', 'ja', 'ko'], starting = 3;
for (let i = 0; i < rest.length; i++) {
  if (rest[i] === '--langs') langs = rest[++i].split(',');
  else if (rest[i] === '--starting') starting = +rest[++i];
}
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const devCmdPs1 = path.resolve(here, '..', '..', 'share', 'tools', 'dev_cmd.ps1');
const shotsDir = path.join(os.tmpdir(), 'pmshots');
const salt = crypto.randomBytes(16);
const pwHash = `pbkdf2-sha256$310000$${salt.toString('hex')}$${crypto.pbkdf2Sync('Zizai-Test-2026', salt, 310000, 32, 'sha256').toString('hex')}`;
const C = {Persistent: 353, TabRemote: 383};
const results = [];
let code = 0;
for (const lang of langs) {
  const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-remote0831fw');
  fs.mkdirSync(dataDir, {recursive: true});
  for (const f of ['remote_devices.txt', 'phonemirror.log']) { try { fs.rmSync(path.join(dataDir, f)); } catch {} }
  fs.writeFileSync(path.join(dataDir, 'settings.ini'),
    `[settings]\nlanguage=${lang}\nremote_port=${18500 + Math.floor(Math.random() * 300)}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\ntoolbar_hint_shown=1\nwhats_new_seen=0.8.3.1\n` +
    `remote_pw_hash=${pwHash}\nremote_fixed_secret=${crypto.randomBytes(32).toString('hex')}\n`);
  try { fs.rmSync(shotsDir, {recursive: true, force: true}); } catch {}
  const app = spawn(exe, ['--dev', '--test-offscreen', '--test-no-network'], {stdio: 'ignore', env: {...process.env, PM_VIDEO_OFFSCREEN: '1',
    PM_DEV_INSTANCE: 'remote0831fw', PM_REMOTE_DRYRUN: '1', PM_REMOTE_TESTCLIP: '1', PM_REMOTE_NOTS: '1', PM_REMOTE_LOOPBACK: '1', PM_REMOTE_FUNNELSTARTING: String(starting)}});
  log(lang, 'app pid', app.pid);
  const dev = (cmd, lp = 0, wait = 700) => { try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmdPs1, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe'}); } catch {} };
  const readLog = () => { try { return fs.readFileSync(path.join(dataDir, 'phonemirror.log'), 'utf8'); } catch { return ''; } };
  const waitLog = async (re, ms) => { const end = Date.now() + ms; while (Date.now() < end) { const m = readLog().match(re); if (m) return m; await sleep(250); } return null; };
  const grab = name => { for (const f of fs.existsSync(shotsDir) ? fs.readdirSync(shotsDir) : []) { fs.copyFileSync(path.join(shotsDir, f), path.join(outDir, `${lang}_${name}_${f}`)); fs.rmSync(path.join(shotsDir, f)); } };
  try {
    await sleep(4500);
    dev(C.TabRemote, 0, 1200);
    dev(C.Persistent, 0, 1500);  // 常駐外網 on (remote control on, the fixed link starts)
    const waiting = await waitLog(/常駐外網: waiting for Tailscale to start/, 8000);
    dev(910, 0, 1200); grab('1-waiting');
    dev(960, 0, 1200); grab('1-waiting-panel');
    const up = await waitLog(/常駐外網 ready at (\S+)/, 60000);
    dev(910, 0, 1500); grab('2-up');
    const L = readLog();
    const r = {lang, waitingLogged: !!waiting, upAt: up ? up[1] : null, failed: /Funnel failed/.test(L), tempOffered: /the page offers a temporary address/.test(L),
               waits: (L.match(/waiting for Tailscale to start \(try \d+ in \d+ s\)/g) || []).length, kept: /kept by tailscaled, nothing changed/.test(L)};
    results.push(r);
    log(lang, JSON.stringify(r));
  } catch (e) { console.error(e); code = 1; }
  dev(104, 0, 500);
  for (let i = 0; i < 100 && app.exitCode === null; i++) await sleep(100);
  if (app.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); log('killed own', app.pid); } catch {} }
  fs.writeFileSync(path.join(outDir, `${lang}_app.log`), readLog().split('\n').filter(l => /remote|funnel|dev remote/.test(l)).join('\n'));
}
const ok = results.length === langs.length && results.every(r => r.waitingLogged && r.upAt && !r.failed && !r.tempOffered && r.waits === starting && r.kept);
log(ok ? 'PASS' : 'FAIL', 'waiting shown, then up with the kept 8443, never "failed" / temporary address, in', results.map(r => r.lang).join(' '));
fs.writeFileSync(path.join(outDir, 'funnel_wait.json'), JSON.stringify({starting, results, ok}, null, 2));
process.exit(ok ? code : 1);
