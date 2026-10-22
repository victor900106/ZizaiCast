// 0.8.3.5 e2e ①: 「另一台電腦」的瀏覽器控制 — the released app (--dev instance, input stub) and ONE
// chrome-headless-shell as the second PC (desktop viewport, mouse, no touch). The PC browser opens the
// 本機網址 (LAN, 127.0.0.3 loopback), the PC answers 允許 (DevCommand 961), then CDP mouse / wheel / key /
// IME events go INTO THAT PAGE only; the host's log (stub: "STUB SendInput ...", never a real SendInput)
// shows what the PC would have done, and where.
//   node remote/tools/e2e0835/s1_input.mjs <自在投影.exe> <outDir>
import fs from 'node:fs';
import path from 'node:path';
import {startApp, startBrowser, results, sleep, connectedJs} from './lib.mjs';

const [exe, outDir] = process.argv.slice(2);
if (!exe || !outDir) { console.error('usage: node s1_input.mjs <自在投影.exe> <outDir>'); process.exit(2); }
const R = results(outDir);
const {check} = R;
const A = await startApp(exe, outDir, {tag: 'input'});
check('app up: remote on (loopback LAN URL), fixed address made up (no tailscale)', !!A.lanUrl && A.fixedReady, A.lanUrl.replace(/\/\w{20,}\//, '/<token>/'));
check('app log: input stub (the injection path runs, SendInput only logged)', /input stub \(tests: the injection path runs, SendInput only logged\)/.test(A.readLog()));
const B = await startBrowser(outDir, 'input');
B.watch.add(A.pid);
const T = await B.tab({w: 1280, h: 720, dpr: 1});
const {s, ev, waitFor} = T;
const evidence = {};
try {
  await T.nav(A.lanUrl);
  check('PC browser: 允許 asked on the PC (a new device)', await A.answer(1, 20000), `asks ${A.asks()}`);
  check('PC browser: connected, first picture', !!(await waitFor(() => ev(connectedJs), 30000)));
  await sleep(1500);
  const logAfter = async (fn, ms = 700) => { const m0 = A.readLog().length; await fn(); await sleep(ms); return A.readLog().slice(m0); };
  const geo = () => ev(`(() => { const b = videoBox(); const z = trackpad.zoom; const cx = b.r.left + b.r.width / 2 + trackpad.panX, cy = b.r.top + b.r.height / 2 + trackpad.panY;
    return {l: cx - b.w * z / 2, t: cy - b.h * z / 2, w: b.w * z, h: b.h * z, dw: deskW(), dh: deskH(), mode: prefs.mode, mon: JSON.stringify(S.monitors || null)}; })()`);
  const g = await geo();
  evidence.geometry = g;
  console.log('picture', JSON.stringify(g));
  const BTN = {left: 1, middle: 4, right: 2, back: 8, forward: 16};
  let held = 0;
  const mouse = async (type, x, y, button = 'none', extra = {}) => {
    if (type === 'mousePressed') held |= BTN[button] || 0;
    await s('Input.dispatchMouseEvent', {type, x, y, button, buttons: held, clickCount: extra.clickCount || 1, pointerType: 'mouse', ...extra});
    if (type === 'mouseReleased') held &= ~(BTN[button] || 0);
  };
  const at = (fx, fy) => [g.l + fx * g.w, g.t + fy * g.h];
  const exp = (fx, fy) => [Math.floor(fx * g.dw), Math.floor(fy * g.dh)];
  const click = async (fx, fy, b = 'left') => { const [x, y] = at(fx, fy); await mouse('mouseMoved', x, y); await sleep(30); await mouse('mousePressed', x, y, b); await sleep(30); await mouse('mouseReleased', x, y, b); };
  const hostAt = (L, b, what) => { const m = L.match(new RegExp(`: ${b} ${what} at (-?\\d+),(-?\\d+)`)); return m ? [+m[1], +m[2]] : null; };
  const near = (p, e, tol = 1) => p && Math.abs(p[0] - e[0]) <= tol && Math.abs(p[1] - e[1]) <= tol;
  const stubLines = L => L.split(/\r?\n/).filter(l => /input \(stub\)|STUB SendInput/.test(l));
  // focus on the picture first (the PC gets the keyboard after a click)
  await click(0.5, 0.5);
  await sleep(500);
  // ---- mouse buttons, each at its own place ----
  const cases = [['left', 0.25, 0.25], ['right', 0.75, 0.30], ['middle', 0.40, 0.60]];
  for (const [b, fx, fy] of cases) {
    const L = await logAfter(() => click(fx, fy, b), 800);
    const d = hostAt(L, b, 'down'), u = hostAt(L, b, 'up'), e = exp(fx, fy);
    const flag = {left: 'LEFTDOWN', right: 'RIGHTDOWN', middle: 'MIDDLEDOWN'}[b];
    const stubDown = L.match(new RegExp(`STUB SendInput mouse [A-Z|]*${flag}[A-Z|]* data -?\\d+ at (-?\\d+),(-?\\d+)`));
    evidence['mouse_' + b] = stubLines(L);
    check(`滑鼠${{left: '左', right: '右', middle: '中'}[b]}鍵 at (${fx}, ${fy}) of the picture: down + up at PC pixel ~${e} (±1)`, near(d, e) && near(u, e), `host down ${d} up ${u}; stub SendInput ${stubDown ? stubDown[1] + ',' + stubDown[2] : 'none'}`);
    check(`  ...SendInput (stub) ${flag} carried to the same pixel`, !!stubDown, stubDown ? stubDown[0] : '');
  }
  // ---- back / forward ----
  {
    const href0 = await ev('location.href');
    const L = await logAfter(async () => { for (const b of ['back', 'forward']) { const [x, y] = at(0.45, 0.45); await mouse('mouseMoved', x, y); await mouse('mousePressed', x, y, b); await mouse('mouseReleased', x, y, b); await sleep(100); } }, 900);
    evidence.mouse_xbuttons = stubLines(L);
    check('側鍵 上一頁/下一頁: XBUTTON1 / XBUTTON2 down + up, the browser did not navigate', /XDOWN data 1/.test(L) && /XUP data 1/.test(L) && /XDOWN data 2/.test(L) && /XUP data 2/.test(L) && (await ev('location.href')) === href0);
  }
  // ---- drag ----
  {
    const L = await logAfter(async () => {
      const [x0, y0] = at(0.2, 0.2), [x1, y1] = at(0.6, 0.5);
      await mouse('mouseMoved', x0, y0); await mouse('mousePressed', x0, y0, 'left');
      for (let i = 1; i <= 15; i++) { await mouse('mouseMoved', x0 + (x1 - x0) * i / 15, y0 + (y1 - y0) * i / 15, 'left'); await sleep(20); }
      await mouse('mouseReleased', x1, y1, 'left');
    }, 900);
    const d = hostAt(L, 'left', 'down'), u = hostAt(L, 'left', 'up');
    evidence.drag = stubLines(L);
    check(`拖曳: left down at ~${exp(0.2, 0.2)}, up at ~${exp(0.6, 0.5)} (moves between, button held)`, near(d, exp(0.2, 0.2)) && near(u, exp(0.6, 0.5)), `down ${d} up ${u}`);
  }
  // ---- wheel ----
  {
    const [x, y] = at(0.5, 0.5);
    const L = await logAfter(async () => {
      await s('Input.dispatchMouseEvent', {type: 'mouseWheel', x, y, deltaX: 0, deltaY: 100, pointerType: 'mouse'}); await sleep(150);
      await s('Input.dispatchMouseEvent', {type: 'mouseWheel', x, y, deltaX: 0, deltaY: -200, pointerType: 'mouse'}); await sleep(150);
      await s('Input.dispatchMouseEvent', {type: 'mouseWheel', x, y, deltaX: 50, deltaY: 0, pointerType: 'mouse'}); await sleep(150);
    }, 1200);
    const wh = [...L.matchAll(/mouse WHEEL data (-?\d+)/g)].map(m => +m[1]), hw = [...L.matchAll(/mouse HWHEEL data (-?\d+)/g)].map(m => +m[1]);
    evidence.wheel = stubLines(L);
    check('滾輪: 1 notch down = WHEEL -120, 2 notches up = +240, sideways 50 px = HWHEEL +60', wh[0] === -120 && wh.slice(1).reduce((a, b) => a + b, 0) === 240 && hw.reduce((a, b) => a + b, 0) === 60, `WHEEL ${wh.join(',')} HWHEEL ${hw.join(',')}`);
  }
  // ---- keyboard by scan code ----
  const VK = {ShiftLeft: 16, ControlLeft: 17, AltLeft: 18, Enter: 13, Escape: 27, Tab: 9, ArrowLeft: 37, F5: 116};
  const key = async (type, code, keyName, mods = 0, text) => {
    const vk = VK[code] || (code.startsWith('Key') ? code.charCodeAt(3) : code.startsWith('Digit') ? code.charCodeAt(5) : 0);
    await s('Input.dispatchKeyEvent', {type, code, key: keyName, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk, modifiers: mods, ...(text ? {text, unmodifiedText: text} : {})});
  };
  const press = async (code, k, mods = 0, text) => { await key('keyDown', code, k, mods, text); await key('keyUp', code, k, mods); };
  const scans = L => [...L.matchAll(/STUB SendInput key vk 0x[0-9A-F]+ scan 0x([0-9A-F]+)( ext)?( unicode)? (down|up)/g)].map(m => `${m[1]}${m[2] ? 'e' : ''}${m[3] ? 'u' : ''}${m[4] === 'down' ? '+' : '-'}`);
  await click(0.5, 0.5);
  await sleep(300);
  {
    const L = await logAfter(async () => { for (const c of 'hello') await press('Key' + c.toUpperCase(), c, 0, c); }, 800);
    const sc = scans(L);
    evidence.keys_hello = stubLines(L);
    check('鍵盤 scancode: h e l l o -> 23 12 26 26 18 (down/up each)', sc.join(' ') === '23+ 23- 12+ 12- 26+ 26- 26+ 26- 18+ 18-', sc.join(' '));
  }
  {
    const L = await logAfter(async () => {
      await key('keyDown', 'ShiftLeft', 'Shift', 8); await press('Digit1', '!', 8, '!'); await key('keyUp', 'ShiftLeft', 'Shift', 0);
      await key('keyDown', 'ControlLeft', 'Control', 2); await press('KeyC', 'c', 2); await key('keyUp', 'ControlLeft', 'Control', 0);
      await press('ArrowLeft', 'ArrowLeft'); await press('Enter', 'Enter', 0, '\r'); await press('F5', 'F5');
    }, 900);
    const sc = scans(L);
    evidence.keys_mods = stubLines(L);
    check('修飾鍵/快捷鍵: Shift+1 (2A 02), Ctrl+C (1D 2E), ← (4B ext), Enter (1C), F5 (3F) reach the PC; the page stays',
      sc.join(' ') === '2A+ 02+ 02- 2A- 1D+ 2E+ 2E- 1D- 4Be+ 4Be- 1C+ 1C- 3F+ 3F-' && (await ev(connectedJs)) === 1, sc.join(' '));
    check('the PC log never names the keys (only counts): no KeyH / Digit1 text outside the stub lines', !/KeyH|KeyE|Digit1/.test(L.replace(/[^\n]*STUB SendInput[^\n]*/g, '')));
  }
  // ---- IME (this PC's Chinese / Japanese input method): composition, then the text ----
  {
    const L = await logAfter(async () => {
      await s('Input.imeSetComposition', {text: 'ㄋㄧˇ', selectionStart: 3, selectionEnd: 3}); await sleep(80);
      await s('Input.imeSetComposition', {text: 'ㄋㄧˇㄏㄠˇ', selectionStart: 6, selectionEnd: 6}); await sleep(80);
      await s('Input.imeSetComposition', {text: '你好', selectionStart: 2, selectionEnd: 2}); await sleep(80);
      await s('Input.insertText', {text: '你好'});
    }, 900);
    const uni = scans(L).filter(x => x.includes('u'));
    evidence.ime_zh = stubLines(L);
    check('中文輸入法 (注音 composition -> 你好): one text message, the PC gets U+4F60 U+597D as unicode keys (no composition keys)',
      (L.match(/text \(\d+ bytes\)/g) || []).length === 1 && uni.join(' ') === '4F60u+ 4F60u- 597Du+ 597Du-', `${(L.match(/text \(\d+ bytes\)/g) || []).join(',')} | ${uni.join(' ')}`);
    const L2 = await logAfter(async () => { await s('Input.imeSetComposition', {text: 'にほん', selectionStart: 3, selectionEnd: 3}); await sleep(60); await s('Input.insertText', {text: '日本'}); }, 800);
    const uni2 = scans(L2).filter(x => x.includes('u'));
    evidence.ime_ja = stubLines(L2);
    check('日文輸入法 (にほん -> 日本): U+65E5 U+672C', uni2.join(' ') === '65E5u+ 65E5u- 672Cu+ 672Cu-', uni2.join(' '));
  }
  // ---- nothing stuck at the end ----
  await T.shot(path.join(outDir, 's1_pc_browser.png'));
  evidence.timing = (A.readLog().match(/input timing[^\n]*/g) || []).slice(-2);
} catch (e) { check('s1 ran without an exception', false, String(e && e.stack || e)); }
try { await T.close(); } catch {}
const alt = await B.close();
const st = await A.stop();
const L = A.readLog();
check('no real SendInput anywhere in the log (every injection went to the stub)', !/input: SendInput\(/.test(L) && /STUB SendInput/.test(L));
check('app quit by itself (Exit), not killed', !st.killed, JSON.stringify(st));
check(`Alt+Tab / taskbar windows of the test's processes = 0 (${alt.samples} samples)`, alt.samples > 0 && alt.max === 0, alt.seen.join(' ; '));
R.save('s1_input.json', {evidence});
console.log(R.failed() ? `FAILED (${R.failed()})` : 'ALL PASSED');
process.exit(R.failed() ? 1 : 0);
