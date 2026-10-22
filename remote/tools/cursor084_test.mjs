// 0.8.4 游標 before / after (node 22+): is the PC pointer seen on the phone, with its shape (arrow, I-beam,
// hand, busy), in 直接點 and 觸控板; is it in the picture (burned in) or drawn by the page; what a pointer moving
// over a still picture costs. A SYNTHETIC still desktop (--scene synth-still: never this PC's screen) and the
// stub pointer (--inject-stub: nothing is injected, the real mouse never moves) taking Windows' own shapes in
// turn (--test-cursor-cycle). One chrome-headless-shell (quiet_browser.mjs: no window, no Alt+Tab entry) as
// an iPhone-sized phone (852x393, devicePixelRatio 3, touch), closed at the end. No sound (--no-audio).
//   node remote/tools/cursor084_test.mjs <pm_remote_spike.exe> <outDir> [--label before|after] [--no-shots]
// Screenshots: <outDir>/cursor-<label>-<mode>-<shape>.png (the whole phone screen) and ...-zoom.png (around
// the pointer, 3x). Results: <outDir>/cursor-<label>.json. Exit code 0 = ran (checks: see "checks").
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import {launch, cdpClient, watchAltTab} from './quiet_browser.mjs';

const [spikeExe, outDir, ...rest] = process.argv.slice(2);
if (!spikeExe || !outDir) { console.error('usage: node cursor084_test.mjs <spike.exe> <outDir> [--label NAME] [--no-shots]'); process.exit(2); }
let label = 'after', shots = true;
for (let i = 0; i < rest.length; i++) { if (rest[i] === '--label') label = rest[++i]; else if (rest[i] === '--no-shots') shots = false; }
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const PHONE = {width: 852, height: 393, deviceScaleFactor: 3, mobile: true};
const P = {x: 0.42, y: 0.45};  // where the pointer is for the pictures (0..1 of the PC screen)

const work = fs.mkdtempSync(path.join(os.tmpdir(), 'pm-cur084-'));
const logFile = path.join(work, 'spike.log');
fs.writeFileSync(logFile, '');
const readLog = () => fs.readFileSync(logFile, 'utf8');
const token = crypto.randomBytes(16).toString('hex'), port = 19800 + Math.floor(Math.random() * 60);
const spike = spawn(spikeExe, ['--token', token, '--port', String(port), '--log', logFile, '--duration', '400', '--stdin-quit', '--loopback', '--no-tailscale',
  '--no-audio', '--fixed-fps', '--inject-stub', '--test-cursor-cycle', '--scene', 'synth-still', '--quality', '2'], {stdio: ['pipe', 'ignore', 'ignore'], windowsHide: true});
