// 0.8.3.2: the phone's sound level end to end (node 22+), SILENT: the spike's synthetic source
// (--synthetic-audio: no device is captured, nothing is played on the PC) -> mix -> level.h ->
// Opus -> WebRTC -> a headless Edge with --mute-audio (nothing is played on the "phone" either).
//   node level_e2e.mjs <pm_remote_spike.exe> <outDir> [--seconds N] [--modes quiet,speech,loud,music] [--ab]
// --loopback always: the spike's "LAN" side is 127.0.0.3 only (no firewall prompt); --no-tailscale;
// no tailscale / cloudflared is ever run. --ab: each mode also with --no-agc (0.8.3.1's level path
// minus its AGC, which stayed at x1 on such material anyway). Reads the PC's "stats audio" lines
// (peak, AGC, RMS) and the page's own analyser (S.rmsIn, before its volume) every second.
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';

const [spikeExe, outDir, ...rest] = process.argv.slice(2);
if (!spikeExe || !outDir) { console.error('usage: node level_e2e.mjs <pm_remote_spike.exe> <outDir> [--seconds N] [--modes a,b] [--ab]'); process.exit(2); }
let seconds = 20, modes = ['quiet', 'speech', 'loud', 'music'], ab = false;
for (let i = 0; i < rest.length; i++) {
  if (rest[i] === '--seconds') seconds = +rest[++i];
  else if (rest[i] === '--modes') modes = rest[++i].split(',');
  else if (rest[i] === '--ab') ab = true;
}
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
const db = x => x > 0 ? +(20 * Math.log10(x)).toFixed(1) : -120;

