// 0.8.3.5 checks, part 2 (run by pcmouse0835_more.mjs): uipi (③; spike --inject-stub-elevated <exe>).
// 0.8.4: once per window (the stub's program has two windows: the left / right half of the screen).
export async function run(ctx, want) {
  const {s, ev, check, sleep, mouse, logAfter, shot} = ctx;

  if (want('uipi') && !ctx.elevated) {  // a normal window: no false alarm
    await ev(`(() => { $('toast').style.display = 'none'; S.blocked = 0; return 1; })()`);
    const L = await logAfter(() => ctx.click(0.5, 0.5, 'left'), 900);
    check('③ a normal window: no UIPI notice (PC log, page)', !/not delivered/.test(L) && (await ev('S.blocked || 0')) === 0 && /: left down at/.test(L));
  }
  if (want('uipi') && ctx.elevated) {
    // a click on a window "run as administrator" (the stub: that program has two windows, the left and the
    // right half of the screen): the PC log and the page say so, ONCE per window (0.8.4; 0.8.3.5: every 3 s)
    const visible = () => ev(`$('toast').style.display === 'block' ? $('toast').textContent : ''`);
    const hide = () => ev(`(() => { $('toast').style.display = 'none'; return 1; })()`);
    const admLines = L => (L.match(/runs as administrator \(integrity above ours\)/g) || []).length;
    await hide();
    const L = await logAfter(() => ctx.click(0.25, 0.5, 'left'), 900);
    const toastTxt = await visible();
    check('③ PC log: the click is not delivered (UIPI), the program named', /runs as administrator \(integrity above ours\): click not delivered \(UIPI/.test(L) && /regedit\.exe/.test(L));
    const want0 = await ev(`t('uipiAdmin').split('{0}').join(tf('uipiApp', 'regedit.exe'))`);
    check('③ the page says it in plain words (with the program)', toastTxt === want0, toastTxt);
    await shot(`uipi_pc_${ctx.lang}`);
    // 0.8.4: the same window again, after the 3 s that 0.8.3.5 waited: nothing more (PC log, DataChannel, page)
    await hide();
    await sleep(3300);
    const n0 = await ev('S.blocked || 0');
    const L2 = await logAfter(async () => { await ctx.click(0.25, 0.5, 'left'); await ctx.click(0.3, 0.4, 'right'); await ctx.click(0.2, 0.6, 'left', 2); }, 900);
    const again = await visible();
    check('③ 0.8.4: the same window clicked again 3.3 s later (4 presses): no second notice (PC log, page)',
      admLines(L2) === 0 && again === '' && (await ev('S.blocked || 0')) === n0 && /: left down at/.test(L2), `log ${admLines(L2)}, toast "${again}"`);
    // the phone (touch, 直接點): a tap on the program's OTHER window is told once, a second tap is not
    await s('Emulation.setDeviceMetricsOverride', {width: 390, height: 844, deviceScaleFactor: 3, mobile: true});
    await s('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5});
    await sleep(600);
    await ev(`(() => { $('toast').style.display = 'none'; setMode('direct'); return 1; })()`);
    const b = await ev(`(() => { const b = videoBox(); return {x: b.r.left + (b.r.width - b.w) / 2 + 0.75 * b.w, y: b.r.top + b.r.height / 2}; })()`);
    const tap = async () => {
      await s('Input.dispatchTouchEvent', {type: 'touchStart', touchPoints: [{x: b.x, y: b.y}]});
      await sleep(60);
      await s('Input.dispatchTouchEvent', {type: 'touchEnd', touchPoints: []});
      await sleep(900);
    };
    const L3 = await logAfter(tap, 100);
    const t2 = await visible();
    check('③ phone: a tap on the program\'s other administrator window shows the words once', t2 === want0 && admLines(L3) === 1, `log ${admLines(L3)}: ${t2}`);
    await shot(`uipi_phone_${ctx.lang}`);
    await hide();
    const L4 = await logAfter(tap, 100);
    const t3 = await visible();
    check('③ 0.8.4 phone: a second tap on that window: no second notice', t3 === '' && admLines(L4) === 0, `log ${admLines(L4)}, toast "${t3}"`);
    await shot(`uipi_phone_again_${ctx.lang}`);
    await s('Emulation.setTouchEmulationEnabled', {enabled: false});
    await s('Emulation.setDeviceMetricsOverride', {width: ctx.W, height: ctx.H, deviceScaleFactor: 1, mobile: false});
    await sleep(400);
  }

  const next = new URL('./pcmouse0835_more3.mjs', import.meta.url);
  const fsm = await import('node:fs');
  if (fsm.existsSync(next)) { const mod = await import(next.href); await mod.run(ctx, want); }
}
