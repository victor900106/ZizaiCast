// 0.8.3: replaces the app screenshots embedded in the four tutorial files (figures 1-5, 7, 9, 10 of
// the 遠端控制 section; 6 and 8 are the phone's own page and stay) with the ones tools/shell_shots.mjs
// took (made-up addresses only: 203.0.113.x / example hosts). Same JPEG embedding as
// remote/tools/embed_shots.mjs (remote/tools/shot_jpeg.ps1).
//   node tools/embed_shots_083.mjs <shots dir (the --tunnel run's output)>
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {execFileSync} from 'node:child_process';

const [dir] = process.argv.slice(2);
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const root = path.resolve(here, '..');
const docs = {'zh-TW': '自在投影教學.html', en: 'ZizaiCast-Guide.html', ja: 'ZizaiCast-Guide-ja.html', ko: 'ZizaiCast-Guide-ko.html'};
const map = L => ({
  1: [`${L}_a_01-cast_window.png`, 420],
  2: [`${L}_a_04-remote-off_window.png`, 420],
  3: [`${L}_a_05-remote-on-lanqr_window.png`, 420],
  4: [`${L}_a_07-password_remote_password.png`, 460],
  5: [`${L}_b_12-fixed-ready_window.png`, 420],
  7: [`${L}_b_14-tunnel_remote_panel_tun.png`, 460],
  9: [`${L}_b_10-pw-set-off_window.png`, 420],
  10: [`${L}_b_16-banner_remote_banner.png`, 640],
});
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'shots083-'));
const report = [];
for (const [L, file] of Object.entries(docs)) {
  const f = path.join(root, 'docs', 'tutorial', file);
  let s = fs.readFileSync(f, 'utf8');
  const re = /<figure class="real-shot" style="[^"]*"><img src="data:image\/jpeg;base64,([^"]+)"/g;
  let k = 0;
  const m = map(L);
  s = s.replace(re, (all, b64) => {
    k++;
    const e = m[k];
    if (!e) return all;
    const png = path.join(dir, e[0]);
    if (!fs.existsSync(png)) { report.push(`${L} #${k}: missing ${e[0]} (kept)`); return all; }
    const jpg = path.join(tmp, `${L}-${k}.jpg`);
    execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', path.join(root, 'remote', 'tools', 'shot_jpeg.ps1'), '-In', png, '-Out', jpg, '-MaxW', String(e[1] * 2)]);
    const nb = fs.readFileSync(jpg).toString('base64');
    report.push(`${L} #${k}: ${e[0]} (${Math.round(nb.length / 1024)} KB)`);
    return all.replace(b64, nb);
  });
  report.push(`${L}: ${k} figures`);
  fs.writeFileSync(f, s);
}
fs.rmSync(tmp, {recursive: true, force: true});
console.log(report.join('\n'));
