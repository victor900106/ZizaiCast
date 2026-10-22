// 0.8.3.7: two drafts of the login page's cast (phone / monitor / mouse + 投投), side by side, for the owner to pick from (node 22+).
//   node remote/tools/login_pals_design.mjs <outDir>
// Writes <outDir>/draftA.svg, draftB.svg and drafts.png (rendered in ONE chrome-headless-shell, no window; closed at the end).
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import {launch, cdpClient} from './quiet_browser.mjs';

const outDir = process.argv[2];
if (!outDir) { console.error('usage: node login_pals_design.mjs <outDir>'); process.exit(2); }
fs.mkdirSync(outDir, {recursive: true});
const INK = '#3A2830', GY = 282;

const cloud = (cx, s) => {
  const tx = cx - 133 * s, ty = GY - 166 * s;
  return `<g transform="translate(${tx} ${ty}) scale(${s})"><g fill="url(#cl)"><circle cx="86" cy="112" r="30"/><circle cx="132" cy="90" r="40"/><circle cx="178" cy="114" r="26"/><rect x="54" y="108" width="158" height="58" rx="29"/></g>` +
    `<ellipse cx="116" cy="66" rx="22" ry="12" transform="rotate(-24 116 66)" fill="#fff" opacity=".9"/><ellipse cx="96" cy="143" rx="10" ry="6" fill="#FF9AAE" opacity=".55"/><ellipse cx="170" cy="143" rx="10" ry="6" fill="#FF9AAE" opacity=".55"/>` +
    `<ellipse cx="111" cy="131" rx="7" ry="9.2" fill="${INK}"/><ellipse cx="155" cy="131" rx="7" ry="9.2" fill="${INK}"/><circle cx="109" cy="127.5" r="2.9" fill="#fff"/><circle cx="153" cy="127.5" r="2.9" fill="#fff"/>` +
    `<path d="M126 141Q133 149 140 141" fill="none" stroke="${INK}" stroke-width="3" stroke-linecap="round"/></g>`;
};
const eyes = (x, y, gap, r, col) => `<circle cx="${x - gap}" cy="${y}" r="${r}" fill="${col}"/><circle cx="${x + gap}" cy="${y}" r="${r}" fill="${col}"/>`;
const smile = (x, y, w, col) => `<path d="M${x - w} ${y}Q${x} ${y + w * 1.1} ${x + w} ${y}" fill="none" stroke="${col}" stroke-width="3" stroke-linecap="round"/>`;
const defs = `<defs><radialGradient id="cl" gradientUnits="userSpaceOnUse" cx="118" cy="70" r="125"><stop offset="0" stop-color="#fff"/><stop offset=".55" stop-color="#FBF7F8"/><stop offset="1" stop-color="#E3DAE6"/></radialGradient></defs>`;
const ground = `<ellipse cx="200" cy="284" rx="185" ry="5" fill="#F5A7A7" opacity=".18"/>`;

