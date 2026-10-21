// Minimal headless Chrome driver over CDP (own profile, debugging port on 127.0.0.1 only).
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
const CHROME = 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const sleep = ms => new Promise(r => setTimeout(r, ms));
export async function browser({width = 412, height = 860, dpr = 2, mobile = true} = {}) {
  const port = 9300 + Math.floor(Math.random() * 400);
  const proc = spawn(CHROME, ['--headless=new', '--disable-gpu', '--hide-scrollbars', '--no-first-run', '--mute-audio',
    `--user-data-dir=${(process.env.UX_WORK||process.env.TEMP+"/zizai-ux-shots")}/_chrome_cdp`, `--remote-debugging-port=${port}`, '--remote-debugging-address=127.0.0.1', 'about:blank'], {stdio: 'ignore'});
  let tabs;
  for (let i = 0; i < 60; i++) { try { tabs = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json(); if (tabs.length) break; } catch {} await sleep(250); }
  const page = tabs.find(t => t.type === 'page');
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise(r => ws.addEventListener('open', r));
  let id = 0; const pending = new Map();
  ws.addEventListener('message', e => { const m = JSON.parse(e.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } });
  const send = (method, params = {}) => new Promise(r => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({id: i, method, params})); });
  await send('Page.enable');
  await send('Emulation.setDeviceMetricsOverride', {width, height, deviceScaleFactor: dpr, mobile});
  const api = {
    send,
    async go(url, wait = 3000) { await send('Page.navigate', {url}); await sleep(wait); },
    async eval(expr) { const r = await send('Runtime.evaluate', {expression: expr, awaitPromise: true, returnByValue: true}); return r.result && r.result.result && r.result.result.value; },
    async shot(out, full = false) {
      let clip;
      if (full) {
        const m = await send('Page.getLayoutMetrics');
        const h = Math.ceil(m.result.cssContentSize.height);
        await send('Emulation.setDeviceMetricsOverride', {width, height: h, deviceScaleFactor: dpr, mobile});
        await sleep(400);
      }
      const r = await send('Page.captureScreenshot', {format: 'png', captureBeyondViewport: full});
      fs.writeFileSync(out, Buffer.from(r.result.data, 'base64'));
      if (full) await send('Emulation.setDeviceMetricsOverride', {width, height, deviceScaleFactor: dpr, mobile});
    },
    async close() { try { await send('Browser.close'); } catch {} await sleep(500); try { execFileSync('taskkill', ['/PID', String(proc.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} },
  };
  return api;
}