const br = await launch(path.join(work, '_shell'), ['--allow-loopback-in-peer-connection', '--autoplay-policy=no-user-gesture-required']);
const watch = watchAltTab([br.proc.pid, process.pid, spike.pid], path.join(work, 'alttab.txt'));
const C = await cdpClient(br.ver.webSocketDebuggerUrl);
const R = {label, when: new Date().toISOString(), phone: PHONE, shots: [], checks: []};
const check = (ok, what, detail) => { R.checks.push({ok: !!ok, what, detail}); console.log((ok ? 'PASS ' : 'FAIL ') + what + (detail !== undefined ? '  -- ' + JSON.stringify(detail) : '')); };
let targetId;
try {
  let url = '';
  for (let i = 0; i < 150 && !url; i++) { await sleep(100); const m = readLog().match(/urls: lan (http:\/\/127\.0\.0\.3:\d+\/[0-9a-f]+\/)/); if (m) url = m[1]; }
  if (!url) throw new Error('spike did not start');
  ({targetId} = await C.cdp('Target.createTarget', {url: 'about:blank'}));
  const {sessionId} = await C.cdp('Target.attachToTarget', {targetId, flatten: true});
  const s = (m, p) => C.cdp(m, p, sessionId);
  const ev = async (expression, awaitPromise = false) => { const x = await s('Runtime.evaluate', {expression, returnByValue: true, awaitPromise}); return x && x.result ? x.result.value : (x && x.exceptionDetails ? {err: x.exceptionDetails.text} : undefined); };
  await s('Emulation.setDeviceMetricsOverride', PHONE);
  await s('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5});
  await s('Page.enable', {});
  await s('Page.addScriptToEvaluateOnNewDocument', {source: `try { localStorage.setItem('pm.modeHint0835', '1'); localStorage.setItem('pm.q', '2'); localStorage.setItem('pm.q084', '2'); } catch {}`});
  await s('Page.navigate', {url});
  for (let i = 0; i < 200 && !(await ev('window.__pmFirstFrame ? 1 : 0')); i++) await sleep(100);
  if (!(await ev('window.__pmFirstFrame ? 1 : 0'))) throw new Error('no picture');
  // what the PC says about its pointer (kind / shape id), seen before the page handles it
  await ev(`(() => { const d = dc, o = d.onmessage; window.__cur = {n: 0, kinds: {}, shapes: 0, last: null};
    d.onmessage = e => { try { const m = JSON.parse(e.data); if (m.t === 'cur') { const c = window.__cur; c.n++; c.last = m; c.kinds[m.k] = (c.kinds[m.k] || 0) + 1; }
      if (m.t === 'cshape') window.__cur.shapes++; } catch {} o.call(d, e); }; return 1; })()`);
  const moveTo = async (x, y) => { await ev(`send({t: 'abs', x: ${x}, y: ${y}}); 1`); await sleep(250); };
  await moveTo(P.x, P.y);
  await sleep(800);
  // ---- is the pointer in the picture? the video's own pixels around it, with it there and away ----
  const grab = async () => ev(`(() => { const w = v.videoWidth, h = v.videoHeight; if (!w) return null;
    const cx = Math.round(${P.x} * w), cy = Math.round(${P.y} * h), r = Math.round(w * 0.02);
    const c = document.createElement('canvas'); c.width = 2 * r; c.height = 2 * r; const g = c.getContext('2d');
    g.drawImage(v, cx - r, cy - r, 2 * r, 2 * r, 0, 0, 2 * r, 2 * r); return Array.from(g.getImageData(0, 0, 2 * r, 2 * r).data); })()`);
  const at = await grab();
  await moveTo(0.9, 0.9);
  await sleep(600);
  const away = await grab();
  await moveTo(P.x, P.y);
  let diff = NaN;
  if (at && away && at.length === away.length) { let d = 0; for (let i = 0; i < at.length; i += 4) d += Math.abs(at[i] - away[i]) + Math.abs(at[i + 1] - away[i + 1]) + Math.abs(at[i + 2] - away[i + 2]); diff = +(d / (at.length / 4) / 3).toFixed(2); }
  R.pictureDiff = diff;  // mean |difference| per channel of the video around the pointer (0 = the pointer is not in the picture)
  R.pointerInPicture = diff > 1.5;
  console.log('pointer in the picture:', R.pointerInPicture, 'diff', diff);
  // ---- the pictures: 直接點 and 觸控板, each shape ----
  for (const mode of ['direct', 'trackpad']) {
    await ev(`setMode('${mode}'); closeSheets && closeSheets(); 1`);
    await sleep(300);
    for (const kind of ['arrow', 'ibeam', 'hand', 'wait']) {
      for (let i = 0; i < 60 && (await ev('window.__cur.last && window.__cur.last.k')) !== kind; i++) await sleep(100);
      await sleep(350);  // the frame / the overlay after the shape change
      const info = await ev(`(() => { const p = normToClient({x: ${P.x}, y: ${P.y}}); const q = id => { const e = document.getElementById(id); if (!e) return null;
          const st = getComputedStyle(e), r = e.getBoundingClientRect(); return {display: st.display, x: +r.left.toFixed(1), y: +r.top.toFixed(1), w: +r.width.toFixed(1), h: +r.height.toFixed(1), src: e.tagName === 'IMG' ? (e.src || '').slice(0, 40) : undefined}; };
        return {p: {x: +p.x.toFixed(1), y: +p.y.toFixed(1)}, kind: window.__cur.last && window.__cur.last.k, shape: window.__cur.last && window.__cur.last.s,
          tcur: q('tcur'), pcurC: q('pcurC'), pcurI: q('pcurI'), hot: typeof pcurHot === 'function' ? pcurHot() : null, sync: S.curSync || null}; })()`);
      const shot = {mode, kind, info};
      if (shots) {
        const base = path.join(outDir, `cursor-${label}-${mode}-${kind}`);
        const full = await s('Page.captureScreenshot', {format: 'png'});
        fs.writeFileSync(base + '.png', Buffer.from(full.data, 'base64'));
        const cw = 220, ch = 130;
        const clip = {x: Math.max(0, info.p.x - cw / 2), y: Math.max(0, info.p.y - ch / 2), width: cw, height: ch, scale: 1};
        const z = await s('Page.captureScreenshot', {format: 'png', clip});
        fs.writeFileSync(base + '-zoom.png', Buffer.from(z.data, 'base64'));
        shot.files = [base + '.png', base + '-zoom.png'];
      }
      R.shots.push(shot);
      console.log(mode, kind, JSON.stringify(info));
    }
  }
  // ---- a pointer moving over a still picture: what it costs (the PC's 5-second stats lines) ----
  await ev(`setMode('direct'); 1`);
  await sleep(1500);
  const mark = readLog().length;
  await ev(`(() => { let t = 0; window.__mv = setInterval(() => { t += 0.05; send({t: 'abs', x: 0.5 + 0.3 * Math.cos(t), y: 0.5 + 0.3 * Math.sin(t)}); }, 16); return 1; })()`);
  await sleep(11000);
  await ev(`clearInterval(window.__mv); 1`);
  const st = [...readLog().slice(mark).matchAll(/stats: (\d+)x(\d+) ([\d.]+) fps sent, (\d+) kbps.*?CPU ([\d.]+)% of machine \(([\d.]+)% of one core\)/g)]
    .map(m => ({fps: +m[3], kbps: +m[4], cpuCore: +m[6]}));
  R.pointerOnly = st;
  console.log('pointer moving over a still picture:', JSON.stringify(st));
  R.hostCursorLines = (readLog().match(/^.*(pointer |cursor ).*$/gm) || []).filter(l => !/page-report|page-diag/.test(l)).slice(0, 8);
  R.curStats = await ev('window.__cur');
  R.shapesKept = await ev(`typeof shapes === 'object' && shapes instanceof Map ? shapes.size : 0`);
  // checks (what 0.8.4 promises; "before" is expected to fail some: that is the point)
  const sh = R.shots;
  check(!R.pointerInPicture, 'the pointer is not burned into the picture', {diff});
  check(sh.filter(x => x.mode === 'direct').every(x => x.info.pcurC && x.info.pcurC.display !== 'none'), '直接點: the PC pointer is drawn over the picture for every shape');
  check(new Set(sh.filter(x => x.mode === 'direct').map(x => x.info.shape)).size === 4, '4 shapes, 4 different pictures (the shape follows the PC)', sh.map(x => x.info.shape));
  check(sh.filter(x => x.mode === 'trackpad').every(x => x.info.tcur && x.info.tcur.display !== 'none' && x.info.pcurC && x.info.pcurC.display !== 'none'), '觸控板: the ring and the PC pointer\'s own shape');
  check(sh.every(x => !x.info.hot || (Math.abs(x.info.hot.x - x.info.p.x) <= 1.5 && Math.abs(x.info.hot.y - x.info.p.y) <= 1.5)), 'the hot spot is where the PC pointer is (±1.5 CSS px)',
    sh.map(x => x.info.hot && [x.mode, x.kind, +(x.info.hot.x - x.info.p.x).toFixed(1), +(x.info.hot.y - x.info.p.y).toFixed(1)]));
  check(sh.filter(x => x.mode === 'direct').every(x => x.info.pcurC && x.info.pcurC.h >= 18), '直接點: big enough to see on a phone (>= 18 CSS px)', sh.map(x => x.info.pcurC && x.info.pcurC.h));
  check(R.shapesKept >= 4 && R.curStats && R.curStats.shapes <= 4 && R.curStats.n > R.curStats.shapes * 10, 'each shape sent once (not with every move)',
    {shapesOnThePage: R.shapesKept, shapeMessagesSeen: R.curStats && R.curStats.shapes, curMessages: R.curStats && R.curStats.n});
  const pm = R.pointerOnly.length ? R.pointerOnly[R.pointerOnly.length - 1] : null;
  check(pm && pm.fps < 5, 'a pointer moving over a still picture encodes (almost) no frames', pm);
} catch (e) { R.error = String(e && e.message || e); console.log('ERROR', R.error); }
try { if (targetId) await C.cdp('Target.closeTarget', {targetId}); } catch {}
C.close();
await br.close();
try { spike.stdin.end(); } catch {}
for (let i = 0; i < 50 && spike.exitCode === null; i++) await sleep(100);
if (spike.exitCode === null) try { execFileSync('taskkill', ['/PID', String(spike.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
const alt = await watch.stop();
R.altTabMax = alt.max, R.altTabSamples = alt.samples;
fs.writeFileSync(path.join(outDir, `cursor-${label}.json`), JSON.stringify(R, null, 1));
if (process.env.KEEP_BENCH) console.log('log kept in', work); else try { fs.rmSync(work, {recursive: true, force: true}); } catch {}
console.log(`checks: ${R.checks.filter(c => c.ok).length} passed, ${R.checks.filter(c => !c.ok).length} failed; Alt+Tab / taskbar windows: max ${alt.max} over ${alt.samples} samples`);
process.exit(R.error || alt.max ? 1 : 0);
