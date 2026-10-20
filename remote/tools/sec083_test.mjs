// 0.8.3 before / after checks for the remote-control fixes (node 22+).
//   node sec083_test.mjs <pm_remote_spike.exe> <outDir> [--before] [--only a,b,...] [--mb N]
// Every scenario starts its own spike (dry-run input, test clipboard, own random port,
// --no-tailscale except the Tailscale file run) and, where a page is needed, a headless
// Edge with a throw-away profile. Nothing is played, typed or shown; no tailscale /
// cloudflared is ever run (refused below); only the processes started here are closed.
// --before: the 0.8.2 / e69c71e spike (no file routes: those checks are skipped).
// Scenarios: http (password limits, relay headers, body cap, slow request, concurrent
// session set-up), page (direct mode, malformed messages, fps, latency, ttff, files on
// the LAN), viewonly (clipboard / files in 僅觀看), fixed (中斷 vs the same login,
// 解除僅觀看 with the password), ts (files over the Tailscale address).
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import net from 'node:net';
import http from 'node:http';
import crypto from 'node:crypto';

const [spikeExe, outDir, ...rest] = process.argv.slice(2);
let before = false, only = null, mbs = 64;
for (let i = 0; i < rest.length; i++) {
  if (rest[i] === '--before') before = true;
  else if (rest[i] === '--only') only = rest[++i].split(',');
  else if (rest[i] === '--mb') mbs = +rest[++i];
}
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
const results = {};
const PW = 'Zizai-Test-2026';
const want = name => !only || only.includes(name);

// ---- spike ----
async function startSpike(tag, args) {
  for (const bad of ['--funnel', '--tunnel', '--fixed-tunnel', '--inject', '--tone', '--test-tone'])
    if (args.includes(bad)) throw new Error('refused: ' + bad);
  const token = crypto.randomBytes(16).toString('hex');
  const port = 18200 + Math.floor(Math.random() * 600);
  const logFile = path.join(outDir, tag + '-spike.log');
  fs.writeFileSync(logFile, '');
  const p = spawn(spikeExe, ['--token', token, '--port', String(port), '--log', logFile, '--duration', '400', '--stdin-quit', ...(args.includes('--synthetic-audio') ? [] : ['--no-audio']), ...args],
                  {stdio: ['pipe', 'pipe', 'pipe']});
  let out = '';
  p.stdout.on('data', d => { out += d; });
  p.stderr.on('data', d => { out += d; });
  let lan = '', ts = '';
  for (let i = 0; i < 150 && !lan; i++) {
    await sleep(100);
    const m = out.match(/urls: lan (http:\/\/[\d.]+:\d+\/[0-9a-f]+\/) tailscale (\S+)/);
    if (m) lan = m[1], ts = m[2] === '-' ? '' : m[2];
  }
  if (!lan) throw new Error('spike did not start: ' + out.slice(0, 400));
  const ip = lan.match(/\/\/([\d.]+):/)[1];
  log(tag, 'spike pid', p.pid, args.join(' '));
  return {p, token, port, lan, ts, ip, logFile, readLog: () => fs.readFileSync(logFile, 'utf8'), alive: () => p.exitCode === null};
}
async function stopSpike(s) {
  try { s.p.stdin.end(); } catch {}
  for (let i = 0; i < 100 && s.p.exitCode === null; i++) await sleep(100);
  if (s.p.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(s.p.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} log('killed own spike', s.p.pid); }
}

