// Replaces the tutorial's screenshot placeholders (<!-- SCREENSHOT: Sxx ... --> + the
// grey <div class="shot">) with the real screenshots, embedded as JPEG data URIs (the
// tutorial stays one file; the phone page serves it at /help too). Placeholders
// without a screenshot (S07 / S08: the phones' own Share / menu sheets) stay.
//   node embed_shots.mjs <evidence dir>
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {execFileSync} from 'node:child_process';

const [ev] = process.argv.slice(2);
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const root = path.resolve(here, '..', '..');
const docs = {'zh-TW': '自在投影教學.html', en: 'ZizaiCast-Guide.html', ja: 'ZizaiCast-Guide-ja.html', ko: 'ZizaiCast-Guide-ko.html'};
const map = L => ({
  S01: [`ui/${L}_after_1-cast_window.png`, 420],
  S02: [`ui/${L}_after_2-remote-off_window.png`, 420],
  S03: [`ui/${L}_after_3-remote-on-lanqr_window.png`, 420],
  S04: [`ui/${L}_after_6-password_remote_password.png`, 460],
  S05: [`ui/${L}_after_10-fixed-ready_window.png`, 420],
  S06: [`app-fixed-${L}/phone-1-password.png`, 330],
  S09: [`app-tunnel-${L}/3-ready_remote_panel_tun.png`, 460],
  S10: [`app-fixed-${L}/phone-3b-settings.png`, 330],
  S11: [`ui/${L}_after_8-pw-set-off_window.png`, 420],
  S12: [`app-fixed-${L}/5-connected_remote_banner.png`, 640],
});
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'shots-'));
const report = [];
for (const [L, file] of Object.entries(docs)) {
  const f = path.join(root, 'docs', 'tutorial', file);
  let s = fs.readFileSync(f, 'utf8');
  for (const [id, [rel, maxW]] of Object.entries(map(L))) {
    const png = path.join(ev, rel);
    const re = new RegExp(`<!-- SCREENSHOT: ${id} [^>]*-->\\r?\\n<div class="shot" role="img" aria-label="([^"]*)"><b>[^<]*</b><span>([^<]*)</span></div>`);
    const m = s.match(re);
    if (!m) { report.push(`${L} ${id}: no placeholder`); continue; }
    if (!fs.existsSync(png)) { report.push(`${L} ${id}: no screenshot (${rel}) - placeholder kept`); continue; }
    const jpg = path.join(tmp, `${L}-${id}.jpg`);
    execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', path.join(here, 'shot_jpeg.ps1'), '-In', png, '-Out', jpg, '-MaxW', String(maxW * 2)]);
    const b64 = fs.readFileSync(jpg).toString('base64');
    const fig = `<figure class="real-shot" style="margin:14px auto;text-align:center"><img src="data:image/jpeg;base64,${b64}" alt="${m[1]}" style="max-width:100%;width:${maxW}px;border-radius:12px;box-shadow:0 4px 18px rgba(0,0,0,.18)"><figcaption style="font-size:13px;opacity:.75;margin-top:6px">${m[2]}</figcaption></figure>`;
    s = s.replace(re, () => fig);
    report.push(`${L} ${id}: ${rel} (${Math.round(b64.length / 1024)} KB)`);
  }
  fs.writeFileSync(f, s);
}
fs.rmSync(tmp, {recursive: true, force: true});
console.log(report.join('\n'));
