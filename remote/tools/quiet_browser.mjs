// 0.8.3.4 tests: a browser that never shows a window (no Alt+Tab entry, no taskbar button, never takes the
// focus), and a watcher that proves it. chrome-headless-shell (Playwright's copy) has no top-level window
// at all, unlike `chrome --headless=new`, whose page windows show up in Alt+Tab as blank
// "自在投影 遠端控制" windows. One instance per test, reused for every page, closed at the end.
//   PM_HEADLESS_SHELL=<path> overrides the shell; without one the test refuses to run (no fallback to a
//   browser with windows).
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

const HERE = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const sleep = ms => new Promise(r => setTimeout(r, ms));

export function headlessShell() {
  const c = [process.env.PM_HEADLESS_SHELL, path.join(process.env.LOCALAPPDATA || '', 'ms-playwright')].filter(Boolean);
  if (c[0] && c[0].endsWith('.exe') && fs.existsSync(c[0])) return c[0];
  const root = c[c.length - 1];
  if (fs.existsSync(root))
    for (const d of fs.readdirSync(root).filter(d => d.startsWith('chromium_headless_shell-')).sort().reverse()) {
      const exe = path.join(root, d, 'chrome-headless-shell-win64', 'chrome-headless-shell.exe');
      if (fs.existsSync(exe)) return exe;
    }
  throw new Error('chrome-headless-shell not found (set PM_HEADLESS_SHELL): not falling back to a browser with windows');
}

// Starts the shell on 127.0.0.1:<port> with its own profile; returns {proc, port, ver, close()}.
export async function launch(profileDir, extra = []) {
  const port = 9300 + Math.floor(Math.random() * 600);
  fs.mkdirSync(profileDir, {recursive: true});
  const proc = spawn(headlessShell(), [`--user-data-dir=${profileDir}`, `--remote-debugging-port=${port}`, '--remote-debugging-address=127.0.0.1',
    '--no-first-run', '--no-default-browser-check', '--mute-audio', '--disable-gpu', '--disable-extensions', ...extra, 'about:blank'],
    {stdio: 'ignore', windowsHide: true});
  let ver;
  for (let i = 0; i < 80 && !ver; i++) { await sleep(200); try { ver = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json(); } catch {} }
  if (!ver) { try { execFileSync('taskkill', ['/PID', String(proc.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} throw new Error('the headless shell did not start'); }
  const close = async () => {
    try { execFileSync('taskkill', ['/PID', String(proc.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
    for (let i = 0; i < 30 && proc.exitCode === null; i++) await sleep(100);
  };
  return {proc, port, ver, close};
}

// A minimal CDP client on the browser's websocket (flat sessions).
export async function cdpClient(wsUrl) {
  const ws = new WebSocket(wsUrl);
  await new Promise((r, j) => { ws.addEventListener('open', r); ws.addEventListener('error', j); });
  let nextId = 1;
  const pend = new Map();
  ws.addEventListener('message', e => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } });
  const raw = (method, params = {}, sessionId) => new Promise(r => { const i = nextId++; pend.set(i, r); ws.send(JSON.stringify({id: i, method, params, ...(sessionId ? {sessionId} : {})})); });
  const cdp = async (method, params, sessionId) => { const m = await raw(method, params, sessionId); return m.result || m.error; };
  return {cdp, raw, close: () => { try { ws.close(); } catch {} }};
}

// Watches the Alt+Tab / taskbar windows of these PIDs and their children (alttab_watch.ps1, read-only).
export function watchAltTab(pids, outFile) {
  for (const f of [outFile, outFile + '.stop', outFile + '.pids']) try { fs.rmSync(f, {force: true}); } catch {}
  const ps = spawn('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', path.join(HERE, 'alttab_watch.ps1'), '-Pids', pids.join(','), '-Out', outFile],
    {stdio: 'ignore', windowsHide: true});
  return {
    pid: ps.pid,
    add(pid) { fs.appendFileSync(outFile + '.pids', pid + '\n'); },
    async stop() {
      fs.writeFileSync(outFile + '.stop', '');
      for (let i = 0; i < 100 && ps.exitCode === null; i++) await sleep(100);
      if (ps.exitCode === null) try { execFileSync('taskkill', ['/PID', String(ps.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {}
      const lines = fs.existsSync(outFile) ? fs.readFileSync(outFile, 'utf8').split(/\r?\n/).filter(Boolean) : [];
      const samples = lines.filter(l => /^\d+ \d+/.test(l));
      const max = samples.reduce((m, l) => Math.max(m, +l.split(' ')[1]), 0);
      const seen = [...new Set(samples.filter(l => +l.split(' ')[1] > 0).map(l => l.split(' ').slice(2).join(' ')))];
      return {samples: samples.length, max, seen};
    },
  };
}
