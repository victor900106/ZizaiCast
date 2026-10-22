// 0.8.4 trim: before / after menu screenshots (tray menu + every submenu + 放大鏡 / 翻譯 / 管理翻譯模型), rendered to PNG by the app
// itself (DevCommand 901 / 907: no window is shown, no mouse). Own --dev --test-offscreen instance, own data folder and TEMP.
//   node tools/shots_084trim.mjs <自在投影.exe> <outDir> <before|after> [lang=zh-TW]
import { spawn, execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

const [exe, outRoot, which, lang = 'zh-TW'] = process.argv.slice(2);
const sleep = ms => new Promise(r => setTimeout(r, ms));
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const devCmdPs1 = path.resolve(here, '..', 'share', 'tools', 'dev_cmd.ps1');
const inst = 'shots084' + which;
const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-' + inst);
const tmp = path.join(process.env.TEMP, 'zizai-shots084' + which);
for (const d of [dataDir, tmp]) { fs.rmSync(d, { recursive: true, force: true }); fs.mkdirSync(d, { recursive: true }); }
fs.writeFileSync(path.join(dataDir, 'settings.ini'), `[settings]\nlanguage=${lang}\nremote_port=${19400 + Math.floor(Math.random() * 300)}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\ntoolbar_hint_shown=1\nwhats_new_seen=0.8.9\nandroid_paired=0\n`);
const env = { ...process.env, TEMP: tmp, TMP: tmp, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: inst, PM_ADB_PORT: '15998' };
const app = spawn(exe, ['--dev', '--test-offscreen', '--test-no-network'], { stdio: 'ignore', env, windowsHide: true });
const dev = (cmd, wait = 1500) => { try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmdPs1, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', '0', '-Wait', String(wait)], { stdio: 'pipe' }); } catch { } };
await sleep(5000);
dev(901, 4000);
dev(907, 2500);
dev(104, 300);
for (let i = 0; i < 100 && app.exitCode === null; i++) await sleep(100);
if (app.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], { stdio: 'ignore' }); } catch { } }
const out = path.join(outRoot, which);
fs.mkdirSync(out, { recursive: true });
const src = path.join(tmp, 'pmshots');
let n = 0;
for (const f of fs.existsSync(src) ? fs.readdirSync(src) : []) if (/^menu_(tray|context|settings|magnifier|translate|models|sub\d+)\.png$/.test(f)) { fs.copyFileSync(path.join(src, f), path.join(out, f)); n++; }
console.log(which, 'png', n, '->', out);
