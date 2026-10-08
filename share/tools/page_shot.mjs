// Phone-sized screenshot of a 傳到手機 page (headless Chrome, mobile emulation):
//   node share/tools/page_shot.mjs <url> <out.png> [ios|android] [width=390] [stub]
// Serve the page first: pm_share_test --serve 120 [--en] FILE... (127.0.0.1).
import { spawn } from 'node:child_process';
import { writeFileSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const [url, out, kind = 'ios', width = '390', stub = ''] = process.argv.slice(2);  // stub: fake navigator.share (全部儲存 shown); none: no file sharing (fallback)
const UA = kind === 'android'
  ? 'Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/129.0 Mobile Safari/537.36'
  : 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1';
const PORT = 9300 + Math.floor(Math.random() * 400);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const proc = spawn('C:/Program Files/Google/Chrome/Application/chrome.exe', ['--headless=new', '--disable-gpu', '--hide-scrollbars',
  '--mute-audio', `--remote-debugging-port=${PORT}`, `--user-data-dir=${mkdtempSync(join(tmpdir(), 'pmshare-chrome-'))}`, 'about:blank'],
  { stdio: 'ignore' });
let list = [];
for (let i = 0; i < 100 && !list.length; i++) { try { list = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json(); } catch {} await sleep(100); }
const ws = new WebSocket(list.find((p) => p.type === 'page').webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener('open', r, { once: true }));
let id = 0; const wait = new Map();
ws.addEventListener('message', (e) => { const m = JSON.parse(e.data); if (m.id && wait.has(m.id)) { wait.get(m.id)(m); wait.delete(m.id); } });
const send = (method, params = {}) => new Promise((r) => { const i = ++id; wait.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const w = Number(width);
await send('Emulation.setUserAgentOverride', { userAgent: UA });
await send('Emulation.setDeviceMetricsOverride', { width: w, height: 844, deviceScaleFactor: 1, mobile: true });
await send('Emulation.setTouchEmulationEnabled', { enabled: true });
await send('Page.enable');
if (stub) await send('Page.addScriptToEvaluateOnNewDocument', { source:
  "Object.defineProperty(Navigator.prototype,'canShare',{configurable:true,value:()=>" + (stub === 'none' ? 'false' : 'true') + "});Object.defineProperty(Navigator.prototype,'share',{configurable:true,value:()=>Promise.resolve()});" });
await send('Page.navigate', { url });
await sleep(3000);
const m = await send('Runtime.evaluate', { returnByValue: true, expression: '[document.documentElement.scrollWidth, document.documentElement.scrollHeight]' });
const [sw, sh] = m.result.result.value;
const r = await send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true, clip: { x: 0, y: 0, width: w, height: sh, scale: 1 } });
writeFileSync(out, Buffer.from(r.result.data, 'base64'));
console.log(out, 'scrollWidth', sw, 'height', sh);
ws.close(); proc.kill();
