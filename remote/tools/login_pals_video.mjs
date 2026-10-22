// 0.8.3.7: "recordings" of the login page's cast: consecutive screenshots of the cast's region, put together with ffmpeg into mp4 + gif (node 22+).
//   node remote/tools/login_pals_video.mjs <outDir> [--only intro,follow,...] [--vp phone,pc] [--web <remote/web dir>] [--ffmpeg <path>]
// Scenes: intro follow account pw peek show wait wrong1 wrong3 lock happy.  The page is served by a local mock (/login: "right-pw" ok,
// "locked-pw" locked, "slow-pw" wrong after 2 s, "slowok-pw" ok after 1.6 s, else wrong); the typing and clicks are injected into the
// page with CDP (no real input); ONE chrome-headless-shell (no window), closed at the end.
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import http from 'node:http';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';
import {launch, cdpClient} from './quiet_browser.mjs';

const args = process.argv.slice(2);
const outDir = args[0];
if (!outDir) { console.error('usage: node login_pals_video.mjs <outDir> [--only a,b] [--vp phone,pc] [--web dir] [--ffmpeg path]'); process.exit(2); }
const opt = (n, d) => args.includes(n) ? args[args.indexOf(n) + 1] : d;
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const web = opt('--web', path.resolve(here, '..', 'web'));
const ALL = ['intro', 'follow', 'account', 'pw', 'peek', 'show', 'wait', 'wrong1', 'wrong3', 'lock', 'happy'];
const only = opt('--only', ALL.join(',')).split(',');
const vps = opt('--vp', 'phone,pc').split(',');
const ffmpeg = opt('--ffmpeg', 'ffmpeg');
const lang = +opt('--lang', 1);
const sleep = ms => new Promise(r => setTimeout(r, ms));
fs.mkdirSync(outDir, {recursive: true});

