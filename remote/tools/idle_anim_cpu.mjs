// Idle-screen animation CPU (0.8.2 item 4), 0.8.1 values vs 0.8.2 in the same build
// (PM_IDLE_ANIM=old = 40 s at 30 fps after activity; default 15 s at 20 fps).
// Own --dev --test-offscreen instance + data folder; "activity" = a WM_MOUSEMOVE
// posted to the app's own window (DevCommand 964): the real pointer never moves.
// Per variant: settled (no activity), 60 s with activity every 5 s (like the
// owner using the PC with the app window under the pointer), the 60 s after it.
//   node idle_anim_cpu.mjs <自在投影.exe> <outDir>
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

const [exe, outDir] = process.argv.slice(2);
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const devCmdPs1 = path.resolve(here, '..', '..', 'share', 'tools', 'dev_cmd.ps1');
const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-remote082cpu');
fs.mkdirSync(dataDir, {recursive: true});
const results = [];
for (const variant of ['old', 'new']) {
  try { fs.rmSync(path.join(dataDir, 'phonemirror.log')); } catch {}
  fs.writeFileSync(path.join(dataDir, 'settings.ini'), `[settings]\nlanguage=zh-TW\nremote_port=${8800 + Math.floor(Math.random() * 90)}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\ntoolbar_hint_shown=1\nwhats_new_seen=0.8.2\n`);
  const env = {...process.env, PM_DEV_INSTANCE: 'remote082cpu', PM_REMOTE_DRYRUN: '1', PM_REMOTE_NOTS: '1', PM_VIDEO_OFFSCREEN: '1'};
  if (variant === 'old') env.PM_IDLE_ANIM = 'old'; else delete env.PM_IDLE_ANIM;
  const app = spawn(exe, ['--dev', '--test-offscreen'], {stdio: 'ignore', env});
  const dev = (cmd, lp = 0, wait = 100) => { try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmdPs1, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe'}); } catch {} };
  const cpu = () => +execFileSync('powershell', ['-NoProfile', '-Command', `(Get-Process -Id ${app.pid}).TotalProcessorTime.TotalMilliseconds`], {encoding: 'utf8'}).trim();
  const window = async (name, ms, during) => {
    const a = cpu(), t0 = Date.now();
    if (during) await during(); else await sleep(ms);
    const wall = Date.now() - t0, b = cpu();
    const r = {variant, state: name, seconds: +(wall / 1000).toFixed(1), pctOneCore: +(((b - a) / wall) * 100).toFixed(2), cpuMs: Math.round(b - a)};
    console.log(JSON.stringify(r));
    results.push(r);
  };
  await sleep(50000);  // the start's own animation settles (old: 40 s)
  await window('settled (no activity)', 30000);
  await window('activity every 5 s (60 s)', 0, async () => { const end = Date.now() + 60000; let i = 0; while (Date.now() < end) { dev(964, i++); await sleep(5000); } });
  await window('the 60 s after the last activity', 60000);
  dev(104, 0, 500);
  for (let i = 0; i < 100 && app.exitCode === null; i++) await sleep(100);
  if (app.exitCode === null) try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
  await sleep(2000);
}
fs.writeFileSync(path.join(outDir, 'idle-anim-cpu.json'), JSON.stringify(results, null, 1));
const by = (v, s) => results.find(r => r.variant === v && r.state === s);
const lines = ['state | 0.8.1 values (40 s @ 30 fps) | 0.8.2 (15 s @ 20 fps)', '---|---|---'];
for (const s of [...new Set(results.map(r => r.state))]) lines.push(`${s} | ${by('old', s)?.pctOneCore}% of one core | ${by('new', s)?.pctOneCore}% of one core`);
const tot = v => results.filter(r => r.variant === v && r.state !== 'settled (no activity)').reduce((a, r) => a + r.cpuMs, 0);
lines.push(`CPU time for the 60 s of activity + 60 s after | ${tot('old')} ms | ${tot('new')} ms`);
fs.writeFileSync(path.join(outDir, 'idle-anim-cpu.md'), lines.join('\n') + '\n');
console.log(lines.join('\n'));
