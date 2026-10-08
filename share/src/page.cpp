#include "page.h"

#include <windows.h>

#include <cstdio>

#include "pm/i18n.h"

namespace pm::share {
namespace {

using pm::i18n::Lang;
using pm::i18n::S;

std::string u8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Table texts are trusted HTML (some carry <b>); arguments are escaped.
struct T {
    Lang lang;
    std::string operator()(S id) const { return u8(pm::i18n::tr(id, lang)); }
    std::string operator()(S id, std::initializer_list<std::wstring> args) const {
        std::string out = u8(pm::i18n::tr(id, lang));
        size_t k = 0;
        for (const std::wstring& a : args) {
            const std::string ph = "{" + std::to_string(k++) + "}";
            for (size_t p; (p = out.find(ph)) != std::string::npos;) out.replace(p, ph.size(), htmlEscape(u8(a)));
        }
        return out;
    }
};

std::string hex(uint32_t c) {
    char b[8];
    std::snprintf(b, sizeof(b), "#%06X", c & 0xFFFFFF);
    return b;
}
std::string rgba(uint32_t c, double a) {
    char b[48];
    std::snprintf(b, sizeof(b), "rgba(%u,%u,%u,%.2f)", (c >> 16) & 255, (c >> 8) & 255, c & 255, a);
    return b;
}
double luminance(uint32_t c) {
    return (0.2126 * ((c >> 16) & 255) + 0.7152 * ((c >> 8) & 255) + 0.0722 * (c & 255)) / 255.0;
}

std::string head(int lang, const std::string& title, const PagePalette& p) {
    // Text on the accent (buttons): dark on a light accent, white otherwise.
    const std::string on = luminance(p.accent) > 0.55 ? "#2A1C1F" : "#FFFFFF";
    std::string h;
    h += "<!doctype html>\n<html lang=\"";
    h += lang == 1 ? "en" : lang == 2 ? "ja" : lang == 3 ? "ko" : "zh-Hant";
    h += "\"><head><meta charset=\"utf-8\">"
         "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">"
         "<meta name=\"referrer\" content=\"no-referrer\"><meta name=\"robots\" content=\"noindex,nofollow\">"
         "<meta name=\"format-detection\" content=\"telephone=no\">";
    h += "<meta name=\"color-scheme\" content=\"";
    h += p.light ? "light" : "dark";
    h += "\"><meta name=\"theme-color\" content=\"" + hex(p.background) + "\">";
    h += "<title>" + title + "</title><style>";
    h += ":root{--bg:" + hex(p.background) + ";--c1:" + hex(p.cardTop) + ";--c2:" + hex(p.cardBottom) +
         ";--fg:" + hex(p.fg) + ";--dim:" + hex(p.dim) + ";--ac:" + hex(p.accent) + ";--on:" + on +
         ";--line:" + rgba(p.accent, 0.35) + ";--soft:" + rgba(p.accent, 0.14) + "}";
    h += R"CSS(
*{box-sizing:border-box}html{-webkit-text-size-adjust:100%}[hidden]{display:none!important}
body{margin:0;min-height:100vh;background:var(--bg);background-image:radial-gradient(140% 60% at 50% -10%,var(--c1) 0%,var(--bg) 70%);color:var(--fg);
font:16px/1.55 -apple-system,BlinkMacSystemFont,"PingFang TC","Noto Sans TC","Microsoft JhengHei UI","Segoe UI",Roboto,sans-serif;
padding:env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left)}
main{max-width:560px;margin:0 auto;padding:18px 14px 28px}
header{display:flex;align-items:center;gap:12px;margin:6px 4px 18px}
.logo{flex:none;width:46px;height:46px;border-radius:14px;background:var(--ac);display:grid;place-items:center;box-shadow:0 6px 18px var(--line)}
.logo svg{width:26px;height:26px}
h1{font-size:21px;line-height:1.25;margin:0;font-weight:700}
.sub{color:var(--dim);font-size:13.5px;margin:3px 0 0}
.live{display:flex;align-items:center;gap:7px;color:var(--ac);font-size:12.5px;font-weight:600;margin:4px 0 0}
.live i{flex:none;width:8px;height:8px;border-radius:50%;background:var(--ac);animation:blink 1.6s ease-in-out infinite}
@keyframes blink{50%{opacity:.25}}
.card{background:linear-gradient(180deg,var(--c1),var(--c2));border:1px solid var(--line);border-radius:20px;padding:12px;margin:0 0 18px;box-shadow:0 10px 28px rgba(0,0,0,.28)}
.card.new{animation:pop 1.2s ease-out}
@keyframes pop{0%{transform:scale(.97);box-shadow:0 0 0 3px var(--ac),0 10px 28px rgba(0,0,0,.28)}100%{transform:none}}
.media{display:block;width:100%;height:auto;max-height:78vh;object-fit:contain;border-radius:13px;background:rgba(0,0,0,.35)}
img.media{-webkit-touch-callout:default}
.meta{display:flex;justify-content:space-between;gap:10px;font-size:12.5px;color:var(--dim);margin:9px 4px 0;word-break:break-all}
.meta span:last-child{flex:none}
.tip{margin:12px 4px 0;padding:11px 13px;border-radius:13px;background:var(--soft);font-size:15px}
.tip b{color:var(--ac);font-weight:700}
.os{font-size:12px;font-weight:700;letter-spacing:.06em;color:var(--ac);margin:14px 6px 2px}
ol{margin:6px 4px 0;padding-left:24px;font-size:15px}li{margin:5px 0}
.btn{display:block;width:100%;text-align:center;text-decoration:none;font:inherit;font-weight:700;font-size:17px;padding:14px 16px;border:0;border-radius:15px;margin:12px 0 0;background:var(--ac);color:var(--on);cursor:pointer;-webkit-appearance:none;appearance:none}
.btn.ghost{background:transparent;border:1.5px solid var(--line);color:var(--fg);font-weight:600;font-size:16px;padding:11px 16px}
.btn:disabled{opacity:.55;cursor:default}
.card.saved .btn[data-save]{background:transparent;border:1.5px solid var(--line);color:var(--dim);font-weight:600;font-size:16px;padding:11px 16px}
.bar{position:sticky;top:0;z-index:2;margin:0 0 16px;padding:10px 0 2px;background:linear-gradient(var(--bg) 82%,transparent)}
.bar .btn{margin:0;box-shadow:0 8px 22px var(--line)}
.hint{color:var(--dim);font-size:13px;margin:8px 6px 0;text-align:center}.hint b{color:var(--fg)}
.note{margin:0 4px 14px;padding:10px 13px;border-radius:13px;background:var(--soft);font-size:14px;text-align:center}
.empty{text-align:center;color:var(--dim);padding:30px 18px}
footer{color:var(--dim);font-size:12.5px;text-align:center;margin:6px 16px 0}
.gone{text-align:center;padding:28px 18px}
.gone h1{margin:14px 0 8px}
)CSS";
    h += "</style></head><body><main";
    return h;  // the caller closes <main ...>
}