async function startSpike(tag, args) {
  for (const bad of ['--funnel', '--tunnel', '--fixed-tunnel', '--inject', '--tone', '--test-tone', '--cloudflared'])
    if (args.includes(bad)) throw new Error('refused: ' + bad);
  const token = crypto.randomBytes(16).toString('hex');
  const port = 18200 + Math.floor(Math.random() * 600);
  const logFile = path.join(outDir, tag + '-spike.log');
  fs.writeFileSync(logFile, '');
  const p = spawn(spikeExe, ['--token', token, '--port', String(port), '--log', logFile, '--duration', String(seconds + 60), '--stdin-quit',
                             '--loopback', '--no-tailscale', ...args], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  let out = '';
  p.stdout.on('data', d => { out += d; });
  p.stderr.on('data', d => { out += d; });
  let lan = '';
  for (let i = 0; i < 150 && !lan; i++) {
    await sleep(100);
    const m = out.match(/urls: lan (http:\/\/127\.0\.0\.3:\d+\/[0-9a-f]+\/)/);
    if (m) lan = m[1];
  }
  if (!lan) { try { p.kill(); } catch {} throw new Error('spike did not start on 127.0.0.3: ' + out.slice(0, 400)); }
  log(tag, 'spike pid', p.pid, args.join(' '));
  return {p, lan, readLog: () => fs.readFileSync(logFile, 'utf8')};
}
async function stopSpike(s) {
  try { s.p.stdin.end(); } catch {}
  for (let i = 0; i < 100 && s.p.exitCode === null; i++) await sleep(100);
  if (s.p.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(s.p.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} log('killed own spike', s.p.pid); }
}
async function startEdge() {
  const profileDir = path.join(os.tmpdir(), 'edge-0832-level-' + crypto.randomBytes(3).toString('hex'));
  const cdpPort = 9500 + Math.floor(Math.random() * 300);
  const p = spawn('C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
                  [`--user-data-dir=${profileDir}`, `--remote-debugging-port=${cdpPort}`, '--no-first-run', '--no-default-browser-check', '--headless=new',
                   '--mute-audio', '--allow-loopback-in-peer-connection', '--autoplay-policy=no-user-gesture-required', '--disable-extensions', '--disable-sync',
                   'about:blank'], {stdio: 'ignore', windowsHide: true});
  let ver;
  for (let i = 0; i < 80 && !ver; i++) { await sleep(200); try { ver = await (await fetch(`http://127.0.0.1:${cdpPort}/json/version`)).json(); } catch {} }
  const ws = new WebSocket(ver.webSocketDebuggerUrl);
  await new Promise(r => ws.addEventListener('open', r));
  let nextId = 1;
  const pend = new Map();
  ws.addEventListener('message', e => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m.result || m.error); pend.delete(m.id); } });
  const cdp = (method, params = {}, sessionId) => new Promise(r => { const id = nextId++; pend.set(id, r); ws.send(JSON.stringify({id, method, params, ...(sessionId ? {sessionId} : {})})); });
  const e = {p, profileDir};
  e.open = async url => {
    const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
    const {sessionId} = await cdp('Target.attachToTarget', {targetId, flatten: true});
    await cdp('Emulation.setDeviceMetricsOverride', {width: 390, height: 844, deviceScaleFactor: 3, mobile: true}, sessionId);
    await cdp('Page.navigate', {url}, sessionId);
    const ev = async (expression, awaitPromise = false) => {
      const r = await cdp('Runtime.evaluate', {expression, returnByValue: true, awaitPromise, userGesture: true}, sessionId);
      return r && r.result ? r.result.value : undefined;
    };
    return {ev, close: () => cdp('Target.closeTarget', {targetId})};
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

const results = {};
const runs = [];
for (const m of modes) { if (ab) runs.push([m + '-no-agc', ['--synthetic-audio', m, '--no-agc']]); runs.push([m, ['--synthetic-audio', m]]); }
for (const [tag, args] of runs) {
  let s = null, e = null;
  try {
    s = await startSpike(tag, args);
    e = await startEdge();
    const tab = await e.open(s.lan);
    await waitFor(() => tab.ev('window.__pmFirstFrame ? 1 : 0'), 20000);
    await tab.ev("(() => { soundTap(); return 1; })()");  // the page's own 開啟聲音 (Web Audio path; Edge is muted)
    const page = [];
    for (let i = 0; i < seconds; i++) {
      await sleep(1000);
      page.push(await tab.ev('JSON.stringify({rmsIn: S.rmsIn || 0, rmsOut: S.rmsOut || 0, level: S.aud ? S.aud.level || 0 : null, gain: prefs.gain})'));
    }
    const pg = page.map(x => JSON.parse(x || '{}'));
    const st = [...s.readLog().matchAll(/stats audio: (\d+) Opus packets sent .*? peak ([\d.]+) \(AGC x([\d.]+), volume x[\d.]+\)(?:, RMS (-?[\d.]+) dBFS)?/g)]
      .map(x => ({packets: +x[1], peak: +x[2], agc: +x[3], agcDb: db(+x[3]), rmsDb: x[4] === undefined ? null : +x[4]}));
    const levelLine = (s.readLog().match(/audio: level: [^\n]*/) || [''])[0];
    const settledPage = pg.slice(Math.floor(pg.length / 2)).map(x => x.rmsIn).filter(x => x > 0);
    results[tag] = {
      levelLine, pcStats: st,
      pageRmsInDb: pg.map(x => db(x.rmsIn)),
      pageRmsInSettledDb: settledPage.length ? db(Math.sqrt(settledPage.reduce((a, x) => a + x * x, 0) / settledPage.length)) : null,
      pageGain: pg.length ? pg[0].gain : null,
    };
    log(tag, JSON.stringify({stats: st.map(x => `${x.peak}/${x.agcDb}dB/${x.rmsDb}`).join(' '), pageSettledDb: results[tag].pageRmsInSettledDb}));
    await tab.close();
  } catch (err) { results[tag] = {error: String(err && err.message || err)}; log(tag, 'ERROR', results[tag].error); }
  if (e) await e.close();
  if (s) await stopSpike(s);
}
fs.writeFileSync(path.join(outDir, 'level_e2e.json'), JSON.stringify(results, null, 1));
log('wrote', path.join(outDir, 'level_e2e.json'));
