// 0.8.4: a Mac (or an iPad with a keyboard) controls Windows the Mac way: ⌘ + a letter = Ctrl + that letter,
// ⌘ alone = Win, Option = Alt, a switch 「Mac：Cmd 當 Ctrl」 (on by default on a Mac / iPad).
//   node remote/tools/mac084_test.mjs <pm_remote_spike.exe> <outDir> [--shots DIR]
// ONE chrome-headless-shell (quiet_browser.mjs: no window, no Alt+Tab entry, never the focus) pretending to be a
// Mac's Chrome / Safari or an iPad (CDP user agent + navigator.userAgentData platform), its keys are CDP
// Input.dispatchKeyEvent INTO THAT PAGE only (with macOS's habit: no keyup for a letter while ⌘ is down). The spike
// runs --loopback, --no-tailscale, --no-audio, --scene static, --inject-stub: the injection path runs but SendInput
// is a stub that only logs (never a real key on this PC). Not a real Mac: what a real Mac's browser keeps for itself
// (⌘Q / ⌘W / ⌘Tab ...) is not tested here.
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import {launch, cdpClient, watchAltTab} from './quiet_browser.mjs';

const [spikeExe, outDir, ...rest] = process.argv.slice(2);
if (!spikeExe || !outDir) { console.error('usage: node mac084_test.mjs <pm_remote_spike.exe> <outDir> [--shots DIR]'); process.exit(2); }
let shots = '';
for (let i = 0; i < rest.length; i++) if (rest[i] === '--shots') shots = rest[++i];
fs.mkdirSync(outDir, {recursive: true});
if (shots) fs.mkdirSync(shots, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
let passed = 0, failed = 0;
const check = (name, ok, detail = '') => { ok ? passed++ : failed++; console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail !== '' ? '  -- ' + detail : ''}`); };

const br = await launch(path.join(os.tmpdir(), 'shell-084-mac-' + crypto.randomBytes(3).toString('hex')), ['--allow-loopback-in-peer-connection']);
const watch = watchAltTab([br.proc.pid, process.pid], path.join(outDir, 'alttab_mac.txt'));
const C = await cdpClient(br.ver.webSocketDebuggerUrl);
const token = crypto.randomBytes(16).toString('hex');
const port = 19300 + Math.floor(Math.random() * 300);
const logFile = path.join(outDir, 'spike_mac.log');
fs.writeFileSync(logFile, '');
const readLog = () => fs.readFileSync(logFile, 'utf8');
const spike = spawn(spikeExe, ['--token', token, '--port', String(port), '--log', logFile, '--duration', '300', '--stdin-quit', '--loopback', '--no-tailscale',
  '--scene', 'static', '--no-audio', '--test-clipboard', 'on the PC', '--inject-stub'], {stdio: ['pipe', 'ignore', 'ignore'], windowsHide: true});
watch.add(spike.pid);
let url = '';
for (let i = 0; i < 150 && !url; i++) { await sleep(100); const m = readLog().match(/urls: lan (http:\/\/127\.0\.0\.3:\d+\/[0-9a-f]+\/)/); if (m) url = m[1]; }
check('spike up on 127.0.0.3 (loopback, input stub)', !!url, url);

const MAC_UA = 'Mozilla/5.0 (Macintosh; Intel Mac OS X 15_6) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36';
const SAFARI_UA = 'Mozilla/5.0 (Macintosh; Intel Mac OS X 15_6) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.1 Safari/605.1.15';
const WIN_UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36';
const meta = p => ({brands: [{brand: 'Chromium', version: '155'}], fullVersion: '155.0.0.0', platform: p, platformVersion: '15.6.0', architecture: 'arm', model: '', mobile: false});

async function page({ua, platform, uaPlatform, touch = false, w = 1280, h = 720}) {
  const {targetId} = await C.cdp('Target.createTarget', {url: 'about:blank'});
  const {sessionId} = await C.cdp('Target.attachToTarget', {targetId, flatten: true});
  const s = (m, p) => C.cdp(m, p, sessionId);
  const ev = async (expression, awaitPromise = false) => { const r = await s('Runtime.evaluate', {expression, returnByValue: true, awaitPromise}); return r && r.result ? r.result.value : undefined; };
  await s('Emulation.setDeviceMetricsOverride', {width: w, height: h, deviceScaleFactor: touch ? 2 : 1, mobile: touch});
  if (touch) await s('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5});
  await s('Emulation.setUserAgentOverride', {userAgent: ua, platform, ...(uaPlatform ? {userAgentMetadata: meta(uaPlatform)} : {})});
  await s('Storage.clearDataForOrigin', {origin: new URL(url).origin, storageTypes: 'local_storage'});
  await s('Page.navigate', {url});
  for (let i = 0; i < 200 && !(await ev('window.__pmFirstFrame ? 1 : 0')); i++) await sleep(100);
  await sleep(800);
  // a click on the picture: the keyboard goes to the PC (like a mouse user does)
  await s('Input.dispatchMouseEvent', {type: 'mouseMoved', x: w / 2, y: h / 2});
  await s('Input.dispatchMouseEvent', {type: 'mousePressed', x: w / 2, y: h / 2, button: 'left', clickCount: 1});
  await s('Input.dispatchMouseEvent', {type: 'mouseReleased', x: w / 2, y: h / 2, button: 'left', clickCount: 1});
  await sleep(300);
  const VK = {ShiftLeft: 16, ControlLeft: 17, AltLeft: 18, MetaLeft: 91, MetaRight: 92, ArrowLeft: 37, F4: 115, Tab: 9};
  const MOD = {AltLeft: 1, ControlLeft: 2, MetaLeft: 4, MetaRight: 4, ShiftLeft: 8};
  let mods = 0;
  const key = async (type, code, keyName) => {
    const vk = VK[code] || (code.startsWith('Key') ? code.charCodeAt(3) : 0);
    if (type === 'keyDown' && MOD[code]) mods |= MOD[code];
    await s('Input.dispatchKeyEvent', {type: type === 'keyDown' ? 'rawKeyDown' : 'keyUp', code, key: keyName, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk, modifiers: mods});
    if (type === 'keyUp' && MOD[code]) mods &= ~MOD[code];
  };
  // ⌘ + letters the macOS way: ⌘ down, the letter down (no keyup while ⌘ is down), ⌘ up
  const cmdLetters = async (letters, {shift = false, mac = true} = {}) => {
    await key('keyDown', 'MetaLeft', 'Meta');
    if (shift) await key('keyDown', 'ShiftLeft', 'Shift');
    for (const c of letters) { await key('keyDown', 'Key' + c.toUpperCase(), c); if (!mac) await key('keyUp', 'Key' + c.toUpperCase(), c); await sleep(20); }
    if (shift) await key('keyUp', 'ShiftLeft', 'Shift');
    await sleep(30);
    await key('keyUp', 'MetaLeft', 'Meta');
  };
  const close = () => C.cdp('Target.closeTarget', {targetId});
  const shot = async name => { if (!shots) return; const r = await s('Page.captureScreenshot', {format: 'png'}); if (r && r.data) fs.writeFileSync(path.join(shots, name + '.png'), Buffer.from(r.data, 'base64')); };
  return {s, ev, key, cmdLetters, close, shot};
}
const scans = L => [...L.matchAll(/STUB SendInput key vk 0x[0-9A-F]+ scan 0x([0-9A-F]+)( ext)?( unicode)? (down|up)/g)].map(m => `${m[1]}${m[2] ? 'e' : ''}${m[4] === 'down' ? '+' : '-'}`);
async function logAfter(fn, wait = 700) { const mark = readLog().length; await fn(); await sleep(wait); return readLog().slice(mark); }
const SC = {C: '2E', V: '2F', X: '2D', Z: '2C', A: '1E', S: '1F', F: '21', T: '14', W: '11'};

try {
  // ---- a Mac's Chrome (userAgentData platform macOS) ----
  let P = await page({ua: MAC_UA, platform: 'MacIntel', uaPlatform: 'macOS'});
  check('Mac Chrome: detected, 「Mac：Cmd 當 Ctrl」 on by default', await P.ev('appleKb && prefs.cmdCtrl === true'));
  for (const c of 'CVXZASFTW') {
    const L = await logAfter(() => P.cmdLetters(c.toLowerCase()), c === 'V' ? 900 : 600);
    const sc = scans(L).join(' ');
    check(`Mac: ⌘${c} -> Ctrl+${c} on the PC (1D+ ${SC[c]}+ ${SC[c]}- 1D-, no Win)`, sc === `1D+ ${SC[c]}+ ${SC[c]}- 1D-`, sc);
  }
  let L = await logAfter(() => P.cmdLetters('zz'));
  check('Mac: ⌘ held, Z twice (repeat) -> Ctrl down once, Z tapped twice', scans(L).join(' ') === '1D+ 2C+ 2C- 2C+ 2C- 1D-', scans(L).join(' '));
  L = await logAfter(() => P.cmdLetters('s', {shift: true}));
  check('Mac: ⌘⇧S -> Ctrl+Shift+S (no Win)', (s => s.includes('2A+') && s.includes('1D+') && s.includes('1F+') && !s.includes('5Be+'))(scans(L).join(' ')), scans(L).join(' '));
  L = await logAfter(async () => { await P.key('keyDown', 'MetaLeft', 'Meta'); await sleep(40); await P.key('keyUp', 'MetaLeft', 'Meta'); });
  check('Mac: ⌘ alone -> the Win key (5Be+ 5Be-)', scans(L).join(' ') === '5Be+ 5Be-', scans(L).join(' '));
  L = await logAfter(async () => { await P.key('keyDown', 'MetaLeft', 'Meta'); await P.key('keyDown', 'ArrowLeft', 'ArrowLeft'); await sleep(30); await P.key('keyUp', 'MetaLeft', 'Meta'); });
  check('Mac: ⌘← -> Win+Left (snap), both let go when ⌘ comes up', scans(L).join(' ') === '5Be+ 4Be+ 4Be- 5Be-', scans(L).join(' '));
  L = await logAfter(async () => { await P.key('keyDown', 'AltLeft', 'Alt'); await P.key('keyDown', 'F4', 'F4'); await P.key('keyUp', 'F4', 'F4'); await P.key('keyUp', 'AltLeft', 'Alt'); });
  check('Mac: Option+F4 -> Alt+F4 (Option is Alt)', scans(L).join(' ') === '38+ 3E+ 3E- 38-', scans(L).join(' '));
  L = await logAfter(async () => { await P.key('keyDown', 'MetaLeft', 'Meta'); await P.key('keyDown', 'KeyC', 'c'); await P.ev(`(window.dispatchEvent(new Event('blur')), 1)`); });
  check('Mac: the window loses the focus with ⌘ held -> nothing stays down on the PC', (s => s.includes('1D+') && s.includes('1D-') && s.split(' ').filter(x => x.endsWith('+')).length === s.split(' ').filter(x => x.endsWith('-')).length)(scans(L).join(' ')), scans(L).join(' '));
  await P.ev(`$('bSet').onclick(); 1`); await sleep(300);
  check('Mac: the settings sheet shows 「Mac：Cmd 當 Ctrl」, switched on', await P.ev(`$('rowCmd').style.display !== 'none' && $('swCmd').checked && $('lCmd').textContent.length > 3`));
  await P.ev(`$('rowCmd').scrollIntoView({block: 'center'}); 1`); await sleep(200);
  await P.shot('after_mac_settings');
  await P.ev(`$('swCmd').checked = false; $('swCmd').onchange(); closeSheets(); 1`); await sleep(200);
  L = await logAfter(() => P.cmdLetters('c'));
  check('Mac, switched off: ⌘C -> Win+C as before, and C let go with ⌘ (macOS sends no keyup for it)', scans(L).join(' ') === '5Be+ 2E+ 2E- 5Be-', scans(L).join(' '));
  check('Mac: the switch is remembered (cmdCtrl084 = false)', await P.ev(`store.get('cmdCtrl084', null) === false`));
  await P.close();

  // ---- a Mac's Safari (no userAgentData) ----
  P = await page({ua: SAFARI_UA, platform: 'MacIntel'});
  check('Mac Safari (user agent only): detected, on by default', await P.ev('appleKb && prefs.cmdCtrl === true'));
  L = await logAfter(() => P.cmdLetters('a'));
  check('Mac Safari: ⌘A -> Ctrl+A', scans(L).join(' ') === '1D+ 1E+ 1E- 1D-', scans(L).join(' '));
  await P.close();

  // ---- an iPad with a keyboard (iPadOS reports a Mac: MacIntel + touch points) ----
  P = await page({ua: SAFARI_UA, platform: 'MacIntel', touch: true, w: 1180, h: 820});
  check('iPad (MacIntel + touch): detected, on by default', await P.ev('isIOS && appleKb && prefs.cmdCtrl === true'));
  L = await logAfter(() => P.cmdLetters('c'));
  check('iPad: ⌘C -> Ctrl+C', scans(L).join(' ') === '1D+ 2E+ 2E- 1D-', scans(L).join(' '));
  await P.close();

  // ---- Windows Chrome: unchanged (Win key = Meta = Win) ----
  P = await page({ua: WIN_UA, platform: 'Win32', uaPlatform: 'Windows'});
  check('Windows Chrome: not a Mac, the switch is off by default', await P.ev('!appleKb && prefs.cmdCtrl === false'));
  L = await logAfter(() => P.cmdLetters('e', {mac: false}));
  check('Windows Chrome: Win+E stays Win+E (5Be+ 12+ 12- 5Be-)', scans(L).join(' ') === '5Be+ 12+ 12- 5Be-', scans(L).join(' '));
  await P.close();
} catch (e) { check('ran', false, String(e && e.stack || e)); }

try { spike.stdin.end(); } catch {}
for (let i = 0; i < 50 && spike.exitCode === null; i++) await sleep(100);
if (spike.exitCode === null) try { execFileSync('taskkill', ['/PID', String(spike.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
const L = readLog();
check('no real SendInput: every key went to the stub', !/input: SendInput|SendInput failed/.test(L.replace(/STUB SendInput/g, '')) && /input stub/.test(L));
C.close();
await br.close();
const alt = await watch.stop();
check('no window in Alt+Tab / the taskbar from the test (chrome-headless-shell)', alt.max === 0, `max ${alt.max} over ${alt.samples} samples`);
console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
