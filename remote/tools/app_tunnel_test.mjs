// 免安裝外網 (0.8.1) inside the real app: --dev --test-offscreen instance with
// its own data folder (PM_DEV_INSTANCE=remote081t), input dry-run, test
// clipboard. Turns remote control on, starts the no-app link (cloudflared),
// renders the panel's third tab, opens the one-time link in Edge headless
// (phone emulation, own profile), enters the 通行碼, answers 允許 + 記住 on the
// PC, checks the picture / the 直連 badge / the QR panel closing, then stops the
// link from the PC (the phone is told) and quits. Closes only what it started.
//   node app_tunnel_test.mjs <自在投影.exe> <outDir> [--lang zh-TW|en|ja|ko]
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';

const [exe, outDir, ...rest] = process.argv.slice(2);
let lang = 'zh-TW', rebind = false;
for (let i = 0; i < rest.length; i++) { if (rest[i] === '--lang') lang = rest[++i]; else if (rest[i] === '--rebind') rebind = true; }
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const devCmdPs1 = path.resolve(here, '..', '..', 'share', 'tools', 'dev_cmd.ps1');
const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-remote081t');
fs.mkdirSync(dataDir, {recursive: true});
for (const f of ['remote_devices.txt', 'phonemirror.log', 'phonemirror.old.log']) { try { fs.rmSync(path.join(dataDir, f)); } catch {} }
const token = crypto.randomBytes(24).toString('base64').replace(/[^A-Za-z0-9]/g, '').slice(0, 32).padEnd(32, 'x');
const port = 8700 + Math.floor(Math.random() * 200);
fs.writeFileSync(path.join(dataDir, 'settings.ini'),
  `[settings]\nlanguage=${lang}\nremote_token=${token}\nremote_port=${port}\nremote_idle_minutes=30\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\ntoolbar_hint_shown=1\n`);
const appLog = path.join(dataDir, 'phonemirror.log');
const readLog = () => { try { return fs.readFileSync(appLog, 'utf8'); } catch { return ''; } };
const app = spawn(exe, ['--dev', '--test-offscreen'], {
  stdio: 'ignore', env: {...process.env, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: 'remote081t', PM_REMOTE_DRYRUN: '1', PM_REMOTE_TESTCLIP: '1', PM_REMOTE_NOTS: '1', PM_REMOTE_TESTPASS: '1'}});
