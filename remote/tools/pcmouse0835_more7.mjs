// 0.8.3.5 checks, part 7 (run by pcmouse0835_more6.mjs): e (input event rate, injection latency).
// A 2 s flood of mouse moves made inside the page (~1-2 kHz: a gaming mouse; CDP's own events come
// at most once per frame), a click every 100 ms; then 60 keys. Both pointer and mouse events are
// made (0.8.3.4 listened to mouse events). The page counts its events / messages, the PC logs
// event -> injected (stub; only the new page stamps its messages). Optimised (moves once per
// frame) vs naive (a message per event, S.noBatch) on the same build; --before: the 0.8.3.4 page.
import fs from 'node:fs';
import path from 'node:path';
export async function run(ctx, want) {
  const {ev, check, sleep, logAfter, before} = ctx;
  const shots = async () => { const mod = await import(new URL('./pcmouse0835_more8.mjs', import.meta.url).href); await mod.run(ctx, want); };
  if (!want('e')) return shots();
  const timing = L => {
    const m = [...L.matchAll(/input timing \((\d+) s\): (\d+) messages \(([^)]*)\); event -> injected: moves p50 ([\d.]+) \/ p95 ([\d.]+) \/ max ([\d.]+) ms \(n (\d+)\), buttons \/ keys p50 ([\d.]+) \/ p95 ([\d.]+) \/ max ([\d.]+) ms \(n (\d+)\); injector p50 (\d+) \/ p95 (\d+) us/g)];
    return m.map(x => ({secs: +x[1], msgs: +x[2], types: x[3], moveP50: +x[4], moveP95: +x[5], moveMax: +x[6], moveN: +x[7], pressP50: +x[8], pressP95: +x[9], pressMax: +x[10], pressN: +x[11], injP50: +x[12], injP95: +x[13]}));
  };
  const counts = L => { const m = [...L.matchAll(/input timing \(\d+ s\): (\d+) messages \(([^)]*)\)/g)].pop(); return m ? m[2] : ''; };
  const flood = async (label, holdLeft) => {
    const r = await ev(`(async () => {
      S.inEvents = 0; S.inMsgs = 0; __sent.length = 0;
      const b = videoBox(); const l = b.r.left + (b.r.width - b.w) / 2, top = b.r.top + (b.r.height - b.h) / 2;
      const mk = (type, x, y, buttons, button) => {
        const o = {bubbles: true, cancelable: true, clientX: x, clientY: y, buttons, button: button || 0, pointerId: 1, pointerType: 'mouse', isPrimary: true, movementX: 2, movementY: 0};
        pad.dispatchEvent(new PointerEvent('pointer' + type, o));
        pad.dispatchEvent(new MouseEvent('mouse' + type, o));
      };
      let n = 0, clicks = 0, x = l + 50, dir = 1; const y = top + b.h / 2;
      const t0 = performance.now(); let nextClick = t0 + 100;
      if (${holdLeft}) mk('down', x, y, 1);
      while (performance.now() - t0 < 2000) {
        const sliceEnd = performance.now() + 2;
        while (performance.now() < sliceEnd) {
          x += dir * 0.7; if (x > l + b.w - 50 || x < l + 50) dir = -dir;
          mk('move', x, y + (n % 5), ${holdLeft} ? 1 : 0); n++;
          if (!${holdLeft} && performance.now() >= nextClick) { mk('down', x, y, 1); mk('up', x, y, 0); clicks++; nextClick += 100; }
        }
        await new Promise(r => setTimeout(r, 0));  // let the page run (frames, sends)
      }
      if (${holdLeft}) mk('up', x, y, 0);
      const dur = (performance.now() - t0) / 1000;
      await new Promise(r => setTimeout(r, 300));
      const c = {}; for (const [, m] of __sent) { const t = JSON.parse(m).t; c[t] = (c[t] || 0) + 1; }
      return {dur, made: n, clicks, events: S.inEvents || 0, c};
    })()`, true);
    return {label, ...r};
  };
  const res = {};
  await logAfter(() => sleep(10), 5600);  // an empty timing window first: the next lines are ours
  // the flood may straddle two of the PC's 5 s timing windows: the busiest line, and the stamped presses of all
  const one = async (label, holdLeft) => { let f; const L = await logAfter(async () => { f = await flood(label, holdLeft); }, 6000);
    const all = timing(L); const host = all.length ? {...all.reduce((a, b) => (b.msgs > a.msgs ? b : a)), pressN: all.reduce((a, b) => a + b.pressN, 0)} : null;
    return {...f, host, hostCounts: counts(L)}; };
  if (!before) {
    await ev('(S.measure = true, S.noBatch = false, 1)');
    res.optimised = await one('optimised (moves once per frame)');
    await ev('(S.noBatch = true, 1)');
    res.naive = await one('naive (a message per event)');
    await ev('(S.noBatch = false, 1)');
    res.dragOptimised = await one('optimised drag (left held)', true);
    // keys: 60 as fast as CDP gives them
    const kL = await logAfter(async () => { for (let i = 0; i < 30; i++) for (const c of ['KeyA', 'KeyS']) await ctx.press(c, c[3].toLowerCase(), 0, c[3].toLowerCase()); }, 6500);
    res.keys = {sent: 60, downsAtPc: (kL.match(/scan 0x(1E|1F) down/g) || []).length, host: timing(kL).pop() || null};
    for (const k of Object.keys(res)) console.log(`e ${k}: ${JSON.stringify(res[k])}`);
    const rate = r => Math.max(0, (r.c.abs || 0) - (r.c.down || 0) - (r.c.up || 0)) / r.dur, o = res.optimised, nv = res.naive;  // a click sends the pointer first
    check('e moves: at most one message per frame however many events (gaming mouse flood)', rate(o) <= 62 && o.events / o.dur > 500,
      `${o.events} events (${Math.round(o.events / o.dur)}/s) -> ${o.c.abs || 0} abs (${rate(o).toFixed(1)}/s); naive ${nv.c.abs || 0} (${rate(nv).toFixed(0)}/s)`);
    check('e clicks are never thinned out: every down / up of the flood reached the PC', o.c.down === o.clicks && o.c.up === o.clicks && o.host && o.host.pressN === 2 * o.clicks,
      `${o.clicks} clicks -> page ${o.c.down} down / ${o.c.up} up; PC stamped ${o.host ? o.host.pressN : '-'}`);
    check('e keys are never thinned out: 60 / 60 at the PC', res.keys.downsAtPc === 60, `${res.keys.downsAtPc}`);
    check('e buttons: event -> injected p95 < 10 ms on loopback, also under the flood', o.host && o.host.pressP95 < 10, o.host ? `p50 ${o.host.pressP50} / p95 ${o.host.pressP95} ms (naive: p95 ${nv.host && nv.host.pressP95} ms)` : '-');
    check('e moves: event -> injected p95 within ~one frame + transport (< 30 ms)', o.host && o.host.moveP95 < 30, o.host ? `p50 ${o.host.moveP50} / p95 ${o.host.moveP95} ms (naive p50 ${nv.host && nv.host.moveP50} / p95 ${nv.host && nv.host.moveP95})` : '-');
  } else {
    res.drag = await one('0.8.3.4 drag (left held)', true);
    res.clicks = await one('0.8.3.4 moves + clicks', false);
    for (const k of Object.keys(res)) console.log(`e before ${k}: ${JSON.stringify(res[k])}`);
  }
  fs.writeFileSync(path.join(ctx.outDir, before ? 'e_before.json' : 'e_after.json'), JSON.stringify(res, null, 1));
  await shots();
}
