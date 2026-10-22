// 0.8.4 自訂快捷鍵 end to end (node 22+): the phone page against a real host, input into the STUB (never the PC).
//   node remote/tools/keys084_test.mjs <pm_remote_spike.exe> <outDir>
// Spike: --loopback (127.0.0.2/.3 only), --no-tailscale, --inject-stub (SendInput replaced by a logging stub: no real key
// is ever sent), --scene static, no sound (--no-audio), --keys-file (the settings.ini stand-in), the fixed link with the
// owner's password and one 臨時帳號 (可操作). The phone is ONE chrome-headless-shell (no window, no Alt+Tab entry, never the
// focus; quiet_browser.mjs), muted, with its own throw-away profile; a watcher counts Alt+Tab / taskbar windows of every
// process the test starts (must stay 0). Only these processes are closed.
// Run 1: the owner sees the 3 examples; adds a button by ticking (Ctrl+Shift+N) and by "recording" keys (Win+V pressed on a
// PC keyboard); a media key (volume up); renames; drags to reorder; deletes; the files / log show what the PC kept and
// the stub got; a held button repeats and stops on release; the PC refuses a 13th button, an unknown key, a doubled key.
// Run 2: the spike restarts with the same file: another phone (cleared storage) sees the same list (a new device). A
// 臨時帳號 sees it, uses it (stub gets the combo), cannot edit (no 編輯 / ＋, a keys-set is refused, the file stays).
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import {launch, cdpClient, watchAltTab} from './quiet_browser.mjs';

