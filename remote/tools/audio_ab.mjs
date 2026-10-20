// --pattern PLAYS AUDIBLE TONES (owner rule: not any more); use --synthetic-audio music|loud (silent).
// Sound before / after (0.8.2): the PC's real sound (whatever plays on this PC's
// devices) to a headless phone page for N seconds, sound on.
// Per 0.5 s: the level the browser receives (inbound-rtp audioLevel, before any
// phone-side gain) and, on 0.8.2 pages, the Web Audio RMS before / after the gain.
// From the spike log: how often the sent device changed (0.8.1 "audio: sending ... now").
//   node audio_ab.mjs <spike.exe> <outDir> [--seconds 60] [--label X] [spike args]
// Dry-run input, no visible window, own temp profile; closes only its own processes.
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';

const [spikeExe, outDir, ...rest] = process.argv.slice(2);
let seconds = 60, label = path.basename(outDir);
const spikeArgs = [];
for (let i = 0; i < rest.length; i++) {
  if (rest[i] === '--seconds') seconds = +rest[++i];
  else if (rest[i] === '--label') label = rest[++i];
  else if (rest[i] === '--pattern') {  // --pattern <device part>: 3 s tone, 1.5 s pause, over the run (amplitude 0.1)
    const dev = rest[++i];
    const gap = +(process.env.PM_AB_GAP || 1.5);
    for (let t = 8; t < 8 + 60; t += 3 + gap) spikeArgs.push('--tone', `${dev}@${t}+3:0.1`);
  }
  else spikeArgs.push(rest[i]);
}
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const logFile = path.join(outDir, 'spike.log');
fs.writeFileSync(logFile, '');
const token = crypto.randomBytes(16).toString('hex');
const port = 8600 + Math.floor(Math.random() * 400);
const spike = spawn(spikeExe, ['--token', token, '--port', String(port), '--log', logFile, '--duration', String(seconds + 90), '--stdin-quit', '--no-tailscale', ...spikeArgs],
                    {stdio: ['pipe', 'ignore', 'ignore']});
let url = '';
for (let i = 0; i < 100 && !url; i++) { await sleep(100); const m = fs.readFileSync(logFile, 'utf8').match(/urls: lan (http:\/\/[\d.]+:\d+\/[0-9a-f]+\/)/); if (m) url = m[1]; }
const profileDir = path.join(os.tmpdir(), 'edge-082-audioab');
try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
const cdpPort = 9400 + Math.floor(Math.random() * 400);
const edge = spawn('C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
  [`--user-data-dir=${profileDir}`, `--remote-debugging-port=${cdpPort}`, '--no-first-run', '--headless=new', '--autoplay-policy=no-user-gesture-required', 'about:blank'], {stdio: 'ignore'});
let ver;
for (let i = 0; i < 50 && !ver; i++) { await sleep(200); try { ver = await (await fetch(`http://127.0.0.1:${cdpPort}/json/version`)).json(); } catch {} }
const ws = new WebSocket(ver.webSocketDebuggerUrl);
await new Promise(r => ws.addEventListener('open', r));
let id = 1; const pend = new Map();
ws.addEventListener('message', e => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m.result); pend.delete(m.id); } });
const cdp = (method, params = {}, sessionId) => new Promise(r => { const i = id++; pend.set(i, r); ws.send(JSON.stringify({id: i, method, params, ...(sessionId ? {sessionId} : {})})); });
const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
const {sessionId} = await cdp('Target.attachToTarget', {targetId, flatten: true});
const ev = async e => ((await cdp('Runtime.evaluate', {expression: e, returnByValue: true, userGesture: true}, sessionId)).result || {}).value;
await cdp('Page.navigate', {url}, sessionId);
for (let i = 0; i < 100; i++) { await sleep(100); if (await ev('typeof S !== "undefined" && S.state === "connected"')) break; }
await ev('soundTap()');
await sleep(1500);
// The same meter for every page version: an analyser on the received audio track (before any phone-side gain).
await ev(`(() => { window.__ac = new AudioContext(); const src = __ac.createMediaStreamSource(v.srcObject);
  window.__an = __ac.createAnalyser(); __an.fftSize = 2048; src.connect(__an); __ac.resume(); return true; })()`);
await sleep(500);
const t0Log = fs.readFileSync(logFile, 'utf8').length;
const samples = [];
const end = Date.now() + seconds * 1000;
while (Date.now() < end) {
  const s = await ev(`JSON.stringify({rx: (() => { const a = new Float32Array(2048); __an.getFloatTimeDomainData(a); let q = 0; for (const x of a) q += x * x; return Math.sqrt(q / a.length); })(), level: S.aud ? S.aud.level : null, rmsIn: S.rmsIn ?? null, outPeak: (typeof snd !== "undefined" && snd.anOut) ? (() => { const a = new Float32Array(2048); snd.anOut.getFloatTimeDomainData(a); let m = 0; for (const x of a) m = Math.max(m, Math.abs(x)); return m; })() : null, rmsOut: S.rmsOut ?? null, aDev: S.srv && S.srv.aDev, gain: typeof prefs.gain === 'number' ? prefs.gain : null})`);
  samples.push(JSON.parse(s || '{}'));
  await sleep(500);
}
const L = fs.readFileSync(logFile, 'utf8').slice(t0Log);
const switches = (L.match(/audio: sending [^\n]* now/g) || []).length;
const lv = samples.map(s => s.level).filter(x => typeof x === 'number');
const avg = a => a.length ? a.reduce((x, y) => x + y, 0) / a.length : null;
const pct = (a, p) => { const b = a.slice().sort((x, y) => x - y); return b.length ? b[Math.min(b.length - 1, Math.floor(b.length * p))] : null; };
const quiet = lv.filter(x => x < 0.05).length;
const rin = samples.map(s => s.rmsIn).filter(x => typeof x === 'number'), rout = samples.map(s => s.rmsOut).filter(x => typeof x === 'number');
const rx = samples.map(s => s.rx).filter(x => typeof x === 'number');
const op = samples.map(s => s.outPeak).filter(x => typeof x === "number");
const hostPeaks = (L.match(/peak ([0-9.]+) from/g) || []).map(x => +x.split(" ")[1]);
const summary = {label, out_peak_max: op.length ? Math.max(...op) : null, host_sent_peak_max: hostPeaks.length ? Math.max(...hostPeaks) : null, rx_rms_mean: avg(rx), rx_rms_p50: pct(rx, 0.5), rx_share_quiet: rx.length ? +(rx.filter(x => x < 0.01).length / rx.length).toFixed(2) : null, seconds, switches, switchesPerMinute: +(switches * 60 / seconds).toFixed(1), level_mean: avg(lv), level_p50: pct(lv, 0.5), level_p10: pct(lv, 0.1),
                 share_below_0_05: lv.length ? +(quiet / lv.length).toFixed(2) : null, rmsIn_mean: avg(rin), rmsOut_mean: avg(rout), gain: samples.at(-1)?.gain ?? null,
                 devices: [...new Set(samples.map(s => s.aDev))]};
fs.writeFileSync(path.join(outDir, 'samples.json'), JSON.stringify(samples));
fs.writeFileSync(path.join(outDir, 'summary.json'), JSON.stringify(summary, null, 1));
console.log(JSON.stringify(summary));
await cdp('Browser.close');
for (let i = 0; i < 50 && edge.exitCode === null; i++) await sleep(100);
try { spike.stdin.end(); } catch {}
for (let i = 0; i < 100 && spike.exitCode === null; i++) await sleep(100);
for (const p of [edge, spike]) if (p.exitCode === null) try { execFileSync('taskkill', ['/PID', String(p.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
