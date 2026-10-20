// 0.8.3 app-level checks (node 22+): 遠端斷線後自動鎖定電腦 (test stub), 連線紀錄 (rotation,
// masking), 中斷 ends the fixed-link login also after a restart, 3 wrong passwords -> tray note,
// a file from the phone -> the PC's notice.
//   node app083_test.mjs <自在投影.exe> <outDir>
// Own --dev --test-offscreen instance and data folder (PM_DEV_INSTANCE=remote083app), input
// dry-run, test clipboard, no Tailscale listener, the fixed link faked (PM_REMOTE_FUNNELFAKE:
// nothing is run, port 8443 is never touched). --dev never calls LockWorkStation (it logs
// "TEST STUB"). A headless Edge (own throw-away profile) is the phone. Nothing is shown,
// played or typed; only the processes started here are closed.
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import http from 'node:http';
import crypto from 'node:crypto';

const [exe, outDir] = process.argv.slice(2);
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const devCmdPs1 = path.resolve(here, '..', '..', 'share', 'tools', 'dev_cmd.ps1');
const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-remote083app');
fs.rmSync(dataDir, {recursive: true, force: true});
fs.mkdirSync(dataDir, {recursive: true});
const PW = 'Zizai-Test-2026';
const salt = crypto.randomBytes(16);
const pwHash = `pbkdf2-sha256$310000$${salt.toString('hex')}$${crypto.pbkdf2Sync(PW, salt, 310000, 32, 'sha256').toString('hex')}`;
const token = crypto.randomBytes(24).toString('base64').replace(/[^A-Za-z0-9]/g, '').slice(0, 32).padEnd(32, 'x');
const port = 18900 + Math.floor(Math.random() * 300);
fs.writeFileSync(path.join(dataDir, 'settings.ini'),
  `[settings]\nlanguage=zh-TW\nremote_token=${token}\nremote_port=${port}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\ntoolbar_hint_shown=1\n` +
  `whats_new_seen=0.8.3\nremote_persistent=1\nremote_pw_hash=${pwHash}\nremote_fixed_secret=${crypto.randomBytes(32).toString('hex')}\n`);
// a full connection log (70 KB of old lines): the first new line rotates it
fs.writeFileSync(path.join(dataDir, 'remote_connections.log'), ('2026-01-01 00:00:00\tlan\t10.0.*.*\told phone\tok\n').repeat(1500));
const appLog = path.join(dataDir, 'phonemirror.log');
const readLog = () => { try { return fs.readFileSync(appLog, 'utf8'); } catch { return ''; } };
const env = {...process.env, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: 'remote083app', PM_REMOTE_DRYRUN: '1', PM_REMOTE_TESTCLIP: '1', PM_REMOTE_NOTS: '1',
             PM_REMOTE_FUNNELFAKE: 'https://example-pc.example-tailnet.ts.net:8443/'};
let app = null;
const dev = (cmd, lp = 0, wait = 600) => { try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmdPs1, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe'}); } catch {} };
async function startApp() {
  app = spawn(exe, ['--dev', '--test-offscreen'], {stdio: 'ignore', env});
  log('app pid', app.pid);
  for (let i = 0; i < 100 && !/remote: on at http/.test(readLog()); i++) await sleep(200);
  await sleep(1500);
}
async function stopApp() {
  dev(104, 0, 500);
  for (let i = 0; i < 100 && app.exitCode === null; i++) await sleep(100);
  if (app.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} log('killed own app'); }
}
function req(host, p, method, body, headers = {}) {
  return new Promise(res => {
    const r = http.request({host, port, method, path: p, headers: {'content-type': 'application/json', 'content-length': String(body ? Buffer.byteLength(body) : 0), ...headers}, timeout: 15000}, rs => {
      let b = ''; rs.on('data', d => { b += d; }); rs.on('end', () => res({status: rs.statusCode, body: b}));
    });
    r.on('error', e => res({status: 0, body: String(e.code)}));
    if (body) r.write(body);
    r.end();
  });
}

