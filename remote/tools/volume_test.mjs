// 0.8.3.4: the phone page's 手機音量 offline, in chrome-headless-shell (quiet_browser.mjs: no window, no
// Alt+Tab entry, never the focus; the watcher's count is printed and must be 0). No sound is played:
// OfflineAudioContext, --mute-audio, nothing reaches a speaker.
//   node remote/tools/volume_test.mjs <outDir>          (PM_VOLP='{"preDb":12,...}' overrides pmVolume.P: tuning only)
// remote/web/index.html is opened as a file (no server); its own window.pmVolume builds the chain.
// Signals (synthetic, 48 kHz):
//   speech  speech-like noise bursts: 2 s loud at -14 dBFS RMS (what the PC's AGC sends since 0.8.3.4),
//           2 s quiet at -34 dBFS RMS, x2. Measured: RMS of the loud / quiet stretches (after 0.5 s), peak.
//   music   dense music-like noise (pink-ish, 4 Hz beat, crest ~12 dB) at -14 dBFS RMS: the overall loudness.
//   sine    1 kHz at -14 dBFS RMS and 100 Hz at -8 dBFS RMS: THD+N at each position (破音 = distortion).
// The 0.8.3.3 chain (gain x percent, then a -2 dB / 20:1 compressor) at 100-400 % for comparison.
// Exit 1 if: 100 % is not transparent (+-0.5 dB); the slider is not louder at every step (music RMS);
// 400 % is not at least 5 dB louder than 100 % on music, 12 dB on quiet speech, 1.5 dB on loud speech
// (it is at full scale already at 100 %: more would only come from clipping it); any sample reaches
// 0.99; THD+N above 2 % (1 kHz, pushed 15 dB into the limiter at 400 %) / 5 % (100 Hz, hot) anywhere;
// a test window seen in Alt+Tab.
import fs from 'node:fs';
import path from 'node:path';
import {launch, cdpClient, watchAltTab} from './quiet_browser.mjs';

