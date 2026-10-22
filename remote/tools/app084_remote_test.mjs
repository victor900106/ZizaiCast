// 0.8.4 遠端 end to end inside the real app (node 22+): a --dev instance + one chrome-headless-shell as an iPhone.
//   node remote/tools/app084_remote_test.mjs <自在投影.exe> <outDir>
// The app: --dev --test-offscreen --test-no-network, its own data folder (PM_DEV_INSTANCE=remote084), input dry-run
// (PM_REMOTE_DRYRUN=1), silent synthetic sound (PM_REMOTE_SYNTHAUDIO=quiet), no Tailscale (PM_REMOTE_NOTS=1), loopback
// only (PM_REMOTE_LOOPBACK=1: 127.0.0.3, nothing on the LAN). A --dev instance never runs the tailscale CLI
// (remote_actions.cpp cliAllowed()), and this test never turns on 固定網址. Uploads go to the instance's own data
// folder (never Downloads). Driven with PhoneMirror.DevCommand (share/tools/dev_cmd.ps1). Closes only what it started.
// Checks: 允許 -> picture; the page's screen at 60 (?hz=60) with 每秒畫面 120 -> the app sends 60 and logs the phone's
// screen; the stats overlay's three layers; the 5-s report's fpsRecv / fpsDec / fpsShown / fpsAsk; the GPU figure
// 0..100 or absent; 畫質's line per choice; a 1.76 MB file over the files DataChannel (?fdc), byte-identical.
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import {launch, cdpClient, watchAltTab} from './quiet_browser.mjs';

