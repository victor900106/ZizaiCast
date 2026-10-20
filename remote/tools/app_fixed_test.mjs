// 常駐外網 (0.8.2) inside the real app, through the PUBLIC Tailscale Funnel address.
// --dev --test-offscreen instance with its own data folder (PM_DEV_INSTANCE=remote082f),
// input dry-run, no Tailscale-address listener (PM_REMOTE_NOTS: the phone must come in
// through Funnel). Sets the password in the app's window (test hook: typed into the
// password boxes, no keyboard), turns 常駐外網 on, opens the fixed address in Edge
// headless (phone emulation, own profile; the host name mapped to the Funnel relay IP
// from public DNS, so nothing goes over the tailnet), types a wrong then the right
// password into the page (DOM, no OS input), checks: no 允許 asked, the picture, the
// source address in the log, the saved login on reload; then 常駐外網 off from the menu
// (8443 removed, the phone told) and quit. Port 443 is compared before / after.
//   node app_fixed_test.mjs <自在投影.exe> <outDir>
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';

const [exe, outDir, ...rest] = process.argv.slice(2);
let lang = 'zh-TW';
for (let i = 0; i < rest.length; i++) if (rest[i] === '--lang') lang = rest[++i];
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const devCmdPs1 = path.resolve(here, '..', '..', 'share', 'tools', 'dev_cmd.ps1');
const TS = 'C:\\Program Files\\Tailscale\\tailscale.exe';
const funnelJson = () => execFileSync(TS, ['funnel', 'status', '--json'], {encoding: 'utf8'});
const port443 = j => { const f = JSON.parse(j); const k = Object.keys(f.Web || {}).find(x => x.endsWith(':443')); return JSON.stringify({tcp: (f.TCP || {})['443'], web: k ? f.Web[k] : null, funnel: k ? (f.AllowFunnel || {})[k] : null}); };
const has8443 = j => /:8443"/.test(j);
const before = funnelJson();
fs.writeFileSync(path.join(outDir, 'funnel-before.json'), before);
if (has8443(before)) { console.error('8443 is already in use: not touching it'); process.exit(2); }

const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-remote082f');
fs.mkdirSync(dataDir, {recursive: true});
for (const f of ['remote_devices.txt', 'phonemirror.log', 'phonemirror.old.log']) { try { fs.rmSync(path.join(dataDir, f)); } catch {} }
const token = crypto.randomBytes(24).toString('base64').replace(/[^A-Za-z0-9]/g, '').slice(0, 32).padEnd(32, 'x');
const port = 18100 + Math.floor(Math.random() * 300);
fs.writeFileSync(path.join(dataDir, 'settings.ini'),
  `[settings]\nlanguage=${lang}\nremote_token=${token}\nremote_port=${port}\nremote_idle_minutes=30\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\ntoolbar_hint_shown=1\nwhats_new_seen=0.8.2\n`);
const appLog = path.join(dataDir, 'phonemirror.log');
const readLog = () => { try { return fs.readFileSync(appLog, 'utf8'); } catch { return ''; } };
const app = spawn(exe, ['--dev', '--test-offscreen'], {
  stdio: 'ignore', env: {...process.env, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: 'remote082f', PM_REMOTE_DRYRUN: '1', PM_REMOTE_TESTCLIP: '1', PM_REMOTE_NOTS: '1'}});
