// End-to-end test of 遠端操控 inside the real app (node 22+):
//   node app_remote_test.mjs <自在投影.exe> <outDir> [--lang zh-TW|en|ja|ko]
// Runs a --dev --test-offscreen --test-no-network instance with its own data
// folder (PM_DEV_INSTANCE=remote081), input in dry-run (PM_REMOTE_DRYRUN=1:
// the real mouse / keyboard are never touched) and a test clipboard. Drives it
// with PhoneMirror.DevCommand (share/tools/dev_cmd.ps1) and a headless Edge
// (own throw-away profile) as the phone, through the LAN address. Checks the
// whole flow: turn on -> QR panel -> new phone asks -> 允許 + 記住 -> picture ->
// banner -> 僅觀看 -> 中斷 (kick) -> asks again -> 拒絕 -> off -> on again ->
// the remembered phone goes straight in -> quit. Closes only what it started.
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';

const [exe, outDir, ...rest] = process.argv.slice(2);
let lang = 'zh-TW', noTs = false;
for (let i = 0; i < rest.length; i++) { if (rest[i] === '--lang') lang = rest[++i]; else if (rest[i] === '--no-ts') noTs = true; }
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const devCmdPs1 = path.resolve(here, '..', '..', 'share', 'tools', 'dev_cmd.ps1');

// ---- the app's own data folder ----
const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-remote081');
fs.mkdirSync(dataDir, {recursive: true});
for (const f of ['remote_devices.txt', 'phonemirror.log', 'phonemirror.old.log']) { try { fs.rmSync(path.join(dataDir, f)); } catch {} }
const token = crypto.randomBytes(24).toString('base64').replace(/[^A-Za-z0-9]/g, '').slice(0, 32).padEnd(32, 'x');
const port = 8700 + Math.floor(Math.random() * 200);
fs.writeFileSync(path.join(dataDir, 'settings.ini'),
  `[settings]\nlanguage=${lang}\nremote_token=${token}\nremote_port=${port}\nremote_idle_minutes=30\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\ntoolbar_hint_shown=1\n`);
const appLog = path.join(dataDir, 'phonemirror.log');
const readLog = () => { try { return fs.readFileSync(appLog, 'utf8'); } catch { return ''; } };

const app = spawn(exe, ['--dev', '--test-offscreen', '--test-no-network'], {
  stdio: 'ignore', env: {...process.env, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: 'remote081', PM_REMOTE_DRYRUN: '1', PM_REMOTE_TESTCLIP: '1', PM_REMOTE_NOTS: noTs ? '1' : '0'}});
log('app pid', app.pid);
const dev = (cmd, lp = 0, wait = 600) => {
  try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmdPs1, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe'}); }
  catch (e) { log('dev cmd failed', cmd, String(e.stdout || e.message).trim()); }
};
const shotsDir = path.join(os.tmpdir(), 'pmshots');
const grab = tag => {
  for (const f of ['remote_panel.png', 'remote_panel_ts.png', 'remote_banner.png', 'remote_ask.png', 'menu_tray.png', 'menu_context.png']) {
    const src = path.join(shotsDir, f);
    if (fs.existsSync(src)) { fs.copyFileSync(src, path.join(outDir, `${tag}_${f}`)); fs.rmSync(src); }
  }
};
const waitLog = async (re, ms = 10000) => { const end = Date.now() + ms; while (Date.now() < end) { if (re.test(readLog())) return true; await sleep(200); } return false; };

let edge = null, ws = null, nextId = 1;
const pending = new Map();
const profileDir = path.join(os.tmpdir(), 'edge-081-app-' + crypto.randomBytes(3).toString('hex'));
const cdp = (method, params = {}, sessionId) => { const id = nextId++; ws.send(JSON.stringify({id, method, params, ...(sessionId ? {sessionId} : {})}));
  return new Promise((res, rej) => pending.set(id, {res, rej})); };