const [outDir = path.join(process.env.TEMP, 'pm-volume-test')] = process.argv.slice(2);
fs.mkdirSync(outDir, {recursive: true});
const HERE = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const web = path.resolve(HERE, '..', 'web');
const work = path.join(outDir, '_page');
fs.rmSync(work, {recursive: true, force: true});
fs.mkdirSync(work, {recursive: true});
for (const f of fs.readdirSync(web)) fs.copyFileSync(path.join(web, f), path.join(work, f));
const sleep = ms => new Promise(r => setTimeout(r, ms));
const br = await launch(path.join(work, '_shell'));
const watch = watchAltTab([br.proc.pid], path.join(outDir, 'alttab_volume.txt'));
const C = await cdpClient(br.ver.webSocketDebuggerUrl);
const {targetId} = await C.cdp('Target.createTarget', {url: 'about:blank'});
const {sessionId} = await C.cdp('Target.attachToTarget', {targetId, flatten: true});
const send = (method, params = {}) => C.raw(method, params, sessionId);
const POS = [25, 50, 75, 100, 150, 200, 250, 300, 350, 400];
let rows = [], err = null;
try {
  await send('Page.enable');
  await send('Page.navigate', {url: `file:///${work.replace(/\\/g, '/')}/index.html`});
  await sleep(2500);
  const r = await send('Runtime.evaluate', {awaitPromise: true, returnByValue: true, expression: `(async () => {
    const override = ${process.env.PM_VOLP || 'null'};
    if (override) Object.assign(window.pmVolume.P, override);
    const R = 48000, N = 8 * R;
    let seed = 7; const rnd = () => (seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296 * 2 - 1;
    const rms = (a, s = 0, e = a.length) => { let q = 0; for (let i = s; i < e; i++) q += a[i] * a[i]; return Math.sqrt(q / (e - s)); };
    const db = x => 20 * Math.log10(Math.max(x, 1e-9));
    const peakOf = a => { let p = 0; for (const x of a) p = Math.max(p, Math.abs(x)); return p; };
    // speech-like: noise through a 300-3000 Hz shape, syllables at ~4 Hz, scaled per stretch
    const speech = new Float32Array(N); { let lp = 0, hp = 0;
      for (let i = 0; i < N; i++) { const x = rnd(); lp += 0.3 * (x - lp); hp = lp - 0.97 * hp; const env = Math.max(0, Math.sin(2 * Math.PI * 4 * i / R)) ** 1.5; speech[i] = (lp - hp * 0.2) * env; } }
    const loud = t => Math.floor(t / 2) % 2 === 0;
    for (let k = 0; k < 4; k++) { const s = k * 2 * R, e = s + 2 * R, want = loud(k * 2) ? -14 : -34; const g = 10 ** (want / 20) / rms(speech, s, e); for (let i = s; i < e; i++) speech[i] *= g; }
    for (let i = 0; i < N; i++) speech[i] = Math.max(-0.891, Math.min(0.891, speech[i]));
    // music-like: pink-ish noise (Paul Kellet) + a 55 Hz bass line, a beat every 0.25 s, -14 dBFS RMS, peak <= 0.891
    const music = new Float32Array(N); { let b0 = 0, b1 = 0, b2 = 0;
      for (let i = 0; i < N; i++) { const w = rnd(); b0 = 0.99765 * b0 + w * 0.099; b1 = 0.963 * b1 + w * 0.2965; b2 = 0.57 * b2 + w * 1.0527;
        const ph = (i / R) % 0.25, beat = 0.55 + 0.45 * Math.exp(-ph * 14);
        music[i] = ((b0 + b1 + b2 + w * 0.1848) * 0.12 + 0.35 * Math.sin(2 * Math.PI * 55 * i / R) * Math.exp(-ph * 6)) * beat; }
      const g = 10 ** (-14 / 20) / rms(music); for (let i = 0; i < N; i++) music[i] = Math.max(-0.891, Math.min(0.891, music[i] * g)); }
    const sine = (f, dbRms) => { const a = new Float32Array(N), A = 10 ** (dbRms / 20) * Math.SQRT2; for (let i = 0; i < N; i++) a[i] = A * Math.sin(2 * Math.PI * f * i / R); return a; };
    const s1k = sine(1000, -14), s100 = sine(100, -8);
    // THD+N: least-squares fit of the fundamental over the last 2 s (whole cycles), the residual vs the fit
    const thd = (out, f) => { const s = 6 * R, e = 8 * R; let ss = 0, cc = 0, sc = 0, ys = 0, yc = 0;
      for (let i = s; i < e; i++) { const w = 2 * Math.PI * f * i / R, a = Math.sin(w), b = Math.cos(w); ss += a * a; cc += b * b; sc += a * b; ys += out[i] * a; yc += out[i] * b; }
      const det = ss * cc - sc * sc, A = (ys * cc - yc * sc) / det, B = (yc * ss - ys * sc) / det; let res = 0, fun = 0;
      for (let i = s; i < e; i++) { const w = 2 * Math.PI * f * i / R, fit = A * Math.sin(w) + B * Math.cos(w); res += (out[i] - fit) ** 2; fun += fit * fit; }
      return 100 * Math.sqrt(res / Math.max(fun, 1e-12)); };
    const render = async (input, wire) => {
      const ctx = new OfflineAudioContext(1, N, R);
      const b = ctx.createBuffer(1, N, R); b.copyToChannel(input, 0);
      const src = ctx.createBufferSource(); src.buffer = b;
      wire(ctx, src); src.start(0);
      return (await ctx.startRendering()).getChannelData(0);
    };
    const measure = async wire => {
      const sp = await render(speech, wire), mu = await render(music, wire), a = await render(s1k, wire), b = await render(s100, wire);
      const L = [], Q = [];
      for (let k = 0; k < 4; k++) { const s = k * 2 * R + R / 2, e = (k + 1) * 2 * R; (loud(k * 2) ? L : Q).push(db(rms(sp, s, e))); }
      const avg = x => x.reduce((p, c) => p + c, 0) / x.length;
      return {loud: avg(L), quiet: avg(Q), music: db(rms(mu, R, N)), peak: Math.max(peakOf(sp), peakOf(mu), peakOf(a), peakOf(b)), thd1k: thd(a, 1000), thd100: thd(b, 100)};
    };
    const rows = [{chain: 'input', pos: '-', loud: -14, quiet: -34, music: db(rms(music, R, N)), peak: Math.max(peakOf(speech), peakOf(music)), thd1k: 0, thd100: 0}];
    for (const pct of [100, 200, 300, 400]) rows.push({chain: '0.8.3.3', pos: pct, ...await measure((ctx, src) => {
      const g = ctx.createGain(); g.gain.value = pct / 100;
      const lim = ctx.createDynamicsCompressor(); lim.threshold.value = -2; lim.knee.value = 0; lim.ratio.value = 20; lim.attack.value = 0.002; lim.release.value = 0.12;
      src.connect(g); g.connect(lim); lim.connect(ctx.destination); })});
    for (const pos of ${JSON.stringify(POS)}) rows.push({chain: '0.8.3.4', pos, ...await measure((ctx, src) => {
      const c = window.pmVolume.build(ctx); c.set(pos, 0); src.connect(c.in); c.out.connect(ctx.destination); })});
    return {rows, P: window.pmVolume.P}; })()`});
  if (!r.result || !r.result.result || r.result.exceptionDetails) err = JSON.stringify(r.result && r.result.exceptionDetails || r).slice(0, 800);
  else rows = r.result.result.value.rows, console.log('pmVolume.P', JSON.stringify(r.result.result.value.P));
} finally {
  C.close();
  await br.close();
}
const alt = await watch.stop();
try { fs.rmSync(work, {recursive: true, force: true}); } catch {}
console.log(`Alt+Tab / taskbar windows of the test's processes: max ${alt.max} over ${alt.samples} samples${alt.seen.length ? ' (' + alt.seen.join(' ; ') + ')' : ''}`);
if (err) { console.log('FAILED: the page threw: ' + err); process.exit(1); }
const f = x => (x >= 0 ? '+' : '') + x.toFixed(1);
const lines = ['chain    slider   speech loud  speech quiet  music RMS   peak   THD+N 1k  THD+N 100Hz(hot)'];
for (const x of rows) lines.push(`${x.chain.padEnd(8)} ${String(x.pos === '-' ? '-' : x.pos + '%').padEnd(8)} ${f(x.loud).padStart(11)}  ${f(x.quiet).padStart(12)}  ${f(x.music).padStart(9)}  ${x.peak.toFixed(3)}  ${x.thd1k.toFixed(2).padStart(7)}%  ${x.thd100.toFixed(2).padStart(9)}%`);
const by = p => rows.find(x => x.chain === '0.8.3.4' && x.pos === p), inp = rows[0];
const fails = [];
if (alt.max !== 0 || alt.samples < 1) fails.push(`a test window was visible in Alt+Tab (max ${alt.max}, ${alt.samples} samples)`);
if (Math.abs(by(100).loud - inp.loud) > 0.5 || Math.abs(by(100).quiet - inp.quiet) > 0.5 || Math.abs(by(100).music - inp.music) > 0.5) fails.push('100 % is not transparent');
for (let i = 1; i < POS.length; i++) if (!(by(POS[i]).music > by(POS[i - 1]).music + 0.5)) fails.push(`${POS[i]} % is not louder than ${POS[i - 1]} % (music)`);
if (by(400).loud - by(100).loud < 1.5) fails.push('400 % is less than 1.5 dB louder than 100 % (loud speech)');
if (by(400).music - by(100).music < 5) fails.push('400 % is less than 5 dB louder than 100 % (music)');
if (by(400).quiet - by(100).quiet < 12) fails.push('400 % lifts quiet speech by less than 12 dB');
for (const x of rows.filter(x => x.chain === '0.8.3.4')) {
  if (x.peak >= 0.99) fails.push(`${x.pos} %: a sample reached ${x.peak.toFixed(3)}`);
  if (x.thd1k > 2) fails.push(`${x.pos} %: THD+N ${x.thd1k.toFixed(2)} % at 1 kHz`);
  if (x.thd100 > 5) fails.push(`${x.pos} %: THD+N ${x.thd100.toFixed(2)} % at 100 Hz (hot)`);
}
lines.push('', `400 % vs 100 %: loud speech ${f(by(400).loud - by(100).loud)} dB, quiet speech ${f(by(400).quiet - by(100).quiet)} dB, music ${f(by(400).music - by(100).music)} dB` +
  ` (0.8.3.3: ${f(rows[4].loud - rows[1].loud)} / ${f(rows[4].quiet - rows[1].quiet)} / ${f(rows[4].music - rows[1].music)} dB)`);
lines.push('', fails.length ? 'FAILED: ' + fails.join('; ') : 'all targets met');
fs.writeFileSync(path.join(outDir, 'volume_test.txt'), lines.join('\n') + '\n');
console.log(lines.join('\n'));
process.exit(fails.length ? 1 : 0);
