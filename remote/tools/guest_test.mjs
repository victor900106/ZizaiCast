// 0.8.3.2 臨時帳號 (guest accounts) security checks (node 22+), against pm_remote_spike only:
//   node guest_test.mjs <pm_remote_spike.exe> <outDir>
// The spike runs with --loopback (127.0.0.3 / 127.0.0.2 only: no firewall prompt), --no-tailscale,
// dry-run input, a test clipboard and a throw-away upload folder; its fixed link (127.0.0.2, what
// Funnel would deliver) has the owner's password and these accounts (all made up):
//   g-once   用一次, 可操作            g-view   1 hour, 僅觀看        g-files  1 hour, 可操作 + 傳檔
//   g-short  4 seconds, 可操作         g-drop   1 hour, deleted 3 s after its phone is connected
// A headless, muted Edge (own throw-away profile) is the phone: it logs in with the page's own form.
// Nothing is run besides the spike and Edge (no tailscale / cloudflared); only they are closed.
// Checks: a guest login is not a 30-day one and needs 允許; 用一次 works once, then 401 (login and
// its old link); expired -> 401; 僅觀看 = input dropped on the PC; no 傳檔 = upload / download /
// list refused (403); deleting the account ends its session at once; no password in the log.
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import http from 'node:http';
import crypto from 'node:crypto';

const [spikeExe, outDir] = process.argv.slice(2);
if (!spikeExe || !outDir) { console.error('usage: node guest_test.mjs <pm_remote_spike.exe> <outDir>'); process.exit(2); }
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
const OWNER = 'Owner-Test-2026';
const G = {once: 'ABC234', view: 'DEF567', files: 'GHJ789', short: 'KMN234', drop: 'PQR567'};
const upDir = path.join(outDir, 'uploads');
fs.rmSync(upDir, {recursive: true, force: true});
const offered = path.join(outDir, 'offered.txt');
fs.writeFileSync(offered, 'a file copied on the PC');

const token = crypto.randomBytes(16).toString('hex');
const port = 19700 + Math.floor(Math.random() * 200);
const logFile = path.join(outDir, 'spike.log');
fs.writeFileSync(logFile, '');
const readLog = () => fs.readFileSync(logFile, 'utf8');
const spike = spawn(spikeExe, ['--token', token, '--port', String(port), '--log', logFile, '--duration', '300', '--stdin-quit', '--loopback', '--no-tailscale',
  '--no-audio', '--test-clipboard', 'x', '--test-files', offered, '--upload-dir', upDir, '--fixed-password', OWNER,
  '--guest', `g-once:${G.once}:once:control`, '--guest', `g-view:${G.view}:3600:view`, '--guest', `g-files:${G.files}:3600:control+files`,
  '--guest', `g-short:${G.short}:4:control`, '--guest', `g-drop:${G.drop}:3600:control`, '--guest-drop', 'g-drop:3'],
  {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
let out = '';
spike.stdout.on('data', d => { out += d; });
spike.stderr.on('data', d => { out += d; });
const t0 = Date.now();
for (let i = 0; i < 150 && !/fixed: origin/.test(readLog()); i++) await sleep(100);
log('spike pid', spike.pid);

function req(method, p, body) {
  return new Promise(res => {
    const r = http.request({host: '127.0.0.2', port, method, path: p, headers: {'content-type': 'application/json', 'content-length': String(body ? Buffer.byteLength(body) : 0),
      'x-forwarded-for': '203.0.113.50'}, timeout: 20000}, rs => {
      let b = ''; rs.on('data', d => { b += d; }); rs.on('end', () => { let j = null; try { j = JSON.parse(b); } catch {} res({status: rs.statusCode, json: j, body: b}); });
    });
    r.on('error', e => res({status: 0, body: String(e.code)}));
    if (body) r.write(body);
    r.end();
  });
}
const login = (user, password) => req('POST', '/login', JSON.stringify({user, password}));

// ---- the phone ----
const profileDir = path.join(os.tmpdir(), 'edge-0832-guest-' + crypto.randomBytes(3).toString('hex'));
const cdpPort = 9300 + Math.floor(Math.random() * 90);
const edge = spawn('C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe', [`--user-data-dir=${profileDir}`, `--remote-debugging-port=${cdpPort}`,
  '--no-first-run', '--no-default-browser-check', '--headless=new', '--mute-audio', '--allow-loopback-in-peer-connection', '--disable-extensions', 'about:blank'],
  {stdio: 'ignore', windowsHide: true});
let ver;
for (let i = 0; i < 80 && !ver; i++) { await sleep(200); try { ver = await (await fetch(`http://127.0.0.1:${cdpPort}/json/version`)).json(); } catch {} }
const ws = new WebSocket(ver.webSocketDebuggerUrl);
await new Promise(r => ws.addEventListener('open', r));
let nextId = 1;
const pend = new Map();
ws.addEventListener('message', e => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m.result || m.error); pend.delete(m.id); } });
const cdp = (method, params = {}, sessionId) => new Promise(r => { const i = nextId++; pend.set(i, r); ws.send(JSON.stringify({id: i, method, params, ...(sessionId ? {sessionId} : {})})); });
async function tab(url) {
  const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
  const {sessionId} = await cdp('Target.attachToTarget', {targetId, flatten: true});
  await cdp('Emulation.setDeviceMetricsOverride', {width: 390, height: 844, deviceScaleFactor: 3, mobile: true}, sessionId);
  await cdp('Page.navigate', {url}, sessionId);
  const ev = async (expression, awaitPromise = false) => { const r = await cdp('Runtime.evaluate', {expression, returnByValue: true, awaitPromise}, sessionId); return r && r.result ? r.result.value : undefined; };
  return {ev, close: () => cdp('Target.closeTarget', {targetId})};
}
const waitFor = async (fn, ms) => { for (let t = 0; t < ms; t += 200) { const v = await fn(); if (v) return v; await sleep(200); } return null; };
// The page's own form: account + password, then the picture (after 允許: the spike allows).
async function phoneLogin(user, pw) {
  await cdp('Storage.clearDataForOrigin', {origin: `http://127.0.0.2:${port}`, storageTypes: 'local_storage'});
  const t = await tab(`http://127.0.0.2:${port}/`);
  await waitFor(() => t.ev(`$('pass').style.display === 'flex'`), 8000);
  t.userShown = await t.ev(`$('userIn').style.display !== 'none'`);
  await t.ev(`(() => { $('userIn').value = ${JSON.stringify(user)}; $('passIn').value = ${JSON.stringify(pw)}; submitPass(); return 1; })()`);
  t.frame = !!(await waitFor(() => t.ev('window.__pmFirstFrame ? 1 : 0'), 20000));
  t.savedLogin = await t.ev(`store.get('fixedTok', '')`);
  t.base = await t.ev('BASE');
  return t;
}
const count = re => (readLog().match(re) || []).length;

