// Headless-only Chrome rendering helpers (no window ever shown).
import { writeFileSync, unlinkSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
const CHROME = 'C:/Program Files/Google/Chrome/Application/chrome.exe';
export function renderHtml(html, out, w, h, scale = 1) {
  const tmp = resolve(out + '.tmp.html');
  writeFileSync(tmp, html);
  execFileSync(CHROME, ['--headless=new', '--disable-gpu', '--hide-scrollbars', '--mute-audio',
    `--force-device-scale-factor=${scale}`, '--virtual-time-budget=4000',
    `--screenshot=${resolve(out)}`, `--window-size=${w},${h}`, '--default-background-color=00000000',
    pathToFileURL(tmp).href], { stdio: 'ignore' });
  unlinkSync(tmp);
}
export const page = (body, bg = 'transparent', css = '') =>
  `<!doctype html><html><head><meta charset="utf-8"><style>html,body{margin:0;background:${bg};font-family:"Noto Sans TC","Microsoft JhengHei UI","Microsoft JhengHei","Segoe UI",sans-serif}${css}</style></head><body>${body}</body></html>`;
export const svgUri = (svg) => 'data:image/svg+xml;base64,' + Buffer.from(svg).toString('base64');
