// 0.8.3.6 影像 before / after benchmark (node 22+): frame rate, encode time, CPU, bandwidth and latency of
// the picture path on a SYNTHETIC desktop (--scene synth: made-up windows / text / photo, panned every frame;
// never this PC's screen), offline: everything on this PC, loopback only.
//   node remote/tools/video0836_bench.mjs <pm_remote_spike.exe> <out.json> [--set before|after] [--only a,b] [--secs N]
// One chrome-headless-shell (quiet_browser.mjs: no window, no Alt+Tab entry, never the focus) as "the phone"
// (an iPhone 15 Pro in landscape: 852x393 CSS px, devicePixelRatio 3), reused for every scenario and closed
// at the end. The spike runs --loopback (127.0.0.3), --no-tailscale, --no-audio (nothing is played), dry-run
// input (nothing is injected), --stamp (capture -> shown latency), --fixed-fps (no touch needed for the
// full rate). Per scenario: warm-up, then <secs> seconds of the PC's 5-second stats lines (fps sent, kbps,
// capture -> encoded, encode, CPU of the spike process) and the page's own numbers (frames shown per
// second, decode time, capture -> shown latency).
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import {launch, cdpClient, watchAltTab} from './quiet_browser.mjs';

const [spikeExe, outJson, ...rest] = process.argv.slice(2);
if (!spikeExe || !outJson) { console.error('usage: node video0836_bench.mjs <spike.exe> <out.json> [--set before|after] [--only a,b] [--secs N]'); process.exit(2); }
let set = 'after', only = null, secs = 20, warm = 6;
for (let i = 0; i < rest.length; i++) {
  if (rest[i] === '--set') set = rest[++i];
  else if (rest[i] === '--only') only = new Set(rest[++i].split(','));
  else if (rest[i] === '--secs') secs = +rest[++i];
  else if (rest[i] === '--warm') warm = +rest[++i];
}
const sleep = ms => new Promise(r => setTimeout(r, ms));
const PHONE = {width: 852, height: 393, deviceScaleFactor: 3, mobile: true};

// What each scenario asks for. `page` runs in the page after the first picture (0.8.3.6 settings only
// exist in the after page); `spike` = extra spike arguments.
const SCEN = {
  before: [
    {name: 'q2-60', spike: ['--quality', '2']},
    {name: 'q1-60', spike: ['--quality', '1']},
    {name: 'q0-60', spike: ['--quality', '0']},
    {name: 'q2-60-static', spike: ['--quality', '2'], scene: 'static'},
    {name: 'switch-q2-q0', spike: ['--quality', '2'], sw: 'setQuality(0)'},
  ],
  after: [
    {name: 'q2-60', spike: ['--quality', '2']},
    {name: 'q1-60', spike: ['--quality', '1']},
    {name: 'q0-60', spike: ['--quality', '0']},
    {name: 'q2-60-static', spike: ['--quality', '2'], scene: 'static'},
    {name: 'switch-q2-q0', spike: ['--quality', '2'], sw: 'setQuality(0)'},
    {name: 'q2-60-phone', spike: ['--quality', '2'], page: 'setPhoneRes(true)'},
    {name: 'q1-120', spike: ['--quality', '1'], page: 'setFps(120)'},
    {name: 'q2-120', spike: ['--quality', '2'], page: 'setFps(120)'},
    {name: 'q2-120-phone', spike: ['--quality', '2'], page: 'setPhoneRes(true); setFps(120)'},
    {name: 'q2-120-static', spike: ['--quality', '2'], scene: 'static', page: 'setFps(120)'},
    {name: 'mon1-120', spike: ['--quality', '2'], page: 'prefs.mon = 1; setFps(120)'},  // the second screen (59 Hz here)
    {name: 'mon1-120-waitahead', spike: ['--quality', '2', '--wait-ahead'], page: 'prefs.mon = 1; setFps(120)'},
    {name: 'switch-fps-60-120', spike: ['--quality', '1'], sw: 'setFps(120)'},
    {name: 'switch-kbps', spike: ['--quality', '2'], sw: 'setQuality(1)'},
    {name: 'q2-60-oldpace', spike: ['--quality', '2', '--no-even-pacing']},
    {name: 'q2-60-sleepwait', spike: ['--quality', '2', '--sleep-wait']},
    {name: 'q2-120-sleepwait', spike: ['--quality', '2', '--sleep-wait'], page: 'setFps(120)'},
  ],
}[set].filter(s => !only || only.has(s.name));

const work = fs.mkdtempSync(path.join(os.tmpdir(), 'pm-bench0836-'));
const br = await launch(path.join(work, '_shell'), ['--allow-loopback-in-peer-connection', '--autoplay-policy=no-user-gesture-required']);
const watch = watchAltTab([br.proc.pid, process.pid], path.join(work, 'alttab.txt'));
const C = await cdpClient(br.ver.webSocketDebuggerUrl);
const results = [];
const avg = a => a.length ? a.reduce((x, y) => x + y, 0) / a.length : NaN;
const med = a => { const b = a.filter(Number.isFinite).sort((x, y) => x - y); return b.length ? b[b.length >> 1] : NaN; };

