// 0.8.3.3 checks inside the app (node 22+). Every case: its own --dev --test-offscreen instance, data folder
// (PhoneMirror-dev-app0833) and TEMP; PM_REMOTE_LOOPBACK (127.0.0.1 / .2 / .3 only: no firewall prompt),
// PM_REMOTE_DRYRUN (input only logged); every window checked against every monitor. tailscale and
// cloudflared are never run: the fixed address is made up (PM_REMOTE_FUNNELFAKE) or comes from the FAKE
// CLI (pm_fake_tailscale.exe), its TLS check goes to a FAKE TLS server on 127.0.0.1 (self-signed, made
// here with openssl), the temporary address is made up (PM_REMOTE_TUNNELFAKE). Nothing is ever allowed
// on the PC (允許 is never pressed: no picture of the real desktop is sent anywhere).
//   node remote/tools/app0833_test.mjs <自在投影.exe> <pm_fake_tailscale.exe> <outDir> [--only probe,cli,giveup,login,tunnel]
// Cases:
//   probe   the made-up fixed address, the TLS server drops every connection for 8 s (as Tailscale does
//           while it issues the certificate): 正在準備網址 (no 已開啟 / "ready at") until it answers
//   cli     the same through the fake tailscale CLI (the real start path) and a 5 s drop
//   giveup  it never answers (PM_REMOTE_FUNNELPROBEMAX=6): the card fails with what to do
//   login   the phone page's two kinds of login: the owner's has no account field; "#u=guest-4821"
//           fills the account in (and never reaches the server or the log); a wrong password and an
//           unknown account get the same answer; the right password of a deleted account says so
//   tunnel  the temporary address: a phone remembered on this PC is still asked, without 記住
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import net from 'node:net';
import tls from 'node:tls';
import http from 'node:http';

const [exe, fakeCli, outDir, ...rest] = process.argv.slice(2);
if (!exe || !fakeCli || !outDir) { console.error('usage: node app0833_test.mjs <自在投影.exe> <pm_fake_tailscale.exe> <outDir> [--only a,b]'); process.exit(2); }
let only = ['probe', 'cli', 'giveup', 'login', 'tunnel'];
for (let i = 0; i < rest.length; i++) if (rest[i] === '--only') only = rest[++i].split(',');
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const devCmdPs1 = path.resolve(here, '..', '..', 'share', 'tools', 'dev_cmd.ps1');
const tmpDir = path.resolve(outDir, '_tmp');
const FUNNEL = 'https://example-pc.example.ts.net:8443/';
const C = {Toggle: 330, Tunnel: 352, TabRemote: 383, GuestNew: 440, GuestDel0: 450, Exit: 104};
const salt = crypto.randomBytes(16);
const pwHash = `pbkdf2-sha256$310000$${salt.toString('hex')}$${crypto.pbkdf2Sync('Zizai-Test-2026', salt, 310000, 32, 'sha256').toString('hex')}`;
const results = [];
let failed = 0;
const check = (name, ok, detail = '') => { results.push({name, ok: !!ok, detail}); if (!ok) failed++; console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail !== '' ? '  -- ' + detail : ''}`); };

function windowsOnScreen(pid) {
  const ps = `Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Collections.Generic;
