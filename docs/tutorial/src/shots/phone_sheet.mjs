// The phone page (remote/web/index.html, a copy) rendered from file:// with a made-up PC picture,
// no server: the settings sheet (音量 / 觸控板・直接點) per language.
import {browser} from './cdp.mjs';
import fs from 'node:fs';
import path from 'node:path';
const REPO = path.resolve(path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1')), '..', '..', '..', '..');
// a copy of remote/web plus desk.png (feed/desk.html rendered at 1600x900) in UX_WORK/_phone
const dir = (process.env.UX_WORK || process.env.TEMP + '/zizai-ux-shots') + '/_phone';
const src = fs.readFileSync(`${dir}/index.html`, 'utf8');
const langs = {'zh-TW': 0, en: 1, ja: 2, ko: 3};
const b = await browser({width: 412, height: 860});
try {
  for (const [l, n] of Object.entries(langs)) {
    // FIXED-less, tunnel-less: connect() fails quietly against file://; the overlay is hidden below.
    fs.writeFileSync(`${dir}/p_${l}.html`, src.replace('window.PM = /*PM_CONFIG*/{};', `window.PM = {lang: ${n}, audio: true};`));
    await b.go(`file:///${dir}/p_${l}.html`, 2500);
    const r = await b.eval(`(() => {
      document.getElementById('msg').style.display = 'none';
      for (const id of ['v', 'scr']) document.getElementById(id).style.display = 'none';
      document.body.style.background = '#111 url(desk.png) center 38% / 100% auto no-repeat';
      const bar = document.getElementById('bar'); bar.style.display = 'flex'; bar.style.opacity = '1';
      document.getElementById('bSet').click();
      const s = document.getElementById('set'); s.style.display = 'block';
      const v = document.getElementById('vol'); v.value = 200; v.dispatchEvent(new Event('input', {bubbles: true}));
      return getComputedStyle(bar).display + ' ' + s.offsetHeight;
    })()`);
    console.log(l, r);
    await new Promise(r => setTimeout(r, 600));
    await b.shot(`${REPO}/_shots/tut/${l}/h_phone-settings.png`);
    await b.eval(`(() => { document.getElementById('set').style.display = 'none'; document.getElementById('bSet').classList.remove('on'); return 1 })()`);
    await new Promise(r => setTimeout(r, 400));
    await b.shot(`${REPO}/_shots/tut/${l}/h_phone-bar.png`);
  }
} finally { await b.close(); }
