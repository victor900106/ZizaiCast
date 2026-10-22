// 0.8.3.6 影像: before / after screenshots of the phone page's settings sheet (畫質, 每秒畫面, 解析度跟手機一樣)
// and the stats overlay, per language.
//   node remote/tools/phone_shots_0836v.mjs <outRoot> [--before-rev 8929de9]
// The page is opened as a file (no host, no network): before = remote/web at <before-rev> (git show), after =
// the working tree's remote/web; window.PM gets the language. One chrome-headless-shell (quiet_browser.mjs: no
// window, no Alt+Tab entry, never the focus), closed at the end; the watcher's count is printed. Scenes:
//   set     the settings sheet as it opens (defaults)
//   cap     每秒畫面 120 on a PC whose screen runs at 60 Hz (the PC's stats say hz 60)
//   hud     連線資訊 on, with the stats a PC sends (3440x1440 screen at 144 Hz, 120 fps asked)
// Output: <outRoot>/<before|after>/<lang>_<scene>.png
import {execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {launch, cdpClient, watchAltTab} from './quiet_browser.mjs';

const [outRoot, ...rest] = process.argv.slice(2);
if (!outRoot) { console.error('usage: node phone_shots_0836v.mjs <outRoot> [--before-rev REV]'); process.exit(2); }
let rev = '8929de9';
for (let i = 0; i < rest.length; i++) if (rest[i] === '--before-rev') rev = rest[++i];
const HERE = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const REPO = path.resolve(HERE, '..', '..');
const LANGS = ['zh-TW', 'en', 'ja', 'ko'];
const sleep = ms => new Promise(r => setTimeout(r, ms));
const work = fs.mkdtempSync(path.join(os.tmpdir(), 'pm-phoneshots36v-'));

function pageDir(which, lang) {
  const dir = path.join(work, `${which}-${lang}`);
  fs.mkdirSync(dir, {recursive: true});
  const files = which === 'before'
    ? execFileSync('git', ['-C', REPO, 'ls-tree', '--name-only', `${rev}:remote/web`], {encoding: 'utf8'}).split('\n').filter(Boolean)
    : fs.readdirSync(path.join(REPO, 'remote', 'web'));
  for (const f of files) {
    let b = which === 'before' ? execFileSync('git', ['-C', REPO, 'show', `${rev}:remote/web/${f}`]) : fs.readFileSync(path.join(REPO, 'remote', 'web', f));
    if (f === 'index.html') b = Buffer.from(b.toString('utf8').replace('/*PM_CONFIG*/{}', JSON.stringify({lang})), 'utf8');
    fs.writeFileSync(path.join(dir, f), b);
  }
  return dir;
}

const SRV = {t: 'stats', fps: 119.8, kbps: 18400, c2s_avg: 6.6, c2s_p95: 8.5, c2e_avg: 6.0, c2e_p95: 7.8, enc_avg: 5.8, enc_p95: 7.1, render_avg: 0.2,
  cpu: 0.8, cpu1: 19.3, encoder: 'NVIDIA H.264 Encoder MFT', hw: true, gpuPath: true, w: 3234, h: 1352, deskW: 3440, deskH: 1440, inject: false,
  stamp: true, dropped: 0, maxKB: 106, viewOnly: false, audio: false, monitor: 0, quality: 2, fpsNow: 120, paused: false, idleMinutes: 30,
  aOn: false, aPeak: 0, aDev: '', fpsWant: 120, fpsCap: 120, hz: 144, kbpsBase: 19300, fit: false, levelCut: true};
const SCENES = {
  set: {js: ''},
  cap: {js: `prefs.fps = 120; S.srv = Object.assign({}, ${JSON.stringify(SRV)}, {hz: 60, fpsCap: 60, fpsWant: 120}); typeof syncSettings === 'function' && syncSettings();`},
  hud: {hud: true, js: `S.srv = ${JSON.stringify(SRV)}; S.state = 'connected'; S.w = 3234; S.h = 1352; S.fps = 119.6; S.kbps = 18200; S.rtt = 3.1; S.rttMin = 1.2;
    S.latShow = 17; S.latP95 = 19; S.e2e = 24; S.dec = 3.2; S.jb = 1.1; S.lost = 0; S.pair = 'host(192.168.1.20) ↔ host(192.168.1.10:50000)';
    prefs.hud = true; hud.style.display = 'block';`},
};

const br = await launch(path.join(work, '_shell'));
const watch = watchAltTab([br.proc.pid], path.join(work, 'alttab.txt'));
const C = await cdpClient(br.ver.webSocketDebuggerUrl);
const made = [];
try {
  for (const which of ['before', 'after']) {
    fs.mkdirSync(path.join(outRoot, which), {recursive: true});
    for (let lang = 0; lang < 4; lang++) {
      const dir = pageDir(which, lang);
      for (const [scene, st] of Object.entries(SCENES)) {
        const {targetId} = await C.cdp('Target.createTarget', {url: 'about:blank'});
        const {sessionId} = await C.cdp('Target.attachToTarget', {targetId, flatten: true});
        await C.cdp('Emulation.setDeviceMetricsOverride', {width: 390, height: st.hud ? 844 : 1240, deviceScaleFactor: 3, mobile: true}, sessionId);
        await C.cdp('Page.navigate', {url: 'file:///' + path.join(dir, 'index.html').replace(/\\/g, '/')}, sessionId);
        await sleep(1800);
        const r0 = await C.cdp('Runtime.evaluate', {returnByValue: true, expression: `(() => {
          const st = document.createElement('style'); st.textContent = '#msg,#pass,#lock,#toast{display:none!important}#bar,#bar .ib{opacity:1!important}'; document.head.appendChild(st);
          document.getElementById('bar').style.display = 'flex';
          ${st.js}
          closeSheets(); ${st.hud ? '' : "$('bSet').onclick();"}
          const rows = [...document.querySelectorAll('#set .row')].filter(r => r.offsetParent).map(r => r.innerText.replace(/\\s+/g, ' ').slice(0, 60));
          return rows.join(' | '); })()`}, sessionId);
        await sleep(st.hud ? 1300 : 700);
        const r = await C.cdp('Page.captureScreenshot', {format: 'png'}, sessionId);
        const file = path.resolve(outRoot, which, `${LANGS[lang]}_${scene}.png`);
        if (scene !== 'hud') console.log(which, LANGS[lang], scene, r0 && r0.result && r0.result.value);
        if (r && r.data) { fs.writeFileSync(file, Buffer.from(r.data, 'base64')); made.push(file); }
        await C.cdp('Target.closeTarget', {targetId});
      }
    }
  }
} finally {
  C.close();
  await br.close();
}
const alt = await watch.stop();
try { fs.rmSync(work, {recursive: true, force: true}); } catch {}
console.log(made.length + ' pictures in ' + outRoot);
console.log(`Alt+Tab / taskbar windows of the test's processes: max ${alt.max} over ${alt.samples} samples`);
process.exit(made.length === 24 && alt.max === 0 ? 0 : 1);