log('app pid', app.pid);
const dev = (cmd, lp = 0, wait = 600) => {
  try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmdPs1, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe'}); }
  catch (e) { log('dev cmd failed', cmd, String(e.stdout || e.message).trim()); }
};
const shotsDir = path.join(os.tmpdir(), 'pmshots');
const grab = tag => {
  for (const f of ['remote_panel.png', 'remote_panel_ts.png', 'remote_panel_tun.png', 'remote_banner.png', 'remote_ask.png']) {
    const src = path.join(shotsDir, f);
    if (fs.existsSync(src)) { fs.copyFileSync(src, path.join(outDir, `${tag}_${f}`)); fs.rmSync(src); }
  }
};
const waitLog = async (re, ms = 10000) => { const end = Date.now() + ms; while (Date.now() < end) { const m = readLog().match(re); if (m) return m; await sleep(200); } return null; };
const profileDir = path.join(os.tmpdir(), 'edge-081-apptun-' + crypto.randomBytes(3).toString('hex'));
let edge = null, ws = null, nextId = 1;
const pending = new Map();
const cdp = (method, params = {}, sessionId) => { const id = nextId++; ws.send(JSON.stringify({id, method, params, ...(sessionId ? {sessionId} : {})})); return new Promise((res, rej) => pending.set(id, {res, rej})); };
const results = {};
let code = 0;
try {
  await sleep(4000);
  dev(330, 0, 1500);  // 開啟遠端操控
  results.on = !!(await waitLog(/remote: on at http/));
  dev(960, 0, 800);
  grab('1-on');
  dev(352, 0, 800);   // 免安裝外網連結 (menu, CmdRemoteTunnel = 352)
  dev(960, 0, 600);
  grab('2-starting');
  const m = await waitLog(/remote: test tunnel link (\S+) passcode (\d{6})/, 90000);
  results.ready = !!m;
  if (!m) throw new Error('no tunnel link');
  const link = m[1], pass = m[2];
  log('link', link);
  dev(960, 0, 1000);
  grab('3-ready');
  // ---- the phone ----
  const cdpPort = 9400 + Math.floor(Math.random() * 400);
  edge = spawn('C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
    [`--user-data-dir=${profileDir}`, `--remote-debugging-port=${cdpPort}`, '--no-first-run', '--no-default-browser-check', '--disable-extensions', '--headless=new', 'about:blank'], {stdio: 'ignore'});
  let ver;
  for (let i = 0; i < 50 && !ver; i++) { await sleep(200); try { ver = await (await fetch(`http://127.0.0.1:${cdpPort}/json/version`)).json(); } catch {} }
  ws = new WebSocket(ver.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  ws.onmessage = e => { const x = JSON.parse(e.data); if (x.id && pending.has(x.id)) { const p = pending.get(x.id); pending.delete(x.id); x.error ? p.rej(new Error(x.error.message)) : p.res(x.result); } };
  const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
  const {sessionId: S} = await cdp('Target.attachToTarget', {targetId, flatten: true});
  await cdp('Emulation.setDeviceMetricsOverride', {width: 390, height: 844, deviceScaleFactor: 2, mobile: true}, S);
  await cdp('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5}, S);
  const evalJs = async expr => (await cdp('Runtime.evaluate', {expression: expr, returnByValue: true}, S)).result.value;
  const shot = async name => { const r = await cdp('Page.captureScreenshot', {format: 'png'}, S); fs.writeFileSync(path.join(outDir, name), Buffer.from(r.data, 'base64')); };
  const until = async (fn, ms = 15000) => { const end = Date.now() + ms; while (Date.now() < end) { try { if (await fn()) return true; } catch {} await sleep(300); } return false; };
  for (let i = 0; i < 20; i++) { try { const r = await fetch(link.replace(/\/t\/.*/, '/'), {redirect: 'manual'}); if (r.status) break; } catch {} await sleep(1000); }
  await cdp('Page.navigate', {url: link}, S);
  results.passAsked = await until(() => evalJs(`getComputedStyle(document.getElementById('pass')).display === 'flex'`), 20000);
  await shot('phone-1-passcode.png');
  await evalJs(`document.getElementById('passIn').focus()`);
  await cdp('Input.insertText', {text: pass}, S);
  results.asks = !!(await waitLog(/remote: .* \(免安裝外網\) asks to connect/, 15000));
  await sleep(800);
  dev(960, 0, 800);
  grab('4-ask');
  await shot('phone-2-waiting.png');
  dev(961, 3, 800);  // 允許 + 記住
  results.playing = await until(() => evalJs(`!!window.__pmFirstFrame`), 25000);
  results.panelClosed = !!(await waitLog(/remote: QR panel closed \(a phone is connected\)/, 5000));
  await sleep(2500);
  results.badge = await evalJs(`document.getElementById('path').textContent`);
  await shot('phone-3-playing.png');
  dev(960, 0, 800);
  grab('5-connected');
  if (rebind) {  // the owner's 23:11:34 case: remote control off and on with a phone in through the tunnel
    const portOf = () => [...readLog().matchAll(/remote: on at http:\/\/[\d.]+:(\d+)\//g)].map(m => +m[1]);
    dev(330, 0, 300);
    dev(330, 0, 2500);
    const ports = portOf();
    results.rebindPorts = ports.join(' -> ');
    results.samePort = ports.length >= 2 && ports[ports.length - 1] === ports[0];
    results.bindFailed = /http: bind [^ ]+ failed/.test(readLog());
    fs.writeFileSync(path.join(outDir, 'rebind.txt'), 'ports: ' + results.rebindPorts + '\nbind failed in log: ' + results.bindFailed + '\n');
    throw new Error('rebind run done');
  }
  // Stop the link on the PC: the phone is told in plain words.
  dev(352, 0, 1500);
  results.stoppedOnPhone = await until(() => evalJs(`S.bye === 'stopped' && $('msgT').textContent === t('stopped')`), 8000);
  await shot('phone-4-link-stopped.png');
  results.stopLogged = !!(await waitLog(/remote: 免安裝外網 stopped \(menu\)/, 3000));
  dev(330, 0, 1200);  // remote control off
} catch (e) {
  console.error('test failed:', e);
  code = 1;
}
try { if (ws) await Promise.race([cdp('Browser.close'), sleep(3000)]); } catch {}
dev(104, 0, 500);  // 結束
for (let i = 0; i < 100 && app.exitCode === null; i++) await sleep(100);
results.appExited = app.exitCode !== null;
for (const p of [edge, app]) if (p && p.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(p.pid), '/T', '/F'], {stdio: 'ignore'}); log('killed own', p.pid); } catch {} }
const mine = () => { try { return execFileSync('powershell', ['-NoProfile', '-Command',
  `Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" | Where-Object { $_.CommandLine -and $_.CommandLine.Contains('${profileDir}') } | ForEach-Object { $_.ProcessId }`],
  {encoding: 'utf8'}).split(/\s+/).filter(Boolean).map(Number); } catch { return []; } };
let left = mine();
for (let i = 0; i < 20 && left.length; i++) { await sleep(1000); left = mine(); }
for (const pid of left) { try { execFileSync('taskkill', ['/PID', String(pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} }
try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
// Only cloudflared started by this test's app (the owner may run his own).
results.cloudflaredLeft = (() => { try { return execFileSync('powershell', ['-NoProfile', '-Command', `@(Get-CimInstance Win32_Process -Filter "Name='cloudflared.exe'" | Where-Object { $_.ParentProcessId -eq ${app.pid} }).Count`], {encoding: 'utf8'}).trim() !== '0'; } catch { return false; } })();
const L = readLog();
fs.writeFileSync(path.join(outDir, 'app-remote.log'), L.split(/\r?\n/).filter(l => /remote|tunnel|dev remote/.test(l)).map(l => l.replace(/passcode \d{6}/, 'passcode ******')).join('\n'));
const checks = [
  ['remote control on', results.on === true],
  ['免安裝外網: panel tab rendered while starting and when ready', fs.existsSync(path.join(outDir, '2-starting_remote_panel_tun.png')) && fs.existsSync(path.join(outDir, '3-ready_remote_panel_tun.png'))],
  ['cloudflared gave a public address, one-time link + 通行碼', results.ready === true],
  ['the phone page asks for the 通行碼', results.passAsked === true],
  ['after the 通行碼 the PC asks 允許 (shown as 免安裝外網)', results.asks === true && fs.existsSync(path.join(outDir, '4-ask_remote_ask.png'))],
  ['picture after 允許', results.playing === true],
  ['QR panel closed when the phone came in', results.panelClosed === true],
  ['phone shows the path badge (直連 / 隧道 + ms)', /\d+ ms/.test(results.badge || ''), results.badge],
  ['stopping the link on the PC: the phone says 「電腦端已關閉遠端操控」', results.stoppedOnPhone === true && results.stopLogged === true],
  ['app quit cleanly, no cloudflared left', results.appExited === true && results.cloudflaredLeft === false],
];
let ok = code === 0;
const lines = checks.map(([n, p, info]) => { ok = ok && !!p; return (p ? 'PASS ' : 'FAIL ') + n + (info ? '  [' + info + ']' : ''); });
lines.push(ok ? 'ALL PASS' : 'SOME FAILED');
fs.writeFileSync(path.join(outDir, 'checks.txt'), lines.join('\n') + '\n');
console.log(lines.join('\n'));
log('results', JSON.stringify(results));
process.exit(ok ? 0 : 1);
