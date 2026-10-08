// Live 傳到手機 page in headless Chrome (iPhone user agent, 390 px), driven
// together with the app (share/tools/app_share_batch_test.ps1 -Mode live):
//   node share/tools/live_page_test.mjs <url> <appPid> <dev_cmd.ps1> <outPrefix>
// navigator.share / canShare are stubbed (headless Chrome has no share
// sheet): the stub records the File objects 全部儲存 hands over. Steps:
//   1 the empty page (waiting card)            → <outPrefix>_p1-empty.png
//   2 app 截圖 → a card appears by itself       → <outPrefix>_p2-one.png
//   3 app 截圖 + a recording → 3 cards, 全部儲存 ready
//   4 tap 全部儲存 (real mouse events)         → share({files}) with 3 files, sizes = list
//                                              → <outPrefix>_p3-saved.png
// Prints one "ok"/"FAIL" line per check and exits 1 on a failure.
import { spawn, execFileSync } from 'node:child_process';
import { writeFileSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const [url, pid, devCmd, out] = process.argv.slice(2);
const UA = 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1';
const PORT = 9300 + Math.floor(Math.random() * 400);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failed = 0;
const check = (ok, what) => { if (!ok) failed++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}`); };
const dev = (cmds, wait = 900) => execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmd,
  '-ProcessId', String(pid), '-Cmd', cmds, '-Wait', String(wait)], { stdio: 'inherit' });

const proc = spawn('C:/Program Files/Google/Chrome/Application/chrome.exe', ['--headless=new', '--disable-gpu', '--hide-scrollbars',
  '--mute-audio', `--remote-debugging-port=${PORT}`, `--user-data-dir=${mkdtempSync(join(tmpdir(), 'pmlive-chrome-'))}`, 'about:blank'],
  { stdio: 'ignore' });
let list = [];
for (let i = 0; i < 100 && !list.length; i++) { try { list = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json(); } catch {} await sleep(100); }
const ws = new WebSocket(list.find((p) => p.type === 'page').webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener('open', r, { once: true }));
let id = 0; const wait = new Map();
ws.addEventListener('message', (e) => { const m = JSON.parse(e.data); if (m.id && wait.has(m.id)) { wait.get(m.id)(m); wait.delete(m.id); } });
const send = (method, params = {}) => new Promise((r) => { const i = ++id; wait.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const evaluate = async (expression) => (await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true })).result.result.value;
const shot = async (name) => {
  const [w, h] = await evaluate('[document.documentElement.scrollWidth, document.documentElement.scrollHeight]');
  const r = await send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true, clip: { x: 0, y: 0, width: w, height: h, scale: 1 } });
  writeFileSync(`${out}_${name}.png`, Buffer.from(r.result.data, 'base64'));
};
const until = async (expr, ms = 15000) => { const t = Date.now(); while (Date.now() - t < ms) { if (await evaluate(expr)) return true; await sleep(200); } return false; };

await send('Page.enable');
await send('Emulation.setUserAgentOverride', { userAgent: UA });
await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 1, mobile: true });
await send('Emulation.setTouchEmulationEnabled', { enabled: true });
// The share sheet stub (before the page's own script runs).
await send('Page.addScriptToEvaluateOnNewDocument', { source: `
  window.__shared = [];
  Object.defineProperty(Navigator.prototype, 'canShare', { configurable: true, value: function (d) { return !!(d && d.files && d.files.length); } });
  Object.defineProperty(Navigator.prototype, 'share', { configurable: true, value: function (d) {
    window.__shared.push({ active: navigator.userActivation ? navigator.userActivation.isActive : null,
      files: d.files.map(function (f) { return { name: f.name, size: f.size, type: f.type }; }) });
    return Promise.resolve(); } });` });
await send('Page.navigate', { url });
await sleep(2500);

const cards = 'document.querySelectorAll("#list .card[data-i]").length';
check(await evaluate(`${cards} === 0 && !document.getElementById('empty').hidden && !!document.getElementById('live')`),
  'empty live page: waiting card + 即時更新');
await shot('p1-empty');

dev('107');  // 截圖 in the app
check(await until(`${cards} === 1`), 'after a screenshot on the PC: 1 card on the open page (no reload)');
check(await evaluate(`document.getElementById('empty').hidden && document.querySelector('#list .card').classList.contains('new')`),
  'waiting card gone, the new card highlighted');
check(await until(`!document.getElementById('saveall').disabled`, 10000), '全部儲存 ready (file fetched ahead)');
await shot('p2-one');

dev('107', 1200);            // another screenshot
dev('112', 2500); dev('112', 600);  // a recording
check(await until(`${cards} === 3`, 20000), 'after a screenshot and a recording: 3 cards, newest first');
check(await evaluate(`document.querySelector('#list .card').getAttribute('data-video') === '1'`), 'the recording is the top card');
check(await until(`!document.getElementById('saveall').disabled`, 20000), '全部儲存 ready for 3');
const label = await evaluate(`document.getElementById('saveall').textContent`);
check(/3/.test(label), `button reads "${label}"`);
const listed = JSON.parse(await evaluate(`fetch('list').then(r => r.text())`));

// A real tap on 全部儲存 (user activation, as iOS requires).
const box = await evaluate(`(() => { const r = document.getElementById('saveall').getBoundingClientRect(); return [r.x + r.width / 2, r.y + r.height / 2]; })()`);
await send('Input.dispatchMouseEvent', { type: 'mousePressed', x: box[0], y: box[1], button: 'left', clickCount: 1 });
await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x: box[0], y: box[1], button: 'left', clickCount: 1 });
await sleep(800);
const shared = await evaluate('window.__shared');
check(shared.length === 1 && shared[0].files.length === 3, `navigator.share called once with ${shared[0]?.files.length} files`);
check(shared[0]?.active !== false, 'share() called inside the tap (user activation active)');
const want = listed.files.map((f) => `${f.name}:${f.size}:${f.type}`).sort().join('|');
const got = (shared[0]?.files || []).map((f) => `${f.name}:${f.size}:${f.type}`).sort().join('|');
check(want === got, `shared files = the page's files (names, sizes, types)\n      ${got}`);
check(await evaluate(`document.querySelectorAll('.card.saved').length === 3 && document.getElementById('saveall').disabled`),
  'all cards marked 已儲存, button 都已儲存');
await shot('p3-saved');

dev('107', 1500);  // a 4th: only it is offered next
check(await until(`${cards} === 4 && !document.getElementById('saveall').disabled`, 15000), 'a new screenshot after saving: 全部儲存（1）');
console.log('      button:', await evaluate(`document.getElementById('saveall').textContent`));
await shot('p4-new');

ws.close(); proc.kill();
console.log(failed ? `${failed} FAILED` : 'live page: all checks passed');
process.exit(failed ? 1 : 0);
