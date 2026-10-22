// 0.8.3.5 固定網址: remote control off -> on in quick succession must leave the fixed address up
// (owner log 2026-10-21 21:37:24.314 off, 24.805 on, 25.080 "funnel: port 8443 off"), node 22+.
//   node app0835_funnel_test.mjs <自在投影.exe> <pm_fake_tailscale.exe> <outDir>
// The app against a FAKE tailscale CLI (pm_fake_tailscale.exe, real processes; tailscale itself is never
// run, nothing is changed anywhere): its "funnel --https=8443 off" takes 1.5 s (PM_FAKE_TS_OFF_MS), its
// 8443 is a line in a state file. Own --dev --test-offscreen instance (WS_EX_TOOLWINDOW, never
// activated: checked with tools/alttab_check.ps1) and data folder, PM_REMOTE_LOOPBACK, PM_REMOTE_DRYRUN
// (no input is injected), PM_REMOTE_NOTS. Only this script's own PID is ever ended.
// Cases:
//   off-on       開/關 off, 0.7 s later on (debounce set to 300 ms): the slow off runs first, then the on
//                sets 8443 up again; 8443 -> our origin at the end, the fixed address ready again
//   double       a double click on the switch (2nd click 0.25 s later, the real debounce): the 2nd is
//                ignored (logged), remote control stays off, 8443 gone at the end
//   not-ours     8443 taken over by another program while ours was up, then off: left alone
//   on-again     after "double": on again -> 常駐外網 starts with it (0.8.3.4 did not), 8443 ours again
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';

const [exe, fakeCli, outDir] = process.argv.slice(2);
if (!exe || !fakeCli || !outDir) { console.error('usage: node app0835_funnel_test.mjs <自在投影.exe> <pm_fake_tailscale.exe> <outDir>'); process.exit(2); }
if (path.basename(fakeCli) !== 'pm_fake_tailscale.exe') { console.error('the fake CLI must be pm_fake_tailscale.exe'); process.exit(2); }
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const devCmdPs1 = path.resolve(here, '..', '..', 'share', 'tools', 'dev_cmd.ps1');
const altTabPs1 = path.resolve(here, '..', '..', 'tools', 'alttab_check.ps1');
const tmpDir = path.resolve(outDir, '_tmp');
const C = {Toggle: 330, Exit: 104};
const salt = crypto.randomBytes(16);
const pwHash = `pbkdf2-sha256$310000$${salt.toString('hex')}$${crypto.pbkdf2Sync('Zizai-Test-2026', salt, 310000, 32, 'sha256').toString('hex')}`;

