// 0.8.1 sound test: the PC sound reaches the phone page from whichever
// playback device plays (two devices, one test tone each, played by the spike
// itself and stopped by it), the page says 「電腦目前沒有在播放聲音」 while
// nothing plays, and turning the sound off / on quickly does not reopen the
// capture (0.8.0: three reopenings in 7 s).
//   node audio_test.mjs <spike.exe> <outDir> --dev1 <name part> --dev2 <name part> [--amp 0.03] [spike args]
// Edge headless, throw-away profile (%TEMP%\edge-081-audio); only its own processes are closed.
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';

// 0.8.2: this test PLAYS AUDIBLE TONES on real speakers. Not to be run where people listen (owner rule);
// use audio_ab.mjs --synthetic-audio instead. Needs --allow-audible.
const [spikeExe, outDir, ...rest] = process.argv.slice(2);
if (!rest.includes("--allow-audible")) { console.error("refusing: plays audible tones (use audio_ab.mjs --synthetic-audio, or pass --allow-audible)"); process.exit(2); }
let dev1 = 'Razer', dev2 = 'Display', amp = '0.03', amp2 = '', source = '';
const spikeArgs = [];
for (let i = 0; i < rest.length; i++) {
  if (rest[i] === '--dev1') dev1 = rest[++i];
  else if (rest[i] === '--dev2') dev2 = rest[++i];
  else if (rest[i] === '--amp') amp = rest[++i];
  else if (rest[i] === '--amp2') amp2 = rest[++i];
  else if (rest[i] === '--source') source = rest[++i];  // a device name part: chosen by hand (else automatic)
  else if (rest[i] !== '--allow-audible') spikeArgs.push(rest[i]);
}
fs.mkdirSync(outDir, {recursive: true});
if (!amp2) amp2 = amp;
if (source) {
  const list = execFileSync(spikeExe, ['--list-audio'], {encoding: 'utf8'}).split(/\r?\n/).filter(Boolean);
  const line = list.find(l => l.toLowerCase().includes(source.toLowerCase()));
  if (!line) { console.error('no device', source, list); process.exit(2); }
  spikeArgs.push('--audio-source', line.split('\t')[1]);
}
const sleep = ms => new Promise(r => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
const token = crypto.randomBytes(16).toString('hex');
const port = 8600 + Math.floor(Math.random() * 400);
const logFile = path.join(outDir, 'spike.log');
fs.writeFileSync(logFile, '');
// Timeline (s after the spike starts): dev1 plays 12-16, dev2 plays 24-28.
const spike = spawn(spikeExe, ['--token', token, '--port', String(port), '--log', logFile, '--duration', '120', '--stdin-quit', '--no-tailscale',
                               '--tone', `${dev1}@12+4:${amp}`, '--tone', `${dev2}@24+4:${amp2}`, ...spikeArgs], {stdio: ['pipe', 'pipe', 'pipe']});
const t0 = Date.now();
let out = '';
spike.stdout.on('data', d => { out += d; });
spike.stderr.on('data', d => { out += d; });
let url = '';
for (let i = 0; i < 100 && !url; i++) { await sleep(100); const m = out.match(/urls: lan (http:\/\/[\d.]+:\d+\/[0-9a-f]+\/)/); if (m) url = m[1]; }
if (!url) { console.error(out); spike.kill(); process.exit(1); }
const profileDir = path.join(os.tmpdir(), 'edge-081-audio');
try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
const cdpPort = 9400 + Math.floor(Math.random() * 400);
const edge = spawn('C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
  [`--user-data-dir=${profileDir}`, `--remote-debugging-port=${cdpPort}`, '--no-first-run', '--no-default-browser-check', '--disable-extensions',
   '--disable-sync', '--window-size=420,900', '--headless=new', 'about:blank'], {stdio: 'ignore'});
let ws, nextId = 1;
const pending = new Map();
const cdp = (method, params = {}, sessionId) => { const id = nextId++; ws.send(JSON.stringify({id, method, params, ...(sessionId ? {sessionId} : {})})); return new Promise((res, rej) => pending.set(id, {res, rej})); };
async function cleanup(code) {
  try { if (ws) await Promise.race([cdp('Browser.close'), sleep(3000)]); } catch {}
  await sleep(1000);
  try { spike.stdin.end(); } catch {}
  for (let i = 0; i < 150 && spike.exitCode === null; i++) await sleep(100);
  for (const p of [edge, spike]) if (p.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(p.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} }
  const mine = () => { try { return execFileSync('powershell', ['-NoProfile', '-Command', `Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" | Where-Object { $_.CommandLine -and $_.CommandLine.Contains('${profileDir}') } | ForEach-Object { $_.ProcessId }`], {encoding: 'utf8'}).split(/\s+/).filter(Boolean).map(Number); } catch { return []; } };
  let left = mine();
  for (let i = 0; i < 20 && left.length; i++) { await sleep(1000); left = mine(); }
  for (const pid of left) { try { execFileSync('taskkill', ['/PID', String(pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} }
  log('own edge processes left:', mine().length, 'spike exit', spike.exitCode);
  process.exit(code);
}
let wsUrl = '';
for (let i = 0; i < 100 && !wsUrl; i++) { await sleep(200); try { wsUrl = (await (await fetch(`http://127.0.0.1:${cdpPort}/json/version`)).json()).webSocketDebuggerUrl; } catch {} }
ws = new WebSocket(wsUrl);
await new Promise(r => ws.addEventListener('open', r));
ws.addEventListener('message', ev => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { const p = pending.get(m.id); pending.delete(m.id); m.error ? p.rej(new Error(m.error.message)) : p.res(m.result); } });
const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
const {sessionId: sid} = await cdp('Target.attachToTarget', {targetId, flatten: true});
const S = (m, p) => cdp(m, p, sid);
await S('Page.enable');
await S('Emulation.setDeviceMetricsOverride', {width: 390, height: 844, deviceScaleFactor: 3, mobile: true});
await S('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5});
await S('Page.navigate', {url});
const ev = async e => (await S('Runtime.evaluate', {expression: e, returnByValue: true})).result.value;
const tapEl = async sel => {
  const r = await ev(`(() => { const e = document.querySelector('${sel}'); if (!e || !e.offsetParent && getComputedStyle(e).position !== 'fixed') return null; const b = e.getBoundingClientRect(); return {x: b.left + b.width / 2, y: b.top + b.height / 2}; })()`);
  if (!r) return false;
  await S('Input.dispatchTouchEvent', {type: 'touchStart', touchPoints: [{x: r.x, y: r.y}]});
  await S('Input.dispatchTouchEvent', {type: 'touchEnd', touchPoints: []});
  return true;
};
for (let i = 0; i < 100; i++) { await sleep(100); if (await ev('!!(typeof S !== "undefined" && S.state === "connected" && document.querySelector("video").videoWidth > 16)')) break; }
log('connected; sound on via the page button');
await tapEl('#bSnd');
await sleep(1500);
// Rapid off / on (like the owner's taps at 20:31:42-49): the capture must stay open.
for (let k = 0; k < 3; k++) { await tapEl('#bSnd'); await sleep(400); await tapEl('#bSnd'); await sleep(600); }
log('toggled 3x; sampling');
const samples = [];
while (Date.now() - t0 < 34000) {
  const s = await ev(`JSON.stringify({sound: S.sound, hint: getComputedStyle(document.getElementById('silent')).display !== 'none', hintText: document.getElementById('silent').textContent,
    level: S.aud ? S.aud.level : null, packets: S.aud ? S.aud.packets : 0, aDev: S.srv && S.srv.aDev, aPeak: S.srv && S.srv.aPeak})`);
  samples.push({t: +((Date.now() - t0) / 1000).toFixed(1), ...JSON.parse(s)});
  await sleep(500);
}
await S('Page.captureScreenshot', {format: 'png'}).then(r => fs.writeFileSync(path.join(outDir, 'phone-end.png'), Buffer.from(r.data, 'base64')));
fs.writeFileSync(path.join(outDir, 'samples.json'), JSON.stringify(samples, null, 1));
// Checks.
const inWin = (a, b) => samples.filter(s => s.t >= a && s.t <= b);
const maxPeak = (a, b) => Math.max(0, ...inWin(a, b).map(s => s.aPeak || 0));
const devIn = (a, b) => [...new Set(inWin(a, b).map(s => s.aDev))].join(' / ');
// The hint's page logic, fed with the PC's stats (the real PC may not be silent during a test:
// the owner was playing music on one device). 6 silent seconds -> shown; sound -> hidden.
const hintLogic = await ev(`(() => {
  const st = p => onDc({data: JSON.stringify({t: 'stats', aOn: true, aPeak: p, aDev: 'test', viewOnly: S.viewOnly})});
  const vis = () => getComputedStyle(document.getElementById('silent')).display !== 'none';
  const r = {};
  st(0.2);  // start from "sound" (a real silence before may have shown it already)
  for (let i = 0; i < 5; i++) st(0);
  r.after5 = vis();
  st(0); r.after6 = vis(); r.text = document.getElementById('silent').textContent;
  st(0.2); r.afterSound = vis();
  return r; })()`);
const L = fs.readFileSync(logFile, 'utf8');
const quiet = inWin(9, 11.5).every(s => (s.aPeak || 0) < 0.001);
const checks = [
  ['device list logged with names and default roles', /audio: playback devices \(source: [^)]*\): .*\[Windows default: console/.test(L)],
  ...(source ? [[`chosen device (${source}) is the one sent all along`, samples.filter(s => s.aDev).every(s => s.aDev.toLowerCase().includes(source.toLowerCase())), devIn(0, 40)],
                [`its tone (${dev2}, 25-28 s) reaches the phone: peak > 0.005 and > 3x the 9-11 s level`,
                 maxPeak(25.5, 28.5) > 0.005 && maxPeak(25.5, 28.5) > 3 * maxPeak(9, 11.5), maxPeak(9, 11.5).toFixed(4) + ' -> ' + maxPeak(25.5, 28.5).toFixed(4)],
                [`the other device's tone (${dev1}, 13-16 s) is not sent`, maxPeak(13.5, 16.5) < Math.max(0.005, 3 * maxPeak(9, 11.5)), maxPeak(13.5, 16.5).toFixed(4)]]
           : [[`while ${dev1} plays (13-16 s) the phone gets sound: peak > 0.01`, maxPeak(13.5, 16.5) > 0.01, maxPeak(13.5, 16.5).toFixed(4), devIn(13.5, 16.5)],
              [`while ${dev2} plays (25-28 s) the phone gets sound: peak > 0.01`, maxPeak(25.5, 28.5) > 0.01, maxPeak(25.5, 28.5).toFixed(4), devIn(25.5, 28.5)],
              ['automatic: sent the device that plays (log "sending ... it is the one playing")', /sending .* now \(automatic: it is the one playing/.test(L)]]),
  ['page logic: 「電腦目前沒有在播放聲音」 after 6 silent s, gone with sound', !hintLogic.after5 && hintLogic.after6 && !hintLogic.afterSound, JSON.stringify(hintLogic)],
  ...(quiet ? [['real silence 9-11.5 s: hint shown', inWin(9.5, 12).some(s => s.hint)]] : []),
  ['quick off/on x3 did not reopen the capture (one "sending" start line, "nothing reopened" x3)',
   (L.match(/audio: sending [^\n]*\((automatic: the Windows default until another one plays|as set);/g) || []).length === 1 && (L.match(/nothing reopened/g) || []).length >= 3],
  ['the browser received audio packets', samples.some(s => s.packets > 50)],
];
if (!quiet) console.log('note: the PC was not silent 9-11.5 s (another program plays sound): the real-silence hint check is skipped');
let ok = true;
const lines = checks.map(([name, pass, ...info]) => { ok = ok && pass; return (pass ? 'PASS ' : 'FAIL ') + name + (info.length ? '  [' + info.join(', ') + ']' : ''); });
lines.push(ok ? 'ALL PASS' : 'SOME FAILED');
fs.writeFileSync(path.join(outDir, 'checks.txt'), lines.join('\n') + '\n');
console.log(lines.join('\n'));
await cleanup(ok ? 0 : 1);