public static class PmWin4 {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public static List<int[]> Of(uint pid) { var l = new List<int[]>(); EnumWindows((h, x) => { uint p; GetWindowThreadProcessId(h, out p);
    if (p == pid && IsWindowVisible(h)) { RECT r; GetWindowRect(h, out r); l.Add(new int[] { r.L, r.T, r.R, r.B }); } return true; }, IntPtr.Zero); return l; }
}
"@
$w = [PmWin4]::Of(${pid}); $on = 0
foreach ($r in $w) { foreach ($s in [System.Windows.Forms.Screen]::AllScreens) { $b = $s.Bounds; if ($r[0] -lt $b.Right -and $r[2] -gt $b.Left -and $r[1] -lt $b.Bottom -and $r[3] -gt $b.Top) { $on++ } } }
Write-Output "$($w.Count) $on"`;
  try { const [n, on] = execFileSync('powershell', ['-NoProfile', '-Command', ps], {encoding: 'utf8', windowsHide: true}).trim().split(/\s+/).map(Number); return {n, on}; }
  catch { return {n: -1, on: -1}; }
}

// The fake "Funnel" for the TLS check (127.0.0.1, self-signed): drops every connection until readyAt.
async function fakeTls() {
  const key = path.join(outDir, 'fake_key.pem'), cert = path.join(outDir, 'fake_cert.pem');
  if (!fs.existsSync(cert))
    execFileSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', cert, '-days', '2', '-subj', '/CN=example-pc.example.ts.net'], {stdio: 'ignore'});
  const ts = tls.createServer({key: fs.readFileSync(key), cert: fs.readFileSync(cert)}, s => {
    s.on('data', () => s.end('HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nnot found'));
    s.on('error', () => {});
  });
  const st = {readyAt: Infinity, dropped: 0, served: 0, firstServedAt: 0};
  const srv = net.createServer(sock => {
    sock.on('error', () => {});
    if (Date.now() < st.readyAt) { st.dropped++; sock.destroy(); return; }
    if (!st.served++) st.firstServedAt = Date.now();
    ts.emit('connection', sock);
  }).listen(0, '127.0.0.1');
  await new Promise(r => srv.on('listening', r));
  st.port = srv.address().port;
  st.close = () => srv.close();
  return st;
}

async function instance(tag, {seed = '', env = {}}, steps) {
  const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-app0833');
  fs.rmSync(dataDir, {recursive: true, force: true});
  fs.mkdirSync(dataDir, {recursive: true});
  fs.rmSync(tmpDir, {recursive: true, force: true});
  fs.mkdirSync(path.join(tmpDir, 'pmshots'), {recursive: true});
  const port = 18900 + Math.floor(Math.random() * 300);
  fs.writeFileSync(path.join(dataDir, 'settings.ini'),
    `[settings]\nlanguage=zh-TW\nremote_port=${port}\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\ntoolbar_hint_shown=1\nwhats_new_seen=0.8.3\nidle_tab=remote\n${seed}`);
  if (env.__devices) { fs.writeFileSync(path.join(dataDir, 'remote_devices.txt'), env.__devices); delete env.__devices; }
  const fullEnv = {...process.env, TEMP: tmpDir, TMP: tmpDir, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: 'app0833', PM_REMOTE_DRYRUN: '1', PM_REMOTE_NOTS: '1',
    PM_REMOTE_LOOPBACK: '1', PM_REMOTE_TESTCLIP: '1', ...env};
  for (const k of Object.keys(fullEnv)) if (fullEnv[k] === null) delete fullEnv[k];
  const app = spawn(exe, ['--dev', '--test-offscreen', '--test-no-network'], {stdio: 'ignore', env: fullEnv, windowsHide: true});
  const dev = (cmd, lp = 0, wait = 600) => { try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmdPs1, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe', windowsHide: true}); } catch {} };
  const readLog = () => { try { return fs.readFileSync(path.join(dataDir, 'phonemirror.log'), 'utf8'); } catch { return ''; } };
  const waitLog = async (re, ms) => { const end = Date.now() + ms; while (Date.now() < end) { const m = readLog().match(re); if (m) return m; await sleep(100); } return null; };
  const shot = name => {
    dev(910, 0, 1200);
    const f = path.join(tmpDir, 'pmshots', 'window.png');
    if (fs.existsSync(f)) { fs.copyFileSync(f, path.join(outDir, `${tag}_${name}.png`)); fs.rmSync(f); }
  };
  try {
    await sleep(3500);
    let w = windowsOnScreen(app.pid);
    check(`${tag}: no window on a monitor (start)`, w.on === 0, `${w.n} windows`);
    if (w.on !== 0) throw new Error('on a monitor');
    await steps({dev, readLog, waitLog, shot, dataDir, app});
    w = windowsOnScreen(app.pid);
    check(`${tag}: no window on a monitor (end)`, w.on === 0, `${w.n} windows`);
  } catch (e) { check(`${tag}: ran`, false, String(e && e.stack || e)); }
  dev(C.Exit, 0, 300);
  for (let i = 0; i < 150 && app.exitCode === null; i++) await sleep(100);
  if (app.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} check(`${tag}: quit`, false, 'killed'); }
  try { fs.copyFileSync(path.join(dataDir, 'phonemirror.log'), path.join(outDir, `${tag}.log`)); } catch {}
  return readLog();
}

// Headless Chrome (own profile, CDP on 127.0.0.1), the phone page on 127.0.0.2 only.
async function browser() {
  const port = 9300 + Math.floor(Math.random() * 400);
  const proc = spawn('C:/Program Files/Google/Chrome/Application/chrome.exe', ['--headless=new', '--disable-gpu', '--hide-scrollbars', '--no-first-run', '--mute-audio',
    `--user-data-dir=${tmpDir}/_chrome`, `--remote-debugging-port=${port}`, '--remote-debugging-address=127.0.0.1', 'about:blank'], {stdio: 'ignore'});
  let tabs;
  for (let i = 0; i < 60; i++) { try { tabs = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json(); if (tabs.length) break; } catch {} await sleep(250); }
  const ws = new WebSocket(tabs.find(t => t.type === 'page').webSocketDebuggerUrl);
  await new Promise(r => ws.addEventListener('open', r));
  let id = 0; const pending = new Map();
  ws.addEventListener('message', e => { const m = JSON.parse(e.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } });
  const send = (method, params = {}) => new Promise(r => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({id: i, method, params})); });
  await send('Page.enable');
  await send('Emulation.setDeviceMetricsOverride', {width: 412, height: 860, deviceScaleFactor: 2, mobile: true});
  return {
    go: async (url, wait = 2500) => { await send('Page.navigate', {url: 'about:blank'}); await sleep(200); await send('Page.navigate', {url}); await sleep(wait); },
    eval: async expr => { const r = await send('Runtime.evaluate', {expression: expr, awaitPromise: true, returnByValue: true}); return r.result && r.result.result && r.result.result.value; },
    shot: async out => { const r = await send('Page.captureScreenshot', {format: 'png'}); fs.writeFileSync(out, Buffer.from(r.result.data, 'base64')); },
    close: async () => { try { await send('Browser.close'); } catch {} await sleep(500); try { execFileSync('taskkill', ['/PID', String(proc.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} },
  };
}
const pwSeed = () => `remote_pw_hash=${pwHash}\nremote_fixed_secret=${crypto.randomBytes(32).toString('hex')}\n`;

if (only.includes('probe')) {
  const fk = await fakeTls();
  const log = await instance('probe', {seed: pwSeed() + 'remote_persistent=1\n', env: {PM_REMOTE_FUNNELFAKE: FUNNEL, PM_REMOTE_FUNNELPROBE: `https://127.0.0.1:${fk.port}/`}},
    async ({dev, waitLog, shot, readLog}) => {
      fk.readyAt = Date.now() + 8000;
      const t0 = Date.now();
      const known = await waitLog(/常駐外網 address \S+: checking that it answers/, 20000);
      check('probe: the address is checked before 已開啟', !!known);
      const prep = await waitLog(/not reachable yet after \d+ ms \((.*)\): 正在準備網址/, 10000);
      check('probe: 正在準備網址 while the connection is closed', !!prep, prep ? prep[1] : '');
      check('probe: no "ready at" (已開啟) while preparing', !/常駐外網 ready at/.test(readLog()));
      dev(C.TabRemote, 0, 1000); dev(985, 96, 1200); shot('1-preparing');
      const ok = await waitLog(/常駐外網 ready at (\S+) \(Tailscale Funnel; port 443 untouched\); 固定網址 reachable after (\d+) ms \((.*)\)/, 30000);
      check('probe: ready once it answers', !!ok, ok ? `${ok[2]} ms, ${ok[3]}` : '');
      check('probe: not before the server answered (>= 7 s)', ok && +ok[2] >= 7000 && fk.firstServedAt && fk.firstServedAt - t0 >= 7000, ok ? ok[2] : '');
      check('probe: the fake server dropped, then served', fk.dropped >= 1 && fk.served >= 1, `dropped ${fk.dropped}, served ${fk.served}`);
      await sleep(600); shot('2-ready');
    });
  fk.close();
}