// Cloud-and-phone mark in the accent's text colour (inline, no request).
std::string logo(const PagePalette& p) {
    const std::string on = luminance(p.accent) > 0.55 ? "#2A1C1F" : "#FFFFFF";
    return "<div class=\"logo\"><svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"" + on +
           "\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\" aria-hidden=\"true\">"
           "<rect x=\"6.5\" y=\"2.5\" width=\"11\" height=\"19\" rx=\"2.5\"/>"
           "<path d=\"M12 7v7\"/><path d=\"M9 11.5l3 3 3-3\"/><path d=\"M10.5 18.5h3\"/></svg></div>";
}

// The how-to under a picture / a video for this phone kind (also handed to
// the script for cards it adds).
std::string imgTips(const T& t, const std::string& kind) {
    const bool ios = kind == "ios", android = kind == "android", both = !ios && !android;
    std::string h;
    if (ios || both) {
        if (both) h += "<div class=\"os\">iPhone / iPad</div>";
        h += "<div class=\"tip\">" + t(S::PgImgTipIos) + "</div>";
    }
    if (android || both) {
        if (both) h += "<div class=\"os\">Android</div>";
        h += "<div class=\"tip\">" + t(S::PgImgTipAndroid) + "</div>";
    }
    return h;
}
std::string vidTips(const T& t, const std::string& kind) {
    const bool ios = kind == "ios", android = kind == "android", both = !ios && !android;
    std::string h;
    if (ios || both) {
        h += "<div class=\"howto\">";
        if (both) h += "<div class=\"os\">iPhone / iPad</div>";
        h += "<ol><li>" + t(S::PgVidIos1) + "</li><li>" + t(S::PgVidIos2) + "</li><li>" + t(S::PgVidIos3) +
             "</li></ol></div>";
    }
    if (android || both) {
        if (both) h += "<div class=\"os\">Android</div>";
        h += "<div class=\"tip\">" + t(S::PgVidAndroid) + "</div>";
    }
    return h;
}