const [spikeExe, outDir] = process.argv.slice(2);
if (!spikeExe || !outDir) { console.error('usage: node keys084_test.mjs <pm_remote_spike.exe> <outDir>'); process.exit(2); }
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const OWNER = 'Owner-Test-2026', GC = 'GHJ789';
const results = [];
let failed = 0;
const check = (name, ok, detail = '') => { results.push({name, ok: !!ok, detail}); if (!ok) failed++; console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail !== '' ? '  -- ' + detail : ''}`); };
const profileDir = path.join(os.tmpdir(), 'shell-084keys-' + crypto.randomBytes(3).toString('hex'));
const keysFile = path.join(outDir, 'keys_settings.json');
fs.rmSync(keysFile, {force: true});
const br = await launch(profileDir, ['--allow-loopback-in-peer-connection']);
const watch = watchAltTab([br.proc.pid, process.pid], path.join(outDir, 'alttab_keys.txt'));
const C = await cdpClient(br.ver.webSocketDebuggerUrl);
const cdp = C.cdp;
const readKeys = () => { try { return JSON.parse(fs.readFileSync(keysFile, 'utf8')); } catch { return null; } };

async function run(tag, body) {
  const token = crypto.randomBytes(16).toString('hex');
  const port = 19700 + Math.floor(Math.random() * 200);
  const logFile = path.join(outDir, `spike_${tag}.log`);
  fs.writeFileSync(logFile, '');
  const readLog = () => fs.readFileSync(logFile, 'utf8');
  const spike = spawn(spikeExe, ['--token', token, '--port', String(port), '--log', logFile, '--duration', '240', '--stdin-quit', '--loopback', '--no-tailscale',
    '--scene', 'static', '--no-audio', '--inject-stub', '--keys-file', keysFile, '--fixed-password', OWNER, '--guest', `g-ctl:${GC}:3600:control`],
    {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  watch.add(spike.pid);
  spike.stdout.on('data', () => {}); spike.stderr.on('data', () => {});
  for (let i = 0; i < 150 && !/fixed: origin/.test(readLog()); i++) await sleep(100);
  const waitFor = async (fn, ms) => { for (let t = 0; t < ms; t += 200) { const v = await fn(); if (v) return v; await sleep(200); } return null; };
  async function phone(user, pw) {
    await cdp('Storage.clearDataForOrigin', {origin: `http://127.0.0.2:${port}`, storageTypes: 'local_storage'});
    const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
    const {sessionId} = await cdp('Target.attachToTarget', {targetId, flatten: true});
    await cdp('Emulation.setDeviceMetricsOverride', {width: 390, height: 1300, deviceScaleFactor: 1, mobile: true}, sessionId);
    await cdp('Page.navigate', {url: `http://127.0.0.2:${port}/${user ? '#u=' + user : ''}`}, sessionId);
    const ev = async (expression, awaitPromise = false) => { const r = await cdp('Runtime.evaluate', {expression, returnByValue: true, awaitPromise}, sessionId); return r && r.result ? r.result.value : undefined; };
    await sleep(300);
    await ev(`(() => { try { localStorage.clear(); sessionStorage.clear(); } catch {} location.reload(); return 1; })()`);
    await sleep(800);
    await waitFor(() => ev(`$('pass').style.display === 'flex'`), 8000);
    await ev(`(() => { $('passIn').value = ${JSON.stringify(pw)}; submitPass(); return 1; })()`);
    const frame = !!(await waitFor(() => ev('window.__pmFirstFrame ? 1 : 0'), 25000));
    await waitFor(() => ev(`(typeof dc !== 'undefined' && dc && dc.readyState === 'open' && typeof S.keysOk === 'boolean' && S.keys.length >= 0) ? 1 : 0`), 10000);
    await sleep(1200);
    await ev(`(() => { showKb(true); return 1; })()`);
    const labels = () => ev(`[...document.querySelectorAll('#ckList .ck')].map(b => b.dataset.c)`);
    const center = (sel, scroll = true) => ev(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return null; if (${scroll}) e.scrollIntoView({block: 'center'}); const r = e.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()`);
    const mouse = (type, x, y) => cdp('Input.dispatchMouseEvent', {type, x, y, button: 'left', buttons: type === 'mouseReleased' ? 0 : 1, clickCount: 1}, sessionId);
    const click = async sel => { const p = await center(sel); await sleep(100); await mouse('mousePressed', p[0], p[1]); await sleep(60); await mouse('mouseReleased', p[0], p[1]); await sleep(250); };
    const shot = async name => { const r = await cdp('Page.captureScreenshot', {format: 'png'}, sessionId); if (r && r.data) fs.writeFileSync(path.join(outDir, `${tag}_${name}.png`), Buffer.from(r.data, 'base64')); };
    return {ev, frame, labels, center, mouse, click, shot, close: () => cdp('Target.closeTarget', {targetId})};
  }
  const count = re => (readLog().match(re) || []).length;
  try { await body({phone, readLog, count, waitFor}); }
  catch (e) { check(`${tag}: ran`, false, String(e && e.stack || e)); }
  try { spike.stdin.end(); } catch {}
  for (let i = 0; i < 50 && spike.exitCode === null; i++) await sleep(100);
  if (spike.exitCode === null) try { execFileSync('taskkill', ['/PID', String(spike.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
  await sleep(500);
  check(`${tag}: nothing was really sent (stub only)`, readLog().includes('STUB SendInput') ? !/input: SendInput\(/.test(readLog()) : true);
}

await run('owner', async ({phone, readLog, count, waitFor}) => {
  const o = await phone('', OWNER);
  check('owner: connected', o.frame);
  const st = await o.ev(`({ok: S.keysOk, keys: S.keys.map(k => k.c.join('+') + '|' + (k.t || '')), edit: $('ckEditB').style.display !== 'none'})`);
  check('owner: the 3 examples (Win+V, Ctrl+Shift+T, Win+L), may edit', st.ok && st.edit && st.keys.join(',') === 'Meta+V|dClip,Control+Shift+T|dReopen,Meta+L|dLock', JSON.stringify(st));
  await o.shot('1_defaults');
  // use an example: the stub gets Win+V
  await o.click('#ckList .ckc:nth-child(1) .ck');
  check('a custom button sends its combination (stub: Meta+V)', await waitFor(() => /input \(stub\): combo Meta\+V/.test(readLog()), 3000));
  // add by ticking: Ctrl+Shift+N named "New window"
  await o.click('#ckAdd');
  await o.ev(`(() => { $('ckName').value = 'New window'; return 1; })()`);
  await o.click('[data-cm="Control"]'); await o.click('[data-cm="Shift"]');
  await o.ev(`(() => { $('ckKey').value = 'N'; $('ckKey').onchange(); return 1; })()`);
  await o.shot('2_editor_ticked');
  await o.click('#ckSave');
  await waitFor(() => readKeys() && readKeys().length === 4, 4000);
  let k = readKeys();
  check('add by ticking: the PC kept it (4 buttons, Control+Shift+N, name)', k && k.length === 4 && k[3].c.join('+') === 'Control+Shift+N' && k[3].n === 'New window', JSON.stringify(k && k[3]));
  // add by recording: a PC keyboard's Win+V ... as Ctrl+Alt+F5 here (Win+V is already there): events like a keyboard's
  await o.click('#ckAdd');
  await o.click('#ckRec');
  const recOn = await o.ev(`ckRecOn`);
  await o.ev(`(() => { window.dispatchEvent(new KeyboardEvent('keydown', {key: 'Control', code: 'ControlLeft', ctrlKey: true, bubbles: true, cancelable: true}));
    window.dispatchEvent(new KeyboardEvent('keydown', {key: 'Alt', code: 'AltLeft', ctrlKey: true, altKey: true, bubbles: true, cancelable: true}));
    window.dispatchEvent(new KeyboardEvent('keydown', {key: 'F5', code: 'F5', ctrlKey: true, altKey: true, bubbles: true, cancelable: true})); return 1; })()`);
  const rec = await o.ev(`({on: ckRecOn, prev: $('ckPrev').textContent, sent: S.keyMsgs || 0})`);
  check('recording: Ctrl+Alt+F5 pressed on a keyboard is taken (and not sent to the PC)', recOn && !rec.on && rec.prev === 'Ctrl+Alt+F5' && !/kd/.test(readLog().slice(-2000).replace(/combo/g, '')), JSON.stringify(rec));
  // recording the Win key alone
  await o.click('#ckRec');
  await o.ev(`(() => { window.dispatchEvent(new KeyboardEvent('keydown', {key: 'Meta', code: 'MetaLeft', metaKey: true, bubbles: true, cancelable: true}));
    window.dispatchEvent(new KeyboardEvent('keyup', {key: 'Meta', code: 'MetaLeft', bubbles: true, cancelable: true})); return 1; })()`);
  check('recording: the Win key alone is taken (Win)', (await o.ev(`$('ckPrev').textContent`)) === 'Win');
  // back to Ctrl+Alt+F5 through the list, then save with a media-free name
  await o.ev(`(() => { ckMods.clear(); ckMods.add('Control'); ckMods.add('Alt'); ckKey = 'F5'; ckSyncEd(); $('ckName').value = ''; return 1; })()`);
  await o.click('#ckSave');
  // a media key
  await o.click('#ckAdd');
  await o.ev(`(() => { ckMods.clear(); $('ckKey').value = 'AudioVolumeUp'; $('ckKey').onchange(); $('ckName').value = 'Vol+'; return 1; })()`);
  await o.shot('3_editor_media');
  await o.click('#ckSave');
  await waitFor(() => readKeys() && readKeys().length === 6, 4000);
  k = readKeys();
  check('add by recording + a media key: the PC kept 6 buttons (Ctrl+Alt+F5, AudioVolumeUp)', k && k.length === 6 && k[4].c.join('+') === 'Control+Alt+F5' && k[5].c.join('+') === 'AudioVolumeUp', JSON.stringify(k && k.slice(4)));
  // long press: volume up repeats while held, stops on release
  const before = count(/combo AudioVolumeUp/g);
  const p = await o.center('#ckList .ckc:nth-child(6) .ck');
  await o.mouse('mousePressed', p[0], p[1]);
  await sleep(1500);
  const during = count(/combo AudioVolumeUp/g) - before;
  await o.mouse('mouseReleased', p[0], p[1]);
  await sleep(300);
  const after1 = count(/combo AudioVolumeUp/g);
  await sleep(500);
  const after2 = count(/combo AudioVolumeUp/g);
  check('long press repeats (volume up x' + during + ' in 1.5 s), and stops on release', during >= 6 && after1 === after2, `during ${during}, after release ${after1} -> ${after2}`);
  // a short tap sends exactly one
  const b2 = count(/combo AudioVolumeUp/g);
  await o.click('#ckList .ckc:nth-child(6) .ck');
  check('a short tap sends exactly one', count(/combo AudioVolumeUp/g) === b2 + 1);
  // a fixed button repeats too (arrow key: the host gets "key ArrowRight" many times)
  const bk = count(/input \(stub\): key ArrowRight/g);
  const pa = await o.center('#keys button[data-k="ArrowRight"]');
  await o.mouse('mousePressed', pa[0], pa[1]); await sleep(1300); await o.mouse('mouseReleased', pa[0], pa[1]); await sleep(300);
  check('the fixed → button repeats while held', count(/input \(stub\): key ArrowRight/g) - bk >= 5, String(count(/input \(stub\): key ArrowRight/g) - bk));
  // edit mode: drag the 5th button (handle) to the front; rename; delete
  await o.click('#ckEditB');
  await o.shot('4_edit_mode');
  const h = await o.center('#ckList .ckc:nth-child(5) .ckh'), t0 = await o.center('#ckList .ckc:nth-child(1) .ck', false);
  await o.mouse('mousePressed', h[0], h[1]);
  for (let i = 1; i <= 8; i++) { await o.mouse('mouseMoved', h[0] + (t0[0] - 20 - h[0]) * i / 8, h[1] + (t0[1] - h[1]) * i / 8); await sleep(30); }
  await o.mouse('mouseReleased', t0[0] - 20, t0[1]);
  await waitFor(() => { const kk = readKeys(); return kk && kk[0].c.join('+') === 'Control+Alt+F5'; }, 4000);
  k = readKeys();
  check('drag to reorder: Ctrl+Alt+F5 moved to the front, the PC kept the order', k && k.length === 6 && k[0].c.join('+') === 'Control+Alt+F5' && k[1].c.join('+') === 'Meta+V', JSON.stringify(k && k.map(x => x.c.join('+'))));
  await o.click('#ckList .ckc:nth-child(1) .ckx');
  await waitFor(() => readKeys() && readKeys().length === 5, 4000);
  k = readKeys();
  check('delete: the PC kept 5 (an example can be deleted too)', k && k.length === 5 && k[0].c.join('+') === 'Meta+V', JSON.stringify(k && k.map(x => x.c.join('+'))));
  await o.click('#ckList .ckc:nth-child(1) .ckx'); await o.click('#ckList .ckc:nth-child(1) .ckx'); await o.click('#ckList .ckc:nth-child(1) .ckx');
  await waitFor(() => readKeys() && readKeys().length === 2, 4000);
  check('delete all but the added ones: 2 left (New window, Vol+)', (readKeys() || []).map(x => x.n).join(',') === 'New window,Vol+', JSON.stringify(readKeys()));
  await o.click('#ckEditB');
  // the PC refuses what it does not allow
  const refuse = async (what, list) => {
    const n0 = count(/自訂快捷鍵 save REFUSED \(bad list/g);
    await o.ev(`(() => { send({t: 'keys-set', list: ${JSON.stringify(list)}}); return 1; })()`);
    await waitFor(() => count(/自訂快捷鍵 save REFUSED \(bad list/g) > n0, 3000);
    const kk = readKeys();
    check(`the PC refuses ${what}, the saved list is unchanged and the page is told`, count(/自訂快捷鍵 save REFUSED \(bad list/g) === n0 + 1 && kk && kk.length === 2 && (await o.ev(`S.keys.length`)) === 2);
  };
  await refuse('13 buttons', Array.from({length: 13}, () => ({n: '', c: ['A']})));
  await refuse('a key that is not in the table (Launch)', [{n: 'x', c: ['Control', 'Launch']}]);
  await refuse('two keys besides the modifiers', [{n: 'x', c: ['Control', 'A', 'B']}]);
  await refuse('a name with a control character', [{n: 'a\u0001b', c: ['A']}]);
  await o.shot('5_final');
  await o.close();
});

const saved = readKeys();
await run('again', async ({phone, readLog, count, waitFor}) => {
  const o = await phone('', OWNER);
  const st = await o.ev(`({keys: S.keys.map(k => k.n + '|' + k.c.join('+'))})`);
  check('after a restart, on another device (cleared storage): the same list', o.frame && st.keys.join(',') === (saved || []).map(k => k.n + '|' + k.c.join('+')).join(','), JSON.stringify(st) + ' vs ' + JSON.stringify(saved));
  await o.close();
  await sleep(2000);
  const g = await phone('g-ctl', GC);
  check('g-ctl (臨時帳號 可操作): connected and sees the same list', g.frame && (await g.labels()).join(',') === (saved || []).map(k => k.c.join('+')).join(','));
  const st2 = await g.ev(`({ok: S.keysOk, why: S.keysWhy, edit: $('ckEditB').style.display, add: $('ckAdd').style.display, note: $('ckNote').textContent})`);
  check('g-ctl: no 編輯 / ＋, with the reason', st2.ok === false && st2.why === 'guest' && st2.edit === 'none' && st2.add === 'none' && st2.note === (await g.ev(`t('ckNoGuest')`)), JSON.stringify(st2));
  await g.shot('6_guest');
  await g.click('#ckList .ckc:nth-child(1) .ck');
  check('g-ctl: can use a button (stub gets the combination)', await waitFor(() => readLog().includes('input (stub): combo ' + saved[0].c.join('+')), 3000));
  const before = JSON.stringify(readKeys());
  await g.ev(`(() => { send({t: 'keys-set', list: [{n: 'evil', c: ['Meta', 'R']}]}); return 1; })()`);
  await waitFor(() => /自訂快捷鍵 save REFUSED \(a 臨時帳號\)/.test(readLog()), 3000);
  await sleep(500);
  check('g-ctl: a save is refused on the PC, the list stays, the page keeps the PC list', /save REFUSED \(a 臨時帳號\)/.test(readLog()) && JSON.stringify(readKeys()) === before && (await g.ev(`S.keys.length`)) === (saved || []).length);
  await g.close();
});

C.close();
await br.close();
await sleep(500);
try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
const alt = await watch.stop();
check(`Alt+Tab / taskbar windows of the test's processes = 0 (${alt.samples} samples)`, alt.max === 0 && alt.samples > 0, `max ${alt.max}${alt.seen.length ? ': ' + alt.seen.join(' ; ') : ''}`);
fs.writeFileSync(path.join(outDir, 'keys084.json'), JSON.stringify(results, null, 1));
console.log(failed ? `FAILED (${failed})` : `ALL PASSED (${results.length})`);
process.exit(failed ? 1 : 0);