if (only.includes('cli')) {
  const fk = await fakeTls();
  const modeFile = path.join(outDir, 'cli_mode.txt');
  fs.writeFileSync(modeFile, 'ok\n');
  await instance('cli', {seed: pwSeed() + 'remote_persistent=1\n', env: {PM_REMOTE_FAKECLI: fakeCli, PM_FAKE_TS_MODEFILE: modeFile, PM_FAKE_TS_PIDS: path.join(outDir, 'cli_pids.txt'),
    PM_FAKE_TS_STATE: path.join(outDir, 'cli_state.txt'), PM_REMOTE_FUNNELPROBE: `https://127.0.0.1:${fk.port}/`}},
  async ({waitLog, readLog}) => {
    fk.readyAt = Date.now() + 5000;
    const known = await waitLog(/常駐外網 address (\S+): checking/, 25000);
    check('cli: the fake CLI says the address is up, then it is checked', known && known[1] === FUNNEL, known ? known[1] : '');
    const ok = await waitLog(/常駐外網 ready at (\S+) .*reachable after (\d+) ms/, 30000);
    check('cli: ready only after the TLS check', ok && ok[1] === FUNNEL && /not reachable yet/.test(readLog()), ok ? ok[2] + ' ms' : '');
  });
  fk.close();
}

