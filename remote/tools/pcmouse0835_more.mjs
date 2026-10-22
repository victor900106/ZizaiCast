// 0.8.3.5 PC-browser checks, run by pcmouse0835_test.mjs (same page, same stubbed spike).
// Sections: a (mouse), b (keyboard), c (modes / pointer lock / cursor), d (toolbar / fullscreen /
// clipboard / monitors), e (event rate / latency), uipi (③), touch (② phone page).
export async function run(ctx, want) {
  const {s, ev, check, since, readLog, mouse, sleep, before} = ctx;
  const logAfter = async (fn, ms = 700) => { const m0 = readLog().length; await fn(); await sleep(ms); return since(m0); };
  const n = (L, re) => (L.match(re) || []).length;
  const geo = () => ev(`(() => { const b = videoBox(); const z = trackpad.zoom; const cx = b.r.left + b.r.width / 2 + trackpad.panX, cy = b.r.top + b.r.height / 2 + trackpad.panY;
    return {l: cx - b.w * z / 2, t: cy - b.h * z / 2, w: b.w * z, h: b.h * z, vw: innerWidth, vh: innerHeight, dw: deskW(), dh: deskH()}; })()`);
  const downAt = L => { const m = L.match(/: left down at (-?\d+),(-?\d+)/); return m ? [+m[1], +m[2]] : null; };
  const upAt = L => { const m = L.match(/: left up at (-?\d+),(-?\d+)/); return m ? [+m[1], +m[2]] : null; };
  ctx.logAfter = logAfter;

  if (want('a') && !before) {
    // a1. middle, back, forward; the page stays (no browser back)
    const href0 = await ev('location.href');
    let L = await logAfter(async () => {
      for (const b of ['middle', 'back', 'forward']) { const [x, y] = ctx.at(0.4, 0.4); await mouse('mouseMoved', x, y); await mouse('mousePressed', x, y, b); await mouse('mouseReleased', x, y, b); await sleep(80); }
    }, 900);
    check('a middle button: down + up', n(L, /: middle down at/g) === 1 && n(L, /: middle up at/g) === 1);
    check('a back button: XBUTTON1 down + up', /: back down at/.test(L) && /XDOWN data 1/.test(L) && /XUP data 1/.test(L));
    check('a forward button: XBUTTON2 down + up', /: forward down at/.test(L) && /XDOWN data 2/.test(L) && /XUP data 2/.test(L));
    const href1 = await ev('location.href'), st = await ev('S.state + "/" + (dc && dc.readyState)');
    check('a back / forward did not navigate the browser (still connected)', href1 === href0 && /open/.test(st), `${href1 === href0 ? 'same page' : href1} ${st}`);
    // a2. double click: two downs at the same pixel, fast
    L = await logAfter(async () => { await ctx.click(0.3, 0.6, 'left', 1); await ctx.click(0.3, 0.6, 'left', 2); });
    const downs = [...L.matchAll(/(\d\d):(\d\d):(\d\d)\.(\d+) \[remote\] input \(stub\): left down at (-?\d+),(-?\d+)/g)];
    const dt = downs.length === 2 ? (((+downs[1][3]) * 1000 + +downs[1][4]) - ((+downs[0][3]) * 1000 + +downs[0][4])) : -1;
    check('a double click: two down + up at the same pixel within the double-click time', downs.length === 2 && downs[0][5] === downs[1][5] && downs[0][6] === downs[1][6] && dt >= 0 && dt < 400,
      `${downs.length} downs, ${dt} ms apart`);
    // a3. drag: down, 12 moves with the button held, up somewhere else
    let g = await geo();
    L = await logAfter(async () => {
      const x0 = g.l + 0.2 * g.w, y0 = g.t + 0.2 * g.h, x1 = g.l + 0.6 * g.w, y1 = g.t + 0.5 * g.h;
      await mouse('mouseMoved', x0, y0); await mouse('mousePressed', x0, y0, 'left');
      for (let i = 1; i <= 12; i++) { await mouse('mouseMoved', x0 + (x1 - x0) * i / 12, y0 + (y1 - y0) * i / 12, 'left'); await sleep(20); }
      await mouse('mouseReleased', x1, y1, 'left');
    }, 900);
    const d0 = downAt(L), u0 = upAt(L);
    const ex = [Math.floor(0.6 * g.dw), Math.floor(0.5 * g.dh)];
    check('a drag: down at the start, up at the end (moves in between)', d0 && u0 && Math.abs(d0[0] - Math.floor(0.2 * g.dw)) <= 1 && Math.abs(u0[0] - ex[0]) <= 1 && Math.abs(u0[1] - ex[1]) <= 1,
      `down ${d0} up ${u0} (expected ~${ex})`);
    // a3b. a drag that leaves the picture ends there (pointer capture), clamped to the edge
    L = await logAfter(async () => {
      const x0 = g.l + 0.5 * g.w, y0 = g.t + 0.5 * g.h;
      await mouse('mouseMoved', x0, y0); await mouse('mousePressed', x0, y0, 'left');
      await mouse('mouseMoved', x0, g.t - 40, 'left'); await sleep(40);
      await mouse('mouseReleased', x0, g.t - 40, 'left');
    }, 900);
    const u1 = upAt(L);
    check('a drag out of the picture: released at the top edge', u1 && u1[1] === 0, `up ${u1}`);
    // a4. wheel: a notch, horizontal, a laptop trackpad's small pixel deltas
    L = await logAfter(async () => {
      const [x, y] = ctx.at(0.5, 0.5);
      await s('Input.dispatchMouseEvent', {type: 'mouseWheel', x, y, deltaX: 0, deltaY: 100, pointerType: 'mouse'}); await sleep(120);
      await s('Input.dispatchMouseEvent', {type: 'mouseWheel', x, y, deltaX: 50, deltaY: 0, pointerType: 'mouse'}); await sleep(120);
      for (let i = 0; i < 10; i++) { await s('Input.dispatchMouseEvent', {type: 'mouseWheel', x, y, deltaX: 0, deltaY: -4, pointerType: 'mouse'}); await sleep(20); }
    }, 1500);
    const wheels = [...L.matchAll(/mouse WHEEL data (-?\d+)/g)].map(m => +m[1]);
    const hw = [...L.matchAll(/mouse HWHEEL data (-?\d+)/g)].map(m => +m[1]);
    const smooth = wheels.slice(1).reduce((a, b) => a + b, 0);
    check('a wheel: one notch (100 px) down = -120', wheels[0] === -120, `wheel ${wheels.join(',')}`);
    check('a wheel: sideways 50 px = +60 (HWHEEL)', hw.reduce((a, b) => a + b, 0) === 60, `hwheel ${hw.join(',')}`);
    check('a wheel: ten 4 px trackpad steps up = +48 in small steps (smooth; fractions carried)', smooth >= 47 && smooth <= 48 && wheels.length > 2, `${wheels.length - 1} events, sum ${smooth}`);
    // a5. right click: no browser menu
    const cm = await ev(`(() => { const e = new MouseEvent('contextmenu', {bubbles: true, cancelable: true, clientX: 300, clientY: 300}); pad.dispatchEvent(e); return e.defaultPrevented; })()`);
    check('a right click: the browser menu is suppressed (contextmenu defaultPrevented)', cm === true);
    // a6. 1:1: 30 random points of the picture -> the pixel under them (letterbox, DPR 1 / 2, zoomed 1:1)
    const pixelCheck = async (label) => {
      g = await geo();
      let bad = 0, worst = 0, first = '';
      for (let i = 0; i < 30; i++) {
        const fx = 0.02 + 0.96 * Math.random(), fy = 0.02 + 0.96 * Math.random();
        const x = Math.round(g.l + fx * g.w) + 0.5, y = Math.round(g.t + fy * g.h) + 0.5;  // a CSS pixel's middle
        if (x < 0 || y < 0 || x >= g.vw || y >= g.vh) { i--; continue; }
        if (g.w > g.vw && (x < 60 || x > g.vw - 60 || y < 60 || y > g.vh - 60)) { i--; continue; }  // zoomed: the picture follows the mouse at the edges (d)
        if ((await ev(`document.elementFromPoint(${x}, ${y}) === pad`)) !== true) { i--; continue; }  // under the toolbar
        const LL = await logAfter(async () => { await mouse('mouseMoved', x, y); await mouse('mousePressed', x, y, 'left'); await mouse('mouseReleased', x, y, 'left'); }, 120);
        const got = downAt(LL);
        const want = [Math.floor((x - g.l) / g.w * g.dw), Math.floor((y - g.t) / g.h * g.dh)];
        const err = got ? Math.max(Math.abs(got[0] - want[0]), Math.abs(got[1] - want[1])) : 99;
        worst = Math.max(worst, err);
        if (err > 0) { bad++; if (!first) first = `${got} for ${want}`; }
      }
      check(`a 1:1 ${label}: 30 random points land on the pixel under the mouse`, bad === 0, `${bad} off (worst ${worst} px)${first ? ', first ' + first : ''}; picture ${Math.round(g.w)}x${Math.round(g.h)} at ${Math.round(g.l)},${Math.round(g.t)} of ${g.vw}x${g.vh}, desktop ${g.dw}x${g.dh}`);
    };
    await pixelCheck('(1280x720, letterboxed, DPR 1)');
    // letterbox bars: a click on the black bar above the picture does nothing
    g = await geo();
    if (g.t > 10) {
      L = await logAfter(async () => { await mouse('mouseMoved', g.l + g.w / 2, g.t / 2); await mouse('mousePressed', g.l + g.w / 2, g.t / 2, 'left'); await mouse('mouseReleased', g.l + g.w / 2, g.t / 2, 'left'); });
      check('a a click on the black bar (letterbox) is not sent', !/: left down/.test(L));
    }
    await s('Emulation.setDeviceMetricsOverride', {width: 1000, height: 900, deviceScaleFactor: 2, mobile: false});
    await sleep(500);
    await pixelCheck('(1000x900, bars above / below, DPR 2)');
    await ev(`(() => { $('bZoom').onclick(); return trackpad.zoom; })()`);
    await sleep(400);
    const z = await ev('trackpad.zoom');
    await pixelCheck(`(zoomed in x${(+z).toFixed(2)}, panned)`);
    await ev(`(() => { if (trackpad.zoom > 1.01) $('bZoom').onclick(); return trackpad.zoom; })()`);
    await s('Emulation.setDeviceMetricsOverride', {width: ctx.W, height: ctx.H, deviceScaleFactor: 1, mobile: false});
    await sleep(400);
  }

  const extra2 = new URL('./pcmouse0835_more2.mjs', import.meta.url);
  try { const fsm = await import('node:fs'); if (fsm.existsSync(extra2)) { const mod = await import(extra2.href); await mod.run(ctx, want); } } catch (e) { check('more2 ran', false, String(e && e.stack || e)); }
}
