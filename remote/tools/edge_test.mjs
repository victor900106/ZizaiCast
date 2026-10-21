// Automated Edge test for pm_remote_spike (node 22+, built-in WebSocket).
//   node edge_test.mjs <spike.exe> <outDir> [--via lan|tailscale] [--visible] [--seconds N]
//                      [--profile DIR] [--page-query q] [--loopback] [spike args...]
// --loopback (0.8.3.1): the spike serves its "LAN" side on 127.0.0.3 only and Edge may use
// loopback ICE candidates: a fresh build path makes no Windows Firewall prompt.
// Starts the spike and Microsoft Edge with a throw-away profile (default
// %TEMP%\edge-080), opens the page through the LAN IP (or this PC's Tailscale
// IP), emulates a phone (touch, 390x844), plays trackpad gestures through
// CDP, turns the sound on, takes screenshots, reads the page's state, checks
// the spike log, then closes only the processes it started.
// Input is dry-run unless the spike args contain "--inject --test-window":
// then the spike injects ONLY into its own test window and the test checks
// that clicks / keys arrived there and that moves outside it were dropped.
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';

const [spikeExe, outDir, ...rest] = process.argv.slice(2);
if (!spikeExe || !outDir) { console.error('usage: node edge_test.mjs <spike.exe> <outDir> [--via lan|tailscale] [--visible] [--seconds N] [spike args]'); process.exit(2); }
let visible = false, seconds = 30, pageQuery = '', via = 'lan', profileDir = path.join(os.tmpdir(), 'edge-080'), lockoutTest = false, loopbackRun = false;
const spikeArgs = [];
for (let i = 0; i < rest.length; i++) {
  if (rest[i] === '--visible') visible = true;
  else if (rest[i] === '--seconds') seconds = +rest[++i];
  else if (rest[i] === '--page-query') pageQuery = '&' + rest[++i];
  else if (rest[i] === '--via') via = rest[++i];
  else if (rest[i] === '--profile') profileDir = rest[++i];
  else if (rest[i] === '--lockout-test') lockoutTest = true;
  else if (rest[i] === '--loopback') { loopbackRun = true; spikeArgs.push('--loopback'); }
  else spikeArgs.push(rest[i]);
}
const inject = spikeArgs.includes('--inject');
if (inject && !spikeArgs.includes('--test-window')) { console.error('refusing: --inject only together with --test-window'); process.exit(2); }
const approval = spikeArgs.includes('--approval'), denyFirst = spikeArgs.includes('--deny');
const viewOnly = spikeArgs.includes('--view-only');
const kickAfter = spikeArgs.includes('--kick-after');
const idleTest = spikeArgs.includes('--idle-minutes');
const clipText = spikeArgs.includes('--test-clipboard') ? spikeArgs[spikeArgs.indexOf('--test-clipboard') + 1] : null;
const simIce = pageQuery.includes('simice'), simLoss = pageQuery.includes('simloss');
const PM_help = spikeArgs.includes('--help-file');
const special = simIce || idleTest || lockoutTest || simLoss;  // focused runs: only their own checks
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
const token = crypto.randomBytes(16).toString('hex');
const port = 8600 + Math.floor(Math.random() * 400);  // never 8090 (the owner's spike)
const logFile = path.join(outDir, 'spike.log');
fs.writeFileSync(logFile, '');

// ---- spike ----
const spike = spawn(spikeExe, ['--token', token, '--port', String(port), '--log', logFile, '--snapdir', outDir,
                               '--duration', String(seconds + 120), '--stdin-quit', ...spikeArgs], {stdio: ['pipe', 'pipe', 'pipe']});
log('spike pid', spike.pid, spikeArgs.join(' '));
let spikeOut = '';
spike.stdout.on('data', d => { spikeOut += d; });
spike.stderr.on('data', d => { spikeOut += d; });
let lanUrl = '', tsUrl = '';
for (let i = 0; i < 100 && !lanUrl; i++) {
  await sleep(100);
  const m = spikeOut.match(/urls: lan (http:\/\/[\d.]+:\d+\/[0-9a-f]+\/) tailscale (\S+)/);
  if (m) { lanUrl = m[1]; tsUrl = m[2] === '-' ? '' : m[2]; }
}
if (!lanUrl) { console.error('spike did not print a URL\n' + spikeOut); spike.kill(); process.exit(1); }
const url = via === 'tailscale' ? tsUrl : lanUrl;
if (!url) { console.error('no ' + via + ' URL (Tailscale not up?)'); spike.stdin.end(); await sleep(3000); process.exit(1); }
if (url.includes('//127.') && !(loopbackRun && url.includes('//127.0.0.3:'))) { console.error('URL is loopback'); spike.kill(); process.exit(1); }
log('page via', via, url);

