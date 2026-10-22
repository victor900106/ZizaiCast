// 0.8.3.5 checks, part 2 (run by pcmouse0835_more.mjs): uipi (③; spike --inject-stub-elevated <exe>).
export async function run(ctx, want) {
  const {s, ev, check, sleep, mouse, logAfter, shot} = ctx;

  if (want('uipi') && !ctx.elevated) {  // a normal window: no false alarm
    await ev(`(() => { $('toast').style.display = 'none'; S.blocked = 0; return 1; })()`);
    const L = await logAfter(() => ctx.click(0.5, 0.5, 'left'), 900);
    check('③ a normal window: no UIPI notice (PC log, page)', !/not delivered/.test(L) && (await ev('S.blocked || 0')) === 0 && /: left down at/.test(L));
  }
  if (want('uipi') && ctx.elevated) {
    // a click on a window "run as administrator" (the stub says every window is): the PC log and the page say so
    await ev(`(() => { $('toast').style.display = 'none'; return 1; })()`);
    const L = await logAfter(() => ctx.click(0.5, 0.5, 'left'), 900);
    const toastTxt = await ev(`$('toast').style.display === 'block' ? $('toast').textContent : ''`);
    check('③ PC log: the click is not delivered (UIPI), the program named', /runs as administrator \(integrity above ours\): click not delivered \(UIPI\)/.test(L) && /regedit\.exe/.test(L));
    const want0 = await ev(`t('uipiAdmin').split('{app}').join(PM.app || '')`);
    check('③ the page says it in plain words (with the program)', toastTxt.startsWith(want0) && toastTxt.includes('regedit.exe'), toastTxt);
    await shot(`uipi_pc_${ctx.lang}`);
    // the phone (touch, 直接點): a tap on it says the same
    await s('Emulation.setDeviceMetricsOverride', {width: 390, height: 844, deviceScaleFactor: 3, mobile: true});
    await s('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5});
    await sleep(600);
    await ev(`(() => { $('toast').style.display = 'none'; setMode('direct'); return 1; })()`);
    await sleep(3200);  // the same program again only after 3 s
    const b = await ev(`(() => { const b = videoBox(); return {x: b.r.left + b.r.width / 2, y: b.r.top + b.r.height / 2}; })()`);
    await s('Input.dispatchTouchEvent', {type: 'touchStart', touchPoints: [{x: b.x, y: b.y}]});
    await sleep(60);
    await s('Input.dispatchTouchEvent', {type: 'touchEnd', touchPoints: []});
    await sleep(900);
    const t2 = await ev(`$('toast').style.display === 'block' ? $('toast').textContent : ''`);
    check('③ phone: a tap on an administrator window shows the same words', t2.startsWith(want0), t2);
    await shot(`uipi_phone_${ctx.lang}`);
    await s('Emulation.setTouchEmulationEnabled', {enabled: false});
    await s('Emulation.setDeviceMetricsOverride', {width: ctx.W, height: ctx.H, deviceScaleFactor: 1, mobile: false});
    await sleep(400);
  }

  const next = new URL('./pcmouse0835_more3.mjs', import.meta.url);
  const fsm = await import('node:fs');
  if (fsm.existsSync(next)) { const mod = await import(next.href); await mod.run(ctx, want); }
}
