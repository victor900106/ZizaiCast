import { icon, THEMES } from './art.mjs';
import { renderHtml, page, svgUri } from './render.mjs';
const r = process.argv[2] || '0', S = THEMES.Sakura;
const rows = ['A', 'B', 'C'].map((d) => `<div style="display:flex;align-items:flex-end;gap:16px;margin:8px">
 <img src="${svgUri(icon(d, 256, S.accent, S.bg))}" width=256 height=256>
 ${[48, 32, 24, 16].map((s) => `<img src="${svgUri(icon(d, s, S.accent, S.bg))}" width=${s} height=${s}>`).join('')}
 ${[48, 32, 24, 16].map((s) => `<img src="${svgUri(icon(d, s, S.accent, S.bg))}" width=${s} height=${s} style="background:#eee;padding:4px">`).join('')}
 ${[16, 24, 32].map((s) => `<canvas data-src="${svgUri(icon(d, s, S.accent, S.bg))}" data-s="${s}" width="${s * 6}" height="${s * 6}"></canvas>`).join('')}</div>`).join('');
renderHtml(page(rows + `<script>for(const c of document.querySelectorAll('canvas')){const i=new Image();i.onload=()=>{const s=+c.dataset.s,t=document.createElement('canvas');t.width=t.height=s;t.getContext('2d').drawImage(i,0,0,s,s);const x=c.getContext('2d');x.imageSmoothingEnabled=false;x.drawImage(t,0,0,s*6,s*6)};i.src=c.dataset.src}</script>`, '#2b2b30'), `work/icons_${r}.png`, 1100, 830, 1);
