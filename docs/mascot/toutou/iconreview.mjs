// Icon critique sheet: node iconreview.mjs <round> -> work/icon_<round>.png (1:1 on dark / light taskbars + x8 zoom)
import { iconSvg, ICON_SIZES } from './icon.mjs';
import { renderHtml, page, svgUri } from './render.mjs';
const r = process.argv[2] || '0';
const img = (s, z = 1, o = {}) => `<img src="${svgUri(iconSvg(s, o))}" width="${s * z}" height="${s * z}" style="image-rendering:pixelated;vertical-align:bottom;margin:4px">`;
// zoom must sample the 1x raster: draw to canvas at 1x first
const zc = (s) => `<canvas data-src="${svgUri(iconSvg(s))}" data-s="${s}" width="${s * 5}" height="${s * 5}" style="margin:4px;vertical-align:bottom"></canvas>`;
const bar = (bg) => `<div style="background:${bg};padding:6px 10px">${ICON_SIZES.map((s) => img(s)).join('')}${ICON_SIZES.slice(0, 5).map((s) => img(s, 1, { tile: false })).join('')}</div>`;
const html = page(`${bar('#1c1c1c')}${bar('#eeeeee')}${bar('#2A1C1F')}
<div style="background:#1c1c1c;padding:6px">${[16, 20, 24, 32].map(zc).join('')}</div>
<div style="background:#1c1c1c;padding:6px">${[40, 48, 64].map(zc).join('')}${img(256)}</div>
<script>for(const c of document.querySelectorAll('canvas')){const i=new Image();i.onload=()=>{const s=+c.dataset.s,t=document.createElement('canvas');t.width=t.height=s;t.getContext('2d').drawImage(i,0,0,s,s);const x=c.getContext('2d');x.imageSmoothingEnabled=false;x.drawImage(t,0,0,s*5,s*5)};i.src=c.dataset.src}</script>`, '#555');
renderHtml(html, `work/icon_${r}.png`, 1500, 1400, 1);
console.log(`work/icon_${r}.png`);