log('app pid', app.pid);
const dev = (cmd, lp = 0, wait = 600) => {
  try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmdPs1, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe'}); }
  catch (e) { log('dev cmd failed', cmd, String(e.stdout || e.message).trim()); }
};
const shotsDir = path.join(os.tmpdir(), 'pmshots');
const grab = tag => {
  for (const f of ['remote_panel.png', 'remote_panel_ts.png', 'remote_panel_tun.png', 'remote_banner.png', 'remote_ask.png', 'remote_fixed.png', 'remote_password.png', 'window.png', 'menu_tray.png']) {
    const src = path.join(shotsDir, f);
    if (fs.existsSync(src)) { fs.copyFileSync(src, path.join(outDir, `${tag}_${f}`)); fs.rmSync(src); }
  }
};
const waitLog = async (re, ms = 10000) => { const end = Date.now() + ms; while (Date.now() < end) { const m = readLog().match(re); if (m) return m; await sleep(200); } return null; };
const profileDir = path.join(os.tmpdir(), 'edge-082-fixed-' + crypto.randomBytes(3).toString('hex'));
let edge = null, ws = null, nextId = 1;
const pending = new Map();
const cdp = (method, params = {}, sessionId) => { const id = nextId++; ws.send(JSON.stringify({id, method, params, ...(sessionId ? {sessionId} : {})})); return new Promise((res, rej) => pending.set(id, {res, rej})); };
const results = {};
let code = 0;
const PW = 'Zizai-Test-2026';
try {
  await sleep(4000);
  dev(910, 0, 800); grab('0-idle');
  dev(963, 1, 1200);           // the password window (from 常駐外網 ▸ off, no password yet)
  dev(963, 5, 800); grab('1-password-empty');
  dev(963, 7, 600);            // too short
  dev(963, 5, 800); grab('2-password-short');
  dev(963, 3, 600);            // mismatch
  dev(963, 5, 800); grab('3-password-mismatch');
  results.shortMsg = fs.existsSync(path.join(outDir, '2-password-short_remote_password.png'));
  dev(963, 2, 2500);           // the right one twice + 儲存並開啟 -> 常駐外網 on
  const ini = fs.readFileSync(path.join(dataDir, 'settings.ini'), 'utf8');
  results.hashStored = /remote_pw_hash=pbkdf2-sha256\$310000\$[0-9a-f]{32}\$[0-9a-f]{64}/.test(ini);
  results.noPlaintext = !ini.includes(PW) && !readLog().includes(PW);
  results.persistent = /remote_persistent=1/.test(ini);
  const m = await waitLog(/remote: 常駐外網 ready at (https:\/\/\S+:8443\/)/, 30000);
  results.ready = !!m;
  if (!m) throw new Error('fixed link not ready: ' + (readLog().match(/常駐外網[^\n]*/g) || []).slice(-3).join(' | '));
  const url = m[1];
  const mid = funnelJson();
  fs.writeFileSync(path.join(outDir, 'funnel-while-on.json'), mid);
  results.port443SameWhileOn = port443(mid) === port443(before);
  results.funnel8443On = has8443(mid) && mid.includes(`http://127.0.0.2:`);
  dev(960, 0, 800); dev(963, 5, 800); dev(910, 0, 800); dev(900, 0, 800); grab('4-ready');
  // ---- the phone, from the internet ----
  const host = new URL(url).hostname;
  const dns = await (await fetch(`https://cloudflare-dns.com/dns-query?name=${host}&type=A`, {headers: {accept: 'application/dns-json'}})).json();
  const relay = (dns.Answer || []).find(a => a.type === 1).data;
  log('fixed', url, 'relay', relay);
  const cdpPort = 9400 + Math.floor(Math.random() * 400);
  edge = spawn('C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
    [`--user-data-dir=${profileDir}`, `--remote-debugging-port=${cdpPort}`, '--no-first-run', '--no-default-browser-check', '--disable-extensions', '--headless=new',
     `--host-resolver-rules=MAP ${host} ${relay}`, 'about:blank'], {stdio: 'ignore'});
  let ver;
  for (let i = 0; i < 50 && !ver; i++) { await sleep(200); try { ver = await (await fetch(`http://127.0.0.1:${cdpPort}/json/version`)).json(); } catch {} }
  ws = new WebSocket(ver.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  ws.onmessage = e => { const x = JSON.parse(e.data); if (x.id && pending.has(x.id)) { const p = pending.get(x.id); pending.delete(x.id); x.error ? p.rej(new Error(x.error.message)) : p.res(x.result); } };
  const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
  const {sessionId: S} = await cdp('Target.attachToTarget', {targetId, flatten: true});
  await cdp('Emulation.setDeviceMetricsOverride', {width: 390, height: 844, deviceScaleFactor: 2, mobile: true}, S);
  await cdp('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5}, S);
  const evalJs = async expr => (await cdp('Runtime.evaluate', {expression: expr, returnByValue: true, awaitPromise: true}, S)).result.value;
  const shot = async name => { const r = await cdp('Page.captureScreenshot', {format: 'png'}, S); fs.writeFileSync(path.join(outDir, name), Buffer.from(r.data, 'base64')); };
  const until = async (fn, ms = 15000) => { const end = Date.now() + ms; while (Date.now() < end) { try { if (await fn()) return true; } catch {} await sleep(300); } return false; };
  await cdp('Page.navigate', {url}, S);
  results.pwAsked = await until(() => evalJs(`getComputedStyle(document.getElementById('pass')).display === 'flex' && document.getElementById('passIn').type === 'password'`), 25000);
  await shot('phone-1-password.png');
  const typeAndGo = async pw => {
    await evalJs(`document.getElementById('passIn').focus()`);
    await cdp('Input.insertText', {text: pw}, S);
    await evalJs(`document.getElementById('passGo').click()`);
  };
  await typeAndGo('not-the-password');
  results.wrongShown = await until(() => evalJs(`/4/.test(document.getElementById('passErr').textContent)`), 10000);
  results.wrongText = await evalJs(`document.getElementById('passErr').textContent`);
  await shot('phone-2-wrong.png');
  const t0 = Date.now();
  await typeAndGo(PW);
  results.playing = await until(() => evalJs(`!!window.__pmFirstFrame`), 30000);
  results.firstPictureMs = Date.now() - t0;
  await sleep(2000);
  await shot('phone-3-playing.png');
  await evalJs(`document.getElementById('bSet').click()`);  // the settings sheet: 音量 0-300 %
  await sleep(600);
  await shot('phone-3b-settings.png');
  await evalJs(`document.getElementById('bSet').click()`);
  const L1 = readLog();
  results.noAsk = !/asks to connect/.test(L1);
  results.passwordIn = /came in with the password \(fixed link\): no 允許 needed/.test(L1);
  const ipm = L1.match(/fixed: password right from (\S+)/);
  results.sourceIp = ipm ? ipm[1] : '';
  results.sourceIpPublic = !!ipm && !/^(127\.|100\.|192\.168\.|10\.)/.test(ipm[1]);
  results.tokenStored = await evalJs(`(JSON.parse(localStorage.getItem('pm.fixedTok') || '""') || '').length > 40`);
  dev(960, 0, 800); grab('5-connected');
  // Reload: the saved login goes straight in (no password screen).
  await cdp('Page.reload', {}, S);
  await sleep(1500);
  results.reloadNoPassword = await until(() => evalJs(`!!window.__pmFirstFrame && getComputedStyle(document.getElementById('pass')).display === 'none'`), 30000);
  await shot('phone-4-reloaded.png');
  // 常駐外網 off from the menu: the phone is told, port 8443 removed.
  dev(353, 0, 3000);
  results.stoppedOnPhone = await until(() => evalJs(`S.bye === 'stopped'`), 10000);
  await shot('phone-5-fixed-off.png');
  results.removedLogged = !!(await waitLog(/remote: 常駐外網: Funnel port 8443 removed/, 10000));
  const after = funnelJson();
  fs.writeFileSync(path.join(outDir, 'funnel-after-off.json'), after);
  results.gone8443 = !has8443(after);
  results.port443Same = port443(after) === port443(before);
  results.unreachable = await (async () => { try { const r = await fetch(url, {signal: AbortSignal.timeout(8000)}); return r.status >= 400; } catch { return true; } })();
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
// Safety net: if the app died without removing ITS 8443 (pointing at 127.0.0.2), remove only that.
const end = funnelJson();
if (has8443(end) && end.includes('http://127.0.0.2:')) { log('removing a leftover 8443 of this test'); try { execFileSync(TS, ['funnel', '--https=8443', 'off']); } catch {} }
const final = funnelJson();
fs.writeFileSync(path.join(outDir, 'funnel-end.json'), final);
results.endSameAsBefore = final.replace(/\s/g, '') === before.replace(/\s/g, '');
const L = readLog();
fs.writeFileSync(path.join(outDir, 'app-remote.log'), L.split(/\r?\n/).filter(l => /remote|fixed|funnel|常駐|dev remote/.test(l)).join('\n'));
const checks = [
  ['password window: too short / mismatch refused, then saved', results.shortMsg === true && results.hashStored === true],
  ['settings.ini keeps only a PBKDF2-SHA256 hash (310000 rounds), never the password (ini + log)', results.hashStored === true && results.noPlaintext === true],
  ['常駐外網 on (remote_persistent=1) -> Funnel :8443 -> 127.0.0.2, port 443 unchanged', results.persistent === true && results.ready === true && results.funnel8443On === true && results.port443SameWhileOn === true],
  ['public address: the page asks for the password (type=password)', results.pwAsked === true],
  ['wrong password: plain message with tries left', results.wrongShown === true, results.wrongText],
  ['right password: picture without 允許 on the PC', results.playing === true && results.noAsk === true && results.passwordIn === true, results.firstPictureMs + ' ms'],
  ['the log has the source address (public IP)', results.sourceIpPublic === true, results.sourceIp],
  ['login kept on the phone: reload goes straight in', results.tokenStored === true && results.reloadNoPassword === true],
  ['常駐外網 off: the phone is told, 8443 removed, address unreachable, 443 unchanged', results.stoppedOnPhone === true && results.removedLogged === true && results.gone8443 === true && results.unreachable === true && results.port443Same === true],
  ['app quit, Funnel config identical to before the test', results.appExited === true && results.endSameAsBefore === true],
];
let ok = code === 0;
const lines = checks.map(([n, p, info]) => { ok = ok && !!p; return (p ? 'PASS ' : 'FAIL ') + n + (info ? '  [' + info + ']' : ''); });
lines.push(ok ? 'ALL PASS' : 'SOME FAILED');
fs.writeFileSync(path.join(outDir, 'checks.txt'), lines.join('\n') + '\n');
console.log(lines.join('\n'));
log('results', JSON.stringify(results));
process.exit(ok ? 0 : 1);
