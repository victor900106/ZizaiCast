// 0.8.3.5 checks, part 4 (run by pcmouse0835_more3.mjs): b (a PC's keyboard).
export async function run(ctx, want) {
  const {s, ev, check, sleep, logAfter, shot, mouse} = ctx;
  const VK = {ShiftLeft: 16, ControlLeft: 17, AltLeft: 18, MetaLeft: 91, Enter: 13, Escape: 27, Delete: 46, F5: 116, Tab: 9, KeyW: 87};
  const key = async (type, code, keyName, mods = 0, text) => {
    const vk = VK[code] || (code.startsWith('Key') ? code.charCodeAt(3) : code.startsWith('Digit') ? code.charCodeAt(5) : 0);
    await s('Input.dispatchKeyEvent', {type, code, key: keyName, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk, modifiers: mods, ...(text ? {text, unmodifiedText: text} : {})});
  };
  const press = async (code, keyName, mods = 0, text) => { await key('keyDown', code, keyName, mods, text); await key('keyUp', code, keyName, mods); };
  ctx.key = key; ctx.press = press;
  const scans = L => [...L.matchAll(/STUB SendInput key vk 0x[0-9A-F]+ scan 0x([0-9A-F]+)( ext)?( unicode)? (down|up)/g)].map(m => `${m[1]}${m[2] ? 'e' : ''}${m[3] ? 'u' : ''}${m[4] === 'down' ? '+' : '-'}`);

  if (want('b')) {
    // focus: a click on the picture (the PC gets the keyboard)
    await ctx.click(0.5, 0.5);
    await sleep(300);
    // b1. letters by their place: h e l l o, and Shift+1
    let L = await logAfter(async () => {
      for (const c of 'hello') await press('Key' + c.toUpperCase(), c, 0, c);
      await key('keyDown', 'ShiftLeft', 'Shift', 8); await press('Digit1', '!', 8, '!'); await key('keyUp', 'ShiftLeft', 'Shift', 0);
    }, 800);
    const sc = scans(L);
    check('b keys by scan code: h e l l o = 23 12 26 26 18 (down / up each)', sc.slice(0, 10).join(' ') === '23+ 23- 12+ 12- 26+ 26- 26+ 26- 18+ 18-', sc.slice(0, 10).join(' '));
    check('b Shift held while 1 is pressed (2A+ 02+ 02- 2A-)', sc.slice(10).join(' ') === '2A+ 02+ 02- 2A-', sc.slice(10).join(' '));
    check('b the PC log never says which keys (passwords): only a count', /input \(stub\): keys x\d+/.test(L) && !/KeyH|KeyE|Digit1/.test(L.replace(/STUB SendInput[^\n]*/g, '')));
    // b2. shortcuts go to the PC, not this browser
    await ev(`(() => { window.__dp = []; window.addEventListener('keydown', e => __dp.push(e.code + ':' + e.defaultPrevented), true); return 1; })()`);
    L = await logAfter(async () => {
      for (const [c, k] of [['KeyC', 'c'], ['KeyZ', 'z'], ['KeyA', 'a'], ['KeyF', 'f'], ['KeyS', 's']]) { await key('keyDown', 'ControlLeft', 'Control', 2); await press(c, k, 2); await key('keyUp', 'ControlLeft', 'Control', 0); }
      await press('F5', 'F5'); await press('Tab', 'Tab');
    }, 800);
    const dp = await ev('__dp.join(" ")');
    check('b Ctrl+C / Z / A / F / S, F5, Tab: the browser does nothing (defaultPrevented)', dp && !/:false/.test(dp) && dp.split(' ').length >= 12, dp);
    const sc2 = scans(L);
    check('b ... and the PC gets them (Ctrl = 1D around C = 2E ...)', sc2.slice(0, 4).join(' ') === '1D+ 2E+ 2E- 1D-' && sc2.includes('3F+') && sc2.includes('0F+'), sc2.slice(0, 8).join(' '));
    // b3. modifiers never stay down: the window loses the focus while Ctrl is held
    L = await logAfter(async () => { await key('keyDown', 'ControlLeft', 'Control', 2); await key('keyDown', 'AltRight', 'Alt', 3); await sleep(100); await ev(`(window.dispatchEvent(new Event('blur')), 1)`); }, 900);
    const sc3 = scans(L);
    check('b window loses the focus with Ctrl + AltGr held -> both let go on the PC', sc3.includes('1D-') && sc3.includes('38e-'), sc3.join(' '));
    // ... and the page dies with Shift held (no key up, no heartbeat): the PC lets go after 1.5 s
    L = await logAfter(async () => { await key('keyDown', 'ShiftLeft', 'Shift', 8); await sleep(150); await ev('(keysDown.clear(), 1)'); }, 3200);
    check('b the page goes quiet with Shift held -> the PC lets go by itself (1.5 s)', /key\(s\) let go \(the page went quiet for 1\.5 s\)/.test(L) && scans(L).includes('2A-'), scans(L).join(' '));
    await key('keyUp', 'ShiftLeft', 'Shift', 0);
    // ... held on purpose (heartbeat): still down after 2.5 s, then up
    L = await logAfter(async () => { await key('keyDown', 'ShiftLeft', 'Shift', 8); await sleep(2500); await key('keyUp', 'ShiftLeft', 'Shift', 0); }, 500);
    check('b a key held on purpose stays down (heartbeat) until it is let go', scans(L).join(' ') === '2A+ 2A-' && !/let go/.test(L), scans(L).join(' '));
    // b4. this PC's input method (Chinese): composition, then the text
    await ctx.click(0.5, 0.5);
    L = await logAfter(async () => {
      await s('Input.imeSetComposition', {text: 'ㄋㄧˇ', selectionStart: 3, selectionEnd: 3});
      await sleep(80);
      await s('Input.imeSetComposition', {text: '你', selectionStart: 1, selectionEnd: 1});
      await sleep(80);
      await s('Input.insertText', {text: '你好'});
    }, 800);
    const uni = scans(L).filter(x => x.includes('u'));
    check('b IME (注音 composition -> 你好): the PC gets the text once, not the keys of the composition', /input \(stub\): text \(6 bytes\)/.test(L) && (L.match(/text \(\d+ bytes\)/g) || []).length === 1 && uni.length === 4,
      `${(L.match(/text \(\d+ bytes\)/g) || []).join(',')} ${uni.length} unicode events`);
    const L2 = await logAfter(async () => { await s('Input.imeSetComposition', {text: 'にほん', selectionStart: 3, selectionEnd: 3}); await sleep(60); await s('Input.insertText', {text: '日本'}); }, 700);
    check('b IME (Japanese にほん -> 日本)', /text \(6 bytes\)/.test(L2), (L2.match(/text \(\d+ bytes\)/g) || []).join(','));
    // b5. the bar's buttons for what a browser never lets a page have
    await ev(`(() => { showPcbar(0); return 1; })()`);
    await sleep(300);
    await shot(`pc_bar_keys_${ctx.lang}`);
    L = await logAfter(async () => { for (const c of ['Alt+Tab', 'Meta', 'Control+W']) await ev(`(document.querySelector('#pcbar button[data-c="${c}"]').click(), 1)`); }, 700);
    check('b bar: Alt+Tab, Win, Ctrl+W reach the PC', /combo Alt\+Tab/.test(L) && /combo Meta/.test(L) && /combo Control\+W/.test(L));
    await ev(`(() => { $('toast').style.display = 'none'; return 1; })()`);
    L = await logAfter(() => ev(`(document.querySelector('#pcbar button[data-c="Control+Alt+Delete"]').click(), 1)`), 700);
    const tt = await ev(`$('toast').style.display === 'block' ? $('toast').textContent : ''`);
    check('b bar: Ctrl+Alt+Del -> said plainly that no program can send it (and what to do)', /cannot be sent by any program/.test(L) && tt === (await ev(`t('sasNo')`)), tt.slice(0, 40));
    await shot(`pc_bar_sas_${ctx.lang}`);
    // typed Ctrl+Alt+Del: the same
    L = await logAfter(async () => { await key('keyDown', 'ControlLeft', 'Control', 2); await key('keyDown', 'AltLeft', 'Alt', 3); await press('Delete', 'Delete', 3); await key('keyUp', 'AltLeft', 'Alt', 2); await key('keyUp', 'ControlLeft', 'Control', 0); }, 700);
    check('b typed Ctrl+Alt+Del: Delete not sent, said plainly', /cannot be sent by any program/.test(L) && !scans(L).includes('53e+'), scans(L).join(' '));
    // b6. a text field of the page keeps its keys (the settings sheet / password)
    L = await logAfter(async () => { await ev(`(() => { $('bKb').onclick(); return 1; })()`); await sleep(200); await ev(`($('txt').focus(), 1)`); await press('KeyQ', 'q', 0, 'q'); await ev(`(() => { showKb(false); return 1; })()`); }, 600);
    check('b typing in the page\'s own field is not sent as keys', !scans(L).includes('10+'), scans(L).join(' '));
  }

  const next = new URL('./pcmouse0835_more5.mjs', import.meta.url);
  const fsm = await import('node:fs');
  if (fsm.existsSync(next)) { const mod = await import(next.href); await mod.run(ctx, want); }
}
