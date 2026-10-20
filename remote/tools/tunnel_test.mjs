// 外網連結 (0.8.1) end to end through a real cloudflared quick tunnel
// (https://<random>.trycloudflare.com): the spike starts cloudflared, Edge
// headless (phone emulation, throw-away profile %TEMP%\edge-081-tunnel) opens
// the one-time link, enters the 通行碼, the spike answers 允許, then
//   run "direct": WebRTC with public STUN connects (on this PC: host / srflx candidates)
//   run "tunnel": the page drops every ICE candidate (?simice) -> H.264 + Opus over the
//                 tunnel's WebSocket, decoded with WebCodecs; input over the same socket
// Each run: first-picture phases, 直連 / 隧道 badge, RTT, capture->shown latency,
// frame rate, bitrate, a click (dry-run), sound packets, screenshots.
//   node tunnel_test.mjs <spike.exe> <outDir> [--cloudflared EXE] [--seconds N] [spike args]
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';

const [spikeExe, outDir, ...rest] = process.argv.slice(2);
let cf = 'C:\\Program Files (x86)\\cloudflared\\cloudflared.exe', seconds = 15, runs = 'direct,tunnel';
const spikeArgs = [];
for (let i = 0; i < rest.length; i++) {
  if (rest[i] === '--cloudflared') cf = rest[++i];
  else if (rest[i] === '--seconds') seconds = +rest[++i];
  else if (rest[i] === '--runs') runs = rest[++i];  // direct,tunnel,slow
  else spikeArgs.push(rest[i]);
}
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
const logFile = path.join(outDir, 'spike.log');
fs.writeFileSync(logFile, '');
const port = 8600 + Math.floor(Math.random() * 400);
const spike = spawn(spikeExe, ['--port', String(port), '--log', logFile, '--duration', '400', '--stdin-quit', '--no-tailscale', '--tunnel', '--cloudflared', cf,
                               '--approval', '--approve-after', '800', '--stamp', ...spikeArgs], {stdio: ['pipe', 'ignore', 'ignore']});
log('spike pid', spike.pid);
const L = () => fs.readFileSync(logFile, 'utf8');
let link = '', pass = '';
for (let i = 0; i < 300 && !link; i++) { await sleep(100); const m = L().match(/tunnel: ready \S+ link (\S+) passcode (\d{6})/); if (m) link = m[1], pass = m[2]; }
if (!link) { console.error('no tunnel URL'); try { spike.stdin.end(); } catch {} await sleep(3000); process.exit(1); }
log('one-time link', link, 'passcode', pass);
// The public name needs a moment to resolve everywhere.
for (let i = 0; i < 20; i++) { try { const r = await fetch(link.replace(/\/t\/.*/, '/'), {redirect: 'manual'}); if (r.status) break; } catch {} await sleep(1000); }

const profileDir = path.join(os.tmpdir(), 'edge-081-tunnel');
try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
const cdpPort = 9400 + Math.floor(Math.random() * 400);
const edge = spawn('C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
  [`--user-data-dir=${profileDir}`, `--remote-debugging-port=${cdpPort}`, '--no-first-run', '--no-default-browser-check', '--disable-extensions', '--disable-sync',
   '--window-size=420,900', '--headless=new', '--autoplay-policy=no-user-gesture-required', 'about:blank'], {stdio: 'ignore'});
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
  const cfLeft = execFileSync('tasklist', ['/FI', 'IMAGENAME eq cloudflared.exe'], {encoding: 'utf8'}).includes('cloudflared.exe');
  log('own edge processes left:', mine().length, '| spike exit', spike.exitCode, '| cloudflared left:', cfLeft);
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
await S('Emulation.setUserAgentOverride', {userAgent: 'Mozilla/5.0 (Linux; Android 15; Pixel 9) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0 Mobile Safari/537.36'});
const ev = async e => { const r = await S('Runtime.evaluate', {expression: e, returnByValue: true}); return r.result.value; };
const shot = async name => { const r = await S('Page.captureScreenshot', {format: 'png'}); fs.writeFileSync(path.join(outDir, name), Buffer.from(r.data, 'base64')); };
const tap = async (x, y) => { await S('Input.dispatchTouchEvent', {type: 'touchStart', touchPoints: [{x, y}]}); await S('Input.dispatchTouchEvent', {type: 'touchEnd', touchPoints: []}); };
const tapEl = async sel => { const r = await ev(`(() => { const e = document.querySelector('${sel}'); if (!e) return null; const b = e.getBoundingClientRect(); return {x: b.left + b.width / 2, y: b.top + b.height / 2}; })()`); if (r) await tap(r.x, r.y); return !!r; };
const until = async (fn, ms) => { const end = Date.now() + ms; while (Date.now() < end) { try { if (await fn()) return true; } catch {} await sleep(150); } return false; };
const results = {};
const checks = [];
const check = (name, pass, info) => { checks.push((pass ? 'PASS ' : 'FAIL ') + name + (info !== undefined ? '  [' + info + ']' : '')); };