// ---- Edge (own temp profile) ----
const edgeExe = 'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe';
const cdpPort = 9400 + Math.floor(Math.random() * 400);
const edgeArgs = [`--user-data-dir=${profileDir}`, `--remote-debugging-port=${cdpPort}`, '--no-first-run',
                  '--no-default-browser-check', '--disable-extensions', '--disable-sync',
                  '--disable-features=msEdgeSidebarV2,msHubApps,msEdgeShoppingUI', '--window-size=420,900'];
// No --autoplay-policy flag: the page must cope with the browser default
// (muted autoplay ok, sound only after a tap), like iOS Safari.
if (loopbackRun) edgeArgs.push('--allow-loopback-in-peer-connection');
if (!visible) edgeArgs.push('--headless=new');
edgeArgs.push('about:blank');
// A fresh profile every run (page settings live in its localStorage). Only our own temp profile.
if (/edge-080[\w-]*$/.test(profileDir)) { try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch (e) { console.log('profile not fully removed:', e.code); } }
const edge = spawn(edgeExe, edgeArgs, {stdio: 'ignore'});

log('edge pid', edge.pid, visible ? '(visible)' : '(headless)', 'profile', profileDir);

let ws, nextId = 1;
const pending = new Map();
const events = [];
function cdp(method, params = {}, sessionId) {
  const id = nextId++;
  ws.send(JSON.stringify({id, method, params, ...(sessionId ? {sessionId} : {})}));
  return new Promise((res, rej) => pending.set(id, {res, rej}));
}
async function cleanup(code) {
  try { if (ws) await Promise.race([cdp('Browser.close'), sleep(3000)]); } catch {}
  await sleep(1500);
  try { spike.stdin.end(); } catch {}  // graceful stop (--stdin-quit)
  for (let i = 0; i < 150 && spike.exitCode === null; i++) await sleep(100);
  log('spike exit code', spike.exitCode);
  for (const [name, p] of [['edge', edge], ['spike', spike]]) {
    if (p.exitCode === null) {
      try { execFileSync('taskkill', ['/PID', String(p.pid), '/T', '/F'], {stdio: 'ignore'}); log('killed own', name, p.pid); } catch {}
    }
  }
  // Edge keeps tearing down for a while after Browser.close: wait for every
  // process of OUR throw-away profile to be gone (kill stragglers of that
  // profile only; the owner's own browsers are never touched).
  const mine = () => {
    try {
      const out = execFileSync('powershell', ['-NoProfile', '-Command',
        `Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" | Where-Object { $_.CommandLine -and (Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue | Where-Object { -not $_.HasExited }) -and $_.CommandLine.Contains('${profileDir.replace(/'/g, "''")}') } | ForEach-Object { $_.ProcessId }`],
        {encoding: 'utf8'});
      return out.split(/\s+/).filter(Boolean).map(Number);
    } catch { return []; }
  };
  let left = mine();
  for (let i = 0; i < 20 && left.length; i++) { await sleep(1000); left = mine(); }
  for (const pid of left) { try { execFileSync('taskkill', ['/PID', String(pid), '/T', '/F'], {stdio: 'ignore'}); log('killed own edge straggler', pid); } catch {} }
  log('own edge processes left:', mine().length);
  process.exit(code);

}
const results = {};
try {
  let ver;
  for (let i = 0; i < 50 && !ver; i++) {
    await sleep(200);
    try { ver = await (await fetch(`http://127.0.0.1:${cdpPort}/json/version`)).json(); } catch {}
  }
  if (!ver) throw new Error('Edge DevTools endpoint did not come up');
  log('edge', ver.Browser);
  ws = new WebSocket(ver.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  ws.onmessage = e => {
    const m = JSON.parse(e.data);
    if (m.id && pending.has(m.id)) { const p = pending.get(m.id); pending.delete(m.id); m.error ? p.rej(new Error(m.error.message)) : p.res(m.result); }
    else if (m.method) events.push(m);
  };
  const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
  const {sessionId: S} = await cdp('Target.attachToTarget', {targetId, flatten: true});
  await cdp('Page.enable', {}, S);
  await cdp('Runtime.enable', {}, S);
  await cdp('Emulation.setDeviceMetricsOverride', {width: 390, height: 844, deviceScaleFactor: 2, mobile: true, screenOrientation: {type: 'portraitPrimary', angle: 0}}, S);
  await cdp('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5}, S);
  await cdp('Page.navigate', {url: url + '?auto=1' + pageQuery}, S);
  const evalJs = async expr => (await cdp('Runtime.evaluate', {expression: expr, returnByValue: true, awaitPromise: true, userGesture: true}, S)).result.value;
  const shot = async name => {
    const r = await cdp('Page.captureScreenshot', {format: 'png'}, S);
    const f = path.join(outDir, name);
    fs.writeFileSync(f, Buffer.from(r.data, 'base64'));
    log('screenshot', f);
  };
  const tp = (x, y, id = 0) => ({x, y, id, radiusX: 5, radiusY: 5, force: 1});
  const touch = (type, points) => cdp('Input.dispatchTouchEvent', {type, touchPoints: points}, S);
  const tap = async (x, y) => { await touch('touchStart', [tp(x, y)]); await sleep(50); await touch('touchEnd', []); };
  const isPlaying = () => evalJs('!!(document.getElementById("v").videoWidth && S.state === "connected")').catch(() => false);

  if (approval) {
    await sleep(800);
    await shot('edge-0-pending.png');
    results.pendingShown = await evalJs('S.state').catch(() => '');
    log('state while waiting for approval:', results.pendingShown);
    if (denyFirst) {
      for (let i = 0; i < 20 && (await evalJs('S.state')) !== 'denied'; i++) await sleep(250);
      results.deniedShown = await evalJs('S.state');
      await shot('edge-0-denied.png');
      log('after deny:', results.deniedShown, '- waiting 21 s for the deny cool-down, then retrying');
      await sleep(21000);
      await evalJs('typeof retry === "function" ? retry() : (document.getElementById("msg").click(), true)');
    }
  }
  // wait for playback
  let playing = false;
  for (let i = 0; i < 60 && !playing; i++) { await sleep(500); playing = await isPlaying(); }
  log('playing:', playing, await evalJs('JSON.stringify({state: S.state, w: v.videoWidth, h: v.videoHeight, playErr: S.playErr})'));
  results.playing = playing;
  await sleep(2500);
  await shot('edge-1-playing.png');

  // ---- UI screenshots (portrait): sound call-to-action, settings, gestures, keyboard ----
  results.ctaShown = await evalJs('getComputedStyle(document.getElementById("cta")).display !== "none"');
  await shot('ui-1-portrait-cta.png');
  await evalJs('document.getElementById("bSet").click(), true'); await sleep(300);
  await shot('ui-2-portrait-settings.png');
  results.overflow = await evalJs(`(() => { const bad = []; for (const el of document.querySelectorAll('#set *, #help *, #kb *, #msg *, #bar *')) {
      if (!el.offsetParent || !el.textContent.trim() || el.children.length) continue;
      if (el.scrollWidth > el.clientWidth + 1 || el.scrollHeight > el.clientHeight + 2) bad.push(el.id || el.tagName + ':' + el.textContent.slice(0, 20)); }
      const set = document.getElementById('set').getBoundingClientRect(); if (set.right > innerWidth + 1) bad.push('set sheet wider than the screen');
      return bad; })()`);
  await evalJs('document.getElementById("bHelp").click(), true'); await sleep(300);
  await shot('ui-3-portrait-gestures.png');
  await evalJs('document.getElementById("helpClose").click(), true');
  await evalJs('document.getElementById("bKb").click(), true'); await sleep(300);
  await shot('ui-4-portrait-keyboard.png');
  await evalJs('document.getElementById("kbClose").click(), true');
  // landscape (phone turned sideways)
  await cdp('Emulation.setDeviceMetricsOverride', {width: 844, height: 390, deviceScaleFactor: 2, mobile: true, screenOrientation: {type: 'landscapePrimary', angle: 90}}, S);
  await sleep(800);
  await shot('ui-5-landscape.png');
  await evalJs('document.getElementById("bSet").click(), true'); await sleep(300);
  await shot('ui-6-landscape-settings.png');
  results.overflowLandscape = await evalJs(`(() => { const bad = []; for (const el of document.querySelectorAll('#set *')) {
      if (!el.offsetParent || !el.textContent.trim() || el.children.length) continue;
      if (el.scrollWidth > el.clientWidth + 1) bad.push(el.id || el.tagName + ':' + el.textContent.slice(0, 20)); } return bad; })()`);
  await evalJs('document.getElementById("setClose").click(), true');
  await evalJs('document.getElementById("swFill").click(), true'); await sleep(400);
  results.fillZoom = await evalJs('trackpad.zoom');
  await shot('ui-7-landscape-fill.png');
  await evalJs('document.getElementById("swFill").click(), true');
  await cdp('Emulation.setDeviceMetricsOverride', {width: 390, height: 844, deviceScaleFactor: 2, mobile: true, screenOrientation: {type: 'portraitPrimary', angle: 0}}, S);
  await sleep(500);
  log('UI: overflow portrait', JSON.stringify(results.overflow), 'landscape', JSON.stringify(results.overflowLandscape), 'fill zoom', results.fillZoom);

  // ---- sound: the button (a user gesture) unmutes and asks the PC for audio ----
  await evalJs('(document.getElementById("bSnd") || {click(){}}).click(), true');
  await sleep(6000);
  results.audio = await evalJs('(async () => { await pollStats(); return JSON.stringify({aud: S.aud || null, muted: v.muted, tracks: v.srcObject ? v.srcObject.getTracks().map(t => t.kind + ":" + t.readyState) : []}); })()');
  log('audio:', results.audio);

  // ---- input ----
  let norm = null;
  if (inject) {
    const L0 = fs.readFileSync(logFile, 'utf8');
    const m = L0.match(/testwin: rect .* norm-center ([\d.]+),([\d.]+)/);
    if (m) norm = [+m[1], +m[2]];
    log('test window centre (normalised):', norm);
    // put the pointer in the middle of the test window (direct-click message)
    await evalJs(`send({t: 'abs', x: ${norm[0]}, y: ${norm[1]}}), true`);
    await sleep(300);
  }
  log('gesture: one-finger drag (small move)');
  await touch('touchStart', [tp(150, 400)]);
  for (let i = 1; i <= 6; i++) { await touch('touchMove', [tp(150 + i, 400 + i)]); await sleep(16); }
  await touch('touchEnd', []);
  await sleep(400);
  log('gesture: tap (left click)');
  await tap(200, 450);
  await sleep(600);
  log('keyboard: text');
  await evalJs('document.getElementById("bKb").click(), true');
  await sleep(300);
  await cdp('Input.insertText', {text: 'hi 你好'}, S);
  await sleep(400);
  await evalJs('document.getElementById("kbClose").click(), true');
  await sleep(300);
  log('gesture: two-finger tap (right click)');
  await touch('touchStart', [tp(180, 450, 0), tp(230, 450, 1)]); await sleep(80); await touch('touchEnd', []);
  await sleep(500);
  if (inject) { await evalJs('send({t: "key", k: "Escape"}), true'); await sleep(300); }  // close a context menu, if any
  log('gesture: two-finger scroll');
  await touch('touchStart', [tp(180, 500, 0), tp(230, 500, 1)]);
  for (let i = 1; i <= 12; i++) { await touch('touchMove', [tp(180, 500 - i * 10, 0), tp(230, 500 - i * 10, 1)]); await sleep(16); }
  await touch('touchEnd', []);
  await sleep(800);
  log('gesture: big drag (would leave the test window)');
  await touch('touchStart', [tp(60, 600)]);
  for (let i = 1; i <= 30; i++) { await touch('touchMove', [tp(60 + i * 10, 600 - i * 5)]); await sleep(16); }
  await touch('touchEnd', []);
  await sleep(500);

  // ---- page features: direct mode, pointer speed (remembered), quality switch ----
  if (!kickAfter && !viewOnly && !special) {
    log('direct mode: tap = pointer placed + left click');
    await evalJs('setMode("direct")');
    await tap(120, 420);
    await sleep(600);
    await evalJs('setMode("trackpad")');
    // F8 tap ripple
    await touch('touchStart', [tp(200, 300)]); await sleep(40); await touch('touchEnd', []);
    await sleep(60);
    results.ripple = await evalJs('document.querySelectorAll(".rip").length');
    // F3 fit / 1:1 button and zoom lock
    await evalJs('document.getElementById("bZoom").click(), true'); await sleep(200);
    results.zoom11 = await evalJs('trackpad.zoom');
    await shot('ui-8-zoom-1to1.png');
    await evalJs('document.getElementById("bZoom").click(), true'); await sleep(200);
    results.zoomFit = await evalJs('trackpad.zoom');
    await evalJs('(() => { const s = document.getElementById("swLock"); s.checked = true; s.dispatchEvent(new Event("change")); return true; })()');
    await touch('touchStart', [tp(180, 450, 0), tp(210, 450, 1)]);
    for (let i = 1; i <= 10; i++) { await touch('touchMove', [tp(180 - i * 8, 450, 0), tp(210 + i * 8, 450, 1)]); await sleep(16); }
    await touch('touchEnd', []);
    results.zoomLocked = await evalJs('trackpad.zoom');
    await evalJs('(() => { const s = document.getElementById("swLock"); s.checked = false; s.dispatchEvent(new Event("change")); return true; })()');
    // F4 shortcuts: Alt+F4 asks first (cancel = nothing sent), Ctrl+A goes straight out
    await evalJs('document.getElementById("bKb").click(), true'); await sleep(200);
    await evalJs('document.querySelector(\'#keys button[data-c="Alt+F4"]\').click(), true'); await sleep(200);
    results.askShown = await evalJs('getComputedStyle(document.getElementById("ask")).display === "flex"');
    await shot('ui-9-altf4-confirm.png');
    await evalJs('document.getElementById("askNo").click(), true');
    await evalJs('document.querySelector(\'#keys button[data-c="Control+A"]\').click(), true');
    await evalJs('document.querySelector(\'#keys button[data-c="Meta+Shift+S"]\').click(), true');
    await shot('ui-10-keyboard-more.png');
    await evalJs('document.getElementById("kbClose").click(), true');
    await evalJs('(() => { const s = document.getElementById("sens"); s.value = 1.5; s.dispatchEvent(new Event("input")); return true; })()');
    results.sensStored = await evalJs('localStorage.getItem("pm.sens")');
    log('quality -> 0 (smooth): reconnects with a smaller stream');
    await evalJs('setQuality(0)');
    let small = false;
    for (let i = 0; i < 30 && !small; i++) { await sleep(500); small = await evalJs('S.state === "connected" && v.videoWidth > 0 && v.videoWidth <= 1920').catch(() => false); }
    results.quality0 = small;
    results.quality0Size = await evalJs('v.videoWidth + "x" + v.videoHeight');
    log('after quality 0:', small, results.quality0Size);
  }

  // ---- F7 clipboard (in-memory test clipboard on the PC), O7 pointer, O5 pause ----
  if (!kickAfter && !viewOnly && !special) {
    await evalJs('document.getElementById("bKb").click(), true'); await sleep(200);
    await evalJs('document.getElementById("bClip").click(), true'); await sleep(800);
    results.clipFromPc = await evalJs('document.getElementById("clipOut").value');
    await evalJs('(() => { document.getElementById("clipIn").value = "phone→pc 你好"; document.getElementById("clipSend").click(); return true; })()');
    await sleep(600);
    await shot('ui-11-clipboard.png');
    await evalJs('document.getElementById("clipClose").click(), true');
    results.cursorKnown = await evalJs('!!srvCur && typeof srvCur.x === "number"');
    await evalJs('send({t: "pause", on: true}), true'); await sleep(2500);
    await evalJs('send({t: "pause", on: false}), true'); await sleep(1500);
    results.playingAfterPause = await isPlaying();
    if (PM_help) results.helpStatus = await (async () => { try { return (await fetch(url + 'help')).status; } catch { return 0; } })();
  }
  if (idleTest) {
    for (let i = 0; i < 60 && !(await evalJs('S.bye || ""')); i++) await sleep(1000);
    results.bye = await evalJs('S.bye || ""');
    results.idleMsg = await evalJs('document.getElementById("msgT").textContent');
    await shot('ui-12-idle-bye.png');
  }
  if (simIce) {
    for (let i = 0; i < 60 && !(await evalJs('document.getElementById("msgT").textContent')).length; i++) await sleep(1000);
    for (let i = 0; i < 60; i++) {
      const tt = await evalJs('document.getElementById("msgT").textContent');
      if (tt && !/連線中|Connecting/.test(tt)) break;
      await sleep(1000);
    }
    results.iceMsg = await evalJs('document.getElementById("msgT").textContent');
    results.iceButtons = await evalJs('Array.from(document.querySelectorAll("#msgB button")).map(b => b.textContent).join("|")');
    await shot('ui-13-firewall-msg.png');
  }

  // ---- let statistics accumulate ----
  const tEnd = Date.now() + Math.max(5, seconds - 20) * 1000;
  while (Date.now() < tEnd) await sleep(1000);
  await shot('edge-2-later.png');
  const final = await evalJs(`(async () => { await pollStats(); return JSON.stringify({S, hud: hud.textContent, ua: navigator.userAgent}); })()`);
  fs.writeFileSync(path.join(outDir, 'page-state.json'), final);
  const st = JSON.parse(final);
  await sleep(5500);  // one more page report into the log

  if (kickAfter) {
    for (let i = 0; i < 40 && !(await evalJs('S.bye || ""')); i++) await sleep(500);
    results.bye = await evalJs('S.bye || ""');
    await sleep(3000);
    results.reconnectedAfterKick = await isPlaying();
    await shot('edge-3-kicked.png');
    log('kicked:', results.bye, 'reconnected by itself:', results.reconnectedAfterKick);
  } else {
    log('reload page (session replacement)');
    await cdp('Page.reload', {}, S);
    playing = false;
    for (let i = 0; i < 40 && !playing; i++) { await sleep(500); playing = await isPlaying(); }
    results.playingAfterReload = playing;
    results.prefsAfterReload = await evalJs('JSON.stringify({q: prefs.q, sens: prefs.sens, mode: prefs.mode})').catch(() => '');

    log('playing after reload:', playing);
    await sleep(2000);
  }

  // ---- security rules (from this PC) ----
  const status = async u => { try { return (await fetch(u, {method: 'GET'})).status; } catch { return 'refused'; } };
  const badToken = await status(url.replace(token, 'x'.repeat(token.length)));
  const noToken = await status(url.replace(token + '/', ''));
  const loopback = await status(`http://127.0.0.1:${port}/${token}/`);
  const manifest = await (async () => { try { return await (await fetch(url + 'manifest.webmanifest')).json(); } catch { return null; } })();
  log('security: wrong token ->', badToken, '| no token ->', noToken, '| 127.0.0.1 ->', loopback);
  if (lockoutTest) {
    // F5: 10 wrong links from this address -> shut out (even the right link) for 15 minutes.
    for (let i = 0; i < 10; i++) await status(url.replace(token, 'z'.repeat(token.length)));
    results.lockedRight = await status(url);
    log('lockout: right link after 10 wrong ones ->', results.lockedRight);
  }
  await sleep(300);

  // ---- checks against the spike log ----
  const L = fs.readFileSync(logFile, 'utf8');
  const aud = JSON.parse(results.audio || '{}').aud || {};
  const viaIp = url.match(/\/\/([\d.]+):/)[1];
  const checks = [
    ['wrong token -> 403', badToken === 403],
    ['no token -> 403', noToken === 403],
    ['not reachable on 127.0.0.1 (LAN/Tailscale bind only)', loopbackRun ? loopback !== 200 : loopback === 'refused'],
    ['403s logged with source IP', /\d+\.\d+\.\d+\.\d+ GET \/[^ ]* .*-> 403/.test(L)],
    [`page served via ${via} IP ${viaIp}`, new RegExp(`GET page via ${viaIp.replace(/\./g, '\\.')}`).test(L)],
    [loopbackRun ? 'Tailscale listener (n/a: --loopback)' : 'Tailscale listener up', loopbackRun || /listening on 100\.\d+\.\d+\.\d+:\d+ \(Tailscale peers only\)/.test(L)],
    ['video track open', /video track open/.test(L)],
    ['audio track open', /audio track open/.test(L)],
    ['DataChannel open', /DataChannel open/.test(L)],
    ['picture plays', results.playing === true],
    ['ACTIVE event', /event: ACTIVE/.test(L)],
    ['sound turned on from the page', /page [\d.]+: sound on/.test(L) && /PC sound -> phone ON/.test(L)],
    ['Opus packets sent (PC log)', /stats audio: [1-9]\d* Opus packets sent/.test(L)],
    ['Opus packets received (browser)', (aud.packets || 0) > 20],
    ['manifest standalone', manifest && manifest.display === 'standalone' && manifest.start_url === '/' + token + '/'],
    ['ICE pair logged', /selected pair local host/.test(L)],
  ];
  if (via === 'tailscale') checks.push(['session over Tailscale (RTP payload <= 1100)', /\(Tailscale\) ready in .*RTP payload 1100/.test(L)]);
  if (approval) {
    checks.push(['page waited for approval', results.pendingShown === 'pending']);
    checks.push(['approval asked in the PC log', /NEW DEVICE asks to connect/.test(L) && /approval: request #1/.test(L)]);
    if (denyFirst) {
      checks.push(['denied shown on the page', results.deniedShown === 'denied']);
      checks.push(['DENIED logged', /host: DENIED/.test(L)]);
    }
    checks.push(['ALLOWED logged', /host: ALLOWED/.test(L)]);
  }
  if (kickAfter) {
    checks.push(['kick: page told (bye)', results.bye === 'kicked']);
    checks.push(['kick: page does not reconnect by itself', results.reconnectedAfterKick === false]);
    checks.push(['kick: ENDED event', /event: ENDED .*kicked/.test(L)]);
  } else {
    checks.push(['new session plays after reload (same device, no new dialog)', results.playingAfterReload === true &&
      (L.match(/NEW DEVICE asks/g) || []).length <= (denyFirst ? 2 : 1)]);
  }
  checks.push(['sound call-to-action shown before use', results.ctaShown === true]);
  checks.push(['no clipped text in the panels (portrait)', Array.isArray(results.overflow) && results.overflow.length === 0]);
  checks.push(['no clipped text in settings (landscape)', Array.isArray(results.overflowLandscape) && results.overflowLandscape.length === 0]);
  checks.push(['fill zooms the picture to cover the screen', results.fillZoom > 1.001]);
  if (!kickAfter && !viewOnly && !special) {
    checks.push(['direct mode: pointer placed + click', /pointer placed x\d+/.test(L) && /left click/.test(L)]);
    checks.push(['pointer speed remembered', results.sensStored === '1.5']);
    checks.push(['F8 tap ripple shown', results.ripple > 0]);
    if (clipText) {
      checks.push(['F7 PC clipboard text shown on the phone', results.clipFromPc === clipText]);
      checks.push(['F7 phone text put on the PC clipboard', /clipboard set on the PC \(\d+ bytes\)$/m.test(L)]);
    }
    checks.push(['O7 PC pointer position reaches the page', results.cursorKnown === true]);
    checks.push(['O5 background pause and resume (IDR)', /in the background -> picture and sound paused/.test(L) && /back -> resuming/.test(L) && results.playingAfterPause === true]);
    checks.push(['O6 frame rate follows use', /frame rate \d+ -> 60 fps \(phone in use\)/.test(L) && /-> 30 fps \(phone not touched\)/.test(L)]);
    checks.push(['F1 PC kept awake while connected, released after', /keeping the PC awake/.test(L) && /PC may sleep again/.test(L)]);
    if (PM_help) checks.push(['F6 guide served to the phone', results.helpStatus === 200]);
    checks.push(['F3 1:1 zooms in, fit returns to 1', results.zoom11 > 1.4 && results.zoomFit === 1]);
    checks.push(['F3 zoom lock: pinch does not zoom', results.zoomLocked === 1]);
    checks.push(['F4 Alt+F4 asks first; cancel sends nothing', results.askShown === true && !L.includes('combo Alt+F4')]);
    checks.push(['F4 Ctrl+A / Win+Shift+S sent', L.includes('combo Control+A') && L.includes('combo Meta+Shift+S')]);

    checks.push(['quality switch -> smaller stream, no new dialog', results.quality0 === true && /stream 1920x\d+@60 6000 kbps/.test(L)]);
    checks.push(['settings remembered after reload', results.prefsAfterReload === JSON.stringify({q: 0, sens: 1.5, mode: 'trackpad'})]);
  }

  if (viewOnly) {
    checks.push(['view-only: no input reached the injector', !/input( \(dry-run\))?: (left|right|move|text|key)/.test(L)]);
  } else if (inject) {
    checks.push(['injected left click landed in the test window', /testwin: WM_LBUTTONDOWN/.test(L)]);
    checks.push(['injected text landed in the test window', /testwin: edit text now ".*hi 你好/.test(L)]);
    checks.push(['injected right click landed in the test window', /testwin: WM_RBUTTONDOWN/.test(L)]);
    checks.push(['injected wheel landed in the test window', /testwin: WM_MOUSEWHEEL/.test(L)]);
    checks.push(['moves outside the test window were dropped', /pointer move to .* not injected \(test guard/.test(L)]);
    checks.push(['no "SendInput failed"', !/SendInput.*failed/.test(L)]);
  } else {
    checks.push(['left click (dry-run)', /input \(dry-run\): left click/.test(L)]);
    checks.push(['right click (dry-run)', /input \(dry-run\): right click/.test(L)]);
    checks.push(['wheel (dry-run)', /input \(dry-run\): wheel x\d+/.test(L)]);
    checks.push(['text (dry-run)', /input \(dry-run\): text \(\d+ bytes\)/.test(L)]);
    checks.push(['no real injection', /dry-run \(events are only logged\)/.test(L)]);
  }
  if (idleTest) {
    checks.push(['F5 idle: page told (bye idle)', results.bye === 'idle' && /idle|閒置|操作/.test(results.idleMsg)]);
    checks.push(['F5 idle: ENDED event', /event: ENDED .*idle/.test(L)]);
  }
  if (lockoutTest) {
    checks.push(['F5 lockout: BLOCKED event', /event: BLOCKED/.test(L)]);
    checks.push(['F5 lockout: right link refused (429) while blocked', results.lockedRight === 429]);
  }
  if (simIce) {
    checks.push(['F6 media path blocked -> plain reason on the phone', /影像連線被擋住|picture connection is blocked/.test(results.iceMsg || '')]);
    checks.push(['F6 how-to button offered', /看排除方法|How to fix/.test(results.iceButtons || '')]);
    checks.push(['F6 reason reported to the PC', /event: TROUBLE .*firewall/.test(L)]);
  }
  if (simLoss) {
    checks.push(['O1 lowers the bitrate on loss', /adapt-down.*"to":"bitrate"/.test(L) && /encoder: bitrate -> \d+ kbps \(ok\)/.test(L)]);
    checks.push(['O1 then lowers the quality one step', /adapt-down.*"to":"quality"/.test(L)]);
  }
  if (special) {
    const keep = /^(wrong token|no token|not reachable|F5|F6 media|F6 how|F6 reason|O1)/;
    const keepVideo = /^(picture plays|video track open|DataChannel open)/;
    for (let i = checks.length - 1; i >= 0; i--)
      if (!keep.test(checks[i][0]) && !(keepVideo.test(checks[i][0]) && !simIce)) checks.splice(i, 1);
  }
  let ok = true;
  const lines = [];

  for (const [name, pass] of checks) { const l = (pass ? 'PASS ' : 'FAIL ') + name; log(l); lines.push(l); ok = ok && pass; }
  fs.writeFileSync(path.join(outDir, 'checks.txt'), lines.join('\n') + '\n' + (ok ? 'ALL PASS' : 'SOME FAILED') + '\n');
  await cleanup(ok ? 0 : 1);
} catch (e) {
  console.error('test failed:', e);
  await cleanup(1);
}
