import {browser} from './cdp.mjs';
const [file, out, w = '1280'] = process.argv.slice(2);
const b = await browser({width: +w, height: 900, dpr: 1, mobile: +w < 600});
try {
  await b.go('file:///' + file.split('\\').join('/'), 2500);
  await b.eval(`document.querySelectorAll('details').forEach(d => d.open = true); document.body.scrollHeight`);
  await b.shot(out, true);
} finally { await b.close(); }
