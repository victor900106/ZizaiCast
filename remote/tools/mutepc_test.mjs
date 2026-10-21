// 0.8.3.4 電腦喇叭靜音 from the phone (node 22+): who may change the PC's 手機播聲音時電腦靜音.
//   node remote/tools/mutepc_test.mjs <pm_remote_spike.exe> <outDir>
// Two spike runs, each --loopback (127.0.0.2 / .3 only: no firewall prompt), --no-tailscale, dry-run input,
// --scene static, a SILENT synthetic sound source (--synthetic-audio quiet: no playback device is captured,
// and the mute only logs "no device touched": the owner's speakers are never muted or unmuted), the fixed
// link with the owner's password and two 臨時帳號 (g-view 僅觀看, g-ctl 可操作). The phone is ONE muted
// chrome-headless-shell (quiet_browser.mjs: no window at all, no Alt+Tab entry, never the focus) with its
// own throw-away profile, reused for every phone and both runs; a watcher counts the Alt+Tab / taskbar
// windows of every process the test starts (must stay 0). Nothing else is run; only these are closed.
//   --shots <dir> --lang N: also a screenshot of the settings sheet per phone (spike --lang N).
// Run 1 (normal): the owner turns 電腦喇叭靜音 on / off (the PC's setting follows, the page's switch too);
// every 臨時帳號 is refused, g-view and g-ctl alike (switch disabled with the 臨時帳號 reason, the message
// refused on the PC: it is the owner's setting).
// Run 2 (--view-only on the PC): the owner's phone is refused too.
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import {launch, cdpClient, watchAltTab} from './quiet_browser.mjs';

