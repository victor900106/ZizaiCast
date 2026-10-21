// Anchors / links / TODO / placeholders / IP check across the four guides.
import fs from 'node:fs';
const dir = new URL('../../', import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1');
const files = ['自在投影教學.html', 'ZizaiCast-Guide.html', 'ZizaiCast-Guide-ja.html', 'ZizaiCast-Guide-ko.html'];
const required = 'before idle iphone miracast adb features remote faq disconnect magnifier translate share update remote-open remote-home remote-outside remote-pwa remote-tailscale remote-nolink remote-gestures remote-audio remote-safety remote-limits remote-trouble remote-faq-nosound remote-faq-noconnect remote-faq-forgot faq-notfound faq-audio faq-lag faq-blur faq-takeover faq-fw faq-vpn faq-miracast faq-adb faq-disconnect faq-translate'.split(' ');
let ref = null;
for (const f of files) {
  const s = fs.readFileSync(dir + f, 'utf8').replace(/data:[a-z/]+;base64,[A-Za-z0-9+/=]+/g, 'DATA');
  const ids = [...s.matchAll(/\sid="([^"]+)"/g)].map(m => m[1]);
  const dup = ids.filter((x, i) => ids.indexOf(x) !== i);
  const links = [...s.matchAll(/href="#([^"]+)"/g)].map(m => m[1]);
  const broken = [...new Set(links.filter(l => !ids.includes(l)))];
  const miss = required.filter(r => !ids.includes(r));
  const todo = (s.match(/TODO-0832/g) || []).length;
  const ph = (s.match(/class="shot"/g) || []).length;
  const ips = [...new Set((s.match(/\b(?:\d{1,3}\.){3}\d{1,3}\b/g) || []))];
  const imgs = (s.match(/<img /g) || []).length;
  console.log(f, `ids ${ids.length} dup [${dup}] broken [${broken}] missing-required [${miss}] TODO-0832 ${todo} placeholders ${ph} imgs ${imgs} ips [${ips}]`);
  const set = ids.filter(x => x !== 'dot').sort().join(',');
  if (ref === null) ref = set; else if (ref !== set) console.log('  ID SET DIFFERS from zh');
}