const results = {};
let code = 0;
try {
  await sleep(4000);
  dev(330, 0, 1500);  // 開啟遠端操控 -> QR panel
  const on = await waitLog(/remote: on at (http:\/\/\S+\/)/);
  const url = (readLog().match(/remote: on at (http:\/\/[\d.]+:\d+\/\w+\/)/) || [])[1];
  results.on = on && !!url;
  log('remote on:', url);
  dev(960, 0, 1000);
  grab('1-on');

  // ---- the phone (headless Edge, own profile) ----
  const cdpPort = 9400 + Math.floor(Math.random() * 400);
  edge = spawn('C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
    [`--user-data-dir=${profileDir}`, `--remote-debugging-port=${cdpPort}`, '--no-first-run', '--no-default-browser-check', '--disable-extensions',
     '--headless=new', 'about:blank'], {stdio: 'ignore'});
  let ver;
  for (let i = 0; i < 50 && !ver; i++) { await sleep(200); try { ver = await (await fetch(`http://127.0.0.1:${cdpPort}/json/version`)).json(); } catch {} }
  ws = new WebSocket(ver.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  ws.onmessage = e => { const m = JSON.parse(e.data); if (m.id && pending.has(m.id)) { const p = pending.get(m.id); pending.delete(m.id); m.error ? p.rej(new Error(m.error.message)) : p.res(m.result); } };
  const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
  const {sessionId: S} = await cdp('Target.attachToTarget', {targetId, flatten: true});
  await cdp('Emulation.setDeviceMetricsOverride', {width: 390, height: 844, deviceScaleFactor: 2, mobile: true}, S);
  await cdp('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5}, S);
  const evalJs = async expr => (await cdp('Runtime.evaluate', {expression: expr, returnByValue: true, awaitPromise: true, userGesture: true}, S)).result.value;
  const shot = async name => { const r = await cdp('Page.captureScreenshot', {format: 'png'}, S); fs.writeFileSync(path.join(outDir, name), Buffer.from(r.data, 'base64')); };
  const state = () => evalJs('S.state').catch(() => '');
  const playing = () => evalJs('!!(v.videoWidth && S.state === "connected")').catch(() => false);
  const until = async (fn, ms = 15000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await fn()) return true; await sleep(300); } return false; };

  await cdp('Page.navigate', {url}, S);
  results.pending = await until(async () => (await state()) === 'pending');
  results.askLogged = await waitLog(/remote: .* asks to connect/);
  await sleep(800);
  dev(960, 0, 1000);
  grab('2-ask');
  await shot('phone-1-waiting.png');
  dev(961, 3, 800);  // 允許 + 記住這支手機
  results.playing = await until(playing, 20000);
  results.remembered = fs.existsSync(path.join(dataDir, 'remote_devices.txt'));
  results.connectedLogged = await waitLog(/remote: .* is connected/);
  await sleep(1500);
  dev(960, 0, 1000);
  grab('3-connected');
  await shot('phone-2-playing.png');
  // a few gestures: dry-run (logged by the host, nothing moves)
  await cdp('Input.dispatchTouchEvent', {type: 'touchStart', touchPoints: [{x: 200, y: 400}]}, S);
  await cdp('Input.dispatchTouchEvent', {type: 'touchEnd', touchPoints: []}, S);
  results.dryRunClick = await waitLog(/input \(dry-run\): left click/);
  // 0.8.1 (A): the QR panel went away when the phone came in.
  results.panelClosed = /remote: QR panel closed \(a phone is connected\)/.test(readLog());
  // 0.8.1 (A): the phone's clicks never reach the PC-only switches.
  // A menu command right after a phone click (timestamp guard): refused.
  dev(962, 6, 300);  // armed: the app runs the menu toggle right after the phone's next click
  await cdp('Input.dispatchTouchEvent', {type: 'touchStart', touchPoints: [{x: 200, y: 400}]}, S);
  await cdp('Input.dispatchTouchEvent', {type: 'touchEnd', touchPoints: []}, S);
  results.menuAfterClickRefused = await waitLog(/the phone clicked 開啟遠端操控 \(menu\) -> ignored/, 4000);
  await sleep(1200);
  dev(962, 3, 600);  // banner 中斷, with the phone's input tag
  dev(962, 4, 600);  // banner 僅觀看, with the phone's input tag
  dev(962, 5, 600);  // menu 開啟遠端操控 (off), with the phone's input tag
  dev(962, 1, 1200); // QR panel 關閉遠端操控, with the phone's input tag (opens the panel first)
  const Lr = readLog();
  results.bannerRefused = /the phone clicked 中斷 \/ 僅觀看 \(banner\) -> ignored/.test(Lr);
  results.menuTagRefused = (Lr.match(/the phone clicked 開啟遠端操控 \(menu\) -> ignored/g) || []).length >= 2;
  results.panelRefused = /the phone clicked 關閉遠端操控[^\n]*\(QR panel\) -> ignored/.test(Lr);
  results.stillOnAfterPhoneClicks = /dev remote click done: remote on, phone connected/.test(Lr) && !/remote: off/.test(Lr) && await playing();
  results.viewOnlyUntouched = await evalJs('S.viewOnly === false');
  results.pcOnlyToastOnPhone = await evalJs(`$('toast').style.display === 'block' && $('toast').textContent === t('pcOnly')`);
  await shot('phone-2b-pc-only-toast.png');
  dev(960, 0, 800);
  grab('3b-panel-reopened');
  await sleep(1200);
  // 僅觀看 on / off from the PC (menu command)
  dev(332, 0, 1200);
  results.viewOnlyOnPhone = await until(() => evalJs('S.viewOnly === true'), 6000);
  dev(960, 0, 800);
  grab('4-viewonly');
  await shot('phone-3-viewonly.png');
  dev(332, 0, 800);
  // 中斷 from the PC (the bar / Ctrl+D / menu all call the same kick)
  dev(340, 0, 1200);  // CmdRemoteKick
  results.kickedOnPhone = await until(() => evalJs('S.bye === "kicked"'), 6000);
  results.kickLogged = await waitLog(/remote: session of .* ended: kicked/);
  await shot('phone-4-kicked.png');
  // asks again after a kick, even though remembered; this time 拒絕
  await evalJs('retry()');
  results.askedAgain = await until(async () => (await state()) === 'pending', 8000);
  await sleep(600);
  dev(961, 2, 800);
  results.deniedOnPhone = await until(async () => (await state()) === 'denied', 8000);
  await shot('phone-5-denied.png');
  // turn off: the page is told
  dev(330, 0, 1500);
  results.offLogged = await waitLog(/remote: off/);
  // on again: the remembered phone goes straight in (no dialog)
  const asksBefore = (readLog().match(/asks to connect/g) || []).length;
  dev(330, 0, 2000);
  await sleep(500);
  await evalJs('retry()');
  results.rememberedStraightIn = await until(playing, 20000);
  results.noNewAsk = (readLog().match(/asks to connect/g) || []).length === asksBefore;
  await shot('phone-6-remembered.png');
  results.panelClosedAgain = (readLog().match(/QR panel closed \(a phone is connected\)/g) || []).length >= 2;
  dev(960, 0, 1000);
  grab('5-again');
  // 0.8.1 (A): 關閉遠端操控 clicked on the PC itself (plain click): off, and the phone says so in plain words.
  dev(962, 2, 1500);
  results.offByPcClick = await until(async () => (readLog().match(/remote: off/g) || []).length >= 2, 5000);
  results.stoppedOnPhone = await until(() => evalJs(`S.bye === 'stopped' && $('msgT').textContent === t('stopped')`), 6000);
  await shot('phone-7-pc-turned-off.png');
} catch (e) {
  console.error('test failed:', e);
  code = 1;
}
// ---- quit: the app, then the phone ----
try { if (ws) await Promise.race([cdp('Browser.close'), sleep(3000)]); } catch {}
dev(104, 0, 500);  // 結束
for (let i = 0; i < 100 && app.exitCode === null; i++) await sleep(100);
results.appExited = app.exitCode !== null;
for (const p of [edge, app]) if (p && p.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(p.pid), '/T', '/F'], {stdio: 'ignore'}); log('killed own', p.pid); } catch {} }
const mine = () => { try { return execFileSync('powershell', ['-NoProfile', '-Command',
  `Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" | Where-Object { $_.CommandLine -and (Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue | Where-Object { -not $_.HasExited }) -and $_.CommandLine.Contains('${profileDir}') } | ForEach-Object { $_.ProcessId }`],
  {encoding: 'utf8'}).split(/\s+/).filter(Boolean).map(Number); } catch { return []; } };