const [exe, outDir] = process.argv.slice(2);
if (!exe || !outDir) { console.error('usage: node app084_remote_test.mjs <自在投影.exe> <outDir>'); process.exit(2); }
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
let passed = 0, failed = 0;
const check = (name, ok, detail = '') => { ok ? passed++ : failed++; console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail !== '' ? '  -- ' + String(detail).slice(0, 300) : ''}`); };
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const devCmdPs1 = path.resolve(here, '..', '..', 'share', 'tools', 'dev_cmd.ps1');

const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-remote084');
fs.mkdirSync(dataDir, {recursive: true});
for (const f of ['remote_devices.txt', 'phonemirror.log', 'phonemirror.old.log']) { try { fs.rmSync(path.join(dataDir, f)); } catch {} }
try { fs.rmSync(path.join(dataDir, 'uploads'), {recursive: true, force: true}); } catch {}
const token = crypto.randomBytes(24).toString('base64').replace(/[^A-Za-z0-9]/g, '').slice(0, 32).padEnd(32, 'x');
// never near the owner's Funnel targets (443 -> 127.0.0.1:8790, 8443 -> 127.0.0.2:8091): 18700..18899
const port = 18700 + Math.floor(Math.random() * 200);
fs.writeFileSync(path.join(dataDir, 'settings.ini'),
  `[settings]\nlanguage=zh-TW\nremote_token=${token}\nremote_port=${port}\nremote_idle_minutes=30\ntutorial_shown=1\ntray_hint_shown=1\nclose_hint_shown=1\ntoolbar_hint_shown=1\n`);
const appLog = path.join(dataDir, 'phonemirror.log');
const readLog = () => { try { return fs.readFileSync(appLog, 'utf8'); } catch { return ''; } };
const waitLog = async (re, ms = 10000) => { const end = Date.now() + ms; while (Date.now() < end) { const m = readLog().match(re); if (m) return m; await sleep(200); } return null; };

const br = await launch(path.join(os.tmpdir(), 'shell-084-app-' + crypto.randomBytes(3).toString('hex')), ['--allow-loopback-in-peer-connection', '--autoplay-policy=no-user-gesture-required']);
const watch = watchAltTab([br.proc.pid, process.pid], path.join(outDir, 'alttab_app.txt'));
const app = spawn(exe, ['--dev', '--test-offscreen', '--test-no-network'], {stdio: 'ignore', env: {...process.env, PM_VIDEO_OFFSCREEN: '1', PM_DEV_INSTANCE: 'remote084',
  PM_REMOTE_DRYRUN: '1', PM_REMOTE_SYNTHAUDIO: 'quiet', PM_REMOTE_TESTCLIP: '1', PM_REMOTE_NOTS: '1', PM_REMOTE_LOOPBACK: '1'}});
watch.add(app.pid);
console.log('app pid', app.pid, '(this test ends only this one)');
const dev = (cmd, lp = 0, wait = 600) => {
  try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmdPs1, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe'}); }
  catch (e) { console.log('dev cmd failed', cmd, String(e.stdout || e.message).trim()); }
};
const C = await cdpClient(br.ver.webSocketDebuggerUrl);
try {
  await sleep(4000);
  dev(330, 0, 1500);  // 開啟遠端操控
  const on = await waitLog(/remote: on at (http:\/\/[\d.]+:\d+\/\w+\/)/);
  check('remote control on (loopback 127.0.0.3)', on && /127\.0\.0\.3/.test(on[1]), on && on[1]);
  check('the --dev instance ran no tailscale CLI', !/funnel: tailscale funnel|tailscale.exe/.test(readLog()));
  const url = on[1];
  const {targetId} = await C.cdp('Target.createTarget', {url: 'about:blank'});
  const {sessionId} = await C.cdp('Target.attachToTarget', {targetId, flatten: true});
  const s = (m, p) => C.cdp(m, p, sessionId);
  const ev = async (expression, awaitPromise = false) => { const r = await s('Runtime.evaluate', {expression, returnByValue: true, awaitPromise}); return r && r.result ? r.result.value : undefined; };
  const until = async (fn, ms = 15000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await fn()) return true; await sleep(300); } return false; };
  const shot = async name => { const r = await s('Page.captureScreenshot', {format: 'png'}); if (r && r.data) fs.writeFileSync(path.join(outDir, name + '.png'), Buffer.from(r.data, 'base64')); };
  await s('Emulation.setDeviceMetricsOverride', {width: 956, height: 440, deviceScaleFactor: 3, mobile: true});
  await s('Emulation.setTouchEmulationEnabled', {enabled: true, maxTouchPoints: 5});
  await s('Emulation.setUserAgentOverride', {userAgent: 'Mozilla/5.0 (iPhone; CPU iPhone OS 26_1 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.1 Mobile/15E148 Safari/604.1', platform: 'iPhone'});
  await s('Page.navigate', {url: url + '?hz=60&fdc'});
  check('the phone waits for 允許', await until(async () => (await ev('S.state')) === 'pending'));
  await sleep(800);
  dev(961, 3, 800);  // 允許 + 記住
  check('允許 -> the picture plays', await until(() => ev('!!(v.videoWidth && S.state === "connected")'), 25000));
  await ev('setFps(120); prefs.hud = true; $("swHud").checked = true; $("swHud").onchange(); 1');
  const capLine = await waitLog(/picture -> [^\n]*@60 fps \(asked 60, [^\n]*phone screen 60 Hz/, 8000);
  check('每秒畫面 120 on a screen that draws 60: the app sends 60 and logs the phone\'s screen', !!capLine, capLine && capLine[0]);
  await sleep(7000);
  const hudTxt = await ev('hud.textContent');
  check('stats overlay: received / decoded / shown and the layer', /收到 \d+ · 解碼 \d+ · 顯示 \d+ 張\/秒/.test(hudTxt), hudTxt.split('\n').slice(0, 2).join(' | '));
  const rep = await waitLog(/page-report [^\n]*"fpsRecv":[\d.]+[^\n]*/, 12000);
  check('the 5-s report: fpsRecv / fpsDec / fpsShown / dispHz 60 / fpsAsk 120 / fpsSent 60', rep && /"fpsDec":[\d.]+/.test(rep[0]) && /"fpsShown":[\d.]+/.test(rep[0]) && /"dispHz":60/.test(rep[0]) && /"fpsAsk":120/.test(rep[0]) && /"fpsSent":60/.test(rep[0]),
    rep && rep[0].match(/"fpsRecv[^}]*?fpsSent":\d+/) ? rep[0].match(/"fpsRecv[^}]*?fpsSent":\d+/)[0] : '');
  const gpuLines = [...readLog().matchAll(/GPU (?:3D )?([\d.]+)%/g)].map(m => +m[1]);
  check('GPU figures in the app log all within 0..100 %', gpuLines.every(x => x >= 0 && x <= 100), `${gpuLines.length} figures, max ${gpuLines.length ? Math.max(...gpuLines) : '-'}`);
  const g = await ev('S.srv ? [S.srv.gpu3d, S.srv.gpuEnc] : null');
  check('the stats message: gpu3d / gpuEnc 0..100 or absent', Array.isArray(g) && g.every(x => x === undefined || x === null || (x >= 0 && x <= 100)), JSON.stringify(g));
  await shot('app_phone_hud');
  await ev(`$('bSet').onclick(); 1`); await sleep(400);
  const subs = [];
  for (const q of [0, 1, 2]) { await ev(`setQuality(${q}); 1`); await sleep(1800); subs.push(await ev(`$('lQSub').textContent`)); }
  check('畫質\'s line differs per choice', new Set(subs).size === 3 && /^流暢/.test(subs[0]) && /^標準/.test(subs[1]) && /^清晰/.test(subs[2]), subs.join(' / '));
  check('每秒畫面\'s line says the phone draws 60 and how to get 120 (iPhone)', /每秒只畫 60 次/.test(await ev(`$('lFpsSub').textContent`)) && /Prefer Page Rendering Updates near 60fps/.test(await ev(`$('lFpsSub').textContent`)));
  await ev(`$('segFps').closest('.row').scrollIntoView({block: 'center'}); 1`); await sleep(300);
  await shot('app_phone_settings_fps');
  await ev('closeSheets(); 1');
  // a file over the files DataChannel
  const f = path.join(os.tmpdir(), 'IMG_9574.png'), b = crypto.randomBytes(1845494);
  Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]).copy(b, 0);
  fs.writeFileSync(f, b);
  await ev(`$('bFile').onclick(); 1`); await sleep(500);
  const doc = await s('DOM.getDocument', {depth: 1});
  const qn = await s('DOM.querySelector', {nodeId: doc.root.nodeId, selector: '#upIn'});
  await s('DOM.setFileInputFiles', {nodeId: qn.nodeId, files: [f]});
  await ev(`$('upIn').dispatchEvent(new Event('change')); 1`);
  check('the file arrives', await until(() => ev('window.__pmUp.files >= 1'), 20000));
  const last = await ev('window.__pmUp.list[window.__pmUp.list.length - 1]');
  check('... over the direct DataChannel', last && last.via === 'dc', JSON.stringify(last));
  const saved = path.join(dataDir, 'uploads', 'IMG_9574.png');
  check('... saved byte for byte in the instance\'s own folder', fs.existsSync(saved) && crypto.createHash('sha256').update(fs.readFileSync(saved)).digest('hex') === crypto.createHash('sha256').update(b).digest('hex'));
  check('... the log says time / speed / path', !!(await waitLog(/files: "IMG_9574\.png" from [^\n]* saved \([\d.]+ MB in [\d.]+ s, [\d.]+ MB\/s, direct DataChannel\)/, 4000)));
  await shot('app_phone_files');
  try { fs.rmSync(f); } catch {}
  await C.cdp('Target.closeTarget', {targetId});
} catch (e) { check('ran', false, String(e && e.stack || e)); }
dev(330, 0, 1200);  // off
dev(104, 0, 500);   // 結束 (dev command), then make sure
for (let i = 0; i < 60 && app.exitCode === null; i++) await sleep(250);
const quitClean = app.exitCode !== null;
if (!quitClean) { try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} }
check('the test\'s app instance quit by itself (結束)', quitClean, `exit ${app.exitCode}`);
check('no tailscale CLI run in the whole log', !/funnel: tailscale funnel|Funnel port 8443 removed/.test(readLog()));
fs.copyFileSync(appLog, path.join(outDir, 'app084.log'));
C.close();
await br.close();
const alt = await watch.stop();
check('no window in Alt+Tab / the taskbar (app offscreen, headless shell)', alt.max === 0, `max ${alt.max} over ${alt.samples} samples`);
console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