let failed = 0;
const check = (name, ok, detail = '') => { console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? '  -- ' + detail : ''}`); if (!ok) failed++; };

async function run(debounceMs, body) {
  const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-funnel0835');
  fs.rmSync(dataDir, {recursive: true, force: true});
  fs.mkdirSync(dataDir, {recursive: true});
  fs.rmSync(tmpDir, {recursive: true, force: true});
  fs.mkdirSync(path.join(tmpDir, 'pmshots'), {recursive: true});
  const port = 18800 + Math.floor(Math.random() * 300);
  const pidsFile = path.join(tmpDir, 'pids.txt'), stateFile = path.join(tmpDir, 'state.txt');
  fs.writeFileSync(path.join(dataDir, 'settings.ini'),
    `[settings]\nlanguage=zh-TW\nremote_port=${port}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\ntoolbar_hint_shown=1\n` +
    `whats_new_seen=0.8.3\nidle_tab=remote\nremote_persistent=1\nremote_pw_hash=${pwHash}\nremote_fixed_secret=${crypto.randomBytes(32).toString('hex')}\n`);
  const env = {...process.env, TEMP: tmpDir, TMP: tmpDir, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: 'funnel0835', PM_REMOTE_DRYRUN: '1', PM_REMOTE_NOTS: '1',
    PM_REMOTE_LOOPBACK: '1', PM_REMOTE_FAKECLI: fakeCli, PM_FAKE_TS_MODE: 'ok', PM_FAKE_TS_PIDS: pidsFile, PM_FAKE_TS_STATE: stateFile, PM_FAKE_TS_OFF_MS: '1500'};
  if (debounceMs !== null) env.PM_REMOTE_DEBOUNCE_MS = String(debounceMs);
  const app = spawn(exe, ['--dev', '--test-offscreen'], {stdio: 'ignore', env, windowsHide: true});
  const dev = (cmd, lp = 0, wait = 50) => { try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmdPs1, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe', windowsHide: true}); } catch {} };
  const readLog = () => { try { return fs.readFileSync(path.join(dataDir, 'phonemirror.log'), 'utf8'); } catch { return ''; } };
  const count = re => (readLog().match(new RegExp(re.source, 'g')) || []).length;
  const waitFor = async (fn, ms) => { const end = Date.now() + ms; while (Date.now() < end) { const v = fn(); if (v) return v; await sleep(100); } return null; };
  const state = () => { try { return fs.readFileSync(stateFile, 'utf8').trim(); } catch { return ''; } };
  const fakeRuns = () => { try { return fs.readFileSync(pidsFile, 'utf8').trim().split('\n').filter(Boolean).length; } catch { return 0; } };
  const origin = `http://127.0.0.2:${port}`;
  const ctx = {dev, readLog, count, waitFor, state, origin, stateFile, app, fakeRuns};
  try {
    check('dev instance: the fake CLI is used, not tailscale', !!(await waitFor(() => /常駐外網 \(PM_REMOTE_FAKECLI\): a fake tailscale CLI, not tailscale/.test(readLog()), 15000)));
    check('start: 常駐外網 ready, 8443 -> our origin', !!(await waitFor(() => /常駐外網 ready at https:\/\/example-pc\.example\.ts\.net:8443\//.test(readLog()), 20000)) && state() === origin, state());
    try {
      const out = execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', altTabPs1, '-ProcessId', String(app.pid)], {encoding: 'utf8', windowsHide: true});
      const m = out.match(/alttab \d+ (\d+)/);
      check('no window of the test instance in Alt+Tab / the taskbar', !!m && m[1] === '0', out.trim().split('\n')[0]);
    } catch (e) { check('alttab_check ran', false, String(e).slice(0, 120)); }
    await sleep(1000);  // (clicks well after the start)
    await body(ctx);
  } catch (e) { check('no exception', false, String(e && e.stack || e)); }
  dev(C.Exit, 0, 300);
  for (let i = 0; i < 150 && app.exitCode === null; i++) await sleep(100);
  if (app.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} check('quit', false, 'killed'); }
  await sleep(300);
  const log = readLog();
  fs.writeFileSync(path.join(outDir, `app0835_funnel_${debounceMs === null ? 'default' : debounceMs}.log`), log);
  // Every CLI run was the fake one (its PID list) and none was anything but status / funnel.
  const runs = (() => { try { return fs.readFileSync(pidsFile, 'utf8').trim().split('\n').filter(Boolean); } catch { return []; } })();
  const odd = runs.filter(l => !/^\d+ (status --json|funnel status --json|funnel --bg --https=8443 http:\/\/127\.0\.0\.2:\d+|funnel --https=8443 off)$/.test(l.trim()));
  check('only status / funnel status / funnel --bg 8443 / funnel 8443 off were run (fake CLI)', odd.length === 0, `${runs.length} runs` + (odd.length ? ', odd: ' + odd.join(' | ') : ''));
  return log;
}

// off -> on 0.7 s apart (the debounce at 300 ms lets the 2nd through): the slow off first, then the on.
await run(300, async ({dev, readLog, count, waitFor, state, origin}) => {
  const ready0 = count(/常駐外網 ready at/);
  dev(`${C.Toggle},${C.Toggle}`, 0, 700);  // off, 0.7 s later on (one dev_cmd run: exact spacing) while the fake "off" (1.5 s) runs
  check('off-on: remote control on again', !!(await waitFor(() => (readLog().match(/remote: (on at|off)\b/g) || []).slice(-1)[0] === 'remote: on at', 5000)));
  check('off-on: 常駐外網 started with it', !!(await waitFor(() => count(/常駐外網 ready at/) > ready0, 15000)));
  await sleep(2500);  // anything still queued would have run by now
  const log = readLog();
  const iOff = log.lastIndexOf('Funnel port 8443 removed (remote control turned off)'), iBg = log.lastIndexOf('funnel --bg --https=8443');
  check('off-on: the off finished before the on set 8443 up again', iOff >= 0 && iBg > iOff, `off at ${iOff}, --bg at ${iBg}`);
  check('off-on: 8443 -> our origin at the end (fixed address still up)', state() === origin, JSON.stringify(state()));
});

// The real debounce: a double click (2nd click 0.25 s later) is one click; then on again; then not-ours.
await run(null, async ({dev, readLog, count, waitFor, state, origin, stateFile}) => {
  dev(`${C.Toggle},${C.Toggle}`, 0, 250);  // off, and the double click's 2nd click 0.25 s later
  check('double: the 2nd click is ignored (防抖)', !!(await waitFor(() => /remote: command 330 ignored: \d+ ms after remote control \/ the fixed address was turned off/.test(readLog()), 3000)));
  await sleep(2500);
  check('double: remote control stays off', (readLog().match(/remote: (on at|off)\b/g) || []).slice(-1)[0] === 'remote: off');
  check('double: 8443 removed (it was ours)', state() === '', JSON.stringify(state()));
  const ready0 = count(/常駐外網 ready at/);
  dev(C.Toggle);  // on again, well after the debounce
  check('on-again: 常駐外網 comes back with remote control (0.8.3.4: only at the next start)', !!(await waitFor(() => count(/常駐外網 ready at/) > ready0, 15000)) && state() === origin, JSON.stringify(state()));
  // not-ours: another program takes 8443 over; turning off must leave it alone.
  fs.writeFileSync(stateFile, 'http://127.0.0.1:9000\n');
  await sleep(900);
  dev(C.Toggle);  // off
  check('not-ours: logged as not ours, left alone', !!(await waitFor(() => /Funnel port 8443 not ours \(or already gone\): left alone \(remote control turned off\)/.test(readLog()), 6000)));
  check('not-ours: the other program\'s 8443 is still there', state() === 'http://127.0.0.1:9000', JSON.stringify(state()));
});

try { fs.rmSync(tmpDir, {recursive: true, force: true}); } catch {}
console.log(failed ? `FAILED (${failed} failed)` : 'ALL PASSED');
process.exit(failed ? 1 : 0);
