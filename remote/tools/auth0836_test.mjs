// 0.8.3.6 連線時要不要允許 end to end (node 22+), against pm_remote_spike only:
//   node remote/tools/auth0836_test.mjs <pm_remote_spike.exe> <outDir>
// The spike runs --loopback (127.0.0.2 / .3 only: no firewall prompt), --no-tailscale, dry-run input, --scene
// static, --no-audio, the fixed link (127.0.0.2, what Funnel would deliver) with a made-up owner password and a
// made-up 臨時帳號, --approval: every 允許 the host asks for is answered by the spike after 600 ms and logged as
// "approval: request #n"; each connection's "event: ACTIVE ... policy <p> admit <a>" says what let it in. The
// phone is ONE muted chrome-headless-shell (quiet_browser.mjs: no window, no Alt+Tab entry, never the focus);
// a watcher counts the Alt+Tab / taskbar windows of every process the test starts (must stay 0).
// No tailscale / cloudflared, nothing else is run; only these processes are ended.
// Run 1 (defaults): 主帳號 = 只用密碼 (straight in), 臨時帳號 = 每次問 (允許 asked) -- as 0.8.3.5.
// Run 2 (--policy-guest password --policy-fixed ask): 臨時帳號 straight in with its password; 主帳號 asks once,
//        a 切換畫質 after that goes straight in (this phone, this run).
// Run 3 (--policy-guest remember --approve-remember): 允許 + 記住 ticked; the host asked the 記住 list with the
//        account's own scope; a 切換畫質 goes straight in (the same login).
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import {launch, cdpClient, watchAltTab} from './quiet_browser.mjs';

