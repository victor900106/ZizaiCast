// 0.8.3.7 遠端 before / after (node 22+), on this PC only (loopback, a SYNTHETIC desktop: --scene synth), never this
// PC's screen, nothing played (--no-audio), input dry-run, no tailscale (--no-tailscale; the spike never runs a CLI).
//   node remote/tools/remote0837_bench.mjs <before\pm_remote_spike.exe> <after\pm_remote_spike.exe> <out.json> [--shots DIR] [--only a,b] [--lang zh-TW]
// One chrome-headless-shell (quiet_browser.mjs: no window, no Alt+Tab entry, never the focus) as "the phone", an
// iPhone 17 Pro Max in landscape (956x440 CSS px, devicePixelRatio 3, touch) or a PC's browser (1440x900), closed at the end.
// Scenarios (each with the before spike, then the after spike: the page is the one each spike embeds):
//   tiers      120 fps asked, 10 s: frames the PC sent / the page received (getStats framesReceived) / decoded
//              (framesDecoded) / shown (requestVideoFrameCallback), measured by this script the same way for both
//              pages; the after page's own three-layer line; requestAnimationFrame rate of the headless "screen"
//   cap60      phone, 120 asked, the page told its screen draws 60 (?hz=60, as an iPhone Safari with the 60 fps flag):
//              what the PC sends (fps, kbps)
//   pc120      a PC's browser (no touch), 120 asked, ?hz=60: must NOT be capped
//   upload     files 0.46 MB + 1.76 MB (the owner's two PNGs' sizes) and 20 MB: time per file, (a) loopback as is,
//              (b) "Funnel model": the page's HTTP limited by CDP Network.emulateNetworkConditions (60 ms, 0.37 MB/s up:
//              the owner's log, 1.76 MB in ~4.7 s through Funnel) -- it slows HTTP only, never WebRTC, like the
//              real relay; the after page sends over the files DataChannel. Saved files are compared byte for byte.
//   during     frames shown a second and the shown-frame interval p95, 5 s before and during a 60 MB upload
// --shots DIR: screenshots (settings sheet, stats overlay, files panel) per spike: DIR/<before|after>_<scene>.png
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import {launch, cdpClient, watchAltTab} from './quiet_browser.mjs';

const [beforeExe, afterExe, outJson, ...rest] = process.argv.slice(2);
if (!beforeExe || !afterExe || !outJson) { console.error('usage: node remote0837_bench.mjs <before spike> <after spike> <out.json> [--shots DIR] [--only a,b]'); process.exit(2); }
let shots = '', only = null, lang = '0', which = ['before', 'after'];
for (let i = 0; i < rest.length; i++) {
  if (rest[i] === '--shots') shots = rest[++i];
  else if (rest[i] === '--only') only = new Set(rest[++i].split(','));
  else if (rest[i] === '--lang') lang = rest[++i];
  else if (rest[i] === '--which') which = rest[++i].split(',');
}
const sleep = ms => new Promise(r => setTimeout(r, ms));
const PHONE = {width: 956, height: 440, deviceScaleFactor: 3, mobile: true};
const PCWIN = {width: 1440, height: 900, deviceScaleFactor: 1, mobile: false};
const IPHONE_UA = 'Mozilla/5.0 (iPhone; CPU iPhone OS 26_1 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.1 Mobile/15E148 Safari/604.1';
const work = fs.mkdtempSync(path.join(os.tmpdir(), 'pm-bench0837-'));
if (shots) fs.mkdirSync(shots, {recursive: true});

// test files: PNG-looking (header + incompressible bytes), the owner's two sizes, 20 MB and 60 MB
const files = {};
for (const [name, size] of [['IMG_9575.png', 482345], ['IMG_9574.png', 1845494], ['big20.bin', 20 << 20], ['big60.bin', 60 << 20], ['big200.bin', 200 << 20]]) {
  const f = path.join(work, name), b = crypto.randomBytes(size);
  if (name.endsWith('.png')) Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]).copy(b, 0);
  fs.writeFileSync(f, b);
  files[name] = {path: f, size, sha: crypto.createHash('sha256').update(b).digest('hex')};
}