// ---- plain HTTP with any headers (the relays' own ones included) ----
function req(host, port, method, p, body, headers = {}) {
  return new Promise(res => {
    const t0 = Date.now();
    const len = body === undefined ? 0 : Buffer.byteLength(body);  // (no chunked bodies: the PC's server reads Content-Length)
    const r = http.request({host, port, method, path: p, headers: {'content-type': 'application/json', 'content-length': String(len), ...headers}, timeout: 20000}, rs => {
      let b = '';
      rs.on('data', d => { b += d; });
      rs.on('end', () => res({status: rs.statusCode, body: b, ms: Date.now() - t0}));
    });
    r.on('error', e => res({status: 0, body: String(e.code || e), ms: Date.now() - t0}));
    r.on('timeout', () => { r.destroy(); res({status: 0, body: 'timeout', ms: Date.now() - t0}); });
    if (body !== undefined) r.write(body);
    r.end();
  });
}
// A raw request: what comes back within waitMs (status line or nothing).
function raw(host, port, text, waitMs) {
  return new Promise(res => {
    const s = net.connect(port, host);
    let got = '';
    const t0 = Date.now();
    s.on('connect', () => s.write(text));
    s.on('data', d => { got += d; });
    const done = why => { s.destroy(); res({got: got.split('\r\n')[0], why, ms: Date.now() - t0}); };
    s.on('close', () => done('closed'));
    s.on('error', () => done('error'));
    setTimeout(() => done('waited'), waitMs);
  });
}
// A request that sends one header line every `every` ms: how long the server keeps it.
function slow(host, port, p, every, maxMs) {
  return new Promise(res => {
    const s = net.connect(port, host);
    const t0 = Date.now();
    let timer = 0, n = 0;
    s.on('connect', () => {
      s.write(`GET ${p} HTTP/1.1\r\nHost: x\r\n`);
      timer = setInterval(() => { try { s.write(`X-Slow-${n++}: 1\r\n`); } catch {} }, every);
    });
    const done = why => { clearInterval(timer); s.destroy(); res({heldMs: Date.now() - t0, why}); };
    s.on('close', () => done('server closed'));
    s.on('error', () => done('error'));
    setTimeout(() => done('still open at the cap'), maxMs);
  });
}

// ---- headless Edge over CDP ----
async function startEdge(tag) {
  const profileDir = path.join(os.tmpdir(), 'edge-083-sec-' + tag + '-' + crypto.randomBytes(3).toString('hex'));
  const cdpPort = 9600 + Math.floor(Math.random() * 300);
  const p = spawn('C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
                  [`--user-data-dir=${profileDir}`, `--remote-debugging-port=${cdpPort}`, '--no-first-run', '--no-default-browser-check', '--headless=new',
                   '--mute-audio', '--disable-extensions', '--disable-sync', 'about:blank'], {stdio: 'ignore'});
  let ver;
  for (let i = 0; i < 80 && !ver; i++) { await sleep(200); try { ver = await (await fetch(`http://127.0.0.1:${cdpPort}/json/version`)).json(); } catch {} }
  const ws = new WebSocket(ver.webSocketDebuggerUrl);
  await new Promise(r => ws.addEventListener('open', r));
  let nextId = 1;
  const pend = new Map();
  ws.addEventListener('message', e => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m.result || m.error); pend.delete(m.id); } });
  const cdp = (method, params = {}, sessionId) => new Promise(r => { const id = nextId++; pend.set(id, r); ws.send(JSON.stringify({id, method, params, ...(sessionId ? {sessionId} : {})})); });
  const e = {p, profileDir, cdp};
  e.open = async url => {
    const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
    const {sessionId} = await cdp('Target.attachToTarget', {targetId, flatten: true});
    await cdp('Emulation.setDeviceMetricsOverride', {width: 390, height: 844, deviceScaleFactor: 3, mobile: true}, sessionId);
    await cdp('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5}, sessionId);
    await cdp('Page.navigate', {url}, sessionId);
    const ev = async (expression, awaitPromise = false) => {
      const r = await cdp('Runtime.evaluate', {expression, returnByValue: true, awaitPromise}, sessionId);
      return r && r.result ? r.result.value : undefined;
    };
    return {targetId, sessionId, ev, close: () => cdp('Target.closeTarget', {targetId})};
  };
  e.close = async () => {
    try { await Promise.race([cdp('Browser.close'), sleep(3000)]); } catch {}
    for (let i = 0; i < 50 && p.exitCode === null; i++) await sleep(100);
    if (p.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(p.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} }
    await sleep(800);
    try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
  };
  return e;
}
const waitFor = async (fn, ms, step = 200) => { for (let t = 0; t < ms; t += step) { const v = await fn(); if (v) return v; await sleep(step); } return null; };
const firstFrame = tab => waitFor(() => tab.ev('window.__pmFirstFrame ? Math.round(window.__pmFirstFrame) : 0'), 20000);
// Every DataChannel / WebSocket message from the PC, kept in window.__msgs.
const hookMsgs = tab => tab.ev(`(() => { window.__msgs = []; const d = dc; if (!d) return false; const o = d.onmessage; d.onmessage = e => { try { window.__msgs.push(JSON.parse(e.data)); } catch {} o(e); }; return true; })()`);