// ---- the one-time link, the 通行碼 ----
const navT = Date.now();
await S('Page.navigate', {url: link});
const passShown = await until(() => ev(`getComputedStyle(document.getElementById('pass')).display === 'flex'`), 20000);
check('one-time link -> page asks for the 通行碼', passShown);
const credUrl = await ev('location.href');
await shot('1-passcode.png');
await ev(`document.getElementById('passIn').focus()`);
await S('Input.insertText', {text: '000000'});
await until(() => ev(`document.getElementById('passErr').textContent.length > 0`), 8000);
results.wrongText = await ev(`document.getElementById('passErr').textContent`);
check('wrong 通行碼 -> told, tries left', /4/.test(results.wrongText), results.wrongText);
await shot('2-passcode-wrong.png');
await S('Input.insertText', {text: pass});
check('right 通行碼 -> on to the PC (允許)', await until(() => ev(`getComputedStyle(document.getElementById('pass')).display === 'none'`), 8000));
// A second visit of the spent one-time link: refused.
const again = await fetch(link, {redirect: 'manual'}).then(r => r.status).catch(() => 0);
check('the one-time link works once (second use 410)', again === 410, again);

async function run(name, query, slow) {
  log('run', name);
  // A slow phone link (mobile data): 1.5 Mbps down, 80 ms each way.
  // (The slow link itself is the spike's --tunnel-test-kbps: DevTools throttling applies its latency
  // to every WebSocket message in turn, which no real network does.)
  if (query) { await S('Page.navigate', {url: credUrl + '?' + query}); }
  const t0 = Date.now();
  const playing = await until(() => ev(`!!(window.__pmFirstFrame && (document.getElementById('scr').width > 16 || document.querySelector('video').videoWidth > 16))`), 30000);
  const ph = await ev('JSON.stringify(S.ph)');
  results[name] = {firstFrameWall: Date.now() - t0, ph: JSON.parse(ph || '{}')};
  check(`${name}: picture shows`, playing, results[name].firstFrameWall + ' ms');
  await tapEl('#bSnd');  // sound on (in a tap: the tunnel's AudioContext may start)
  await sleep(1500);
  const samples = [];
  for (let i = 0; i < seconds; i++) {
    await sleep(1000);
    const s = await ev(`JSON.stringify({path: document.getElementById('path').textContent, pathShown: getComputedStyle(document.getElementById('path')).display !== 'none',
      rtt: S.rtt, fps: S.fps, lat: S.lat.length ? S.lat.slice().sort((a, b) => a - b)[S.lat.length >> 1] : null, latN: S.lat.length,
      kbps: S.srv && S.srv.kbps, srvFps: S.srv && S.srv.fps, aud: S.aud && S.aud.packets, w: S.w, h: S.h, media: tun.media, state: S.state})`);
    samples.push(JSON.parse(s));
    if (i === 3) await tap(200, 400);  // a click (dry-run on the PC)
  }
  fs.writeFileSync(path.join(outDir, `${name}-samples.json`), JSON.stringify(samples, null, 1));
  await shot(`3-${name}.png`);
  const med = k => { const a = samples.map(x => x[k]).filter(x => typeof x === 'number' && isFinite(x)).sort((a, b) => a - b); return a.length ? a[a.length >> 1] : null; };
  results[name].samples = samples.length;
  const p95 = k => { const a = samples.map(x => x[k]).filter(x => typeof x === 'number' && isFinite(x)).sort((a, b) => a - b); return a.length ? a[Math.min(a.length - 1, Math.floor(a.length * 0.95))] : null; };
  results[name].median = {rtt: med('rtt'), lat: med('lat'), fps: med('fps'), kbps: med('kbps'), srvFps: med('srvFps')};
  results[name].p95 = {rtt: p95('rtt'), lat: p95('lat')};

  results[name].pathText = samples[samples.length - 1].path;
  results[name].audio = (samples[samples.length - 1].aud || 0) - (samples[0].aud || 0);
  return samples;
}
const R = runs.split(',');
if (R.includes('direct')) {
const sd = await run('direct', '');
const wantDirect = await ev(`t('viaDirect')`), wantTunnel = await ev(`t('viaTunnel')`);
check('direct: badge says 直連 + ms', sd.every(s => s.pathShown && s.path.startsWith(wantDirect) && /\d+ ms/.test(s.path)), results.direct.pathText);
check('direct: WebRTC carried it (not the tunnel)', sd.every(s => !s.media));
const Ld = L();
check('direct: selected pair logged (STUN configured)', /selected pair local/.test(Ld) && /direct WebRTC through the 外網連結/.test(Ld));
check('direct: a click arrived (dry-run)', /input \(dry-run\): left click/.test(Ld));
check('direct: sound packets', results.direct.audio > 20, results.direct.audio);

}
if (R.includes('tunnel')) {
const clicksBefore = (L().match(/input \(dry-run\): left click/g) || []).length;
const st = await run('tunnel', 'simice');
check('tunnel: badge says 隧道 + ms', st.every(s => s.pathShown && s.path.startsWith(wantTunnel) && /\d+ ms/.test(s.path)), results.tunnel.pathText);
check('tunnel: the canvas (WebCodecs) shows it', st.every(s => s.media && s.w > 16));
const Lt = L();
check('tunnel: PC log says picture over the tunnel', /picture and sound now over the tunnel/.test(Lt));
check('tunnel: a click arrived over the WebSocket (dry-run)', (Lt.match(/input \(dry-run\): left click/g) || []).length > clicksBefore);
check('tunnel: sound over the WebSocket (AudioDecoder)', results.tunnel.audio > 20, results.tunnel.audio);
check('tunnel: frames keep coming (median fps > 5)', (results.tunnel.median.fps || 0) > 5, results.tunnel.median.fps);
}
if (R.includes('slow')) {
  await run('slow', 'simice&slow=1', true);
  const Ls = L();
  results.slow.cuts = (Ls.match(/the tunnel is full/g) || []).length;
  results.slow.cutLines = Ls.split(/\r?\n/).filter(l => /tunnel: (the tunnel is full|flowing again)/.test(l)).slice(0, 12);
  check('slow tunnel: picture keeps moving (median fps > 3)', (results.slow.median.fps || 0) > 3, results.slow.median.fps);
  check('slow tunnel: latency stays bounded (p95 < 2000 ms)', (results.slow.p95.lat || 1e9) < 2000, results.slow.median.lat + ' / p95 ' + results.slow.p95.lat);
}

// ---- summary ----
fs.writeFileSync(path.join(outDir, 'results.json'), JSON.stringify(results, null, 2));
const ok = checks.every(c => c.startsWith('PASS'));
checks.push(ok ? 'ALL PASS' : 'SOME FAILED');
fs.writeFileSync(path.join(outDir, 'checks.txt'), checks.join('\n') + '\n');
console.log(checks.join('\n'));
console.log(JSON.stringify(Object.fromEntries(Object.entries(results).filter(([k, v]) => v && typeof v === 'object').map(([k, v]) => [k, {median: v.median, p95: v.p95, ph: v.ph, cuts: v.cuts}]))));
await cleanup(ok ? 0 : 1);