// A: light screens (ink faces), flat-colour bodies. Phone at the back, monitor in the middle, 投投 and the mouse in front.
const A = () => {
  const phone = `<rect x="122" y="74" width="86" height="208" rx="18" fill="#E8708A"/><rect x="130" y="90" width="70" height="172" rx="9" fill="#FFE6EA"/><rect x="154" y="80" width="22" height="4.5" rx="2.2" fill="#B84A63"/>` +
    eyes(165, 136, 19, 6.5, INK) + smile(165, 152, 8, INK);
  const monitor = `<rect x="254" y="250" width="16" height="26" fill="#F0A878"/><rect x="232" y="274" width="60" height="8" rx="4" fill="#F0A878"/><rect x="200" y="160" width="124" height="96" rx="12" fill="#FFC8A2"/><rect x="208" y="168" width="108" height="76" rx="7" fill="#FFF2E6"/>` +
    eyes(262, 200, 18, 5.5, INK) + smile(262, 214, 7, INK);
  const mouse = `<path d="M370 272C396 274 399 248 385 241" fill="none" stroke="#FF8F7A" stroke-width="5" stroke-linecap="round"/><rect x="316" y="203" width="12" height="24" rx="6" fill="#E5705C"/>` +
    `<path d="M274 282V256C274 228 296 214 322 214C348 214 370 228 370 256V282Z" fill="#FF8F7A"/><path d="M322 226V244" stroke="#E5705C" stroke-width="3" stroke-linecap="round"/>` +
    eyes(322, 258, 15, 5, INK) + smile(322, 267, 6, INK);
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 400 300">${defs}${ground}${phone}${monitor}${cloud(92, .72)}${mouse}</svg>`;
};
// B: dark screens with cream faces (the screens glow), a flat laptop-style monitor foot, a classic mouse shape with the wire looping at the back.
const B = () => {
  const cream = '#FFE9D6';
  const phone = `<rect x="122" y="74" width="86" height="208" rx="22" fill="#F58CA0"/><rect x="129" y="86" width="72" height="184" rx="14" fill="${INK}"/><circle cx="165" cy="96" r="3" fill="#5B4650"/>` +
    eyes(165, 140, 19, 6.5, cream) + smile(165, 156, 8, cream);
  const monitor = `<path d="M246 252H278L286 276H238Z" fill="#E9A07C"/><rect x="226" y="274" width="72" height="8" rx="4" fill="#E9A07C"/><rect x="200" y="160" width="124" height="96" rx="14" fill="#FFB892"/><rect x="207" y="167" width="110" height="72" rx="8" fill="${INK}"/><circle cx="262" cy="248" r="2.5" fill="#E9A07C"/>` +
    eyes(262, 200, 18, 5.5, cream) + smile(262, 214, 7, cream);
  const mouse = `<path d="M276 250C262 250 262 226 280 224" fill="none" stroke="#FF9F8A" stroke-width="5" stroke-linecap="round"/><path d="M274 282C270 240 292 214 322 214C352 214 374 240 370 282Z" fill="#FF9F8A"/>` +
    `<path d="M322 216V240M280 244C300 252 344 252 364 244" fill="none" stroke="#E5806C" stroke-width="3" stroke-linecap="round"/><rect x="316" y="226" width="12" height="20" rx="6" fill="#E5806C"/>` +
    eyes(322, 262, 15, 5, INK) + smile(322, 271, 6, INK);
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 400 300">${defs}${ground}${phone}${monitor}${cloud(92, .72)}${mouse}</svg>`;
};
fs.writeFileSync(path.join(outDir, 'draftA.svg'), A());
fs.writeFileSync(path.join(outDir, 'draftB.svg'), B());
const page = `<!doctype html><meta charset=utf-8><body style="margin:0;background:#1E1416;color:#F6E9EA;font:22px system-ui,'Microsoft JhengHei',sans-serif;width:1700px;overflow:hidden">
<div style="display:flex;gap:30px;padding:30px 30px 10px"><div style="flex:1"><div style="margin:0 0 8px 10px">A  淺色螢幕（臉畫在淡粉螢幕上）、圓頂滑鼠 + 滾輪 + 尾線</div>${A()}</div>
<div style="flex:1"><div style="margin:0 0 8px 10px">B  深色發光螢幕（臉是奶油色）、經典滑鼠形、線繞在後面</div>${B()}</div></div>
<div style="padding:0 40px 30px;font-size:18px;opacity:.8">後排手機（最高）→ 中排螢幕 → 前排 投投（左）與滑鼠（右），同一條地平線，臉都在遮擋之外。</div>`;
const pf = path.join(outDir, '_drafts.html');
fs.writeFileSync(pf, page);
const profileDir = path.join(os.tmpdir(), 'shell-0837-design-' + crypto.randomBytes(3).toString('hex'));
const br = await launch(profileDir);
const C = await cdpClient(br.ver.webSocketDebuggerUrl);
const {targetId} = await C.cdp('Target.createTarget', {url: 'about:blank'});
const {sessionId} = await C.cdp('Target.attachToTarget', {targetId, flatten: true});
await C.cdp('Emulation.setDeviceMetricsOverride', {width: 1700, height: 780, deviceScaleFactor: 1, mobile: false}, sessionId);
await C.cdp('Page.navigate', {url: 'file:///' + pf.replace(/\\/g, '/')}, sessionId);
await new Promise(r => setTimeout(r, 700));
const r = await C.cdp('Page.captureScreenshot', {format: 'png'}, sessionId);
fs.writeFileSync(path.join(outDir, 'drafts.png'), Buffer.from(r.data, 'base64'));
fs.rmSync(pf);
try { await C.cdp('Target.closeTarget', {targetId}); } catch {}
C.close();
await br.close();
try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
console.log('ok');
