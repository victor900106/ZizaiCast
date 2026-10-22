// 0.8.3.5 允許 once per 臨時帳號 login, not per connection (node 22+), against pm_remote_spike only:
//   node remote/tools/reapprove_test.mjs <pm_remote_spike.exe> <outDir>
// The spike runs --loopback (127.0.0.2 / .3 only: no firewall prompt), --no-tailscale, dry-run input,
// --scene static, --no-audio, the fixed link (127.0.0.2, what Funnel would deliver) with the owner's
// password and 臨時帳號 (made up), --approval: every 允許 the host asks for is answered by the spike
// (allow after 600 ms; --deny: the first one denied) and logged as "approval: request #n". The phone is
// ONE muted chrome-headless-shell (quiet_browser.mjs: no window, no Alt+Tab entry, never the focus);
// a watcher counts the Alt+Tab / taskbar windows of every process the test starts (must stay 0).
// No tailscale / cloudflared, nothing else is run; only these processes are ended.
// Run 1: a 臨時帳號 is asked 允許 once; then 切換畫質, 切換螢幕, 自動調整畫質 (down / up), 臨時降畫質,
//        a reconnect after a lost connection: no new request; a reload and the tab closed and opened
//        again (the page asks a guest's password again: its login is kept for that page only): no 允許.
// Run 2 (--deny): 拒絕 -> after the 20 s pause a reconnect asks again.
// Run 3 (--kick-after): 中斷 on the PC -> its login is revoked; logged in again, 允許 is asked again.
// Run 4: an account that expires (6 s): after that its login no longer works (the login form).
// rel-0835 (after the merge with fix-0835-input / fix-0835-files): run 1 also switches the screen the PC
// browser's ways (the PC bar's screen buttons #pbMons, Ctrl+Alt+Shift+M): no new 允許; and the PC bar's
// 檔案 button follows the 傳檔 table (a 臨時帳號 without 傳檔: greyed, says why, no sheet; run 5 with 傳檔: on).
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import {launch, cdpClient, watchAltTab} from './quiet_browser.mjs';