const br = await launch(path.join(work, '_shell'), ['--allow-loopback-in-peer-connection', '--autoplay-policy=no-user-gesture-required']);
const watch = watchAltTab([br.proc.pid, process.pid], path.join(work, 'alttab.txt'));
const C = await cdpClient(br.ver.webSocketDebuggerUrl);
const results = [];

async function withSpike(exe, tag, extra, fn) {
  const token = crypto.randomBytes(16).toString('hex');
  const port = 19700 + Math.floor(Math.random() * 250);
  const logFile = path.join(work, tag + '.log');
  const upDir = path.join(work, 'up-' + tag);
  fs.mkdirSync(upDir, {recursive: true});
  fs.writeFileSync(logFile, '');
  const readLog = () => fs.readFileSync(logFile, 'utf8');
  const spike = spawn(exe, ['--token', token, '--port', String(port), '--log', logFile, '--duration', '400', '--stdin-quit', '--loopback', '--no-tailscale',
    '--no-audio', '--stamp', '--fixed-fps', '--scene', 'synth', '--quality', '1', '--upload-dir', upDir, '--lang', lang, ...extra], {stdio: ['pipe', 'ignore', 'ignore'], windowsHide: true});
  watch.add(spike.pid);
  let url = '';
  const fixed = extra.includes('--fixed-password');
  for (let i = 0; i < 150 && !url; i++) {
    await sleep(100);
    const m = fixed ? readLog().match(/fixed: origin http:\/\/(127\.0\.0\.2:\d+)/) : readLog().match(/urls: lan (http:\/\/127\.0\.0\.3:\d+\/[0-9a-f]+\/)/);
    if (m) url = fixed ? 'http://' + m[1] + '/' : m[1];
  }
  try {
    if (!url) throw new Error('spike did not start');
    return await fn({url, readLog, upDir});
  } finally {
    try { spike.stdin.end(); } catch {}
    for (let i = 0; i < 50 && spike.exitCode === null; i++) await sleep(100);
    if (spike.exitCode === null) try { execFileSync('taskkill', ['/PID', String(spike.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
  }
}

async function openPage(url, {pcBrowser = false, ua = '', query = '', login = '', throttle = null} = {}) {
  if (!login) query = [query, 'fdc'].filter(Boolean).join('&');
  if (process.env.FDC_TRACE) query = [query, 'fdcTrace'].filter(Boolean).join('&');  // 0.8.3.7: the files channel as on the fixed / temporary address (the bench uses the LAN one)
  const {targetId} = await C.cdp('Target.createTarget', {url: 'about:blank'});
  const {sessionId} = await C.cdp('Target.attachToTarget', {targetId, flatten: true});
  const s = (m, p) => C.cdp(m, p, sessionId);
  const ev = async (expression, awaitPromise = false) => { const x = await s('Runtime.evaluate', {expression, returnByValue: true, awaitPromise}); return x && x.result ? x.result.value : (x && x.exceptionDetails ? {err: x.exceptionDetails.text} : undefined); };
  await s('Emulation.setDeviceMetricsOverride', pcBrowser ? PCWIN : PHONE);
  if (!pcBrowser) await s('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5});
  if (ua) await s('Emulation.setUserAgentOverride', {userAgent: ua, platform: 'iPhone'});
  await s('Network.enable', {});
  await s('Page.enable', {});
  await s('Page.navigate', {url: url + (query ? '?' + query : '')});
  if (login) {  // 固定網址: the owner's password (as typed on the phone), then the session
    for (let i = 0; i < 80 && !(await ev(`$('pass') && $('pass').style.display === 'flex' ? 1 : 0`)); i++) await sleep(100);
    await ev(`(() => { $('passIn').value = ${JSON.stringify(login)}; submitPass(); return 1; })()`);
  }
  for (let i = 0; i < 250 && !(await ev('window.__pmFirstFrame ? 1 : 0')); i++) await sleep(100);
  if (!(await ev('window.__pmFirstFrame ? 1 : 0'))) throw new Error('no picture');
  await ev('prefs.auto = false; 1');  // no automatic quality steps while measuring
  // the Funnel model from here on: CDP's throttling also slows WebRTC sockets opened after it is set (measured: a
  // throttled login made the files channel 0.3 MB/s too), so it is set once the picture's connection is up --
  // like the real relay, which carries HTTP only
  if (throttle) await s('Network.emulateNetworkConditions', throttle);
  const close = () => C.cdp('Target.closeTarget', {targetId});
  const shot = async name => {
    if (!shots) return;
    const r = await s('Page.captureScreenshot', {format: 'png'});
    if (r && r.data) fs.writeFileSync(path.join(shots, name + '.png'), Buffer.from(r.data, 'base64'));
  };
  return {s, ev, close, shot};
}

// frames over `secs`: received / decoded (getStats), shown (rVFC; interval p50 / p95), rAF rate, the PC's fps / kbps
const MEASURE = secs => `(async () => {
  const inb = async () => { let x = null; (await pc.getStats()).forEach(s => { if (s.type === 'inbound-rtp' && s.kind === 'video') x = s; }); return x; };
  const a = await inb(), t0 = performance.now(); const iv = []; let last = 0, n = 0, raf = 0, pf0 = -1, pf1 = 0; const srv = [];
  await new Promise(res => {
    const cb = (now, md) => { if (last) iv.push(now - last); last = now; n++; if (pf0 < 0) pf0 = md.presentedFrames; pf1 = md.presentedFrames; if (performance.now() - t0 < ${secs * 1000}) v.requestVideoFrameCallback(cb); else res(); };
    v.requestVideoFrameCallback(cb);
    const rf = () => { raf++; if (performance.now() - t0 < ${secs * 1000}) requestAnimationFrame(rf); }; requestAnimationFrame(rf);
    const iv2 = setInterval(() => { if (S.srv) srv.push({fps: S.srv.fps, kbps: S.srv.kbps, cap: S.srv.fpsCap, want: S.srv.fpsWant}); if (performance.now() - t0 > ${secs * 1000}) clearInterval(iv2); }, 1000);
  });
  const b = await inb(), dt = (performance.now() - t0) / 1000;
  const q = p => { const s = iv.slice().sort((x, y) => x - y); return s.length ? +s[Math.min(s.length - 1, Math.floor(s.length * p))].toFixed(1) : null; };
  const avg = k => srv.length ? +(srv.reduce((x, y) => x + (y[k] || 0), 0) / srv.length).toFixed(1) : null;
  return {secs: +dt.toFixed(2), sent: avg('fps'), kbps: avg('kbps'), fpsCap: srv.length ? srv[srv.length - 1].cap : null, fpsWant: srv.length ? srv[srv.length - 1].want : null,
    recv: a && b ? +((b.framesReceived - a.framesReceived) / dt).toFixed(1) : null, dec: a && b ? +((b.framesDecoded - a.framesDecoded) / dt).toFixed(1) : null,
    shown: +(n / dt).toFixed(1), presented: +((pf1 - pf0) / dt).toFixed(1), ivP50: q(.5), ivP95: q(.95), raf: +(raf / dt).toFixed(1),
    page: window.__pmTiers || null, hud: hud.textContent.split('\\n').slice(0, 3).join(' | ')};
})()`;

async function uploadOne(P, name, timeoutMs = 300000) {
  const before = await P.ev('JSON.stringify({files: window.__pmUp.files, ms: window.__pmUp.ms, errors: window.__pmUp.errors})').then(JSON.parse);
  await P.ev(`$('bFile').onclick && ($('fileS').style.display === 'block' || $('bFile').onclick()); 1`);
  const doc = await P.s('DOM.getDocument', {depth: 1});
  const q = await P.s('DOM.querySelector', {nodeId: doc.root.nodeId, selector: '#upIn'});
  await P.s('DOM.setFileInputFiles', {nodeId: q.nodeId, files: [files[name].path]});
  const t0 = Date.now();
  await P.ev(`$('upIn').dispatchEvent(new Event('change')); 1`);
  let now = before;
  while (Date.now() - t0 < timeoutMs) {
    await sleep(100);
    now = await P.ev('JSON.stringify({files: window.__pmUp.files, ms: window.__pmUp.ms, errors: window.__pmUp.errors, list: window.__pmUp.list || null})').then(JSON.parse);
    if (now.files > before.files || now.errors > before.errors) break;
  }
  const last = now.list && now.list.length ? now.list[now.list.length - 1] : null;
  return {name, size: files[name].size, ok: now.files > before.files, ms: Math.round(now.ms - before.ms), via: last ? last.via : 'http', wallMs: Date.now() - t0,
    text: await P.ev(`$('upList').firstChild ? $('upList').firstChild.textContent : ''`), trace: process.env.FDC_TRACE ? await P.ev('JSON.stringify(window.__fdcTrace || null)') : undefined};
}
function checkSaved(upDir, name) {
  const want = files[name];
  const got = fs.readdirSync(upDir).filter(f => !f.endsWith('.part')).map(f => path.join(upDir, f)).filter(f => fs.statSync(f).size === want.size);
  return got.some(f => crypto.createHash('sha256').update(fs.readFileSync(f)).digest('hex') === want.sha);
}

const funnelModel = {offline: false, latency: 60, uploadThroughput: 0.37 * 1048576, downloadThroughput: 2 * 1048576};
const SCEN = {
  async tiers(exe, tag) {
    return withSpike(exe, tag, [], async ({url}) => {
      const P = await openPage(url, {query: 'nohzcap'});
      try {
        await P.ev('prefs.hud = true; $("swHud").checked = true; $("swHud").onchange(); setFps(120); 1');
        await sleep(5000);
        const m = await P.ev(MEASURE(10), true);
        await P.shot(tag + '_hud_tiers');
        return m;
      } finally { await P.close(); }
    });
  },
  async cap60(exe, tag) {
    return withSpike(exe, tag, [], async ({url, readLog}) => {
      const P = await openPage(url, {query: 'hz=60', ua: IPHONE_UA});
      try {
        await P.ev('setFps(120); 1');
        await sleep(5000);
        const m = await P.ev(MEASURE(10), true);
        m.sub = await P.ev(`$('lFpsSub').textContent`);
        m.askLine = (readLog().match(/^.*picture -> .*$/gm) || []).slice(-1)[0] || '';
        await P.ev(`$('bSet').onclick(); 1`); await sleep(400);
        await P.ev(`(() => { const r = $('segFps').closest('.row'); r.scrollIntoView({block: 'center'}); })(); 1`); await sleep(300);
        await P.shot(tag + '_settings_fps120_screen60');
        return m;
      } finally { await P.close(); }
    });
  },
  async pc120(exe, tag) {
    return withSpike(exe, tag, [], async ({url}) => {
      const P = await openPage(url, {pcBrowser: true, query: 'hz=60'});
      try { await P.ev('setFps(120); 1'); await sleep(5000); return await P.ev(MEASURE(8), true); } finally { await P.close(); }
    });
  },
  async quality(exe, tag) {  // screenshots: 畫質's line for each choice
    return withSpike(exe, tag, [], async ({url}) => {
      const P = await openPage(url);
      try {
        await P.ev(`$('bSet').onclick(); 1`); await sleep(300);
        const out = {};
        for (const q of [0, 1, 2]) {
          await P.ev(`setQuality(${q}); 1`); await sleep(1500);
          out['q' + q] = await P.ev(`$('lQSub').textContent`);
          await P.ev(`$('segQ').closest('.row').scrollIntoView({block: 'center'}); 1`);
          await P.shot(`${tag}_settings_q${q}`);
        }
        return out;
      } finally { await P.close(); }
    });
  },
  async upload(exe, tag) {
    return withSpike(exe, tag, [], async ({url, upDir}) => {
      const out = {};
      for (const mode of ['loopback', 'funnelModel']) {
        const P = await openPage(url);
        try {
          if (mode === 'funnelModel') await P.s('Network.emulateNetworkConditions', funnelModel);
          const r = [];
          for (const name of ['IMG_9575.png', 'IMG_9574.png', 'big20.bin']) { const x = await uploadOne(P, name); x.saved = checkSaved(upDir, name); r.push(x); }
          out[mode] = r;
          if (mode === 'funnelModel') await P.shot(tag + '_files_panel');
        } finally { await P.close(); }
      }
      return out;
    });
  },
  // the owner's case end to end: 固定網址 (page and login over HTTP through the "relay" = the Funnel model, the picture
  // straight over WebRTC), the two photos' sizes and 20 MB. Before: HTTP 1 MB pieces; after: the files DataChannel by itself.
  async fixed(exe, tag) {
    const PW = 'pw-' + crypto.randomBytes(4).toString('hex');
    return withSpike(exe, tag, ['--fixed-password', PW], async ({url, upDir, readLog}) => {
      const P = await openPage(url, {login: PW, throttle: process.env.FIXED_NOTHROTTLE ? null : funnelModel});
      try {
        const r = [];
        for (const name of ['IMG_9575.png', 'IMG_9574.png', 'big20.bin']) { const x = await uploadOne(P, name); x.saved = checkSaved(upDir, name); r.push(x); }
        await P.shot(tag + '_files_panel_fixed');
        return {files: r, log: (readLog().match(/^.*files: .*(saved|sends).*$/gm) || []).map(l => l.replace(/^.*?\[remote\] /, ''))};
      } finally { await P.close(); }
    });
  },
  // a screenshot of the files panel half way through 1.76 MB (shots only): before = HTTP through the Funnel model,
  // after = the files channel held to 1 MB/s (?fdcRate=1, only so the bar can be caught on loopback)
  async progress(exe, tag) {
    const before = /before/.test(tag);
    return withSpike(exe, tag, [], async ({url, upDir}) => {
      const P = await openPage(url, {query: before ? '' : 'fdcRate=1'});
      try {
        if (before) await P.s('Network.emulateNetworkConditions', funnelModel);
        const upP = uploadOne(P, 'IMG_9574.png');
        await sleep(before ? 2500 : 1000);
        const mid = await P.ev(`(() => { const p = document.querySelector('#upList progress'); return p ? {value: p.value, max: p.max, st: p.parentNode.textContent} : null; })()`);
        await P.shot(tag + '_files_progress');
        const x = await upP; x.saved = checkSaved(upDir, 'IMG_9574.png'); x.mid = mid;
        return x;
      } finally { await P.close(); }
    });
  },
  async fallback(exe, tag) {  // after only: ?nofdc = the old HTTP path still works
    return withSpike(exe, tag, [], async ({url, upDir}) => {
      const P = await openPage(url, {query: 'nofdc'});
      try { const x = await uploadOne(P, 'IMG_9574.png'); x.saved = checkSaved(upDir, 'IMG_9574.png'); return x; } finally { await P.close(); }
    });
  },
  async dctune(exe, tag) {  // after only: the files channel's sizes (KB) on loopback, 60 MB
    return withSpike(exe, tag, [], async ({url, upDir}) => {
      const out = [];
      for (const [hi, lo, pc] of [[1024, 256, 64], [4096, 1024, 64], [4096, 1024, 256], [16384, 4096, 256]]) {
        const P = await openPage(url, {query: `fdcHigh=${hi}&fdcLow=${lo}&fdcPiece=${pc}`});
        try { const x = await uploadOne(P, 'big60.bin'); x.saved = checkSaved(upDir, 'big60.bin'); out.push({hi, lo, pc, ms: x.ms, mbs: +(x.size / 1048576 / (x.ms / 1000)).toFixed(1), via: x.via, saved: x.saved}); }
        finally { await P.close(); }
      }
      return out;
    });
  },
  // the picture while a file goes: before = HTTP through the Funnel model (as on the fixed link), after = the files
  // DataChannel (200 MB on loopback lasts long enough); 30 and 60 frames a second (the headless screen draws 60)
  // the files channel at a few speed limits (MB/s, 0 = none), the picture while 200 MB go, twice each (after only)
  async rate(exe, tag) {
    return withSpike(exe, tag, [], async ({url, upDir}) => {
      const out = [];
      for (const rate of [0, 8, 0, 8]) {
        const P = await openPage(url, {query: `nohzcap&fdcRate=${rate}`});
        try {
          await P.ev('setFps(60); 1'); await sleep(3000);
          const idle = await P.ev(MEASURE(4), true);
          const upP = uploadOne(P, 'big200.bin'); await sleep(300);
          const busy = await P.ev(MEASURE(4), true);
          const up = await upP;
          out.push({rate, mbs: +(up.size / 1048576 / (up.ms / 1000)).toFixed(1), via: up.via, idleShown: idle.shown, idlePres: idle.presented, idleP95: idle.ivP95, busyShown: busy.shown, busyPres: busy.presented, busyP95: busy.ivP95, busyRecv: busy.recv, busyDec: busy.dec});
        } finally { await P.close(); }
      }
      return out;
    });
  },
  // the same with HTTP pieces (?nofdc), the page's HTTP limited to 8 MB/s by CDP (comparison for 'rate'; after only)
  async httprate(exe, tag) {
    return withSpike(exe, tag, [], async ({url}) => {
      const out = [];
      for (const i of [0, 1]) {
        const P = await openPage(url, {query: 'nohzcap&nofdc'});
        try {
          await P.ev('setFps(60); 1'); await sleep(3000);
          await P.s('Network.emulateNetworkConditions', {offline: false, latency: 2, uploadThroughput: 8 * 1048576, downloadThroughput: 50 * 1048576});
          const idle = await P.ev(MEASURE(4), true);
          const upP = uploadOne(P, 'big60.bin'); await sleep(300);
          const busy = await P.ev(MEASURE(4), true);
          const up = await upP;
          out.push({rate: 'http8', mbs: +(up.size / 1048576 / (up.ms / 1000)).toFixed(1), via: up.via, idleShown: idle.shown, idlePres: idle.presented, idleP95: idle.ivP95, busyShown: busy.shown, busyPres: busy.presented, busyP95: busy.ivP95, busyRecv: busy.recv, busyDec: busy.dec});
        } finally { await P.close(); }
      }
      return out;
    });
  },
  async during(exe, tag) {
    const before = /before/.test(tag);
    return withSpike(exe, tag, [], async ({url, upDir}) => {
      const out = {};
      for (const fps of [30, 60]) {
        const P = await openPage(url, {query: 'nohzcap'});
        try {
          await P.ev(`setFps(${fps}); 1`);
          if (before) await P.s('Network.emulateNetworkConditions', funnelModel);
          await sleep(4000);
          const idle = await P.ev(MEASURE(6), true);
          const upP = uploadOne(P, before ? 'big20.bin' : 'big200.bin');
          await sleep(300);
          const busy = await P.ev(MEASURE(5), true);
          const up = await upP;
          up.saved = checkSaved(upDir, before ? 'big20.bin' : 'big200.bin');
          out['fps' + fps] = {idle, busy, up};
        } finally { await P.close(); }
      }
      return out;
    });
  },
};

for (const name of Object.keys(SCEN)) {
  if (only && !only.has(name)) continue;
  for (const w of which) {
    if ((name === 'fallback' || name === 'dctune' || name === 'rate' || name === 'httprate') && w === 'before') continue;
    const exe = w === 'before' ? beforeExe : afterExe;
    const r = {scenario: name, which: w};
    try { r.result = await SCEN[name](exe, w + '_' + name); } catch (e) { r.error = String(e && e.message || e); }
    console.log(JSON.stringify(r));
    results.push(r);
  }
}
C.close();
await br.close();
const alt = await watch.stop();
fs.writeFileSync(outJson, JSON.stringify({when: new Date().toISOString(), before: beforeExe, after: afterExe, phone: PHONE, funnelModel, results, altTabMax: alt.max, altTabSamples: alt.samples}, null, 1));
if (process.env.KEEP_BENCH) console.log('kept', work); else try { fs.rmSync(work, {recursive: true, force: true}); } catch {}
console.log(`Alt+Tab / taskbar windows of the test's processes: max ${alt.max} over ${alt.samples} samples`);
process.exit(alt.max === 0 && results.every(r => !r.error) ? 0 : 1);
