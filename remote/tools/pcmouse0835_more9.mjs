// 0.8.3.5 checks, part 9 (run by pcmouse0835_more8.mjs): x = the three small ones after the
// competitor study: #6 遊戲模式's own shortcuts, #8 the keyboard lock refused / missing, #9 the stats
// HUD's shortcut and quick switches (PC bar and phone bar).
export async function run(ctx, want) {
  if (!want('x') || ctx.before) return;
  const {s, ev, check, sleep, logAfter, shot, mouse} = ctx;
  const {key, press} = ctx;
  const scans = L => [...L.matchAll(/STUB SendInput key vk 0x[0-9A-F]+ scan 0x([0-9A-F]+)( ext)? (down|up)/g)].map(m => `${m[1]}${m[2] ? 'e' : ''}${m[3] === 'down' ? '+' : '-'}`);
  const chord = async code => {
    await key('keyDown', 'ControlLeft', 'Control', 2); await key('keyDown', 'AltLeft', 'Alt', 3); await key('keyDown', 'ShiftLeft', 'Shift', 11);
    await press(code, code[3].toLowerCase(), 11);
    await key('keyUp', 'ShiftLeft', 'Shift', 3); await key('keyUp', 'AltLeft', 'Alt', 2); await key('keyUp', 'ControlLeft', 'Control', 0);
  };
  await ctx.click(0.5, 0.5);
  await sleep(300);

  // #9 stats: Ctrl+Alt+Shift+S, the PC bar's 統計, the phone bar's 統計
  const hud0 = await ev('!!prefs.hud');
  let L = await logAfter(() => chord('KeyS'), 700);
  const hud1 = await ev(`({on: !!prefs.hud, shown: getComputedStyle(hud).display, btn: $('pbHud').classList.contains('on'), sw: $('swHud').checked})`);
  check('#9 Ctrl+Alt+Shift+S turns the stats on (HUD shown, 設定 switch and bar button follow)', !hud0 && hud1.on && hud1.shown === 'block' && hud1.btn && hud1.sw, JSON.stringify(hud1));
  const sc = scans(L);
  check('#6/#9 the page\'s shortcut is not sent: no S at the PC, Ctrl / Alt / Shift let go there', !sc.includes('1F+') && sc.includes('1D-') && sc.includes('38-') && sc.includes('2A-'), sc.join(' '));
  await sleep(1500);
  await ev(`(() => { showPcbar(0); return 1; })()`);
  await sleep(300);
  await shot(`x_pc_bar_stats_on_${ctx.lang}`);
  await ev(`($('pbHud').click(), 1)`);
  await sleep(300);
  check('#9 the PC bar\'s 統計 turns it off again', (await ev('!prefs.hud && getComputedStyle(hud).display === "none" && !$("pbHud").classList.contains("on")')) === true);

  // #6 遊戲模式: the first time says its shortcuts; Ctrl+Alt+Shift+Q frees the mouse; +M next screen
  await ev(`(() => { localStorage.removeItem('pm.gameKeys0835'); $('toast').style.display = 'none'; showPcbar(0); return 1; })()`);
  await sleep(300);
  const r = await ev(`(() => { const b = $('pbLock').getBoundingClientRect(); return {x: b.left + b.width / 2, y: b.top + b.height / 2}; })()`);
  await mouse('mouseMoved', r.x, r.y); await mouse('mousePressed', r.x, r.y, 'left'); await mouse('mouseReleased', r.x, r.y, 'left');
  await sleep(800);
  const g = await ev(`({locked: locked(), toast: $('toast').style.display === 'block' ? $('toast').textContent : '', hint: $('lockHint').textContent})`);
  check('#6 first 遊戲模式: the shortcuts are told (Q / M / S), the hint names Ctrl+Alt+Shift+Q', g.locked && g.toast === (await ev(`t('gameKeys')`)) && /Ctrl\+Alt\+Shift\+Q/.test(g.hint), JSON.stringify(g).slice(0, 160));
  await shot(`x_gamemode_first_${ctx.lang}`);
  L = await logAfter(() => chord('KeyQ'), 800);
  check('#6 Ctrl+Alt+Shift+Q frees the mouse (and Q is not sent)', (await ev('locked()')) === false && !scans(L).includes('10+'), scans(L).join(' '));
  // screens: with one screen it says so; with two (made up) it switches
  await ev(`(() => { S.monitors = [{primary: true}]; $('toast').style.display = 'none'; return 1; })()`);
  await chord('KeyM'); await sleep(300);
  check('#6 Ctrl+Alt+Shift+M with one screen: said so', (await ev(`$('toast').textContent === t('monOnly')`)) === true);
  await ev(`(() => { S.monitors = [{primary: true}, {primary: false}]; return 1; })()`);
  L = await logAfter(() => chord('KeyM'), 600);
  check('#6 Ctrl+Alt+Shift+M with two screens: the next one (and M is not sent)', (await ev('prefs.mon')) === 1 && (await ev(`$('toast').textContent === tf('monNext', 2)`)) && !scans(L).includes('32+'));
  await ctx.waitFor(() => ev('window.__pmFirstFrame && dc && dc.readyState === "open" ? 1 : 0'), 20000);
  await ev(`(() => { prefs.mon = 0; store.set('mon', 0); connect('switch'); return 1; })()`);
  await sleep(2500);
  await ctx.waitFor(() => ev('dc && dc.readyState === "open" ? 1 : 0'), 20000);
  await ev(`(() => { const o = dc.send.bind(dc); window.__sent = []; dc.send = m => { if (__sent.length < 20000) __sent.push([performance.now(), m]); return o(m); }; return 1; })()`);
  await sleep(1200);
  // an ordinary Ctrl+Alt+Shift+<other key> still goes to the PC
  L = await logAfter(() => chord('KeyT'), 700);
  check('#6 Ctrl+Alt+Shift+T (not ours) still reaches the PC', scans(L).includes('14+'), scans(L).join(' '));

  // #8 the keyboard lock refused, then missing: the card (what does not get through, the bar's buttons, Esc), 再試一次
  const fsVia = async () => { await ev(`(() => { showPcbar(0); return 1; })()`); await sleep(250); const b = await ev(`(() => { const r = $('pbFs').getBoundingClientRect(); return {x: r.left + r.width / 2, y: r.top + r.height / 2}; })()`);
    await mouse('mouseMoved', b.x, b.y); await mouse('mousePressed', b.x, b.y, 'left'); await mouse('mouseReleased', b.x, b.y, 'left'); await sleep(800); };
  await ev(`(() => { window.__klReal = navigator.keyboard.lock.bind(navigator.keyboard); navigator.keyboard.lock = () => Promise.reject(new DOMException('refused', 'NotAllowedError')); return 1; })()`);
  await fsVia();
  let c = await ev(`({card: $('pcCard').style.display, t: $('pcT').textContent, s: $('pcS').textContent, b1: $('pcB1').textContent, kl: S.kl, fs: !!document.fullscreenElement})`);
  check('#8 keyboard lock refused: the card says which keys do not get through, the bar\'s buttons, Esc; 再試一次', c.card === 'block' && c.kl === 'refused' && /Alt\+Tab/.test(c.s) && /Esc/.test(c.s) && c.b1 === (await ev(`t('klRetry')`)), JSON.stringify(c).slice(0, 200));
  await shot(`x_pc_keyboardlock_refused_${ctx.lang}`);
  await ev(`(() => { navigator.keyboard.lock = window.__klReal; return 1; })()`);
  const b1 = await ev(`(() => { const r = $('pcB1').getBoundingClientRect(); return {x: r.left + r.width / 2, y: r.top + r.height / 2}; })()`);
  await mouse('mouseMoved', b1.x, b1.y); await mouse('mousePressed', b1.x, b1.y, 'left'); await mouse('mouseReleased', b1.x, b1.y, 'left');
  await sleep(800);
  c = await ev(`({card: $('pcCard').style.display, kl: S.kl, toast: $('toast').textContent})`);
  check('#8 再試一次 (the lock allowed now): card gone, 「長按 Esc 會離開全螢幕」', c.card === 'none' && c.kl === 'ok' && c.toast === (await ev(`t('fsLocked')`)) && /Esc/.test(c.toast), JSON.stringify(c).slice(0, 200));
  await ev('(document.exitFullscreen(), 1)'); await sleep(500);
  await ev(`(() => { window.__kb = navigator.keyboard; Object.defineProperty(navigator, 'keyboard', {configurable: true, value: undefined}); return 1; })()`);
  await fsVia();
  c = await ev(`({card: $('pcCard').style.display, kl: S.kl})`);
  check('#8 no keyboard lock in this browser (Safari / Firefox): the same card', c.card === 'block' && c.kl === 'missing', JSON.stringify(c));
  await ev(`($('pcB2').click(), 1)`);
  check('#8 知道了 closes it', (await ev(`$('pcCard').style.display`)) === 'none');
  await ev(`(() => { Object.defineProperty(navigator, 'keyboard', {configurable: true, value: window.__kb}); return 1; })()`);
  if (await ev('!!document.fullscreenElement')) await ev('(document.exitFullscreen(), 1)');
  await sleep(400);

  // #9 on the phone: the 統計 button on the phone bar
  await ctx.asPhone(390, 844, true);
  await ev(`(() => { $('modeHint').style.display = 'none'; document.body.classList.remove('hint-open'); if (prefs.hud) toggleHud(); return 1; })()`);
  const pb = await ev(`(() => { const r = $('bHud').getBoundingClientRect(); return {x: r.left + r.width / 2, y: r.top + r.height / 2, w: r.width, right: $('bar').getBoundingClientRect().left}; })()`);
  await s('Input.dispatchTouchEvent', {type: 'touchStart', touchPoints: [{x: pb.x, y: pb.y}]}); await sleep(40); await s('Input.dispatchTouchEvent', {type: 'touchEnd', touchPoints: []});
  await sleep(1500);
  const ph = await ev(`({on: !!prefs.hud, shown: getComputedStyle(hud).display, btn: $('bHud').classList.contains('on'), barLeft: $('bar').getBoundingClientRect().left})`);
  check('#9 phone: the 統計 button on the bar turns the stats on (the bar still fits: 6 buttons)', ph.on && ph.shown === 'block' && ph.btn && ph.barLeft >= 0, JSON.stringify(ph));
  await shot(`x_phone_stats_on_${ctx.lang}`);
  await ev(`(() => { if (prefs.hud) toggleHud(); return 1; })()`);
  await s('Emulation.setTouchEmulationEnabled', {enabled: false});
  await s('Emulation.setDeviceMetricsOverride', {width: ctx.W, height: ctx.H, deviceScaleFactor: 1, mobile: false});
}