// ================================================================ scenarios
async function scenarioHttp() {
  const s = await startSpike('http', ['--no-tailscale', '--fixed-password', PW]);
  const r = {};
  const login = (pw, xff, extra = {}) => req('127.0.0.2', s.port, 'POST', '/login', JSON.stringify({password: pw}), {'x-forwarded-for': xff, ...extra});
  // 1. relay headers: Funnel's X-Forwarded-For last entry is the phone; a client-sent CF-Connecting-IP / first XFF entries are not
  await login('wrong-x', '1.2.3.4, 203.0.113.10', {'cf-connecting-ip': '9.9.9.9'});
  await sleep(300);
  const m = [...s.readLog().matchAll(/WRONG password from (\S+)/g)].map(x => x[1]);
  r.headers = {sent: 'CF-Connecting-IP: 9.9.9.9, X-Forwarded-For: 1.2.3.4, 203.0.113.10 (via the fixed link = Funnel)', loggedAs: m[m.length - 1]};
  // 2. password limits: 5 wrong from one address, then the right one from another
  const wrong = [];
  for (let i = 0; i < 5; i++) wrong.push((await login('wrong-' + i, '203.0.113.5')).status);
  const sixth = await login('wrong-6', '203.0.113.5');
  const owner = await login(PW, '198.51.100.7');
  r.lockout = {wrongStatuses: wrong, sixthFromSame: sixth.status, ownerFromOtherAddress: owner.status, ownerBody: owner.body.slice(0, 60)};
  // 3. a huge body announced before any check
  const big = await raw(s.ip, s.port, `POST /${s.token}/session HTTP/1.1\r\nHost: x\r\nContent-Length: 8000000\r\n\r\n`, 3000);
  r.bodyCap = {announced: '8,000,000 bytes on POST session', answer: big.got || '(nothing)', how: big.why, ms: big.ms};
  // 4. a request that never ends (one header line every 3 s)
  r.slowRequest = await slow(s.ip, s.port, `/${s.token}/`, 3000, 32000);
  // 5. two session set-ups at the same moment (two phones / two tabs)
  const body = d => JSON.stringify({device: d, q: 0, m: 0});
  const [a, b] = await Promise.all([req(s.ip, s.port, 'POST', `/${s.token}/session`, body('deviceAAAA1111')), req(s.ip, s.port, 'POST', `/${s.token}/session`, body('deviceBBBB2222'))]);
  await sleep(1500);
  const lg = s.readLog();
  r.concurrent = {statuses: [a.status, b.status], offersBuilt: (lg.match(/webrtc\[\w+\]: offer for/g) || []).length,
                  closedAsReplaced: (lg.match(/closing \(replaced by a new session\)/g) || []).length};
  // 6. malformed JSON on HTTP routes (handled, the program stays up)
  const bad = [];
  for (const b2 of ['{"device": 12345}', '[1,2', '{"device":"abcdefgh12","q":"x","m":{}}'])
    bad.push((await req(s.ip, s.port, 'POST', `/${s.token}/session`, b2)).status);
  bad.push((await req('127.0.0.2', s.port, 'POST', '/login', '{"password": 7}', {'x-forwarded-for': '203.0.113.77'})).status);
  r.malformedHttp = {statuses: bad, alive: s.alive()};
  // 7. the wrong-link block: 9 wrong paths from one address, 257 other addresses once each, then the 10th
  for (let i = 0; i < 9; i++) await req('127.0.0.2', s.port, 'GET', '/nope-' + i, undefined, {'x-forwarded-for': '203.0.113.99'});
  for (let i = 0; i < 257; i++) await req('127.0.0.2', s.port, 'GET', '/x', undefined, {'x-forwarded-for': `198.18.${i >> 8}.${i & 255}`});
  await req('127.0.0.2', s.port, 'GET', '/nope-10', undefined, {'x-forwarded-for': '203.0.113.99'});
  const after10 = await req('127.0.0.2', s.port, 'GET', '/', undefined, {'x-forwarded-for': '203.0.113.99'});
  r.wrongLinkBlock = {tenthWrongAfter257Others: /203\.0\.113\.99 used .* -> blocked/.test(s.readLog()) ? 'blocked' : 'NOT blocked', nextRequest: after10.status};
  await stopSpike(s);
  return r;
}

