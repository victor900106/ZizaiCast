// 0.8.3.5 e2e ③: 臨時帳號 (可操作) 傳檔 both ways, byte-exact (SHA-256), and the greyed button when it may not.
//   node remote/tools/e2e0835/s3_files.mjs <自在投影.exe> <outDir>
// The released app as a --dev instance: uploads go to <outDir>/uploads_files (PM_REMOTE_UPLOADDIR, never the
// owner's Downloads); the PC's "copied files" are made-up test files (PM_REMOTE_TESTFILES + the test
// clipboard, never the real clipboard). The phone = ONE chrome-headless-shell tab (390x844 mobile): upload =
// the page's own <input type=file> (CDP DOM.setFileInputFiles) and its uploader; download = a click on the
// file's link in the 檔案 sheet, saved by the browser (Browser.setDownloadBehavior) into <outDir>/downloads.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {startApp, startBrowser, results, sleep, connectedJs, sha256} from './lib.mjs';

const [exe, outDir] = process.argv.slice(2);
if (!exe || !outDir) { console.error('usage: node s3_files.mjs <自在投影.exe> <outDir>'); process.exit(2); }
const R = results(outDir);
const {check} = R;
const ev3 = {sha: {}};
// test files (random bytes / text; nothing of the owner's)
const pcDir = path.join(outDir, 'pc_files'), phoneDir = path.join(outDir, 'phone_files'), dlDir = path.join(outDir, 'downloads');
for (const d of [pcDir, phoneDir, dlDir]) { fs.rmSync(d, {recursive: true, force: true}); fs.mkdirSync(d, {recursive: true}); }
const mk = (dir, name, bytes) => { const p = path.join(dir, name); fs.writeFileSync(p, bytes); return p; };
const up1 = mk(phoneDir, 'phone_photo_9MB.bin', crypto.randomBytes(9 * 1048576 + 12345));
const up2 = mk(phoneDir, '手機筆記 測試.txt', Buffer.from('臨時帳號上傳測試 0.8.3.5\n' + crypto.randomBytes(2000).toString('hex'), 'utf8'));
const dn1 = mk(pcDir, 'pc_report_6MB.bin', crypto.randomBytes(6 * 1048576 + 777));
const dn2 = mk(pcDir, '電腦文件 下載.txt', Buffer.from('電腦端下載測試\n' + crypto.randomBytes(1500).toString('hex'), 'utf8'));
const A = await startApp(exe, outDir, {tag: 'files', env: {PM_REMOTE_TESTFILES: [dn1, dn2].join(';')}});
check('app up, remote on, fixed address (made up) ready', !!A.lanUrl && A.fixedReady);
const B = await startBrowser(outDir, 'files');
B.watch.add(A.pid);
let T = await B.tab({w: 390, h: 844, dpr: 3, mobile: true});
async function login(name, pw) {
  await B.cdp('Storage.clearDataForOrigin', {origin: A.fixedOrigin, storageTypes: 'local_storage,session_storage'});
  await T.nav('about:blank'); await sleep(300);
  await T.nav(`${A.fixedOrigin}/#u=${name}`);
  await T.waitFor(() => T.ev(`(typeof $ === 'function' && $('pass') && $('pass').style.display === 'flex') ? 1 : 0`), 12000);
  await T.ev(`(() => { if ($('userIn')) $('userIn').value = ${JSON.stringify(name)}; $('passIn').value = ${JSON.stringify(pw)}; submitPass(); return 1; })()`);
  const asked = await A.answer(1, 15000);
  const ok = await T.waitFor(() => T.ev(connectedJs), 30000);
  await sleep(1200);
  await T.ev(`(() => { if ($('modeHint')) $('modeHint').style.display = 'none'; return 1; })()`);
  return asked && !!ok;
}
const btn = () => T.ev(`(() => { const b = $('bFile'), p = $('pbFile'); return {off: b.classList.contains('off'), aria: b.getAttribute('aria-disabled'), pbOff: p ? p.classList.contains('off') : null, pbTitle: p ? p.title : null, op: getComputedStyle(b).opacity, filesWhy: S.filesWhy || '', viewOnly: !!S.viewOnly, upload: S.upload}; })()`);

