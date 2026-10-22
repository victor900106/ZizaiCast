// 0.8.3.5 end-to-end tests against the RELEASED app binary (自在投影.exe, same SHA-256 as the one installed),
// run as its own --dev --test-offscreen instance (node 22+). Shared helpers for s1..s4.
//
// Safety (the owner's PC):
//  - own data folder (PM_DEV_INSTANCE=e2e0835-<tag>), own mutex, own random port; the owner's program and its
//    processes are never touched. Only PIDs this script started are ever ended.
//  - --test-no-network: no AirPlay / Miracast / adb (nothing on the LAN, the owner's adb server untouched).
//  - PM_REMOTE_LOOPBACK=1 (127.0.0.2 / .3 only, no firewall prompt), PM_REMOTE_NOTS=1, PM_REMOTE_FUNNELFAKE
//    (a made-up fixed address: a --dev instance never runs the tailscale CLI; 8443 is never touched).
//  - PM_REMOTE_INPUTSTUB=1: the whole injection path runs, SendInput replaced by a stub that only logs.
//  - PM_REMOTE_TESTCLIP=1: never the real clipboard. PM_REMOTE_SYNTHAUDIO=quiet: no playback device is
//    captured (synthetic PCM); the browser is --mute-audio. --test-offscreen: no MessageBeep, every window
//    off-screen + WS_EX_TOOLWINDOW. ONE chrome-headless-shell (no window at all), closed at the end.
//  - A watcher counts Alt+Tab / taskbar windows of every process started here (must stay 0).
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import {launch, cdpClient, watchAltTab} from '../quiet_browser.mjs';

export const HERE = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
export const REPO = path.resolve(HERE, '..', '..', '..');
export const sleep = ms => new Promise(r => setTimeout(r, ms));
const devCmdPs1 = path.join(REPO, 'share', 'tools', 'dev_cmd.ps1');
export const OWNER_PW = 'Zizai-E2E-2026';