// ---- a raw WebSocket client (masked frames) for the tunnel path ----
function wsConnect(host, port, p, headers) {
  return new Promise(res => {
    const s = net.connect(port, host);
    const t0 = Date.now();
    let head = '', up = false, closedAt = 0;
    s.on('connect', () => s.write(`GET ${p} HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n` +
                                  `Sec-WebSocket-Version: 13\r\n${Object.entries(headers).map(([k, v]) => k + ': ' + v + '\r\n').join('')}\r\n`));
    s.on('data', d => { if (!up) { head += d; if (head.includes('\r\n\r\n')) { up = head.startsWith('HTTP/1.1 101'); res(c); } } });
    s.on('close', () => { closedAt = Date.now(); if (!up) res(c); });
    s.on('error', () => {});
    const c = {
      ok: () => up, closedAfterMs: () => closedAt ? closedAt - t0 : 0, t0,
      send: text => { const b = Buffer.from(text), m = crypto.randomBytes(4), h = Buffer.from([0x81, 0x80 | b.length]); const x = Buffer.alloc(b.length); for (let i = 0; i < b.length; i++) x[i] = b[i] ^ m[i & 3]; s.write(Buffer.concat([h, m, x])); },
      end: () => s.destroy(),
    };
  });
}
async function scenarioWs() {
  const s = await startSpike('ws', ['--no-tailscale', '--fixed-password', PW]);
  const r = {};
  const xff = {'x-forwarded-for': '203.0.113.60'};
  const lg = JSON.parse((await req('127.0.0.2', s.port, 'POST', '/login', JSON.stringify({password: PW}), xff)).body);
  const off = JSON.parse((await req('127.0.0.2', s.port, 'POST', `/k/${lg.token}/session`, JSON.stringify({device: 'wsdevice0001', q: 0, m: 0}), xff)).body);
  const p = `/k/${lg.token}/ws?id=${off.id}`;
  const a = await wsConnect('127.0.0.2', s.port, p, xff);
  a.send(JSON.stringify({t: 'ws-media', on: true, why: 'test'}));  // active (the session is not "never connected")
  await sleep(1500);
  // a reconnect with the same session: the old connection
  const b = await wsConnect('127.0.0.2', s.port, p, xff);
  await sleep(2000);
  r.replacedConnection = {firstUpgraded: a.ok(), secondUpgraded: b.ok(), firstClosedAfterReplace: a.closedAfterMs() ? 'closed' : 'still open'};
  a.end();
  // the second one goes silent: how long it holds its connection slot (cap 80 s)
  const t0 = Date.now();
  await waitFor(async () => b.closedAfterMs() > 0, 80000, 500);
  r.silentConnection = {heldMs: b.closedAfterMs() ? Date.now() - t0 : null, cappedAt80s: !b.closedAfterMs()};
  b.end();
  await stopSpike(s);
  return r;
}

