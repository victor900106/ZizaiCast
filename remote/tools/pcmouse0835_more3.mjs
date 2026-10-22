// 0.8.3.5 checks, part 3 (run by pcmouse0835_more2.mjs): touch (② the phone page: mode card, big
// pointer in 觸控板, 直接點 pixels on a phone, taps far from the pointer).
export async function run(ctx, want) {
  const {s, ev, check, sleep, logAfter, shot, waitFor} = ctx;
  const tap = async (x, y, holdMs = 50) => {
    await s('Input.dispatchTouchEvent', {type: 'touchStart', touchPoints: [{x, y}]});
    await sleep(holdMs);
    await s('Input.dispatchTouchEvent', {type: 'touchEnd', touchPoints: []});
  };
  const swipe = async (x0, y0, x1, y1, steps = 12) => {
    await s('Input.dispatchTouchEvent', {type: 'touchStart', touchPoints: [{x: x0, y: y0}]});
    for (let i = 1; i <= steps; i++) { await s('Input.dispatchTouchEvent', {type: 'touchMove', touchPoints: [{x: x0 + (x1 - x0) * i / steps, y: y0 + (y1 - y0) * i / steps}]}); await sleep(16); }
    await s('Input.dispatchTouchEvent', {type: 'touchEnd', touchPoints: []});
  };
  ctx.tap = tap;
  // a fresh phone: touch, 390x844, nothing saved; reloads the page (the same session slot)
  ctx.asPhone = async (w = 390, h = 844, keep = false) => {
    await s('Emulation.setDeviceMetricsOverride', {width: w, height: h, deviceScaleFactor: 3, mobile: true});
    await s('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5});
    await s('Emulation.setEmitTouchEventsForMouse', {enabled: false});
    if (!keep) await ev(`(() => { const d = localStorage.getItem('pm.device'); localStorage.clear(); if (d) localStorage.setItem('pm.device', d); return 1; })()`);
    await ev('(location.reload(), 1)');
    await sleep(800);
    const ok = await waitFor(() => ev('window.__pmFirstFrame && typeof dc !== "undefined" && dc && dc.readyState === "open" ? 1 : 0'), 25000);
    await ev(`(() => { const o = dc.send.bind(dc); window.__sent = []; dc.send = m => { if (__sent.length < 20000) __sent.push([performance.now(), m]); return o(m); }; return 1; })()`);
    await sleep(1200);
    return ok;
  };
  const geo = () => ev(`(() => { const b = videoBox(); const z = trackpad.zoom; const cx = b.r.left + b.r.width / 2 + trackpad.panX, cy = b.r.top + b.r.height / 2 + trackpad.panY;
    return {l: cx - b.w * z / 2, t: cy - b.h * z / 2, w: b.w * z, h: b.h * z, vw: innerWidth, vh: innerHeight, dw: deskW(), dh: deskH()}; })()`);

  if (want('touch')) {
    check('② phone: connected', !!(await ctx.asPhone()));
    const st = await ev(`({mode: prefs.mode, hint: $('modeHint').style.display, t: $('mhT').textContent, s: $('mhS').textContent, btn: $('mhSwitch').textContent, why: S.modeHint})`);
    check('② a new phone starts in 直接點', st.mode === 'direct', st.mode);
    check('② first connection: the card says the mode and how to switch', st.hint === 'block' && st.why === 'first' && st.t === (await ev(`t('mhDirectT')`)) && st.btn === (await ev(`t('mhToTrack')`)), `${st.t} / ${st.btn}`);
    await shot(`phone_hint_direct_${ctx.lang}`);
    // 直接點 pixels on the phone (portrait: the 3440x1440 picture is a thin strip)
    for (const [w, h] of [[390, 844], [844, 390]]) {
      if (w !== 390) await ctx.asPhone(w, h, true);
      await ev(`(() => { $('modeHint').style.display = 'none'; return 1; })()`);
      const g = await geo();
      let bad = 0, first = '';
      for (let i = 0; i < 12; i++) {
        const x = g.l + (0.03 + 0.94 * Math.random()) * g.w, y = g.t + (0.05 + 0.9 * Math.random()) * g.h;
        if ((await ev(`document.elementFromPoint(${x}, ${y}) === pad`)) !== true) { i--; continue; }
        const L = await logAfter(() => tap(x, y), 250);
        const m = L.match(/: left click at (-?\d+),(-?\d+)/);
        const exp = [Math.floor((x - g.l) / g.w * g.dw), Math.floor((y - g.t) / g.h * g.dh)];
        // touch points are whole CSS px in CDP: allow the PC pixels one CSS px covers
        const tol = Math.ceil(g.dw / g.w) + 1;
        const err = m ? Math.max(Math.abs(+m[1] - exp[0]), Math.abs(+m[2] - exp[1])) : 999;
        if (err > tol) { bad++; if (!first) first = `${m ? m[1] + ',' + m[2] : 'no click'} for ${exp}`; }
      }
      check(`② 直接點 ${w}x${h}: 12 taps click under the finger (3440x1440 picture ${Math.round(g.w)}x${Math.round(g.h)} CSS px)`, bad === 0, `${bad} off${first ? ', first ' + first : ''}`);
    }
    // 觸控板: the big pointer; taps far from it -> the card explains
    await ctx.asPhone(390, 844, true);
    await ev(`(() => { $('modeHint').style.display = 'none'; setMode('trackpad'); return 1; })()`);
    const g = await geo();
    await ev(`(send({t: 'abs', x: 0.35, y: 0.5}), 1)`);  // start from the middle of the picture
    await sleep(300);
    await swipe(g.l + g.w * 0.3, g.t + g.h * 0.5, g.l + g.w * 0.45, g.t + g.h * 0.5);
    await sleep(600);
    const cur = await ev(`(() => { const e = $('tcur'), r = e.getBoundingClientRect(), c = cursorClient(); return {mu: !!S.mouseUsed, srv: srvCur, mode: trackpad.mode, shown: getComputedStyle(e).display, w: r.width, h: r.height, x: r.left + 13, y: r.top + 13, cx: c && c.x, cy: c && c.y}; })()`);
    check('② 觸控板: the PC pointer is drawn big (a 24 px arrow in a pink ring, 48 px) where the PC pointer is', cur.shown === 'block' && cur.w >= 30 && Math.abs(cur.x - cur.cx) < 2 && Math.abs(cur.y - cur.cy) < 2, JSON.stringify(cur));
    await shot(`phone_trackpad_pointer_${ctx.lang}`);
    // three taps far away from the pointer (what the owner did): the card says where the click goes
    const c0 = await ev('cursorClient()');
    const farX = c0.x > g.l + g.w / 2 ? g.l + 24 : g.l + g.w - 24;  // the other end of the picture from the pointer
    for (const dx of [0, 10, -10]) { await tap(farX + dx, g.t + g.h * 0.5 + dx); await sleep(450); }
    const st2 = await ev(`({hint: $('modeHint').style.display, why: S.modeHint, t: $('mhT').textContent, miss: missTaps.length, lt: lastTouch, c: cursorClient(), sent: __sent.filter(m => /click|down/.test(m[1])).length})`);
    check('② 觸控板: three taps far from the pointer -> 「點擊會落在箭頭的位置」 + 改用直接點', st2.hint === 'block' && st2.why === 'miss', JSON.stringify(st2));
    await shot(`phone_trackpad_miss_${ctx.lang}`);
    await ev(`(() => { $('mhSwitch').click(); return 1; })()`);
    check('② one tap on 改用直接點 switches and remembers', (await ev(`prefs.mode + '/' + JSON.parse(localStorage.getItem('pm.mode083'))`)) === 'direct/direct');
    // back to the PC browser for the next sections
    await s('Emulation.setTouchEmulationEnabled', {enabled: false});
    await s('Emulation.setDeviceMetricsOverride', {width: ctx.W, height: ctx.H, deviceScaleFactor: 1, mobile: false});
    await ev('(location.reload(), 1)');
    await sleep(800);
    await waitFor(() => ev('window.__pmFirstFrame && typeof dc !== "undefined" && dc && dc.readyState === "open" ? 1 : 0'), 25000);
    await ev(`(() => { const o = dc.send.bind(dc); window.__sent = []; dc.send = m => { if (__sent.length < 20000) __sent.push([performance.now(), m]); return o(m); }; return 1; })()`);
    await sleep(1000);
    check('② a PC browser (mouse) gets no phone mode card', (await ev(`$('modeHint').style.display`)) !== 'block');
  }

  const next = new URL('./pcmouse0835_more4.mjs', import.meta.url);
  const fsm = await import('node:fs');
  if (fsm.existsSync(next)) { const mod = await import(next.href); await mod.run(ctx, want); }
}