// ---- the phone ----
const profileDir = path.join(os.tmpdir(), 'edge-083-app-' + crypto.randomBytes(3).toString('hex'));
const cdpPort = 9900 + Math.floor(Math.random() * 90);
let edge = null, ws = null, nextId = 1;
const pend = new Map();
const cdp = (method, params = {}, sessionId) => new Promise(r => { const i = nextId++; pend.set(i, r); ws.send(JSON.stringify({id: i, method, params, ...(sessionId ? {sessionId} : {})})); });
async function startEdge() {
  edge = spawn('C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe', [`--user-data-dir=${profileDir}`, `--remote-debugging-port=${cdpPort}`, '--no-first-run',
    '--headless=new', '--mute-audio', 'about:blank'], {stdio: 'ignore'});
  let ver;
  for (let i = 0; i < 60 && !ver; i++) { await sleep(200); try { ver = await (await fetch(`http://127.0.0.1:${cdpPort}/json/version`)).json(); } catch {} }
  ws = new WebSocket(ver.webSocketDebuggerUrl);
  await new Promise(r => ws.addEventListener('open', r));
  ws.addEventListener('message', e => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m.result || m.error); pend.delete(m.id); } });
}
async function tab(url) {
  const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
  const {sessionId} = await cdp('Target.attachToTarget', {targetId, flatten: true});
  await cdp('Emulation.setDeviceMetricsOverride', {width: 390, height: 844, deviceScaleFactor: 3, mobile: true}, sessionId);
  await cdp('Page.navigate', {url}, sessionId);
  const ev = async (expression, awaitPromise = false) => { const r = await cdp('Runtime.evaluate', {expression, returnByValue: true, awaitPromise}, sessionId); return r && r.result ? r.result.value : undefined; };
  return {ev, close: () => cdp('Target.closeTarget', {targetId})};
}
const waitFor = async (fn, ms) => { for (let t = 0; t < ms; t += 200) { const v = await fn(); if (v) return v; await sleep(200); } return null; };
const frame = t => waitFor(() => t.ev('window.__pmFirstFrame ? 1 : 0'), 20000);
async function fixedLogin() {
  // a fresh login each time (the page would otherwise go straight in with the one it saved)
  await cdp('Storage.clearDataForOrigin', {origin: `http://127.0.0.2:${port}`, storageTypes: 'local_storage'});
  const t = await tab(`http://127.0.0.2:${port}/`);
  await waitFor(() => t.ev(`$('pass').style.display === 'flex'`), 8000);
  await t.ev(`(() => { $('passIn').value = ${JSON.stringify(PW)}; submitPass(); return 1; })()`);
  await frame(t);
  await waitFor(() => t.ev(`!!store.get('fixedTok', '')`), 5000);
  return t;
}
const stubs = () => (readLog().match(/TEST STUB: would lock the PC now \(([^)]*)/g) || []);