function statsLines(log) {
  const re = /stats: (\d+)x(\d+) ([\d.]+) fps sent, (\d+) kbps, max frame ([\d.]+) KB, \d+ IDR \| capture->encoded avg ([\d.]+) p95 ([\d.]+) ms \| capture->sent avg ([\d.]+) p95 ([\d.]+) ms \| render ([\d.]+) ms \| encode ([\d.]+) ms \| CPU ([\d.]+)% of machine \(([\d.]+)% of one core\)/g;
  const out = [];
  for (const m of log.matchAll(re)) out.push({w: +m[1], h: +m[2], fps: +m[3], kbps: +m[4], maxKB: +m[5], c2e: +m[6], c2eP95: +m[7], c2s: +m[8], c2sP95: +m[9], render: +m[10], enc: +m[11], cpuMachine: +m[12], cpuCore: +m[13]});
  return out;
}

for (const sc of SCEN) {
  const token = crypto.randomBytes(16).toString('hex');
  const port = 19700 + Math.floor(Math.random() * 250);
  const logFile = path.join(work, sc.name + '.log');
  fs.writeFileSync(logFile, '');
  const readLog = () => fs.readFileSync(logFile, 'utf8');
  const spike = spawn(spikeExe, ['--token', token, '--port', String(port), '--log', logFile, '--duration', String(warm + secs + 60), '--stdin-quit',
    '--loopback', '--no-tailscale', '--no-audio', '--stamp', '--fixed-fps', '--scene', sc.scene || 'synth', ...sc.spike], {stdio: ['pipe', 'ignore', 'ignore'], windowsHide: true});
  watch.add(spike.pid);
  let url = '';
  for (let i = 0; i < 150 && !url; i++) { await sleep(100); const m = readLog().match(/urls: lan (http:\/\/127\.0\.0\.3:\d+\/[0-9a-f]+\/)/); if (m) url = m[1]; }
  const r = {name: sc.name, set};
  try {
    if (!url) throw new Error('spike did not start');
    const {targetId} = await C.cdp('Target.createTarget', {url: 'about:blank'});
    const {sessionId} = await C.cdp('Target.attachToTarget', {targetId, flatten: true});
    const s = (m, p) => C.cdp(m, p, sessionId);
    const ev = async (expression, awaitPromise = false) => { const x = await s('Runtime.evaluate', {expression, returnByValue: true, awaitPromise}); return x && x.result ? x.result.value : undefined; };
    await s('Emulation.setDeviceMetricsOverride', PHONE);
    await s('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5});
    await s('Page.navigate', {url});
    for (let i = 0; i < 250 && !(await ev('window.__pmFirstFrame ? 1 : 0')); i++) await sleep(100);
    if (!(await ev('window.__pmFirstFrame ? 1 : 0'))) throw new Error('no picture');
    await ev(`prefs.hud = true; prefs.auto = false; 1`);  // the stamp is read with the HUD on; no automatic quality steps while measuring
    // rel-0836: 解析度跟手機一樣 is on by default on a touch screen now (this emulated iPhone too): the
    // scenarios without "phone" in their name ask for it off, as they were measured
    if (set === 'after' && !/setPhoneRes/.test(sc.page || '')) sc.page = 'setPhoneRes(false)' + (sc.page ? '; ' + sc.page : '');
    if (sc.page) r.pageSetup = await ev(`(() => { try { ${sc.page}; return 'ok'; } catch (e) { return String(e); } })()`);
    if (sc.sw) {
      // a switch while connected: how long until frames of the new stream are shown again, and was it a new session
      await sleep(4000);
      const sessBefore = await ev('sessId');
      const gapMs = await ev(`new Promise(res => {
        const t0 = performance.now(), w0 = v.videoWidth, n0 = S.srv ? S.srv.fpsNow : 0, sid = sessId;
        let last = t0, maxGap = 0, frames = 0;
        const cb = (now) => { maxGap = Math.max(maxGap, now - last); last = now; frames++; if (performance.now() - t0 < 6000) v.requestVideoFrameCallback(cb); };
        v.requestVideoFrameCallback(cb);
        ${sc.sw};
        setTimeout(() => res({maxGapMs: Math.round(maxGap), frames, w0, w1: v.videoWidth, newSession: sessId !== sid, state: S.state}), 6500);
      })`, true);
      r.switch = gapMs;
      r.switch.sessBefore = sessBefore;
      r.switch.sessAfter = await ev('sessId');
      r.switch.log = (readLog().match(/^.*(same connection|switching to monitor|picture ->|frame rate \d).*$/gm) || []).filter(l => !/page-report/.test(l)).slice(-6);
    } else {
      await sleep(warm * 1000);
      const mark = readLog().length;
      const page = [];
      const inb = async () => ev(`(async () => { let x = null; (await pc.getStats()).forEach(s => { if (s.type === 'inbound-rtp' && s.kind === 'video') x = s; });
        return x ? {rx: x.framesReceived, dec: x.framesDecoded, drop: x.framesDropped || 0, t: performance.now()} : null; })()`, true);
      const i0 = await inb();
      for (let t = 0; t < secs; t++) {
        await sleep(1000);
        page.push(await ev(`({fps: S.fps, dec: S.dec, lat: S.latShow, latP95: S.latP95, e2e: S.e2e, w: S.w, h: S.h, kbps: S.kbps, srvFps: S.srv && S.srv.fps, hz: S.srv && S.srv.hz, cap: S.srv && S.srv.fpsCap})`));
      }
      // how the shown frames are spaced (5 s): intervals in ms, and frames the compositor never showed
      // capIv: the PC's capture times of the frames shown (the stamp) -- even spacing at the source?
      r.pacing = await ev(`new Promise(res => { const iv = [], civ = []; let last = 0, lastCap = NaN, pf0 = -1, pf = 0, n = 0; const t0 = performance.now();
        const q = (a, p) => a.length ? +a.slice().sort((x, y) => x - y)[Math.min(a.length - 1, Math.floor(a.length * p))].toFixed(1) : null;
        const cb = (now, m) => { if (last) iv.push(now - last); last = now; if (pf0 < 0) pf0 = m.presentedFrames; pf = m.presentedFrames; n++;
          const c = readStamp(); if (isFinite(c) && isFinite(lastCap) && c > lastCap) civ.push(c - lastCap); if (isFinite(c)) lastCap = c;
          if (performance.now() - t0 < 5000) v.requestVideoFrameCallback(cb);
          else res({n, presented: pf - pf0, p10: q(iv, .1), p50: q(iv, .5), p90: q(iv, .9), over25: iv.filter(x => x > 25).length,
                    capP10: q(civ, .1), capP50: q(civ, .5), capP90: q(civ, .9)}); };
        v.requestVideoFrameCallback(cb); })`, true);
      const i1 = await inb();
      const dt = i0 && i1 ? (i1.t - i0.t) / 1000 : NaN;
      r.recv = i0 && i1 ? {rxFps: +((i1.rx - i0.rx) / dt).toFixed(1), decFps: +((i1.dec - i0.dec) / dt).toFixed(1), dropped: i1.drop - i0.drop} : null;
      const st = statsLines(readLog().slice(mark));
      // 0.8.3.6 host lines also say how evenly frames left: "pace E early / L late of N"
      const pm = [...readLog().slice(mark).matchAll(/pace (\d+) early \/ (\d+) late of (\d+)/g)];
      if (pm.length) { const e = pm.reduce((a, m) => a + +m[1], 0), l = pm.reduce((a, m) => a + +m[2], 0), n = pm.reduce((a, m) => a + +m[3], 0);
        r.pace = {early: e, late: l, n, offBeatPct: +((e + l) * 100 / Math.max(1, n)).toFixed(1)}; }
      r.host = st.length ? Object.fromEntries(Object.keys(st[0]).map(k => [k, +avg(st.map(x => x[k])).toFixed(2)])) : null;
      r.hostLines = st.length;
      r.page = {fpsShown: +med(page.map(p => p.fps)).toFixed(1), decMs: +med(page.map(p => p.dec)).toFixed(2), latMs: med(page.map(p => p.lat)),
        latP95: med(page.map(p => p.latP95)), e2eMs: Math.round(med(page.map(p => p.e2e))), kbps: Math.round(med(page.map(p => p.kbps))),
        size: page.length ? page[page.length - 1].w + 'x' + page[page.length - 1].h : '', hz: page.length ? page[page.length - 1].hz : null, fpsCap: page.length ? page[page.length - 1].cap : null};
    }
    await C.cdp('Target.closeTarget', {targetId});
  } catch (e) { r.error = String(e && e.message || e); }
  try { spike.stdin.end(); } catch {}
  for (let i = 0; i < 50 && spike.exitCode === null; i++) await sleep(100);
  if (spike.exitCode === null) try { execFileSync('taskkill', ['/PID', String(spike.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
  r.encoder = (readLog().match(/encoder: (.*?) ready/) || [])[1] || '';
  console.log(JSON.stringify(r));
  results.push(r);
}
C.close();
await br.close();
const alt = await watch.stop();
fs.writeFileSync(outJson, JSON.stringify({set, spike: spikeExe, when: new Date().toISOString(), phone: PHONE, results, altTabMax: alt.max, altTabSamples: alt.samples}, null, 1));
if (process.env.KEEP_BENCH) console.log('logs kept in', work); else try { fs.rmSync(work, {recursive: true, force: true}); } catch {}
console.log(`Alt+Tab / taskbar windows of the test's processes: max ${alt.max} over ${alt.samples} samples`);
process.exit(alt.max === 0 && results.every(r => !r.error) ? 0 : 1);