const [spikeExe, outDir] = process.argv.slice(2);
if (!spikeExe || !outDir) { console.error('usage: node auth0836_test.mjs <pm_remote_spike.exe> <outDir>'); process.exit(2); }
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const OWNER = 'Owner-Test-2026', GC = 'GHJ789';
let failed = 0;
const check = (name, ok, detail = '') => { if (!ok) failed++; console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail !== '' ? '  -- ' + detail : ''}`); };
const profileDir = path.join(os.tmpdir(), 'shell-0836-auth-' + crypto.randomBytes(3).toString('hex'));
const br = await launch(profileDir, ['--allow-loopback-in-peer-connection']);
const watch = watchAltTab([br.proc.pid, process.pid], path.join(outDir, 'alttab_auth0836.txt'));
const C = await cdpClient(br.ver.webSocketDebuggerUrl);
const cdp = C.cdp;

async function run(tag, extra, body) {
  const token = crypto.randomBytes(16).toString('hex');
  const port = 19700 + Math.floor(Math.random() * 200);
  const logFile = path.join(outDir, `spike_${tag}.log`);
  fs.writeFileSync(logFile, '');
  const readLog = () => fs.readFileSync(logFile, 'utf8');
  const spike = spawn(spikeExe, ['--token', token, '--port', String(port), '--log', logFile, '--duration', '240', '--stdin-quit', '--loopback', '--no-tailscale',
    '--scene', 'static', '--no-audio', '--fixed-password', OWNER, '--approval', '--approve-after', '600', '--guest', `g-ctl:${GC}:3600:control`, ...extra],
    {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  watch.add(spike.pid);
  spike.stdout.on('data', () => {}); spike.stderr.on('data', () => {});
  for (let i = 0; i < 150 && !/fixed: origin/.test(readLog()); i++) await sleep(100);
  const waitFor = async (fn, ms) => { for (let t = 0; t < ms; t += 200) { const v = await fn(); if (v) return v; await sleep(200); } return null; };
  const requests = () => (readLog().match(/approval: request #\d+/g) || []).length;
  const actives = () => [...readLog().matchAll(/event: ACTIVE .* policy (\S*) admit (\S*)/g)].map(m => `${m[1]}:${m[2]}`);
  const tabs = [];
  // A fresh tab with an empty storage (a new device id), logged in as the owner (user '') or a 臨時帳號.
  async function login(user, pw) {
    await cdp('Storage.clearDataForOrigin', {origin: `http://127.0.0.2:${port}`, storageTypes: 'local_storage,session_storage'});
    const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
    const {sessionId} = await cdp('Target.attachToTarget', {targetId, flatten: true});
    await cdp('Emulation.setDeviceMetricsOverride', {width: 390, height: 844, deviceScaleFactor: 3, mobile: true}, sessionId);
    const ev = async (expression) => { const r = await cdp('Runtime.evaluate', {expression, returnByValue: true}, sessionId); return r && r.result ? r.result.value : undefined; };
    const t = {targetId, sessionId, ev, close: () => cdp('Target.closeTarget', {targetId})};
    tabs.push(t);
    await cdp('Page.navigate', {url: `http://127.0.0.2:${port}/${user ? '#u=' + user : ''}`}, sessionId);
    await sleep(300);
    await t.ev(`(() => { try { localStorage.clear(); sessionStorage.clear(); } catch {} location.reload(); return 1; })()`);  // a new device, no login kept
    await sleep(1000);
    await waitFor(() => t.ev(`(typeof $ === 'function' && $('pass').style.display === 'flex') ? 1 : 0`), 10000);
    await t.ev(`(() => { $('passIn').value = ${JSON.stringify(pw)}; submitPass(); return 1; })()`);
    let waited = false;
    const ok = await waitFor(async () => {
      const st = await t.ev(`(typeof S === 'object' ? {st: S.state, pc: pc ? pc.connectionState : ''} : null)`);
      if (st && st.st === 'pending') waited = true;
      return st && st.pc === 'connected' && S_ok(st);
    }, 25000);
    return {t, ok: !!ok, waited};
  }
  const S_ok = st => st.st === 'connected' || st.st === 'playing' || st.pc === 'connected';
  // 切換畫質: a new session from the same tab; whether it waited for 允許.
  // rel-0836 (feat-0836-video merged): 切換畫質 is done inside the same connection now ({"t":"quality"}), so
  // no new session is asked for; the new-session path stays as its fallback (a change beyond the offer's
  // H.264 level, an older PC: connect('switch')) and is what is checked here, right after the quality change.
  async function switchQuality(t) {
    const before = await t.ev('sessId');
    const askBefore = await t.ev(`typeof streamAskId === 'number' ? streamAskId : 0`);
    let waited = false, how = '';
    await t.ev(`(() => { setQuality(curQ === 0 ? 1 : 0); return 1; })()`);
    const same = await waitFor(async () => {
      const st = await t.ev(`({id: sessId, pc: pc ? pc.connectionState : '', ask: typeof streamAskId === 'number' ? streamAskId : 0, ans: S.stream ? S.stream.id : 0, ansOk: !!(S.stream && S.stream.ok)})`);
      return st && st.id === before && st.ask > askBefore && st.ans === st.ask && st.ansOk && st.pc === 'connected';
    }, 6000);
    if (same) { how = 'same connection, then connect(switch)'; await t.ev(`(() => { connect('switch'); return 1; })()`); }
    const ok = await waitFor(async () => {
      const st = await t.ev(`({id: sessId, st: S.state, pc: pc ? pc.connectionState : ''})`);
      if (st && st.st === 'pending') waited = true;
      return st && st.id && st.id !== before && st.pc === 'connected';
    }, 20000);
    if (!how) how = 'new session';
    return {ok: !!ok, waited, how};
  }
  try { await body({login, switchQuality, requests, actives, readLog}); }
  catch (e) { check(`${tag}: ran`, false, String(e && e.stack || e)); }
  for (const t of tabs) try { await t.close(); } catch {}
  try { spike.stdin.end(); } catch {}
  for (let i = 0; i < 50 && spike.exitCode === null; i++) await sleep(100);
  if (spike.exitCode === null) try { execFileSync('taskkill', ['/PID', String(spike.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
  await sleep(300);
}

await run('defaults', [], async ({login, requests, actives, readLog}) => {
  const o = await login('', OWNER);
  check('預設 主帳號 (只用密碼): straight in, no 允許', o.ok && !o.waited && requests() === 0, `ok ${o.ok}, waited ${o.waited}, requests ${requests()}`);
  check('預設 主帳號: 連線紀錄 says password:password', actives()[0] === 'password:password', actives().join(' '));
  await o.t.close();
  await sleep(800);
  const g = await login('g-ctl', GC);
  check('預設 臨時帳號 (每次問): 允許 asked', g.ok && g.waited && requests() === 1, `ok ${g.ok}, waited ${g.waited}, requests ${requests()}`);
  check('預設 臨時帳號: 連線紀錄 says ask:allowed', actives().includes('ask:allowed'), actives().join(' '));
  check('預設 臨時帳號 (每次問): the 記住 list was not even asked', !/remembered\? device \S+ scope guest/.test(readLog()));
});

await run('guestpw', ['--policy-guest', 'password', '--policy-fixed', 'ask'], async ({login, switchQuality, requests, actives}) => {
  const g = await login('g-ctl', GC);
  check('臨時帳號 只用密碼: straight in with its password, no 允許', g.ok && !g.waited && requests() === 0, `ok ${g.ok}, waited ${g.waited}, requests ${requests()}`);
  check('臨時帳號 只用密碼: 連線紀錄 says password:password', actives()[0] === 'password:password', actives().join(' '));
  await g.t.close();
  await sleep(800);
  const o = await login('', OWNER);
  check('主帳號 每次問: 允許 asked (0.8.3.5: straight in)', o.ok && o.waited && requests() === 1, `ok ${o.ok}, waited ${o.waited}, requests ${requests()}`);
  const s = await switchQuality(o.t);
  check('主帳號 每次問: 切換畫質 after that: no 允許 again (this-run)', s.ok && !s.waited && requests() === 1 && actives().includes('ask:this-run'), `${JSON.stringify(s)} ${actives().join(' ')}`);
});

await run('guestremember', ['--policy-guest', 'remember', '--approve-remember'], async ({login, switchQuality, requests, actives, readLog}) => {
  const g = await login('g-ctl', GC);
  check('臨時帳號 記住這台裝置: 允許 asked the first time', g.ok && g.waited && requests() === 1, `ok ${g.ok}, waited ${g.waited}, requests ${requests()}`);
  check('臨時帳號 記住這台裝置: the host asked the 記住 list with the account\'s own scope', /remembered\? device \S+ scope guest -> no/.test(readLog()));
  check('臨時帳號 記住這台裝置: 連線紀錄 says remember:allowed-remember', actives().includes('remember:allowed-remember'), actives().join(' '));
  const s = await switchQuality(g.t);
  check('臨時帳號 記住這台裝置: 切換畫質: no 允許 again (same-login)', s.ok && !s.waited && requests() === 1 && actives().includes('remember:same-login'), `${JSON.stringify(s)} ${actives().join(' ')}`);
});

const alt = await watch.stop();
check('no window in Alt+Tab / the taskbar during the test', alt && alt.samples > 0 && alt.max === 0, JSON.stringify(alt));
C.close();
await br.close();
try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
console.log(failed ? `FAILED (${failed} failed)` : 'ALL PASSED');
process.exit(failed ? 1 : 0);
