// End-to-end test of 遠端操控 inside the real app (node 22+):
//   node app_remote_test.mjs <自在投影.exe> <outDir> [--lang zh-TW|en|ja|ko] [--no-ts] [--loopback]
// --block-autoplay (0.8.3.1): at the end, remote control on again and a second tab whose video.play()
// is refused until a tap (like the owner's iPhone in Low Power Mode, videoWidth 0 while paused): what the
// page says (no endless 連線中), then a tap (CDP touch on that page only) and the picture.
// --loopback (0.8.3.1): PM_REMOTE_LOOPBACK=1, the remote side on 127.0.0.3 only and Edge with
// loopback ICE candidates: nothing listens on the LAN, so a fresh build path gets no firewall prompt.
// Runs a --dev --test-offscreen --test-no-network instance with its own data
// folder (PM_DEV_INSTANCE=remote081), input in dry-run (PM_REMOTE_DRYRUN=1:
// the real mouse / keyboard are never touched) and a test clipboard. Drives it
// with PhoneMirror.DevCommand (share/tools/dev_cmd.ps1) and a headless Edge
// (own throw-away profile) as the phone, through the LAN address. Checks the
// whole flow: turn on -> QR panel -> new phone asks -> 允許 + 記住 -> picture ->
// banner -> 僅觀看 -> 中斷 (kick) -> asks again -> 拒絕 -> off -> on again ->
// 0.8.3.6: 中斷 / 拒絕 took the phone off the 記住 list: it asks again -> 允許 -> quit. Closes only what it started.
import {spawn, execFileSync} from 'node:child_process';
import {headlessShell} from './quiet_browser.mjs';  // rel-0836: chrome-headless-shell (no window, no Alt+Tab entry), not Edge / Chrome --headless=new
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';

