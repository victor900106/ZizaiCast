// Critique sheet for the final hero art.  node review.mjs <round>  -> work/final_<round>.png
// Big master with beam, every mood large, the same moods at 120 px (the size they must read at),
// a face / hand zoom and a black silhouette.  Headless Chrome only.
import { hero, monitor, MOODS, THEMES } from './hero.mjs';
import { renderHtml, page } from './render.mjs';
const r = process.argv[2] || '0';
const S = THEMES.Sakura;
const svg = (vb, inner, w, h, extra = '') => `<svg xmlns="http://www.w3.org/2000/svg" viewBox="${vb}" width="${w}" height="${h}" ${extra}>${inner}</svg>`;
export const master = (th = S, mood = 'idle') => monitor(322, 34, th.accent) + hero({ mood, accent: th.accent, beam: true, beamTo: [286, 44] });
const MVB = '20 -14 350 270';
const cells = MOODS.map((m) => `<div style="background:${S.bg};border-radius:12px;display:inline-block;margin:4px">${svg('34 18 222 190', hero({ mood: m, accent: S.accent }), 250, 214)}<div style="color:#c9a9a6;font:12px sans-serif;text-align:center;margin-top:-16px">${m}</div></div>`).join('');
const small = MOODS.map((m) => `<span style="display:inline-block;background:${S.bg};margin:3px;border-radius:8px">${svg('34 18 222 190', hero({ mood: m, accent: S.accent }), 120, 103)}</span>`).join('');
const smallLight = MOODS.map((m) => `<span style="display:inline-block;background:#F4EEEA;margin:3px;border-radius:8px">${svg('34 18 222 190', hero({ mood: m, accent: S.accent }), 120, 103)}</span>`).join('');
const themes = Object.values(THEMES).map((th) => `<span style="display:inline-block;background:${th.bg};margin:3px;border-radius:10px">${svg(MVB, master(th), 248, 191)}</span>`).join('');
const html = page(`<div style="padding:8px;width:1890px">
  <div style="display:flex;gap:8px">
    <div style="background:${S.bg};border-radius:14px">${svg(MVB, master(), 640, 494)}</div>
    <div>
      <div style="background:#fff;border-radius:14px;margin-bottom:8px">${svg('86 92 100 66', hero({ mood: 'idle', phone: false, fx: false, shadow: false }), 300, 198)}${svg('170 100 80 66', hero({ mood: 'idle', fx: false }), 300, 248)}</div>
    </div>
    <div>
      <div style="background:${S.bg};border-radius:14px;margin-bottom:8px">${svg('180 30 120 108', hero({ mood: 'connecting', fx: false, beamTo: [286, 44] }), 300, 270)}</div>
      <div style="background:#fff;border-radius:14px">${svg(MVB, master(), 300, 232, 'style="filter:brightness(0)"')}</div>
    </div>
  </div>
  <div>${cells}</div>
  <div>${small}</div>
  <div>${smallLight}</div>
  <div>${themes}</div>
</div>`, '#666');
renderHtml(html, `work/final_${r}.png`, 1900, 1200, 1);
renderHtml(page(`<div style="padding:4px">${small}</div>`, '#666'), `work/final_${r}_120x2.png`, 900, 120, 2);
console.log(`work/final_${r}.png`);
