// 0.8.3.7: the login page's cast (投投 + phone / monitor / mouse): states, faces, layout, timing, idle cost, reduced motion, safety (node 22+).
//   node remote/tools/login_pals_test.mjs <outDir> <before|after> [--web <remote/web dir>]
// The page is served by a tiny local http server on 127.0.0.1 (a MOCK of /login: "right-pw" -> ok, "locked-pw" -> locked, "slow-pw" -> wrong after
// 2 s, anything else -> wrong with 4 tries left; no PC, no network, no real input: the typing / clicks are injected into the page with CDP) and
// opened in ONE chrome-headless-shell (quiet_browser.mjs: no window, no Alt+Tab entry, never the focus), closed at the end.
// "before": only idle / typed / wrong screenshots (a page without the cast); "after": every state, the checks. Recordings: login_pals_video.mjs.
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import http from 'node:http';
import crypto from 'node:crypto';
import {launch, cdpClient, watchAltTab} from './quiet_browser.mjs';

const args = process.argv.slice(2);
const [outDir, which] = args;
if (!outDir || !/^(before|after)$/.test(which || '')) { console.error('usage: node login_pals_test.mjs <outDir> <before|after> [--web dir]'); process.exit(2); }
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
      const send = (o, ms = 0) => setTimeout(() => { res.setHeader('content-type', 'application/json'); res.end(JSON.stringify(o)); }, ms);
      if (pw === 'right-pw') send({ok: true, token: 'tok'});
      else if (pw === 'locked-pw') send({locked: true, retryAfter: 900});
      else if (pw === 'slow-pw') send({left: 4}, 2000);
      else send({left: 4});
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

const profileDir = path.join(os.tmpdir(), 'shell-0837-login-' + crypto.randomBytes(3).toString('hex'));
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
const type = async (sel, text) => { await ev(`document.querySelector('${sel}').focus()`); await cdp('Input.insertText', {text}, sessionId); await sleep(80); };
const clear = async () => { await ev(`(() => { const i = document.getElementById('passIn'); i.value = ''; i.dispatchEvent(new Event('input', {bubbles: true})); i.blur(); document.activeElement && document.activeElement.blur(); })()`); await sleep(150); };
const open = async (vp, li, hash = '') => {
  await cdp('Emulation.setDeviceMetricsOverride', VP[vp], sessionId);
  await cdp('Page.navigate', {url: base + '?l=' + li + hash}, sessionId);
  await sleep(2300);
  await ev(`document.activeElement && document.activeElement.blur()`);
  await sleep(500);
};
const click = id => ev(`document.getElementById('${id}').click()`);
const waitState = async (want, ms = 3000) => { const t0 = Date.now(); let s = ''; while (Date.now() - t0 < ms) { s = await st(); if (s === want) return s; await sleep(40); } return s; };
const submit = async pw => { await type('#passIn', pw); await ev(`window.__shook = false; new MutationObserver(() => { if (document.getElementById('passIn').classList.contains('shake')) window.__shook = true; }).observe(document.getElementById('passIn'), {attributes: true})`); await click('passGo'); };
const faces = () => ev(`PALS.faces()`);
// every character has its own face in a state: the four expression strings differ
const own = async (name, expectState) => {
  const f = await faces();
  check(`${name}: four different faces`, new Set(f).size === 4 && f.every(Boolean), JSON.stringify(f));
};
// no face is covered by another character: each eye is topmost to its own character (back views and faces turned away are excepted)
const visible = async name => {
  const r = await ev(`(() => { const box = document.getElementById('pals'); box.style.pointerEvents = 'auto'; const bad = [];
    for (const c of PALS.faceCheck()) { const g = document.querySelector('#pals .ch[data-c="' + c.id + '"]'); if (g.classList.contains('back')) continue;
      for (const e of c.eyes) { const el = document.elementFromPoint(e[0], e[1]); const ch = el && el.closest && el.closest('.ch'); if (!ch || ch.dataset.c !== c.id) bad.push(c.id); } }
    box.style.pointerEvents = ''; return bad; })()`);
  check(`${name}: no face is covered by another character`, Array.isArray(r) && r.length === 0, JSON.stringify(r));
};

