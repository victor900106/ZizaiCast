// 0.8.3.1: "connected, but the page stays on 連線中 / no picture" (owner's iPhone Safari, 2026-10-21
// 07:48 on the fixed link), reproduced with a headless Edge as the phone (node 22+):
//   node novideo_test.mjs <pm_remote_spike.exe> <outDir> [--only a,b] [--seconds N] [--label NAME]
// Every scenario starts its own spike: --loopback (the "LAN" side on 127.0.0.3, nothing listens on
// the LAN: no Windows Firewall prompt), --no-audio (nothing is played or muted on the PC), input
// dry-run, --scene static (a PC nobody touches: unchanged frames are not encoded), the fixed link
// on 127.0.0.2 with a test password. The page comes in like the owner's did: with its SAVED 30-day
// login (no password typed = no tap on the page at all) and quality 2 (3440x1440 here, level 5.2).
// Scenarios:
//   owner     WebKit-like autoplay block (play() refused with NotAllowedError until a user gesture,
//             as iPhone Low Power Mode does; a paused MediaStream <video> reports videoWidth 0, as
//             the owner's page-diag did), nobody taps: does a picture come, what does the page say?
//   tap       the same, the "phone" taps 3 s after the page asks for it: tap -> picture
//   normal    no autoplay block: first picture (regression check)
//   stuck     the session request never gets an answer (a relay that hangs): what does the page say?
//   nopic     no autoplay block, but the direct <video> never shows a picture (videoWidth stays 0, as
//             if nothing decodes): does the page ask the PC for a key frame, then switch paths?
// Edge runs headless with a throw-away profile (no window anywhere); CDP touch events go to that
// page only (no OS input). tailscale / cloudflared are never run. Only processes started here are
// closed, and the script checks that none of them is left.
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import http from 'node:http';
import crypto from 'node:crypto';

const [spikeExe, outDir, ...rest] = process.argv.slice(2);
if (!spikeExe || !outDir) { console.error('usage: node novideo_test.mjs <spike.exe> <outDir> [--only a,b] [--seconds N] [--label NAME]'); process.exit(2); }
let only = null, seconds = 20, label = path.basename(outDir);
for (let i = 0; i < rest.length; i++) {
  if (rest[i] === '--only') only = rest[++i].split(',');
  else if (rest[i] === '--seconds') seconds = +rest[++i];
  else if (rest[i] === '--label') label = rest[++i];
}
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
const PW = 'test-' + crypto.randomBytes(6).toString('hex');
const edgeExe = 'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe';
const started = [];  // {name, proc, profile?}

function req(host, port, method, p, body, headers = {}) {
  return new Promise(res => {
    const r = http.request({host, port, method, path: p, headers: {'content-type': 'application/json', 'content-length': Buffer.byteLength(body || ''), ...headers}, timeout: 8000}, s => {
      let b = ''; s.on('data', d => { b += d; }); s.on('end', () => res({status: s.statusCode, body: b}));
    });
    r.on('error', () => res({status: 0, body: ''}));
    r.on('timeout', () => { r.destroy(); res({status: 0, body: ''}); });
    if (body) r.write(body);
    r.end();
  });
}

async function startSpike(name, extra) {
  const port = 8600 + Math.floor(Math.random() * 400);
  const logFile = path.join(outDir, name + '-spike.log');
  fs.writeFileSync(logFile, '');
  const args = ['--token', crypto.randomBytes(16).toString('hex'), '--port', String(port), '--log', logFile, '--duration', String(seconds + 90),
                '--stdin-quit', '--loopback', '--no-tailscale', '--no-audio', '--scene', 'static', '--fixed-password', PW, ...extra];
  for (const a of args) if (/tailscale\.exe|cloudflared|--funnel|--fixed-tunnel|--tunnel$|--inject/.test(a)) throw new Error('refused: ' + a);
  const proc = spawn(spikeExe, args, {stdio: ['pipe', 'pipe', 'pipe']});
  started.push({name: 'spike', proc});
  let out = '';
  proc.stdout.on('data', d => { out += d; });
  proc.stderr.on('data', d => { out += d; });
  for (let i = 0; i < 100 && !/urls: lan/.test(out); i++) await sleep(100);
  if (!/urls: lan http:\/\/127\.0\.0\.3:/.test(out)) throw new Error('spike: no loopback URL\n' + out.slice(0, 2000));
  log(name, 'spike pid', proc.pid, 'port', port);
  return {proc, port, logFile, readLog: () => fs.readFileSync(logFile, 'utf8')};
}
async function stopSpike(s) {
  try { s.proc.stdin.end(); } catch {}
  for (let i = 0; i < 150 && s.proc.exitCode === null; i++) await sleep(100);
  if (s.proc.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(s.proc.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} }
}

