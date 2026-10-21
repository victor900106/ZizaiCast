// Tutorial screenshots (feeds: render feed/*.html with Chrome headless 720x1560 and encode as in README; put
// phone_en.h264 / phone_ja.h264 and dummy Report.pdf / app-demo.apk into UX_WORK/feed)
// Tutorial screenshots for one UI language -> _shots/tut/<lang>/ (made-up data only).
import {run, sleep, WORK, REPO} from './ux_run.mjs';
import {browser} from './cdp.mjs';
import {execFileSync} from 'node:child_process';
import http from 'node:http';
import crypto from 'node:crypto';
import fs from 'node:fs';
const lang = process.argv[2] || 'zh-TW';
const only = process.argv[3] || 'abcdefg';
const OUT = `${REPO}/_shots/tut/${lang}`;
fs.mkdirSync(OUT, {recursive: true});
const FEED = `${WORK}/feed/phone_en.h264`, FEED_TR = lang === 'en' ? `${WORK}/feed/phone_ja.h264` : FEED;
const stroke = (pid, a) => { try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', new URL('./stroke.ps1', import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'), '-ProcessId', String(pid), ...a], {stdio: 'pipe'}); } catch {} };
const salt = crypto.randomBytes(16);
const pwHash = `pbkdf2-sha256$310000$${salt.toString('hex')}$${crypto.pbkdf2Sync('Zizai-Test-2026', salt, 310000, 32, 'sha256').toString('hex')}`;
const pwSeed = `remote_pw_hash=${pwHash}\nremote_fixed_secret=${crypto.randomBytes(32).toString('hex')}\n`;
const o = {outDir: OUT, lang};

if (only.includes('a')) await run({...o, tag: 'a', steps: async ({dev, grab, win}) => {   // idle + pairing
  dev(950, 1, 900); await win('idle');
  dev(127, 0, 2500); dev(902, 0, 1200); grab('pair');
}});
if (only.includes('b')) await run({...o, tag: 'b', args: ['--test-feed', FEED], steps: async ({dev, grab, win, pid}) => {  // iPhone
  await sleep(2500);
  dev(964, 0, 300); dev(964, 3, 700); await win('toolbar');
  dev(107, 0, 1000); await win('snapshot');
  dev(112, 0, 2500); await win('recording'); dev(112, 0, 1500);
  dev(400, 0, 1200);
  stroke(pid, ['-X0', '0.22', '-Y0', '0.30', '-X1', '0.80', '-Y1', '0.31']); await sleep(400);
  dev(406, 0, 600);  // 黃 (second colour)
  dev(401, 0, 400);
  stroke(pid, ['-X0', '0.15', '-Y0', '0.55', '-X1', '0.85', '-Y1', '0.56']); await sleep(600);
  await win('ink');
  dev(400, 0, 800);
  dev(160, 0, 2500); dev(903, 0, 1000); grab('picker');
  dev(161, 0, 3000); dev(903, 0, 1000); grab('share'); await win('share-window');
  dev(164, 0, 600);
}});
if (only.includes('c')) await run({...o, tag: 'c', args: ['--test-feed', FEED_TR], steps: async ({dev, win}) => {  // translate
  await sleep(2500);
  dev(191, 0, 4000); await win('translate');
}});
if (only.includes('d')) await run({...o, tag: 'd', args: ['--test-feed', FEED, '--test-source', 'android'],
  env: {PM_DROP_TEST: `${WORK}/feed/Report.pdf|${WORK}/feed/app-demo.apk`, PM_DROP_FAKE_OK: '1'}, steps: async ({dev, grab, win}) => {  // Android
  await sleep(2500);
  dev(964, 0, 300); dev(964, 3, 700); await win('android');
  dev(976, 1, 900); await win('drop');
  dev(976, 0, 300);
}});
if (only.includes('e')) await run({...o, tag: 'e', steps: async ({dev, grab, win}) => {  // remote at home
  dev(383, 0, 1200); await win('remote-off');
  dev(359, 0, 3500); await win('remote-lan');
}});
if (only.includes('f')) await run({...o, tag: 'f', seed: pwSeed, steps: async ({dev, grab, win, readLog}) => {  // fixed address + phone login page
  dev(383, 0, 1200); dev(330, 0, 3000); dev(356, 0, 1000); await sleep(6000); await win('remote-fixed');
  const fixed = (readLog().match(/listening on (127\.0\.0\.2:\d+)/) || [])[1];
  const bl = {'zh-TW': 'zh-TW', en: 'en-US', ja: 'ja-JP', ko: 'ko-KR'}[lang];
  if (fixed) {
    const b = await browser();
    try {
      await b.send('Network.enable'); await b.send('Network.setExtraHTTPHeaders', {headers: {'Accept-Language': bl}});
      await b.send('Emulation.setLocaleOverride', {locale: bl}).catch(() => {});
      await b.go(`http://${fixed}/`, 3000); await b.shot(`${OUT}/f_phone-login.png`);
    } finally { await b.close(); }
  }
}});
if (only.includes('g')) {  // update dialog (made-up 0.8.4 on 127.0.0.1)
  const manifest = JSON.stringify({version: '0.8.4', url: 'http://127.0.0.1:1/x.exe', sha256: '0'.repeat(64), size: 52428800, date: '2026-11-01', notes: '-',
    changes_zh: ['範例：新功能一', '範例：修正二'], changes_en: ['Example: new feature one', 'Example: fix two'], changes_ja: ['例：新機能 1', '例：修正 2'], changes_ko: ['예: 새 기능 1', '예: 수정 2']});
  const srv = http.createServer((q, s) => { s.writeHead(200, {'content-type': 'application/json'}); s.end(manifest); }).listen(0, '127.0.0.1');
  await new Promise(r => srv.on('listening', r));
  await run({...o, tag: 'g', args: ['--test-no-install'], seed: `update_url=http://127.0.0.1:${srv.address().port}/update.json\n`, steps: async ({dev, grab}) => {
    dev(125, 0, 3500); dev(904, 0, 1200); grab('update');
  }});
  srv.close();
}