std::string card(const T& t, const PageFile& f, const std::string& kind) {
    const std::string i = std::to_string(f.index);
    const std::string name = htmlEscape(f.name);
    std::string h = "<section class=\"card\" data-i=\"" + i + "\" data-name=\"" + name + "\" data-type=\"" +
                    htmlEscape(f.type) + "\" data-size=\"" + std::to_string(f.size) + "\" data-video=\"" +
                    (f.video ? "1" : "0") + "\">";
    if (!f.video) {
        h += "<img class=\"media\" src=\"v/" + i + "\" alt=\"" + name + "\">";
        h += "<div class=\"meta\"><span>" + name + "</span><span>" + sizeText(f.size) + "</span></div>";
        h += imgTips(t, kind);
        // iOS: Download puts a picture in Files, not Photos -> the long press is the main way.
        h += std::string("<a class=\"btn") + (kind == "ios" ? " ghost" : "") + "\" href=\"d/" + i +
             "\" download data-save=\"" + i + "\">" + t(S::PgDownload) + "</a>";
    } else {
        // #t=0.1: iOS Safari shows the first frame instead of a black box.
        h += "<video class=\"media\" src=\"v/" + i + "#t=0.1\" controls playsinline preload=\"metadata\"></video>";
        h += "<div class=\"meta\"><span>" + name + "</span><span>" + sizeText(f.size) + "</span></div>";
        h += "<a class=\"btn\" href=\"d/" + i + "\" download data-save=\"" + i + "\">" + t(S::PgDownloadVideo) + "</a>";
        h += vidTips(t, kind);
    }
    h += "</section>";
    return h;
}

void jsonField(std::string& j, const char* key, const std::string& value, bool first = false) {
    if (!first) j += ',';
    j += '"';
    j += key;
    j += "\":\"" + jsonEscape(value) + "\"";
}

}  // namespace

std::string htmlEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        case '\'': o += "&#39;"; break;
        default: o += c;
        }
    }
    return o;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        case '<': o += "\\u003c"; break;  // never "</script>" inside the page's JSON block
        case '>': o += "\\u003e"; break;
        case '&': o += "\\u0026"; break;
        default:
            if (c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof(b), "\\u%04x", c);
                o += b;
            } else {
                o += char(c);
            }
        }
    }
    return o;
}

std::string sizeText(uint64_t b) {
    char buf[32];
    if (b >= (1ull << 30)) std::snprintf(buf, sizeof(buf), "%.2f GB", double(b) / double(1ull << 30));
    else if (b >= (1ull << 20)) std::snprintf(buf, sizeof(buf), "%.1f MB", double(b) / double(1ull << 20));
    else std::snprintf(buf, sizeof(buf), "%llu KB", static_cast<unsigned long long>((b + 1023) / 1024));
    return buf;
}