async function startEdge(name) {
  const profile = path.join(os.tmpdir(), 'edge-0831-novideo-' + name + '-' + crypto.randomBytes(3).toString('hex'));
  const cdpPort = 9400 + Math.floor(Math.random() * 400);
  const proc = spawn(edgeExe, [`--user-data-dir=${profile}`, `--remote-debugging-port=${cdpPort}`, '--no-first-run', '--no-default-browser-check',
    '--disable-extensions', '--disable-sync', '--allow-loopback-in-peer-connection', '--window-size=420,900', '--headless=new', 'about:blank'], {stdio: 'ignore'});
  started.push({name: 'edge', proc, profile});
  let wsUrl = '';
  for (let i = 0; i < 100 && !wsUrl; i++) {
    await sleep(200);
    try { wsUrl = (await (await fetch(`http://127.0.0.1:${cdpPort}/json/version`)).json()).webSocketDebuggerUrl; } catch {}
  }
  if (!wsUrl) throw new Error('no CDP');
  const ws = new WebSocket(wsUrl);
  await new Promise(r => ws.addEventListener('open', r));
  let nextId = 1;
  const pending = new Map();
  ws.addEventListener('message', ev => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) { const p = pending.get(m.id); pending.delete(m.id); m.error ? p.rej(new Error(m.error.message)) : p.res(m.result); }
  });
  const cdp = (method, params = {}, sessionId) => { const id = nextId++; ws.send(JSON.stringify({id, method, params, ...(sessionId ? {sessionId} : {})}));
    return new Promise((res, rej) => pending.set(id, {res, rej})); };
  const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
  const {sessionId: sid} = await cdp('Target.attachToTarget', {targetId, flatten: true});
  const S = (m, p) => cdp(m, p, sid);
  await S('Page.enable');
  await S('Runtime.enable');
  await S('Emulation.setDeviceMetricsOverride', {width: 440, height: 764, deviceScaleFactor: 3, mobile: true});  // the owner's vw / vh
  await S('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5});
  const evalJs = async expr => (await S('Runtime.evaluate', {expression: expr, returnByValue: true})).result.value;
  return {proc, profile, S, evalJs, close: async () => { try { await Promise.race([cdp('Browser.close'), sleep(3000)]); } catch {} }};
}
const ownEdgeLeft = profile => {
  try {
    return execFileSync('powershell', ['-NoProfile', '-Command',
      `Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" | Where-Object { $_.CommandLine -and $_.CommandLine.Contains('${profile}') } | ForEach-Object { $_.ProcessId }`],
      {encoding: 'utf8'}).split(/\s+/).filter(Boolean).map(Number);
  } catch { return []; }
};
async function stopEdge(e) {
  await e.close();
  let left = ownEdgeLeft(e.profile);
  for (let i = 0; i < 20 && left.length; i++) { await sleep(1000); left = ownEdgeLeft(e.profile); }
  for (const pid of left) { try { execFileSync('taskkill', ['/PID', String(pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} }
  try { fs.rmSync(e.profile, {recursive: true, force: true}); } catch {}
}

// Runs before the page: the saved login + quality (as on the owner's phone), the WebKit-like
// autoplay block, and marks for the first picture (direct <video> or the tunnel's canvas).
const injectFor = ({tok, block, stuck, nopic}) => `(() => {
  if (location.host.startsWith('127.0.0.2:')) {
    try { if (!localStorage.getItem('pm.fixedTok')) localStorage.setItem('pm.fixedTok', JSON.stringify(${JSON.stringify(tok)})); localStorage.setItem('pm.q', '2'); } catch {}
  }
  const T = window.__nv = {t0: performance.now(), marks: {}, playRejects: 0, block: ${!!block}, nopic: ${!!nopic}};
  const mark = k => { if (T.marks[k] === undefined) T.marks[k] = Math.round(performance.now()); };
  const of = window.fetch;
  window.fetch = function (u, o) {
    const s = String(u);
    if (${!!stuck} && /\\/session/.test(s)) { mark('session_stuck'); return new Promise(() => {}); }
    const p = of.apply(this, arguments);
    if (/\\/session/.test(s)) p.then(r => { if (r.status === 200) mark('offer'); }).catch(() => {});
    return p;
  };
  const vw = Object.getOwnPropertyDescriptor(HTMLVideoElement.prototype, 'videoWidth');
  const vh = Object.getOwnPropertyDescriptor(HTMLVideoElement.prototype, 'videoHeight');
  T.realW = v => vw.get.call(v);
  if (T.nopic) {
    Object.defineProperty(HTMLVideoElement.prototype, 'videoWidth', {configurable: true, get() { return 0; }});
    Object.defineProperty(HTMLVideoElement.prototype, 'videoHeight', {configurable: true, get() { return 0; }});
  }
  if (T.block) {
    const op = HTMLMediaElement.prototype.play;
    HTMLMediaElement.prototype.play = function () {
      if (!this.__blessed) {
        if (navigator.userActivation && navigator.userActivation.isActive) this.__blessed = true;
        else { T.playRejects++; mark('play_rejected'); return Promise.reject(new DOMException('blocked (test: like iPhone Low Power Mode)', 'NotAllowedError')); }
      }
      return op.apply(this, arguments);
    };
    // The owner's page-diag: videoW 0 while paused (frames were decoded: dec_ms, frameWidth 3440).
    Object.defineProperty(HTMLVideoElement.prototype, 'videoWidth', {configurable: true, get() { return this.paused ? 0 : vw.get.call(this); }});
    Object.defineProperty(HTMLVideoElement.prototype, 'videoHeight', {configurable: true, get() { return this.paused ? 0 : vh.get.call(this); }});
  }
  const hook = () => {
    const v = document.getElementById('v');
    if (!v) return setTimeout(hook, 5);
    if (T.block) { v.autoplay = false; v.removeAttribute('autoplay'); }  // the attribute does not start it either
    const cb = () => { if (!T.nopic && !v.paused && vw.get.call(v) > 16 && getComputedStyle(v).opacity !== '0') { mark('first_frame'); T.via = T.via || 'direct'; } else v.requestVideoFrameCallback(cb); };
    v.requestVideoFrameCallback(cb);
  };
  hook();
  setInterval(() => {
    const c = document.getElementById('scr');
    if (window.__pmFirstFrame && c && c.style.display === 'block' && c.width > 16 && T.marks.first_frame === undefined) { T.marks.first_frame = Math.round(performance.now()); T.via = 'tunnel'; }
  }, 50);
})();`;

const stateExpr = `(() => {
  const m = document.getElementById('msg'), t = document.getElementById('msgT'), s = document.getElementById('msgS'), sp = document.getElementById('msgSpin');
  const th = document.getElementById('tapHint');
  const shown = m && !m.classList.contains('hide');
  const btns = shown ? Array.from(document.querySelectorAll('#msgB button')).map(b => b.textContent) : [];
  const c = document.getElementById('scr');
  const v = document.getElementById('v');
  return {t: Math.round(performance.now()), msg: shown ? t.textContent : '', sub: shown ? s.textContent.slice(0, 90) : '', spin: !!(shown && sp && sp.style.display !== 'none'),
          btns, tapPill: !!(th && th.style.display !== 'none' && th.style.display !== ''), needTap: document.body.classList.contains('need-tap'),
          marks: window.__nv ? window.__nv.marks : {}, via: window.__nv ? window.__nv.via || '' : '', rejects: window.__nv ? window.__nv.playRejects : 0,
          paused: v ? v.paused : null, realW: v && window.__nv ? window.__nv.realW(v) : 0,
          now: c && c.style.display === 'block' ? 'tunnel' : v && !v.paused && window.__nv && window.__nv.realW(v) > 16 && getComputedStyle(v).opacity !== '0' && !window.__nv.nopic ? 'direct' : ''};
})()`;
// Where to tap: the page's own button if it has one, else the middle (the whole page takes the tap).
const tapAtExpr = `(() => { const b = document.querySelector('#msg:not(.hide) #msgB button'); if (b) { const r = b.getBoundingClientRect(); return {x: r.left + r.width / 2, y: r.top + r.height / 2}; }
  return {x: innerWidth / 2, y: innerHeight / 2}; })()`;

async function scenario(name, {block = false, tapAfter = -1, stuck = false, nopic = false}) {
  const sp = await startSpike(name, []);
  const lg = await req('127.0.0.2', sp.port, 'POST', '/login', JSON.stringify({password: PW}));
  const tok = (JSON.parse(lg.body || '{}') || {}).token;
  if (!tok) throw new Error('login failed: ' + lg.status);
  const e = await startEdge(name);
  await e.S('Page.addScriptToEvaluateOnNewDocument', {source: injectFor({tok, block, stuck, nopic})});
  const navT = Date.now();
  await e.S('Page.navigate', {url: `http://127.0.0.2:${sp.port}/`});
  const timeline = [];
  let asked = -1, tapped = -1, last = '';
  while (Date.now() - navT < seconds * 1000) {
    await sleep(250);
    const st = await e.evalJs(stateExpr).catch(() => null);
    if (!st) continue;
    const key = [st.msg, st.spin, st.btns.join('|'), st.tapPill, st.via].join('/');
    if (key !== last) { last = key; timeline.push({ms: Date.now() - navT, msg: st.msg, sub: st.sub, spin: st.spin, btns: st.btns, tapPill: st.tapPill, via: st.via}); }
    if (asked < 0 && (st.needTap || st.tapPill || st.btns.length)) asked = Date.now() - navT;
    if (tapAfter >= 0 && tapped < 0 && asked >= 0 && Date.now() - navT >= asked + tapAfter) {
      const p = await e.evalJs(tapAtExpr);
      await e.S('Input.dispatchTouchEvent', {type: 'touchStart', touchPoints: [{x: p.x, y: p.y}]});
      await e.S('Input.dispatchTouchEvent', {type: 'touchEnd', touchPoints: []});
      tapped = Date.now() - navT;
    }
    if (st.marks.first_frame !== undefined && (tapAfter < 0 || tapped >= 0) && Date.now() - navT > 8000 && name !== 'owner') break;
  }
  const fin = await e.evalJs(stateExpr).catch(() => ({}));
  await e.S('Page.captureScreenshot', {format: 'png'}).then(r => fs.writeFileSync(path.join(outDir, name + '.png'), Buffer.from(r.data, 'base64'))).catch(() => {});
  await stopEdge(e);
  await sleep(500);
  await stopSpike(sp);
  const L = sp.readLog();
  const count = re => (L.match(re) || []).length;
  const r = {
    scenario: name, block, stuck, tapAfter,
    first_picture_ms: fin.marks && fin.marks.first_frame !== undefined ? fin.marks.first_frame : null,
    via: fin.via || '', path_at_end: fin.now || '', play_rejects: fin.rejects, asked_tap_ms: asked, tapped_ms: tapped,
    final_msg: fin.msg || '', final_sub: fin.sub || '', final_spin: !!fin.spin, final_btns: fin.btns || [],
    stream: (L.match(/stream (\d+x\d+)@\d+ \d+ kbps, level ([\d.]+)/g) || []).slice(-1)[0] || '',
    host_first_frame: (L.match(/first frame sent [^\n]*/) || [''])[0],
    host_idr: (L.match(/(\d+) IDR \|/g) || []).map(x => +x.match(/\d+/)[0]).reduce((a, b) => a + b, 0),
    host_pli: count(/PLI\/FIR from the browser/g), host_page_idr: count(/asked for a key frame/g),
    tunnel_media: count(/picture and sound now over the tunnel/g), back_to_direct: count(/no longer over the tunnel [(]direct is playing[)]/g),
    page_idr_why: (L.match(/asked for a key frame [(]([^)]*)[)]/g) || []), quality_drop: count(/lower quality: no picture/g),
    diags: (L.match(/page-diag [^:]+: \{"what":"([\w-]+)"/g) || []).map(x => x.match(/"what":"([\w-]+)"/)[1]),
    timeline,
  };
  log(label, name, JSON.stringify({first_picture_ms: r.first_picture_ms, via: r.via, pathAtEnd: r.path_at_end, asked: asked, tapped, final: r.final_msg, spin: r.final_spin, btns: r.final_btns, idr: r.host_idr, pageIdr: r.host_page_idr, tunnel: r.tunnel_media, backToDirect: r.back_to_direct, diags: r.diags}));
  for (const x of timeline) log('   ', x.ms, 'ms', JSON.stringify(x.msg), x.spin ? '(spinner)' : '', x.btns.length ? '[' + x.btns.join(' | ') + ']' : '', x.tapPill ? '(tap pill)' : '', x.via ? 'picture via ' + x.via : '');
  return r;
}

const all = {owner: {block: true}, tap: {block: true, tapAfter: 3000}, normal: {}, stuck: {stuck: true}, nopic: {nopic: true}};
const results = [];
let code = 0;
try {
  for (const [n, o] of Object.entries(all)) if (!only || only.includes(n)) results.push(await scenario(n, o));
} catch (err) { console.error(err); code = 1; }
finally {
  for (const s of started) if (s.proc.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(s.proc.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} }
  await sleep(1500);
  const left = started.filter(s => s.name === 'edge').flatMap(s => ownEdgeLeft(s.profile)).length + started.filter(s => s.proc.exitCode === null).length;
  log('own processes left:', left);
  fs.writeFileSync(path.join(outDir, 'novideo.json'), JSON.stringify({label, seconds, results, processesLeft: left}, null, 2));
}
process.exit(code);
