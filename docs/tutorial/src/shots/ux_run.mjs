// UX audit / tutorial screenshots: own --dev --test-offscreen --test-no-network instance per run,
// own data folder (PhoneMirror-dev-uxaudit) and TEMP, loopback remote (PM_REMOTE_LOOPBACK=1),
// input dry-run, fake Funnel / tunnel addresses (the tailscale CLI and cloudflared never run).
// Window placement checked against every monitor after start and before quit.
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

// PM_EXE = the 自在投影.exe to run (a build of this tree); UX_WORK = scratch folder (TEMP of the
// instance, logs, feeds, Chrome profile); UX_OUT = where the audit shots go.
const HERE = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
export const REPO = path.resolve(HERE, '..', '..', '..', '..');
export const EXE = process.env.PM_EXE;
if (!EXE) { console.error('set PM_EXE to the 自在投影.exe to run'); process.exit(1); }
export const WORK = process.env.UX_WORK || path.join(process.env.TEMP, 'zizai-ux-shots');
fs.mkdirSync(WORK, {recursive: true});
export const OUT = process.env.UX_OUT || path.join(REPO, '_shots', 'ux');
const DEVCMD = path.join(REPO, 'share', 'tools', 'dev_cmd.ps1');
const OFFCHECK = path.join(HERE, 'offcheck.ps1');
export const sleep = ms => new Promise(r => setTimeout(r, ms));
export const LAN = 'http://203.0.113.10:18571/AbCdEfGhJkMnPqRsTuVwXyZ234567892/';
export const TS = 'http://203.0.113.11:18571/AbCdEfGhJkMnPqRsTuVwXyZ234567892/';
export const FUNNEL = 'https://example-pc.example.ts.net:8443/';
export const TUNNEL = 'https://example-demo.example.com';
export const started = [];

function ps(file, args) {
  try { return execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', file, ...args], {encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe']}); }
  catch (e) { return (e.stdout || '') + (e.stderr || '') + `\n[exit ${e.status}]`; }
}

export async function run({tag, lang = 'zh-TW', args = [], env = {}, settings = null, seed = '', steps, outDir = OUT, startWait = 4500}) {
  const inst = 'uxaudit';
  const dataDir = path.join(process.env.LOCALAPPDATA, `PhoneMirror-dev-${inst}`);
  const tmpDir = path.join(WORK, '_tmp');
  const shotsDir = path.join(tmpDir, 'pmshots');
  fs.rmSync(dataDir, {recursive: true, force: true});
  fs.mkdirSync(dataDir, {recursive: true});
  fs.rmSync(shotsDir, {recursive: true, force: true});
  fs.mkdirSync(shotsDir, {recursive: true});
  fs.mkdirSync(outDir, {recursive: true});
  const ini = settings !== null ? settings
    : `[settings]\nlanguage=${lang}\nremote_port=${18500 + Math.floor(Math.random() * 300)}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\ntoolbar_hint_shown=1\nright_click_hint_shown=1\nwhats_new_seen=0.8.3\nmiracast=0\nandroid_auto=0\n${seed}`;
  fs.writeFileSync(path.join(dataDir, 'settings.ini'), ini);
  const fullEnv = {...process.env, TEMP: tmpDir, TMP: tmpDir, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: inst,
    PM_REMOTE_DRYRUN: '1', PM_REMOTE_LOOPBACK: '1', PM_REMOTE_TESTCLIP: '1', PM_SHARE_BIND: '127.0.0.1',
    PM_REMOTE_FUNNELFAKE: FUNNEL, PM_REMOTE_SHOWLAN: LAN, PM_REMOTE_SHOWTS: TS, PM_REMOTE_TUNNELFAKE: TUNNEL, ...env};
  for (const k of Object.keys(fullEnv)) if (fullEnv[k] === null) delete fullEnv[k];
  const app = spawn(EXE, ['--dev', '--test-offscreen', '--test-no-network', '--demo-branding', ...args], {stdio: 'ignore', env: fullEnv});
  started.push(app.pid);
  console.log(`[${tag}] pid ${app.pid} ${args.join(' ')}`);
  const kill = () => { try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} };
  const offcheck = when => {
    const o = ps(OFFCHECK, ['-ProcessId', String(app.pid)]);
    if (/ON-SCREEN/.test(o)) { console.log(`[${tag}] ${when}: WINDOW ON A MONITOR -> kill own pid\n${o}`); kill(); throw new Error('on-screen'); }
    return o.trim();
  };
  const dev = (cmd, lp = 0, wait = 700) => { ps(DEVCMD, ['-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)]); };
  const logPath = path.join(dataDir, 'phonemirror.log');
  const readLog = () => { try { return fs.readFileSync(logPath, 'utf8'); } catch { return ''; } };
  const waitLog = async (re, ms) => { const end = Date.now() + ms; while (Date.now() < end) { const m = readLog().match(re); if (m) return m; await sleep(250); } return null; };
  const grab = name => {
    const got = [];
    for (const f of fs.readdirSync(shotsDir)) {
      if (!/\.png$/.test(f)) continue;
      const dst = path.join(outDir, `${tag}_${name}_${f}`);
      fs.copyFileSync(path.join(shotsDir, f), dst);
      fs.rmSync(path.join(shotsDir, f));
      got.push(path.basename(dst));
    }
    console.log(`[${tag}] ${name}: ${got.join(', ') || '(nothing)'}`);
    return got;
  };
  const win = async (name, wait = 900) => { dev(910, 0, wait); return grab(name); };
  try {
    await sleep(startWait);
    console.log(`[${tag}] start: ${offcheck('start')}`);
    await steps({dev, grab, win, waitLog, readLog, offcheck, pid: app.pid, dataDir});
    console.log(`[${tag}] end: ${offcheck('end')}`);
  } catch (e) { console.error(`[${tag}]`, e.message); }
  dev(104, 0, 500);  // quit
  for (let i = 0; i < 100 && app.exitCode === null; i++) await sleep(100);
  if (app.exitCode === null) { console.log(`[${tag}] quit did not end it: taskkill own pid`); kill(); }
  fs.mkdirSync(path.join(WORK, 'logs'), {recursive: true});
  try { fs.copyFileSync(logPath, path.join(WORK, 'logs', `${tag}.log`)); } catch {}
}
