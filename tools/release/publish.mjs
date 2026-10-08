// GitHub API steps for publishing a 自在投影 / Zizai Cast release (no gh CLI needed;
// uses the git credential for github.com). Run from the repo root.
//   node tools/release/publish.mjs settings
//   node tools/release/publish.mjs drop-draft <tag>
//   node tools/release/publish.mjs release <version> <notes.md> <asset>...   (asset = path[=name])
//   node tools/release/publish.mjs pages
//   node tools/release/publish.mjs show
//   node tools/release/publish.mjs manifest <version> <installer.exe> <url> <changes.md> [out.json]
//        [--date YYYY-MM-DD] [--notes "one-line summary"]          (offline: no GitHub access)
import { execSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { readFileSync, statSync, writeFileSync } from 'node:fs';
import { basename } from 'node:path';

const OWNER = 'victor900106', REPO = 'ZizaiCast', FULL = `${OWNER}/${REPO}`;
let auth = null;  // GitHub headers, from the git credential on first use
function headers() {
  if (!auth) {
    const token = execSync('git credential fill', { input: 'protocol=https\nhost=github.com\n\n' })
      .toString().match(/^password=(.*)$/m)[1].trim();
    auth = { Authorization: `token ${token}`, Accept: 'application/vnd.github+json' };
  }
  return auth;
}
async function api(method, url, body, extra = {}) {
  const H = headers();
  const r = await fetch(url.startsWith('http') ? url : `https://api.github.com${url}`, {
    method, headers: { ...H, ...(body && !Buffer.isBuffer(body) ? { 'Content-Type': 'application/json' } : {}), ...extra },
    body: body && !Buffer.isBuffer(body) ? JSON.stringify(body) : body,
  });
  const t = await r.text();
  if (!r.ok && r.status !== 404) throw new Error(`${method} ${url} -> ${r.status} ${t.slice(0, 400)}`);
  return { status: r.status, json: t ? JSON.parse(t) : {} };
}
const [cmd, ...args] = process.argv.slice(2);

if (cmd === 'settings') {
  const description = '自在投影：免費開源的 Windows 手機投影 — iPhone/iPad 用 AirPlay 免裝 App，Android 用投放(Miracast)或無線偵錯並可用電腦操控；錄影、截圖、低延遲、最高 4K60。Free open-source AirPlay receiver & Android screen mirroring for Windows.';
  await api('PATCH', `/repos/${FULL}`, { description, homepage: `https://${OWNER}.github.io/${REPO}/`, has_issues: true, has_wiki: false, has_projects: false });
  const names = 'airplay airplay-receiver screen-mirroring screen-mirror iphone ipad android miracast scrcpy adb wireless-debugging windows windows-11 screen-recorder cpp directx uxplay mirroring cast taiwan'.split(' ');
  await api('PUT', `/repos/${FULL}/topics`, { names });
  for (const [name, color, d] of [['needs-triage', 'FBCA04', '待分類'], ['source-request', '0E8A16', '索取原始碼 / source request']]) {
    const r = await api('GET', `/repos/${FULL}/labels/${name}`);
    if (r.status === 404) await api('POST', `/repos/${FULL}/labels`, { name, color, description: d });
  }
  console.log('settings ok');
} else if (cmd === 'drop-draft') {
  const tag = args[0];
  const list = (await api('GET', `/repos/${FULL}/releases?per_page=50`)).json;
  for (const rel of list.filter((r) => r.tag_name === tag)) {
    await api('DELETE', `/repos/${FULL}/releases/${rel.id}`);
    console.log('deleted release', rel.id, rel.tag_name, rel.draft ? '(draft)' : '');
  }
  const ref = await api('GET', `/repos/${FULL}/git/refs/tags/${tag}`);
  if (ref.status !== 404) { await api('DELETE', `/repos/${FULL}/git/refs/tags/${tag}`); console.log('deleted tag', tag); }
} else if (cmd === 'release') {
  const [version, notesFile, ...assets] = args;
  const rel = (await api('POST', `/repos/${FULL}/releases`, {
    tag_name: `v${version}`, target_commitish: 'main', name: `自在投影 Zizai Cast v${version}`,
    body: readFileSync(notesFile, 'utf8'), draft: false, prerelease: false, make_latest: 'true',
  })).json;
  const up = rel.upload_url.replace(/\{.*$/, '');
  for (const a of assets) {
    const [path, name0] = a.split('=');
    const name = name0 || basename(path);
    const ct = name.endsWith('.json') ? 'application/json' : name.endsWith('.zip') ? 'application/zip' : 'application/octet-stream';
    await api('POST', `${up}?name=${encodeURIComponent(name)}`, readFileSync(path), { 'Content-Type': ct });
    console.log('uploaded', name, statSync(path).size);
  }
  console.log('release', rel.html_url);
} else if (cmd === 'replace-asset') {
  // replace-asset <tag> <path> [name]: delete the asset with that name, upload the new file.
  const [tag, path, name0] = args;
  const name = name0 || basename(path);
  const rel = (await api('GET', `/repos/${FULL}/releases/tags/${tag}`)).json;
  for (const a of rel.assets.filter((x) => x.name === name)) {
    await api('DELETE', `/repos/${FULL}/releases/assets/${a.id}`);
    console.log('deleted asset', a.name, a.id);
  }
  const up = rel.upload_url.replace(/\{.*$/, '');
  const ct = name.endsWith('.json') ? 'application/json' : 'application/octet-stream';
  await api('POST', `${up}?name=${encodeURIComponent(name)}`, readFileSync(path), { 'Content-Type': ct });
  console.log('uploaded', name, statSync(path).size);
} else if (cmd === 'pages') {
  const cur = await api('GET', `/repos/${FULL}/pages`);
  if (cur.status === 404) await api('POST', `/repos/${FULL}/pages`, { source: { branch: 'gh-pages', path: '/' } });
  console.log('pages', (await api('GET', `/repos/${FULL}/pages`)).json.html_url);
} else if (cmd === 'show') {
  const repo = (await api('GET', `/repos/${FULL}`)).json;
  console.log(repo.html_url, '|', repo.description, '| homepage', repo.homepage, '| topics', (repo.topics || []).length);
  const list = (await api('GET', `/repos/${FULL}/releases?per_page=10`)).json;
  for (const r of list) console.log(r.tag_name, r.draft ? 'DRAFT' : '', r.prerelease ? 'PRE' : '', r.assets.map((x) => `${x.name}(${x.size})`).join(', '));
} else if (cmd === 'manifest') {
  // update.json for 自動更新 (docs/app.md "Online manifest"): version, url, sha256 and
  // notes (read by every app version) plus date, size, changes_zh, changes_en (0.7.0+;
  // 0.6.x skip unknown keys) and, if the file has them, changes_ja / changes_ko (0.7.0+:
  // `## 日本語` / `## 한국어`; the app falls back to changes_en). <changes.md> has
  // `## 中文` and `## English` sections whose
  // `- ` bullets become the lists (indented continuation lines are joined; **bold** and
  // `code` marks dropped); the first plain line of a section is its summary for `notes`.
  // The same JSON next to an installer as <installer>.json gives 本機更新 its change list;
  // for such a sidecar <url> may be "" (the local file is used, not a download).
  const opt = (name) => { const i = args.indexOf(name); return i < 0 ? undefined : args.splice(i, 2)[1]; };
  const date = opt('--date') ?? new Date().toISOString().slice(0, 10);
  const notesArg = opt('--notes');
  const [version, installer, url, changesFile, out = 'update.json'] = args;
  if (!version || !installer || url === undefined || !changesFile)
    throw new Error('usage: manifest <version> <installer> <url> <changes.md> [out.json] [--date D] [--notes S]');
  if (!/^\d+\.\d+\.\d+$/.test(version)) throw new Error(`bad version ${version}`);
  if (url !== '' && !/^https:\/\//.test(url)) throw new Error('url must be https:// (or "" for a local sidecar)');
  if (!/^\d{4}-\d{2}-\d{2}$/.test(date)) throw new Error(`bad date ${date}`);
  const bytes = readFileSync(installer);
  const sec = { zh: { bullets: [], text: [] }, en: { bullets: [], text: [] }, ja: { bullets: [], text: [] }, ko: { bullets: [], text: [] } };
  let cur = null, last = null;
  const clean = (t) => t.replace(/\*\*(.+?)\*\*/g, '$1').replace(/`([^`]+)`/g, '$1').replace(/\s+/g, ' ').trim();
  for (const raw of readFileSync(changesFile, 'utf8').replace(/^﻿/, '').split(/\r?\n/)) {
    const h = raw.match(/^##\s+(.*)$/);
    if (h) {
      cur = /中文/.test(h[1]) ? sec.zh : /english/i.test(h[1]) ? sec.en : /日本語/.test(h[1]) ? sec.ja : /한국어/.test(h[1]) ? sec.ko : null;
      last = null;
      continue;
    }
    if (!cur || /^#/.test(raw)) continue;
    const b = raw.match(/^\s*[-*]\s+(.*)$/);
    if (b) { cur.bullets.push(clean(b[1])); last = cur.bullets; continue; }
    if (!raw.trim()) { last = null; continue; }
    if (last && /^\s+/.test(raw)) { last[last.length - 1] = clean(`${last[last.length - 1]} ${raw}`); continue; }
    cur.text.push(clean(raw));
  }
  if (!sec.zh.bullets.length || !sec.en.bullets.length)
    throw new Error(`${changesFile}: needs "## 中文" and "## English" sections with "- " bullets`);
  const notes = notesArg ?? [sec.zh.text[0] ?? sec.zh.bullets[0], sec.en.text[0] ?? sec.en.bullets[0]].join(' ');
  const m = {
    version, url, sha256: createHash('sha256').update(bytes).digest('hex'), notes,
    date, size: bytes.length, changes_zh: sec.zh.bullets, changes_en: sec.en.bullets,
  };
  if (sec.ja.bullets.length) m.changes_ja = sec.ja.bullets;
  if (sec.ko.bullets.length) m.changes_ko = sec.ko.bullets;
  const json = `${JSON.stringify(m, null, 2)}\n`;
  if (Buffer.byteLength(json) > 60 * 1024) throw new Error('manifest over 60 KB (the app reads at most 64 KB)');
  writeFileSync(out, json);  // UTF-8 without BOM
  console.log(`wrote ${out}: v${version}, ${m.size} bytes, sha256 ${m.sha256}, ${m.changes_zh.length} zh / ${m.changes_en.length} en / ${sec.ja.bullets.length} ja / ${sec.ko.bullets.length} ko changes`);
} else {
  console.log('usage: settings | drop-draft <tag> | release <version> <notes> <assets...> | replace-asset <tag> <path> [name]\n' +
    '       | pages | show | manifest <version> <installer> <url> <changes.md> [out.json] [--date D] [--notes S]');
}
