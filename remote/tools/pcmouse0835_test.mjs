// 0.8.3.5 遠端控制 from another PC's browser (node 22+): mouse, keyboard, modes, toolbar, latency.
//   node remote/tools/pcmouse0835_test.mjs <pm_remote_spike.exe> <outDir> [--only a,b,..] [--before] [--shots <dir>] [--lang N]
// The "PC browser" is ONE chrome-headless-shell (quiet_browser.mjs: no window, no Alt+Tab entry, never the
// focus), desktop viewport (no touch), its own throw-away profile; mouse / keys are CDP Input.dispatch*
// events INTO THAT PAGE only (nothing reaches Windows). The spike runs --loopback (127.0.0.3: no firewall
// prompt), --no-tailscale, --no-audio, --scene static, with --inject-stub: the whole injection path runs but
// SendInput is a stub that only logs (never a real mouse / key on this PC). --before: the 0.8.3.4 spike
// (dry-run: it has no stub), to show what the old page sent. A watcher counts Alt+Tab / taskbar windows of
// every process the test starts (must stay 0).
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import {launch, cdpClient, watchAltTab} from './quiet_browser.mjs';

const [spikeExe, outDir, ...rest] = process.argv.slice(2);
if (!spikeExe || !outDir) { console.error('usage: node pcmouse0835_test.mjs <pm_remote_spike.exe> <outDir> [--only a,b] [--before]'); process.exit(2); }
let only = null, before = false, shotsDir = null, lang = 0, spikeExtra = [];
for (let i = 0; i < rest.length; i++) {
  if (rest[i] === '--only') only = new Set(rest[++i].split(','));
  else if (rest[i] === '--before') before = true;
  else if (rest[i] === '--shots') shotsDir = rest[++i];
  else if (rest[i] === '--lang') lang = +rest[++i];
  else spikeExtra.push(rest[i]);
}
fs.mkdirSync(outDir, {recursive: true});
if (shotsDir) fs.mkdirSync(shotsDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const results = [];
let failed = 0;
const check = (name, ok, detail = '') => { results.push({name, ok: !!ok, detail}); if (!ok) failed++; console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail !== '' ? '  -- ' + detail : ''}`); };
const want = k => !only || only.has(k);

const profileDir = path.join(os.tmpdir(), 'shell-0835-pc-' + crypto.randomBytes(3).toString('hex'));
const br = await launch(profileDir, ['--allow-loopback-in-peer-connection']);
const watch = watchAltTab([br.proc.pid, process.pid], path.join(outDir, 'alttab_pc.txt'));
const C = await cdpClient(br.ver.webSocketDebuggerUrl);
const cdp = C.cdp;

const token = crypto.randomBytes(16).toString('hex');
const port = 19300 + Math.floor(Math.random() * 300);
const logFile = path.join(outDir, before ? 'spike_before.log' : 'spike.log');
fs.writeFileSync(logFile, '');
const readLog = () => fs.readFileSync(logFile, 'utf8');
const spikeArgs = ['--token', token, '--port', String(port), '--log', logFile, '--duration', '400', '--stdin-quit', '--loopback', '--no-tailscale',
  '--scene', 'static', '--no-audio', '--test-clipboard', 'on this PC before', '--lang', String(lang), ...(before ? [] : ['--inject-stub']), ...spikeExtra];
const spike = spawn(spikeExe, spikeArgs, {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
watch.add(spike.pid);
spike.stdout.on('data', () => {}); spike.stderr.on('data', () => {});
let url = '';
for (let i = 0; i < 150 && !url; i++) { await sleep(100); const m = readLog().match(/urls: lan (http:\/\/127\.0\.0\.3:\d+\/[0-9a-f]+\/)/); if (m) url = m[1]; }
check('spike up on 127.0.0.3 (loopback only)', !!url, url);
const waitFor = async (fn, ms) => { for (let t = 0; t < ms; t += 100) { const v = await fn(); if (v) return v; await sleep(100); } return null; };
const count = re => (readLog().match(re) || []).length;
const since = mark => readLog().slice(mark);

const W = 1280, H = 720;
const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
const {sessionId} = await cdp('Target.attachToTarget', {targetId, flatten: true});
const s = (m, p) => cdp(m, p, sessionId);
await s('Emulation.setDeviceMetricsOverride', {width: W, height: H, deviceScaleFactor: 1, mobile: false});
// This PC's real clipboard is never touched: the page's writes go to window.__wrote (before any page script).
await s('Page.enable', {});
await s('Page.addScriptToEvaluateOnNewDocument', {source: `(() => {
  window.__wrote = [];
  try { Object.defineProperty(navigator, 'clipboard', {configurable: true, value: {writeText: async s => { window.__wrote.push(s); }, readText: async () => '', write: async () => {}, read: async () => []}}); } catch {}
  const ex = Document.prototype.execCommand;
  Document.prototype.execCommand = function (c, ...a) { if (/^(copy|cut|paste)$/i.test(c)) { window.__wrote.push('execCommand:' + c); return true; } return ex.call(this, c, ...a); };
})();`});
await s('Page.navigate', {url});
const ev = async (expression, awaitPromise = false) => { const r = await s('Runtime.evaluate', {expression, returnByValue: true, awaitPromise}); return r && r.result ? r.result.value : undefined; };
const frame = await waitFor(() => ev('window.__pmFirstFrame ? 1 : 0'), 25000);
check('PC browser: first picture', !!frame);
await waitFor(() => ev(`(typeof dc !== 'undefined' && dc && dc.readyState === 'open') ? 1 : 0`), 10000);
await sleep(800);
// every message the page sends (counted / read by the checks)
await ev(`(() => { const o = dc.send.bind(dc); window.__sent = []; dc.send = m => { if (__sent.length < 20000) __sent.push([performance.now(), m]); return o(m); }; return 1; })()`);
const box =await ev(`(() => { const b = videoBox(); return {l: b.r.left + (b.r.width - b.w) / 2, t: b.r.top + (b.r.height - b.h) / 2, w: b.w, h: b.h, pw: picW(), ph: picH(), dw: S.srv && S.srv.deskW, dh: S.srv && S.srv.deskH}; })()`);
console.log('picture box', JSON.stringify(box));
// CDP mouse helpers (events into the page only)
const BTN = {left: 1, middle: 4, right: 2, back: 8, forward: 16};
let held = 0;
const mouse = async (type, x, y, button = 'none', extra = {}) => {
  if (type === 'mousePressed') held |= BTN[button] || 0;
  await s('Input.dispatchMouseEvent', {type, x, y, button, buttons: held, clickCount: extra.clickCount || 1, pointerType: 'mouse', ...extra});
  if (type === 'mouseReleased') held &= ~(BTN[button] || 0);
};
const at = (fx, fy) => [box.l + fx * box.w, box.t + fy * box.h];
const click = async (fx, fy, button = 'left', n = 1) => { const [x, y] = at(fx, fy); await mouse('mouseMoved', x, y); await mouse('mousePressed', x, y, button, {clickCount: n}); await mouse('mouseReleased', x, y, button, {clickCount: n}); };
const ctx = {elevated: spikeExtra.includes('--inject-stub-elevated'), s, ev, check, readLog, count, since, waitFor, mouse, at, click, box, sleep, before, shotsDir, lang, outDir, W, H};

// Screenshots never show this PC's real screen (the spike's "static" scene is a frozen frame of the real
// desktop): the picture is covered by a made-up desktop of the same size and place while the shot is taken.
const shot = async name => {
  if (!shotsDir) return;
  await ev(`(() => { const b = videoBox(); let m = document.getElementById('__mock'); if (!m) { m = document.createElement('div'); m.id = '__mock'; document.body.appendChild(m); }
    const z = trackpad.zoom, cx = b.r.left + b.r.width / 2 + trackpad.panX, cy = b.r.top + b.r.height / 2 + trackpad.panY;
    Object.assign(m.style, {position: 'absolute', zIndex: 1, pointerEvents: 'none', left: (cx - b.w * z / 2) + 'px', top: (cy - b.h * z / 2) + 'px', width: b.w * z + 'px', height: b.h * z + 'px',
      background: 'linear-gradient(135deg,#3a2a30,#5a3f46 45%,#2c2226)', boxShadow: 'inset 0 -' + Math.max(8, b.h * z * 0.035) + 'px 0 #241a1d'});
    m.innerHTML = '<div style="position:absolute;left:8%;top:12%;width:46%;height:56%;background:#f6e9ea;border-radius:6px;box-shadow:0 6px 20px rgba(0,0,0,.4)"><div style="height:12%;background:#e9d3d6;border-radius:6px 6px 0 0"></div></div><div style="position:absolute;left:60%;top:22%;width:30%;height:44%;background:#fbf4f4;border-radius:6px;opacity:.92"></div>';
    $('v').style.visibility = 'hidden'; $('scr').style.visibility = 'hidden'; return 1; })()`);
  await sleep(150);
  const r = await s('Page.captureScreenshot', {format: 'png'});
  await ev(`(() => { const m = document.getElementById('__mock'); if (m) m.remove(); $('v').style.visibility = ''; $('scr').style.visibility = ''; return 1; })()`);
  if (r && r.data) fs.writeFileSync(path.join(shotsDir, name + '.png'), Buffer.from(r.data, 'base64'));
};
ctx.shot = shot;

// ---- ① a real mouse: left / right click ----
if (want('1')) {
  const m0 = readLog().length;
  await click(0.5, 0.5, 'left');
  await click(0.25, 0.75, 'right');
  await sleep(800);
  const L = since(m0);
  const n = re => (L.match(re) || []).length;
  console.log('① log:\n' + L.split('\n').filter(l => /input/.test(l)).slice(0, 12).join('\n'));
  check('① left button: down + up reach the PC', n(/: left down/g) >= 1 && n(/: left up/g) >= 1, `down ${n(/: left down/g)} up ${n(/: left up/g)}`);
  check('① right button: down + up reach the PC', n(/: right down/g) >= 1 && n(/: right up/g) >= 1, `down ${n(/: right down/g)} up ${n(/: right up/g)}`);
  check('① the pointer is placed before the click', /pointer placed|pointer to/.test(L));
}

const extra = path.join(path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1')), 'pcmouse0835_more.mjs');
if (fs.existsSync(extra)) { const mod = await import('file:///' + extra.replace(/\\/g, '/')); await mod.run(ctx, want); }

await shot('final');
try { await s('Target.closeTarget', {targetId}); } catch {}
C.close();
await br.close();
try { spike.stdin.end(); } catch {}
for (let i = 0; i < 80 && spike.exitCode === null; i++) await sleep(100);
if (spike.exitCode === null) try { execFileSync('taskkill', ['/PID', String(spike.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
await sleep(300);
try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
const L = readLog();
if (!before) check('no real SendInput: every injection went to the stub', !/input: SendInput|SendInput failed/.test(L.replace(/STUB SendInput/g, '')) && /--inject-stub|input stub/.test(L), '');
const alt = await watch.stop();
check(`Alt+Tab / taskbar windows of the test's processes = 0 (${alt.samples} samples)`, alt.max === 0 && alt.samples > 0, `max ${alt.max}${alt.seen.length ? ': ' + alt.seen.join(' ; ') : ''}`);
fs.writeFileSync(path.join(outDir, before ? 'pcmouse_before.json' : 'pcmouse.json'), JSON.stringify(results, null, 1));
console.log(failed ? `FAILED (${failed})` : `ALL PASSED (${results.length})`);
process.exit(failed ? 1 : 0);