const r = {};
try {
  // 1. logins: the owner's (30 days) vs a guest's (no 30-day login, at most 12 h)
  const own = await login('', OWNER);
  const exp = tok => +(tok || '0').split('.')[0] - Math.floor(Date.now() / 1000);
  r.ownerLogin = {status: own.status, secondsValid: exp(own.json && own.json.token)};
  const gv = await login('G-VIEW', G.view.toLowerCase());  // the name and the password: any case
  r.guestLogin = {status: gv.status, guest: gv.json && gv.json.guest, secondsValid: exp(gv.json && gv.json.token)};
  const wrong = await login('g-view', 'XXXXXX');
  const nobody = await login('nobody', 'XXXXXX');
  r.wrongGuest = {wrongPassword: wrong.status, unknownAccount: nobody.status, left: wrong.json && wrong.json.left};
  // 2. 用一次: a phone logs in (允許 asked and given), leaves; then the same account again -> 401
  {
    const asked0 = count(/NEW DEVICE asks to connect: .*臨時帳號 g-once/g);
    const t = await phoneLogin('g-once', G.once);
    r.once = {userFieldShown: t.userShown, connected: t.frame, savedLogin: t.savedLogin, askedForAllow: count(/NEW DEVICE asks to connect: .*臨時帳號 g-once/g) > asked0};
    const tok = t.base.split('/')[2];
    await t.close();
    await waitFor(async () => /臨時帳號 g-once: its session ended -> spent/.test(readLog()), 15000);
    r.once.secondLogin = (await login('g-once', G.once)).status;
    r.once.oldLink = (await req('POST', `/k/${tok}/check`)).status;
    r.once.oldLinkSession = (await req('POST', `/k/${tok}/session`, JSON.stringify({device: 'onceagain0001', q: 0, m: 0}))).status;
  }
  // 3. expired: g-short lived 4 s
  await sleep(Math.max(0, 5000 - (Date.now() - t0)));
  r.expired = {login: (await login('g-short', G.short)).status};
  // 4. 僅觀看: the page's clicks / keys reach the PC as nothing
  {
    const t = await phoneLogin('g-view', G.view);
    const before = count(/input \(dry-run\): /g);
    await t.ev(`(() => { send({t: 'click', b: 0}); send({t: 'key', k: 'Enter'}); send({t: 'text', s: 'hello'}); return 1; })()`);
    await sleep(1500);
    r.viewOnly = {connected: t.frame, pageViewOnly: await t.ev('S.viewOnly'), inputLogged: count(/input \(dry-run\): /g) - before,
                  unlockOffered: await t.ev('!!(S.unlockable)')};
    // and no files for it either (no 傳檔)
    r.viewOnly.upload = await t.ev(`(async () => { const q = await fetch(BASE + 'up?id=' + encodeURIComponent(sessId || '') + '&f=abcdefgh12&n=x.txt&o=0&t=5', {method: 'POST', body: 'hello'}); return q.status; })()`, true);
    await t.close();
    await sleep(1500);
  }
  // 5. 可操作 without 傳檔 (g-drop) and with (g-files): upload / list
  {
    const t = await phoneLogin('g-files', G.files);
    const before = count(/input \(dry-run\): /g);
    await t.ev(`(() => { send({t: 'click', b: 0}); return 1; })()`);
    await sleep(1000);
    r.files = {connected: t.frame, inputLogged: count(/input \(dry-run\): /g) - before};
    r.files.upload = await t.ev(`(async () => { await uploadFile(new File(['from the guest'], 'guest.txt')); return __pmUp; })()`, true);
    await t.close();
    await sleep(1500);
  }
  // 6. deleting the account ends its session at once (g-drop: deleted 3 s after it is connected); also no 傳檔 for it
  {
    const t = await phoneLogin('g-drop', G.drop);
    r.drop = {connected: t.frame};
    r.drop.upload = await t.ev(`(async () => { const q = await fetch(BASE + 'up?id=' + encodeURIComponent(sessId || '') + '&f=abcdefgh13&n=y.txt&o=0&t=5', {method: 'POST', body: 'hello'}); return q.status; })()`, true);
    await t.ev(`(() => { window.__fl = []; const o = dc.onmessage; dc.onmessage = e => { try { window.__fl.push(JSON.parse(e.data).t); } catch {} o(e); }; send({t: 'files-get'}); return 1; })()`);
    await sleep(800);
    r.drop.filesList = await t.ev('window.__fl');
    const tDel = Date.now();
    const ended = await waitFor(async () => /ended: its 臨時帳號 g-drop was deleted/.test(readLog()), 15000);
    r.drop.endedOnThePc = !!ended;
    r.drop.pageBye = await waitFor(() => t.ev('S.bye || S.state === "bye" ? (S.bye || S.state) : ""'), 5000);
    r.drop.secondsAfterConnected = +((Date.now() - tDel) / 1000).toFixed(1);
    r.drop.loginAfter = (await login('g-drop', G.drop)).status;
    await t.close();
  }
} catch (e) { r.error = String(e && e.stack || e); }
try { await cdp('Browser.close'); } catch {}
for (let i = 0; i < 50 && edge.exitCode === null; i++) await sleep(100);
if (edge.exitCode === null) try { execFileSync('taskkill', ['/PID', String(edge.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
try { spike.stdin.end(); } catch {}
for (let i = 0; i < 100 && spike.exitCode === null; i++) await sleep(100);
if (spike.exitCode === null) try { execFileSync('taskkill', ['/PID', String(spike.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
await sleep(500);
try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
const L = readLog();
r.plaintextInLog = [OWNER, ...Object.values(G)].filter(p => L.includes(p) || L.includes(p.toLowerCase()));
const up = r.files && r.files.upload || {};
const checks = [
  ['owner login is the 30-day one', r.ownerLogin && r.ownerLogin.status === 200 && r.ownerLogin.secondsValid > 29 * 86400],
  ['guest login: 200, no 30-day login (at most 12 h)', r.guestLogin && r.guestLogin.status === 200 && r.guestLogin.guest === true && r.guestLogin.secondsValid <= 12 * 3600 + 5],
  ['wrong guest password / unknown account: 401 alike', r.wrongGuest && r.wrongGuest.wrongPassword === 401 && r.wrongGuest.unknownAccount === 401],
  ['page has the account field', r.once && r.once.userFieldShown === true],
  ['用一次: connected after 允許 was asked', r.once && r.once.connected && r.once.askedForAllow],
  ['用一次: the page keeps no 30-day login', r.once && r.once.savedLogin === ''],
  ['用一次: a second login -> 401', r.once && r.once.secondLogin === 401],
  ['用一次: its old link -> 401', r.once && r.once.oldLink === 401 && r.once.oldLinkSession === 401],
  ['expired account -> 401', r.expired && r.expired.login === 401],
  ['僅觀看: input dropped on the PC (0 inputs)', r.viewOnly && r.viewOnly.connected && r.viewOnly.inputLogged === 0 && r.viewOnly.pageViewOnly === true],
  ['僅觀看: no 解除僅觀看 offered', r.viewOnly && r.viewOnly.unlockOffered === false],
  ['no 傳檔: upload 403', r.viewOnly && r.viewOnly.upload === 403 && r.drop && r.drop.upload === 403],
  ['no 傳檔: file list refused', r.drop && Array.isArray(r.drop.filesList) && r.drop.filesList.includes('files-denied')],
  ['可操作 + 傳檔: input arrives, upload saved', r.files && r.files.inputLogged > 0 && (up.files || 0) >= 1 && fs.existsSync(path.join(upDir, 'guest.txt'))],
  ['deleted while connected: the session ends at once', r.drop && r.drop.connected && r.drop.endedOnThePc && r.drop.secondsAfterConnected < 6],
  ['deleted: its login -> 401', r.drop && r.drop.loginAfter === 401],
  ['no password in the log', r.plaintextInLog.length === 0],
];
r.checks = checks.map(([n, ok]) => `${ok ? 'PASS' : 'FAIL'} ${n}`);
fs.writeFileSync(path.join(outDir, 'guest_test.json'), JSON.stringify(r, null, 1));
for (const c of r.checks) console.log(c);
process.exit(checks.every(c => c[1]) ? 0 : 1);
