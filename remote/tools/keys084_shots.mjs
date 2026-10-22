// 0.8.4 自訂快捷鍵: before / after pictures of the phone page's keyboard panel, and a layout check of every picture.
//   node remote/tools/keys084_shots.mjs <outRoot> [--before-rev e7f98d0]
// The page is opened as a file (no host, no network): before = remote/web at <before-rev> (git show), after = the
// working tree's remote/web; window.PM gets the language. One chrome-headless-shell (quiet_browser.mjs: no window, no
// Alt+Tab entry, never the focus), closed at the end; the watcher's count is printed. Scenes (portrait 390x844 /
// landscape 844x390, per language):
//   kb      the keyboard panel as it opens (before: the fixed buttons; after: with the 自訂 area and the 3 examples)
//   edit    (after) 編輯 on: handles and ✕ on every button
//   add     (after) the ＋ editor open with Ctrl + Shift ticked and a key picked
//   full    (after) 12 buttons (a long name among them) and the editor open
//   guest   (after) a 臨時帳號: the buttons can be used, no 編輯 / ＋
// Layout check per picture: nothing of the panel wider than the screen / cut at the sides, no two buttons of a row
// overlapping, the panel inside the screen. Output: <outRoot>/<before|after>/<lang>_<scene>_<p|l>.png and layout.json.
import {execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {launch, cdpClient, watchAltTab} from './quiet_browser.mjs';

const [outRoot, ...rest] = process.argv.slice(2);
if (!outRoot) { console.error('usage: node keys084_shots.mjs <outRoot> [--before-rev REV]'); process.exit(2); }
let rev = 'e7f98d0';
for (let i = 0; i < rest.length; i++) if (rest[i] === '--before-rev') rev = rest[++i];
const HERE = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const REPO = path.resolve(HERE, '..', '..');
const LANGS = ['zh-TW', 'en', 'ja', 'ko'];
const sleep = ms => new Promise(r => setTimeout(r, ms));
const work = fs.mkdtempSync(path.join(os.tmpdir(), 'pm-keys084-'));

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

const EX = [{n: '', c: ['Meta', 'V'], t: 'dClip'}, {n: '', c: ['Control', 'Shift', 'T'], t: 'dReopen'}, {n: '', c: ['Meta', 'L'], t: 'dLock'}];
const FULL = [...EX, {n: 'Premiere 剪下', c: ['Control', 'Shift', 'K']}, {n: '音量＋', c: ['AudioVolumeUp']}, {n: '音量－', c: ['AudioVolumeDown']},
  {n: '播放／暫停', c: ['MediaPlayPause']}, {n: 'OBS 錄影 Start/Stop recording', c: ['Control', 'Alt', 'Shift', 'F9']}, {n: '遊戲 Push-to-talk', c: ['Control', 'Alt', 'Meta', 'Shift', 'F12']},
  {n: '放大', c: ['Control', '=']}, {n: 'Alt+Tab', c: ['Alt', 'Tab']}, {n: '', c: ['Meta', 'Shift', 'S']}];
const SCENES = {
  kb: {after: true, before: true, js: ''},
  edit: {js: `ckEdit = true; ckRender();`},
  add: {js: `ckEdit = true; ckRender(); ckOpenEd(-1); $('ckName').value = 'Ctrl+Shift+N'; ckMods.add('Control'); ckMods.add('Shift'); ckKey = 'N'; ckSyncEd();`},
  full: {js: `S.keys = ckNorm(${JSON.stringify(FULL)}); ckEdit = true; ckRender(); ckOpenEd(3);`},
  guest: {js: `S.keysOk = false; S.keysWhy = 'guest'; ckRender();`},
};
const SIZES = {p: {w: 390, h: 844}, l: {w: 844, h: 390}};

// in the page: the layout of the open panel
const CHECK = `(() => {
  const out = {bad: []};
  const kb = document.getElementById('kb'), kr = kb.getBoundingClientRect();
  const W = innerWidth, H = innerHeight;
  if (kr.left < -0.5 || kr.right > W + 0.5 || kr.top < -0.5 || kr.bottom > H + 1) out.bad.push('panel outside the screen ' + [kr.left, kr.top, kr.right, kr.bottom].map(Math.round));
  if (kb.scrollWidth > kb.clientWidth + 1) out.bad.push('panel wider than itself (' + kb.scrollWidth + ' > ' + kb.clientWidth + ')');
  const vis = [...kb.querySelectorAll('*')].filter(e => e.offsetParent && e.getBoundingClientRect().width > 0 && e.tagName !== 'OPTION' && e.tagName !== 'OPTGROUP');
  let n = 0;
  for (const e of vis) {
    const r = e.getBoundingClientRect();
    if (r.left < kr.left - 0.5 || r.right > kr.right + 0.5) { out.bad.push('cut at the side: ' + (e.id || e.className || e.tagName) + ' ' + (e.textContent || '').trim().slice(0, 20)); if (++n > 5) break; }
  }
  const overlap = (a, b) => { const x = Math.min(a.right, b.right) - Math.max(a.left, b.left), y = Math.min(a.bottom, b.bottom) - Math.max(a.top, b.top); return x > 1 && y > 1; };
  const groups = ['#keys', '#ckList', '#ckHead', '#ckEd .r', '#ckEd .btnrow'];
  for (const g of groups) for (const box of document.querySelectorAll('#kb ' + g)) {
    if (!box.offsetParent) continue;
    const kids = [...box.children].filter(k => k.offsetParent);
    for (let i = 0; i < kids.length; i++) for (let j = i + 1; j < kids.length; j++)
      if (overlap(kids[i].getBoundingClientRect(), kids[j].getBoundingClientRect())) out.bad.push('overlap in ' + g + ': ' + (kids[i].textContent || '').trim().slice(0, 12) + ' / ' + (kids[j].textContent || '').trim().slice(0, 12));
  }
  // the text inside a button must fit it (ellipsis is allowed only on a custom button's name)
  for (const b of document.querySelectorAll('#kb button')) {
    if (!b.offsetParent) continue;
    if (b.scrollWidth > b.clientWidth + 1 && !b.classList.contains('ck')) out.bad.push('text cut in button ' + (b.id || b.textContent.trim().slice(0, 16)));
  }
  out.sheetScroll = kb.scrollHeight > kb.clientHeight + 1;
  out.buttons = document.querySelectorAll('#ckList .ck').length;
  return JSON.stringify(out);
})()`;

const br = await launch(path.join(work, '_shell'));
const watch = watchAltTab([br.proc.pid], path.join(work, 'alttab.txt'));
const C = await cdpClient(br.ver.webSocketDebuggerUrl);
const made = [], layout = [];
try {
  for (const which of ['before', 'after']) {
    fs.mkdirSync(path.join(outRoot, which), {recursive: true});
    for (let lang = 0; lang < 4; lang++) {
      const dir = pageDir(which, lang);
      for (const [scene, st] of Object.entries(SCENES)) {
        if (which === 'before' && !st.before) continue;
        for (const [pl, sz] of Object.entries(SIZES)) {
          const {targetId} = await C.cdp('Target.createTarget', {url: 'about:blank'});
          const {sessionId} = await C.cdp('Target.attachToTarget', {targetId, flatten: true});
          await C.cdp('Emulation.setDeviceMetricsOverride', {width: sz.w, height: sz.h, deviceScaleFactor: 3, mobile: true}, sessionId);
          await C.cdp('Page.navigate', {url: 'file:///' + path.join(dir, 'index.html').replace(/\\/g, '/')}, sessionId);
          await sleep(1500);
          const r0 = await C.cdp('Runtime.evaluate', {returnByValue: true, expression: `(() => {
            const st = document.createElement('style'); st.textContent = '#msg,#pass,#lock,#toast{display:none!important}#bar,#bar .ib{opacity:1!important}'; document.head.appendChild(st);
            document.getElementById('bar').style.display = 'flex';
            closeSheets();
            ${which === 'after' ? `S.keys = ckNorm(${JSON.stringify(EX)}); S.keysOk = true; ckRender();` : ''}
            showKb(true);
            ${which === 'after' ? st.js : ''}
            return 1; })()`}, sessionId);
          await sleep(500);
          const chk = JSON.parse((await C.cdp('Runtime.evaluate', {returnByValue: true, expression: CHECK}, sessionId)).result.value);
          const r = await C.cdp('Page.captureScreenshot', {format: 'png'}, sessionId);
          const file = path.resolve(outRoot, which, `${LANGS[lang]}_${scene}_${pl}.png`);
          if (r && r.data) { fs.writeFileSync(file, Buffer.from(r.data, 'base64')); made.push(file); }
          layout.push({which, lang: LANGS[lang], scene, view: pl, ...chk});
          console.log(`${which} ${LANGS[lang]} ${scene} ${pl}: ${chk.bad.length ? 'BAD ' + chk.bad.join(' | ') : 'ok'}${chk.sheetScroll ? ' (panel scrolls)' : ''} buttons=${chk.buttons}`);
          await C.cdp('Target.closeTarget', {targetId});
        }
      }
    }
  }
} finally {
  C.close();
  await br.close();
}
const alt = await watch.stop();
try { fs.rmSync(work, {recursive: true, force: true}); } catch {}
fs.writeFileSync(path.join(outRoot, 'layout.json'), JSON.stringify(layout, null, 1));
const bad = layout.filter(l => l.which === 'after' && l.bad.length);
console.log(`${made.length} pictures in ${outRoot}; layout problems in after: ${bad.length}`);
console.log(`Alt+Tab / taskbar windows of the test's processes: max ${alt.max} over ${alt.samples} samples`);
process.exit(bad.length === 0 && alt.max === 0 ? 0 : 1);
