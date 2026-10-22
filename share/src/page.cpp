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
.card{background:linear-gradient(180deg,var(--c1),var(--c2));border:1px solid var(--line);border-radius:20px;padding:12px;margin:0 0 18px;box-shadow:0 10px 28px rgba(0,0,0,.28)}
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
.ziphint{margin:-6px 0 16px}.ziphint .tip{margin-top:0;font-size:14px}.ziphint .os{margin-top:8px}
.note{margin:0 4px 14px;padding:10px 13px;border-radius:13px;background:var(--soft);font-size:14px;text-align:center}
.empty{text-align:center;color:var(--dim);padding:30px 18px}
footer{color:var(--dim);font-size:12.5px;text-align:center;margin:6px 16px 0}
.gone{text-align:center;padding:28px 18px}
.gone h1{margin:14px 0 8px}
.clip .head{display:flex;align-items:center;justify-content:space-between;gap:10px;margin:2px 4px 10px}
.clip .head b{font-size:16px}
.btn.small{width:auto;margin:0;padding:7px 14px;font-size:14px}
.clip textarea{display:block;width:100%;min-height:88px;max-height:42vh;border-radius:13px;border:1px solid var(--line);background:rgba(0,0,0,.22);color:var(--fg);font:15px/1.5 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;padding:10px 12px;resize:vertical;-webkit-user-select:text;user-select:text}
.clip .empty{padding:14px 8px}
.clip .note2{color:var(--dim);font-size:12.5px;margin:9px 4px 0;text-align:center}
.file .fileicon{display:grid;place-items:center;height:96px;border-radius:13px;background:rgba(0,0,0,.22)}
.file .fileicon svg{width:44px;height:44px}
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

// Under 「全部下載（ZIP）」: how the ZIP's files get into Photos / the gallery.
std::string zipTips(const T& t, const std::string& kind) {
    const bool ios = kind == "ios", android = kind == "android", both = !ios && !android;
    std::string h;
    if (ios || both) {
        if (both) h += "<div class=\"os\">iPhone / iPad</div>";
        h += "<div class=\"tip\">" + t(S::PgZipHintIos) + "</div>";
    }
    if (android || both) {
        if (both) h += "<div class=\"os\">Android</div>";
        h += "<div class=\"tip\">" + t(S::PgZipHintAndroid) + "</div>";
    }
    return h;
}

// A document / other file (0.8.3, 拖檔到視窗): no preview, just its name and 下載檔案.
std::string fileIcon(const PagePalette& p) {
    return "<div class=\"fileicon\"><svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"" + hex(p.accent) +
           "\" stroke-width=\"1.6\" stroke-linecap=\"round\" stroke-linejoin=\"round\" aria-hidden=\"true\">"
           "<path d=\"M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z\"/><path d=\"M14 3v5h5\"/>"
           "<path d=\"M9 13h6\"/><path d=\"M9 17h4\"/></svg></div>";
}
bool isPicture(const PageFile& f) { return f.type.rfind("image/", 0) == 0; }