const [spikeExe, outDir] = process.argv.slice(2);
if (!spikeExe || !outDir) { console.error('usage: node reapprove_test.mjs <pm_remote_spike.exe> <outDir>'); process.exit(2); }
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const OWNER = 'Owner-Test-2026', GC = 'GHJ789', GS = 'KMN234';
let failed = 0;
const check = (name, ok, detail = '') => { if (!ok) failed++; console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail !== '' ? '  -- ' + detail : ''}`); };
const profileDir = path.join(os.tmpdir(), 'shell-0835-reapprove-' + crypto.randomBytes(3).toString('hex'));
const br = await launch(profileDir, ['--allow-loopback-in-peer-connection']);
const watch = watchAltTab([br.proc.pid, process.pid], path.join(outDir, 'alttab_reapprove.txt'));
const C = await cdpClient(br.ver.webSocketDebuggerUrl);
const cdp = C.cdp;

async function run(tag, extra, guests, body) {
  const token = crypto.randomBytes(16).toString('hex');
  const port = 19500 + Math.floor(Math.random() * 200);
  const logFile = path.join(outDir, `spike_${tag}.log`);
  fs.writeFileSync(logFile, '');
  const readLog = () => fs.readFileSync(logFile, 'utf8');
  const spike = spawn(spikeExe, ['--token', token, '--port', String(port), '--log', logFile, '--duration', '300', '--stdin-quit', '--loopback', '--no-tailscale',
    '--scene', 'static', '--no-audio', '--fixed-password', OWNER, '--approval', '--approve-after', '600', ...guests.flatMap(g => ['--guest', g]), ...extra],
    {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  watch.add(spike.pid);
  spike.stdout.on('data', () => {}); spike.stderr.on('data', () => {});
  for (let i = 0; i < 150 && !/fixed: origin/.test(readLog()); i++) await sleep(100);
  const waitFor = async (fn, ms) => { for (let t = 0; t < ms; t += 200) { const v = await fn(); if (v) return v; await sleep(200); } return null; };
  const requests = () => (readLog().match(/approval: request #\d+/g) || []).length;
  let tab = null;
  async function open(fresh) {
    if (fresh) await cdp('Storage.clearDataForOrigin', {origin: `http://127.0.0.2:${port}`, storageTypes: 'local_storage'});
    const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
    const {sessionId} = await cdp('Target.attachToTarget', {targetId, flatten: true});
    await cdp('Emulation.setDeviceMetricsOverride', {width: 390, height: 844, deviceScaleFactor: 3, mobile: true}, sessionId);
    const ev = async (expression, awaitPromise = false) => { const r = await cdp('Runtime.evaluate', {expression, returnByValue: true, awaitPromise}, sessionId); return r && r.result ? r.result.value : undefined; };
    tab = {targetId, sessionId, ev, close: () => cdp('Target.closeTarget', {targetId})};
    return tab;
  }
  // Logs in as a 臨時帳號 with the page's own form; true when the picture is there.
  async function login(user, pw) {
    const t = await open(true);
    await cdp('Page.navigate', {url: `http://127.0.0.2:${port}/#u=${user}`}, t.sessionId);
    await sleep(300);
    await t.ev(`(() => { try { localStorage.clear(); sessionStorage.clear(); } catch {} location.reload(); return 1; })()`);
    await sleep(800);
    await waitFor(() => t.ev(`$('pass').style.display === 'flex'`), 8000);
    await t.ev(`(() => { $('passIn').value = ${JSON.stringify(pw)}; submitPass(); return 1; })()`);
    return !!(await waitFor(() => t.ev('window.__pmFirstFrame ? 1 : 0'), 25000));
  }
  // The picture again after a new session: a new session id, connected, and whether it waited for 允許.
  async function again(t, action, ms = 20000) {
    const before = await t.ev('sessId');
    let waited = false;
    await t.ev(`(() => { ${action}; return 1; })()`);
    const ok = await waitFor(async () => {
      const st = await t.ev(`({id: sessId, st: S.state, pc: pc ? pc.connectionState : ''})`);
      if (st && st.st === 'pending') waited = true;
      return st && st.id && st.id !== before && st.pc === 'connected';
    }, ms);
    return {ok: !!ok, waited};
  }
  // The login form (again): the password, then the picture; whether it waited for 允許 on the way.
  async function relogin(t, pw, ms = 25000) {
    await waitFor(() => t.ev(`(typeof $ === 'function' && $('pass').style.display === 'flex') ? 1 : 0`), 10000);
    await t.ev(`(() => { $('passIn').value = ${JSON.stringify(pw)}; submitPass(); return 1; })()`);
    let waited = false;
    const ok = await waitFor(async () => {
      const st = await t.ev(`(typeof S === 'object' ? {st: S.state, pc: pc ? pc.connectionState : ''} : null)`);
      if (st && st.st === 'pending') waited = true;
      return st && st.pc === 'connected';
    }, ms);
    return {ok: !!ok, waited};
  }
  try { await body({login, open, again, relogin, readLog, requests, waitFor, port, tab: () => tab}); }
  catch (e) { check(`${tag}: ran`, false, String(e && e.stack || e)); }
  try { if (tab) await tab.close(); } catch {}
  try { spike.stdin.end(); } catch {}
  for (let i = 0; i < 50 && spike.exitCode === null; i++) await sleep(100);
  if (spike.exitCode === null) try { execFileSync('taskkill', ['/PID', String(spike.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
  await sleep(300);
}

await run('switches', [], [`g-ctl:${GC}:3600:control`], async ({login, again, relogin, open, requests, readLog, tab}) => {
  check('臨時帳號: logged in, the picture is there', await login('g-ctl', GC));
  check('臨時帳號: 允許 asked once for it', requests() === 1, String(requests()));
  const t = tab();
  const steps = [
    ['切換畫質 (setQuality)', `setQuality(curQ === 0 ? 1 : 0)`],
    ['切換螢幕 (the monitor buttons: prefs.mon + connect(\'switch\'))', `prefs.mon = 0; connect('switch')`],
    ['自動調整畫質 step down', `curQ = Math.max(0, curQ - 1); connect('switch')`],
    ['自動調整畫質 step up', `curQ = Math.min(prefs.q, curQ + 1); connect('switch')`],
    ['臨時降畫質', `curQ = 0; adapt.reset(); connect('switch')`],
    ['斷線重連 (connect(\'lost\'))', `stopPc(); connect('lost')`],
    // rel-0835: fix-0835-input's ways to switch the screen from a PC's browser (the page is told two screens;
    // the host clamps the index to the screens it has, the point is the new session's 允許)
    ['切換螢幕 (電腦版工具列的螢幕按鈕 #pbMons)', `S.monitors = [{primary: true, w: 3440, h: 1440}, {primary: false, w: 1920, h: 1080}]; buildMonitorSeg(); $('pbMons').children[prefs.mon === 0 ? 1 : 0].click()`],
    ['切換螢幕 (Ctrl+Alt+Shift+M, nextMonitor)', `S.monitors = [{primary: true, w: 3440, h: 1440}, {primary: false, w: 1920, h: 1080}]; window.dispatchEvent(new KeyboardEvent('keydown', {code: 'KeyM', key: 'M', ctrlKey: true, altKey: true, shiftKey: true, bubbles: true, cancelable: true}))`],
  ];
  for (const [name, js] of steps) {
    const r = await again(t, js);
    check(`臨時帳號 ${name}: connected again without 允許`, r.ok && !r.waited && requests() === 1, `connected ${r.ok}, waited ${r.waited}, requests ${requests()}`);
  }
  // rel-0835: the PC bar's 檔案 for a 臨時帳號 without 傳檔: greyed, its tooltip and a click say why, no sheet
  const pf = await t.ev(`(() => { syncPcbar(); return {off: $('pbFile').classList.contains('off'), dis: $('pbFile').getAttribute('aria-disabled'), title: $('pbFile').title, want: t('filesNoGuest'), phoneOff: $('bFile').classList.contains('off')}; })()`);
  check('電腦版工具列「檔案」(臨時帳號 without 傳檔): greyed, tooltip says why', pf && pf.off && pf.dis === 'true' && pf.title === pf.want && pf.phoneOff, JSON.stringify(pf));
  const pc = await t.ev(`(() => { $('toast').textContent = ''; $('pbFile').click(); return {sheet: $('fileS').style.display, toast: $('toast').textContent, want: t('filesNoGuest')}; })()`);
  check('電腦版工具列「檔案」click: says why, the files sheet stays closed', pc && pc.sheet !== 'block' && pc.toast.includes(pc.want), JSON.stringify(pc));
  // A reload, and the tab closed and opened again (the same browser: the same device id). The page keeps a
  // 臨時帳號's login for that page only (0.8.3.2): the login form comes back, the person types the account's
  // password again (a new login token) -- the same account on the same phone: no 允許 again.
  await t.ev(`(() => { location.reload(); return 1; })()`);
  await sleep(1500);
  let r = await relogin(t, GC);
  check('臨時帳號 重新整理頁面 (the password again): connected without 允許', r.ok && !r.waited && requests() === 1, `connected ${r.ok}, waited ${r.waited}, requests ${requests()}`);
  const url = (await t.ev('location.href')).replace(/#.*$/, '') + '#u=g-ctl';
  await t.close();
  await sleep(1500);
  const t2 = await open(false);
  await cdp('Page.navigate', {url}, t2.sessionId);
  await sleep(1200);
  r = await relogin(t2, GC);
  check('臨時帳號 關掉分頁再打開 (the password again): connected without 允許', r.ok && !r.waited && requests() === 1, `connected ${r.ok}, waited ${r.waited}, requests ${requests()}`);
  check('host log: "allowed on the PC already -> no 允許 again" for them', (readLog().match(/the same 臨時帳號 login as before, allowed on the PC already -> no 允許 again/g) || []).length >= 6);
  check('host log: the account was never spent / its session not "ended" by a switch', !/臨時帳號 g-ctl: its session ended -> spent/.test(readLog()));
});

await run('deny', ['--deny'], [`g-ctl:${GC}:3600:control`], async ({open, requests, waitFor, port}) => {
  const t = await open(true);
  await cdp('Page.navigate', {url: `http://127.0.0.2:${port}/#u=g-ctl`}, t.sessionId);
  await sleep(300);
  await t.ev(`(() => { try { localStorage.clear(); sessionStorage.clear(); } catch {} location.reload(); return 1; })()`);
  await sleep(800);
  await waitFor(() => t.ev(`$('pass').style.display === 'flex'`), 8000);
  await t.ev(`(() => { $('passIn').value = ${JSON.stringify(GC)}; submitPass(); return 1; })()`);
  check('拒絕: the page says denied', !!(await waitFor(() => t.ev(`S.state === 'denied'`), 15000)));
  await sleep(21000);  // the host's 20 s "just denied" pause
  await t.ev(`(() => { retryN = 0; connect(); return 1; })()`);
  check('拒絕 -> the reconnect asks 允許 again (request #2)', !!(await waitFor(() => requests() >= 2, 10000)), String(requests()));
});

await run('kick', ['--kick-after', '4'], [`g-ctl:${GC}:3600:control`], async ({login, relogin, requests, waitFor, tab, readLog}) => {
  check('中斷: logged in', await login('g-ctl', GC));
  check('中斷: the PC ended it', !!(await waitFor(() => /kick: ending the session from the PC/.test(readLog()), 15000)));
  await sleep(1500);
  await tab().ev(`(() => { retryN = 0; connect(); return 1; })()`);  // its login was revoked with it: the login form
  await sleep(1500);
  const r = await relogin(tab(), GC);
  check('中斷 -> logged in again: 允許 asked again (request #2)', r.waited && requests() >= 2, `connected ${r.ok}, waited ${r.waited}, requests ${requests()}`);
});

await run('expire', [], [`g-short:${GS}:6:control`], async ({login, requests, waitFor, tab}) => {
  check('到期: logged in', await login('g-short', GS));
  await sleep(8000);  // expired (its own session goes on until it ends)
  await tab().ev(`(() => { stopPc(); connect('lost'); return 1; })()`);
  const st = await waitFor(() => tab().ev(`S.state === 'passcode' ? 'passcode' : ''`), 10000);
  check('到期 -> its login no longer works: the login form, no straight-in', st === 'passcode' && requests() === 1, `${st} requests ${requests()}`);
});

await run('files', [], [`g-files:${GC}:3600:control+files`], async ({login, tab}) => {
  check('臨時帳號 with 傳檔: logged in', await login('g-files', GC));
  const pf = await tab().ev(`(() => { syncPcbar(); return {off: $('pbFile').classList.contains('off'), dis: $('pbFile').getAttribute('aria-disabled'), title: $('pbFile').title, want: t('lbFile'), phoneOff: $('bFile').classList.contains('off')}; })()`);
  check('電腦版工具列「檔案」(臨時帳號 with 傳檔): on, plain tooltip', pf && !pf.off && pf.dis === 'false' && pf.title === pf.want && !pf.phoneOff, JSON.stringify(pf));
});

const alt = await watch.stop();
check('no window in Alt+Tab / the taskbar during the test', alt && alt.samples > 0 && alt.max === 0, JSON.stringify(alt));
C.close();
await br.close();
try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
console.log(failed ? `FAILED (${failed} failed)` : 'ALL PASSED');
process.exit(failed ? 1 : 0);
