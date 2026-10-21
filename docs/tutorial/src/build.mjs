// Builds the four self-contained guides from src/<lang>.html (0.8.3 rewrite).
//   node docs/tutorial/src/build.mjs [imageDir]
// imageDir (default <repo>/_shots/tut/web) holds <lang>/<name>.webp: screenshots of an
// off-screen --dev instance with made-up data only (see README.md, "Screenshots").
// Markers in the sources:
//   {{fig:NAME}}               an SVG illustration, src/fig/<lang>-NAME.html; ja / ko fall back to
//                              the English one with its texts translated by src/fig/<lang>.json
//   {{img:NAME|caption|width}} a screenshot, embedded as WebP (missing: a grey placeholder)
//   {{todo:caption}}           a placeholder for a 0.8.3.2 screenshot (TODO-0832)
import fs from 'node:fs';
import path from 'node:path';

const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const repo = path.resolve(here, '..', '..', '..');
const imgDir = process.argv[2] || path.join(repo, '_shots', 'tut', 'web');
const mascot = 'data:image/png;base64,' + fs.readFileSync(path.join(repo, 'assets', 'public', 'toutou', 'toutou_340.png')).toString('base64');
const base = fs.readFileSync(path.join(here, 'base.css'), 'utf8') + fs.readFileSync(path.join(here, 'extra.css'), 'utf8');

const L = {
  zh: {file: '自在投影教學.html', html: 'zh-Hant', shots: 'zh-TW', title: '自在投影 使用教學',
    font: '"Microsoft JhengHei UI","Microsoft JhengHei","PingFang TC","Noto Sans TC","Segoe UI",sans-serif',
    svg: '"Microsoft JhengHei UI","Microsoft JhengHei","Segoe UI",sans-serif', todo: '截圖待補（0.8.3.2 合併後）', missing: '截圖待補'},
  en: {file: 'ZizaiCast-Guide.html', html: 'en', shots: 'en', title: 'Zizai Cast — User guide',
    font: '"Segoe UI","Microsoft JhengHei UI","Microsoft JhengHei","PingFang TC","Noto Sans TC",sans-serif',
    svg: '"Segoe UI","Microsoft JhengHei UI","Microsoft JhengHei",sans-serif', todo: 'Screenshot to come (after 0.8.3.2)', missing: 'Screenshot to come'},
  ja: {file: 'ZizaiCast-Guide-ja.html', html: 'ja', shots: 'ja', title: 'Zizai Cast 使い方ガイド',
    font: '"Yu Gothic UI","Meiryo","Segoe UI","Microsoft JhengHei UI","Microsoft JhengHei","PingFang TC","Noto Sans TC",sans-serif',
    svg: '"Yu Gothic UI","Meiryo","Segoe UI","Microsoft JhengHei UI","Microsoft JhengHei",sans-serif', todo: 'スクリーンショットは 0.8.3.2 の後で追加', missing: 'スクリーンショットは後で追加'},
  ko: {file: 'ZizaiCast-Guide-ko.html', html: 'ko', shots: 'ko', title: 'Zizai Cast 사용 가이드',
    font: '"Malgun Gothic","Segoe UI","Microsoft JhengHei UI","Microsoft JhengHei","PingFang TC","Noto Sans TC",sans-serif',
    svg: '"Malgun Gothic","Segoe UI","Microsoft JhengHei UI","Microsoft JhengHei",sans-serif', todo: '스크린샷은 0.8.3.2 이후 추가', missing: '스크린샷은 나중에 추가'},
};
const esc = s => s.replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;');

function fig(lang, name) {
  const own = path.join(here, 'fig', `${lang}-${name}.html`);
  if (fs.existsSync(own)) return fs.readFileSync(own, 'utf8');
  let s = fs.readFileSync(path.join(here, 'fig', `en-${name}.html`), 'utf8');
  const dict = JSON.parse(fs.readFileSync(path.join(here, 'fig', `${lang}.json`), 'utf8'));
  // longest first, only whole text nodes / attribute values
  for (const k of Object.keys(dict).sort((a, b) => b.length - a.length)) {
    const v = dict[k];
    s = s.split(`>${k}<`).join(`>${v}<`).split(`"${k}"`).join(`"${v}"`);
  }
  return s;
}

let missing = [];
for (const [lang, c] of Object.entries(L)) {
  if (!fs.existsSync(path.join(here, `${lang}.html`))) { console.log(`skip ${lang} (no src/${lang}.html)`); continue; }
  const src = fs.readFileSync(path.join(here, `${lang}.html`), "utf8");
  const body = src
    .replace(/\{\{fig:([\w-]+)\}\}/g, (_, n) => fig(lang, n))
    .replace(/\{\{img:([\w-]+)\|([^|}]*)(?:\|(\d+))?\}\}/g, (_, n, cap, w) => {
      const p = path.join(imgDir, c.shots, `${n}.webp`);
      if (!fs.existsSync(p)) {
        missing.push(`${lang}/${n}`);
        return `<!-- SCREENSHOT: ${n} -->\n<div class="shot" role="img" aria-label="${esc(cap)}"><b>${c.missing}</b><span>${cap}</span></div>`;
      }
      const data = fs.readFileSync(p).toString('base64');
      return `<figure class="real"><img src="data:image/webp;base64,${data}" alt="${esc(cap)}"${w ? ` style="width:${w}px"` : ''}><figcaption>${cap}</figcaption></figure>`;
    })
    .replace(/\{\{todo:([^}]*)\}\}/g, (_, cap) =>
      `<!-- TODO-0832: ${cap} -->\n<div class="shot todo" role="img" aria-label="${esc(cap)}"><b>TODO-0832 · ${c.todo}</b><span>${cap}</span></div>`);
  const css = base.replace('__MASCOT__', mascot).replace('__FONT__', c.font).replace('__SVGFONT__', c.svg);
  const html = `<!DOCTYPE html>\n<html lang="${c.html}">\n<head>\n<meta charset="utf-8">\n<meta name="viewport" content="width=device-width, initial-scale=1">\n<title>${c.title}</title>\n<style>${css}</style>\n</head>\n<body>\n${body}\n</body>\n</html>\n`;
  fs.writeFileSync(path.join(here, '..', c.file), html);
  console.log(c.file, Math.round(html.length / 1024), 'KB');
}
if (missing.length) console.log('placeholders (no image):', missing.join(', '));
