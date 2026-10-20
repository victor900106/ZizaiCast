// First-picture timing (TTFF) per phase, for any build of the remote page
// (0.8.0 and later): the phases are recorded by a script injected before the
// page runs (fetch / RTCPeerConnection / requestVideoFrameCallback wrappers),
// so the page itself needs no test code.
//   node ttff_test.mjs <spike.exe> <outDir> [--runs N] [--block-autoplay] [--react MS]
//                      [--approval MS] [--label NAME] [spike args...]
// --block-autoplay: like iPhone Chrome in the owner's log (playErr NotAllowedError):
//   video.play() is refused until the page got a user gesture; the harness
//   "taps" --react ms (default 1000) after the page shows a tap prompt.
// --approval MS: a new phone each run, the PC answers 允許 after MS (spike --approve-after).
// Edge headless with a throw-away profile (%TEMP%\edge-081-ttff), phone
// emulation (390x844, touch). Only the processes it started are closed.
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';

const [spikeExe, outDir, ...rest] = process.argv.slice(2);
if (!spikeExe || !outDir) { console.error('usage: node ttff_test.mjs <spike.exe> <outDir> [--runs N] [--block-autoplay] [--react MS] [--approval MS] [spike args]'); process.exit(2); }
let runs = 5, block = false, react = 1000, approvalMs = -1, label = path.basename(outDir), pageQuery = '';
const spikeArgs = [];
for (let i = 0; i < rest.length; i++) {
  if (rest[i] === '--runs') runs = +rest[++i];
  else if (rest[i] === '--block-autoplay') block = true;
  else if (rest[i] === '--react') react = +rest[++i];
  else if (rest[i] === '--approval') approvalMs = +rest[++i];
  else if (rest[i] === '--label') label = rest[++i];
  else if (rest[i] === '--page-query') pageQuery = rest[++i];
  else spikeArgs.push(rest[i]);
}
if (approvalMs >= 0) spikeArgs.push('--approval', '--approve-after', String(approvalMs));
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
const token = crypto.randomBytes(16).toString('hex');
const port = 8600 + Math.floor(Math.random() * 400);  // never 8090 (the owner's copy)
const logFile = path.join(outDir, 'spike.log');
fs.writeFileSync(logFile, '');
const spike = spawn(spikeExe, ['--token', token, '--port', String(port), '--log', logFile, '--duration', String(runs * 40 + 120),
                               '--stdin-quit', '--no-tailscale', ...spikeArgs], {stdio: ['pipe', 'pipe', 'pipe']});
log('spike pid', spike.pid, spikeArgs.join(' '));
let spikeOut = '';
spike.stdout.on('data', d => { spikeOut += d; });
spike.stderr.on('data', d => { spikeOut += d; });
let url = '';
for (let i = 0; i < 100 && !url; i++) {
  await sleep(100);
  const m = spikeOut.match(/urls: lan (http:\/\/[\d.]+:\d+\/[0-9a-f]+\/)/);
  if (m) url = m[1];
}
if (!url) { console.error('spike did not print a URL\n' + spikeOut); spike.kill(); process.exit(1); }

const profileDir = path.join(os.tmpdir(), 'edge-081-ttff');
try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
const edgeExe = 'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe';
const cdpPort = 9400 + Math.floor(Math.random() * 400);
const edge = spawn(edgeExe, [`--user-data-dir=${profileDir}`, `--remote-debugging-port=${cdpPort}`, '--no-first-run', '--no-default-browser-check',
                             '--disable-extensions', '--disable-sync', '--window-size=420,900', '--headless=new', 'about:blank'], {stdio: 'ignore'});
