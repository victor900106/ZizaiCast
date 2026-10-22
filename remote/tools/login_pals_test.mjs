// 0.8.3.6: the login page's little crowd (投投 + three friends): states, layout, timing, idle cost, reduced motion, safety (node 22+).
//   node remote/tools/login_pals_test.mjs <outDir> <before|after> [--web <remote/web dir>] [--frames]
// The page is served by a tiny local http server on 127.0.0.1 (a MOCK of /login: "right-pw" -> ok, "locked-pw" -> locked, anything
// else -> wrong with 4 tries left; no PC, no network, no real input: the typing / clicks are injected into the page with CDP) and
// opened in ONE chrome-headless-shell (quiet_browser.mjs: no window, no Alt+Tab entry, never the focus), closed at the end.
// "before": only idle / typed / wrong screenshots (the page without the crowd); "after": every state, the checks, the wrong-password frames.
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import http from 'node:http';
import crypto from 'node:crypto';
import {launch, cdpClient, watchAltTab} from './quiet_browser.mjs';

const args = process.argv.slice(2);
const [outDir, which] = args;
if (!outDir || !/^(before|after)$/.test(which || '')) { console.error('usage: node login_pals_test.mjs <outDir> <before|after> [--web dir] [--frames]'); process.exit(2); }
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const web = args.includes('--web') ? args[args.indexOf('--web') + 1] : path.resolve(here, '..', 'web');
const after = which === 'after';
const out = path.resolve(outDir, which);
fs.mkdirSync(out, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
let failed = 0, n = 0;
const check = (name, ok, detail = '') => { n++; if (!ok) failed++; console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail !== '' ? '  -- ' + detail : ''}`); };

const html = fs.readFileSync(path.join(web, 'index.html'), 'utf8');
let logins = [];
const srv = http.createServer((req, res) => {
  const u = new URL(req.url, 'http://x');
  if (req.method === 'POST' && u.pathname === '/login') {
    let b = ''; req.on('data', d => b += d); req.on('end', () => {
      const pw = (JSON.parse(b || '{}').password) || '';
      logins.push(pw);
      res.setHeader('content-type', 'application/json');
      if (pw === 'right-pw') res.end(JSON.stringify({ok: true, token: 'tok'}));
      else if (pw === 'locked-pw') res.end(JSON.stringify({locked: true, retryAfter: 900}));
      else res.end(JSON.stringify({left: 4}));
    });
    return;
  }
  if (u.pathname === '/') {
    res.setHeader('content-type', 'text/html; charset=utf-8');
    return res.end(html.replace('window.PM = /*PM_CONFIG*/{};', `window.PM = {fixed: true, lang: ${+(u.searchParams.get('l') || 0)}, audio: true};`));
  }
  if (/^\/(trackpad|vkbd)\.js$/.test(u.pathname)) { res.setHeader('content-type', 'text/javascript'); return res.end(fs.readFileSync(path.join(web, u.pathname.slice(1)))); }
  res.statusCode = u.pathname.startsWith('/k/') ? 401 : 404; res.end('');
});
await new Promise(r => srv.listen(0, '127.0.0.1', r));
const base = `http://127.0.0.1:${srv.address().port}/`;

const profileDir = path.join(os.tmpdir(), 'shell-0836-login-' + crypto.randomBytes(3).toString('hex'));
const br = await launch(profileDir);
const watch = watchAltTab([br.proc.pid, process.pid], path.join(out, 'alttab_login.txt'));
const C = await cdpClient(br.ver.webSocketDebuggerUrl);
const cdp = C.cdp;
const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
const {sessionId} = await cdp('Target.attachToTarget', {targetId, flatten: true});
const ev = async expression => {
  const r = await cdp('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true}, sessionId);
  if (r && r.exceptionDetails) console.log('page error:', (r.exceptionDetails.exception && r.exceptionDetails.exception.description || r.exceptionDetails.text || '').split('\n')[0]);
  return r && r.result ? r.result.value : undefined;
};
const shot = async name => { const r = await cdp('Page.captureScreenshot', {format: 'png'}, sessionId); if (r && r.data) fs.writeFileSync(path.join(out, name + '.png'), Buffer.from(r.data, 'base64')); };
const VP = {phone: {width: 390, height: 844, deviceScaleFactor: 2, mobile: true}, pc: {width: 1440, height: 900, deviceScaleFactor: 1, mobile: false}};
const LN = ['zh-TW', 'en', 'ja', 'ko'];
const mouse = (x, y) => cdp('Input.dispatchMouseEvent', {type: 'mouseMoved', x, y}, sessionId);
const st = () => ev(`window.PALS ? PALS.state() : 'none'`);
const type = async (sel, text) => { await ev(`document.querySelector('${sel}').focus()`); await cdp('Input.insertText', {text}, sessionId); await sleep(120); };
const clear = async () => { await ev(`(() => { const i = document.getElementById('passIn'); i.value = ''; i.dispatchEvent(new Event('input', {bubbles: true})); i.blur(); document.activeElement && document.activeElement.blur(); })()`); await sleep(80); };
const open = async (vp, li, hash = '') => {
  await cdp('Emulation.setDeviceMetricsOverride', VP[vp], sessionId);
  await cdp('Page.navigate', {url: base + '?l=' + li + hash}, sessionId);
  await sleep(700);
  await ev(`document.getElementById('passIn').blur()`);
  await sleep(150);
};
const submit = async pw => { await type('#passIn', pw);
  await ev(`window.__shook = false; new MutationObserver(() => { if (document.getElementById('passIn').classList.contains('shake')) window.__shook = true; }).observe(document.getElementById('passIn'), {attributes: true})`); await ev(`document.getElementById('passGo').click()`); };

try {
  for (let li = 0; li < 4; li++) {
    for (const vp of Object.keys(VP)) {
      const tag = `${LN[li]}_${vp}`;
      await open(vp, li);
      await mouse(VP[vp].width * 0.78, VP[vp].height * 0.32); await sleep(350);
      await shot(`${tag}_1idle`);
      if (after) check(`${tag}: idle`, (await st()) === 'idle', await st());
      await type('#passIn', 'secret-typed'); await sleep(450);
      await shot(`${tag}_2typed`);
      if (after) {
        check(`${tag}: typing the password -> looking away`, (await st()) === 'pw', await st());
        await ev(`document.getElementById('pwEye').click()`); await sleep(260);
        check(`${tag}: show password -> a peek`, (await st()) === 'pw+peek', await st());
        await shot(`${tag}_3peek`);
        await sleep(1000);
        check(`${tag}: ... then looks away again`, (await st()) === 'pw', await st());
        await ev(`document.getElementById('pwEye').click()`);
      }
      await clear();
      await submit('wrong-pw'); await sleep(330);
      await shot(`${tag}_${after ? '4sad' : '3wrong'}`);
      if (after) {
        check(`${tag}: wrong password -> sad`, (await st()) === 'sad', await st());
        const e = await ev(`({err: document.getElementById('passErr').textContent, shake: String(window.__shook), shown: getComputedStyle(document.getElementById('pass')).display})`);
        check(`${tag}: the page's own error text is still shown, the box shakes`, /4/.test(e.err) && e.shake === 'true' && e.shown === 'flex', JSON.stringify(e));
        await sleep(2500);
        check(`${tag}: ... the face recovers`, (await st()) === 'idle', await st());
        await submit('locked-pw'); await sleep(400);
        await shot(`${tag}_5locked`);
        const l = await ev(`document.getElementById('passErr').textContent`);
        check(`${tag}: locked -> its own face and the plain text`, (await st()) === 'lock' && /15/.test(l), (await st()) + ' / ' + l);
        await clear();
        // success: the face, at most 300 ms of delay from the answer
        await type('#passIn', 'right-pw');
        const ms = await ev(`new Promise(res => { const t0 = performance.now(), p = document.getElementById('pass'); let seen = '';
          new MutationObserver(() => { if (p.style.display === 'none') res({ms: performance.now() - t0, seen}); }).observe(p, {attributes: true});
          document.getElementById('passGo').click(); setTimeout(() => { seen = PALS.state(); }, 60); setTimeout(() => res({ms: -1, seen}), 3000); })`);
        check(`${tag}: success -> happy, the page goes on within 300 ms`, ms && ms.ms > 0 && ms.ms < 300 && ms.seen === 'happy', JSON.stringify(ms));
      }
    }
  }
  if (after) {
    // happy picture (the real flow hides the page after 180 ms; here the face is held by calling the hook only)
    for (let li = 0; li < 4; li++) for (const vp of Object.keys(VP)) {
      await open(vp, li);
      await ev(`PALS.happy(); 0`); await sleep(120);
      await shot(`${LN[li]}_${vp}_6happy`);
    }
    // a guest address: the account box -> they lean at it
    await open('phone', 1, '#u=guest-4821');
    await ev(`document.getElementById('userNot').click()`); await sleep(200);
    await type('#userIn', 'guest-77'); await sleep(450);
    await shot('en_phone_7account');
    check('guest: typing the account -> leaning in', (await st()) === 'user', await st());
    await open('pc', 1, '#u=guest-4821');
    await ev(`document.getElementById('userNot').click()`); await sleep(200);
    await type('#userIn', 'guest-77'); await sleep(450);
    await shot('en_pc_7account');

    // the show-password peek, frame by frame
    if (args.includes('--frames')) {
      const fdir = path.join(out, 'frames_wrong'); fs.rmSync(fdir, {recursive: true, force: true}); fs.mkdirSync(fdir, {recursive: true});
      await open('phone', 1);
      await type('#passIn', 'wrong-pw');
      await ev(`document.getElementById('passGo').click()`);
      for (let i = 0; i < 28; i++) {
        const r = await cdp('Page.captureScreenshot', {format: 'png', clip: {x: 0, y: 190, width: 390, height: 440, scale: 1}}, sessionId);
        fs.writeFileSync(path.join(fdir, `f${String(i).padStart(2, '0')}.png`), Buffer.from(r.data, 'base64'));
        await sleep(45);
      }
    }

    // layout: the phone with a keyboard up (about 470 px of height), and small landscape
    for (const [w, h, name] of [[390, 844, 'portrait'], [390, 470, 'keyboard'], [360, 640, 'small'], [844, 390, 'landscape'], [1440, 900, 'pc']]) {
      await cdp('Emulation.setDeviceMetricsOverride', {width: w, height: h, deviceScaleFactor: 2, mobile: w < 900}, sessionId);
      await cdp('Page.navigate', {url: base + '?l=1'}, sessionId); await sleep(600);
      const g = await ev(`(() => { const r = id => document.getElementById(id).getBoundingClientRect(), p = r('pals'), i = r('passIn'), b = r('passGo'), s = getComputedStyle(document.getElementById('pals')).display;
        return {pals: s, palsH: Math.round(p.height), inTop: Math.round(i.top), goBottom: Math.round(b.bottom), palsTop: Math.round(p.top)}; })()`);
      check(`${name} ${w}x${h}: the password box and the button stay on screen, crowd above`, g.inTop > 0 && g.goBottom <= h && (g.pals === 'none' || g.palsTop >= 0), JSON.stringify(g));
      await shot(`layout_${name}_${w}x${h}`);
    }

    // idle cost: no requestAnimationFrame at all while nothing happens, a blink or two only
    await open('pc', 1);
    const raf = await ev(`new Promise(res => { let n = PALS.frames(); const o = window.requestAnimationFrame;
      const box = document.getElementById('pals'); let blinks = 0; new MutationObserver(() => { if (box.classList.contains('blink')) blinks++; }).observe(box, {attributes: true});
      setTimeout(() => { res({raf: PALS.frames() - n, blinks}); }, 7000); })`);
    check('idle for 7 s: the crowd requests no animation frame, it does blink', raf.raf === 0 && raf.blinks >= 1, JSON.stringify(raf));
    await mouse(300, 300); await mouse(500, 400); await sleep(300);
    const raf2 = await ev(`new Promise(res => { const n = PALS.frames(); setTimeout(() => res(PALS.frames() - n), 1500); })`);
    check('1.5 s after the last pointer move: no more frames requested', raf2 === 0, String(raf2));

    // safety: the source never reads the password's content or length; the crowd never talks to the server
    const src = fs.readFileSync(path.join(web, 'index.html'), 'utf8');
    const blk = src.slice(src.indexOf('<style id="palsCss">'), src.indexOf('<script>window.PM'));
    check('the crowd reads only whether the box is empty (no .length, no charAt, no fetch / XHR / storage in its code)',
      !/value\.length|\.value\[|charAt|fetch\(|XMLHttpRequest|WebSocket|localStorage|sendBeacon/.test(blk), '');
    logins = [];
    await open('pc', 1);
    await submit('wrong-pw'); await sleep(200); await submit('wrong-pw'); await sleep(200);
    check('the number of requests to /login equals the number of submits (2)', logins.length === 2, String(logins.length));

    // reduced motion: static faces only
    await cdp('Emulation.setEmulatedMedia', {features: [{name: 'prefers-reduced-motion', value: 'reduce'}]}, sessionId);
    await open('pc', 1);
    await submit('wrong-pw'); await sleep(250);
    const rm = await ev(`(() => { const c = getComputedStyle(document.querySelector('#pals .ch')), i = getComputedStyle(document.getElementById('passIn'));
      return {state: PALS.state(), anim: c.animationName, trans: c.transitionDuration, inAnim: i.animationName}; })()`);
    check('reduced motion: sad face kept, no animation, no transition, no shake', rm.state === 'sad' && rm.anim === 'none' && /^0s/.test(rm.trans) && rm.inAnim === 'none', JSON.stringify(rm));
    await shot('en_pc_reduced_sad');
    const rb = await ev(`new Promise(res => { const box = document.getElementById('pals'); let b = 0; new MutationObserver(() => { if (box.classList.contains('blink')) b++; }).observe(box, {attributes: true}); setTimeout(() => res(b), 6500); })`);
    check('reduced motion: no blinking', rb === 0, String(rb));
    await cdp('Emulation.setEmulatedMedia', {features: []}, sessionId);
  }
} catch (e) { check('ran', false, String(e && e.stack || e)); }
try { await cdp('Target.closeTarget', {targetId}); } catch {}
const alt = await watch.stop();
check('no window in Alt+Tab / the taskbar during the test', alt && alt.samples > 0 && alt.max === 0, JSON.stringify(alt));
C.close();
await br.close();
srv.close();
try { fs.rmSync(profileDir, {recursive: true, force: true}); } catch {}
console.log(failed ? `FAILED (${failed} of ${n} failed)` : `ALL PASSED (${n})`);
process.exit(failed ? 1 : 0);