try {
  const G = await A.makeGuest(1 | 8, 's3_guest_panel_default.png');      // 1 小時, 可操作, 可以傳檔
  const Gno = await A.makeGuest(1);                                      // 1 小時, 可操作, 不能傳檔
  const Gview = await A.makeGuest(1 | 4);                                // 1 小時, 僅觀看
  A.dev(986, 0, 400);
  ev3.accounts = (A.readLog().match(/dev guests: [^\n]*/g) || []).slice(-1)[0];
  check('three accounts made (可操作+傳檔 / 可操作 不傳檔 / 僅觀看)', !!(G.name && Gno.name && Gview.name), ev3.accounts);

  // ---- 可操作 + 傳檔 ----
  check('G (可操作+傳檔): logged in after 允許', await login(G.name, G.pw));
  let b = await btn();
  check('G: 檔案 button on (not greyed)', !b.off && b.aria === 'false' && b.pbOff === false, JSON.stringify(b));
  await T.ev(`(() => { $('bFile').click(); return 1; })()`);
  const list = await T.waitFor(() => T.ev(`(() => { const a = [...document.querySelectorAll('#dlList a.nm')]; return a.length >= 2 ? a.map(x => ({n: x.textContent, href: x.getAttribute('href')})) : null; })()`), 10000);
  check('G: 檔案 sheet open, the PC\'s two copied files listed for download', !!list && list.length === 2, JSON.stringify(list && list.map(x => x.n)));
  await T.shot(path.join(outDir, 's3_files_sheet.png'));
  // upload through the page's own file input
  {
    const doc = await T.s('DOM.getDocument', {depth: 0});
    const q = await T.s('DOM.querySelector', {nodeId: doc.root.nodeId, selector: '#upIn'});
    const t0 = Date.now();
    await T.s('DOM.setFileInputFiles', {nodeId: q.nodeId, files: [up1, up2]});
    await T.ev(`(() => { const i = $('upIn'); if (i.files.length && !window.__chg) { window.__chg = 1; } return i.files.length; })()`);
    const done = await T.waitFor(() => T.ev(`(window.__pmUp && (__pmUp.files + __pmUp.errors) >= 2) ? JSON.stringify(__pmUp) : null`), 60000);
    ev3.upStat = done; ev3.upMs = Date.now() - t0;
    const rows = await T.ev(`[...document.querySelectorAll('#upList li')].map(li => li.textContent)`);
    ev3.upRows = rows;
    await sleep(800);
    const saved = fs.readdirSync(A.uploadDir);
    ev3.saved = saved;
    for (const src of [up1, up2]) {
      const nm = path.basename(src);
      const got = saved.find(f => f === nm) || saved.find(f => f.startsWith(path.parse(nm).name));
      const a = sha256(src), bsha = got ? sha256(path.join(A.uploadDir, got)) : '';
      ev3.sha['up ' + nm] = {phone: a, pc: bsha, bytes: fs.statSync(src).size, savedAs: got || null};
      check(`上傳 ${nm} (${(fs.statSync(src).size / 1048576).toFixed(2)} MB): saved on the PC, SHA-256 equal`, !!got && a === bsha, `${a.slice(0, 16)}… vs ${bsha.slice(0, 16)}…`);
    }
    check('upload: the page says done for both, no errors', !!done && JSON.parse(done).errors === 0 && JSON.parse(done).files === 2, String(done));
    const savedLines = A.readLog().match(/remote: file from the phone saved: [^\n]*/g) || [];
    check('PC log: both uploads saved ("file from the phone saved")', savedLines.length === 2, savedLines.join(' | '));
    await T.shot(path.join(outDir, 's3_upload_done.png'));
  }
  // download: a click on each link, saved by the browser
  {
    await B.cdp('Browser.setDownloadBehavior', {behavior: 'allow', downloadPath: dlDir, eventsEnabled: true});
    await T.ev(`(() => { $('dlRefresh').click(); return 1; })()`);
    await T.waitFor(() => T.ev(`document.querySelectorAll('#dlList a.nm:not(.used)').length >= 2 ? 1 : 0`), 8000);
    const n = await T.ev(`(() => { const a = [...document.querySelectorAll('#dlList a.nm')]; a.forEach((x, i) => setTimeout(() => x.click(), i * 1500)); return a.length; })()`);
    const want = [dn1, dn2].map(p => path.basename(p));
    const got = await T.waitFor(() => { const f = fs.readdirSync(dlDir).filter(x => !/\.crdownload$/.test(x)); return want.every(w => f.includes(w)) ? f : null; }, 40000);
    await sleep(800);
    ev3.downloaded = fs.readdirSync(dlDir);
    for (const src of [dn1, dn2]) {
      const nm = path.basename(src), p = path.join(dlDir, nm);
      const a = sha256(src), bsha = fs.existsSync(p) ? sha256(p) : '';
      ev3.sha['down ' + nm] = {pc: a, phone: bsha, bytes: fs.statSync(src).size};
      check(`下載 ${nm} (${(fs.statSync(src).size / 1048576).toFixed(2)} MB): saved by the browser, SHA-256 equal`, a === bsha, `${a.slice(0, 16)}… vs ${bsha.slice(0, 16) || 'missing'}`);
    }
    check(`download: ${n} links clicked, both files complete`, !!got);
    // one use: the same link again is refused
    const again = await T.ev(`(async () => { const a = document.querySelector('#dlList a.nm'); const r = await fetch(a.href); return r.status; })()`, true);
    check('a download link works once (the same link again is refused)', again >= 400, String(again));
  }
  await T.ev(`(() => { closeSheets(); return 1; })()`);

  // ---- 可操作, 不能傳檔: greyed, says why, the PC refuses too ----
  await T.close(); T = await B.tab({w: 390, h: 844, dpr: 3, mobile: true});
  check('Gno (可操作, 不能傳檔): logged in after 允許', await login(Gno.name, Gno.pw));
  b = await btn();
  check('Gno: 檔案 button greyed (class off, aria-disabled, PC bar too)', b.off && b.aria === 'true' && b.pbOff === true && +b.op < 1, JSON.stringify(b));
  const why = await T.ev(`(() => { $('toast').textContent = ''; $('bFile').click(); return {sheet: $('fileS').style.display, toast: $('toast').textContent, want: t('filesNoGuest')}; })()`);
  check('Gno: a tap says why in plain words, the sheet stays closed', why.sheet !== 'block' && why.toast === why.want, why.toast);
  await sleep(300);
  await T.shot(path.join(outDir, 's3_no_files_greyed.png'));
  const refused = await T.ev(`(async () => { const r = await post('up?id=' + encodeURIComponent(sessId) + '&f=00112233aabbccdd&n=x.bin&o=0&t=4', new Blob([new Uint8Array(4)])); let j = null; try { j = await r.json(); } catch {} return {status: r.status, j}; })()`, true);
  check('Gno: the PC refuses an upload sent anyway (file_perm on the host)', refused && refused.status >= 400 && !fs.existsSync(path.join(A.uploadDir, 'x.bin')), JSON.stringify(refused));

  // ---- 僅觀看 ----
  await T.close(); T = await B.tab({w: 390, h: 844, dpr: 3, mobile: true});
  check('Gview (僅觀看): logged in after 允許', await login(Gview.name, Gview.pw));
  b = await btn();
  const why2 = await T.ev(`(() => { $('toast').textContent = ''; $('bFile').click(); return {sheet: $('fileS').style.display, toast: $('toast').textContent, want: t('filesDenied')}; })()`);
  check('Gview: 檔案 greyed, a tap says 僅觀看 cannot send files', b.off && b.viewOnly && why2.sheet !== 'block' && why2.toast === why2.want, `${JSON.stringify(b)} ${why2.toast}`);
  await sleep(300);
  await T.shot(path.join(outDir, 's3_viewonly_greyed.png'));
} catch (e) { check('s3 ran without an exception', false, String(e && e.stack || e)); }
try { await T.close(); } catch {}
const alt = await B.close();
const st = await A.stop();
check('app quit', !st.killed, JSON.stringify(st));
check(`Alt+Tab / taskbar windows of the test's processes = 0 (${alt.samples} samples)`, alt.samples > 0 && alt.max === 0, alt.seen.join(' ; '));
R.save('s3_files.json', {evidence: ev3});
fs.writeFileSync(path.join(outDir, 'sha256.txt'), Object.entries(ev3.sha).map(([k, v]) => `${k}\n  ${JSON.stringify(v)}`).join('\n') + '\n');
console.log(R.failed() ? `FAILED (${R.failed()})` : 'ALL PASSED');
process.exit(R.failed() ? 1 : 0);