const [spikeExe, outDir, ...rest] = process.argv.slice(2);
let shotsDir = null, lang = 0;
for (let i = 0; i < rest.length; i++) { if (rest[i] === '--shots') shotsDir = rest[++i]; else if (rest[i] === '--lang') lang = +rest[++i]; }
const LANGS = ['zh-TW', 'en', 'ja', 'ko'];
if (shotsDir) fs.mkdirSync(shotsDir, {recursive: true});
if (!spikeExe || !outDir) { console.error('usage: node mutepc_test.mjs <pm_remote_spike.exe> <outDir>'); process.exit(2); }
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const OWNER = 'Owner-Test-2026', GV = 'DEF567', GC = 'GHJ789';
const results = [];
let failed = 0;
const check = (name, ok, detail = '') => { results.push({name, ok: !!ok, detail}); if (!ok) failed++; console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail !== '' ? '  -- ' + detail : ''}`); };
const profileDir = path.join(os.tmpdir(), 'shell-0834-mutepc-' + crypto.randomBytes(3).toString('hex'));
const br = await launch(profileDir, ['--allow-loopback-in-peer-connection']);
const watch = watchAltTab([br.proc.pid, process.pid], path.join(outDir, 'alttab_mutepc.txt'));
const C = await cdpClient(br.ver.webSocketDebuggerUrl);
const cdp = C.cdp;

async function run(tag, extra, body) {
  const token = crypto.randomBytes(16).toString('hex');
  const port = 19700 + Math.floor(Math.random() * 200);
  const logFile = path.join(outDir, `spike_${tag}.log`);
  fs.writeFileSync(logFile, '');
  const readLog = () => fs.readFileSync(logFile, 'utf8');
  const spike = spawn(spikeExe, ['--token', token, '--port', String(port), '--log', logFile, '--duration', '240', '--stdin-quit', '--loopback', '--no-tailscale',
    '--scene', 'static', '--synthetic-audio', 'quiet', '--fixed-password', OWNER,
    '--guest', `g-view:${GV}:3600:view`, '--guest', `g-ctl:${GC}:3600:control`, '--lang', String(lang), ...extra], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  watch.add(spike.pid);
  spike.stdout.on('data', () => {}); spike.stderr.on('data', () => {});
  for (let i = 0; i < 150 && !/fixed: origin/.test(readLog()); i++) await sleep(100);
  const waitFor = async (fn, ms) => { for (let t = 0; t < ms; t += 200) { const v = await fn(); if (v) return v; await sleep(200); } return null; };
  // A phone: the fixed link (the owner: the password; a 臨時帳號: the address with #u=), then the picture.
  async function phone(user, pw) {
    await cdp('Storage.clearDataForOrigin', {origin: `http://127.0.0.2:${port}`, storageTypes: 'local_storage'});
    const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
    const {sessionId} = await cdp('Target.attachToTarget', {targetId, flatten: true});
    await cdp('Emulation.setDeviceMetricsOverride', {width: 390, height: 844, deviceScaleFactor: 3, mobile: true}, sessionId);
    await cdp('Page.navigate', {url: `http://127.0.0.2:${port}/${user ? '#u=' + user : ''}`}, sessionId);
    const ev = async (expression, awaitPromise = false) => { const r = await cdp('Runtime.evaluate', {expression, returnByValue: true, awaitPromise}, sessionId); return r && r.result ? r.result.value : undefined; };
    // a fresh phone: no saved owner login from the previous tab (the page keeps it 30 days)
    await sleep(300);
    await ev(`(() => { try { localStorage.clear(); sessionStorage.clear(); } catch {} location.reload(); return 1; })()`);
    await sleep(800);
    await waitFor(() => ev(`$('pass').style.display === 'flex'`), 8000);
    await ev(`(() => { $('passIn').value = ${JSON.stringify(pw)}; submitPass(); return 1; })()`);
    const frame = !!(await waitFor(() => ev('window.__pmFirstFrame ? 1 : 0'), 25000));
    // the session's DataChannel is open and its offer / first mode message are in
    await waitFor(() => ev(`(typeof dc !== 'undefined' && dc && dc.readyState === 'open' && typeof S.mutePcOk === 'boolean') ? 1 : 0`), 10000);
    await sleep(1500);
    const flip = async on => { await ev(`(() => { const s = $('swMutePc'); s.checked = ${on}; s.onchange(); return 1; })()`); await sleep(1500); };
    const state = () => ev(`({on: !!S.mutePc, ok: S.mutePcOk, why: S.mutePcWhy, sub: $('lMutePcSub').textContent, guestSub: $('lMutePcSub').textContent === t('mutePcNoGuest'), disabled: $('swMutePc').disabled, checked: $('swMutePc').checked, viewOnly: !!S.viewOnly})`);
    // the settings sheet, scrolled to the sound rows (only with --shots)
    const shot = async name => {
      if (!shotsDir) return;
      await ev(`(() => { closeSheets(); $('bSet').onclick(); const r = $('rowVol'); r && r.scrollIntoView({block: 'start'}); return 1; })()`);
      await sleep(600);
      const r = await cdp('Page.captureScreenshot', {format: 'png'}, sessionId);
      if (r && r.data) fs.writeFileSync(path.join(shotsDir, `${LANGS[lang]}_${name}.png`), Buffer.from(r.data, 'base64'));
      await ev(`(() => { closeSheets(); return 1; })()`);
    };
    return {ev, frame, flip, state, shot, close: () => cdp('Target.closeTarget', {targetId})};
  }
  const count = re => (readLog().match(re) || []).length;
  try { await body({phone, readLog, count, waitFor}); }
  catch (e) { check(`${tag}: ran`, false, String(e && e.stack || e)); }
  try { spike.stdin.end(); } catch {}
  for (let i = 0; i < 50 && spike.exitCode === null; i++) await sleep(100);
  if (spike.exitCode === null) try { execFileSync('taskkill', ['/PID', String(spike.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
  await sleep(500);
  const L = readLog();
  check(`${tag}: no playback device was muted or unmuted (synthetic source)`, !/muted while the phone plays the sound|unmuted \(/.test(L));
}

await run('normal', [], async ({phone, readLog, count, waitFor}) => {
  const o = await phone('', OWNER);
  check('owner: connected', o.frame);
  let s = await o.state();
  check('owner: the switch shows the PC setting (off) and may change it', s && !s.on && s.ok && !s.disabled, JSON.stringify(s));
  await o.ev(`(() => { send({t: 'audio', on: true}); return 1; })()`);  // the phone's own 開聲音 (no tap needed in a test)
  // 0.8.3.4 page-report: how the phone plays the sound (audioSession, output route, path)
  const rep = await waitFor(() => (readLog().match(/page-report [^\n]*"aSession"[^\n]*/) || [null])[0], 15000);
  check('page-report carries aSession / aSessionSet / outRoute / gainPath / vol / boost', rep && ['"aSession"', '"aSessionSet"', '"outRoute"', '"gainPath"', '"vol"', '"boost"', '"actx"'].every(k => rep.includes(k)),
    rep ? rep.slice(rep.indexOf('"aSession"'), rep.indexOf('"aSession"') + 120) : 'no report');
  // iPhone (stubbed: Chromium has no navigator.audioSession): the tap sets 'playback'
  const as = await o.ev(`(() => { const a = {type: 'auto'}; Object.defineProperty(navigator, 'audioSession', {value: a, configurable: true}); return setAudioSession() + '/' + a.type; })()`);
  check('navigator.audioSession.type is set to playback when there is one', as === 'playback/playback', as);
  const none = await o.ev(`(() => { delete navigator.audioSession; return setAudioSession(); })()`);
  check('no navigator.audioSession: nothing set, reported as n/a', none === 'n/a', none);
  await o.shot('P2-settings-owner');
  await o.flip(true);
  await o.shot('P3-settings-owner-mutepc-on');
  s = await o.state();
  check('owner: on -> the PC setting is on', /電腦喇叭靜音 on \(from the phone/.test(readLog()) && /host: 手機播聲音時電腦靜音 on/.test(readLog()), '');
  check('owner: the page follows (switch on)', s && s.on && s.checked, JSON.stringify(s));
  check('owner: with the sound on, the mute is wanted, on the synthetic source only', /audio: TEST: PC speakers muted wanted \(synthetic source: no device touched\)/.test(readLog()));
  await o.flip(false);
  s = await o.state();
  check('owner: off -> the PC setting is off again', /host: 手機播聲音時電腦靜音 off/.test(readLog()) && s && !s.on, JSON.stringify(s));
  await o.close();
  await new Promise(r => setTimeout(r, 2000));
  const v = await phone('g-view', GV);
  check('g-view (僅觀看 臨時帳號): connected', v.frame);
  s = await v.state();
  check('g-view: the switch is disabled, with the 臨時帳號 reason', s && s.disabled && s.ok === false && s.why === 'guest' && s.guestSub, JSON.stringify(s));
  let before = count(/REFUSED \(a 臨時帳號\)/g);
  await v.ev(`(() => { send({t: 'mutepc', on: true}); return 1; })()`);  // even if a page sends it anyway
  await new Promise(r => setTimeout(r, 1500));
  s = await v.state();
  check('g-view: refused on the PC, the setting stays off', count(/REFUSED \(a 臨時帳號\)/g) === before + 1 && count(/host: 手機播聲音時電腦靜音 on/g) === 1 && s && !s.on, JSON.stringify(s));
  await v.close();
  await new Promise(r => setTimeout(r, 2000));
  const c = await phone('g-ctl', GC);
  check('g-ctl (可操作 臨時帳號): connected', c.frame);
  s = await c.state();
  check('g-ctl: the switch is disabled too (the owner\'s setting), with the 臨時帳號 reason', s && s.disabled && s.ok === false && s.why === 'guest' && s.guestSub && !s.viewOnly, JSON.stringify(s));
  await c.shot('P4-settings-guest-control');
  before = count(/REFUSED \(a 臨時帳號\)/g);
  await c.ev(`(() => { send({t: 'mutepc', on: true}); return 1; })()`);
  await new Promise(r => setTimeout(r, 1500));
  s = await c.state();
  check('g-ctl: refused on the PC, the setting stays off', count(/REFUSED \(a 臨時帳號\)/g) === before + 1 && count(/host: 手機播聲音時電腦靜音 on/g) === 1 && s && !s.on, JSON.stringify(s));
  await c.close();
});

await run('viewonly', ['--view-only'], async ({phone, readLog}) => {
  const o = await phone('', OWNER);
  check('PC in 僅觀看: owner connected', o.frame);
  let s = await o.state();
  check('PC in 僅觀看: the owner\'s switch is disabled', s && s.disabled && s.viewOnly, JSON.stringify(s));
  await o.shot('P5-settings-pc-viewonly');
  await o.ev(`(() => { send({t: 'mutepc', on: true}); return 1; })()`);
  await new Promise(r => setTimeout(r, 1500));
  s = await o.state();
  check('PC in 僅觀看: refused on the PC, the setting stays off', /電腦喇叭靜音 on REFUSED \(僅觀看\)/.test(readLog()) && !/host: 手機播聲音時電腦靜音 on/.test(readLog()) && s && !s.on, JSON.stringify(s));
  await o.close();
});

C.close();
await br.close();
await sleep(500);
try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
const alt = await watch.stop();
check(`Alt+Tab / taskbar windows of the test's processes = 0 (${alt.samples} samples)`, alt.max === 0 && alt.samples > 0, `max ${alt.max}${alt.seen.length ? ': ' + alt.seen.join(' ; ') : ''}`);
fs.writeFileSync(path.join(outDir, 'mutepc.json'), JSON.stringify(results, null, 1));
console.log(failed ? `FAILED (${failed})` : `ALL PASSED (${results.length})`);
process.exit(failed ? 1 : 0);
