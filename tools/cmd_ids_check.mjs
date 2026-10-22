// 0.8.3.5: static check of the app's menu / toolbar command ids (no build, no app run), node 18+.
//   node tools/cmd_ids_check.mjs            (exit code 0 = every check passed)
// One number = one command: menus, the toolbar, the idle page's buttons (WM_PM_TOOL) and --dev
// DevCommand scripts all go through runCommand(id), so two names with the same value, or a
// numbered range (CmdXxx0 .. CmdXxxLast, CmdTrDelete0 + kModelPairCount) over another id, would make a
// click on one row run another command (the owner's 2026-10-21 21:37 report: 遠端控制 turned off and
// on again; this check proves that was not an id clash). Also: 進階設定 ▸ / 聲音 ▸ never hold the
// 遠端控制 on / off switch, and every remote id reaches remoteCommand().
import fs from 'node:fs';
import path from 'node:path';

const root = path.resolve(path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1')), '..');
const read = f => fs.readFileSync(path.join(root, f), 'utf8');
let failed = 0;
const check = (name, ok, detail = '') => { console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? '  -- ' + detail : ''}`); if (!ok) failed++; };

// enum <name> : <type> { A = 100, B, C = A + 7, ... };  -> [{name, value, line}]
function parseEnum(src, enumName, consts = {}) {
  const m = src.match(new RegExp(`enum\\s+${enumName}\\s*:\\s*\\w+\\s*\\{([\\s\\S]*?)\\n\\};`));
  if (!m) throw new Error(`enum ${enumName} not found`);
  const body = m[1].replace(/\/\/[^\n]*/g, '').replace(/\/\*[\s\S]*?\*\//g, '');
  const out = [], byName = {...consts};
  let next = 0;
  for (const raw of body.split(',')) {
    const e = raw.trim();
    if (!e) continue;
    const [name, expr] = e.split('=').map(x => x && x.trim());
    let v = next;
    if (expr !== undefined) {
      const js = expr.replace(/[A-Za-z_]\w*/g, id => { if (!(id in byName)) throw new Error(`${enumName}: unknown ${id} in "${e}"`); return String(byName[id]); });
      if (!/^[\d\s+\-*()]+$/.test(js)) throw new Error(`${enumName}: cannot evaluate "${e}"`);
      v = Function(`return (${js});`)();
    }
    byName[name] = v;
    out.push({name, value: v, explicit: expr !== undefined, expr});
    next = v + 1;
  }
  return out;
}

const common = read('app/core/base/app_common.h');
const consts = read('app/core/base/app_consts.h');
const kModelPairCount = +(consts.match(/kModelPairCount\s*=\s*(\d+)/) || [])[1];
const cmds = parseEnum(common, 'Command');
const vols = parseEnum(read('app/volume_ui.h'), 'VolumeCommand');
check('parsed enum Command and VolumeCommand', cmds.length > 150 && vols.length >= 8, `${cmds.length} + ${vols.length} names`);

// Ranges: XxxLast = Xxx0 + n (Xxx0 .. XxxLast belong to Xxx0), and the ranges given in code by a count.
const ranges = [];
for (const c of cmds) {
  const m = c.expr && c.expr.match(/^(\w+)\s*\+\s*(\d+)$/);
  if (m && /Last$/.test(c.name)) ranges.push({name: m[1], from: cmds.find(x => x.name === m[1]).value, to: c.value, last: c.name});
}
const val = n => cmds.find(c => c.name === n).value;
check('kModelPairCount read', kModelPairCount > 0, String(kModelPairCount));
ranges.push({name: 'CmdTrDelete0', from: val('CmdTrDelete0'), to: val('CmdTrDelete0') + kModelPairCount - 1, last: '(+ kModelPairCount)'});
ranges.push({name: 'CmdTrDeleteLang0', from: val('CmdTrDeleteLang0'), to: val('CmdTrDeleteLang0') + 4, last: '(+ 5, commands.cpp)'});
const owner = new Map();  // value -> name (a range's members belong to its first name)
const clash = [];
const take = (v, name) => { if (owner.has(v) && owner.get(v) !== name) clash.push(`${v}: ${owner.get(v)} / ${name}`); else owner.set(v, name); };
for (const r of ranges) for (let v = r.from; v <= r.to; v++) take(v, r.name);
for (const c of cmds) {
  const r = ranges.find(x => x.last === c.name);
  if (r) continue;  // XxxLast: the end of its range
  const inRange = ranges.find(x => c.value >= x.from && c.value <= x.to);
  if (inRange && inRange.name !== c.name) { clash.push(`${c.value}: ${c.name} inside ${inRange.name}..${inRange.last}`); continue; }
  take(c.value, c.name);
}
check('enum Command: every id is one command (no two names, no name inside a numbered range)', clash.length === 0, clash.join('; ') || `${owner.size} ids`);
const volClash = vols.filter(v => owner.has(v.value)).map(v => `${v.value}: ${v.name} / ${owner.get(v.value)}`);
check('VolumeCommand (260..) clear of enum Command', volClash.length === 0, volClash.join('; '));
check('every command id below the --dev DevCommand numbers (900+)', Math.max(...cmds.map(c => c.value), ...vols.map(v => v.value)) < 900);

// The remote ids in use: menus / the page (remote_actions.cpp) name only enum ids.
const ra = read('app/remote/remote_actions.cpp');
const used = new Set([...ra.matchAll(/\b(Cmd[A-Z]\w*)\b/g)].map(m => m[1]));
const unknown = [...used].filter(n => !cmds.some(c => c.name === n) && !vols.some(v => v.name === n));
check('remote_actions.cpp names only known command ids', unknown.length === 0, unknown.join(', '));

// 進階設定 ▸ (remoteAdvancedItems) and its 聲音 ▸: what it keeps from 遠端操控 ▸ never includes the switch.
const adv = ra.slice(ra.indexOf('std::vector<MenuItem> remoteAdvancedItems() {'), ra.indexOf('// The window title and the page follow every change of state.'));
check('remoteAdvancedItems() found', adv.length > 200);
const keepIds = [...adv.matchAll(/m\.id == (Cmd\w+)/g)].map(m => m[1]);
const advIds = [...adv.matchAll(/\b(Cmd\w+)\b/g)].map(m => m[1]);
check('進階設定 / 聲音: never the 遠端控制 on / off switch (CmdRemoteToggle)', !advIds.includes('CmdRemoteToggle'), `kept from 遠端操控 ▸: ${[...new Set(keepIds)].join(', ')}`);
check('聲音 ▸ takes 電腦聲音傳到手機 / 電腦喇叭靜音 / 聲音來源 rows only', /m\.id == CmdRemoteAudio \|\| m\.id == CmdRemoteMutePc\) sound\.push_back/.test(adv));
// 聲音來源 ▸ rows: CmdRemoteAudioDev0 + i for at most 8 devices (the range's size).
const devRange = ranges.find(r => r.name === 'CmdRemoteAudioDev0');
check('聲音來源 ▸: at most as many devices as CmdRemoteAudioDev0..Last', /audioDevs\.size\(\) > 8\) r\.audioDevs\.resize\(8\)/.test(ra) && devRange && devRange.to - devRange.from + 1 === 8,
      devRange ? `${devRange.from}..${devRange.to}` : 'no range');
const forget = ranges.find(r => r.name === 'CmdRemoteForgetDev0');
check('已記住的手機 ▸: at most as many rows as CmdRemoteForgetDev0..Last', /i < list\.size\(\) && i < 16/.test(adv) && forget && forget.to - forget.from + 1 === 16);

// Every remote id that menus / the page use reaches remoteCommand() (commands.cpp).
const cc = read('app/core/app/commands.cpp');
const routed = new Set([...cc.matchAll(/case (CmdRemote\w+):/g)].map(m => m[1]));
const rangeRouted = name => {
  const r = ranges.find(x => x.name === name) || {};
  return new RegExp(`cmd >= ${name} && cmd <= ${r.last}`).test(cc);
};
const notRouted = [...used].filter(n => /^CmdRemote/.test(n) && !/Last$/.test(n) && !routed.has(n) && !rangeRouted(n) &&
  !(val(n) >= val('CmdRemoteGuestNew') && val(n) <= val('CmdRemoteTsDownload') && /cmd >= CmdRemoteGuestNew && cmd <= CmdRemoteTsDownload/.test(cc)) &&
  !(val(n) >= val('CmdRemoteGuestDel0') && val(n) <= val('CmdRemoteGuestResetLast') && /cmd >= CmdRemoteGuestDel0 && cmd <= CmdRemoteGuestResetLast/.test(cc)));
check('every remote id used reaches remoteCommand()', notRouted.length === 0, notRouted.join(', '));

// The ids the --dev test scripts use are the enum's (a script that hard-codes a moved id tests the wrong row).
const scripts = {'remote/tools/app_funnel_test.mjs': {TabRemote: 'CmdIdleTabRemote', FixedToggle: 'CmdRemoteFixedToggle', FixedSetup: 'CmdRemoteFixedSetup', Exit: 'CmdExit'},
                 'remote/tools/app0835_funnel_test.mjs': {Toggle: 'CmdRemoteToggle', Exit: 'CmdExit'}};
for (const [f, map] of Object.entries(scripts)) {
  const src = read(f);
  const bad = Object.entries(map).filter(([k, n]) => { const m = src.match(new RegExp(`\\b${k}: (\\d+)`)); return !m || +m[1] !== val(n); });
  check(`${f}: its command numbers are the enum's`, bad.length === 0, bad.map(([k, n]) => `${k} != ${n} (${val(n)})`).join(', '));
}

console.log(failed ? `FAILED (${failed} failed)` : 'ALL PASSED');
process.exit(failed ? 1 : 0);