async function scenarioPage() {
  const dir = path.join(outDir, 'page-files');
  fs.mkdirSync(dir, {recursive: true});
  const offered = path.join(dir, 'offered.bin');
  fs.writeFileSync(offered, crypto.randomBytes(mbs * 1048576));
  const up = path.join(dir, 'up');
  fs.rmSync(up, {recursive: true, force: true});
  const s = await startSpike('page', ['--no-tailscale', '--scene', 'motion', '--test-clipboard', 'clip-secret-083', ...(before ? [] : ['--upload-dir', up, '--test-files', offered])]);
  const e = await startEdge('page');
  const r = {};
  try {
    const tab = await e.open(s.lan);
    r.ttffMs = await firstFrame(tab);
    r.defaultMode = await tab.ev('trackpad.mode');
    await sleep(1500);
    // frame rate on a moving screen with nobody touching: 12 s, the last 8 counted from the page
    await sleep(4000);
    const f = [];
    for (let i = 0; i < 8; i++) { await sleep(1000); f.push(await tab.ev('Math.round(S.fps)')); }
    r.fpsUntouched = f;
    const srvFps = [...s.readLog().matchAll(/frame rate (\d+) -> (\d+) fps \(([^)]+)\)/g)].map(m => `${m[1]}->${m[2]} (${m[3]})`);
    r.fpsTiers = srvFps;
    // latency: the capture stamp (HUD) for 10 s
    await tab.ev(`(() => { prefs.hud = true; send({t: 'stamp', on: true}); syncSettings(); return 1; })()`);
    await sleep(10000);
    r.latency = await tab.ev(`({lat_p50: Math.round(S.latShow), lat_p95: Math.round(S.latP95), rtt: Math.round(S.rtt * 10) / 10, fpsNow: S.srv && S.srv.fpsNow,
      e2e: typeof S.e2e === 'number' && isFinite(S.e2e) ? Math.round(S.e2e) : null, e2eP95: typeof S.e2eP95 === 'number' && isFinite(S.e2eP95) ? Math.round(S.e2eP95) : null,
      stampOk: S.stampOk, stampBad: S.stampBad})`);
    if (r.latency && r.latency.e2e === null && isFinite(r.latency.lat_p50))  // the 0.8.2 page has no e2e: the same formula here
      r.latency.e2eComputed = Math.round(r.latency.rtt / 2 + 500 / (r.latency.fpsNow || 60) + r.latency.lat_p50);
    await tab.ev(`(() => { prefs.hud = false; send({t: 'stamp', on: false}); syncSettings(); return 1; })()`);
    // malformed DataChannel messages (types the page never sends)
    await hookMsgs(tab);
    const sent = await tab.ev(`(() => { const m = ['{"t":"ping","id":"x","c":"y"}', '{"t":"ack","n":"q"}', '{"t":"bitrate","kbps":"a"}', '{"t":"audio","on":"yes"}', '{"t":"move","dx":"a","dy":[1],"n":"z"}', '{"t":"clip-get","watch":3}', '[]', '{"t":7}', 'not json'];
      for (const x of m) dc.send(x); return m.length; })()`);
    await sleep(2500);
    r.malformedDc = {sent, spikeAlive: s.alive(), pageState: await tab.ev('S.state'), ignoredLogged: (s.readLog().match(/message ignored/g) || []).length};
    if (!s.alive()) throw new Error('spike gone after malformed messages');
    // files (after only)
    if (!before) {
      // upload: a generated file through the page's own uploadFile()
      const up1 = await tab.ev(`(async () => { const b = new Uint8Array(${mbs} * 1048576); for (let i = 0; i < b.length; i += 4096) b[i] = i & 255;
        const t0 = performance.now(); await uploadFile(new File([b], 'phone-${mbs}MB.bin')); return {ms: Math.round(performance.now() - t0), files: __pmUp.files, errors: __pmUp.errors}; })()`, true);
      r.uploadLan = {...up1, mb: mbs, MBps: up1 && up1.ms ? +(mbs / (up1.ms / 1000)).toFixed(1) : null,
                     savedOk: fs.existsSync(path.join(up, `phone-${mbs}MB.bin`)) && fs.statSync(path.join(up, `phone-${mbs}MB.bin`)).size === mbs * 1048576};
      // names from the phone that must not escape the folder / hit a device name
      await tab.ev(`(async () => { for (const n of ['../../evil.txt', '..\\\\..\\\\evil2.txt', 'CON.txt', 'nul', 'a<b>:c?.txt', '....', 'same.txt', 'same.txt']) await uploadFile(new File(['x'], n)); return 1; })()`, true);
      r.uploadNames = fs.readdirSync(up).filter(n => !n.startsWith('phone-')).sort();
      r.escaped = fs.existsSync(path.join(dir, 'evil.txt')) || fs.existsSync(path.join(outDir, 'evil.txt'));
      // download: the copied file, its one-time link twice
      await tab.ev(`send({t: 'files-get'})`);
      const href = await waitFor(() => tab.ev(`(() => { const a = document.querySelector('#dlList a'); return a ? a.href : ''; })()`), 5000);
      const dl = await tab.ev(`(async () => { const t0 = performance.now(); const r = await fetch(${JSON.stringify(href)}); const b = await r.arrayBuffer(); const ms = performance.now() - t0;
        const again = await fetch(${JSON.stringify(href)}); return {status: r.status, bytes: b.byteLength, ms: Math.round(ms), again: again.status}; })()`, true);
      r.downloadLan = {...dl, mb: mbs, MBps: dl && dl.ms ? +(mbs / (dl.ms / 1000)).toFixed(1) : null};
    }
    // the tab is closed: how long the PC keeps the session (the bar up, input open)
    const closes0 = (s.readLog().match(/closing \(/g) || []).length;
    const tc = Date.now();
    await tab.close();
    const gone = await waitFor(() => (s.readLog().match(/closing \(/g) || []).length > closes0, 45000, 250);
    r.tabClosed = {sessionEndedAfterMs: gone ? Date.now() - tc : null, cappedAt45s: !gone, why: ((s.readLog().match(/closing \(([^)]*)\)/g) || []).slice(-1)[0] || '')};
  } catch (err) { r.error = String(err && err.message || err); }
  await e.close();
  await stopSpike(s);
  return r;
}

