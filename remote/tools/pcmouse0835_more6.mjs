// 0.8.3.5 checks, part 6 (run by pcmouse0835_more5.mjs): d (the PC bar, full screen, size, screens, clipboard).
// The clipboard of THIS PC is never read or written: the spike uses its test clipboard, the page's
// writes are caught (pcmouse0835_test.mjs), and a paste is a synthetic paste event.
export async function run(ctx, want) {
  const {s, ev, check, sleep, logAfter, shot, mouse} = ctx;
  const clickEl = async sel => {
    const r = await ev(`(() => { const b = document.querySelector('${sel}').getBoundingClientRect(); return {x: b.left + b.width / 2, y: b.top + b.height / 2}; })()`);
    await mouse('mouseMoved', r.x, r.y); await mouse('mousePressed', r.x, r.y, 'left'); await mouse('mouseReleased', r.x, r.y, 'left');
  };

  if (want('d')) {
    // d1. the bar: everything for a PC's browser at the top, the phone's round buttons gone; fits
    await mouse('mouseMoved', 640, 10);
    await sleep(500);
    for (const [w, h] of [[1280, 720], [1000, 700], [720, 600]]) {
      await s('Emulation.setDeviceMetricsOverride', {width: w, height: h, deviceScaleFactor: 1, mobile: false});
      await sleep(250);
      await ev(`(() => { showPcbar(0); syncPcbar(); return 1; })()`);
      await sleep(300);
      const b = await ev(`(() => { const r = $('pcbarIn').getBoundingClientRect(); return {l: r.left, r: r.right, w: r.width, top: r.top, phoneBar: getComputedStyle($('bar')).display, groups: [...document.querySelectorAll('#pcbar .grp')].filter(g => g.offsetWidth).length,
        labels: getComputedStyle(document.querySelector('#pcbar .pl')).display}; })()`);
      check(`d bar at ${w}x${h}: on one line inside the window, the phone buttons hidden`, b.l >= 0 && b.r <= w && b.top < 20 && b.phoneBar === 'none' && b.groups >= 4,
        `${Math.round(b.w)} px wide at ${Math.round(b.l)}, labels ${b.labels}, ${b.groups} groups, phone bar ${b.phoneBar}`);
      await shot(`pc_bar_${w}_${ctx.lang}`);
    }
    await s('Emulation.setDeviceMetricsOverride', {width: ctx.W, height: ctx.H, deviceScaleFactor: 1, mobile: false});
    await sleep(300);
    // d2. full screen (+ the keyboard lock where there is one)
    await ev(`(() => { showPcbar(0); return 1; })()`);
    await clickEl('#pbFs');
    await sleep(700);
    const fs = await ev(`({fs: !!document.fullscreenElement, label: $('pbFsT').textContent, toast: $('toast').textContent, kl: !!(navigator.keyboard && navigator.keyboard.lock)})`);
    check('d full screen: on from the bar, the button says how to leave', fs.fs && fs.label === (await ev(`t('pbFsOff')`)) && /Esc/.test(fs.toast), JSON.stringify(fs));
    await shot(`pc_fullscreen_${ctx.lang}`);
    await ev('(document.exitFullscreen(), 1)');
    await sleep(500);
    check('d full screen: off again', (await ev('!document.fullscreenElement && $("pbFsT").textContent === t("pbFs")')) === true);
    // d3. actual size 1:1 and back to fit
    await ev(`(() => { showPcbar(0); return 1; })()`);
    await clickEl('#pbZoom');
    await sleep(400);
    const z1 = await ev(`({z: trackpad.zoom, label: $('pbZoomT').textContent, one: deskW() / videoBox().w / (window.devicePixelRatio || 1)})`);
    check('d 原尺寸: one PC pixel per screen pixel (3440 on 1280 = x2.69), then the button says 適應視窗', Math.abs(z1.z - z1.one) < 0.02 && z1.label === (await ev(`t('pbFit')`)), JSON.stringify(z1));
    // the picture follows the mouse near the right edge
    const p0 = await ev('trackpad.panX');
    for (let i = 0; i < 6; i++) { await mouse('mouseMoved', ctx.W - 10, 360 + i); await sleep(30); }
    const p1 = await ev('trackpad.panX');
    check('d 原尺寸: near the right edge the picture moves along', p1 < p0, `panX ${Math.round(p0)} -> ${Math.round(p1)}`);
    await ev(`(() => { showPcbar(0); return 1; })()`);
    await clickEl('#pbZoom');
    await sleep(300);
    check('d 適應視窗: back to the whole picture', (await ev('trackpad.zoom')) === 1);
    // d4. screens: one button per screen (2 made up here), a click switches
    await ev(`(() => { S.monitors = [{primary: true, w: 3440, h: 1440}, {primary: false, w: 1920, h: 1080}]; buildMonitorSeg(); showPcbar(0); return 1; })()`);
    await sleep(300);
    const mons = await ev(`[...$('pbMons').children].map(b => b.textContent + (b.classList.contains('on') ? '(on)' : ''))`);
    check('d screens: a button per screen, the current one marked', mons.length === 2 && /^1 .+\(on\)$/.test(mons[0]), JSON.stringify(mons));
    await shot(`pc_bar_screens_${ctx.lang}`);
    const L4 = await logAfter(() => clickEl('#pbMons button:nth-child(2)'), 3500);
    check('d screens: screen 2 -> the page asks the PC for it', (await ev('prefs.mon')) === 1 && /monitor[^\n]*1|"m":1/.test(L4 + (await ev('JSON.stringify(__sent.slice(-50).map(m => m[1]))'))) || /session/.test(L4), `prefs.mon ${await ev('prefs.mon')}`);
    await ctx.waitFor(() => ev('window.__pmFirstFrame && dc && dc.readyState === "open" ? 1 : 0'), 20000);
    await ev(`(() => { prefs.mon = 0; store.set('mon', 0); return 1; })()`);
    await ev(`(() => { connect('switch'); return 1; })()`);
    await sleep(2500);
    await ctx.waitFor(() => ev('dc && dc.readyState === "open" ? 1 : 0'), 20000);
    await ev(`(() => { const o = dc.send.bind(dc); window.__sent = []; dc.send = m => { if (__sent.length < 20000) __sent.push([performance.now(), m]); return o(m); }; return 1; })()`);
    await sleep(1500);
    // d5. clipboard: this computer -> the PC (Ctrl+V)
    await ctx.click(0.5, 0.5);
    await sleep(1500);  // the watch of the PC clipboard is on (a mouse is used)
    let L = await logAfter(async () => {
      await ctx.key('keyDown', 'ControlLeft', 'Control', 2);
      await ev(`(() => { pasteKey({ctrlKey: true, code: 'KeyV'}); const dt = new DataTransfer(); dt.setData('text/plain', '從這台電腦貼上 0835');
        document.dispatchEvent(new ClipboardEvent('paste', {clipboardData: dt, bubbles: true, cancelable: true})); return 1; })()`);
      await ctx.key('keyUp', 'KeyV', 'v', 2);
      await ctx.key('keyUp', 'ControlLeft', 'Control', 0);
    }, 900);
    const sc = [...L.matchAll(/STUB SendInput key vk 0x[0-9A-F]+ scan 0x([0-9A-F]+)( ext)? (down|up)/g)].map(m => m[1] + (m[3] === 'down' ? '+' : '-'));
    const setAt = L.indexOf('clipboard set on the PC (26 bytes)'), vAt = L.indexOf('scan 0x2F down');
    check('d Ctrl+V: this computer\'s text goes to the PC clipboard first, then Ctrl+V is pressed there', setAt >= 0 && vAt > setAt && sc.join(' ') === '1D+ 2F+ 2F- 1D-', `${sc.join(' ')}; set ${setAt >= 0}`);
    // ... no paste event (nothing to paste here): the PC pastes its own clipboard
    L = await logAfter(async () => { await ev(`(pasteKey({ctrlKey: true, code: 'KeyV'}), 1)`); await sleep(200); await ctx.key('keyUp', 'KeyV', 'v', 0); }, 500);
    check('d Ctrl+V with nothing here: the V still reaches the PC', /scan 0x2F down/.test(L) && !/clipboard set on the PC/.test(L));
    // d6. the PC -> this computer (Ctrl+C there)
    await ev('(window.__wrote.length = 0, 1)');
    L = await logAfter(async () => { await ctx.key('keyDown', 'ControlLeft', 'Control', 2); await ctx.press('KeyC', 'c', 2); await ctx.key('keyUp', 'ControlLeft', 'Control', 0); }, 2500);
    const wrote = await ev('window.__wrote.slice()');
    const tt = await ev(`$('toast').textContent`);
    check('d Ctrl+C on the PC: its text comes to this computer\'s clipboard (writeText; caught by the test)', /the test clipboard now "copied on the PC \d+"/.test(L) && wrote.some(w => /^copied on the PC \d+$/.test(w)) && tt === (await ev(`t('clipFromPc')`)), `${JSON.stringify(wrote)} / ${tt}`);
    await shot(`pc_clip_from_pc_${ctx.lang}`);
    check('d the clipboard that was on the PC at the start was not copied here', !wrote.includes('on this PC before'), JSON.stringify(wrote));
  }

  const next = new URL('./pcmouse0835_more7.mjs', import.meta.url);
  const fsm = await import('node:fs');
  if (fsm.existsSync(next)) { const mod = await import(next.href); await mod.run(ctx, want); }
}
