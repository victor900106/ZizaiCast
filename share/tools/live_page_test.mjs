// Live 傳到手機 page in headless Chrome (iPhone user agent, 390 px), driven
// together with the app (share/tools/app_share_batch_test.ps1 -Mode live):
//   node share/tools/live_page_test.mjs <url> <appPid> <dev_cmd.ps1> <outPrefix>
// Two tabs on the same live page:
//  * "share" tab, http://127.0.0.1:… (a secure context): navigator.share /
//    canShare are stubbed (headless Chrome has no share sheet); the stub
//    records the File objects 全部儲存 hands over.
//  * "http" tab, http://zizai.test:… (mapped to 127.0.0.1 by
//    --host-resolver-rules: a plain http origin like the phone's
//    http://192.168.x.x, NOT a secure context): navigator.share is really
//    undefined, as in iPhone Safari on the LAN page. Nothing is stubbed.
// Steps:
//   1 the empty page (waiting card)            → <outPrefix>_p1-empty.png
//   2 app 截圖 → a card appears by itself       → <outPrefix>_p2-one.png
//   3 app 截圖 + a recording → 3 cards, 全部儲存 ready
//   4 tap 全部儲存 (real mouse events)         → share({files}) with 3 files, sizes = list
//                                              → <outPrefix>_p3-saved.png
//   5 http tab: no Web Share → 全部下載（ZIP，3 個） + the Files → Photos hint;
//     a real tap downloads the ZIP (Browser.setDownloadBehavior, temp dir),
//     unzipped here: 3 entries = the server's files (names, sizes, bytes, CRC)
//                                              → <outPrefix>_p5-http-zip.png
//   6 another 截圖: share tab 全部儲存（1）; http tab 下載新的 1 個（ZIP） → zip?from=3
//     → 1 entry                                → <outPrefix>_p4-new.png, _p6-http-new.png
// Prints one "ok"/"FAIL" line per check and exits 1 on a failure.
import { spawn, execFileSync } from 'node:child_process';
import { writeFileSync, mkdtempSync, readdirSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { crc32 } from 'node:zlib';

const [url, pid, devCmd, out] = process.argv.slice(2);
const UA = 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1';
const PORT = 9300 + Math.floor(Math.random() * 400);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failed = 0;
const check = (ok, what) => { if (!ok) failed++; console.log(`${ok ? 'ok  ' : 'FAIL'}  ${what}`); };
const dev = (cmds, wait = 900) => execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmd,
  '-ProcessId', String(pid), '-Cmd', cmds, '-Wait', String(wait)], { stdio: 'inherit' });
const httpUrl = url.replace('://127.0.0.1:', '://zizai.test:');
const downloads = mkdtempSync(join(tmpdir(), 'pmlive-dl-'));

const proc = spawn('C:/Program Files/Google/Chrome/Application/chrome.exe', ['--headless=new', '--disable-gpu', '--hide-scrollbars',
  '--mute-audio', '--host-resolver-rules=MAP zizai.test 127.0.0.1', '--disable-features=HttpsUpgrades',
  `--remote-debugging-port=${PORT}`, `--user-data-dir=${mkdtempSync(join(tmpdir(), 'pmlive-chrome-'))}`, 'about:blank'],
  { stdio: 'ignore' });
let list = [];
for (let i = 0; i < 100 && !list.length; i++) { try { list = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json(); } catch {} await sleep(100); }

