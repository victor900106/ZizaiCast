// 0.8.3.5 files: the web page's side of the 傳檔 permission table (node 22+, no browser, no network).
//   node remote/tools/fileperm_web_test.mjs        exit 0 = PASS
// Pulls filesWhyNow / filesDenyKey out of remote/web/index.html (between the "0.8.3.5 files perm" markers),
// runs the whole table, and checks the page wires them: the 檔案 button is never hidden, looks off
// when files are not allowed, and a tap says why in all four languages. Twin of pm_fileperm_test.
import fs from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const html = fs.readFileSync(path.join(here, '..', 'web', 'index.html'), 'utf8').replace(/\r\n/g, '\n');
let fails = 0, checks = 0;
const expect = (ok, what) => { checks++; if (!ok) { fails++; console.log('FAIL ', what); } };

const m = html.match(/\/\/ ---- 0\.8\.3\.5 files perm ----\n([\s\S]*?)\/\/ ---- end 0\.8\.3\.5 files perm ----/);
expect(!!m, 'the perm block is in index.html');
const ctx = vm.createContext({});
vm.runInContext(m ? m[1] : '', ctx);
const why = (viewOnly, filesWhy) => vm.runInContext(`filesWhyNow(${JSON.stringify({viewOnly, filesWhy})})`, ctx);
const key = w => vm.runInContext(`filesDenyKey(${JSON.stringify(w)})`, ctx);

// 主帳號 (host says filesWhy ''), 臨時帳號 with 傳檔 (''), 臨時帳號 without ('guest'); each x 僅觀看 on/off
expect(why(false, '') === '', 'main account / guest with files: on');
expect(why(false, 'guest') === 'guest', 'guest without files: off (guest)');
expect(why(true, '') === 'viewOnly', 'main account 僅觀看: off (viewOnly)');
expect(why(true, 'guest') === 'viewOnly', '僅觀看 wins over the guest reason');
expect(why(false, undefined) === '', 'an old host (no filesWhy): on');
expect(why(false, 'viewOnly') === '', 'the viewOnly reason alone does not switch the page off (viewOnly itself does)');
expect(key('guest') === 'filesNoGuest' && key('viewOnly') === 'filesDenied', 'reason -> message key');

// the messages: four languages, plain words
const T = html.match(/filesNoGuest: \[([^\]]*)\]/);
const parts = T ? [...T[1].matchAll(/'((?:[^'\\]|\\.)*)'/g)].map(x => x[1]) : [];
expect(parts.length === 4 && parts.every(s => s.length > 20), 'filesNoGuest has 4 languages');
expect(parts[0].includes('臨時帳號') && parts[0].includes('可以傳檔') && parts[0].includes('電腦'), 'zh message names the account kind, the switch and where to fix it');
expect(/temporary account/i.test(parts[1]) && /PC/.test(parts[1]), 'en message');

// the page wiring
expect(!/\$\('bFile'\)\.style\.display\s*=/.test(html), 'the 檔案 button is never hidden');
expect(/classList\.toggle\('off', !!why\)/.test(html), 'the button looks off when denied');
expect(/\.ib\.off\{/.test(html), 'css for the off look');
expect(/const why = filesWhyNow\(S\);\s*\n\s*if \(why\) \{ toast\(t\(filesDenyKey\(why\)\)/.test(html), 'a tap on the off button says why and does not open the sheet');
expect(/j && j\.noFiles \? t\('filesNoGuest'\)/.test(html), 'an upload refused with noFiles says the guest reason');
expect(/S\.filesWhy = typeof o\.filesWhy === 'string'/.test(html), 'the offer carries filesWhy');
expect(/typeof m\.filesWhy === 'string'/.test(html), 'the mode message carries filesWhy');
// rel-0835: the PC bar's 檔案 (fix-0835-input) follows the same table
expect(/pb\.classList\.toggle\('off', !!why\)/.test(html) && /pb\.setAttribute\('aria-disabled'/.test(html), 'the PC bar 檔案 looks off when denied');
expect(/pb\.title = why \? t\(filesDenyKey\(why\)\) : t\('lbFile'\)/.test(html), 'the PC bar 檔案 tooltip says why');
expect(/#pcbar button\.off\{/.test(html), 'css for the PC bar off look');
expect(/pcBtn\('pbFile', \(\) => \$\('bFile'\)\.onclick\(\)\)/.test(html), 'a click on the PC bar 檔案 goes the phone button\'s way (says why when off)');
expect(/\$\('pbFile'\)\.style\.display = \$\('bFile'\)\.style\.display;\n\s*syncFilesBtn\(\);/.test(html), 'the PC bar refresh keeps the 檔案 state');

console.log(`${fails ? 'FAIL' : 'PASS'}: ${checks} checks, ${fails} failed`);
process.exit(fails ? 1 : 0);
