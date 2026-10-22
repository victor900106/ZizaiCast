// 0.8.4 (feat-084-privacy) 隱私黑屏 + 電腦的檔案, end to end against pm_remote_spike (node 22+):
//   node p084_test.mjs <pm_remote_spike.exe> <outDir> [--before <old spike.exe>] [--mb N]
// The spike runs with --loopback (127.0.0.3 / 127.0.0.2 only: no firewall prompt), --no-tailscale, dry-run input
// (nothing is typed or clicked on this PC), --no-audio, --scene synth (a made-up desktop: the owner's screen is not
// in any screenshot), --privacy-test offscreen (the cover's windows are 1x1 and off every screen) and
// --pcfs-root = a sandbox under <outDir> (never this PC's own folders). The phone is a headless, muted browser with a
// throw-away profile (chrome-headless-shell via quiet_browser.mjs: no window; never the owner's Chrome). Nothing else is started; only these processes are ended.
// --before: the same screens on the old spike (before screenshots only).
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import {launch} from './quiet_browser.mjs';  // chrome-headless-shell: no window, no Alt+Tab entry (never Edge / Chrome with windows)

const args = process.argv.slice(2);
const [spikeExe, outDir] = args;
let beforeExe = '', mbs = 64;
for (let i = 2; i < args.length; i++) { if (args[i] === '--before') beforeExe = args[++i]; else if (args[i] === '--mb') mbs = +args[++i]; }
if (!spikeExe || !outDir) { console.error('usage: node p084_test.mjs <spike.exe> <outDir> [--before <old spike>] [--mb N]'); process.exit(2); }
fs.mkdirSync(outDir, {recursive: true});
const shots = path.join(outDir, 'shots');
fs.mkdirSync(shots, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
let pass = 0, fail = 0;
const results = [];
const check = (what, ok, detail) => { ok ? pass++ : fail++; results.push({what, ok: !!ok, detail}); console.log((ok ? 'PASS  ' : 'FAIL  ') + what + (detail !== undefined ? '  -- ' + JSON.stringify(detail) : '')); };
const OWNER = 'Owner-Test-2026';
const G = {files: 'GHJ789', nofiles: 'KMN234', view: 'DEF567'};

// ---- sandbox ----
const box = path.join(outDir, 'sandbox');
const unlinkJ = () => { for (const j of ['jout']) { try { fs.rmdirSync(path.join(box, 'Docs', j)); } catch {} } try { fs.unlinkSync(path.join(box, 'Docs', 'swap.txt')); } catch {} };
unlinkJ();  // the links first (never what they point to)
fs.rmSync(box, {recursive: true, force: true});
const root = path.join(box, 'Docs'), extra = path.join(box, 'Extra'), outside = path.join(box, 'outside'), sib = path.join(box, 'Docs2');
for (const d of [root, path.join(root, 'sub'), path.join(root, '資料夾'), extra, outside, sib]) fs.mkdirSync(d, {recursive: true});
fs.writeFileSync(path.join(root, 'a.txt'), 'hello from the PC');
fs.writeFileSync(path.join(root, 'sub', 'b.txt'), 'bbbb');
fs.writeFileSync(path.join(root, '資料夾', '報告 (1).txt'), '中文檔名');
const big = crypto.randomBytes(mbs * 1048576);
fs.writeFileSync(path.join(root, 'big.bin'), big);
fs.writeFileSync(path.join(root, 'swap.txt'), 'will be swapped for a link');
fs.writeFileSync(path.join(extra, 'extra.txt'), 'extra folder');
fs.writeFileSync(path.join(outside, 'secret.txt'), 'OUTSIDE-SECRET');
fs.writeFileSync(path.join(sib, 's.txt'), 'SIBLING-SECRET');
try { execFileSync('cmd.exe', ['/d', '/c', 'mklink', '/J', path.join(root, 'jout'), outside], {stdio: 'ignore'}); } catch {}
const hash = b => crypto.createHash('sha256').update(b).digest('hex').slice(0, 16);

// ---- spike ----
async function startSpike(exe, tag, extraArgs) {
  const token = crypto.randomBytes(16).toString('hex');
  const port = 19400 + Math.floor(Math.random() * 250);
  const logFile = path.join(outDir, tag + '-spike.log');
  fs.writeFileSync(logFile, '');
  const p = spawn(exe, ['--token', token, '--port', String(port), '--log', logFile, '--duration', '300', '--stdin-quit', '--loopback', '--no-tailscale',
    '--no-audio', '--scene', 'synth', '--test-clipboard', 'x', '--fixed-password', OWNER,
    '--guest', `g-files:${G.files}:3600:control+files`, '--guest', `g-nofiles:${G.nofiles}:3600:control`, '--guest', `g-view:${G.view}:3600:view`,
    ...extraArgs], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
  let out = '';
  p.stdout.on('data', d => { out += d; });
  p.stderr.on('data', d => { out += d; });
  let lan = '';
  for (let i = 0; i < 150 && !lan; i++) { await sleep(100); const m = out.match(/urls: lan (http:\/\/[\d.]+:\d+\/[0-9a-f]+\/)/); if (m) lan = m[1]; }
  if (!lan) throw new Error('spike did not start: ' + out.slice(0, 400));
  log(tag, 'spike pid', p.pid);
  return {p, port, lan, readLog: () => fs.readFileSync(logFile, 'utf8')};
}
async function stopSpike(s) {
  try { s.p.stdin.end(); } catch {}
  for (let i = 0; i < 100 && s.p.exitCode === null; i++) await sleep(100);
  if (s.p.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(s.p.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} log('killed own spike', s.p.pid); }
}

// ---- the phone: chrome-headless-shell ----
async function startEdge(tag, dlDir) {
  const profileDir = path.join(os.tmpdir(), 'edge-084p-' + tag + '-' + crypto.randomBytes(3).toString('hex'));
  const br = await launch(profileDir, ['--allow-loopback-in-peer-connection']);
  const p = br.proc, ver = br.ver;
  if (!ver) throw new Error('the headless browser did not start');
  const ws = new WebSocket(ver.webSocketDebuggerUrl);
  await new Promise(r => ws.addEventListener('open', r));
  let nextId = 1;
  const pend = new Map();
  const keepAlive = setInterval(() => {}, 1000);  // a closed CDP socket must not leave the test waiting with nothing to run
  ws.addEventListener('close', () => { clearInterval(keepAlive); for (const r of pend.values()) r({error: 'cdp closed'}); pend.clear(); });
  ws.addEventListener('message', e => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m.result || m.error); pend.delete(m.id); } });
  const cdp = (method, params = {}, sessionId) => new Promise(r => { const id = nextId++; pend.set(id, r); ws.send(JSON.stringify({id, method, params, ...(sessionId ? {sessionId} : {})})); });
  if (dlDir) await cdp('Browser.setDownloadBehavior', {behavior: 'allow', downloadPath: dlDir, eventsEnabled: false});
  const e = {p, cdp};
  e.open = async (url, w = 390, h = 844) => {
    const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
    const {sessionId} = await cdp('Target.attachToTarget', {targetId, flatten: true});
    await cdp('Emulation.setDeviceMetricsOverride', {width: w, height: h, deviceScaleFactor: 2, mobile: w < 700}, sessionId);
    if (w < 700) await cdp('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5}, sessionId);
    await cdp('Page.navigate', {url}, sessionId);
    const ev = async (expression, awaitPromise = false) => { const r = await cdp('Runtime.evaluate', {expression, returnByValue: true, awaitPromise}, sessionId); return r && r.result ? r.result.value : undefined; };
    const shot = async name => { const r = await cdp('Page.captureScreenshot', {format: 'png'}, sessionId); if (r && r.data) fs.writeFileSync(path.join(shots, name), Buffer.from(r.data, 'base64')); };
    return {ev, shot, close: () => cdp('Target.closeTarget', {targetId})};
  };
  e.close = async () => {
    try { await Promise.race([cdp('Browser.close'), sleep(3000)]); } catch {}
    for (let i = 0; i < 50 && p.exitCode === null; i++) await sleep(100);
    if (p.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(p.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} }
    await sleep(500);
    try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
  };
  return e;
}
const waitFor = async (fn, ms, step = 200) => { for (let t = 0; t < ms; t += step) { const v = await fn(); if (v) return v; await sleep(step); } return null; };
const frame = tab => waitFor(() => tab.ev('window.__pmFirstFrame ? 1 : 0'), 20000);
// Every message p084.js sees (all of the PC's messages pass it first).
const hook = tab => tab.ev(`(() => { if (!window.P84) return false; window.__m = []; const o = P84.onMsg; P84.onMsg = m => { window.__m.push(m); return o(m); }; window.__stub = false; const st = P84.transfer.start; P84.transfer.start = (k, n) => { if (!window.__stub) st(k, n); }; return true; })()`);
const msgs = (tab, t) => tab.ev(`(window.__m || []).filter(m => m.t === ${JSON.stringify(t)})`);
const lastMsg = async (tab, t, after = 0) => { const a = await msgs(tab, t); return a && a.length > after ? a[a.length - 1] : null; };
const ask = async (tab, o, t) => { const n = ((await msgs(tab, t)) || []).length; await tab.ev(`send(${JSON.stringify(o)}), 1`); return waitFor(() => lastMsg(tab, t, n), 5000); };
const pidAlive = pid => { try { return execFileSync('tasklist', ['/FI', `PID eq ${pid}`, '/NH'], {encoding: 'utf8'}).includes(String(pid)); } catch { return false; } };
async function fixedLogin(e, port, user, pw) {
  const t = await e.open(`http://127.0.0.2:${port}/`);
  await waitFor(() => t.ev(`$('pass').style.display === 'flex'`), 8000);
  await t.ev(`(() => { LOGIN.guest = true; LOGIN.user = ''; $('userIn').value = ${JSON.stringify(user)}; $('passIn').value = ${JSON.stringify(pw)}; submitPass(); return 1; })()`);
  t.frame = !!(await frame(t));
  await hook(t);
  return t;
}