export function results(outDir) {
  fs.mkdirSync(outDir, {recursive: true});
  const list = [];
  const check = (name, ok, detail = '') => {
    list.push({name, ok: !!ok, detail: typeof detail === 'string' ? detail : JSON.stringify(detail)});
    console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail !== '' ? '  -- ' + (typeof detail === 'string' ? detail : JSON.stringify(detail)) : ''}`);
    return !!ok;
  };
  const save = (file, extra = {}) => fs.writeFileSync(path.join(outDir, file), JSON.stringify({checks: list, ...extra}, null, 1));
  return {check, list, save, failed: () => list.filter(c => !c.ok).length};
}

// Starts the app. opts: {tag, guestsFile (text for remote_guests.txt), env, extraIni}
export async function startApp(exe, outDir, opts = {}) {
  const tag = opts.tag || 'x';
  const inst = `e2e0835-${tag}`;
  const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-' + inst);
  if (!opts.keepData) {
    fs.rmSync(dataDir, {recursive: true, force: true});
    fs.mkdirSync(dataDir, {recursive: true});
  }
  const tmpDir = path.join(outDir, '_tmp_' + tag);
  fs.rmSync(tmpDir, {recursive: true, force: true});
  fs.mkdirSync(path.join(tmpDir, 'pmshots'), {recursive: true});
  const port = opts.port || 19900 + Math.floor(Math.random() * 400);
  if (!opts.keepData) {
    const salt = crypto.randomBytes(16);
    const pwHash = `pbkdf2-sha256$310000$${salt.toString('hex')}$${crypto.pbkdf2Sync(OWNER_PW, salt, 310000, 32, 'sha256').toString('hex')}`;
    fs.writeFileSync(path.join(dataDir, 'settings.ini'),
      `[settings]\nlanguage=zh-TW\nremote_port=${port}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\ntoolbar_hint_shown=1\n` +
      `whats_new_seen=0.8.3.5\nidle_tab=remote\nremote_persistent=1\nremote_pw_hash=${pwHash}\nremote_fixed_secret=${crypto.randomBytes(32).toString('hex')}\n` +
      `remote_lock_outside=0\nremote_audio=1\nremote_mute_pc=0\n` + (opts.extraIni || ''));
    if (opts.guestsFile) fs.writeFileSync(path.join(dataDir, 'remote_guests.txt'), opts.guestsFile);
  } else if (opts.guestsFile !== undefined) {
    fs.writeFileSync(path.join(dataDir, 'remote_guests.txt'), opts.guestsFile);
  }
  const uploadDir = path.join(outDir, 'uploads_' + tag);
  fs.mkdirSync(uploadDir, {recursive: true});
  const copyFile = path.join(tmpDir, 'guest_copy.txt');
  const env = {...process.env, TEMP: tmpDir, TMP: tmpDir, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: inst,
    PM_REMOTE_INPUTSTUB: '1', PM_REMOTE_TESTCLIP: '1', PM_REMOTE_NOTS: '1', PM_REMOTE_LOOPBACK: '1', PM_REMOTE_SYNTHAUDIO: 'quiet',
    PM_REMOTE_FUNNELFAKE: 'https://example-pc.example.ts.net:8443/', PM_REMOTE_UPLOADDIR: uploadDir, PM_TEST_GUEST_OUT: copyFile, ...(opts.env || {})};
  delete env.PM_REMOTE_DRYRUN;  // dry-run would skip the stubbed path; the stub never calls SendInput either
  const app = spawn(exe, ['--dev', '--test-offscreen', '--test-no-network'], {stdio: 'ignore', env, windowsHide: true});
  const logPath = path.join(dataDir, 'phonemirror.log');
  // only this run's lines (a kept data folder keeps the earlier run's log)
  const logStart = (() => { try { return fs.readFileSync(logPath, 'utf8').length; } catch { return 0; } })();
  const readLog = () => { try { return fs.readFileSync(logPath, 'utf8').slice(logStart); } catch { return ''; } };
  const count = re => (readLog().match(new RegExp(re.source, 'g')) || []).length;
  const waitLog = async (re, ms) => { const end = Date.now() + ms; while (Date.now() < end) { const m = readLog().match(re); if (m) return m; await sleep(150); } return null; };
  const dev = (cmd, lp = 0, wait = 400) => { try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmdPs1, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe', windowsHide: true}); } catch {} };
  const on = await waitLog(/remote: on at (http:\/\/[\d.]+:\d+\/\w+\/)/, 30000);
  const lanUrl = on ? on[1] : '';
  const fixedReady = await waitLog(/常駐外網 ready at/, 20000);
  const fixedOrigin = `http://127.0.0.2:${(lanUrl.match(/:(\d+)\//) || [])[1] || port}`;
  // 允許 dialogs: answered by the script (DevCommand 961: 1 allow, 2 deny, 3 allow + remember)
  const asks = () => count(/\[info\] remote: [^\n]* asks to connect/);  // one line per 允許 dialog
  let answered = 0;
  const answer = async (how = 1, ms = 15000) => {
    const end = Date.now() + ms;
    while (Date.now() < end) {
      if (asks() > answered) { await sleep(500); dev(961, how, 500); answered = asks(); return true; }
      await sleep(150);
    }
    return false;
  };
  const syncAnswered = () => { answered = asks(); };
  // 新增臨時帳號 through the window: lp = life (0 用一次, 1 1 小時, 2 24 小時, 3 7 天) | 4 僅觀看 | 8 傳檔 | 16 建立
  const makeGuest = async (lp, shotName) => {
    try { fs.rmSync(copyFile); } catch {}
    dev(440, 0, 900);
    if (shotName) { dev(982, 0, 900); const f = path.join(tmpDir, 'pmshots', 'guest_panel.png'); if (fs.existsSync(f)) fs.copyFileSync(f, path.join(outDir, shotName)); }
    dev(981, lp | 16, 1300); dev(983, 0, 500); dev(984, 0, 500);
    const t = fs.existsSync(copyFile) ? fs.readFileSync(copyFile, 'utf8') : '';
    // 「帳號：guest-4231（打開網址就已填好）」: the name ends at the note
    return {name: (t.match(/帳號：([^\s（(]+)/) || [])[1], pw: (t.match(/密碼：([^\s（(]+)/) || [])[1], text: t};
  };
  const stop = async () => {
    dev(104, 0, 300);  // Exit
    for (let i = 0; i < 150 && app.exitCode === null; i++) await sleep(100);
    let killed = false;
    if (app.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} killed = true; }
    await sleep(500);
    let n = 1;
    while (fs.existsSync(path.join(outDir, `app_${tag}${n > 1 ? '_' + n : ''}.log`)) && opts.keepData) n++;
    try { fs.copyFileSync(logPath, path.join(outDir, `app_${tag}${n > 1 ? '_' + n : ''}.log`)); } catch {}
    try { fs.rmSync(tmpDir, {recursive: true, force: true}); } catch {}
    return {exitCode: app.exitCode, killed};
  };
  return {app, pid: app.pid, dataDir, uploadDir, lanUrl, fixedOrigin, fixedReady: !!fixedReady, readLog, count, waitLog, dev, asks, answer, syncAnswered, makeGuest, stop, tmpDir};
}

// One headless shell for the whole script; pages are tabs of it.
export async function startBrowser(outDir, tag) {
  const profileDir = path.join(os.tmpdir(), 'shell-e2e0835-' + tag + '-' + crypto.randomBytes(3).toString('hex'));
  const br = await launch(profileDir, ['--allow-loopback-in-peer-connection', '--autoplay-policy=no-user-gesture-required']);
  const watch = watchAltTab([br.proc.pid, process.pid], path.join(outDir, `alttab_${tag}.txt`));
  const C = await cdpClient(br.ver.webSocketDebuggerUrl);
  const cdp = C.cdp;
  const listeners = [];
  // event fan-out (downloads)
  const close = async () => { C.close(); await br.close(); try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {} return watch.stop(); };
  async function tab({w = 1280, h = 720, dpr = 1, mobile = false, touch = false} = {}) {
    const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
    const {sessionId} = await cdp('Target.attachToTarget', {targetId, flatten: true});
    const s = (m, p) => cdp(m, p, sessionId);
    await s('Emulation.setDeviceMetricsOverride', {width: w, height: h, deviceScaleFactor: dpr, mobile});
    if (touch) { await s('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5}); await s('Emulation.setEmitTouchEventsForMouse', {enabled: false}); }
    await s('Page.enable', {});
    // the real clipboard is never touched by the page
    await s('Page.addScriptToEvaluateOnNewDocument', {source: `(() => {
      window.__wrote = [];
      try { Object.defineProperty(navigator, 'clipboard', {configurable: true, value: {writeText: async s => { window.__wrote.push(s); }, readText: async () => '', write: async () => {}, read: async () => []}}); } catch {}
    })();`});
    const ev = async (expression, awaitPromise = false) => { const r = await s('Runtime.evaluate', {expression, returnByValue: true, awaitPromise}); return r && r.result ? r.result.value : undefined; };
    const waitFor = async (fn, ms) => { for (let t = 0; t < ms; t += 150) { let v; try { v = await fn(); } catch { v = null; } if (v) return v; await sleep(150); } return null; };
    const nav = url => s('Page.navigate', {url});
    // Screenshot with the PC picture covered by a made-up desktop (never this PC's real screen).
    const shot = async file => {
      await ev(`(() => { if (typeof videoBox !== 'function') return 0; const b = videoBox(); let m = document.getElementById('__mock'); if (!m) { m = document.createElement('div'); m.id = '__mock'; document.body.appendChild(m); }
        const z = trackpad.zoom, cx = b.r.left + b.r.width / 2 + trackpad.panX, cy = b.r.top + b.r.height / 2 + trackpad.panY;
        Object.assign(m.style, {position: 'absolute', zIndex: 1, pointerEvents: 'none', left: (cx - b.w * z / 2) + 'px', top: (cy - b.h * z / 2) + 'px', width: b.w * z + 'px', height: b.h * z + 'px',
          background: 'linear-gradient(135deg,#3a2a30,#5a3f46 45%,#2c2226)', boxShadow: 'inset 0 -' + Math.max(8, b.h * z * 0.035) + 'px 0 #241a1d'});
        m.innerHTML = '<div style="position:absolute;left:8%;top:12%;width:46%;height:56%;background:#f6e9ea;border-radius:6px"><div style="height:12%;background:#e9d3d6;border-radius:6px 6px 0 0"></div></div><div style="position:absolute;left:60%;top:22%;width:30%;height:44%;background:#fbf4f4;border-radius:6px;opacity:.92"></div>';
        $('v').style.visibility = 'hidden'; if ($('scr')) $('scr').style.visibility = 'hidden'; return 1; })()`);
      await sleep(200);
      const r = await s('Page.captureScreenshot', {format: 'png'});
      await ev(`(() => { const m = document.getElementById('__mock'); if (m) m.remove(); if (typeof $ === 'function' && $('v')) { $('v').style.visibility = ''; if ($('scr')) $('scr').style.visibility = ''; } return 1; })()`);
      if (r && r.data) fs.writeFileSync(file, Buffer.from(r.data, 'base64'));
      return !!(r && r.data);
    };
    const closeTab = () => cdp('Target.closeTarget', {targetId});
    return {targetId, sessionId, s, ev, waitFor, nav, shot, close: closeTab};
  }
  return {cdp, tab, close, watch, pid: br.proc.pid};
}

// The page's state: connected with a picture and an open data channel.
export const connectedJs = `(typeof S === 'object' && window.__pmFirstFrame && typeof dc !== 'undefined' && dc && dc.readyState === 'open' && pc && pc.connectionState === 'connected') ? 1 : 0`;

export function sha256(file) { return crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex'); }

// Lists processes whose parent chain starts at one of these PIDs (for the "nothing left" check).
export function leftovers(names) {
  try {
    const out = execFileSync('powershell', ['-NoProfile', '-Command',
      `Get-CimInstance Win32_Process | ? { $_.Name -match '${names}' } | % { "$($_.ProcessId) $($_.Name) $($_.ExecutablePath)" }`], {encoding: 'utf8', windowsHide: true});
    return out.split(/\r?\n/).filter(Boolean);
  } catch { return []; }
}
