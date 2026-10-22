// 0.8.3.5 before / after screenshots (run by pcmouse0835_more7.mjs with --only shots --shots <dir>):
// the same steps on the 0.8.3.4 page (--before) and on this one. The picture is always a made-up
// desktop (pcmouse0835_test.mjs shot(): this PC's screen is never in a shot).
//   S1 a PC's browser, the mouse at the top edge   S2 a phone's first connection (直接點)
//   S3 a phone in 觸控板 after a swipe             S4 a click on a window (uipi run: "run as administrator")
export async function run(ctx, want) {
  if (want('shots')) await shots(ctx);
  const mod = await import(new URL('./pcmouse0835_more9.mjs', import.meta.url).href);
  await mod.run(ctx, want);
}
async function shots(ctx) {
  const {s, ev, sleep, mouse, shot, lang} = ctx;
  const L = ['zh-TW', 'en', 'ja', 'ko'][lang] || lang;
  await mouse('mouseMoved', 640, 300); await sleep(200);
  await mouse('mouseMoved', 640, 12); await sleep(700);
  await shot(`S1_pc_browser_${L}`);
  if (ctx.elevated || ctx.before) {
    await ev(`(() => { $('toast').style.display = 'none'; return 1; })()`);
    await ctx.click(0.5, 0.5); await sleep(900);
    await shot(`S4_click_admin_window_${L}`);
  }
  await ctx.asPhone(390, 844);
  await sleep(900);
  await shot(`S2_phone_first_${L}`);
  await ev(`(() => { if ($('modeHint')) $('modeHint').style.display = 'none'; document.body.classList.remove('hint-open'); setMode('trackpad'); send({t: 'abs', x: 0.35, y: 0.5}); return 1; })()`);
  await sleep(400);
  const g = await ev(`(() => { const b = videoBox(); return {l: b.r.left + (b.r.width - b.w) / 2, t: b.r.top + (b.r.height - b.h) / 2, w: b.w, h: b.h}; })()`);
  await s('Input.dispatchTouchEvent', {type: 'touchStart', touchPoints: [{x: g.l + g.w * 0.3, y: g.t + g.h * 0.5}]});
  for (let i = 1; i <= 12; i++) { await s('Input.dispatchTouchEvent', {type: 'touchMove', touchPoints: [{x: g.l + g.w * (0.3 + 0.15 * i / 12), y: g.t + g.h * 0.5}]}); await sleep(16); }
  await s('Input.dispatchTouchEvent', {type: 'touchEnd', touchPoints: []});
  await sleep(800);
  await shot(`S3_phone_trackpad_${L}`);
  await s('Emulation.setTouchEmulationEnabled', {enabled: false});
  await s('Emulation.setDeviceMetricsOverride', {width: ctx.W, height: ctx.H, deviceScaleFactor: 1, mobile: false});
}