// One DevTools connection (a tab or the browser).
async function connect(wsUrl) {
  const ws = new WebSocket(wsUrl);
  await new Promise((r) => ws.addEventListener('open', r, { once: true }));
  let id = 0; const wait = new Map(), events = [];
  ws.addEventListener('message', (e) => {
    const m = JSON.parse(e.data);
    if (m.id && wait.has(m.id)) { wait.get(m.id)(m); wait.delete(m.id); } else if (m.method) events.push(m);
  });
  const send = (method, params = {}) => new Promise((r) => { const i = ++id; wait.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
  const evaluate = async (expression) => (await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true })).result.result.value;
  const shot = async (name) => {
    const [w, h] = await evaluate('[document.documentElement.scrollWidth, document.documentElement.scrollHeight]');
    const r = await send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true, clip: { x: 0, y: 0, width: w, height: h, scale: 1 } });
    writeFileSync(`${out}_${name}.png`, Buffer.from(r.result.data, 'base64'));
  };
  const until = async (expr, ms = 15000) => { const t = Date.now(); while (Date.now() - t < ms) { if (await evaluate(expr)) return true; await sleep(200); } return false; };
  const tap = async (sel) => {  // real mouse events in the middle of the element (user activation)
    const box = await evaluate(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); e.scrollIntoView({ block: 'center' });
      const r = e.getBoundingClientRect(); return [r.x + r.width / 2, r.y + r.height / 2]; })()`);
    await send('Input.dispatchMouseEvent', { type: 'mousePressed', x: box[0], y: box[1], button: 'left', clickCount: 1 });
    await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x: box[0], y: box[1], button: 'left', clickCount: 1 });
  };
  const phone = async () => {
    await send('Page.enable');
    await send('Emulation.setUserAgentOverride', { userAgent: UA });
    await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 1, mobile: true });
    await send('Emulation.setTouchEmulationEnabled', { enabled: true });
  };
  return { ws, send, evaluate, shot, until, tap, phone, events };
}

// A stored ZIP read the way unzippers do: end record → central directory → local headers.
function unzip(z) {
  const e = z.length - 22;
  if (e < 0 || z.readUInt32LE(e) !== 0x06054b50) throw new Error('no end record');
  const n = z.readUInt16LE(e + 10), cdSize = z.readUInt32LE(e + 12), cdOff = z.readUInt32LE(e + 16);
  if (cdOff + cdSize !== e) throw new Error('central directory not where the end record says');
  const files = [];
  for (let i = 0, p = cdOff; i < n; i++) {
    if (z.readUInt32LE(p) !== 0x02014b50) throw new Error('bad central record');
    const flags = z.readUInt16LE(p + 8), method = z.readUInt16LE(p + 10), crc = z.readUInt32LE(p + 16);
    const csz = z.readUInt32LE(p + 20), usz = z.readUInt32LE(p + 24), nl = z.readUInt16LE(p + 28);
    const xl = z.readUInt16LE(p + 30), cl = z.readUInt16LE(p + 32), lo = z.readUInt32LE(p + 42);
    const name = z.subarray(p + 46, p + 46 + nl).toString('utf8');
    p += 46 + nl + xl + cl;
    if (z.readUInt32LE(lo) !== 0x04034b50) throw new Error('bad local header');
    const start = lo + 30 + z.readUInt16LE(lo + 26) + z.readUInt16LE(lo + 28);
    const data = z.subarray(start, start + csz);
    files.push({ name, flags, method, size: usz, data, crcOk: method === 0 && csz === usz && crc32(data) === crc });
  }
  return files;
}
const bytesOf = async (i) => Buffer.from(await (await fetch(`${url}v/${i}`)).arrayBuffer());
// Every file of the ZIP equals the server's file (list entry → bytes from v/<i>).
async function sameAsServer(files, entries) {
  if (files.length !== entries.length) return false;
  for (let k = 0; k < entries.length; k++) {
    const f = files[k], e = entries[k];
    if (f.name !== e.name || f.size !== e.size || !f.crcOk || !(f.flags & 0x800) || !f.data.equals(await bytesOf(e.i))) return false;
  }
  return true;
}

const browser = await connect((await (await fetch(`http://127.0.0.1:${PORT}/json/version`)).json()).webSocketDebuggerUrl);
await browser.send('Browser.setDownloadBehavior', { behavior: 'allow', downloadPath: downloads, eventsEnabled: true });

const A = await connect(list.find((p) => p.type === 'page').webSocketDebuggerUrl);
await A.phone();
// The share sheet stub (before the page's own script runs).
await A.send('Page.addScriptToEvaluateOnNewDocument', { source: `
  window.__shared = [];
  Object.defineProperty(Navigator.prototype, 'canShare', { configurable: true, value: function (d) { return !!(d && d.files && d.files.length); } });
  Object.defineProperty(Navigator.prototype, 'share', { configurable: true, value: function (d) {
    window.__shared.push({ active: navigator.userActivation ? navigator.userActivation.isActive : null,
      files: d.files.map(function (f) { return { name: f.name, size: f.size, type: f.type }; }) });
    return Promise.resolve(); } });` });
await A.send('Page.navigate', { url });
// The http tab: a new target, nothing stubbed.
const target = await (await fetch(`http://127.0.0.1:${PORT}/json/new?about:blank`, { method: 'PUT' })).json();
const B = await connect(target.webSocketDebuggerUrl);
await B.phone();
await B.send('Page.navigate', { url: httpUrl });
await sleep(2500);

const cards = 'document.querySelectorAll("#list .card[data-i]").length';
check(await A.evaluate(`${cards} === 0 && !document.getElementById('empty').hidden && !!document.getElementById('live')`),
  'empty live page: waiting card + 即時更新');
check(await B.evaluate(`location.hostname === 'zizai.test' && window.isSecureContext === false && navigator.share === undefined`),
  'http tab: zizai.test, not a secure context, navigator.share undefined (as iPhone Safari on http://192.168.x.x)');
check(await B.evaluate(`${cards} === 0 && document.getElementById('bar').hidden && document.getElementById('ziphint').hidden`),
  'http tab, empty: no download button yet');
await A.shot('p1-empty');

dev('107');  // 截圖 in the app
check(await A.until(`${cards} === 1`), 'after a screenshot on the PC: 1 card on the open page (no reload)');
check(await A.evaluate(`document.getElementById('empty').hidden && document.querySelector('#list .card').classList.contains('new')`),
  'waiting card gone, the new card highlighted');
check(await A.until(`!document.getElementById('saveall').disabled`, 10000), '全部儲存 ready (file fetched ahead)');
check(await A.evaluate(`document.getElementById('zipall').hidden && !document.getElementById('saveall').hidden`),
  'share tab: 全部儲存 shown, the ZIP button hidden');
await A.shot('p2-one');

