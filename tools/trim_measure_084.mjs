// 0.8.4 trim: before / after numbers from a real --dev --test-offscreen instance (own data folder, nothing visible, no sound).
//   node tools/trim_measure_084.mjs <自在投影.exe> <label> [seconds=25]
// Settings: miracast default (on), android_paired=0 (never paired), adb on its own port (PM_ADB_PORT), remote control turned on
// (dry-run input, no Tailscale) so the host's 5 s statistics loop runs with no viewer. Reports from the log:
//   miracast lines at start, "adb server started" before / after the Android menu is built (DevCommand 975), "idle (no viewer)" lines,
//   Alt+Tab / taskbar windows of the process (tools/alttab_check.ps1). Only the PID started here is ended.
import { spawn, execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

const [exe, label, secArg] = process.argv.slice(2);
const seconds = +(secArg || 25);
const sleep = ms => new Promise(r => setTimeout(r, ms));
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const devCmdPs1 = path.resolve(here, '..', 'share', 'tools', 'dev_cmd.ps1');
const altTabPs1 = path.resolve(here, 'alttab_check.ps1');
const inst = 'trim084' + label.replace(/\W/g, '');
const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-' + inst);
fs.rmSync(dataDir, { recursive: true, force: true });
fs.mkdirSync(dataDir, { recursive: true });
fs.writeFileSync(path.join(dataDir, 'settings.ini'),
  `[settings]\nlanguage=zh-TW\nremote_port=${19100 + Math.floor(Math.random() * 300)}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\ntoolbar_hint_shown=1\nwhats_new_seen=0.8.3.9\nandroid_paired=0\n`);
const env = { ...process.env, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: inst, PM_REMOTE_DRYRUN: '1', PM_REMOTE_NOTS: '1', PM_REMOTE_LOOPBACK: '1', PM_ADB_PORT: '15999' };
const adbPids = () => { try { return execFileSync('powershell', ['-NoProfile', '-Command', "(Get-Process adb -ErrorAction SilentlyContinue | ForEach-Object Id) -join ','"], { encoding: 'utf8' }).trim(); } catch { return '?'; } };
const adbBefore = adbPids();
const app = spawn(exe, ['--dev', '--test-offscreen'], { stdio: 'ignore', env, windowsHide: true });
const dev = (cmd, wait = 600) => { try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmdPs1, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', '0', '-Wait', String(wait)], { stdio: 'pipe' }); } catch (e) { } };
const logFile = path.join(dataDir, 'phonemirror.log');
const readLog = () => { try { return fs.readFileSync(logFile, 'utf8'); } catch { return ''; } };
const count = (s, re) => (s.match(re) || []).length;
const r = { label, exe: path.basename(path.dirname(exe)) };
await sleep(6000);
let log = readLog();
r.miracastLinesAtStart = count(log, /miracast/gi);
r.miracastLines = log.split('\n').filter(l => /miracast/i.test(l)).map(l => l.slice(20, 110));
r.adbStartedBeforeMenu = count(log, /adb server started/g);
r.adbProcsAfterStart = adbPids();
dev(330, 1000);  // 遠端控制 on: the host's statistics loop runs (no viewer)
await sleep(Math.max(0, seconds - 8) * 1000);
try { r.altTab = execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', altTabPs1, '-ProcessId', String(app.pid)], { encoding: 'utf8' }).trim().split('\n')[0]; } catch (e) { r.altTab = 'n/a'; }
log = readLog();
r.idleLines = count(log, /idle \(no viewer\)/g);
r.secondsWithRemoteOn = seconds - 7;
dev(975, 1500);  // builds the Android menu (as opening it does)
await sleep(2500);
log = readLog();
r.adbStartedAfterMenu = count(log, /adb server started/g);
r.logLinesTotal = log.split('\n').filter(Boolean).length;
dev(104, 300);  // 結束
for (let i = 0; i < 100 && app.exitCode === null; i++) await sleep(100);
if (app.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], { stdio: 'ignore' }); } catch { } r.killed = true; }
await sleep(800);
r.adbBefore = adbBefore; r.adbAfterQuit = adbPids();
console.log(JSON.stringify(r, null, 1));