std::string sharePage(int lang, const std::wstring& appName, const PagePalette& pal,
                      const std::vector<PageFile>& files, const std::string& kind, const std::string& untilHHMM,
                      int ttlSeconds, bool live) {
    const T t{static_cast<Lang>(lang)};
    const bool ios = kind == "ios", android = kind == "android";
    std::string h = head(lang, t(S::PgTitle), pal);
    h += std::string(" data-live=\"") + (live ? "1" : "0") + "\" data-n=\"" + std::to_string(files.size()) + "\">";
    const std::wstring app = appName;
    std::wstring count = std::to_wstring(files.size());
    h += "<header>" + logo(pal) + "<div><h1>" + t(S::PgTitle) + "</h1><p class=\"sub\" id=\"sub\">" +
         (files.size() == 1 ? t(S::PgSubOne, {app}) : t(S::PgSubMany, {app, count})) + "</p>";
    if (live) h += "<p class=\"live\" id=\"live\"><i></i>" + t(S::PgLive) + "</p>";
    h += "</div></header>";
    // 全部儲存 (the script shows it where the browser can share files).
    h += "<div class=\"bar\" id=\"bar\" hidden><button class=\"btn\" id=\"saveall\" type=\"button\" disabled>" +
         t(S::PgSaveAll, {count}) + "</button>";
    if (!android) h += "<p class=\"hint\">" + t(S::PgSaveAllHint) + "</p>";
    h += "</div><p class=\"note\" id=\"note\" hidden></p>";
    if (live)
        h += std::string("<section class=\"card empty\" id=\"empty\"") + (files.empty() ? "" : " hidden") + ">" +
             t(S::PgWaiting) + "</section>";
    h += "<div id=\"list\">";
    if (live)  // the newest first: new ones appear at the top
        for (auto it = files.rbegin(); it != files.rend(); ++it) h += card(t, *it, kind);
    else
        for (const PageFile& f : files) h += card(t, f, kind);
    h += "</div>";
    if (live)
        h += "<footer>" + t(S::PgLiveFooter, {std::to_wstring((ttlSeconds + 1799) / 3600)}) + "</footer>";
    else
        h += "<footer>" + t(S::PgFooter, {std::wstring(untilHHMM.begin(), untilHHMM.end()),
                                          std::to_wstring((ttlSeconds + 59) / 60)}) + "</footer>";
    // Texts for the script (a data block: not executed, allowed by the CSP).
    std::string j = "{";
    jsonField(j, "kind", ios ? "ios" : android ? "android" : "other", true);
    jsonField(j, "app", u8(appName));
    jsonField(j, "subOne", u8(pm::i18n::tr(S::PgSubOne, t.lang)));
    jsonField(j, "subMany", u8(pm::i18n::tr(S::PgSubMany, t.lang)));
    jsonField(j, "save", t(S::PgSave));
    jsonField(j, "saved", t(S::PgSaved));
    jsonField(j, "saveAll", u8(pm::i18n::tr(S::PgSaveAll, t.lang)));
    jsonField(j, "allSaved", t(S::PgAllSaved));
    jsonField(j, "preparing", u8(pm::i18n::tr(S::PgPreparing, t.lang)));
    jsonField(j, "download", t(S::PgDownload));
    jsonField(j, "downloadVideo", t(S::PgDownloadVideo));
    jsonField(j, "imgTips", imgTips(t, kind));
    jsonField(j, "vidTips", vidTips(t, kind));
    jsonField(j, "ended", t(S::PgEnded));
    jsonField(j, "offline", t(S::PgOffline));
    jsonField(j, "shareFail", t(S::PgShareFail));
    j += "}";
    h += "</main><script type=\"application/json\" id=\"pm-t\">" + j + "</script><script src=\"app.js\"></script>"
         "</body></html>\n";
    return h;
}

std::string expiredPage(int lang, const std::wstring& appName, const PagePalette& pal, bool live) {
    const T t{static_cast<Lang>(lang)};
    std::string h = head(lang, t(S::PgExpiredTitle), pal);
    h += "><section class=\"card gone\">" + logo(pal) + "<h1>" + t(S::PgExpiredTitle) + "</h1><p>" +
         t(live ? S::PgExpiredLiveText : S::PgExpiredText, {appName}) + "</p></section></main></body></html>\n";
    // the logo is a block; centre it
    const std::string from = "<section class=\"card gone\"><div class=\"logo\"";
    if (const size_t p = h.find(from); p != std::string::npos)
        h.replace(p, from.size(), "<section class=\"card gone\"><div class=\"logo\" style=\"margin:0 auto\"");
    return h;
}

