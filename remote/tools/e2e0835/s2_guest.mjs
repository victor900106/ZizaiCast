// 0.8.3.5 e2e ②: 臨時帳號 — 允許 once per login, not per connection (the released app, --dev instance).
//   node remote/tools/e2e0835/s2_guest.mjs <自在投影.exe> <outDir>
// The phone = ONE chrome-headless-shell tab (390x844, mobile). Accounts are made in the app's own
// 新增臨時帳號 window (DevCommand 440 / 981-984), the 允許 dialog is answered with DevCommand 961, 中斷 with
// 965 2, 刪除 with 450. What counts is the app's log: one "[info] remote: ... asks to connect" line per
// 允許 dialog. Steps after the first 允許 (each a new WebRTC session on the page):
//   切畫質 (settings ▸ 畫質 buttons), 切螢幕 (settings ▸ 螢幕 buttons; this PC has two), 自動 (0.8.4: the PC's
//   own ladder replaced the page's adapt loop: 自動 picked in the connection, then a rung change made by the PC in
//   the same session), 斷線重連 (connect('lost')), 重新整理 (password again),
//   tab closed and opened again (password again)  -> no new 允許.
// 0.8.3.6 (rel-0836): 切畫質 / 切螢幕 / 自動調整畫質 happen INSIDE the connection ({"t":"quality"}, the host logs
//   "picture -> ... in this session (no new connection)"): the same session id stays, no reconnect. Only a size beyond
//   the offer's H.264 level makes the PC answer "beyond this connection's H.264 level -> the page reconnects" (a new
//   session, still no 允許). 斷線重連 / 重新整理 / tab reopened are still new sessions.
// Then: 拒絕 -> asked again; 中斷 (kick) -> asked again; 刪除 -> cannot log in; 到期 (the app restarted with
// that account's expiry moved to "in 50 s"; a made-up edit of its own data folder) -> cannot log in.
import fs from 'node:fs';
import path from 'node:path';
import {startApp, startBrowser, results, sleep, connectedJs} from './lib.mjs';

const [exe, outDir] = process.argv.slice(2);
if (!exe || !outDir) { console.error('usage: node s2_guest.mjs <自在投影.exe> <outDir>'); process.exit(2); }
const R = results(outDir);
const {check} = R;
const ev2 = {};
let A = await startApp(exe, outDir, {tag: 'guest'});
check('app up, remote on, fixed address (made up) ready', !!A.lanUrl && A.fixedReady, A.fixedOrigin);
const B = await startBrowser(outDir, 'guest');
B.watch.add(A.pid);
let T = await B.tab({w: 390, h: 844, dpr: 3, mobile: true});
const noAgainLines = () => A.count(/allowed on the PC already -> no 允許 again/);
const state = () => T.ev(`(typeof S === 'object' ? {id: typeof sessId !== 'undefined' ? sessId : '', st: S.state, pc: (typeof pc !== 'undefined' && pc) ? pc.connectionState : '', bye: S.bye || '', q: typeof curQ !== 'undefined' ? curQ : -1, mon: prefs.mon} : null)`);