async function scenarioViewOnly() {
  const dir = path.join(outDir, 'vo-files');
  fs.mkdirSync(dir, {recursive: true});
  const offered = path.join(dir, 'secret.txt');
  fs.writeFileSync(offered, 'secret file');
  const s = await startSpike('vo', ['--no-tailscale', '--view-only', '--test-clipboard', 'clip-secret-083', ...(before ? [] : ['--upload-dir', path.join(dir, 'up'), '--test-files', offered])]);
  const e = await startEdge('vo');
  const r = {};
  try {
    const tab = await e.open(s.lan);
    r.ttffMs = await firstFrame(tab);
    await sleep(1500);
    await hookMsgs(tab);
    await tab.ev(`send({t: 'clip-get', watch: true})`);
    await tab.ev(`send({t: 'files-get'})`);
    await sleep(2000);
    const msgs = await tab.ev('window.__msgs.filter(m => m.t !== "pong" && m.t !== "stats" && m.t !== "cur")');
    r.clipboardToViewOnlyPhone = msgs.some(m => m.t === 'clip' && m.s === 'clip-secret-083') ? 'SENT (PC clipboard text reached a view-only phone)' : 'not sent';
    r.messages = msgs.map(m => m.t);
    if (!before) {
      const up = await tab.ev(`(async () => { const r = await post('up?id=' + encodeURIComponent(sessId) + '&f=abcdef0123&n=x.txt&o=0&t=1', 'x'); return r.status; })()`, true);
      r.uploadInViewOnly = up;
    }
    await tab.close();
  } catch (err) { r.error = String(err && err.message || err); }
  await e.close();
  await stopSpike(s);
  return r;
}