std::string card(const T& t, const PageFile& f, const std::string& kind, const PageExtra& ex, const PagePalette& pal) {
    const std::string i = std::to_string(f.index);
    const std::string name = htmlEscape(f.name);
    const bool file = !f.video && !isPicture(f);
    std::string h = std::string("<section class=\"card") + (file ? " file" : "") + "\" data-i=\"" + i + "\" data-name=\"" +
                    name + "\" data-type=\"" + htmlEscape(f.type) + "\" data-size=\"" + std::to_string(f.size) +
                    "\" data-video=\"" + (f.video ? "1" : "0") + "\"" + (file ? " data-file=\"1\"" : "") + ">";
    if (file) {
        h += fileIcon(pal);
        h += "<div class=\"meta\"><span>" + name + "</span><span>" + sizeText(f.size) + "</span></div>";
        h += "<div class=\"tip\">" + htmlEscape(ex.fileTip) + "</div>";
        h += "<a class=\"btn\" href=\"d/" + i + "\" download data-save=\"" + i + "\">" + htmlEscape(ex.downloadFile) + "</a>";
    } else if (!f.video) {
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
                      int ttlSeconds, const PageExtra* extra) {
    const T t{static_cast<Lang>(lang)};
    const PageExtra ex = extra ? *extra : PageExtra{};
    const bool ios = kind == "ios", android = kind == "android";
    std::string h = head(lang, t(S::PgTitle), pal);
    h += ">";
    const std::wstring app = appName;
    std::wstring count = std::to_wstring(files.size());
    h += "<header>" + logo(pal) + "<div><h1>" + t(S::PgTitle) + "</h1><p class=\"sub\" id=\"sub\">" +
         (files.size() == 1 ? t(S::PgSubOne, {app}) : t(S::PgSubMany, {app, count})) + "</p>";
    h += "</div></header>";
    // 0.8.3 電腦剪貼簿: the PC's copied text + 複製 (only while the owner shares; the script refreshes it).
    if (ex.clip) {
        h += "<section class=\"card clip\" id=\"clip\"><div class=\"head\"><b>" + htmlEscape(ex.clipTitle) +
             "</b><button class=\"btn ghost small\" id=\"cliprefresh\" type=\"button\">" + htmlEscape(ex.clipRefresh) +
             "</button></div><textarea id=\"cliptext\" readonly rows=\"4\"" + (ex.clipText.empty() ? " hidden" : "") + ">" +
             htmlEscape(ex.clipText) + "</textarea><p class=\"empty\" id=\"clipempty\"" + (ex.clipText.empty() ? "" : " hidden") +
             ">" + htmlEscape(ex.clipEmpty) + "</p><button class=\"btn\" id=\"clipcopy\" type=\"button\"" +
             (ex.clipText.empty() ? " disabled" : "") + ">" + htmlEscape(ex.clipCopy) + "</button>"
             "<p class=\"hint\" id=\"clipmsg\" hidden></p><p class=\"note2\">" + htmlEscape(ex.clipNote) + "</p></section>";
    }
    // 「全部下載（ZIP）」 for every browser (a plain link: works without the
    // script too); where the browser can share files (https / localhost only
    // -- not the LAN page's http) the script swaps it for 「全部儲存」.
    const std::string noFiles = files.empty() ? " hidden" : "";
    h += "<div class=\"bar\" id=\"bar\"" + noFiles + "><a class=\"btn\" id=\"zipall\" href=\"zip\" download>" +
         t(S::PgZipAll, {count}) + "</a><button class=\"btn\" id=\"saveall\" type=\"button\" hidden disabled>" +
         t(S::PgSaveAll, {count}) + "</button>";
    if (!android) h += "<p class=\"hint\" id=\"sharehint\" hidden>" + t(S::PgSaveAllHint) + "</p>";
    h += "</div><div class=\"ziphint\" id=\"ziphint\"" + noFiles + ">" + zipTips(t, kind) + "</div>";
    h += "<p class=\"note\" id=\"note\" hidden></p>";
    h += "<div id=\"list\">";
    for (const PageFile& f : files) h += card(t, f, kind, ex, pal);
    h += "</div>";
    h += "<footer>" + t(S::PgFooter, {std::wstring(untilHHMM.begin(), untilHHMM.end()),
                                      std::to_wstring((ttlSeconds + 59) / 60)}) + "</footer>";
    // Texts for the script (a data block: not executed, allowed by the CSP).
    std::string j = "{";
    jsonField(j, "app", u8(appName), true);
    jsonField(j, "subOne", u8(pm::i18n::tr(S::PgSubOne, t.lang)));
    jsonField(j, "subMany", u8(pm::i18n::tr(S::PgSubMany, t.lang)));
    jsonField(j, "save", t(S::PgSave));
    jsonField(j, "saved", t(S::PgSaved));
    jsonField(j, "saveAll", u8(pm::i18n::tr(S::PgSaveAll, t.lang)));
    jsonField(j, "allSaved", t(S::PgAllSaved));
    jsonField(j, "preparing", u8(pm::i18n::tr(S::PgPreparing, t.lang)));
    jsonField(j, "zipAll", u8(pm::i18n::tr(S::PgZipAll, t.lang)));
    jsonField(j, "shareFail", t(S::PgShareFail));
    jsonField(j, "clipCopy", ex.clipCopy);
    jsonField(j, "clipCopied", ex.clipCopied);
    jsonField(j, "clipFail", ex.clipFail);
    j += "}";
    h += "</main><script type=\"application/json\" id=\"pm-t\">" + j + "</script><script src=\"app.js\"></script>"
         "</body></html>\n";
    return h;
}

std::string expiredPage(int lang, const std::wstring& appName, const PagePalette& pal) {
    const T t{static_cast<Lang>(lang)};
    std::string h = head(lang, t(S::PgExpiredTitle), pal);
    h += "><section class=\"card gone\">" + logo(pal) + "<h1>" + t(S::PgExpiredTitle) + "</h1><p>" +
         t(S::PgExpiredText, {appName}) + "</p></section></main></body></html>\n";
    // the logo is a block; centre it
    const std::string from = "<section class=\"card gone\"><div class=\"logo\"";
    if (const size_t p = h.find(from); p != std::string::npos)
        h.replace(p, from.size(), "<section class=\"card gone\"><div class=\"logo\" style=\"margin:0 auto\"");
    return h;
}

std::string clipJson(const std::string& textUtf8) {
    std::string j = "{";
    jsonField(j, "text", textUtf8, true);
    return j + "}";
}

// The page's script. Progressive: without it the page works as before.
//  * 全部下載（ZIP）: the server-rendered link (zip = every file as one
//    stored ZIP). Web Share exists only in a secure context (https /
//    localhost), so on the phone's http LAN page this is the way to get all
//    at once. The script keeps its count current.
//  * 全部儲存 / 儲存 (only where navigator.canShare({files}) holds, i.e. not
//    over http; replaces the ZIP button): navigator.share({files}) with File objects built from
//    blobs fetched ahead (the share must start inside the tap: iOS refuses
//    it after an await), so the button reads 「準備中…」 until every unsaved
//    file (≤ 200 MB each) is in memory. Shared ones are marked 已儲存.
const std::string& shareScript() {
    static const std::string js = R"JS((function () {
'use strict';
var T = JSON.parse(document.getElementById('pm-t').textContent);
var list = document.getElementById('list');
var bar = document.getElementById('bar'), btn = document.getElementById('saveall');
var sub = document.getElementById('sub'), note = document.getElementById('note');
var CAP = 200 * 1024 * 1024, items = [], fetching = false;
function fill(s, a) { return s.replace(/\{(\d)\}/g, function (m, k) { return a[+k] !== undefined ? String(a[+k]) : m; }); }
var canFiles = false;
try {
  canFiles = !!(navigator.share && navigator.canShare && window.File &&
    navigator.canShare({ files: [new File(['x'], 'x.png', { type: 'image/png' })] }));
} catch (e) { canFiles = false; }
var zipA = document.getElementById('zipall'), zipHint = document.getElementById('ziphint');
var shareHint = document.getElementById('sharehint');
if (canFiles) {  // 全部儲存 replaces the ZIP (straight into Photos)
  zipA.hidden = true; zipHint.hidden = true; btn.hidden = false;
  if (shareHint) shareHint.hidden = false;
}
function say(s) { note.textContent = s || ''; note.hidden = !s; }
function adopt(c) {
  var it = { i: +c.getAttribute('data-i'), name: c.getAttribute('data-name'), type: c.getAttribute('data-type'),
    size: +c.getAttribute('data-size'), video: c.getAttribute('data-video') === '1', card: c,
    blob: null, failed: false, saved: false, btn: c.querySelector('[data-save]'),
    file: c.getAttribute('data-file') === '1' };
  items.push(it);
  if (canFiles && it.btn && it.size <= CAP && !it.file) {
    it.btn.textContent = T.save;
    it.btn.classList.remove('ghost');  // the share sheet saves to Photos: now the main way
    it.btn.addEventListener('click', function (ev) { if (!it.blob) return; ev.preventDefault(); share([it]); });
    var how = c.querySelectorAll('.howto, .tip, .os');
    for (var k = 0; k < how.length; k++) how[k].hidden = true;  // 儲存 replaces the long-press / Files-app steps
  }
  return it;
}
function pending() { return items.filter(function (x) { return !x.saved && !x.failed && x.size <= CAP && !x.file; }); }
function update() {
  sub.textContent = items.length === 1 ? fill(T.subOne, [T.app]) : fill(T.subMany, [T.app, items.length]);
  bar.hidden = items.length === 0;
  if (!canFiles) {
    zipHint.hidden = items.length === 0;
    zipA.textContent = fill(T.zipAll, [items.length]);
    return;
  }
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
// 0.8.3 電腦剪貼簿: refresh / copy (http page: no async clipboard API -> a selected textarea + execCommand)
var clip = document.getElementById('clip');
if (clip) {
  var ct = document.getElementById('cliptext'), ce = document.getElementById('clipempty');
  var cb = document.getElementById('clipcopy'), cm = document.getElementById('clipmsg');
  var showClip = function (t) { ct.value = t; ct.hidden = !t; ce.hidden = !!t; cb.disabled = !t; cm.hidden = true; };
  var loadClip = function () {
    fetch('clip', { cache: 'no-store' }).then(function (r) { return r.ok ? r.json() : null; })
      .then(function (j) { if (j) showClip(j.text || ''); }, function () {});
  };
  document.getElementById('cliprefresh').addEventListener('click', loadClip);
  document.addEventListener('visibilitychange', function () { if (!document.hidden) loadClip(); });
  cb.addEventListener('click', function () {
    var t = ct.value;
    if (!t) return;
    var done = function (ok) {
      cm.textContent = ok ? T.clipCopied : T.clipFail; cm.hidden = false;
      if (ok) { cb.textContent = T.clipCopied; setTimeout(function () { cb.textContent = T.clipCopy; }, 1600); }
    };
    var legacy = function () {
      var ok = false;
      ct.readOnly = false; ct.focus(); ct.select(); ct.setSelectionRange(0, t.length);
      try { ok = document.execCommand('copy'); } catch (e) { ok = false; }
      ct.readOnly = true; ct.blur();
      if (window.getSelection) window.getSelection().removeAllRanges();
      done(ok);
    };
    if (navigator.clipboard && window.isSecureContext) navigator.clipboard.writeText(t).then(function () { done(true); }, legacy);
    else legacy();
  });
}
var cards = list.querySelectorAll('.card[data-i]');
for (var k = 0; k < cards.length; k++) adopt(cards[k]);
update(); prefetch();
})();
)JS";
    return js;
}

}  // namespace pm::share
