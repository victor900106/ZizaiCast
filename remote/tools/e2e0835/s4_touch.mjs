// 0.8.3.5 e2e ④: 手機觸控板模式 — big arrow + pink ring, the first-time mode card (the released app, --dev).
//   node remote/tools/e2e0835/s4_touch.mjs <自在投影.exe> <outDir>
// The phone = ONE chrome-headless-shell tab: 390x844, DPR 3, mobile, touch emulation (CDP
// Input.dispatchTouchEvent; no mouse events). It logs in with a 臨時帳號 (可操作) on the fixed address,
// like the owner's phone. Screenshots: the PC picture is covered by a made-up desktop; the pointer is checked
// in the screenshot's own pixels (white arrow + pink ring counted in a crop around it).
import fs from 'node:fs';
import path from 'node:path';
import {startApp, startBrowser, results, sleep, connectedJs} from './lib.mjs';

const [exe, outDir] = process.argv.slice(2);
if (!exe || !outDir) { console.error('usage: node s4_touch.mjs <自在投影.exe> <outDir>'); process.exit(2); }
const R = results(outDir);
const {check} = R;
const ev4 = {};
const A = await startApp(exe, outDir, {tag: 'touch'});
check('app up, remote on, fixed address (made up) ready', !!A.lanUrl && A.fixedReady);
const B = await startBrowser(outDir, 'touch');
B.watch.add(A.pid);
const T = await B.tab({w: 390, h: 844, dpr: 3, mobile: true, touch: true});
const {s, ev, waitFor} = T;
const tap = async (x, y, hold = 60) => { await s('Input.dispatchTouchEvent', {type: 'touchStart', touchPoints: [{x, y}]}); await sleep(hold); await s('Input.dispatchTouchEvent', {type: 'touchEnd', touchPoints: []}); };
const swipe = async (x0, y0, x1, y1, steps = 14) => {
  await s('Input.dispatchTouchEvent', {type: 'touchStart', touchPoints: [{x: x0, y: y0}]});
  for (let i = 1; i <= steps; i++) { await s('Input.dispatchTouchEvent', {type: 'touchMove', touchPoints: [{x: x0 + (x1 - x0) * i / steps, y: y0 + (y1 - y0) * i / steps}]}); await sleep(16); }
  await s('Input.dispatchTouchEvent', {type: 'touchEnd', touchPoints: []});
};
const geo = () => ev(`(() => { const b = videoBox(); const z = trackpad.zoom; const cx = b.r.left + b.r.width / 2 + trackpad.panX, cy = b.r.top + b.r.height / 2 + trackpad.panY;
  return {l: cx - b.w * z / 2, t: cy - b.h * z / 2, w: b.w * z, h: b.h * z, dw: deskW(), dh: deskH()}; })()`);