async function login(name, pw, {query = '', fresh = false} = {}) {
  if (fresh) await B.cdp('Storage.clearDataForOrigin', {origin: A.fixedOrigin, storageTypes: 'local_storage,session_storage'});
  await T.nav('about:blank'); await sleep(300);  // a full page load (a new #u= alone stays in the same document)
  await T.nav(`${A.fixedOrigin}/${query}#u=${name}`);
  await T.waitFor(() => T.ev(`(typeof $ === 'function' && $('pass') && $('pass').style.display === 'flex') ? 1 : 0`), 12000);
  await T.ev(`(() => { if ($('userIn')) $('userIn').value = ${JSON.stringify(name)}; $('passIn').value = ${JSON.stringify(pw)}; submitPass(); return 1; })()`);
}
async function relogin(pw) {
  const form = await T.waitFor(() => T.ev(`(typeof $ === 'function' && $('pass') && $('pass').style.display === 'flex') ? 1 : 0`), 12000);
  await T.ev(`(() => { $('passIn').value = ${JSON.stringify(pw)}; submitPass(); return 1; })()`);
  return !!form;
}
// Runs an action; true when a NEW session is connected; whether the page waited for 允許 on the way.
async function again(action, ms = 25000) {
  const s0 = await state();
  const asks0 = A.asks();
  let waited = false;
  await action();
  const ok = await T.waitFor(async () => {
    const st = await state();
    if (st && st.st === 'pending') waited = true;
    return st && st.id && st.id !== s0.id && st.pc === 'connected' && (await T.ev(connectedJs)) === 1;
  }, ms);
  await sleep(600);
  return {ok: !!ok, waited, newAsks: A.asks() - asks0, st: await state()};
}
// 0.8.3.6: an action that changes the picture. In the connection: the host logs "in this session" and the session id
// stays; beyond the offer's H.264 level: the host logs "beyond ... reconnects" and a new session connects.
async function stepIn(action, ms = 25000) {
  const s0 = await state();
  const asks0 = A.asks();
  const inRe = /picture -> quality[^\n]*in this session \(no new connection\)/, beyondRe = /picture -> quality[^\n]*beyond this connection's H\.264 level/;
  const in0 = A.count(inRe), by0 = A.count(beyondRe);
  let waited = false;
  await action();
  const ok = await T.waitFor(async () => {
    const st = await state();
    if (st && st.st === 'pending') waited = true;
    const inS = A.count(inRe) > in0, by = A.count(beyondRe) > by0;
    if (!(inS || by)) return false;
    if (by && st.id === s0.id) return false;  // told to reconnect: wait for the new session
    return st && st.id && st.pc === 'connected' && (await T.ev(connectedJs)) === 1;
  }, ms);
  await sleep(600);
  const st = await state();
  const inSession = A.count(inRe) > in0, beyond = A.count(beyondRe) > by0;
  const same = !!st && st.id === s0.id;
  // right: in the connection (same session id) or beyond the level (new session id); never a 允許
  return {ok: !!ok && ((inSession && same) || (beyond && !same)), waited, newAsks: A.asks() - asks0, same, inSession, beyond, st};
}
const stepNote = r => `ok ${r.ok} ${r.same ? 'same session (in the connection)' : 'new session'}${r.beyond ? ' (beyond the H.264 level)' : ''} waited ${r.waited} newAsks ${r.newAsks}`;
const openSettings = () => T.ev(`(() => { if ($('set').style.display !== 'block') $('bSet').click(); return 1; })()`);
const closeSheets_ = () => T.ev(`(() => { closeSheets(); return 1; })()`);

