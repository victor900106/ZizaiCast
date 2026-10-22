// 0.8.3.5 checks, part 5 (run by pcmouse0835_more4.mjs): c (modes, the pointer, 遊戲模式).
export async function run(ctx, want) {
  const {s, ev, check, sleep, logAfter, shot, mouse} = ctx;
  const sentSince = async (t0, re) => ev(`__sent.filter(m => m[0] >= ${t0} && ${re}.test(m[1])).length`);

  if (want('c')) {
    // c1. a PC's browser: absolute (直接點) whatever the phone setting says
    await ev(`(() => { setMode('trackpad'); return 1; })()`);
    const t0 = await ev('performance.now()');
    for (let i = 0; i < 10; i++) { const [x, y] = ctx.at(0.3 + i * 0.03, 0.4); await mouse('mouseMoved', x, y); await sleep(20); }
    await sleep(200);
    const abs = await sentSince(t0, /"t":"abs"/), rel = await sentSince(t0, /"t":"(move|rel)"/);
    check('c a PC browser moves the PC pointer absolutely (直接點), even with 觸控板 saved for touch', abs >= 5 && rel === 0, `abs ${abs}, relative ${rel}`);
    await ev(`(() => { setMode('direct'); return 1; })()`);
    // c2. one pointer: this browser's, the PC leaves its own out of the picture; shapes follow
    const L = ctx.readLog();
    check('c the PC leaves its pointer out of the picture (the browser draws it: no lag, no double)', /pointer drawn by the PC browser/.test(L));
    const shapes = await ev(`(() => { const r = {}; for (const k of ['ibeam', 'hand', 'we', 'nwse', 'wait', 'custom']) { onCursor({x: .5, y: .5, n: 0, v: true, k}); r[k] = pad.style.cursor; }
      onCursor({x: .5, y: .5, n: 0, v: false, k: 'arrow'}); r.hidden = pad.style.cursor; onCursor({x: .5, y: .5, n: 0, v: true, k: 'arrow'}); r.arrow = pad.style.cursor; return r; })()`);
    check('c the pointer shape follows the PC (text, hand, resize, wait; hidden when the PC hides it)',
      shapes.ibeam === 'text' && shapes.hand === 'pointer' && shapes.we === 'ew-resize' && shapes.nwse === 'nwse-resize' && shapes.wait === 'wait' && shapes.custom === 'default' && shapes.hidden === 'none' && shapes.arrow === 'default',
      JSON.stringify(shapes));
    const tc = await ev(`getComputedStyle($('tcur')).display`);
    check('c no phone pointer drawn for a mouse (one pointer only)', tc === 'none', tc);
    // c3. 遊戲模式: the bar button asks for the pointer lock
    await ev(`(() => { showPcbar(0); return 1; })()`);
    await sleep(300);
    const r = await ev(`(() => { const b = $('pbLock').getBoundingClientRect(); return {x: b.left + b.width / 2, y: b.top + b.height / 2}; })()`);
    await mouse('mouseMoved', r.x, r.y); await mouse('mousePressed', r.x, r.y, 'left'); await mouse('mouseReleased', r.x, r.y, 'left');
    await sleep(700);
    let real = await ev('locked()');
    if (!real) {  // the headless shell has no pointer lock: the same state, by hand
      await ev(`(() => { Object.defineProperty(document, 'pointerLockElement', {configurable: true, get: () => window.__fakeLock ? pad : null}); window.__fakeLock = true;
        document.exitPointerLock = () => { window.__fakeLock = false; document.dispatchEvent(new Event('pointerlockchange')); };
        document.dispatchEvent(new Event('pointerlockchange')); return 1; })()`);
      await sleep(300);
    }
    const st = await ev(`({locked: locked(), hint: $('lockHint').style.display, big: $('lockHint').classList.contains('big'), text: $('lockHint').textContent, btn: $('pbLock').classList.contains('on')})`);
    check(`c 遊戲模式 on${real ? '' : ' (pointer lock simulated: the headless shell has none)'}: the hint says how to leave (Esc)`, st.locked && st.hint === 'block' && st.big && /Esc/.test(st.text) && st.btn, JSON.stringify(st));
    await shot(`pc_gamemode_${ctx.lang}`);
    await sleep(300);
    check('c 遊戲模式: the PC draws its pointer again', /pointer back in the picture/.test(ctx.readLog()));
    await mouse('mouseMoved', 600, 360);
    await sleep(1200);  // that jump is flushed and logged before the measured moves
    const g0 = await ev('performance.now()');
    const Lr = await logAfter(async () => {
      let x = 600, y = 360;
      for (let i = 0; i < 20; i++) { x += 7; y -= 3; await mouse('mouseMoved', x, y); await sleep(10); }
      await mouse('mousePressed', x, y, 'left'); await mouse('mouseReleased', x, y, 'left');
    }, 2300);
    const relN = await sentSince(g0, /"t":"rel"/), absN = await sentSince(g0, /"t":"abs"/);
    const parts = [...Lr.matchAll(/relative \(遊戲模式\) x\d+, total dx=(-?\d+) dy=(-?\d+)/g)];  // one log line per second
    const tot = parts.length ? [null, parts.reduce((a, m) => a + +m[1], 0), parts.reduce((a, m) => a + +m[2], 0)] : null;
    check('c 遊戲模式: raw relative motion (no absolute), 20 moves of (+7,-3) = (+140,-60)', relN > 0 && absN === 0 && tot && +tot[1] === 140 && +tot[2] === -60,
      `rel msgs ${relN}, abs ${absN}, PC total ${tot ? tot[1] + ',' + tot[2] : '-'}`);
    check('c 遊戲模式: buttons still work (left down / up)', /: left down at/.test(Lr) && /: left up at/.test(Lr));
    await ev(`(() => { $('lockHint').classList.remove('big'); return 1; })()`);
    await sleep(300);
    await shot(`pc_gamemode_small_${ctx.lang}`);
    // Esc (the browser leaves the lock)
    await ev('(document.exitPointerLock(), 1)');
    await sleep(500);
    const st2 = await ev(`({locked: locked(), hint: $('lockHint').style.display, toast: $('toast').textContent})`);
    check('c leaving 遊戲模式: back to the absolute mouse, said so', !st2.locked && st2.hint === 'none' && st2.toast === (await ev(`t('gameOff')`)), JSON.stringify(st2));
    await sleep(300);
    const Ll = ctx.readLog();
    check('c ... and this browser draws the pointer again', Ll.lastIndexOf('pointer drawn by the PC browser') > Ll.lastIndexOf('pointer back in the picture'));
    if (!real) await ev(`(() => { delete document.pointerLockElement; return 1; })()`);
  }

  const next = new URL('./pcmouse0835_more6.mjs', import.meta.url);
  const fsm = await import('node:fs');
  if (fsm.existsSync(next)) { const mod = await import(next.href); await mod.run(ctx, want); }
}