const [exe, outDir, ...rest] = process.argv.slice(2);
let lang = 'zh-TW', noTs = false, loopback = false, blockAutoplay = false;
for (let i = 0; i < rest.length; i++) { if (rest[i] === '--lang') lang = rest[++i]; else if (rest[i] === '--no-ts') noTs = true; else if (rest[i] === '--loopback') loopback = true; else if (rest[i] === '--block-autoplay') blockAutoplay = true; }
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
  stdio: 'ignore', env: {...process.env, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: 'remote081', PM_REMOTE_DRYRUN: '1', PM_REMOTE_SYNTHAUDIO: 'quiet', PM_REMOTE_TESTCLIP: '1', PM_REMOTE_NOTS: noTs || loopback ? '1' : '0', PM_REMOTE_LOOPBACK: loopback ? '1' : '0'}});
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
  edge = spawn(headlessShell(),
    ['--mute-audio', `--user-data-dir=${profileDir}`, `--remote-debugging-port=${cdpPort}`, '--no-first-run', '--no-default-browser-check', '--disable-extensions',
     ...(loopback ? ['--allow-loopback-in-peer-connection'] : []), 'about:blank'], {stdio: 'ignore', windowsHide: true});
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
  // on again: 0.8.3.6: 中斷 and 拒絕 took it off the 記住 list (remote_devices.txt), so it asks again; 允許
  const asksBefore = (readLog().match(/\[info\] remote: .* asks to connect/g) || []).length;
  results.forgottenOnKick = /remote: 中斷 on the PC: \S+ no longer remembered/.test(readLog()) && !fs.existsSync(path.join(dataDir, 'remote_devices.txt'));
  dev(330, 0, 2000);
  await sleep(500);
  await evalJs('retry()');
  results.askedAfterForget = await until(async () => (await state()) === 'pending', 10000) && (readLog().match(/\[info\] remote: .* asks to connect/g) || []).length === asksBefore + 1;
  dev(961, 1, 800);
  results.rememberedStraightIn = await until(playing, 20000);
  await shot('phone-6-remembered.png');
  results.panelClosedAgain = (readLog().match(/QR panel closed \(a phone is connected\)/g) || []).length >= 2;
  dev(960, 0, 1000);
  grab('5-again');
  // 0.8.1 (A): 關閉遠端操控 clicked on the PC itself (plain click): off, and the phone says so in plain words.
  dev(962, 2, 1500);
  results.offByPcClick = await until(async () => (readLog().match(/remote: off/g) || []).length >= 2, 5000);
  results.stoppedOnPhone = await until(() => evalJs(`S.bye === 'stopped' && $('msgT').textContent === t('stopped')`), 6000);
  await shot('phone-7-pc-turned-off.png');
  if (blockAutoplay) {
    // ---- 0.8.3.1: the owner's 07:48 case inside the app: play() refused, nobody taps ----
    dev(330, 0, 2000);  // remote control on again (the remembered phone goes straight in)
    const {targetId: t2} = await cdp('Target.createTarget', {url: 'about:blank'});
    const {sessionId: S2} = await cdp('Target.attachToTarget', {targetId: t2, flatten: true});
    await cdp('Page.enable', {}, S2);
    await cdp('Runtime.enable', {}, S2);
    await cdp('Emulation.setDeviceMetricsOverride', {width: 440, height: 764, deviceScaleFactor: 2, mobile: true}, S2);
    await cdp('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5}, S2);
    await cdp('Page.addScriptToEvaluateOnNewDocument', {source: `(() => {
      const op = HTMLMediaElement.prototype.play, vw = Object.getOwnPropertyDescriptor(HTMLVideoElement.prototype, 'videoWidth');
      HTMLMediaElement.prototype.play = function () { if (!this.__ok) { if (navigator.userActivation && navigator.userActivation.isActive) this.__ok = true;
        else return Promise.reject(new DOMException('blocked (test)', 'NotAllowedError')); } return op.apply(this, arguments); };
      Object.defineProperty(HTMLVideoElement.prototype, 'videoWidth', {configurable: true, get() { return this.paused ? 0 : vw.get.call(this); }});
      const h = () => { const v = document.getElementById('v'); if (!v) return setTimeout(h, 5); v.autoplay = false; v.removeAttribute('autoplay'); }; h();
    })();`}, S2);
    const ev2 = async expr => (await cdp('Runtime.evaluate', {expression: expr, returnByValue: true}, S2)).result.value;  // (no user gesture)
    const st2 = () => ev2(`({state: S.state, msg: msg.classList.contains('hide') ? '' : $('msgT').textContent, spin: !msg.classList.contains('hide') && $('msgSpin').style.display !== 'none', btns: Array.from(document.querySelectorAll('#msgB button')).map(b => b.textContent), pill: $('tapHint').style.display === 'flex' ? $('tapHintT').textContent || '(empty pill)' : '', playing: !v.paused && v.videoWidth > 16})`).catch(() => ({}));
    const t0 = Date.now();
    await cdp('Page.navigate', {url}, S2);
    const seen = [];
    let last = '';
    while (Date.now() - t0 < 12000) {
      await sleep(300);
      const s = await st2();
      const k = JSON.stringify([s.msg, s.spin, s.btns, s.pill]);
      if (k !== last) { last = k; seen.push({ms: Date.now() - t0, ...s}); }
    }
    results.blockedTimeline = seen;
    log('autoplay refused:', JSON.stringify(seen));
    await cdp('Page.captureScreenshot', {format: 'png'}, S2).then(r => fs.writeFileSync(path.join(outDir, 'phone-8-autoplay-refused.png'), Buffer.from(r.data, 'base64'))).catch(() => {});
    const fin = await st2();
    results.blockedSaysTap = fin.state === 'connected' && fin.msg === await ev2(`t('tapReady')`).catch(() => '?') && !fin.spin;
    results.blockedNoEndlessSpinner = !(fin.spin && /連線中|Connecting/.test(fin.msg || ''));
    await cdp('Input.dispatchTouchEvent', {type: 'touchStart', touchPoints: [{x: 220, y: 382}]}, S2);
    await cdp('Input.dispatchTouchEvent', {type: 'touchEnd', touchPoints: []}, S2);
    const tapT = Date.now();
    let ok = false;
    while (Date.now() - tapT < 6000 && !(ok = !!(await st2()).playing)) await sleep(100);
    results.blockedTapToPicture = ok ? Date.now() - tapT : -1;
    results.tapIdrLogged = await waitLog(/asked for a key frame [(]the video plays after a tap[)]/, 3000);
    log('autoplay refused: tap -> picture', results.blockedTapToPicture, 'ms');
    await cdp('Page.captureScreenshot', {format: 'png'}, S2).then(r => fs.writeFileSync(path.join(outDir, 'phone-9-after-tap.png'), Buffer.from(r.data, 'base64'))).catch(() => {});
  }
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
  `Get-CimInstance Win32_Process -Filter "Name='chrome-headless-shell.exe'" | Where-Object { $_.CommandLine -and (Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue | Where-Object { -not $_.HasExited }) -and $_.CommandLine.Contains('${profileDir}') } | ForEach-Object { $_.ProcessId }`],
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
  ['0.8.3.6 中斷 took the phone off the 記住 list (remote_devices.txt gone)', results.forgottenOnKick === true],
  ['0.8.3.6 ... so after on again it asks 允許 again, then plays', results.askedAfterForget === true && results.rememberedStraightIn === true],
  ['0.8.1 QR panel closes when the phone connects (both times)', results.panelClosed === true && results.panelClosedAgain === true],
  ['0.8.1 menu 關閉 right after a phone click: refused', results.menuAfterClickRefused === true],
  ['0.8.1 phone-tagged clicks refused: banner 中斷/僅觀看, menu, QR panel 關閉遠端操控',
   results.bannerRefused === true && results.menuTagRefused === true && results.panelRefused === true],
  ['0.8.1 ... remote control still on, phone still playing, view-only untouched', results.stillOnAfterPhoneClicks === true && results.viewOnlyUntouched === true],
  ['0.8.1 ... the phone is told (toast)', results.pcOnlyToastOnPhone === true],
  ['0.8.1 a plain click on the PC turns it off', results.offByPcClick === true],
  ['0.8.1 phone shows 「電腦端已關閉遠端操控」 (bye arrived)', results.stoppedOnPhone === true],
  ['app quit cleanly', results.appExited === true],
  ...(blockAutoplay ? [
    ['0.8.3.1 autoplay refused: the page says 已連上，點一下 (no endless 連線中)', results.blockedSaysTap === true && results.blockedNoEndlessSpinner === true],
    ['0.8.3.1 autoplay refused: a tap -> picture within 2 s', results.blockedTapToPicture >= 0 && results.blockedTapToPicture <= 2000],
    ['0.8.3.1 autoplay refused: a key frame asked for after the tap', results.tapIdrLogged === true]] : []),
];
let ok = code === 0;
const lines = [];
for (const [n, p] of checks) { const l = (p ? 'PASS ' : 'FAIL ') + n; log(l); lines.push(l); ok = ok && !!p; }
fs.writeFileSync(path.join(outDir, 'checks.txt'), lines.join('\n') + '\n' + (ok ? 'ALL PASS' : 'SOME FAILED') + '\n');
log('results', JSON.stringify(results));
process.exit(ok ? 0 : 1);