try {
  // ---- G1: 1 小時, 可操作 (+ 傳檔): the default panel state is kept as a picture ----
  const G1 = await A.makeGuest(1 | 8, 's2_guest_panel_default.png');
  check('G1 made in the 新增臨時帳號 window (name + one-time password shown)', !!(G1.name && G1.pw), G1.name);
  const G2 = await A.makeGuest(1 | 8);
  check('G2 made (for 拒絕)', !!(G2.name && G2.pw), G2.name);
  A.dev(986, 0, 400);
  ev2.accounts = (A.readLog().match(/dev guests: [^\n]*/g) || []).slice(-1)[0];

  // first login: 允許 asked once
  const asks0 = A.asks();
  await login(G1.name, G1.pw, {query: '?simloss=0', fresh: true});
  check('G1 first login: 允許 asked on the PC', await A.answer(1, 15000), `asks ${A.asks() - asks0}`);
  check('G1 connected, picture there', !!(await T.waitFor(() => T.ev(connectedJs), 30000)));
  await sleep(1500);
  await T.ev(`(() => { if ($('modeHint')) $('modeHint').style.display = 'none'; return 1; })()`);
  const base = A.asks();
  // 切畫質 (the settings sheet's 畫質 buttons)
  ev2.qStart = (await state()).q;  // the fixed link starts a phone at 流暢 (0)
  for (const q of [1, 0, 2].filter((q, i) => i > 0 || q !== ev2.qStart)) {
    const r = await stepIn(async () => { await openSettings(); await sleep(200); await T.ev(`(() => { $('segQ').querySelector('[data-v="${q}"]').click(); return 1; })()`); });
    (ev2.qSteps = ev2.qSteps || []).push(r.same);
    check(`切畫質 -> ${['流暢', '標準', '清晰'][q] || q} (settings ▸ 畫質): in the connection (or a new session only beyond the H.264 level), no 允許`, r.ok && !r.waited && r.newAsks === 0 && r.st.q === q, `${stepNote(r)} q ${r.st && r.st.q}`);
    await closeSheets_();
  }
  check('切畫質 (0.8.3.6): at least one change stayed in the same session (no reconnect)', (ev2.qSteps || []).some(x => x), JSON.stringify(ev2.qSteps));
  // 切螢幕 (settings ▸ 螢幕): this PC has two
  const mons = await T.ev(`S.monitors ? S.monitors.length : 0`);
  ev2.monitors = mons;
  if (mons > 1) {
    for (const m of [1, 0]) {
      const r = await stepIn(async () => { await openSettings(); await sleep(200); await T.ev(`(() => { $('segMon').children[${m}].click(); return 1; })()`); });
      check(`切螢幕 -> 螢幕 ${m + 1} (settings ▸ 螢幕): in the connection (or a new session only beyond the H.264 level), no 允許`, r.ok && !r.waited && r.newAsks === 0 && r.st.mon === m, `${stepNote(r)} mon ${r.st && r.st.mon}`);
      await closeSheets_();
    }
  } else check('切螢幕: this PC has more than one screen', false, `monitors ${mons}`);
  // 0.8.4 自動畫質 (feat-084-quality): the page's own adapt loop (prefs.auto, ?simloss) is gone. 自動 (畫質's first button,
  // data-v -1, the default) lets the PC move along its ladder from the phone's RTCP reports (the ladder itself:
  // pm_autoq_test, q084_bench). Here: picking 自動 stays inside the connection and needs no 允許, and the PC's own
  // step (it starts low and climbs while the link is clean) happens in the same session, again without 允許.
  {
    const s0 = await state(), asks0 = A.asks();
    const autoRe = /畫質 自動 \(the PC follows the network\)/, stepRe = /auto quality: rung \d+ -> \d+/;
    const a0 = A.count(autoRe), n0 = A.count(stepRe);
    await openSettings(); await sleep(200);
    await T.ev(`(() => { $('segQ').querySelector('[data-v="-1"]').click(); return 1; })()`);
    const on = await T.waitFor(() => A.count(autoRe) > a0, 8000);
    await closeSheets_();
    const stepped = await T.waitFor(() => A.count(stepRe) > n0, 40000);
    await sleep(800);
    const st = await state(), conn = (await T.ev(connectedJs)) === 1, pq = await T.ev(`prefs.q`), rung = await T.ev(`S.autoRung`);
    const line = (A.readLog().match(/auto quality: rung \d+ -> \d+[^\n]{0,60}/g) || []).slice(-1)[0] || '';
    check('自動 (settings ▸ 畫質, 0.8.4): picked inside the connection, the PC says 畫質 自動, no 允許', !!on && pq === -1 && st.id === s0.id && A.asks() === asks0, `on ${!!on} prefs.q ${pq} same ${st.id === s0.id} newAsks ${A.asks() - asks0}`);
    // 自動 picked after a hand-chosen 清晰 goes on at the top rung (4): on this clean loopback link there is nothing to
    // climb, so "already at the top" counts too (the step at the session's start is in the log: rung 1 -> 2 ...)
    check('自動: the PC changes rung by itself (or already sits at the top) in the same session (no reconnect), no 允許', (!!stepped || rung === 4) && st.id === s0.id && conn && A.asks() === asks0, `stepped ${!!stepped} rung ${rung} (last: ${line}) same ${st.id === s0.id} connected ${conn} newAsks ${A.asks() - asks0}`);
  }
  // 斷線重連
  {
    const r = await again(() => T.ev(`(() => { stopPc(); connect('lost'); return 1; })()`));
    check('斷線重連 (connect(\'lost\')): new session, no 允許', r.ok && !r.waited && r.newAsks === 0, `ok ${r.ok} waited ${r.waited} newAsks ${r.newAsks}`);
  }
  // 重新整理: the page asks a 臨時帳號's password again (kept for that page only), but no 允許
  {
    const a0 = A.asks();
    await T.ev(`(() => { location.reload(); return 1; })()`);
    await sleep(1500);
    const form = await relogin(G1.pw);
    const ok = await T.waitFor(() => T.ev(connectedJs), 25000);
    check('重新整理 (password again on the page): connected, no 允許', form && !!ok && A.asks() === a0, `form ${form} ok ${!!ok} newAsks ${A.asks() - a0}`);
  }
  // tab closed and opened again (same browser = same device id)
  {
    const a0 = A.asks();
    const url = (await T.ev('location.href')).replace(/#.*$/, '').replace(/\?.*$/, '') + `#u=${G1.name}`;
    await T.close(); await sleep(1500);
    T = await B.tab({w: 390, h: 844, dpr: 3, mobile: true});
    await T.nav(url);
    const form = await relogin(G1.pw);
    const ok = await T.waitFor(() => T.ev(connectedJs), 25000);
    check('分頁關掉再打開 (password again): connected, no 允許', form && !!ok && A.asks() === a0, `form ${form} ok ${!!ok} newAsks ${A.asks() - a0}`);
  }
  check(`after the first 允許: ${A.asks() - base} more 允許 dialogs for G1 (must be 0)`, A.asks() === base);
  ev2.noAgainLines = noAgainLines();
  // 0.8.3.6: only real new sessions log it -- 斷線重連, 重新整理, tab reopened (3) + every change that went beyond the H.264 level
  const beyondN = A.count(/picture -> quality[^\n]*beyond this connection's H\.264 level/), wantN = 3 + beyondN;
  check(`host log: "allowed on the PC already -> no 允許 again" for each new session (>= ${wantN}: 3 + ${beyondN} beyond the level)`, noAgainLines() >= wantN, String(noAgainLines()));
  await T.shot(path.join(outDir, 's2_guest_connected.png'));

  // ---- 中斷 (kick) from the PC -> its login is revoked: password again AND 允許 again ----
  {
    const a0 = A.asks();
    A.dev(965, 2, 600);  // 中斷 from the PC
    const bye = await T.waitFor(async () => { const st = await state(); return st && st.bye ? st.bye : null; }, 8000);
    check('中斷 on the PC: the page is told (bye)', !!bye, String(bye));
    await sleep(1000);
    await T.ev(`(() => { retryN = 0; S.bye = ''; connect(); return 1; })()`);
    await sleep(1500);
    const form = await relogin(G1.pw);
    const asked = await A.answer(1, 15000);
    const ok = await T.waitFor(() => T.ev(connectedJs), 25000);
    check('中斷 -> logged in again: 允許 asked again (memory cleared)', asked && A.asks() === a0 + 1, `form ${form} asked ${asked} newAsks ${A.asks() - a0} connected ${!!ok}`);
    // and after that new 允許 a switch is quiet again
    const qNow = (await state()).q, qTo = qNow === 1 ? 0 : 1;  // a change that really changes something
    const r = await stepIn(async () => { await openSettings(); await sleep(200); await T.ev(`(() => { $('segQ').querySelector('[data-v="${qTo}"]').click(); return 1; })()`); });
    check('  ...after the new 允許: 切畫質 quiet again (in the connection, or beyond the level)', r.ok && !r.waited && r.newAsks === 0 && r.st.q === qTo, `${stepNote(r)} q ${qNow} -> ${r.st && r.st.q}`);
    await closeSheets_();
  }
  // ---- 刪除 on the PC while connected -> ends now; the account no longer logs in ----
  {
    A.dev(986, 0, 300);
    const rows = ((A.readLog().match(/dev guests: (\d+): ([^\n]*)/g) || []).slice(-1)[0] || '');
    const idx = rows.replace(/^dev guests: \d+: /, '').split(' | ').findIndex(r => r.startsWith(G1.name + ' '));
    A.dev(450 + Math.max(0, idx), 0, 800);
    const bye = await T.waitFor(async () => { const st = await state(); return st && st.bye ? st.bye : null; }, 8000);
    const a0 = A.asks();
    await sleep(1000);
    await login(G1.name, G1.pw);
    await sleep(3500);
    const st = await T.ev(`({st: S.state, err: ($('passErr') && $('passErr').textContent) || '', form: $('pass').style.display})`);
    check('刪除 while connected: the page ends at once; the deleted account cannot log in (no 允許 asked)', !!bye && A.asks() === a0 && !(await T.ev(connectedJs)), `bye ${bye} row ${idx} ${JSON.stringify(st)}`);
    ev2.deleted = {bye, st};
  }
  // ---- 拒絕 (G2, a fresh phone): asked again after the 20 s pause ----
  {
    const a0 = A.asks();
    await login(G2.name, G2.pw, {fresh: true});
    const asked = await A.answer(2, 15000);  // 拒絕
    const denied = await T.waitFor(() => T.ev(`S.state === 'denied' ? 1 : 0`), 15000);
    check('拒絕: the page says denied', asked && !!denied);
    await sleep(21000);  // the host's 20 s "just denied" pause
    await T.ev(`(() => { retryN = 0; connect(); return 1; })()`);
    const asked2 = await A.answer(1, 15000);
    const ok = await T.waitFor(() => T.ev(connectedJs), 25000);
    check('拒絕 -> the next try asks 允許 again (not remembered); 允許 -> connected', asked2 && A.asks() === a0 + 2 && !!ok, `asks +${A.asks() - a0} connected ${!!ok}`);
    const r = await again(() => T.ev(`(() => { stopPc(); connect('lost'); return 1; })()`));
    check('  ...then 斷線重連 quiet', r.ok && !r.waited && r.newAsks === 0, `ok ${r.ok} newAsks ${r.newAsks}`);
  }
  // ---- G3 for 到期 ----
  var G3 = await A.makeGuest(1 | 8);
  check('G3 made (for 到期)', !!(G3.name && G3.pw), G3.name);
} catch (e) { check('s2 part 1 ran without an exception', false, String(e && e.stack || e)); }
try { await T.close(); } catch {}
let st = await A.stop();
check('app quit (part 1)', !st.killed, JSON.stringify(st));

// ---- 到期: the same data folder, G3's expiry moved to now + 50 s, the app started again ----
try {
  const gf = path.join(A.dataDir, 'remote_guests.txt');
  const now = Math.floor(Date.now() / 1000);
  const rows = fs.readFileSync(gf, 'utf8').split('\n');
  let edited = 0;
  const out = rows.map(l => { const f = l.split('\t'); if (f.length >= 9 && G3 && f[1] === G3.name) { f[4] = String(now + 50); edited++; return f.join('\t'); } return l; }).join('\n');
  check('到期 setup: G3 expires in 50 s (its row in the instance\'s own remote_guests.txt)', edited === 1);
  A = await startApp(exe, outDir, {tag: 'guest', keepData: true, guestsFile: out});
  B.watch.add(A.pid);
  check('app up again', !!A.lanUrl && A.fixedReady);
  T = await B.tab({w: 390, h: 844, dpr: 3, mobile: true});
  const a0 = A.asks();
  await login(G3.name, G3.pw, {fresh: true});
  const asked = await A.answer(1, 15000);
  const ok = await T.waitFor(() => T.ev(connectedJs), 25000);
  check('到期: G3 logged in before its expiry (允許 asked once)', asked && !!ok && A.asks() === a0 + 1);
  const r = await again(() => T.ev(`(() => { stopPc(); connect('lost'); return 1; })()`));
  check('到期 (before): 斷線重連 quiet', r.ok && !r.waited && r.newAsks === 0);
  const left = 50 - (Math.floor(Date.now() / 1000) - now);
  await sleep(Math.max(0, left + 3) * 1000);
  const a1 = A.asks();
  await T.ev(`(() => { stopPc(); connect('lost'); return 1; })()`);
  await sleep(6000);
  const st2 = await T.ev(`({st: S.state, form: $('pass').style.display, err: ($('passErr') && $('passErr').textContent) || '', conn: ${connectedJs}})`);
  check('到期 -> 斷線重連 no longer gets in straight away (login form / ended), no 允許 shown', st2.conn === 0 && A.asks() === a1, JSON.stringify(st2));
  // the password typed again: refused as expired, never a 允許
  if (st2.form === 'flex') await relogin(G3.pw);
  else await login(G3.name, G3.pw);
  await sleep(4000);
  const st3 = await T.ev(`({st: S.state, form: $('pass').style.display, err: ($('passErr') && $('passErr').textContent) || '', conn: ${connectedJs}})`);
  check('到期 -> the right password again: refused (expired), no 允許 asked', st3.conn === 0 && A.asks() === a1, JSON.stringify(st3));
  ev2.expired = {st2, st3, log: (A.readLog().match(/[^\n]*(expired|到期)[^\n]*/g) || []).slice(-6)};
  await T.shot(path.join(outDir, 's2_guest_expired.png'));
} catch (e) { check('s2 part 2 (到期) ran without an exception', false, String(e && e.stack || e)); }
try { await T.close(); } catch {}
const alt = await B.close();
st = await A.stop();
check('app quit (part 2)', !st.killed, JSON.stringify(st));
check(`Alt+Tab / taskbar windows of the test's processes = 0 (${alt.samples} samples)`, alt.samples > 0 && alt.max === 0, alt.seen.join(' ; '));
const all = fs.readdirSync(outDir).filter(f => /^app_guest/.test(f)).map(f => fs.readFileSync(path.join(outDir, f), 'utf8')).join('\n');
check('no real SendInput in the log', !/input: SendInput\(/.test(all));
R.save('s2_guest.json', {evidence: ev2, G: {G1: '(made in the window)', G3: G3 && G3.name}});
console.log(R.failed() ? `FAILED (${R.failed()})` : 'ALL PASSED');
process.exit(R.failed() ? 1 : 0);