async function scenarioFixed() {
  const r = {};
  // a) 中斷 vs the same login: the spike ends the session 6 s after it is active
  {
    const s = await startSpike('fixed', ['--no-tailscale', '--fixed-password', PW, '--kick-after', '6']);
    const e = await startEdge('fixed');
    try {
      const tab = await e.open(`http://127.0.0.2:${s.port}/`);
      await waitFor(() => tab.ev(`$('pass').style.display === 'flex'`), 8000);
      await tab.ev(`(() => { $('passIn').value = ${JSON.stringify(PW)}; submitPass(); return 1; })()`);
      r.ttffMs = await firstFrame(tab);
      const tok = await tab.ev(`store.get('fixedTok', '')`);
      await waitFor(() => tab.ev(`S.bye === 'kicked'`), 15000);
      r.kickedOnPage = await tab.ev('S.bye');
      // the same login straight back in, as "a new phone" (its own new device id)
      const again = await req('127.0.0.2', s.port, 'POST', `/k/${tok}/session`, JSON.stringify({device: 'brandnewdevice0001', q: 0, m: 0}), {'x-forwarded-for': '203.0.113.50'});
      const check = await req('127.0.0.2', s.port, 'POST', `/k/${tok}/check`, '', {'x-forwarded-for': '203.0.113.50'});
      r.sameLoginAfterKick = {session: again.status, sessionBody: again.body.slice(0, 50), check: check.status};
      await tab.close();
    } catch (err) { r.errorA = String(err && err.message || err); }
    await e.close();
    await stopSpike(s);
  }
  // b) 解除僅觀看 with the password (fixed link, 僅觀看 on)
  {
    const s = await startSpike('unlock', ['--no-tailscale', '--fixed-password', PW, '--view-only']);
    const e = await startEdge('unlock');
    try {
      const tab = await e.open(`http://127.0.0.2:${s.port}/`);
      await waitFor(() => tab.ev(`$('pass').style.display === 'flex'`), 8000);
      await tab.ev(`(() => { $('passIn').value = ${JSON.stringify(PW)}; submitPass(); return 1; })()`);
      r.unlockTtffMs = await firstFrame(tab);
      await sleep(1000);
      r.viewOnlyAtStart = await tab.ev('S.viewOnly');
      await hookMsgs(tab);
      const inputLines = () => (s.readLog().match(/input( \(dry-run\))?: \w+ click at/g) || []).length;
      const send = x => tab.ev(`(() => { send(${JSON.stringify(x)}); return 1; })()`);
      const in0 = inputLines();
      await send({t: 'abs', x: 0.5, y: 0.5}); await send({t: 'click', b: 0});
      await sleep(800);
      r.inputBeforeUnlock = inputLines() - in0;
      await send({t: 'unlock', pw: 'not-the-password'});
      await sleep(1500);
      await send({t: 'unlock', pw: PW});
      await sleep(1500);
      r.unlockAnswers = await tab.ev('window.__msgs.filter(m => m.t === "unlock")');
      r.viewOnlyAfterUnlock = await tab.ev('S.viewOnly');
      const in1 = inputLines();
      await send({t: 'abs', x: 0.4, y: 0.4}); await send({t: 'click', b: 0});
      await sleep(800);
      r.inputAfterUnlock = inputLines() - in1;
      await tab.close();
    } catch (err) { r.errorB = String(err && err.message || err); }
    await e.close();
    await stopSpike(s);
  }
  // c) malformed messages over the tunnel WebSocket (the fixed link's page opens one): 0.8.2
  //    let the exception out of the connection's thread (std::terminate = the program ends)
  {
    const s = await startSpike('wsbad', ['--no-tailscale', '--fixed-password', PW]);
    const e = await startEdge('wsbad');
    try {
      const tab = await e.open(`http://127.0.0.2:${s.port}/`);
      await waitFor(() => tab.ev(`$('pass').style.display === 'flex'`), 8000);
      await tab.ev(`(() => { $('passIn').value = ${JSON.stringify(PW)}; submitPass(); return 1; })()`);
      await firstFrame(tab);
      const open = await waitFor(() => tab.ev('!!(tun.ws && tun.wsOpen)'), 8000);
      await tab.ev(`(() => { tun.ws.send('{"t":"ping","id":"x","c":"y"}'); tun.ws.send('{"t":"ack","n":"q"}'); return 1; })()`);
      await sleep(2500);
      r.wsMalformed = {wsOpen: !!open, spikeAlive: s.alive(), exitCode: s.p.exitCode, ignoredLogged: (s.readLog().match(/message ignored/g) || []).length};
      await tab.close();
    } catch (err) { r.errorC = String(err && err.message || err); }
    await e.close();
    await stopSpike(s);
  }
  return r;
}

