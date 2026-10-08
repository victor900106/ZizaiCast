// node dev.mjs A [round]  -> work/dev_A_<round>.png (master, 6 moods, face zoom, icons)
import { toutou, monitor, icon, MOODS, THEMES } from './art.mjs';
import { renderHtml, page, svgUri } from './render.mjs';
const [, , dir = 'A', round = '0'] = process.argv;
const acc = THEMES.Sakura.accent, bg = THEMES.Sakura.bg;
export const master = (d, a = acc) => `<svg xmlns="http://www.w3.org/2000/svg" viewBox="10 0 310 240">${monitor(272, 40, a, d)}${toutou(d, { mood: 'idle', accent: a, beam: true, beamTo: [252, 62], sparkles: false })}</svg>`;
const cell = (m) => `<svg xmlns="http://www.w3.org/2000/svg" viewBox="30 10 230 205">${toutou(dir, { mood: m, accent: acc })}</svg>`;
const face = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="80 95 110 75">${toutou(dir, { mood: 'idle', accent: acc, phone: false, tilt: 0 })}</svg>`;
const ic = [256, 48, 32, 24, 16].map((s) => `<img src="${svgUri(icon(dir, s, acc, bg))}" width="${s}" height="${s}" style="margin:6px;vertical-align:bottom">`).join('');
const zoom = [16, 32].map((s) => `<canvas data-src="${svgUri(icon(dir, s, acc, bg))}" data-s="${s}" width="${s * 4}" height="${s * 4}" style="margin:6px;vertical-align:bottom"></canvas>`).join('');
const html = page(`<div style="display:flex;flex-wrap:wrap;gap:10px;padding:10px;width:1580px">
  <div style="width:620px;height:480px;background:${bg};border-radius:16px">${master(dir).replace('<svg', '<svg width="620" height="480"')}</div>
  <div style="width:440px;height:300px;background:#fff;border-radius:16px">${face.replace('<svg', '<svg width="440" height="300"')}</div>
  <div style="width:440px;height:300px;background:#fff;border-radius:16px">${master(dir).replace("<svg", "<svg width=\"440\" height=\"300\" style=\"filter:brightness(0)\"")}</div>
  ${MOODS.map((m) => `<div style="width:250px;height:225px;background:${bg};border-radius:12px;color:#c9a9a6;font-size:12px">${cell(m).replace('<svg', '<svg width="250" height="210"')}<div style="text-align:center;margin-top:-14px">${m}</div></div>`).join('')}
  <div style="background:${bg};border-radius:12px;padding:8px">${ic}${zoom}</div>
  <div style="background:#f3f3f3;border-radius:12px;padding:8px">${ic}</div>
</div>
<script>for(const c of document.querySelectorAll('canvas')){const i=new Image();i.onload=()=>{const s=+c.dataset.s,t=document.createElement('canvas');t.width=t.height=s;t.getContext('2d').drawImage(i,0,0,s,s);const x=c.getContext('2d');x.imageSmoothingEnabled=false;x.drawImage(t,0,0,s*4,s*4)};i.src=c.dataset.src}</script>`, '#777');
renderHtml(html, `work/dev_${dir}_${round}.png`, 1600, 1100, 1);
console.log(`work/dev_${dir}_${round}.png`);