const out = {};
try {
  await startApp();
  const lan = (readLog().match(/remote: on at (http:\/\/[\d.]+:\d+\/\w+\/)/) || [])[1];
  if (!lan) throw new Error('remote did not come on');
  await startEdge();
  // 1. LAN phone: allowed + remembered, then it goes away (the page closes): LAN lock is off by default
  {
    const t = await tab(lan);
    await sleep(2500); dev(961, 3, 600);
    await frame(t);
    out.lanLockNote = await t.ev(`$('lockNote').style.display !== 'none'`);
    await sleep(1500);
    await t.close();
    await sleep(8000);
    out.afterLanSession = {lockStubs: stubs().length};
  }
  // 2. the fixed link (from outside): the phone goes away -> the PC would lock (outside: on by default)
  {
    const t = await fixedLogin();
    out.fixedLockNote = await t.ev(`$('lockNote').style.display !== 'none'`);
    await sleep(1500);
    await t.ev("(() => { store.set('fixedTok', ''); return 1; })()");  // the next tab logs in afresh
    await t.close();
    await sleep(8000);
    out.afterFixedSession = {lockStubs: stubs().length, last: stubs().slice(-1)[0] || null};
  }
  // 3. 中斷 on the PC: no lock (the person is at the PC); the login is revoked and kept
  let tok = '';
  {
    const t = await fixedLogin();
    tok = await t.ev(`store.get('fixedTok', '')`);
    await sleep(1500);
    dev(965, 2, 600);
    await sleep(3000);
    out.afterKick = {lockStubs: stubs().length, pageBye: await t.ev('S.bye')};
    await t.close();
    const s = await req('127.0.0.2', `/k/${tok}/session`, 'POST', JSON.stringify({device: 'otherdevice0001', q: 0, m: 0}), {'x-forwarded-for': '203.0.113.40'});
    out.sameLoginAfterKick = s.status;
    const ini = fs.readFileSync(path.join(dataDir, 'settings.ini'), 'utf8');
    out.revokedInSettings = /remote_revoked=[0-9a-f]{16}:\d+/.test(ini);
  }
  // 4. three wrong passwords from outside -> a tray note (logged: no tray icon in --test-offscreen)
  for (let i = 0; i < 3; i++) await req('127.0.0.2', '/login', 'POST', JSON.stringify({password: 'nope' + i}), {'x-forwarded-for': '203.0.113.88'});
  await sleep(800);
  out.threeWrong = {logged: /3 wrong passwords in a row from 203\.0\.113\.88/.test(readLog()), balloon: (readLog().match(/tray balloon[^\n]*連續輸錯密碼 3 次[^\n]*/) || [''])[0].slice(0, 160)};
  // 5. a file from the phone (LAN, remembered): saved in the instance's own folder, the PC is told
  {
    const t = await tab(lan);
    await frame(t);
    await sleep(1000);
    out.upload = await t.ev(`(async () => { await uploadFile(new File(['hello from the phone'], '../手機的檔案.txt')); return __pmUp; })()`, true);
    await sleep(800);
    out.uploadNotice = (readLog().match(/remote: file from the phone saved: [^\n]*/) || [''])[0];
    out.uploadFiles = fs.existsSync(path.join(dataDir, 'uploads')) ? fs.readdirSync(path.join(dataDir, 'uploads')) : [];
    await t.close();
  }
  // 6. 連線紀錄
  dev(965, 1, 600);
  await sleep(500);
  out.connectionLog = ((readLog().match(/dev remote connection log: ([^\n]*)/) || [])[1] || '').split('|').filter(Boolean).slice(0, 8);
  out.connectionLogRotated = fs.existsSync(path.join(dataDir, 'remote_connections.old.log'));
  out.connectionLogFile = fs.readFileSync(path.join(dataDir, 'remote_connections.log'), 'utf8').trim().split('\n');
  out.passwordInLog = /Zizai-Test-2026/.test(fs.readFileSync(path.join(dataDir, 'remote_connections.log'), 'utf8') + readLog());
  await stopApp();
  // 7. after a restart the revoked login stays revoked
  await startApp();
  const s2 = await req('127.0.0.2', `/k/${tok}/session`, 'POST', JSON.stringify({device: 'otherdevice0002', q: 0, m: 0}), {'x-forwarded-for': '203.0.113.41'});
  out.sameLoginAfterRestart = s2.status;
  await stopApp();
} catch (e) { out.error = String(e && e.message || e); }
try { if (ws) await cdp('Browser.close'); } catch {}
for (let i = 0; i < 50 && edge && edge.exitCode === null; i++) await sleep(100);
if (edge && edge.exitCode === null) try { execFileSync('taskkill', ['/PID', String(edge.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
if (app && app.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} }
fs.writeFileSync(path.join(outDir, 'app083.json'), JSON.stringify(out, null, 1));
fs.writeFileSync(path.join(outDir, 'app.log'), readLog().replace(/\/[A-Za-z0-9]{32}\//g, '/<token>/').replace(/\/k\/[0-9a-f.]+\//g, '/k/<login>/'));
log(JSON.stringify(out));