log('edge pid', edge.pid);
let ws, nextId = 1;
const pending = new Map();
function cdp(method, params = {}, sessionId) {
  const id = nextId++;
  ws.send(JSON.stringify({id, method, params, ...(sessionId ? {sessionId} : {})}));
  return new Promise((res, rej) => pending.set(id, {res, rej}));
}
async function cleanup(code) {
  try { if (ws) await Promise.race([cdp('Browser.close'), sleep(3000)]); } catch {}
  await sleep(1000);
  try { spike.stdin.end(); } catch {}
  for (let i = 0; i < 150 && spike.exitCode === null; i++) await sleep(100);
  for (const p of [edge, spike]) if (p.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(p.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} }
  const mine = () => {
    try {
      return execFileSync('powershell', ['-NoProfile', '-Command',
        `Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" | Where-Object { $_.CommandLine -and $_.CommandLine.Contains('${profileDir}') } | ForEach-Object { $_.ProcessId }`],
        {encoding: 'utf8'}).split(/\s+/).filter(Boolean).map(Number);
    } catch { return []; }
  };
  let left = mine();
  for (let i = 0; i < 20 && left.length; i++) { await sleep(1000); left = mine(); }
  for (const pid of left) { try { execFileSync('taskkill', ['/PID', String(pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} }
  log('own edge processes left:', mine().length, 'spike exit', spike.exitCode);
  process.exit(code);
}
let wsUrl = '';
for (let i = 0; i < 100 && !wsUrl; i++) {
  await sleep(200);
  try { wsUrl = (await (await fetch(`http://127.0.0.1:${cdpPort}/json/version`)).json()).webSocketDebuggerUrl; } catch {}
}
if (!wsUrl) { console.error('no CDP'); await cleanup(1); }
ws = new WebSocket(wsUrl);
await new Promise(r => ws.addEventListener('open', r));
ws.addEventListener('message', ev => {
  const m = JSON.parse(ev.data);
  if (m.id && pending.has(m.id)) { const p = pending.get(m.id); pending.delete(m.id); m.error ? p.rej(new Error(m.error.message)) : p.res(m.result); }
});
const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
const {sessionId: sid} = await cdp('Target.attachToTarget', {targetId, flatten: true});
const S = (m, p) => cdp(m, p, sid);
await S('Page.enable');
await S('Runtime.enable');
await S('Emulation.setDeviceMetricsOverride', {width: 390, height: 844, deviceScaleFactor: 3, mobile: true});
await S('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5});
// Phase marks (ms since navigation start), recorded by wrappers around what every page version uses.
const inject = `(() => {
  const T = window.__ttff = {marks: {}, blocked: ${block}, playRejects: 0};
  const mark = (k) => { if (T.marks[k] === undefined) T.marks[k] = Math.round(performance.now()); };
  const of = window.fetch;
  window.fetch = function (u, o) {
    const s = String(u);
    const p = of.apply(this, arguments);
    if (s.includes('/session')) { mark('session_req'); p.then(r => { if (r.status === 200) mark('offer'); else if (r.status === 202) mark('pending'); }).catch(() => {}); }
    if (s.includes('/answer')) { mark('answer_req'); p.then(() => mark('answer_done')).catch(() => {}); }
    return p;
  };
  const OPC = window.RTCPeerConnection;
  if (OPC) window.RTCPeerConnection = function (...a) {
    const pc = new OPC(...a);
    mark('pc_created');
    pc.addEventListener('connectionstatechange', () => { if (pc.connectionState === 'connected') mark('ice_connected'); });
    pc.addEventListener('track', () => mark('track'));
    return pc;
  };
  if (OPC) window.RTCPeerConnection.prototype = OPC.prototype;
  const op = HTMLMediaElement.prototype.play;
  HTMLMediaElement.prototype.play = function () {
    if (T.blocked && !this.__blessed) {
      if (navigator.userActivation && navigator.userActivation.isActive) this.__blessed = true;
      else { T.playRejects++; mark('play_rejected'); return Promise.reject(new DOMException('blocked (test)', 'NotAllowedError')); }
    }
    return op.apply(this, arguments);
  };
  const hook = () => {
    const v = document.querySelector('video');
    if (v && v.requestVideoFrameCallback) {
      // Blocked autoplay (iPhone): the autoplay attribute does not start it either.
      if (T.blocked) { v.autoplay = false; v.removeAttribute('autoplay'); }
      const cb = () => { if (!v.paused && v.videoWidth > 16) mark('first_frame'); else v.requestVideoFrameCallback(cb); };
      v.requestVideoFrameCallback(cb);
      v.addEventListener('playing', () => { if (v.videoWidth > 16) mark('first_frame'); });
    } else setTimeout(hook, 5);
  };
  hook();
  // A WebCodecs canvas (tunnel page): the page sets window.__pmFirstFrame itself.
  setInterval(() => { if (window.__pmFirstFrame && T.marks.first_frame === undefined) T.marks.first_frame = Math.round(window.__pmFirstFrame); }, 20);
})();`;
await S('Page.addScriptToEvaluateOnNewDocument', {source: inject});
const evalJs = async expr => (await S('Runtime.evaluate', {expression: expr, returnByValue: true})).result.value;
// The page's own tap prompt: 0.8.0 shows #msgT "點一下開始顯示畫面"; 0.8.1 marks <body class="need-tap">.
const promptExpr = `(() => { const b = document.body; if (!b) return null;
  if (b.classList.contains('need-tap')) return {x: innerWidth / 2, y: innerHeight / 2};
  const t = document.getElementById('msgT'), m = document.getElementById('msg');
  if (t && m && !m.classList.contains('hide') && /點一下|Tap to/.test(t.textContent)) {
    const btn = document.querySelector('#msgB button'); const r = (btn || m).getBoundingClientRect();
    return {x: r.left + r.width / 2, y: r.top + r.height / 2};
  }
  return null; })()`;
const results = [];
for (let i = 0; i < runs; i++) {
  if (approvalMs >= 0) await S('Runtime.evaluate', {expression: 'try{localStorage.clear()}catch{}'});  // a new phone each run
  const navT = Date.now();
  await S('Page.navigate', {url: url + (pageQuery ? '?' + pageQuery : '')});
  let tapped = -1, promptAt = -1;
  for (let k = 0; k < 300; k++) {  // up to 15 s
    await sleep(50);
    const st = await evalJs('window.__ttff ? JSON.stringify(window.__ttff.marks) : null').catch(() => null);
    const marks = st ? JSON.parse(st) : {};
    if (marks.first_frame !== undefined) break;
    if (block && tapped < 0) {
      const p = await evalJs(promptExpr).catch(() => null);
      if (p && promptAt < 0) promptAt = Date.now() - navT;
      if (p && promptAt >= 0 && Date.now() - navT >= promptAt + react) {
        await S('Input.dispatchTouchEvent', {type: 'touchStart', touchPoints: [{x: p.x, y: p.y}]});
        await S('Input.dispatchTouchEvent', {type: 'touchEnd', touchPoints: []});
        tapped = Date.now() - navT;
      }
    }
  }
  const marks = JSON.parse(await evalJs('JSON.stringify(window.__ttff.marks)'));
  const dbg = await evalJs('(() => { const v = document.querySelector("video"); return v ? {paused: v.paused, rejects: window.__ttff.playRejects} : null; })()').catch(() => null);
  const r = {run: i + 1, ...marks, prompt_shown: promptAt, tap_at: tapped, video: dbg};
  results.push(r);
  log(label, 'run', i + 1, JSON.stringify(r));
  await sleep(1500);
}
const num = k => results.map(r => r[k]).filter(x => typeof x === 'number' && x >= 0).sort((a, b) => a - b);
const med = k => { const a = num(k); return a.length ? a[a.length >> 1] : null; };
const keys = ['session_req', 'pending', 'offer', 'pc_created', 'answer_req', 'answer_done', 'ice_connected', 'track', 'play_rejected', 'prompt_shown', 'tap_at', 'first_frame'];
const summary = {label, block, react, approvalMs, runs, median: Object.fromEntries(keys.map(k => [k, med(k)])),
                 first_frame_all: num('first_frame'), results};
fs.writeFileSync(path.join(outDir, 'summary.json'), JSON.stringify(summary, null, 2));
await sleep(500);
const offers = (fs.readFileSync(logFile, 'utf8').match(/ready in \d+ ms/g) || []).map(s => +s.match(/\d+/)[0]);
summary.host_offer_ms = offers;
fs.writeFileSync(path.join(outDir, 'summary.json'), JSON.stringify(summary, null, 2));
log('median', JSON.stringify(summary.median), 'host offer ms', offers.join(','));
await cleanup(0);
