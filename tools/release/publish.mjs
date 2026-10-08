// GitHub API steps for publishing a 自在投影 / Zizai Cast release (no gh CLI needed;
// uses the git credential for github.com). Run from the repo root.
//   node tools/release/publish.mjs settings
//   node tools/release/publish.mjs drop-draft <tag>
//   node tools/release/publish.mjs release <version> <notes.md> <asset>...   (asset = path[=name])
//   node tools/release/publish.mjs pages
//   node tools/release/publish.mjs show
import { execSync } from 'node:child_process';
import { readFileSync, statSync } from 'node:fs';
import { basename } from 'node:path';

const OWNER = 'victor900106', REPO = 'ZizaiCast', FULL = `${OWNER}/${REPO}`;
const token = execSync('git credential fill', { input: 'protocol=https\nhost=github.com\n\n' })
  .toString().match(/^password=(.*)$/m)[1].trim();
const H = { Authorization: `token ${token}`, Accept: 'application/vnd.github+json' };
async function api(method, url, body, extra = {}) {
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
} else {
  console.log('usage: settings | drop-draft <tag> | release <version> <notes> <assets...> | pages | show');
}