try {
  for (let li = 0; li < 4; li++) {
    for (const vp of Object.keys(VP)) {
      const tag = `${LN[li]}_${vp}`;
      await open(vp, li);
      await mouse(VP[vp].width * 0.78, VP[vp].height * 0.32); await sleep(450);
      await shot(`${tag}_01idle`);
      if (!after) {
        await type('#passIn', 'secret-typed'); await sleep(450); await shot(`${tag}_02typed`);
        await clear(); await submit('wrong-pw'); await sleep(500); await shot(`${tag}_03wrong`);
        continue;
      }
      check(`${tag}: idle`, (await st()) === 'idle', await st());
      await own(`${tag} idle`); await visible(`${tag} idle`);
      await ev(`document.getElementById('passIn').focus()`); await sleep(500);
      check(`${tag}: focus on the empty box -> they watch it`, (await st()) === 'user', await st());
      await shot(`${tag}_02watch`);
      await own(`${tag} watch`); await visible(`${tag} watch`);
      await type('#passIn', 'secret-typed'); await sleep(450);
      check(`${tag}: typing the password -> turning away`, (await st()) === 'pw', await st());
      await shot(`${tag}_03pw`);
      await own(`${tag} pw`); await visible(`${tag} pw`);
      const pk = await waitState('pw+peek', 3000);
      check(`${tag}: ... the phone bends over to peek at the dots`, pk === 'pw+peek', pk);
      await sleep(150);
      await shot(`${tag}_04peek`);
      await own(`${tag} peek`); await visible(`${tag} peek`);
      await waitState('pw', 3000);
      await click('pwEye'); await sleep(900);
      check(`${tag}: show password -> everybody looks away (and stays so)`, (await st()) === 'show', await st());
      await shot(`${tag}_05show`);
      await own(`${tag} show`); await visible(`${tag} show`);
      await sleep(1500);
      check(`${tag}: ... still looking away while the password is shown`, (await st()) === 'show', await st());
      await click('pwEye'); await sleep(700);
      check(`${tag}: hide password -> they turn back`, (await st()) === 'pw', await st());
      await clear();
      // the answer takes 2 s: a waiting face from 150 ms on, then the sad one
      await submit('slow-pw'); await sleep(700);
      check(`${tag}: waiting for the answer -> the nervous faces`, (await st()) === 'wait', await st());
      await shot(`${tag}_06wait`);
      await own(`${tag} wait`); await visible(`${tag} wait`);
      await waitState('sad', 3000); await sleep(650);
      await shot(`${tag}_07sad`);
      const e = await ev(`({err: document.getElementById('passErr').textContent, shake: String(window.__shook), shown: getComputedStyle(document.getElementById('pass')).display})`);
      check(`${tag}: the page's own error text is still shown, the box shook`, /4/.test(e.err) && e.shake === 'true' && e.shown === 'flex', JSON.stringify(e));
      check(`${tag}: wrong password -> sad`, (await st()) === 'sad', await st());
      await own(`${tag} sad`); await visible(`${tag} sad`);
      await submit('wrong-pw'); await sleep(600); await submit('wrong-pw'); await sleep(700);
      await shot(`${tag}_08sad3`);
      check(`${tag}: the third wrong password in a row -> bigger show`, (await ev(`PALS.faces()[2]`)).includes('wail'), await ev(`PALS.faces()[2]`));
      await own(`${tag} sad3`); await visible(`${tag} sad3`);
      await sleep(3000); await clear();
      await submit('locked-pw'); await sleep(900);
      await shot(`${tag}_09locked`);
      const l = await ev(`document.getElementById('passErr').textContent`);
      check(`${tag}: locked -> everybody asleep and the plain text`, (await st()) === 'lock' && /15/.test(l), (await st()) + ' / ' + l);
      await own(`${tag} lock`); await visible(`${tag} lock`);
      await clear();
      // a quick answer (here: instant) never shows the waiting faces
      await type('#passIn', 'right-pw');
      const ms = await ev(`new Promise(res => { const t0 = performance.now(), p = document.getElementById('pass'); const seen = new Set();
          const iv = setInterval(() => seen.add(PALS.state()), 8);
          new MutationObserver(() => { if (p.style.display === 'none') { clearInterval(iv); res({ms: performance.now() - t0, seen: [...seen]}); } }).observe(p, {attributes: true});
          document.getElementById('passGo').click(); setTimeout(() => { clearInterval(iv); res({ms: -1, seen: [...seen]}); }, 3000); })`);
      check(`${tag}: success -> happy, the page goes on within 300 ms, no waiting face for a quick answer`, ms && ms.ms > 0 && ms.ms < 300 && ms.seen.includes('happy') && !ms.seen.includes('wait'), JSON.stringify(ms));
    }
  }
  if (after) {
    // happy picture + faces (the real flow hides the page after 220 ms; here the hook is called alone)
    for (let li = 0; li < 4; li++) for (const vp of Object.keys(VP)) {
      await open(vp, li);
      await ev(`PALS.happy(); 0`); await sleep(750);
      await shot(`${LN[li]}_${vp}_10happy`);
      if (li === 1) { await own(`${LN[li]}_${vp} happy`); await visible(`${LN[li]}_${vp} happy`); }
    }
    // a guest address: the account box
    for (const vp of ['phone', 'pc']) {
      await open(vp, 1, '#u=guest-4821');
      await click('userNot'); await sleep(200);
      await type('#userIn', 'guest-77'); await sleep(500);
      await shot(`en_${vp}_11account`);
      check(`guest ${vp}: typing the account -> they lean in`, (await st()) === 'user', await st());
    }

    // layout: sizes and places
    for (const [w, h, name] of [[390, 844, 'portrait'], [390, 470, 'keyboard'], [360, 640, 'small'], [844, 390, 'landscape'], [1440, 900, 'pc'], [1024, 600, 'laptop']]) {
      await cdp('Emulation.setDeviceMetricsOverride', {width: w, height: h, deviceScaleFactor: 2, mobile: w < 900}, sessionId);
      await cdp('Page.navigate', {url: base + '?l=1'}, sessionId); await sleep(2300);
      const g = await ev(`(() => { const r = id => document.getElementById(id).getBoundingClientRect(), p = r('pals'), i = r('passIn'), b = r('passGo'), s = getComputedStyle(document.getElementById('pals')).display;
        return {pals: s, palsH: Math.round(p.height), palsTop: Math.round(p.top), palsRight: Math.round(p.right), palsBottom: Math.round(p.bottom), inTop: Math.round(i.top), inLeft: Math.round(i.left), goBottom: Math.round(b.bottom)}; })()`);
      const sideBySide = w >= 700 && w > h;
      check(`${name} ${w}x${h}: the password box and the button stay on screen`, g.inTop > 0 && g.goBottom <= h, JSON.stringify(g));
      if (name === 'portrait') check(`${name}: the cast takes 28-38 % of the screen height (keyboard closed)`, g.palsH / h >= .28 && g.palsH / h <= .38, `${(g.palsH / h * 100).toFixed(1)} %`);
      if (sideBySide) check(`${name}: the cast stands to the left of the form, inside the window`, g.pals !== 'none' && g.palsRight <= g.inLeft + 4 && g.palsTop >= 0 && g.palsBottom <= h, JSON.stringify(g));
      else check(`${name}: the cast above the form (or hidden when very short), never on the box`, g.pals === 'none' || (g.palsBottom <= g.inTop && g.palsTop >= 0), JSON.stringify(g));
      if (name === 'pc') check(`${name}: big on a computer (at least 440 px wide)`, g.palsH >= 330, JSON.stringify(g));
      await shot(`layout_${name}_${w}x${h}`);
    }

    // one pose in every character: the feet on one line
    await open('pc', 1);
    const gl = await ev(`(() => [...document.querySelectorAll('#pals .ch')].map(c => Math.round(Math.max(...[...c.querySelectorAll('.sh > *:not(.hand):not(.face)')].map(e => e.getBoundingClientRect().bottom)))))()`);
    check('the four bodies stand on one ground line (bottoms within 6 px, hands excluded)', Math.max(...gl) - Math.min(...gl) <= 6, JSON.stringify(gl));

    // idle cost: the animation loop only exists while something moves
    const samples = await ev(`new Promise(res => { let busy = 0, all = 0; const iv = setInterval(() => { all++; if (PALS.busy()) busy++; }, 40); setTimeout(() => { clearInterval(iv); res({busy, all}); }, 9000); })`);
    check('idle for 9 s: the loop runs only in short bursts (a little show now and then), not all the time', samples.busy / samples.all < .3 && samples.busy < samples.all, JSON.stringify(samples));
    await mouse(300, 300); await mouse(500, 400); await sleep(1500);
    // the theater is random: look for a quiet moment right after the pointer's last move
    const quiet = await ev(`new Promise(res => { let q = 0; const iv = setInterval(() => { if (!PALS.busy()) q++; }, 30); setTimeout(() => { clearInterval(iv); res(q); }, 700); })`);
    check('after the pointer stopped, the loop stops (no frames while nothing moves)', quiet > 8, String(quiet));

    // safety: the source never reads the password's content or length; the cast never talks to the server
    const src = fs.readFileSync(path.join(web, 'index.html'), 'utf8');
    const blk = src.slice(src.indexOf('<style id="palsCss">'), src.indexOf('<script>window.PM'));
    check('the cast reads only whether the box is empty (no .length of it, no charAt, no fetch / XHR / storage in its code)',
      !/value\.length|\.value\[|charAt|fetch\(|XMLHttpRequest|WebSocket|localStorage|sendBeacon/.test(blk), '');
    logins = [];
    await open('pc', 1);
    await submit('wrong-pw'); await sleep(300); await submit('wrong-pw'); await sleep(300);
    check('the number of requests to /login equals the number of submits (2)', logins.length === 2, String(logins.length));

    // reduced motion: static faces only
    await cdp('Emulation.setEmulatedMedia', {features: [{name: 'prefers-reduced-motion', value: 'reduce'}]}, sessionId);
    await open('pc', 1);
    const f0 = await ev(`PALS.frames()`);
    await submit('wrong-pw'); await sleep(400);
    const rm = await ev(`(() => { const c = getComputedStyle(document.querySelector('#pals .sh')), i = getComputedStyle(document.getElementById('passIn'));
      return {state: PALS.state(), anim: c.animationName, trans: getComputedStyle(document.querySelector('#pals .ch')).transitionDuration, inAnim: i.animationName, busy: PALS.busy()}; })()`);
    check('reduced motion: sad faces kept, no animation, no transition, no shake', rm.state === 'sad' && rm.anim === 'none' && /^0s/.test(rm.trans) && rm.inAnim === 'none', JSON.stringify(rm));
    await shot('en_pc_reduced_sad');
    await sleep(3500);  // the sad face's own hold ends (one pose change) before the idle is measured
    const rb = await ev(`new Promise(res => { const box = document.getElementById('pals'); let b = 0; const f1 = PALS.frames(); new MutationObserver(() => { if (box.classList.contains('blink')) b++; }).observe(box, {attributes: true}); setTimeout(() => res({b, f: PALS.frames() - f1}), 6500); })`);
    check('reduced motion: no blinking, no frames while idle', rb.b === 0 && rb.f === 0, JSON.stringify(rb));
    await cdp('Emulation.setEmulatedMedia', {features: []}, sessionId);
    void f0;
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
