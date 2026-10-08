import { toutou, MOODS, THEMES } from './art.mjs';
import { renderHtml, page } from './render.mjs';
const r = process.argv[2] || '0';
const cells = ['A', 'B', 'C'].map((d) => MOODS.map((m) => `<svg width="250" height="190" viewBox="50 40 200 150" style="background:${THEMES.Sakura.bg};border-radius:10px">${toutou(d, { mood: m, accent: THEMES.Sakura.accent })}</svg>`).join('')).join('<br>');
renderHtml(page(`<div style="padding:8px;line-height:0">${cells}</div>`, '#555', 'svg{margin:3px}'), `work/faces_${r}.png`, 1560, 610, 1);