std::string listJson(const std::vector<PageFile>& files, bool live) {
    std::string j = std::string("{\"live\":") + (live ? "true" : "false") + ",\"n\":" + std::to_string(files.size()) +
                    ",\"files\":[";
    for (size_t k = 0; k < files.size(); ++k) {
        const PageFile& f = files[k];
        if (k) j += ',';
        j += "{\"i\":" + std::to_string(f.index);
        jsonField(j, "name", f.name);
        j += std::string(",\"video\":") + (f.video ? "true" : "false") + ",\"size\":" + std::to_string(f.size);
        jsonField(j, "sizeText", sizeText(f.size));
        jsonField(j, "type", f.type);
        j += "}";
    }
    j += "]}";
    return j;
}

// The page's script. Progressive: without it the page works as before.
//  * 全部儲存 / 儲存: navigator.share({files}) with File objects built from
//    blobs fetched ahead (the share must start inside the tap: iOS refuses
//    it after an await), so the button reads 「準備中…」 until every unsaved
//    file (≤ 200 MB each) is in memory. Shared ones are marked 已儲存.
//  * live: GET list?n=K is held by the server until file K+1 exists (or
//    20 s); new cards are added at the top. 410 → 分享已結束 (the cards and
//    their saves keep working); network errors → 「暫時連不到電腦」 + retry.
const std::string& shareScript() {
    static const std::string js = R"JS((function () {
'use strict';
var T = JSON.parse(document.getElementById('pm-t').textContent);
var main = document.querySelector('main'), list = document.getElementById('list');
var bar = document.getElementById('bar'), btn = document.getElementById('saveall');
var sub = document.getElementById('sub'), note = document.getElementById('note'), empty = document.getElementById('empty');
var live = main.getAttribute('data-live') === '1', known = +main.getAttribute('data-n') || 0;
var CAP = 200 * 1024 * 1024, items = [], byI = {}, ended = false, fails = 0, fetching = false;
function fill(s, a) { return s.replace(/\{(\d)\}/g, function (m, k) { return a[+k] !== undefined ? String(a[+k]) : m; }); }
var canFiles = false;
try {
  canFiles = !!(navigator.share && navigator.canShare && window.File &&
    navigator.canShare({ files: [new File(['x'], 'x.png', { type: 'image/png' })] }));
} catch (e) { canFiles = false; }
function el(tag, cls, text) { var e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; }
function say(s) { note.textContent = s || ''; note.hidden = !s; }
function adopt(c) {
  var it = { i: +c.getAttribute('data-i'), name: c.getAttribute('data-name'), type: c.getAttribute('data-type'),
    size: +c.getAttribute('data-size'), video: c.getAttribute('data-video') === '1', card: c,
    blob: null, failed: false, saved: false, btn: c.querySelector('[data-save]') };
  items.push(it); byI[it.i] = it;
  if (canFiles && it.btn && it.size <= CAP) {
    it.btn.textContent = T.save;
    it.btn.classList.remove('ghost');  // the share sheet saves to Photos: now the main way
    it.btn.addEventListener('click', function (ev) { if (!it.blob) return; ev.preventDefault(); share([it]); });
    var how = c.querySelectorAll('.howto, .tip, .os');
    for (var k = 0; k < how.length; k++) how[k].hidden = true;  // 儲存 replaces the long-press / Files-app steps
  }
  return it;
}
function card(f) {
  var c = el('section', 'card new'), i = String(f.i), m;
  c.setAttribute('data-i', i); c.setAttribute('data-name', f.name); c.setAttribute('data-type', f.type);
  c.setAttribute('data-size', String(f.size)); c.setAttribute('data-video', f.video ? '1' : '0');
  if (f.video) {
    m = el('video', 'media'); m.src = 'v/' + i + '#t=0.1'; m.controls = true; m.preload = 'metadata';
    m.setAttribute('playsinline', '');
  } else { m = el('img', 'media'); m.src = 'v/' + i; m.alt = f.name; }
  c.appendChild(m);
  var meta = el('div', 'meta'); meta.appendChild(el('span', null, f.name)); meta.appendChild(el('span', null, f.sizeText));
  c.appendChild(meta);
  var a = el('a', 'btn' + (!f.video && T.kind === 'ios' ? ' ghost' : ''), f.video ? T.downloadVideo : T.download);
  a.href = 'd/' + i; a.setAttribute('download', ''); a.setAttribute('data-save', i);
  if (f.video) { c.appendChild(a); c.insertAdjacentHTML('beforeend', T.vidTips); }
  else { c.insertAdjacentHTML('beforeend', T.imgTips); c.appendChild(a); }
  return c;
}
function pending() { return items.filter(function (x) { return !x.saved && !x.failed && x.size <= CAP; }); }
function update() {
  sub.textContent = items.length === 1 ? fill(T.subOne, [T.app]) : fill(T.subMany, [T.app, items.length]);
  if (empty) empty.hidden = items.length > 0;
  if (!canFiles) return;
  bar.hidden = items.length === 0;
  var todo = pending(), ready = todo.filter(function (x) { return x.blob; });
  if (!todo.length) { btn.textContent = T.allSaved; btn.disabled = true; }
  else if (ready.length < todo.length) { btn.textContent = fill(T.preparing, [ready.length, todo.length]); btn.disabled = true; }
  else { btn.textContent = fill(T.saveAll, [todo.length]); btn.disabled = false; }
}
function mark(x) { x.saved = true; x.card.classList.add('saved'); if (x.btn) x.btn.textContent = T.saved; }
function share(sel) {
  var files = sel.map(function (x) { return new File([x.blob], x.name, { type: x.type }); });
  var p;
  try { p = navigator.share({ files: files }); } catch (e) { say(T.shareFail); return; }
  Promise.resolve(p).then(function () { sel.forEach(mark); say(''); update(); },
    function (e) { if (!e || e.name !== 'AbortError') say(T.shareFail); });
}
btn.addEventListener('click', function () {
  var sel = pending().filter(function (x) { return x.blob; });
  if (sel.length) share(sel);
});
function prefetch() {
  if (!canFiles || fetching) return;
  var x = pending().filter(function (y) { return !y.blob; })[0];
  if (!x) return;
  fetching = true;
  fetch('v/' + x.i, { cache: 'no-store' }).then(function (r) { if (!r.ok) throw new Error(r.status); return r.blob(); })
    .then(function (b) { x.blob = b; }, function () { x.failed = true; })
    .then(function () { fetching = false; update(); prefetch(); });
}
function end() {
  ended = true; say(T.ended);
  var lv = document.getElementById('live'); if (lv) lv.hidden = true;
}
function poll() {
  if (!live || ended) return;
  fetch('list?n=' + known, { cache: 'no-store' }).then(function (r) {
    if (r.status === 410 || r.status === 404) { end(); return null; }
    if (!r.ok) throw new Error(r.status);
    return r.json();
  }).then(function (j) {
    if (!j) return;
    if (fails) say('');
    fails = 0;
    (j.files || []).forEach(function (f) {
      if (byI[f.i]) return;
      var c = card(f); list.insertBefore(c, list.firstChild); adopt(c);
    });
    known = j.n;
    update(); prefetch();
    setTimeout(poll, 30);
  }, function () {
    fails++;
    if (fails >= 2) say(T.offline);
    setTimeout(poll, Math.min(10000, 1500 * fails));
  });
}
var cards = list.querySelectorAll('.card[data-i]');
for (var k = 0; k < cards.length; k++) adopt(cards[k]);
update(); prefetch(); poll();
})();
)JS";
    return js;
}

}  // namespace pm::share