// Sound level (silent synthetic sources, nothing is played): what the phone gets, AGC off (= 0.8.2) / on.
async function scenarioAudio() {
  if (before) return {skipped: 'A / B with --no-agc on the 0.8.3 spike (the 0.8.2 spike has no "quiet" source)'};
  const r = {};
  for (const [tag, args] of [['quiet-agc-off', ['--synthetic-audio', 'quiet', '--no-agc']], ['quiet-agc-on', ['--synthetic-audio', 'quiet']],
                             ['music-agc-off', ['--synthetic-audio', 'music', '--no-agc']], ['music-agc-on', ['--synthetic-audio', 'music']]]) {
    const s = await startSpike('audio-' + tag, ['--no-tailscale', ...args]);
    const e = await startEdge('audio');
    try {
      const tab = await e.open(s.lan);
      await firstFrame(tab);
      await tab.ev("(() => { send({t: 'audio', on: true}); return 1; })()");
      const lv = [];
      for (let i = 0; i < 16; i++) { await sleep(1000); lv.push(await tab.ev('S.aud ? +(S.aud.level || 0).toFixed(4) : null')); }
      const st = [...s.readLog().matchAll(/stats audio: .*? peak ([\d.]+) \(AGC x([\d.]+)/g)].map(m => ({peak: +m[1], agc: +m[2]}));
      r[tag] = {phoneAudioLevelMax: Math.max(...lv.filter(x => x !== null)), phoneAudioLevels: lv, pcStats: st};
      await tab.close();
    } catch (err) { r[tag] = {error: String(err && err.message || err)}; }
    await e.close();
    await stopSpike(s);
  }
  return r;
}

async function scenarioTs() {
  if (before) return {skipped: 'no file routes before 0.8.3'};
  const dir = path.join(outDir, 'ts-files');
  fs.mkdirSync(dir, {recursive: true});
  const offered = path.join(dir, 'offered.bin');
  fs.writeFileSync(offered, crypto.randomBytes(mbs * 1048576));
  const up = path.join(dir, 'up');
  fs.rmSync(up, {recursive: true, force: true});
  const s = await startSpike('ts', ['--test-clipboard', 'x', '--upload-dir', up, '--test-files', offered]);
  const r = {};
  if (!s.ts) { await stopSpike(s); return {skipped: 'no Tailscale address on this PC'}; }
  const e = await startEdge('ts');
  try {
    const tab = await e.open(s.ts);
    r.ttffMs = await firstFrame(tab);
    await sleep(1000);
    const up1 = await tab.ev(`(async () => { const b = new Uint8Array(${mbs} * 1048576); const t0 = performance.now(); await uploadFile(new File([b], 'ts-${mbs}MB.bin'));
      return {ms: Math.round(performance.now() - t0), files: __pmUp.files, errors: __pmUp.errors}; })()`, true);
    r.uploadTs = {...up1, mb: mbs, MBps: up1 && up1.ms ? +(mbs / (up1.ms / 1000)).toFixed(1) : null};
    await tab.ev(`send({t: 'files-get'})`);
    const href = await waitFor(() => tab.ev(`(() => { const a = document.querySelector('#dlList a'); return a ? a.href : ''; })()`), 5000);
    const dl = await tab.ev(`(async () => { const t0 = performance.now(); const r = await fetch(${JSON.stringify(href)}); const b = await r.arrayBuffer(); return {status: r.status, bytes: b.byteLength, ms: Math.round(performance.now() - t0)}; })()`, true);
    r.downloadTs = {...dl, mb: mbs, MBps: dl && dl.ms ? +(mbs / (dl.ms / 1000)).toFixed(1) : null};
    r.via = s.ts.replace(/\/[0-9a-f]{32}\//, '/<token>/');
    await tab.close();
  } catch (err) { r.error = String(err && err.message || err); }
  await e.close();
  await stopSpike(s);
  return r;
}

for (const [name, fn] of [['http', scenarioHttp], ['ws', scenarioWs], ['audio', scenarioAudio], ['page', scenarioPage], ['viewonly', scenarioViewOnly], ['fixed', scenarioFixed], ['ts', scenarioTs]]) {
  if (!want(name)) continue;
  log('---- ' + name);
  try { results[name] = await fn(); } catch (err) { results[name] = {error: String(err && err.message || err)}; }
  log(name, JSON.stringify(results[name]));
}
fs.writeFileSync(path.join(outDir, 'results.json'), JSON.stringify({before, spike: spikeExe, results}, null, 1));
log('done');