const html = fs.readFileSync(path.join(web, 'index.html'), 'utf8');
const srv = http.createServer((req, res) => {
  const u = new URL(req.url, 'http://x');
  if (req.method === 'POST' && u.pathname === '/login') {
    let b = ''; req.on('data', d => b += d); req.on('end', () => {
      const pw = (JSON.parse(b || '{}').password) || '';
      const send = (o, ms = 0) => setTimeout(() => { res.setHeader('content-type', 'application/json'); res.end(JSON.stringify(o)); }, ms);
      if (pw === 'right-pw') send({ok: true, token: 'tok'});
      else if (pw === 'slowok-pw') send({ok: true, token: 'tok'}, 1600);
      else if (pw === 'locked-pw') send({locked: true, retryAfter: 900});
      else if (pw === 'slow-pw') send({left: 4}, 2000);
      else send({left: 4});
    });
    return;
  }
  if (u.pathname === '/') { res.setHeader('content-type', 'text/html; charset=utf-8'); return res.end(html.replace('window.PM = /*PM_CONFIG*/{};', `window.PM = {fixed: true, lang: ${lang}, audio: true};`)); }
  if (/^\/(trackpad|vkbd)\.js$/.test(u.pathname)) { res.setHeader('content-type', 'text/javascript'); return res.end(fs.readFileSync(path.join(web, u.pathname.slice(1)))); }
  res.statusCode = u.pathname.startsWith('/k/') ? 401 : 404; res.end('');
});
await new Promise(r => srv.listen(0, '127.0.0.1', r));
const base = `http://127.0.0.1:${srv.address().port}/`;
const profileDir = path.join(os.tmpdir(), 'shell-0837-video-' + crypto.randomBytes(3).toString('hex'));
const br = await launch(profileDir);
const C = await cdpClient(br.ver.webSocketDebuggerUrl);
const cdp = C.cdp;
const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
const {sessionId} = await cdp('Target.attachToTarget', {targetId, flatten: true});
const ev = async expression => { const r = await cdp('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true}, sessionId); return r && r.result ? r.result.value : undefined; };
const VP = {phone: {width: 390, height: 844, deviceScaleFactor: 1, mobile: true}, pc: {width: 1440, height: 900, deviceScaleFactor: 1, mobile: false}};
const type = async (sel, text) => { await ev(`document.querySelector('${sel}').focus()`); for (const ch of text) { await cdp('Input.insertText', {text: ch}, sessionId); await sleep(60); } };
const open = async vp => { await cdp('Emulation.setDeviceMetricsOverride', VP[vp], sessionId); await cdp('Page.navigate', {url: base + '?l=' + lang}, sessionId); await sleep(2600); await ev(`document.activeElement && document.activeElement.blur()`); await sleep(300); };
const rectOf = async () => ev(`(() => { const r = document.getElementById('pals').getBoundingClientRect(); return {x: r.left, y: r.top, w: r.width, h: r.height}; })()`);
const mouse = (x, y) => cdp('Input.dispatchMouseEvent', {type: 'mouseMoved', x, y}, sessionId);

// scene: {setup, dur, acts: [[ms, fn]]}
const SC = {
  intro: {reload: true, dur: 2300},
  follow: {dur: 2600, acts: vp => { const w = VP[vp].width, h = VP[vp].height, a = []; for (let i = 0; i < 26; i++) { const t = i / 25; a.push([i * 100, () => mouse(w * (0.95 - 0.9 * t), h * (0.25 + 0.2 * Math.sin(t * 6)))]); } return a; }},
  account: {dur: 2400, acts: () => [[100, () => type('#passIn', 'a')], [500, () => type('#passIn', 'b')], [900, () => type('#passIn', 'c')], [1300, () => ev(`document.getElementById('passIn').value=''; document.getElementById('passIn').dispatchEvent(new Event('input',{bubbles:true}))`)]]},
  pw: {dur: 3600, acts: () => 'abcdefgh'.split('').map((c, i) => [100 + i * 230, () => type('#passIn', c)])},
  peek: {dur: 3300, acts: () => [[0, () => type('#passIn', 'x')], [300, () => type('#passIn', 'y')], [600, () => type('#passIn', 'z')]]},
  show: {dur: 4800, acts: () => [[100, () => type('#passIn', 'a')], [350, () => type('#passIn', 'b')], [700, () => ev(`document.getElementById('pwEye').click()`)], [2700, () => ev(`document.getElementById('pwEye').click()`)]]},
  wait: {dur: 3300, acts: () => [[0, () => type('#passIn', 'slow-pw')], [500, () => ev(`document.getElementById('passGo').click()`)]]},
  wrong1: {dur: 3600, acts: () => [[0, () => type('#passIn', 'wrong-pw')], [400, () => ev(`document.getElementById('passGo').click()`)]]},
  wrong3: {dur: 3800, pre: async () => { for (let i = 0; i < 2; i++) { await type('#passIn', 'wrong-pw'); await ev(`document.getElementById('passGo').click()`); await sleep(500); } }, acts: () => [[0, () => type('#passIn', 'wrong-pw')], [400, () => ev(`document.getElementById('passGo').click()`)]]},
  lock: {dur: 3000, acts: () => [[0, () => type('#passIn', 'locked-pw')], [400, () => ev(`document.getElementById('passGo').click()`)]]},
  happy: {dur: 1900, acts: () => [[300, () => ev(`PALS.happy(); 0`)]]},
};

async function record(vp, name) {
  const s = SC[name];
  const dir = path.join(outDir, vp, name);
  fs.rmSync(dir, {recursive: true, force: true}); fs.mkdirSync(dir, {recursive: true});
  let rect;
  if (s.reload) { await open(vp); rect = await rectOf(); await cdp('Page.navigate', {url: base + '?l=' + lang}, sessionId); }
  else { await open(vp); rect = await rectOf(); if (s.pre) await s.pre(); }
  const pad = 14, up = s.reload ? 260 : 20;
  const clip = {x: Math.max(0, rect.x - pad), y: Math.max(0, rect.y - up), width: Math.min(VP[vp].width, rect.w + 2 * pad), height: rect.h + up + 30, scale: 1};
  clip.width = Math.min(clip.width, VP[vp].width - clip.x); clip.height = Math.min(clip.height, VP[vp].height - clip.y);
  const acts = typeof s.acts === 'function' ? s.acts(vp) : [];
  const t0 = Date.now(), times = [];
  let ai = 0, n = 0;
  while (Date.now() - t0 < s.dur) {
    const el = Date.now() - t0;
    while (ai < acts.length && el >= acts[ai][0]) { await acts[ai][1](); ai++; }
    const r = await cdp('Page.captureScreenshot', {format: 'png', clip}, sessionId); if (!r || !r.data) { await sleep(40); continue; }
    times.push(Date.now() - t0);
    fs.writeFileSync(path.join(dir, `f${String(n++).padStart(3, '0')}.png`), Buffer.from(r.data, 'base64'));
  }
  // real-time playback: each frame is shown for the time until the next one
  const list = times.map((t, i) => `file 'f${String(i).padStart(3, '0')}.png'\nduration ${((times[i + 1] ?? t + 80) - t) / 1000}`).join('\n') + `\nfile 'f${String(times.length - 1).padStart(3, '0')}.png'\n`;
  fs.writeFileSync(path.join(dir, 'list.txt'), list);
  const w = clip.width - (clip.width % 2), h = clip.height - (clip.height % 2);
  const common = ['-y', '-loglevel', 'error', '-f', 'concat', '-safe', '0', '-i', path.join(dir, 'list.txt')];
  try {
    execFileSync(ffmpeg, [...common, '-vf', `fps=20,crop=${w}:${h}:0:0,format=yuv420p`, '-c:v', 'libx264', '-crf', '20', path.join(outDir, `${vp}_${name}.mp4`)], {stdio: 'inherit'});
    execFileSync(ffmpeg, [...common, '-vf', `fps=15,crop=${w}:${h}:0:0,split[a][b];[a]palettegen=max_colors=96[p];[b][p]paletteuse=dither=bayer:bayer_scale=4`, path.join(outDir, `${vp}_${name}.gif`)], {stdio: 'inherit'});
    // a contact sheet: 12 frames evenly spread
    const k = Math.max(1, Math.floor(times.length / 12));
    execFileSync(ffmpeg, ['-y', '-loglevel', 'error', '-framerate', '10', '-i', path.join(dir, 'f%03d.png'), '-vf', `select='not(mod(n,${k}))',crop=${w}:${h}:0:0,scale=${Math.round(w * (vp === 'pc' ? .6 : .85))}:-1,tile=6x2:padding=4:color=0x1E1416`, '-frames:v', '1', path.join(outDir, `${vp}_${name}_sheet.png`)], {stdio: 'inherit'});
  } catch (e) { console.log('ffmpeg:', String(e.message).split('\n')[0]); }
  console.log(`${vp} ${name}: ${times.length} frames in ${s.dur} ms`);
}

try {
  for (const vp of vps) for (const name of ALL.filter(n => only.includes(n))) await record(vp, name);
} catch (e) { console.log('error:', e && e.stack || e); }
try { await cdp('Target.closeTarget', {targetId}); } catch {}
C.close();
await br.close();
srv.close();
try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
