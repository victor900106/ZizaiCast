// 0.8.4 自動畫質 before / after (node 22+): first picture, the quality it settles on, and how it behaves on a
// limited network, on a SYNTHETIC desktop (--scene synth: made-up windows / text / photo, panned every frame;
// never this PC's screen), offline: everything on this PC, loopback only.
//   node remote/tools/q084_bench.mjs <pm_remote_spike.exe> <out.json> [--set before|after] [--only a,b] [--runs N]
// The network is limited inside the browser (CDP Network.emulateNetworkConditions: its WebRTC transport goes
// through the same emulation: throughput, latency, packet loss). Nothing on the PC's network is changed.
// One chrome-headless-shell (quiet_browser.mjs: no window, no Alt+Tab entry, never the focus) as "the phone"
// (852x393 CSS px, devicePixelRatio 3, touch), reused for every run and closed at the end. The spike runs
// --loopback (127.0.0.3), --no-tailscale, --no-audio (nothing is played), --inject-stub (nothing is injected).
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import {launch, cdpClient, watchAltTab} from './quiet_browser.mjs';

const [spikeExe, outJson, ...rest] = process.argv.slice(2);
if (!spikeExe || !outJson) { console.error('usage: node q084_bench.mjs <spike.exe> <out.json> [--set before|after] [--only a,b] [--runs N]'); process.exit(2); }
let set = 'after', only = null, runs = 1;
for (let i = 0; i < rest.length; i++) {
  if (rest[i] === '--set') set = rest[++i];
  else if (rest[i] === '--only') only = new Set(rest[++i].split(','));
  else if (rest[i] === '--runs') runs = +rest[++i];
}
const sleep = ms => new Promise(r => setTimeout(r, ms));
const PHONE = {width: 852, height: 393, deviceScaleFactor: 3, mobile: true};
const MBPS = m => Math.round(m * 1e6 / 8);  // CDP throughput: bytes per second
// net: [from s, conditions] steps (null = no limit). before: the page as it was (the default quality of
// the fixed / temporary address = 流暢 with automatic steps down only: spike --quality 0; on the LAN: 清晰).
const NETS = {
  lan: [[0, null]],
  mbps8: [[0, {latency: 20, downloadThroughput: MBPS(8), uploadThroughput: MBPS(8), packetLoss: 0, packetQueueLength: 64}]],
  mbps3: [[0, {latency: 40, downloadThroughput: MBPS(3), uploadThroughput: MBPS(3), packetLoss: 0, packetQueueLength: 64}]],
  loss5: [[0, {latency: 30, downloadThroughput: 0, uploadThroughput: 0, packetLoss: 5}]],
  // good, then a 2.5 Mbps squeeze from 20 s to 45 s, then good again (how fast down, how it comes back)
  squeeze: [[0, null], [20, {latency: 40, downloadThroughput: MBPS(2.5), uploadThroughput: MBPS(2.5), packetLoss: 0, packetQueueLength: 64}], [45, null]],
};
// what the link carries (kbps; settle = the first moment the PC's bitrate is under it)
const CAP = {lan: Infinity, mbps8: 8000, mbps3: 3000, loss5: Infinity};
const SCEN = [
  {name: 'lan', net: 'lan', secs: 30},
  {name: 'mbps8', net: 'mbps8', secs: 40},
  {name: 'mbps3', net: 'mbps3', secs: 40},
  {name: 'loss5', net: 'loss5', secs: 40},
  // (a squeeze in the middle of a run is not possible here: Chrome applies the conditions to a WebRTC
  // connection when it is made, a change later did nothing (measured); pm_autoq_test models it)
].filter(s => !only || only.has(s.name));
// before: which preset the old page starts with (its default for that kind of address)
const VARIANTS = set === 'before'
  ? [{v: 'outside-default', spike: ['--quality', '0']}, {v: 'lan-default', spike: ['--quality', '2']}]
  : [{v: 'auto-default', spike: ['--quality', '2']}];

const work = fs.mkdtempSync(path.join(os.tmpdir(), 'pm-q084-'));
const br = await launch(path.join(work, '_shell'), ['--allow-loopback-in-peer-connection', '--autoplay-policy=no-user-gesture-required']);
const watch = watchAltTab([br.proc.pid, process.pid], path.join(work, 'alttab.txt'));
const C = await cdpClient(br.ver.webSocketDebuggerUrl);
const results = [];
const med = a => { const b = a.filter(Number.isFinite).sort((x, y) => x - y); return b.length ? b[b.length >> 1] : NaN; };