dev('107', 1200);            // another screenshot
dev('112', 2500); dev('112', 600);  // a recording
check(await A.until(`${cards} === 3`, 20000), 'after a screenshot and a recording: 3 cards, newest first');
check(await A.evaluate(`document.querySelector('#list .card').getAttribute('data-video') === '1'`), 'the recording is the top card');
check(await A.until(`!document.getElementById('saveall').disabled`, 20000), '全部儲存 ready for 3');
const label = await A.evaluate(`document.getElementById('saveall').textContent`);
check(/3/.test(label), `button reads "${label}"`);
const listed = JSON.parse(await A.evaluate(`fetch('list').then(r => r.text())`));

// A real tap on 全部儲存 (user activation, as iOS requires).
await A.tap('#saveall');
await sleep(800);
const shared = await A.evaluate('window.__shared');
check(shared.length === 1 && shared[0].files.length === 3, `navigator.share called once with ${shared[0]?.files.length} files`);
check(shared[0]?.active !== false, 'share() called inside the tap (user activation active)');
const want = listed.files.map((f) => `${f.name}:${f.size}:${f.type}`).sort().join('|');
const got = (shared[0]?.files || []).map((f) => `${f.name}:${f.size}:${f.type}`).sort().join('|');
check(want === got, `shared files = the page's files (names, sizes, types)\n      ${got}`);
check(await A.evaluate(`document.querySelectorAll('.card.saved').length === 3 && document.getElementById('saveall').disabled`),
  'all cards marked 已儲存, button 都已儲存');
await A.shot('p3-saved');

// ---- the http tab: no Web Share → 全部下載（ZIP）
check(await B.until(`${cards} === 3`, 10000), 'http tab: the same 3 cards arrived by themselves');
const zipState = await B.evaluate(`(() => { const a = document.getElementById('zipall'), h = document.getElementById('ziphint');
  const vis = (e) => !!e && !e.hidden && e.getBoundingClientRect().height > 0;
  return { bar: vis(document.getElementById('bar')), zip: vis(a), hint: vis(h), share: vis(document.getElementById('saveall')),
    label: a.textContent, href: a.getAttribute('href'), download: a.hasAttribute('download'), hintText: h.textContent }; })()`);
check(zipState.bar && zipState.zip && !zipState.share && zipState.download && zipState.href === 'zip',
  `http tab: 「${zipState.label}」 visible (href zip, download), 全部儲存 not shown`);
check(/3/.test(zipState.label), 'ZIP button counts 3 files');
check(zipState.hint && /照片|Photos/.test(zipState.hintText) && /檔案|Files/.test(zipState.hintText),
  `http tab: the Files → Photos sentence is shown\n      ${zipState.hintText.slice(0, 90)}…`);
await B.shot('p5-http-zip');
await B.tap('#zipall');
let zipFile = '';
for (let i = 0; i < 100 && !zipFile; i++) {
  await sleep(200);
  zipFile = readdirSync(downloads).find((f) => f.endsWith('.zip')) || '';
}
check(/^ZizaiCast_\d{8}_\d{6}\.zip$/.test(zipFile), `a tap on the ZIP button downloads ${zipFile || '(nothing)'}`);
if (zipFile) {
  let entries = [];
  try { entries = unzip(readFileSync(join(downloads, zipFile))); } catch (e) { console.log('      unzip:', e.message); }
  check(entries.length === 3, `unzipped (node): ${entries.length} files`);
  check(await sameAsServer(entries, listed.files),
    `the ZIP's files = the page's files (names in order, sizes, bytes, CRC-32, UTF-8)\n      ${entries.map((e) => `${e.name}:${e.size}`).join(' | ')}`);
}

dev('107', 1500);  // a 4th: only it is offered next
check(await A.until(`${cards} === 4 && !document.getElementById('saveall').disabled`, 15000), 'a new screenshot after saving: 全部儲存（1）');
console.log('      button:', await A.evaluate(`document.getElementById('saveall').textContent`));
await A.shot('p4-new');
check(await B.until(`${cards} === 4 && document.getElementById('zipall').getAttribute('href') === 'zip?from=3'`, 15000),
  'http tab after the ZIP: 4 cards, the button offers only the new one (zip?from=3)');
const newLabel = await B.evaluate(`document.getElementById('zipall').textContent`);
check(/1/.test(newLabel), `http tab button reads "${newLabel}"`);
const newer = JSON.parse(await B.evaluate(`fetch('list').then(r => r.text())`)).files.filter((f) => f.i >= 3);
let fromEntries = [];
try { fromEntries = unzip(Buffer.from(await (await fetch(`${url}zip?from=3`)).arrayBuffer())); } catch (e) { console.log('      unzip:', e.message); }
check(fromEntries.length === 1 && await sameAsServer(fromEntries, newer), `zip?from=3: just the new screenshot (${fromEntries[0]?.name})`);
await B.shot('p6-http-new');

A.ws.close(); B.ws.close(); browser.ws.close(); proc.kill();
console.log(failed ? `${failed} FAILED` : 'live page: all checks passed');
process.exit(failed ? 1 : 0);
