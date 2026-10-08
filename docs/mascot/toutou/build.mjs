// 投投 Toutou concept build: SVG sources -> docs/mascot/toutou/svg, overview -> concepts.png
// Usage: node docs/mascot/toutou/build.mjs   (headless Chrome only; nothing is shown on screen)
import { writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { toutou, monitor, icon, MOODS, THEMES } from './art.mjs';
import { renderHtml, page, svgUri } from './render.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const SVG = join(here, 'svg');
mkdirSync(SVG, { recursive: true });
const S = THEMES.Sakura;

export const DIRS = {
  A: { name: '軟綿光暈 Soft Glow', tag: 'A', pitch: '一朵剛曬過太陽的棉花雲，抱著發光的小手機，把畫面化成一條帶像素的光絲帶送上大螢幕。',
    notes: '無描邊 · 徑向柔光 + 主題色邊緣反光 · 手機光暈 · 像素光帶' },
  B: { name: '貼圖感 Sticker', tag: 'B', pitch: '粗暖棕描邊 + 白色裁切邊的 LINE 貼圖系雲寶，額頭一枚 ▶ 胎記，最會耍表情。',
    notes: '統一描邊 6.5 · 平塗 + 一層主題色陰影 · ▶ 額頭記號 · ▶▶ 投送箭頭' },
  C: { name: '幾何精品 Geometric', tag: 'C', pitch: '8px 網格上的兩顆圓 + 一條膠囊，無描邊雙色塊；頭頂浮著一枚會亮的「投放訊號」。',
    notes: '8px 格線 · 無描邊雙色 · 螢幕形高光 · 投放訊號（idle 1 弧 / 連線 2 弧 / 睡著熄燈）' },
};
const MLABEL = { idle: '待機 idle', happy: '開心 happy', connecting: '連線中 connecting', surprised: '驚訝 surprised', sleepy: '想睡 sleepy', asleep: '睡著 asleep' };

const svgDoc = (vb, inner) => `<svg xmlns="http://www.w3.org/2000/svg" viewBox="${vb}">${inner}</svg>`;
const master = (d, a = S.accent) => svgDoc('10 0 310 240', monitor(272, 40, a, d) + toutou(d, { mood: 'idle', accent: a, beam: true, beamTo: [252, 62], sparkles: false }));
const moodSvg = (d, m, a = S.accent) => svgDoc('30 10 230 205', toutou(d, { mood: m, accent: a }));
const sized = (svg, w, h) => svg.replace('<svg ', `<svg width="${w}" height="${h}" `);

// ---------- sources ----------
for (const d of Object.keys(DIRS)) {
  writeFileSync(join(SVG, `${d}_master.svg`), master(d));
  for (const m of MOODS) writeFileSync(join(SVG, `${d}_${m}.svg`), moodSvg(d, m));
  for (const s of [256, 48, 32, 24, 16]) writeFileSync(join(SVG, `${d}_icon_${s}.svg`), icon(d, s, S.accent, S.bg));
  for (const [t, th] of Object.entries(THEMES)) writeFileSync(join(SVG, `${d}_master_${t}.svg`), master(d, th.accent));
}

// ---------- app idle mock (540x960) ----------
function mock(d, th = S) {
  const ch = svgDoc('20 20 260 200', toutou(d, { mood: 'idle', accent: th.accent, sparkles: true }));
  const box = (on, label) => `<div style="display:flex;align-items:center;gap:12px;margin:10px 0;font-size:19px;color:${th.text}">
    <span style="width:20px;height:20px;border-radius:5px;box-sizing:border-box;${on ? `background:${th.accent};` : `border:2px solid ${th.sub};`}display:inline-flex;align-items:center;justify-content:center">${on ? `<svg width="14" height="14" viewBox="0 0 14 14"><path d="M3 7.2l2.6 2.6L11 4.2" fill="none" stroke="${th.bg}" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"/></svg>` : ''}</span>${label}</div>`;
  const stars = [[60, 120, 2], [470, 90, 1.6], [420, 260, 1.4], [90, 420, 1.4], [480, 520, 2], [140, 610, 1.2]].map(([x, y, r]) => `<circle cx="${x}" cy="${y}" r="${r}" fill="${th.sub}" opacity=".55"/>`).join('');
  return `<div style="position:relative;width:540px;height:960px;background:radial-gradient(120% 70% at 50% 100%, ${th.bg} 0%, ${th.bg} 55%, ${th.bg2} 100%);overflow:hidden;border-radius:18px">
    <svg width="540" height="960" style="position:absolute;inset:0">${stars}</svg>
    <div style="position:absolute;top:250px;width:100%;text-align:center">
      <div style="font-size:34px;letter-spacing:3px;color:${th.text};font-weight:500">等待手機連線…</div>
      <div style="margin-top:18px;font-size:16px;color:${th.sub};line-height:1.8">在手機上開啟「螢幕鏡像輸出」，選擇這台電腦<br>Android 請掃描 QR Code，或輸入 PIN 配對</div>
      <div style="display:inline-block;text-align:left;margin-top:30px">${box(true, '開機時自動啟動')}${box(false, '連線時需要輸入 PIN 碼')}</div>
    </div>
    <div style="position:absolute;left:0;right:0;bottom:120px;height:80px;background:radial-gradient(50% 50% at 50% 50%, ${th.accent}33, transparent 70%)"></div>
    <div style="position:absolute;left:120px;bottom:130px;width:330px;height:254px;transform:rotate(-4deg)">${sized(ch, 330, 254)}</div>
  </div>`;
}

// ---------- icon block ----------
function iconBlock(d) {
  const ic = (s, bg) => `<img src="${svgUri(icon(d, s, S.accent, S.bg))}" width="${s}" height="${s}" style="display:block">`;
  const strip = (bg, fg) => `<div style="display:flex;align-items:flex-end;gap:14px;padding:10px 12px;background:${bg};border-radius:10px;margin-top:8px">${[48, 32, 24, 16].map((s) => ic(s)).join('')}<span style="font-size:11px;color:${fg};margin-left:auto">48·32·24·16</span></div>`;
  const zoom = [16, 32].map((s) => `<canvas data-src="${svgUri(icon(d, s, S.accent, S.bg))}" data-s="${s}" width="${s * 4}" height="${s * 4}" style="display:block;image-rendering:pixelated"></canvas>`).join('');
  return `<div style="width:300px">
    <div style="display:flex;justify-content:center;background:repeating-conic-gradient(#3a2a2e 0 25%,#33252a 0 50%) 0 0/16px 16px;border-radius:14px;padding:10px 0">${ic(256)}</div>
    ${strip('#1f1f22', '#999')}${strip('#ececec', '#666')}
    <div style="display:flex;align-items:flex-end;gap:14px;padding:10px 12px;background:#1f1f22;border-radius:10px;margin-top:8px">${zoom}<span style="font-size:11px;color:#999;margin-left:auto">16 / 32 ×4</span></div>
  </div>`;
}

const zoomScript = `<script>for(const c of document.querySelectorAll('canvas')){const i=new Image();i.onload=()=>{const s=+c.dataset.s,t=document.createElement('canvas');t.width=t.height=s;t.getContext('2d').drawImage(i,0,0,s,s);const x=c.getContext('2d');x.imageSmoothingEnabled=false;x.drawImage(t,0,0,s*4,s*4)};i.src=c.dataset.src}</script>`;

function row(d) {
  const D = DIRS[d];
  const cap = (t) => `<div style="font-size:12px;color:#8a7672;text-align:center;margin-top:4px">${t}</div>`;
  const exprs = MOODS.map((m) => `<div><div style="width:190px;height:170px;background:${S.bg};border-radius:12px">${sized(moodSvg(d, m), 190, 170)}</div>${cap(MLABEL[m])}</div>`).join('');
  const accents = Object.entries(THEMES).map(([t, th]) => `<div><div style="width:200px;height:155px;background:${th.bg};border-radius:12px">${sized(master(d, th.accent), 200, 155)}</div>${cap(`${t} <span style="display:inline-block;width:10px;height:10px;border-radius:3px;background:${th.accent};vertical-align:-1px"></span> ${th.accent}`)}</div>`).join('');
  return `<section style="display:flex;gap:22px;align-items:flex-start;padding:26px 30px;border-top:1px dashed #d9c9c3">
    <div style="width:270px;flex:none">
      <div style="display:inline-block;font-size:15px;font-weight:700;color:#fff;background:#4A2C30;border-radius:8px;padding:2px 10px">方向 ${D.tag}</div>
      <div style="font-size:27px;font-weight:700;color:#3b2a2e;margin:10px 0 8px">${D.name}</div>
      <div style="font-size:16px;color:#5b4a46;line-height:1.6">${D.pitch}</div>
      <div style="font-size:13px;color:#8a7672;line-height:1.7;margin-top:12px">${D.notes}</div>
    </div>
    <div style="flex:none"><div style="width:520px;height:402px;background:${S.bg};border-radius:16px">${sized(master(d), 520, 402)}</div>${cap('主視覺：抱著手機，把畫面投上大螢幕')}</div>
    <div style="flex:none;display:grid;grid-template-columns:repeat(3,190px);gap:10px 10px">${exprs}</div>
    <div style="flex:none"><div style="width:315px;height:560px;overflow:hidden;border-radius:12px"><div style="transform:scale(.5834);transform-origin:0 0">${mock(d)}</div></div>${cap('App 待機畫面 540×960')}</div>
    <div style="flex:none">${iconBlock(d)}${cap('圖示 256 / 48 / 32 / 24 / 16')}</div>
    <div style="flex:none;display:grid;grid-template-columns:repeat(2,200px);gap:10px 12px">${accents}</div>
  </section>`;
}

const W = 2620;
const html = page(`<div style="width:${W}px;background:#F6F0EC;padding-bottom:16px">
  <header style="padding:26px 30px 18px;display:flex;align-items:baseline;gap:22px">
    <div style="font-size:40px;font-weight:800;color:#3b2a2e">投投 Toutou — 主角吉祥物 · 三個風格方向</div>
    <div style="font-size:17px;color:#7a6662">自在投影 Zizai Cast · 一朵把手機畫面「投」上大螢幕的小雲精靈 · 原創向量 · 主題色只用在手機螢幕 / 光束 / 記號上</div>
  </header>
  ${Object.keys(DIRS).map(row).join('')}
</div>${zoomScript}`, '#F6F0EC');
const H = 120 + 3 * 650;
renderHtml(html, join(here, 'concepts.png'), W, H, 1);
// per-direction mock + master for close review
for (const d of Object.keys(DIRS)) {
  renderHtml(page(mock(d)), join(here, 'work', `mock_${d}.png`), 540, 960, 1);
}
console.log('built concepts.png');
