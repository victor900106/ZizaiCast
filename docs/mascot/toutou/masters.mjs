import { readFileSync } from 'node:fs';
import { renderHtml, page } from './render.mjs';
const r = process.argv[2] || '0';
const m = ['A', 'B', 'C'].map((d) => `<div style="width:620px;height:480px;background:#2A1C1F;border-radius:14px;display:inline-block;margin:6px">${readFileSync(`svg/${d}_master.svg`, 'utf8').replace('<svg ', '<svg width="620" height="480" ')}</div>`).join('');
renderHtml(page(m, '#555'), `work/masters_${r}.png`, 1920, 500, 1);
