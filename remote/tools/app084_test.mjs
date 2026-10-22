// 0.8.4 (feat-084-privacy) 隱私黑屏 + 電腦的檔案 inside the real app (node 22+):
//   node app084_test.mjs <自在投影.exe> <outDir> [--shots-only]
// A --dev --test-offscreen --test-no-network instance with its own data folder (PM_DEV_INSTANCE=p084), input in
// dry-run (PM_REMOTE_DRYRUN=1: the real mouse / keyboard are never touched), loopback only (PM_REMOTE_LOOPBACK=1:
// 127.0.0.3, nothing on the LAN, no Tailscale), quiet synthetic sound, a test clipboard. Its 隱私黑屏 cover is the
// app's own exe as a child (--pm-privacy-mask) in the --dev off-screen mode (1x1 windows off every screen: the
// owner's screen is never covered); 電腦的檔案 sees only a sandbox (PM_REMOTE_PCFS_ROOTS). The phone is
// chrome-headless-shell (no window, own profile; never the owner's Chrome). No phone screenshot here (the app
// streams the real desktop); the menus are rendered as PNGs. Closes only what it started.
import {spawn, execFileSync} from 'node:child_process';
import {launch, cdpClient} from './quiet_browser.mjs';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';

const [exe, outDir, ...rest] = process.argv.slice(2);
const beforeExe = rest[0] === '--before' ? rest[1] : '';
if (!exe || !outDir) { console.error('usage: node app084_test.mjs <自在投影.exe> <outDir> [--before <old exe>]'); process.exit(2); }
fs.mkdirSync(outDir, {recursive: true});
const sleep = ms => new Promise(r => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const devCmdPs1 = path.resolve(here, '..', '..', 'share', 'tools', 'dev_cmd.ps1');
let pass = 0, fail = 0;
const check = (what, ok, detail) => { ok ? pass++ : fail++; console.log((ok ? 'PASS  ' : 'FAIL  ') + what + (detail !== undefined ? '  -- ' + JSON.stringify(detail) : '')); };
const shotsDir = path.join(os.tmpdir(), 'pmshots');
const pidAlive = pid => { try { return execFileSync('tasklist', ['/FI', `PID eq ${pid}`, '/NH'], {encoding: 'utf8'}).includes(' ' + pid + ' '); } catch { return false; } };

// ---- sandbox ----
const box = path.join(outDir, 'sandbox'), root = path.join(box, 'Root'), extra = path.join(box, 'Extra');
fs.rmSync(box, {recursive: true, force: true});
fs.mkdirSync(root, {recursive: true});
fs.mkdirSync(extra, {recursive: true});
fs.writeFileSync(path.join(root, 'report.txt'), 'from the app sandbox');

async function runApp(appExe, tag, body) {
  const dataDir = path.join(process.env.LOCALAPPDATA, 'PhoneMirror-dev-p084' + tag);
  fs.mkdirSync(dataDir, {recursive: true});
  for (const f of ['remote_devices.txt', 'phonemirror.log', 'phonemirror.old.log', 'remote_connections.log', 'remote_connections.old.log']) { try { fs.rmSync(path.join(dataDir, f)); } catch {} }
  const token = crypto.randomBytes(24).toString('base64').replace(/[^A-Za-z0-9]/g, '').slice(0, 32).padEnd(32, 'x');
  const port = 8700 + Math.floor(Math.random() * 200);
  fs.writeFileSync(path.join(dataDir, 'settings.ini'), `[settings]\nlanguage=zh-TW\nremote_token=${token}\nremote_port=${port}\nremote_idle_minutes=30\ntutorial_shown=1\n` +
    `tray_hint_shown=1\nclose_hint_shown=1\ntoolbar_hint_shown=1\nwhats_new_seen=9.9.9\n`);
  const appLog = path.join(dataDir, 'phonemirror.log');
  const readLog = () => { try { return fs.readFileSync(appLog, 'utf8'); } catch { return ''; } };
  const app = spawn(appExe, ['--dev', '--test-offscreen', '--test-no-network'], {stdio: 'ignore', env: {...process.env, PM_VIDEO_OFFSCREEN: '1',
    PM_DEV_INSTANCE: 'p084' + tag, PM_REMOTE_DRYRUN: '1', PM_REMOTE_SYNTHAUDIO: 'quiet', PM_REMOTE_TESTCLIP: '1', PM_REMOTE_NOTS: '1', PM_REMOTE_LOOPBACK: '1',
    PM_REMOTE_PCFS_ROOTS: 'test=' + root, PM_TEST_PCFILES_ADD: extra}});
  log(tag, 'app pid', app.pid);
  const dev = (cmd, lp = 0, wait = 600) => {
    try { execFileSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', devCmdPs1, '-ProcessId', String(app.pid), '-Cmd', String(cmd), '-Lp', String(lp), '-Wait', String(wait)], {stdio: 'pipe'}); }
    catch (e) { log('dev cmd failed', cmd, String(e.stdout || e.message).trim()); }
  };
  const waitLog = async (re, ms = 10000) => { const end = Date.now() + ms; while (Date.now() < end) { if (re.test(readLog())) return true; await sleep(200); } return false; };
  try { await body({app, dev, readLog, waitLog, dataDir}); }
  finally {
    dev(104, 0, 500);  // 結束
    for (let i = 0; i < 100 && app.exitCode === null; i++) await sleep(100);
    if (app.exitCode === null) { try { execFileSync('taskkill', ['/PID', String(app.pid), '/T', '/F'], {stdio: 'ignore'}); } catch {} log('killed own app', app.pid); }
  }
}
const takeShot = (src, dst) => { const p = path.join(shotsDir, src); if (fs.existsSync(p)) { fs.copyFileSync(p, path.join(outDir, dst)); fs.rmSync(p); return true; } return false; };

// ---- before: the 遠端操控 menu of the old app (every tray submenu, the remote one picked by size of its list) ----
if (beforeExe) {
  await runApp(beforeExe, 'b', async ({dev}) => {
    await sleep(4000);
    dev(330, 0, 1500);
    for (const f of fs.existsSync(shotsDir) ? fs.readdirSync(shotsDir).filter(f => f.startsWith('menu_')) : []) fs.rmSync(path.join(shotsDir, f));
    dev(901, 0, 2500);
    const subs = fs.readdirSync(shotsDir).filter(f => /^menu_sub\d+\.png$/.test(f));
    for (const f of subs) fs.copyFileSync(path.join(shotsDir, f), path.join(outDir, 'before_' + f));
    log('before menus:', subs.join(' '));
  });
}

await runApp(exe, 'a', async ({app, dev, readLog, waitLog, dataDir}) => {
  await sleep(4000);
  dev(330, 0, 1500);  // 開啟遠端操控
  const url = await waitLog(/remote: on at (http:\/\/\S+\/)/) && (readLog().match(/remote: on at (http:\/\/[\d.]+:\d+\/\w+\/)/) || [])[1];
  check('app: remote control on (loopback)', !!url && url.includes('127.0.0.3'), url);
  const prof = path.join(os.tmpdir(), 'hs-084-app-' + crypto.randomBytes(3).toString('hex'));
  const br = await launch(prof, ['--allow-loopback-in-peer-connection']);
  const {cdp, close} = await cdpClient(br.ver.webSocketDebuggerUrl);
  try {
    const {targetId} = await cdp('Target.createTarget', {url: 'about:blank'});
    const {sessionId: S} = await cdp('Target.attachToTarget', {targetId, flatten: true});
    await cdp('Emulation.setDeviceMetricsOverride', {width: 390, height: 844, deviceScaleFactor: 2, mobile: true}, S);
    const ev = async e => { const r = await cdp('Runtime.evaluate', {expression: e, returnByValue: true, awaitPromise: true}, S); return r && r.result ? r.result.value : undefined; };
    const until = async (fn, ms = 15000) => { const end = Date.now() + ms; while (Date.now() < end) { const v = await fn(); if (v) return v; await sleep(300); } return null; };
    await cdp('Page.navigate', {url}, S);
    await waitLog(/remote: .* asks to connect/);
    dev(961, 1, 800);  // 允許
    check('phone: connected after 允許', await until(() => ev('!!(v.videoWidth && S.state === "connected")'), 20000));
    await ev(`(() => { window.__m = []; const o = P84.onMsg; P84.onMsg = m => { window.__m.push(m); return o(m); }; P84.transfer.start = () => {}; return 1; })()`);
    check('phone: 黑幕 offered (Windows 10 2004+) and 電腦的檔案 on', await until(() => ev('P84.st.supported && P84.st.ok && P84.st.pcfs')));
    // 隱私黑屏 from the phone: the cover is this app's exe as a child, off-screen in --dev
    await ev(`$('bPriv').click(), 1`);
    const onLogged = await waitLog(/remote: 隱私黑屏 on \(phone\)/);
    const pid = +((readLog().match(/privacy: cover on \(pid (\d+), offscreen\)/) || [])[1] || 0);
    let cmdLine = '';
    try { cmdLine = execFileSync('powershell', ['-NoProfile', '-Command', `(Get-CimInstance Win32_Process -Filter "ProcessId=${pid}").CommandLine`], {encoding: 'utf8'}).trim(); } catch {}
    check('privacy on from the phone: the app shows it (menu state + log)', onLogged && await ev('P84.st.privacy'));
    check('the cover is the app exe itself (--pm-privacy-mask, off-screen in --dev)', pid && pidAlive(pid) && /--pm-privacy-mask \d+ 0 offscreen/.test(cmdLine), cmdLine);
    dev(8405, 0, 800);
    check('menu: 隱私黑屏 ✓ in the 遠端操控 menu (PNG)', takeShot('remote_menu_084.png', 'after_app_remote_menu_privacy_on.png') && takeShot('pcfiles_menu_084.png', 'after_app_pcfiles_menu.png'));
    dev(8401, 0, 1200);  // off from the PC's menu
    check('privacy off from the PC menu: the phone is told, the cover ends', await waitLog(/remote: 隱私黑屏 off \(pc\)/) && await until(async () => !(await ev('P84.st.privacy')), 5000) && await until(() => !pidAlive(pid), 4000));
    dev(8401, 1, 1200);  // on from the PC's menu
    check('privacy on from the PC menu', await waitLog(/remote: 隱私黑屏 on \(pc\)/) && await until(() => ev('P84.st.privacy'), 5000));
    const pid2 = +((readLog().match(/privacy: cover on \(pid (\d+)/g) || []).pop() || '').replace(/\D+/g, ' ').trim().split(' ').pop();
    // 電腦的檔案: the sandbox only, a download, the 連線紀錄
    await ev(`send({t: 'pcfs-roots'}), 1`);
    const roots = await until(() => ev(`(window.__m.filter(m => m.t === 'pcfs-roots').pop() || null)`), 5000);
    check('電腦的檔案: only the sandbox root (PM_REMOTE_PCFS_ROOTS)', roots && roots.roots.length === 1 && roots.roots[0].id === 'test', roots);
    await ev(`send({t: 'pcfs-get', root: 'test', items: ['report.txt', '../x', 'C:/Windows/win.ini']}), 1`);
    const keys = await until(() => ev(`(window.__m.filter(m => m.t === 'pcfs-keys').pop() || null)`), 5000);
    const k = keys && keys.list[0].k;
    const body = k && await ev(`fetch(BASE + 'dl/' + ${JSON.stringify(k)}).then(r => r.text())`);
    check('download: the file, and the escapes refused', body === 'from the app sandbox' && keys.list.slice(1).every(x => !x.k && x.err), keys && keys.list.map(x => x.p + ' ' + (x.k ? 'key' : x.err)));
    await waitLog(/remote: .* downloads pc "test\/report.txt"/, 5000);
    const conn = fs.readFileSync(path.join(dataDir, 'remote_connections.log'), 'utf8');
    check('連線紀錄: the download is a row (download, pc:test/report.txt)', /\tdownload\tpc:test\/report\.txt/.test(conn), conn.trim().split('\n').pop());
    dev(965, 1, 800);  // the 連線紀錄 as shown, into the log
    check('連線紀錄 as shown: 下載檔案 · test \\ report.txt', /下載檔案 · test \\ report\.txt/.test(readLog()));
    // 允許清單: 加入允許的資料夾… (the folder comes from PM_TEST_PCFILES_ADD in --dev: no dialog)
    dev(8402, 0, 800);
    const ini = fs.readFileSync(path.join(dataDir, 'settings.ini'), 'utf8');
    check('允許清單: the folder is saved (settings.ini remote_pc_files_extra)', ini.includes('remote_pc_files_extra=' + extra), (ini.match(/remote_pc_files_extra=.*/) || [''])[0]);
    dev(8405, 0, 800);
    takeShot('pcfiles_menu_084.png', 'after_app_pcfiles_menu_extra.png');
    takeShot('remote_menu_084.png', 'after_app_remote_menu.png');
    dev(8403, 0, 600);
    check('允許清單: removed again', !fs.readFileSync(path.join(dataDir, 'settings.ini'), 'utf8').includes('remote_pc_files_extra='));
    // 電腦的檔案 off at the PC: the phone loses it at once
    dev(8404, 0, 800);
    await ev(`send({t: 'pcfs-roots'}), 1`);
    const off = await until(() => ev(`(window.__m.filter(m => m.t === 'pcfs-err').pop() || null)`), 5000);
    check('電腦的檔案 off at the PC: refused at once (why: off), the tab goes', off && off.why === 'off' && await ev(`$('fsTabs').style.display === 'none'`), off);
    dev(8404, 1, 600);
    // 臨時帳號 switch (own checkbox in 電腦的檔案 ▸; default off; saved; the menu shows it)
    const iniG = () => fs.readFileSync(path.join(dataDir, 'settings.ini'), 'utf8');
    check('臨時帳號 switch: default off in settings.ini (remote_pc_files_guest=0)', /^remote_pc_files_guest=0$/m.test(iniG()), (iniG().match(/remote_pc_files_guest=.*/) || [''])[0]);
    dev(8405, 0, 800);
    check('menu PNG: 電腦的檔案 ▸ with the 臨時帳號 checkbox (off)', takeShot('pcfiles_menu_084.png', 'after_app_pcfiles_menu_guest_off.png') && takeShot('remote_menu_084.png', 'after_app_remote_menu_guest_off.png'));
    dev(8406, 1, 800);
    check('臨時帳號 switch on: saved (remote_pc_files_guest=1) and logged', /^remote_pc_files_guest=1$/m.test(iniG()) && /remote: 電腦的檔案 for 臨時帳號 on/.test(readLog()));
    dev(8405, 0, 800);
    check('menu PNG: the 臨時帳號 checkbox ticked', takeShot('pcfiles_menu_084.png', 'after_app_pcfiles_menu_guest_on.png'));
    takeShot('remote_menu_084.png', 'after_app_remote_menu_guest_on.png');
    dev(8406, 0, 800);
    check('臨時帳號 switch off again: saved (remote_pc_files_guest=0)', /^remote_pc_files_guest=0$/m.test(iniG()) && /remote: 電腦的檔案 for 臨時帳號 off/.test(readLog()));
    // 中斷 from the PC while the cover is on: it ends with the session
    dev(340, 0, 1500);  // 中斷目前的遠端連線
    check('中斷 at the PC: the cover ends with the session', await waitLog(/privacy: OFF \(ended\)/, 6000) && await until(() => !pidAlive(pid2), 4000), {pid2});
  } catch (e) {
    check('app test ran without an exception', false, String(e && e.stack || e));
  } finally {
    close();
    await br.close();
    try { fs.rmSync(prof, {recursive: true, force: true}); } catch {}
  }
});
fs.rmSync(box, {recursive: true, force: true});
console.log(`\nPASS ${pass} / FAIL ${fail}`);
process.exit(fail ? 1 : 0);