// ================================================================ before (screenshots only)
if (beforeExe) {
  const s = await startSpike(beforeExe, 'before', []);
  const e = await startEdge('before');
  try {
    const t = await e.open(s.lan);
    await frame(t);
    await sleep(1500);
    await t.shot('before_phone_toolbar.png');
    await t.ev(`$('bFile').click(), 1`);
    await sleep(1200);
    await t.shot('before_phone_files.png');
    await t.close();
    const p = await e.open(s.lan, 1280, 800);
    await frame(p);
    await p.ev(`(() => { S.mouseUsed = true; document.body.classList.add('pcui'); $('pcbar').classList.add('show'); return 1; })()`);
    await sleep(800);
    await p.shot('before_pc_bar.png');
    await p.close();
  } finally { await e.close(); await stopSpike(s); }
}

// ================================================================ after
const dlDir = path.join(outDir, 'downloads');
fs.rmSync(dlDir, {recursive: true, force: true});
fs.mkdirSync(dlDir, {recursive: true});
const s = await startSpike(spikeExe, 'after', ['--privacy-test', 'offscreen', '--pcfs-root', 'test=' + root, '--pcfs-root', 'extra0=' + extra]);
const e = await startEdge('after', dlDir);
try {
  // ---- A. the owner (LAN link) ----
  const t = await e.open(s.lan);
  check('owner: connected (first frame)', await frame(t));
  check('owner: p084.js loaded (inline)', await hook(t));
  const st = await waitFor(async () => (await t.ev('P84.st.supported && P84.st.pcfs')) && t.ev('JSON.parse(JSON.stringify(P84.st))'), 6000);
  check('owner: the PC says 黑幕 is supported / allowed and 電腦的檔案 is on', st && st.supported && st.ok && st.pcfs, st);
  const four = await t.ev(`(() => { const bad = P84.keys.filter(k => !(T[k].length === 4 && T[k].every(x => typeof x === 'string' && x.length > 0))); return {n: P84.keys.length, bad}; })()`);
  check('p084.js: every string in 4 languages (zh-TW / en / ja / ko)', four && four.n > 30 && four.bad.length === 0, four);
  check('owner: 黑幕 button shown on the toolbar', await t.ev(`$('bPriv').style.display !== 'none' && $('bPriv').parentNode.id === 'bar'`));
  await sleep(800);
  await t.shot('after_phone_toolbar.png');
  // 黑幕 on
  await t.ev(`$('bPriv').click(), 1`);
  const on = await waitFor(() => t.ev('P84.st.privacy'), 6000);
  const pidM = s.readLog().match(/privacy: cover on \(pid (\d+), offscreen\)/);
  const coverPid = pidM ? +pidM[1] : 0;
  check('privacy on from the phone: the PC shows its cover (test mode: off-screen)', on && coverPid && pidAlive(coverPid), {coverPid});
  check('privacy on: the page says so (button on, toast)', await t.ev(`$('bPriv').classList.contains('on') && $('toast').textContent.length > 0`));
  await sleep(300);
  await t.shot('after_phone_privacy_on.png');
  check('privacy: the picture keeps coming while the cover is on', await t.ev('(() => { const a = S.fps; return a > 0; })()'));
  await t.ev(`$('bPriv').click(), 1`);
  const off = await waitFor(async () => !(await t.ev('P84.st.privacy')), 6000);
  check('privacy off from the phone: the cover process ends', off && await waitFor(async () => !pidAlive(coverPid), 4000), {coverPid});
  // on again, then the phone leaves -> off with the session
  await t.ev(`$('bPriv').click(), 1`);
  await waitFor(() => t.ev('P84.st.privacy'), 6000);
  const pid2 = +((s.readLog().match(/privacy: cover on \(pid (\d+)/g) || []).pop() || '').replace(/\D+/g, ' ').trim().split(' ').pop();
  // 電腦的檔案 while it is on
  await t.ev(`$('bFile').click(), 1`);
  await sleep(400);
  check('files sheet: two tabs (傳檔 / 電腦的檔案)', await t.ev(`$('fsTabs').style.display !== 'none' && $('fsTabPc').textContent === '電腦的檔案'`));
  await t.shot('after_phone_files_mine.png');
  await t.ev(`$('fsTabPc').click(), 1`);
  const roots = await waitFor(() => lastMsg(t, 'pcfs-roots'), 5000);
  check('roots: only the allowed folders (the sandbox test root + one extra)', roots && roots.roots.map(r => r.id).join(',') === 'test,extra0', roots && roots.roots);
  await sleep(300);
  check('top of the file browser reads 最上層', await t.ev(`$('fsPath').textContent`) === '最上層');
  await t.shot('after_phone_files_roots.png');
  await t.ev(`document.querySelector('#fsList li[data-root="test"]').click(), 1`);
  const list = await waitFor(() => lastMsg(t, 'pcfs-list'), 5000);
  const names = list && list.entries ? list.entries.map(x => x.n + (x.d ? '/' : '')) : [];
  check('crumb inside a folder: 最上層 / Docs', await t.ev(`$('fsPath').textContent`) === '最上層 / Docs', await t.ev(`$('fsPath').textContent`));
  check('list: files and folders of the root, folders first', names.join(' ') === 'sub/ 資料夾/ a.txt big.bin swap.txt', names);
  check('list: the junction that leads out (jout) is not listed', !names.includes('jout/'));
  await sleep(300);
  await t.shot('after_phone_files_list.png');
  // path escapes over the DataChannel (what a modified page could send)
  const esc = {};
  for (const p of ['..', '../outside', '../Docs2', 'jout', 'sub/../..', 'sub\\..\\..', 'C:/Windows', '/', '//127.0.0.1/c$', 'a.txt::$DATA'])
    esc[p] = (await ask(t, {t: 'pcfs-list', root: 'test', path: p}, 'pcfs-list')).err || 'LISTED';
  check('list: every escape refused', Object.values(esc).every(v => v !== 'LISTED'), esc);
  check('list: an unknown root refused', (await ask(t, {t: 'pcfs-list', root: 'C:', path: ''}, 'pcfs-list')).err === 'no-root');
  const bad = await ask(t, {t: 'pcfs-get', root: 'test', items: ['../outside/secret.txt', '../Docs2/s.txt', 'jout/secret.txt', '..\\outside\\secret.txt', 'C:/Windows/win.ini',
    'a.txt::$DATA', 'CON', 'sub', '%2e%2e/outside/secret.txt']}, 'pcfs-keys');
  check('download: every escape refused, no key given', bad && bad.list.every(x => !x.k && x.err), bad && bad.list.map(x => x.p + ' -> ' + (x.err || 'KEY')));
  check('log: nothing outside the root was ever offered / sent', !/OUTSIDE-SECRET|secret\.txt"? \(\d|downloads "secret/.test(s.readLog()) && !/DOWNLOAD pc .*secret/.test(s.readLog()));
  // two files ticked in the page, downloaded with progress
  await t.ev(`(() => { for (const li of document.querySelectorAll('#fsList li.file')) if (/^(a\\.txt|big\\.bin)/.test(li.querySelector('.nm').textContent)) li.click(); return 1; })()`);
  check('ticking two files: the button says 下載選取的 (2)', await t.ev(`$('fsGet').textContent`) === '下載選取的 (2)');
  await t.shot('after_phone_files_ticked.png');
  await t.ev(`$('fsGet').click(), 1`);
  const done = await waitFor(() => t.ev('P84.stats.done >= 2 ? P84.stats : 0'), 60000, 300);
  check('download: both done (the PC sent every byte)', done && done.done >= 2 && done.cut === 0, done);
  const progs = await msgs(t, 'pcfs-prog');
  const bigKey = progs && (progs.find(m => m.total === big.length) || {}).k;
  const bigProg = (progs || []).filter(m => m.k === bigKey);
  check(`download: progress for the big file (${mbs} MB) arrives in steps`, bigProg.length >= 2 && bigProg[bigProg.length - 1].end, bigProg.map(m => Math.round(m.got / 1048576)));
  await sleep(300);
  await t.shot('after_phone_files_progress.png');
  const gotA = await waitFor(() => fs.existsSync(path.join(dlDir, 'a.txt')) && fs.readFileSync(path.join(dlDir, 'a.txt'), 'utf8'), 8000);
  const gotBig = await waitFor(() => fs.existsSync(path.join(dlDir, 'big.bin')) && fs.statSync(path.join(dlDir, 'big.bin')).size === big.length && hash(fs.readFileSync(path.join(dlDir, 'big.bin'))), 20000);
  check('download: a.txt saved by the browser, same content', gotA === 'hello from the PC');
  check('download: big.bin saved by the browser, same bytes (sha-256)', gotBig === hash(big), gotBig);
  const dlLog = s.readLog().match(/event: DOWNLOAD pc .*/g) || [];
  check('連線紀錄 hook: every download reported (onDownload: kind pc, folder/path, bytes)', dlLog.length === 2 && dlLog.some(l => l.includes('test/big.bin (' + big.length)), dlLog);
  // a used key again -> 410
  const keys = (await msgs(t, 'pcfs-keys')).flatMap(m => m.list).filter(x => x.k);
  const again = await t.ev(`fetch(BASE + 'dl/' + ${JSON.stringify(keys[0] && keys[0].k)}).then(r => r.status)`, true);
  check('download: a used link does not work again (410)', again === 410, again);
  // folder (sub-path) download + UTF-8 name; from here the test fetches the keys itself (the page would start them)
  await t.ev('window.__stub = true');
  const k2 = await ask(t, {t: 'pcfs-get', root: 'test', items: ['資料夾/報告 (1).txt', 'sub/b.txt']}, 'pcfs-keys');
  const body2 = k2 && await t.ev(`fetch(BASE + 'dl/' + ${JSON.stringify(k2.list[0].k)}).then(r => r.text())`, true);
  check('download: a file in a sub-folder with a Chinese name', body2 === '中文檔名', body2);
  // the file swapped for a link after its key was given -> refused at download
  const k3 = await ask(t, {t: 'pcfs-get', root: 'test', items: ['swap.txt']}, 'pcfs-keys');
  fs.rmSync(path.join(root, 'swap.txt'));
  let swapped = false;
  try { fs.symlinkSync(path.join(outside, 'secret.txt'), path.join(root, 'swap.txt'), 'file'); swapped = true; } catch {}
  if (!swapped) { try { execFileSync('cmd.exe', ['/d', '/c', 'mklink', path.join(root, 'swap.txt'), path.join(outside, 'secret.txt')], {stdio: 'ignore'}); swapped = true; } catch {} }
  const st3 = k3 && k3.list[0].k ? await t.ev(`fetch(BASE + 'dl/' + ${JSON.stringify(k3.list[0].k)}).then(async r => r.status + ' ' + (await r.text()).slice(0, 40))`, true) : 'no key';
  if (swapped) check('download: a file swapped for a link out after its key was given is refused (403), the secret never sent', /^403/.test(st3) && !st3.includes('OUTSIDE'), st3);
  else results.push({what: 'swap for a symbolic link: skipped (no symlink rights)', ok: true});
  // the extra folder (允許清單)
  const lx = await ask(t, {t: 'pcfs-list', root: 'extra0', path: ''}, 'pcfs-list');
  check('允許清單: the extra folder is listed', lx && lx.entries && lx.entries.some(x => x.n === 'extra.txt'));
  // the phone leaves while the cover is on -> off
  await t.close();
  await waitFor(() => /privacy: OFF \(ended\)/.test(s.readLog()), 8000);
  check('privacy: the phone left -> the cover ends with the session', /privacy: OFF \(ended\)/.test(s.readLog()) && await waitFor(async () => !pidAlive(pid2), 4000), {pid2});

  // ---- English page (the four languages: zh-TW above, en here; ja / ko: strings checked below) ----
  // ---- B. a guest with 傳檔 (control + files) ----
  const dl0 = (s.readLog().match(/event: DOWNLOAD pc /g) || []).length;
  const gf = await fixedLogin(e, s.port, 'g-files', G.files);
  check('guest with 傳檔: connected', gf.frame);
  await gf.ev(`send({t: 'p84-get'}), 1`);
  const gst = await waitFor(() => lastMsg(gf, 'p84'), 5000);
  check('guest with 傳檔, 臨時帳號 switch off (default): 電腦的檔案 NOT offered (pcfsWhy guestOff)', gst && gst.pcfs === false && gst.pcfsWhy === 'guestOff', gst);
  check('guest with 傳檔: 黑幕 NOT allowed (why: guest)', gst && gst.privacyOk === false && gst.privacyWhy === 'guest');
  const gpv = await ask(gf, {t: 'privacy', on: true}, 'p84');
  check('guest: privacy on refused by the PC (no cover started)', gpv && gpv.ok === false && gpv.why === 'guest' && !/privacy: cover on .*\n.*g-files/.test(s.readLog()), gpv);
  const gl = await ask(gf, {t: 'pcfs-roots'}, 'pcfs-err');
  check('guest with 傳檔, switch off: roots refused by the PC (guestOff)', gl && gl.why === 'guestOff', gl);
  const gll = await ask(gf, {t: 'pcfs-list', root: 'test', path: ''}, 'pcfs-err');
  check('guest, switch off: list refused (guestOff)', gll && gll.why === 'guestOff', gll);
  const glg = await ask(gf, {t: 'pcfs-get', root: 'test', items: ['a.txt']}, 'pcfs-err');
  check('guest, switch off: download keys refused, none handed out', glg && glg.why === 'guestOff' && !(await msgs(gf, 'pcfs-keys')).length, glg);
  check('guest, switch off: the files tab stays hidden', await gf.ev(`$('fsTabs').style.display === 'none'`));
  await waitFor(async () => /event: DOWNLOAD denied pc-guest:list/.test(s.readLog()), 4000);
  check('guest, switch off: refusals reach the 連線紀錄 hook (DOWNLOAD denied pc-guest:list, throttled to one row per 5 s)',
    /event: DOWNLOAD denied pc-guest:list.*/.test(s.readLog()) && (s.readLog().match(/event: DOWNLOAD denied/g) || []).length === 1, (s.readLog().match(/event: DOWNLOAD denied.*/g) || []));
  check('guest, switch off: no PC file was offered or sent to it', (s.readLog().match(/event: DOWNLOAD pc /g) || []).length === dl0);
  await gf.close();
  await sleep(1500);
  // ---- C. a guest without 傳檔 ----
  const gn = await fixedLogin(e, s.port, 'g-nofiles', G.nofiles);
  check('guest without 傳檔: connected', gn.frame);
  const gnr = await ask(gn, {t: 'pcfs-roots'}, 'pcfs-err');
  check('guest without 傳檔, switch off: 電腦的檔案 refused (guestOff)', gnr && gnr.why === 'guestOff', gnr);
  const gng = await ask(gn, {t: 'pcfs-get', root: 'test', items: ['a.txt']}, 'pcfs-err');
  check('guest without 傳檔: download keys refused', gng && gng.why === 'guestOff');
  await gn.close();
  await sleep(1500);
  // ---- D. 僅觀看 (a view-only guest) ----
  const gv = await fixedLogin(e, s.port, 'g-view', G.view);
  check('僅觀看 guest: connected', gv.frame);
  const gvr = await ask(gv, {t: 'pcfs-list', root: 'test', path: ''}, 'pcfs-err');
  check('僅觀看: 電腦的檔案 refused (viewOnly)', gvr && gvr.why === 'viewOnly', gvr);
  const gvp = await ask(gv, {t: 'privacy', on: true}, 'p84');
  check('僅觀看: 黑幕 refused (viewOnly)', gvp && gvp.ok === false && gvp.why === 'viewOnly', gvp);
  check('僅觀看: the 黑幕 button looks off', await gv.ev(`$('bPriv').classList.contains('off')`));
  await gv.close();
  await sleep(1000);
  check('owner is not affected by the 臨時帳號 switch: roots still listed', await (async () => { const o = await e.open(s.lan); await frame(o); await hook(o); const r = await ask(o, {t: 'pcfs-roots'}, 'pcfs-roots'); await o.close(); return r && r.roots.length === 2; })());
  check('no cover left running (every cover on has its OFF)', (s.readLog().match(/privacy: cover on/g) || []).length === (s.readLog().match(/privacy: cover off/g) || []).length);

  // ---- E. screenshots: the PC browser's bar (1280 wide) and English ----
  const p = await e.open(s.lan, 1280, 800);
  await frame(p); await hook(p);
  await waitFor(() => p.ev('P84.st.supported'), 5000);
  await p.ev(`(() => { S.mouseUsed = true; document.body.classList.add('pcui'); $('pcbar').classList.add('show'); return 1; })()`);
  await sleep(600);
  check('PC browser bar: 黑幕 button there', await p.ev(`!!$('pbPriv') && $('pbPriv').style.display !== 'none'`));
  await p.shot('after_pc_bar.png');
  await p.close();
} catch (err) {
  check('test ran without an exception', false, String(err && err.stack || err));
} finally {
  await e.close();
  await stopSpike(s);
}
// ---- F. the 臨時帳號 switch ON (spike --pcfs-guest = settings remote_pc_files_guest=1): independent of 傳檔; 僅觀看 still never ----
{
  const s3 = await startSpike(spikeExe, 'guest-on', ['--privacy-test', 'offscreen', '--pcfs-root', 'test=' + root, '--pcfs-guest']);
  const e3 = await startEdge('guest-on', dlDir);
  try {
    const gn = await fixedLogin(e3, s3.port, 'g-nofiles', G.nofiles);
    check('switch on, guest WITHOUT 傳檔: connected', gn.frame);
    await gn.ev(`send({t: 'p84-get'}), 1`);
    const st3 = await waitFor(() => lastMsg(gn, 'p84'), 5000);
    check('switch on, guest without 傳檔: 電腦的檔案 offered (independent of 傳檔)', st3 && st3.pcfs === true, st3);
    const r3 = await ask(gn, {t: 'pcfs-roots'}, 'pcfs-roots');
    check('switch on, guest without 傳檔: roots listed', r3 && r3.roots.length === 1, r3);
    const l3 = await ask(gn, {t: 'pcfs-list', root: 'test', path: ''}, 'pcfs-list');
    check('switch on, guest: list works', l3 && !l3.err && l3.entries.some(x => x.n === 'a.txt'), l3 && l3.err);
    await gn.ev('window.__stub = true, 1');
    const k3 = await ask(gn, {t: 'pcfs-get', root: 'test', items: ['a.txt', '../outside/secret.txt']}, 'pcfs-keys');
    check('switch on, guest: a key for a.txt, the escape still refused', k3 && k3.list[0].k && k3.list[1].err, k3);
    const g3 = await gn.ev(`fetch(BASE + 'dl/' + ${JSON.stringify(k3 && k3.list[0].k)}).then(r => r.text())`, true);
    check('switch on, guest: the file downloads', g3 === 'hello from the PC', g3);
    check('switch on, guest: 連線紀錄 hook names the file', /event: DOWNLOAD pc test[/]a[.]txt.*/.test(s3.readLog()));
    check('switch on, guest: still no 黑幕', (await ask(gn, {t: 'privacy', on: true}, 'p84')).why === 'guest');
    await gn.close();
    await sleep(1500);
    const gv = await fixedLogin(e3, s3.port, 'g-view', G.view);
    const v3 = await ask(gv, {t: 'pcfs-list', root: 'test', path: ''}, 'pcfs-err');
    check('switch on, 僅觀看 guest: still refused (viewOnly)', v3 && v3.why === 'viewOnly', v3);
    await gv.close();
  } catch (err) {
    check('guest-switch-on block ran without an exception', false, String(err && err.stack || err));
  } finally { await e3.close(); await stopSpike(s3); }
}
let top = '';
const lE = await (async () => {
  const s2 = await startSpike(spikeExe, 'en', ['--lang', '1', '--privacy-test', 'offscreen', '--pcfs-root', 'test=' + root]);
  const e2 = await startEdge('en');
  try {
    const t = await e2.open(s2.lan);
    await frame(t); await hook(t);
    await waitFor(() => t.ev('P84.st.supported'), 5000);
    await t.ev(`$('bFile').click(), 1`); await sleep(300);
    await t.ev(`$('fsTabPc').click(), 1`);
    await waitFor(() => lastMsg(t, 'pcfs-roots'), 5000);
    await sleep(300);
    top = await t.ev(`$('fsPath').textContent`);
    await t.ev(`document.querySelector('#fsList li[data-root="test"]').click(), 1`);
    await waitFor(() => lastMsg(t, 'pcfs-list'), 5000);
    await sleep(400);
    check('English crumb inside a folder: Top level / Docs', await t.ev(`$('fsPath').textContent`) === 'Top level / Docs');
    await t.shot('after_phone_files_list_en.png');
    return await t.ev(`[$('lbPriv').textContent, $('fsTabPc').textContent]`);
  } finally { await e2.close(); await stopSpike(s2); }
})();
check('English page: 黑幕 / 電腦的檔案 in English', lE && lE[0] === 'Cover' && lE[1] === 'PC files', lE);
check('English page: the top of the file browser reads "Top level" (not Folders)', top === 'Top level', top);

unlinkJ();
fs.rmSync(box, {recursive: true, force: true});
fs.writeFileSync(path.join(outDir, 'p084_results.json'), JSON.stringify({pass, fail, results}, null, 1));
console.log(`\nPASS ${pass} / FAIL ${fail}`);
process.exit(fail ? 1 : 0);