const logAfter = async (fn, ms = 700) => { const m0 = A.readLog().length; await fn(); await sleep(ms); return A.readLog().slice(m0); };
// pixels of a PNG crop, counted in a scratch tab (canvas): white-ish, pink-ish (the ring), dark (the outline)
async function countPixels(pngB64) {
  const {targetId} = await B.cdp('Target.createTarget', {url: 'about:blank'});
  const {sessionId} = await B.cdp('Target.attachToTarget', {targetId, flatten: true});
  const r = await B.cdp('Runtime.evaluate', {expression: `(async () => { const im = new Image(); im.src = 'data:image/png;base64,${pngB64}'; await im.decode();
    const c = document.createElement('canvas'); c.width = im.width; c.height = im.height; const x = c.getContext('2d'); x.drawImage(im, 0, 0);
    const d = x.getImageData(0, 0, c.width, c.height).data; let white = 0, pink = 0, dark = 0;
    for (let i = 0; i < d.length; i += 4) { const [R, G, B] = [d[i], d[i + 1], d[i + 2]];
      if (R > 235 && G > 235 && B > 235) white++; else if (R > 200 && G < 170 && B > 120 && R - G > 60) pink++; else if (R < 50 && G < 50 && B < 50) dark++; }
    return {w: c.width, h: c.height, white, pink, dark}; })()`, awaitPromise: true, returnByValue: true}, sessionId);
  await B.cdp('Target.closeTarget', {targetId});
  return r && r.result ? r.result.value : null;
}
try {
  const G = await A.makeGuest(1 | 8);
  check('臨時帳號 (可操作) made in the window', !!(G.name && G.pw), G.name);
  await B.cdp('Storage.clearDataForOrigin', {origin: A.fixedOrigin, storageTypes: 'local_storage,session_storage'});
  await T.nav(`${A.fixedOrigin}/#u=${G.name}`);
  await waitFor(() => ev(`(typeof $ === 'function' && $('pass') && $('pass').style.display === 'flex') ? 1 : 0`), 12000);
  await ev(`(() => { if ($('userIn')) $('userIn').value = ${JSON.stringify(G.name)}; $('passIn').value = ${JSON.stringify(G.pw)}; submitPass(); return 1; })()`);
  check('phone: 允許 on the PC', await A.answer(1, 15000));
  check('phone: connected, picture there', !!(await waitFor(() => ev(connectedJs), 30000)));
  await sleep(1500);
  // ① a new phone: 直接點 by default + the first-time card
  const st = await ev(`({mode: prefs.mode, hint: $('modeHint').style.display, why: S.modeHint, t: $('mhT').textContent, s: $('mhS').textContent, btn: $('mhSwitch').textContent, ok: ($('mhOk') || {}).textContent, touch: matchMedia('(pointer: coarse)').matches})`);
  ev4.firstCard = st;
  check('首次模式提示: shown at the first connection (says the mode, how a tap works, a switch button)', st.hint === 'block' && st.why === 'first' && !!st.t && !!st.btn, `${st.mode} | ${st.t} | ${st.s} | ${st.btn} | ${st.ok}`);
  check('a new phone starts in 直接點', st.mode === 'direct', st.mode);
  await T.shot(path.join(outDir, 's4_first_mode_card.png'));
  // ② 改用觸控板 from the card (one tap, real touch on the button)
  const r = await ev(`(() => { const b = $('mhSwitch').getBoundingClientRect(); return {x: b.left + b.width / 2, y: b.top + b.height / 2}; })()`);
  await tap(r.x, r.y);
  await sleep(600);
  const st2 = await ev(`({mode: prefs.mode, saved: localStorage.getItem('pm.mode083'), hint: $('modeHint').style.display, toast: $('toast').style.display === 'block' ? $('toast').textContent : ''})`);
  check('改用觸控板 (tap on the card): mode 觸控板, remembered, card closed', st2.mode === 'trackpad' && JSON.parse(st2.saved) === 'trackpad' && st2.hint !== 'block', JSON.stringify(st2));
  // ③ move the pointer with a swipe; the big arrow in the pink ring where the PC pointer is
  const g = await geo();
  ev4.geometry = g;
  await swipe(g.l + g.w * 0.3, g.t + g.h * 0.5, g.l + g.w * 0.5, g.t + g.h * 0.55);
  await sleep(900);
  const cur = await ev(`(() => { const e = $('tcur'), r = e.getBoundingClientRect(), c = cursorClient(); return {shown: getComputedStyle(e).display, vis: getComputedStyle(e).visibility, z: getComputedStyle(e).zIndex, w: r.width, h: r.height, l: r.left, t: r.top, cx: c && c.x, cy: c && c.y, srv: typeof srvCur !== 'undefined' ? srvCur : null}; })()`);
  ev4.cursor = cur;
  check('觸控板: the PC pointer drawn big (element #tcur shown, >= 30 CSS px) at the PC pointer', cur.shown !== 'none' && cur.w >= 30 && Math.abs(cur.l + 13 - cur.cx) < 3 && Math.abs(cur.t + 13 - cur.cy) < 3, JSON.stringify(cur));
  await T.shot(path.join(outDir, 's4_trackpad_pointer.png'));
  // the pixels: a crop around the pointer with the made-up desktop under it (what the person sees)
  await ev(`(() => { const b = videoBox(); let m = document.getElementById('__mock'); if (!m) { m = document.createElement('div'); m.id = '__mock'; document.body.appendChild(m); }
    const z = trackpad.zoom, cx = b.r.left + b.r.width / 2 + trackpad.panX, cy = b.r.top + b.r.height / 2 + trackpad.panY;
    Object.assign(m.style, {position: 'absolute', zIndex: 1, pointerEvents: 'none', left: (cx - b.w * z / 2) + 'px', top: (cy - b.h * z / 2) + 'px', width: b.w * z + 'px', height: b.h * z + 'px', background: '#3a2a30'});
    $('v').style.visibility = 'hidden'; return 1; })()`);
  await sleep(200);
  const crop = await s('Page.captureScreenshot', {format: 'png', clip: {x: Math.max(0, cur.l - 20), y: Math.max(0, cur.t - 20), width: cur.w + 40, height: cur.h + 40, scale: 3}});
  await ev(`(() => { const m = document.getElementById('__mock'); if (m) m.remove(); $('v').style.visibility = ''; return 1; })()`);
  fs.writeFileSync(path.join(outDir, 's4_trackpad_pointer_crop.png'), Buffer.from(crop.data, 'base64'));
  const px = await countPixels(crop.data);
  ev4.pixels = px;
  // 0.8.4 (feat-084-quality): the page draws the PC's own cursor (its shape from the PC: #pcurC, at its hotspot) in the
  // pink ring; the white arrow is only the fallback when the PC sent no shape (an older PC).
  const pcur = await ev(`(() => { const e = $('pcurC'), r = e.getBoundingClientRect(); return {shown: getComputedStyle(e).display !== 'none', img: !!(e.src && e.naturalWidth), w: Math.round(r.width), h: Math.round(r.height)}; })()`);
  ev4.pcCursor = pcur;
  check('截圖像素: pink ring + dark outline in the crop, and the pointer drawn in it (0.8.4: the PC cursor image; else the white arrow)',
        px && px.pink > 300 && px.dark > 50 && ((pcur.shown && pcur.img && pcur.w >= 20) || px.white > 300), JSON.stringify({px, pcur}));
  // ④ a tap in 觸控板 clicks where the arrow is (the stub's pointer), not under the finger
  const stubPos = () => { const m = [...A.readLog().matchAll(/STUB SendInput mouse [^\n]*LEFTDOWN[^\n]* at (-?\d+),(-?\d+)/g)].pop(); return m ? [+m[1], +m[2]] : null; };
  const c1 = await ev('cursorClient()');
  const L = await logAfter(() => tap(g.l + g.w * 0.8, g.t + g.h * 0.2), 900);
  const host = L.match(/: left (?:click|down) at (-?\d+),(-?\d+)/);
  const expArrow = [Math.floor((c1.x - g.l) / g.w * g.dw), Math.floor((c1.y - g.t) / g.h * g.dh)];
  const tol = Math.ceil(g.dw / g.w) * 2 + 2;
  ev4.tap = {host: host && host[0], stub: stubPos(), arrowAt: expArrow, finger: [Math.floor(0.8 * g.dw), Math.floor(0.2 * g.dh)]};
  check('觸控板 tap far from the arrow: the click lands at the arrow (PC pointer), as the arrow shows', host && Math.abs(+host[1] - expArrow[0]) <= tol && Math.abs(+host[2] - expArrow[1]) <= tol, JSON.stringify(ev4.tap));
  // ⑤ two more far taps (three within 12 s, > 90 px from the pointer): the card explains, offers 改用直接點
  await sleep(450); await tap(g.l + g.w * 0.82, g.t + g.h * 0.25); await sleep(450); await tap(g.l + g.w * 0.78, g.t + g.h * 0.15); await sleep(700);
  const st3 = await ev(`({hint: $('modeHint').style.display, why: S.modeHint, t: $('mhT').textContent, btn: $('mhSwitch').textContent})`);
  ev4.missCard = st3;
  check('三次點離箭頭很遠 -> 「點擊會落在箭頭的位置」 card with 改用直接點', st3.hint === 'block' && st3.why === 'miss', JSON.stringify(st3));
  await T.shot(path.join(outDir, 's4_trackpad_miss_card.png'));
  // ⑥ the first-time card does not come back for this phone (remembered), after a reload
  await ev(`(() => { $('mhOk') ? $('mhOk').click() : ($('modeHint').style.display = 'none'); return 1; })()`);
  await ev('(location.reload(), 1)');
  await sleep(1500);
  await waitFor(() => ev(`(typeof $ === 'function' && $('pass') && $('pass').style.display === 'flex') ? 1 : 0`), 10000);
  await ev(`(() => { $('passIn').value = ${JSON.stringify(G.pw)}; submitPass(); return 1; })()`);
  await waitFor(() => ev(connectedJs), 25000);
  await sleep(1500);
  const st4 = await ev(`({hint: $('modeHint').style.display, mode: prefs.mode, seen: localStorage.getItem('pm.modeHint0835')})`);
  check('reload: no first-time card again (remembered), 觸控板 kept, no 允許 again', st4.hint !== 'block' && st4.mode === 'trackpad' && A.asks() === 1, JSON.stringify(st4) + ` asks ${A.asks()}`);
} catch (e) { check('s4 ran without an exception', false, String(e && e.stack || e)); }
try { await T.close(); } catch {}
const alt = await B.close();
const st = await A.stop();
check('app quit', !st.killed, JSON.stringify(st));
check(`Alt+Tab / taskbar windows of the test's processes = 0 (${alt.samples} samples)`, alt.samples > 0 && alt.max === 0, alt.seen.join(' ; '));
check('no real SendInput in the log', !/input: SendInput\(/.test(A.readLog()) && /STUB SendInput/.test(A.readLog()));
R.save('s4_touch.json', {evidence: ev4});
console.log(R.failed() ? `FAILED (${R.failed()})` : 'ALL PASSED');
process.exit(R.failed() ? 1 : 0);
