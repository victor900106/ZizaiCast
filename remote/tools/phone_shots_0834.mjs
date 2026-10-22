// 0.8.3.4 before / after screenshots of the phone page's settings sheet (手機音量, 電腦喇叭靜音), per language.
//   node remote/tools/phone_shots_0834.mjs <outRoot> [--before-rev 016da34]
// The page is opened as a file (no host, no network): before = remote/web at <before-rev> (git show),
// after = the working tree's remote/web; window.PM gets the language. One chrome-headless-shell
// (quiet_browser.mjs: no window, no Alt+Tab entry, never the focus), closed at the end; the watcher's
// count is printed. Output: <outRoot>/before/<lang>_P2-settings.png, <outRoot>/after/<lang>_P2-settings.png
// (the real-session shots: mutepc_test.mjs --shots).
import {execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {launch, cdpClient, watchAltTab} from './quiet_browser.mjs';

const [outRoot, ...rest] = process.argv.slice(2);
if (!outRoot) { console.error('usage: node phone_shots_0834.mjs <outRoot> [--before-rev REV]'); process.exit(2); }
let rev = '016da34';
for (let i = 0; i < rest.length; i++) if (rest[i] === '--before-rev') rev = rest[++i];
const HERE = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const REPO = path.resolve(HERE, '..', '..');
const LANGS = ['zh-TW', 'en', 'ja', 'ko'];
const sleep = ms => new Promise(r => setTimeout(r, ms));
const work = fs.mkdtempSync(path.join(os.tmpdir(), 'pm-phoneshots-'));

// a copy of remote/web (before: from git) per language, with window.PM = {lang}
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

const br = await launch(path.join(work, '_shell'));
const watch = watchAltTab([br.proc.pid], path.join(work, 'alttab.txt'));
const C = await cdpClient(br.ver.webSocketDebuggerUrl);
const made = [];
try {
  for (const which of ['before', 'after']) {
    fs.mkdirSync(path.join(outRoot, which), {recursive: true});
    for (let lang = 0; lang < 4; lang++) {
      const dir = pageDir(which, lang);
      const {targetId} = await C.cdp('Target.createTarget', {url: 'about:blank'});
      const {sessionId} = await C.cdp('Target.attachToTarget', {targetId, flatten: true});
      await C.cdp('Emulation.setDeviceMetricsOverride', {width: 390, height: 844, deviceScaleFactor: 3, mobile: true}, sessionId);
      // rel-0836: a touch screen, as a phone is ((pointer: coarse): 解析度跟手機一樣 shows its phone default, on)
      await C.cdp('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5}, sessionId);
      await C.cdp('Page.navigate', {url: 'file:///' + path.join(dir, 'index.html').replace(/\\/g, '/')}, sessionId);
      await sleep(2000);
      // the settings sheet over whatever the page shows without a PC, scrolled to the sound rows; after:
      // as for the owner (the PC offers sound, the switch may be changed)
      await C.cdp('Runtime.evaluate', {returnByValue: true, expression: `(() => {
        const st = document.createElement('style'); st.textContent = '#msg,#pass{display:none!important}'; document.head.appendChild(st);
        if (typeof S !== 'undefined' && typeof syncMutePc === 'function') { S.audioOffered = true; S.mutePc = false; S.mutePcOk = true; S.viewOnly = false; syncMutePc(); }
        closeSheets(); $('bSet').onclick(); const r = $('rowVol'); r && r.scrollIntoView({block: 'start'}); return 1; })()`}, sessionId);
      await sleep(700);
      const r = await C.cdp('Page.captureScreenshot', {format: 'png'}, sessionId);
      const file = path.resolve(outRoot, which, `${LANGS[lang]}_P2-settings.png`);
      if (r && r.data) { fs.writeFileSync(file, Buffer.from(r.data, 'base64')); made.push(file); }
      await C.cdp('Target.closeTarget', {targetId});
    }
  }
} finally {
  C.close();
  await br.close();
}
const alt = await watch.stop();
try { fs.rmSync(work, {recursive: true, force: true}); } catch {}
for (const f of made) console.log(f);
console.log(`Alt+Tab / taskbar windows of the test's processes: max ${alt.max} over ${alt.samples} samples`);
process.exit(made.length === 8 && alt.max === 0 ? 0 : 1);