let left = mine();
for (let i = 0; i < 20 && left.length; i++) { await sleep(1000); left = mine(); }
for (const pid of left) { try { execFileSync('taskkill', ['/PID', String(pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} }
try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
const L = readLog();
fs.writeFileSync(path.join(outDir, 'app-remote.log'), L.split('\n').filter(l => /remote|dev remote/.test(l)).join('\n'));
const checks = [
  ['turned on, URL with the kept token and port', results.on && L.includes(`:${port}/${token}/`)],
  ['QR panel shots (LAN + Tailscale tab)', fs.existsSync(path.join(outDir, '1-on_remote_panel.png')) && fs.existsSync(path.join(outDir, '1-on_remote_panel_ts.png'))],
  ['new phone waits for 允許 on the PC', results.pending === true && results.askLogged === true],
  ['允許 dialog shot', fs.existsSync(path.join(outDir, '2-ask_remote_ask.png'))],
  ['picture plays after 允許', results.playing === true],
  ['記住這支手機 written', results.remembered === true],
  ['banner shot while connected', fs.existsSync(path.join(outDir, '3-connected_remote_banner.png'))],
  ['input is dry-run in the test', results.dryRunClick === true && /input dry-run/.test(L)],
  ['僅觀看 reaches the phone', results.viewOnlyOnPhone === true],
  ['中斷 ends it, phone told, no auto reconnect', results.kickedOnPhone === true && results.kickLogged === true],
  ['after 中斷 the phone is asked again', results.askedAgain === true],
  ['拒絕 shown on the phone', results.deniedOnPhone === true],
  ['turned off', results.offLogged === true],
  ['remembered phone goes straight in after on again', results.rememberedStraightIn === true && results.noNewAsk === true],
  ['0.8.1 QR panel closes when the phone connects (both times)', results.panelClosed === true && results.panelClosedAgain === true],
  ['0.8.1 menu 關閉 right after a phone click: refused', results.menuAfterClickRefused === true],
  ['0.8.1 phone-tagged clicks refused: banner 中斷/僅觀看, menu, QR panel 關閉遠端操控',
   results.bannerRefused === true && results.menuTagRefused === true && results.panelRefused === true],
  ['0.8.1 ... remote control still on, phone still playing, view-only untouched', results.stillOnAfterPhoneClicks === true && results.viewOnlyUntouched === true],
  ['0.8.1 ... the phone is told (toast)', results.pcOnlyToastOnPhone === true],
  ['0.8.1 a plain click on the PC turns it off', results.offByPcClick === true],
  ['0.8.1 phone shows 「電腦端已關閉遠端操控」 (bye arrived)', results.stoppedOnPhone === true],
  ['app quit cleanly', results.appExited === true],
];
let ok = code === 0;
const lines = [];
for (const [n, p] of checks) { const l = (p ? 'PASS ' : 'FAIL ') + n; log(l); lines.push(l); ok = ok && !!p; }
fs.writeFileSync(path.join(outDir, 'checks.txt'), lines.join('\n') + '\n' + (ok ? 'ALL PASS' : 'SOME FAILED') + '\n');
log('results', JSON.stringify(results));
process.exit(ok ? 0 : 1);
