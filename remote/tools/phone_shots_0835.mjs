// 0.8.3.5 files: before / after screenshots of the phone page's 檔案 button, per language and login kind.
//   node remote/tools/phone_shots_0835.mjs <outRoot> [--before-rev 74f63b1]
// The page is opened as a file (no host, no network): before = remote/web at <before-rev> (git show),
// after = the working tree's remote/web; window.PM gets the language. One chrome-headless-shell
// (quiet_browser.mjs: no window, no Alt+Tab entry, never the focus), closed at the end; the watcher's
// count is printed. Scenes (the state a PC would have sent):
//   guest   臨時帳號 made 可操作 without 可以傳檔 (what the owner had): the button is tapped
//   main    主帳號 (fixed link), files on: the button is tapped, the sheet opens
//   view    僅觀看
// Output: <outRoot>/<before|after>/<lang>_<scene>.png
import {execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {launch, cdpClient, watchAltTab} from './quiet_browser.mjs';

const [outRoot, ...rest] = process.argv.slice(2);
if (!outRoot) { console.error('usage: node phone_shots_0835.mjs <outRoot> [--before-rev REV]'); process.exit(2); }
let rev = '74f63b1';
for (let i = 0; i < rest.length; i++) if (rest[i] === '--before-rev') rev = rest[++i];
const HERE = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const REPO = path.resolve(HERE, '..', '..');
const LANGS = ['zh-TW', 'en', 'ja', 'ko'];
const sleep = ms => new Promise(r => setTimeout(r, ms));
const work = fs.mkdtempSync(path.join(os.tmpdir(), 'pm-phoneshots35-'));

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

// What the host's offer said for each scene (upload / viewOnly as 0.8.3.4 sent them; filesWhy is new in 0.8.3.5).
const SCENES = {
  guest: {viewOnly: false, upload: false, filesWhy: 'guest'},
  main: {viewOnly: false, upload: true, filesWhy: ''},
  view: {viewOnly: true, upload: false, filesWhy: ''},
  // the toolbar alone (sheet closed, no toast): where the five buttons sit, at 390 and 360 px wide
  bar: {viewOnly: false, upload: true, filesWhy: '', closed: true},
  barguest: {viewOnly: false, upload: false, filesWhy: 'guest', closed: true},
};
const WIDTHS = [390, 360];

const br = await launch(path.join(work, '_shell'));
const watch = watchAltTab([br.proc.pid], path.join(work, 'alttab.txt'));
const C = await cdpClient(br.ver.webSocketDebuggerUrl);
const made = [];
try {
  for (const which of ['before', 'after']) {
    fs.mkdirSync(path.join(outRoot, which), {recursive: true});
    for (let lang = 0; lang < 4; lang++) {
      const dir = pageDir(which, lang);
      for (const [scene0, st] of Object.entries(SCENES)) for (const width of (st.closed ? WIDTHS : [390])) {
        const scene = st.closed ? `${scene0}${width}` : scene0;
        const {targetId} = await C.cdp('Target.createTarget', {url: 'about:blank'});
        const {sessionId} = await C.cdp('Target.attachToTarget', {targetId, flatten: true});
        await C.cdp('Emulation.setDeviceMetricsOverride', {width, height: 844, deviceScaleFactor: 3, mobile: true}, sessionId);
        await C.cdp('Page.navigate', {url: 'file:///' + path.join(dir, 'index.html').replace(/\\/g, '/')}, sessionId);
        await sleep(1800);
        const r0 = await C.cdp('Runtime.evaluate', {returnByValue: true, expression: `(() => {
          const st = document.createElement('style'); st.textContent = '#msg,#pass,#lock{display:none!important}#bar,#bar .ib{opacity:1!important}'; document.head.appendChild(st);
          S.viewOnly = ${st.viewOnly}; S.upload = ${st.upload}; S.uploadMax = 2147483648;
          if ('filesWhy' in S || ${JSON.stringify(which)} === 'after') S.filesWhy = ${JSON.stringify(st.filesWhy)};
          applyViewOnly(); document.getElementById('bar').style.display = 'flex';
          closeSheets(); ${st.closed} || $('bFile').onclick();
          // before: a guest without files opened the sheet and the PC then answered files-denied (the 僅觀看 text)
          ${which === 'before' && scene === 'guest' ? "toast(t('filesDenied'), 60000);" : ''}
          const bar = document.getElementById('bar').getBoundingClientRect(), W = document.documentElement.clientWidth;
          const lbl = [...document.querySelectorAll('#bar .bl')].map(e => { const r = e.getBoundingClientRect(); return Math.round(r.height) + 'h/' + Math.round(r.left) + '-' + Math.round(r.right); });
          return $('bFile').className + ' ' + $('bFile').style.display + ' sheet=' + $('fileS').style.display + ' W=' + W + ' bar=' + Math.round(bar.left) + '-' + Math.round(bar.right) + ' rightGap=' + Math.round(W - bar.right) + ' lbl=' + lbl.join(','); })()`}, sessionId);
        await sleep(700);
        const r = await C.cdp('Page.captureScreenshot', {format: 'png'}, sessionId);
        const file = path.resolve(outRoot, which, `${LANGS[lang]}_${scene}.png`);
        if (st.closed) console.log(which, LANGS[lang], scene, r0 && r0.result && r0.result.value);
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
for (const f of made) console.log(f);
console.log(`Alt+Tab / taskbar windows of the test's processes: max ${alt.max} over ${alt.samples} samples`);
process.exit(made.length === 56 && alt.max === 0 ? 0 : 1);
