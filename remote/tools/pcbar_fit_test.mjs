// rel-0835: the PC bar (a browser with a mouse) stays inside the window in every language, with 1, 2 and 3 screens
// of the PC, at 1920 / 1280 / 1100 / 1000 / 760 / 720 px (node 22+). 0.8.3.5 (fix-0835-input) hid the labels only
// below 1100 px: in English and Japanese with two screens the bar was wider than a 1280 px window and cut at both
// ends. Now fitPcbar() drops the labels, then the rarer keys, only when the bar does not fit.
//   node remote/tools/pcbar_fit_test.mjs <outDir> [--web <remote/web dir>]
// The page is a copy of remote/web opened as a file in ONE chrome-headless-shell (quiet_browser.mjs: no window, no
// Alt+Tab entry, never the focus), no PC, no network; a watcher counts the Alt+Tab / taskbar windows (must stay 0).
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import {launch, cdpClient, watchAltTab} from './quiet_browser.mjs';

const args = process.argv.slice(2);
const outDir = args[0];
if (!outDir) { console.error('usage: node pcbar_fit_test.mjs <outDir> [--web dir]'); process.exit(2); }
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const web = args.includes('--web') ? args[args.indexOf('--web') + 1] : path.resolve(here, '..', 'web');
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
let failed = 0, n = 0;
const check = (name, ok, detail = '') => { n++; if (!ok) failed++; console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail !== '' ? '  -- ' + detail : ''}`); };

const dir = path.resolve(outDir, 'page');
fs.rmSync(dir, {recursive: true, force: true});
fs.mkdirSync(dir, {recursive: true});
for (const f of fs.readdirSync(web)) fs.copyFileSync(path.join(web, f), path.join(dir, f));
const page = fs.readFileSync(path.join(dir, 'index.html'), 'utf8');
const LANGS = ['zh-TW', 'en', 'ja', 'ko'];
LANGS.forEach((l, i) => fs.writeFileSync(path.join(dir, `p_${i}.html`), page.replace('window.PM = /*PM_CONFIG*/{};', `window.PM = {lang: ${i}, audio: true};`)));

const profileDir = path.join(os.tmpdir(), 'shell-0835-pcbar-' + crypto.randomBytes(3).toString('hex'));
const br = await launch(profileDir);
const watch = watchAltTab([br.proc.pid, process.pid], path.join(outDir, 'alttab_pcbar.txt'));
const C = await cdpClient(br.ver.webSocketDebuggerUrl);
const cdp = C.cdp;
const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
const {sessionId} = await cdp('Target.attachToTarget', {targetId, flatten: true});
const ev = async expression => {
  const r = await cdp('Runtime.evaluate', {expression, returnByValue: true}, sessionId);
  if (r && r.exceptionDetails) console.log('page error:', (r.exceptionDetails.exception && r.exceptionDetails.exception.description || r.exceptionDetails.text || '').split(String.fromCharCode(10))[0]);
  return r && r.result ? r.result.value : undefined;
};
const MON = [[{primary: true, w: 3440, h: 1440}], [{primary: true, w: 3440, h: 1440}, {primary: false, w: 1920, h: 1080}],
  [{primary: true, w: 3840, h: 2160}, {primary: false, w: 1920, h: 1080}, {primary: false, w: 1920, h: 1080}]];
try {
  for (let li = 0; li < 4; li++) {
    await cdp('Emulation.setDeviceMetricsOverride', {width: 1280, height: 720, deviceScaleFactor: 1, mobile: false}, sessionId);
    await cdp('Page.navigate', {url: 'file:///' + path.join(dir, `p_${li}.html`).replace(/\\/g, '/')}, sessionId);
    await sleep(1500);
    for (const mons of MON) {
      for (const w of [1920, 1280, 1100, 1000, 760, 720]) {
        await cdp('Emulation.setDeviceMetricsOverride', {width: w, height: 720, deviceScaleFactor: 1, mobile: false}, sessionId);
        await sleep(120);
        const b = await ev(`(() => { document.body.classList.add('pcui'); $('msg').style.display = 'none'; S.monitors = ${JSON.stringify(mons)}; buildMonitorSeg(); showPcbar(0); syncPcbar();
          const r = $('pcbarIn').getBoundingClientRect(), pl = document.querySelector('#pcbar .pl');
          return {l: r.left, r: r.right, top: r.top, labels: getComputedStyle(pl).display !== 'none', mons: $('pbMons').children.length,
            keys: [...document.querySelectorAll('#pgKeys .pk')].filter(k => k.offsetWidth).length, c: pcbar.className}; })()`);
        check(`${LANGS[li]} ${mons.length} screen(s) at ${w} px: the bar inside the window`, b && b.l >= 0 && b.r <= w && b.top < 20,
          b ? `${Math.round(b.l)}..${Math.round(b.r)}, labels ${b.labels}, ${b.mons} screen buttons, ${b.keys} keys, ${b.c}` : 'no answer');
        if (w === 1920) check(`${LANGS[li]} ${mons.length} screen(s) at 1920 px: labels shown, every key`, b && b.labels && b.keys === 5, b ? `${b.labels} ${b.keys}` : '');
        if (w === 1280 && li === 1 && mons.length === 2) {
          const r = await cdp('Page.captureScreenshot', {format: 'png'}, sessionId);
          if (r && r.data) fs.writeFileSync(path.join(outDir, 'en_2screens_1280.png'), Buffer.from(r.data, 'base64'));
        }
      }
    }
  }
} catch (e) { check('ran', false, String(e && e.stack || e)); }
try { await cdp('Target.closeTarget', {targetId}); } catch {}
const alt = await watch.stop();
check('no window in Alt+Tab / the taskbar during the test', alt && alt.samples > 0 && alt.max === 0, JSON.stringify(alt));
C.close();
await br.close();
try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
console.log(failed ? `FAILED (${failed} of ${n} failed)` : `ALL PASSED (${n})`);
process.exit(failed ? 1 : 0);