for (const sc of SCEN) for (const va of VARIANTS) for (let run = 0; run < runs; run++) {
  const token = crypto.randomBytes(16).toString('hex');
  const port = 19960 + Math.floor(Math.random() * 30);
  const logFile = path.join(work, `${sc.name}-${va.v}-${run}.log`);
  fs.writeFileSync(logFile, '');
  const readLog = () => fs.readFileSync(logFile, 'utf8');
  const spike = spawn(spikeExe, ['--token', token, '--port', String(port), '--log', logFile, '--duration', String(sc.secs + 90), '--stdin-quit',
    '--loopback', '--no-tailscale', '--no-audio', '--stamp', '--fixed-fps', '--inject-stub', '--scene', 'synth', ...va.spike],
    {stdio: ['pipe', 'ignore', 'ignore'], windowsHide: true});
  watch.add(spike.pid);
  let url = '';
  for (let i = 0; i < 150 && !url; i++) { await sleep(100); const m = readLog().match(/urls: lan (http:\/\/127\.0\.0\.3:\d+\/[0-9a-f]+\/)/); if (m) url = m[1]; }
  const r = {name: sc.name, variant: va.v, run, set};
  let targetId;
  try {
    if (!url) throw new Error('spike did not start');
    ({targetId} = await C.cdp('Target.createTarget', {url: 'about:blank'}));
    const {sessionId} = await C.cdp('Target.attachToTarget', {targetId, flatten: true});
    const s = (m, p) => C.cdp(m, p, sessionId);
    const ev = async (expression, awaitPromise = false) => { const x = await s('Runtime.evaluate', {expression, returnByValue: true, awaitPromise}); return x && x.result ? x.result.value : undefined; };
    await s('Emulation.setDeviceMetricsOverride', PHONE);
    await s('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5});
    await s('Network.enable', {});
    const steps = NETS[sc.net];
    const applyNet = async c => s('Network.emulateNetworkConditions', c ? {offline: false, ...c} : {offline: false, latency: 0, downloadThroughput: -1, uploadThroughput: -1, packetLoss: 0});
    await applyNet(steps[0][1]);
    // a fresh page every run: no saved choice (the default), the stamp read with the HUD (prefs.hud)
    await s('Page.enable', {}); await s('Page.addScriptToEvaluateOnNewDocument', {source: `try { for (const k of Object.keys(localStorage)) if (k.startsWith('pm.') && k !== 'pm.device') localStorage.removeItem(k); localStorage.setItem('pm.hud', 'true'); localStorage.setItem('pm.modeHint0835', '1'); } catch {}`});
    const t0 = Date.now();
    await s('Page.navigate', {url});
    for (let i = 0; i < 300 && !(await ev('window.__pmFirstFrame ? 1 : 0')); i++) await sleep(50);
    const ff = await ev('window.__pmFirstFrame || 0');
    if (!ff) throw new Error('no picture');
    r.ttffMs = Math.round(ff);
    r.phases = await ev('S.ph');
    const fl = readLog().match(/first frame sent (\d+) ms after the offer request \((IDR|P), ([\d.]+) KB\)/);
    r.firstFrame = fl ? {sentMs: +fl[1], kind: fl[2], kb: +fl[3]} : null;
    // the shown frames: gaps > 250 ms = the picture froze
    await ev(`(() => { window.__q = {gaps: 0, gapMs: 0, n: 0, last: 0}; const cb = now => { const q = window.__q; if (q.last && now - q.last > 250) { q.gaps++; q.gapMs += now - q.last; } q.last = now; q.n++; v.requestVideoFrameCallback(cb); }; v.requestVideoFrameCallback(cb); return 1; })()`);
    const tl = [];
    let stepI = 1;
    const startS = (Date.now() - t0) / 1000;
    for (let t = 0; t < sc.secs; t++) {
      const el = (Date.now() - t0) / 1000;
      if (stepI < steps.length && el >= steps[stepI][0]) { await applyNet(steps[stepI][1]); stepI++; }
      await sleep(1000);
      tl.push(await ev(`({t: +((performance.now()) / 1000).toFixed(1), w: S.srv && S.srv.w, h: S.srv && S.srv.h, q: S.srv && S.srv.quality, kbpsSent: S.srv && Math.round(S.srv.kbps),
        kbpsTarget: S.srv && (S.srv.kbpsNow || S.srv.kbpsBase), kbpsRecv: Math.round(S.kbps), fps: S.fps, lat: S.latShow, rtt: +(+S.rtt).toFixed(0),
        rung: S.autoRung !== undefined ? S.autoRung : (typeof adapt !== 'undefined' ? adapt.level : null), curQ: typeof curQ !== 'undefined' ? curQ : null,
        rttPc: S.srv && S.srv.rttPc, lossPc: S.srv && S.srv.lossPc,
        loss: S.adapt && S.adapt.loss, gaps: window.__q.gaps, gapMs: Math.round(window.__q.gapMs)})`));
    }
    r.timeline = tl;
    const last = tl.slice(-8);
    r.settled = {size: last.length ? last[last.length - 1].w + 'x' + last[last.length - 1].h : '', q: last.length ? last[last.length - 1].q : null,
      kbpsSent: Math.round(med(last.map(x => x.kbpsSent))), kbpsRecv: Math.round(med(last.map(x => x.kbpsRecv))), fps: +med(last.map(x => x.fps)).toFixed(1),
      latMs: Math.round(med(last.map(x => x.lat))), rtt: Math.round(med(last.map(x => x.rtt)))};
    r.freeze = {gaps: tl.length ? tl[tl.length - 1].gaps : 0, ms: tl.length ? tl[tl.length - 1].gapMs : 0};
    // 0.8.4: from the first picture, how long until the bitrate fits the link (host log times), and the
    // freezes (> 250 ms without a new frame shown) until 5 s after that
    {
      const L = readLog(), hms = x => { const m = x.match(/^(\d+):(\d+):(\d+)\.(\d+)/); return m ? ((+m[1] * 60 + +m[2]) * 60 + +m[3]) * 1000 + +m[4] : NaN; };
      const ffLine = (L.match(/^.*first frame sent.*$/m) || [''])[0];
      const t0 = hms(ffLine), cap = CAP[sc.net] || Infinity;
      const kb0 = +((L.match(/stream \d+x\d+@\d+ (\d+) kbps/) || [])[1] || NaN);
      let settleMs = kb0 <= cap * 0.95 ? 0 : NaN;
      if (!(settleMs === 0)) for (const m of L.matchAll(/^(.*) \[remote\] auto quality: rung \d+ -> \d+ \(.*?= (\d+) kbps\)/gm))
        if (+m[2] <= cap * 0.95) { settleMs = hms(m[1]) - t0; break; }
      r.settleMs = Number.isFinite(settleMs) ? Math.round(settleMs) : null;
      const until = (r.settleMs || 0) / 1000 + 5;
      const at = tl.find(x => x.t - ff / 1000 >= until) || tl[tl.length - 1];
      r.freezeToSettle = at ? {gaps: at.gaps, ms: at.gapMs} : null;
    }
    r.fpsMed = +med(tl.map(x => x.fps)).toFixed(1);
    r.latMed = Math.round(med(tl.map(x => x.lat)));
    // time to the top size the run reached (seconds after the first picture)
    const top = tl.reduce((m, x) => Math.max(m, (x.w || 0) * (x.h || 0)), 0);
    const firstTop = tl.find(x => (x.w || 0) * (x.h || 0) === top);
    r.topSize = firstTop ? firstTop.w + 'x' + firstTop.h : '';
    r.toTopS = firstTop ? +(firstTop.t - ff / 1000).toFixed(1) : null;
    r.steps = (readLog().match(/^.*("what":"adapt-|auto quality: rung).*$/gm) || []).filter(l => !/page-report/.test(l)).length;
    r.autoLog = (readLog().match(/auto quality: rung.*$/gm) || []).map(l => l.slice(0, 160));
    r.qualityChanges = (readLog().match(/picture -> quality \d/g) || []).length;
    r.bitrateChanges = (readLog().match(/encoder: bitrate -> /g) || []).length;
    void startS;
  } catch (e) { r.error = String(e && e.message || e); }
  try { if (targetId) await C.cdp('Target.closeTarget', {targetId}); } catch {}
  try { spike.stdin.end(); } catch {}
  for (let i = 0; i < 50 && spike.exitCode === null; i++) await sleep(100);
  if (spike.exitCode === null) try { execFileSync('taskkill', ['/PID', String(spike.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
  r.encoder = (readLog().match(/encoder: (.*?) ready/) || [])[1] || '';
  if (process.env.KEEP_BENCH) r.log = logFile;
  const {timeline, autoLog, ...brief} = r;
  console.log(JSON.stringify(brief));
  results.push(r);
}
C.close();
await br.close();
const alt = await watch.stop();
fs.writeFileSync(outJson, JSON.stringify({set, spike: spikeExe, when: new Date().toISOString(), phone: PHONE, results, altTabMax: alt.max, altTabSamples: alt.samples}, null, 1));
if (process.env.KEEP_BENCH) console.log('logs kept in', work); else try { fs.rmSync(work, {recursive: true, force: true}); } catch {}
console.log(`Alt+Tab / taskbar windows of the test's processes: max ${alt.max} over ${alt.samples} samples`);
process.exit(alt.max === 0 && results.every(r => !r.error) ? 0 : 1);