if (only.includes('giveup')) {
  const fk = await fakeTls();  // never ready
  await instance('giveup', {seed: pwSeed() + 'remote_persistent=1\n', env: {PM_REMOTE_FUNNELFAKE: FUNNEL, PM_REMOTE_FUNNELPROBE: `https://127.0.0.1:${fk.port}/`, PM_REMOTE_FUNNELPROBEMAX: '6'}},
    async ({dev, waitLog, shot, readLog}) => {
      const gave = await waitLog(/still not reachable after (\d+) ms \((.*)\): gave up/, 30000);
      check('giveup: gives up after the limit, says what to do', !!gave, gave ? gave[1] + ' ms' : '');
      check('giveup: never said 已開啟', !/常駐外網 ready at/.test(readLog()));
      dev(C.TabRemote, 0, 1000); dev(985, 96, 1200); shot('1-failed');
    });
  fk.close();
}

if (only.includes('login')) {
  const copyFile = path.join(tmpDir, 'copy.txt');
  const log = await instance('login', {seed: pwSeed() + 'remote_persistent=1\n', env: {PM_REMOTE_FUNNELFAKE: FUNNEL, PM_TEST_GUEST_NAME: 'guest-4821', PM_TEST_GUEST_OUT: copyFile}},
    async ({dev, waitLog, readLog}) => {
      await waitLog(/常駐外網 ready at/, 20000);
      const fixed = (readLog().match(/listening on (127\.0\.0\.2:\d+)/) || [])[1];
      dev(C.GuestNew, 0, 1000); dev(981, 2 | 16, 1500); dev(983, 0, 800);
      const copied = fs.existsSync(copyFile) ? fs.readFileSync(copyFile, 'utf8') : '';
      fs.rmSync(copyFile, {force: true});  // (it holds the test account's password)
      dev(984, 0, 600);
      const url = (copied.match(/https:\/\/\S+/) || [''])[0];
      const pw = (copied.match(/密碼：(\S+)/) || [])[1] || '';
      check('login: 複製全部 gives the address with the account after #', url === FUNNEL + '#u=guest-4821', url);
      const b = await browser();
      try {
        const state = `(() => { const v = id => { const e = document.getElementById(id); return !!e && getComputedStyle(e).display !== 'none'; };
          return {title: document.getElementById('passT').textContent, userIn: v('userIn'), userRow: v('userRow'), name: document.getElementById('userName').textContent,
                  link: document.getElementById('modeLnk').textContent, err: document.getElementById('passErr').textContent}; })()`;
        const login = async (password, wait = 2500) => b.eval(`(async () => { document.getElementById('passIn').value = ${JSON.stringify(password)};
          document.getElementById('passGo').click(); await new Promise(r => setTimeout(r, ${wait})); return document.getElementById('passErr').textContent; })()`);
        await b.go(`http://${fixed}/`);
        let s = await b.eval(state);
        check('login: owner mode has no account field', s && !s.userIn && !s.userRow, JSON.stringify(s));
        check('login: owner mode title 輸入你設定的密碼, link 用臨時帳號登入', s && s.title === '輸入你設定的密碼' && s.link === '用臨時帳號登入', s && s.title + ' / ' + s.link);
        await b.shot(path.join(outDir, 'login_1-owner.png'));
        await b.go(`http://${fixed}/#u=guest-4821`);
        s = await b.eval(state);
        check('login: #u= opens 臨時帳號 mode with the account filled in', s && s.userRow && !s.userIn && s.name === 'guest-4821' && s.title === '輸入對方給你的密碼', JSON.stringify(s));
        await b.shot(path.join(outDir, 'login_2-guest-prefilled.png'));
        const e1 = await login('WRONG1');
        await b.go(`http://${fixed}/#u=guest-0000`);
        const e2 = await login('WRONG2');
        const same = x => x.replace(/\d+/g, 'N');
        check('login: a wrong password and an unknown account get the same answer', e1 && same(e1) === same(e2) && /帳號或密碼不對/.test(e1), `${e1} | ${e2}`);
        await b.shot(path.join(outDir, 'login_3-wrong.png'));
        dev(C.GuestDel0, 0, 1200);  // 刪除 on the PC
        await b.go(`http://${fixed}/#u=guest-4821`);
        const e3 = pw ? await login(pw, 3000) : '';
        check('login: the right password of a deleted account is told so', /已經被刪除/.test(e3), e3);
        await b.shot(path.join(outDir, 'login_4-deleted.png'));
        await b.go(`http://${fixed}/#u=guest-0001`);
        const e4 = await login('ABC234', 3000);
        check('login: an unknown account with any password: still the same answer (no "deleted")', /帳號或密碼不對/.test(e4), e4);
      } finally { await b.close(); }
      const L = readLog();
      check('login: "#u=" never reaches the app or its log', !/#u=|[?&]u=guest|u=guest-4821/.test(L));
      check('login: no password in the log', pw && !L.includes(pw));
      const gets = (L.match(/http: [^\n]*GET page[^\n]*/g) || []).slice(0, 3);
      check('login: the server saw the page request without the fragment', gets.length > 0 && gets.every(x => !x.includes('guest-4821')), gets.join(' | '));
    });
}

if (only.includes('tunnel')) {
  const device = 'd0833seededphone0000000000000001';
  await instance('tunnel', {env: {PM_REMOTE_TUNNELFAKE: 'https://example-demo.example.com', PM_REMOTE_TESTPASS: '1', __devices: `${device}\tTest phone (seeded)\t2026-10-21\n`}},
    async ({dev, waitLog, readLog}) => {
      dev(C.Toggle, 0, 2500); dev(C.Tunnel, 0, 2000);
      const m = await waitLog(/test tunnel link \S+?(\/t\/\S+) passcode (\d+) \(local port (\d+)\)/, 10000);
      check('tunnel: the made-up temporary address is up (cloudflared not run)', !!m && /cloudflared not run/.test(readLog()));
      if (!m) return;
      const [, linkPath, code, port] = m;
      const req = (method, p, body, headers = {}) => new Promise(res => {
        const len = body ? Buffer.byteLength(body) : 0;  // (no chunked bodies: the server reads Content-Length)
        const r = http.request({host: '127.0.0.1', port: +port, method, path: p, headers: {'content-type': 'application/json', 'content-length': len, ...headers}}, rs => {
          let d = ''; rs.on('data', x => d += x); rs.on('end', () => res({status: rs.statusCode, headers: rs.headers, body: d}));
        });
        r.on('error', e => res({status: 0, body: String(e)}));
        if (body) r.write(body);
        r.end();
      });
      const r1 = await req('GET', linkPath);
      const base = r1.body.trim();
      check('tunnel: the one-time link leads to its own path', r1.status === 303 && /^\/c\/\w{32}\/$/.test(base), `${r1.status} ${base}`);
      const r2 = await req('POST', base + 'pass', JSON.stringify({code}));
      check('tunnel: the 通行碼 is accepted', r2.status === 200 && /"ok":true/.test(r2.body), r2.body.slice(0, 80));
      const r3 = await req('POST', base + 'session', JSON.stringify({device, q: 0, m: 0}), {'user-agent': 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) Mobile/15E148 Safari/604.1'});
      check('tunnel: a phone remembered on this PC still waits for 允許 (202 pending)', r3.status === 202 && /"pending":true/.test(r3.body), `${r3.status} ${r3.body.slice(0, 80)}`);
      const ask = await waitLog(/asks to connect \(asked every time: no 記住\)/, 5000);
      check('tunnel: the PC asks, with no 記住這支手機', !!ask);
      dev(960, 0, 1200);
      const f = path.join(tmpDir, 'pmshots', 'remote_ask.png');
      if (fs.existsSync(f)) fs.copyFileSync(f, path.join(outDir, 'tunnel_ask.png'));
      dev(961, 2, 1000);  // 拒絕
      check('tunnel: denied (nothing of the desktop was sent)', /denied/.test(readLog()));
    });
}

fs.writeFileSync(path.join(outDir, 'app0833.json'), JSON.stringify(results, null, 1));
try { fs.rmSync(tmpDir, {recursive: true, force: true}); } catch {}
console.log(failed ? `FAILED (${failed})` : `ALL PASSED (${results.length})`);
process.exit(failed ? 1 : 0);
