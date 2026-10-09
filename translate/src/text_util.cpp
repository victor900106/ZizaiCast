#include "text_util.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <regex>

#include "pm/i18n.h"

namespace pm::translate {

namespace {
bool isKana(wchar_t c) {
    return (c >= 0x3040 && c <= 0x30FF && c != 0x30FB && c != 0x30FC) || (c >= 0x31F0 && c <= 0x31FF) ||
           (c >= 0xFF66 && c <= 0xFF9F);
}
bool isHangul(wchar_t c) {
    return (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F);
}
bool isHan(wchar_t c) { return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0xF900 && c <= 0xFAFF); }
bool isLatin(wchar_t c) {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) ||
           (c >= 0xFF21 && c <= 0xFF3A) || (c >= 0xFF41 && c <= 0xFF5A);
}
}  // namespace

bool isCjk(wchar_t c) {
    return isKana(c) || isHangul(c) || isHan(c) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xFF00 && c <= 0xFF65) ||
           c == 0x30FB || c == 0x30FC;
}

ScriptCount countScripts(const std::wstring& s) {
    ScriptCount n;
    for (wchar_t c : s) {
        if (isKana(c)) ++n.kana;
        else if (isHangul(c)) ++n.hangul;
        else if (isHan(c)) ++n.han;
        else if (isLatin(c)) ++n.latin;
        else if (iswdigit(c) || (c >= 0xFF10 && c <= 0xFF19)) ++n.digits;
        else if (!iswspace(c)) ++n.other;
    }
    return n;
}

Lang detectScript(const std::wstring& text) {
    const ScriptCount n = countScripts(text);
    const int letters = n.letters();
    if (letters == 0) return Lang::Unknown;
    if (n.hangul > 0 && n.hangul * 3 >= n.hangul + n.han + n.kana) return Lang::Ko;
    if (n.kana > 0 && n.kana * 10 >= n.kana + n.han) return Lang::Ja;
    if (n.han > 0 && n.han * 2 >= letters) {
        const Lang v = chineseVariant(text);
        return v == Lang::Unknown ? Lang::ZhHant : v;
    }
    if (n.latin > 0) return Lang::En;
    return Lang::Unknown;
}

namespace {
std::wstring lcmap(const std::wstring& s, DWORD flag) {
    if (s.empty()) return s;
    const int n = LCMapStringEx(L"zh-CN", flag, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr, 0);
    if (n <= 0) return s;
    std::wstring out(static_cast<size_t>(n), L'\0');
    LCMapStringEx(L"zh-CN", flag, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr, 0);
    return out;
}
}  // namespace

std::wstring toTraditional(const std::wstring& s) {
    std::wstring t = lcmap(s, LCMAP_TRADITIONAL_CHINESE);
    // LCMapStringEx maps character by character: the common one-to-many
    // Simplified characters stay wrong (饼干 -> 餅干, 置于, 制品).  Fixed by word.
    static const struct {
        const wchar_t *from, *to;
    } kWords[] = {{L"餅干", L"餅乾"}, {L"干燥", L"乾燥"}, {L"曬干", L"曬乾"}, {L"晒干", L"曬乾"}, {L"干淨", L"乾淨"},
                  {L"制品", L"製品"}, {L"制造", L"製造"}, {L"制作", L"製作"}, {L"制成", L"製成"}, {L"面粉", L"麵粉"},
                  {L"面條", L"麵條"}, {L"面包", L"麵包"}, {L"方便面", L"方便麵"}, {L"拉面", L"拉麵"}, {L"里面", L"裡面"},
                  {L"這里", L"這裡"}, {L"那里", L"那裡"}, {L"哪里", L"哪裡"}, {L"凈含量", L"淨含量"}, {L"于", L"於"}};
    for (const auto& w : kWords)
        for (size_t at = t.find(w.from); at != std::wstring::npos; at = t.find(w.from, at + wcslen(w.to)))
            t.replace(at, wcslen(w.from), w.to);
    return t;
}
std::wstring toSimplified(const std::wstring& s) { return lcmap(s, LCMAP_SIMPLIFIED_CHINESE); }

Lang chineseVariant(const std::wstring& s) {
    const bool isTrad = toTraditional(s) == s, isSimp = toSimplified(s) == s;
    if (isTrad && !isSimp) return Lang::ZhHant;
    if (isSimp && !isTrad) return Lang::ZhHans;
    return Lang::Unknown;
}

std::string toUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring fullWidthPunctuation(const std::wstring& s) {
    std::wstring out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        const wchar_t c = out[i];
        wchar_t fw = 0;
        switch (c) {
        case L',': fw = 0xFF0C; break;
        case L'?': fw = 0xFF1F; break;
        case L'!': fw = 0xFF01; break;
        case L':': fw = 0xFF1A; break;
        case L';': fw = 0xFF1B; break;
        case L'(': fw = 0xFF08; break;
        case L')': fw = 0xFF09; break;
        default: continue;
        }
        // Previous / next non-space character is CJK (not between digits: 10:30, 3,980).
        wchar_t prev = 0, next = 0;
        for (size_t j = i; j-- > 0;)
            if (out[j] != L' ') {
                prev = out[j];
                break;
            }
        for (size_t j = i + 1; j < out.size(); ++j)
            if (out[j] != L' ') {
                next = out[j];
                break;
            }
        if (iswdigit(prev) && iswdigit(next)) continue;
        if ((prev && isCjk(prev)) || (c != L'(' && !next && prev && isCjk(prev)) || (next && isCjk(next) && c != L')')) {
            out[i] = fw;
            // Drop the spaces around it.
            if (i + 1 < out.size() && out[i + 1] == L' ') out.erase(i + 1, 1);
            if (i > 0 && out[i - 1] == L' ') {
                out.erase(i - 1, 1);
                --i;
            }
        }
    }
    // A space between two CJK characters (sentence joins).
    for (size_t i = 1; i + 1 < out.size(); ++i)
        if (out[i] == L' ' && isCjk(out[i - 1]) && isCjk(out[i + 1])) out.erase(i--, 1);
    return out;
}

bool looksMisread(const std::wstring& t) {
    // Components that stand in for kana / hangul strokes; rare in real UI text.
    static const std::wstring kOdd = L"卜丩匚乇亻冫丿匕夕囗乚礻弖仺卫刁冂勹丬彡攵丶亅乛乀廴辶屮丨巛";
    int odd = 0, han = 0;
    for (wchar_t c : t) {
        if (c >= 0x3100 && c <= 0x312F) ++odd;  // bopomofo inside recognised text
        else if (kOdd.find(c) != std::wstring::npos) ++odd, ++han;
        else if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF)) ++han;
    }
    return odd >= 4 && odd * 8 >= han;
}

const wchar_t* langTag(Lang l) {
    switch (l) {
    case Lang::Ja: return L"ja";
    case Lang::Ko: return L"ko";
    case Lang::En: return L"en";
    case Lang::ZhHans: return L"zh-Hans";
    case Lang::ZhHant: return L"zh-Hant";
    default: return L"";
    }
}

std::wstring langName(Lang l) {
    using pm::i18n::S;
    switch (l) {
    case Lang::Ja: return pm::i18n::tr(S::TrLangJa);
    case Lang::Ko: return pm::i18n::tr(S::TrLangKo);
    case Lang::En: return pm::i18n::tr(S::TrLangEn);
    case Lang::ZhHans: return pm::i18n::tr(S::TrLangZhHans);
    case Lang::ZhHant: return pm::i18n::tr(S::TrLangZhHant);
    default: return L"?";
    }
}

Lang defaultTarget() {
    switch (pm::i18n::lang()) {
    case pm::i18n::Lang::En: return Lang::En;
    case pm::i18n::Lang::Ja: return Lang::Ja;
    case pm::i18n::Lang::Ko: return Lang::Ko;
    default: return Lang::ZhHant;
    }
}

// ---------------------------------------------------------------------------
// Lines -> blocks

namespace {
// ln starts a new item of a label / sign / menu rather than continuing p.
bool separateItem(const OcrLine& p, const OcrLine& ln) {
    const ScriptCount a = countScripts(p.text), b = countScripts(ln.text);
    // Prices, quantities, numbers (8,000원, 850円, 29 g).
    if (a.digits >= a.letters() || b.digits >= b.letters()) return true;
    // Table labels stacked in a column (内容量 / 保存方法, 熱量 / たんぱく質):
    // field names, or two short label-like lines.
    if (isFieldLabel(p.text) || isFieldLabel(ln.text)) return true;
    if (a.latin == 0 && b.latin == 0 && a.letters() <= 5 && b.letters() <= 5 && a.other == 0 && b.other == 0) return true;
    // An address / phone line is a row of its own (製造者: 株式会社… 〒601-8446 … / お客様相談室 0120-…).
    static const std::wregex phone(L"[0-9]{2,4}-[0-9]{2,4}-[0-9]{2,4}");
    if (looksLikeAddress(p.text) || looksLikeAddress(ln.text) || std::regex_search(p.text, phone) || std::regex_search(ln.text, phone))
        return true;
    // A field label "配料：…", "保存方法：…" (a colon within the first few characters).
    const size_t colon = ln.text.find_first_of(L"：:");
    if (colon != std::wstring::npos && colon > 0 && colon <= 8 && isCjk(ln.text.front())) return true;
    // Latin: an all-capitals heading (CAUTION), or a new capitalised line after
    // a line that does not continue (no comma / hyphen at its end).
    if (a.latin >= 3 && a.letters() == a.latin) {
        bool upper = true;
        for (wchar_t c : p.text) upper &= !(c >= L'a' && c <= L'z');
        if (upper && b.latin > 0) {
            bool lower = false;
            for (wchar_t c : ln.text) lower |= c >= L'a' && c <= L'z';
            if (lower) return true;
        }
    }
    if (b.latin > 0 && !ln.text.empty() && iswupper(ln.text.front()) && !p.text.empty()) {
        const wchar_t e = p.text.back();
        if (e != L',' && e != L'-' && e != L'(' && !(ln.text.size() > 1 && ln.text[0] == L'I' && ln.text[1] == L' ')) return true;
    }
    return false;
}

bool endsSentence(const std::wstring& t) {
    if (t.empty()) return true;
    const wchar_t c = t.back();
    if (c == 0x3002 || c == 0xFF01 || c == 0xFF1F || c == L'!' || c == L'?' || c == 0xFF1A || c == L':') return true;
    // Japanese polite sentence endings without a 。 (banners, signs: …仕上げました).
    // Korean: a full stop after hangul, the polite endings (…합니다, …주세요).
    if (c == L'.' && t.size() >= 2 && t[t.size() - 2] >= 0xAC00 && t[t.size() - 2] <= 0xD7AF) return true;
    for (const wchar_t* e : {L"ました", L"ます", L"ません", L"です", L"でした", L"ください", L"니다", L"세요", L"어요", L"아요",
                             L"불가", L"금지"})  // Korean notices: …주문 불가, 촬영 금지
        if (t.size() >= wcslen(e) && t.compare(t.size() - wcslen(e), std::wstring::npos, e) == 0) return true;
    return false;
}
}  // namespace

std::vector<Block> groupLines(const std::vector<OcrLine>& lines, float aspect) {
    std::vector<Block> out;
    struct Open {
        size_t block;
        OcrLine last;
        float pitch = 0;  // centre distance of the block's lines so far (0: one line)
    };
    std::vector<Open> open;
    // x distances in "y units" (line heights are vertical): x * aspect.
    // Was line p wrapped (the first word of ln would not have fitted after
    // it)?  The paragraph's right edge: the widest line starting at p's left
    // edge within a few line heights.
    auto wrapped = [&](const OcrLine& p, const OcrLine& ln) {
        const float h = p.lineH > 0 ? p.lineH : p.y1 - p.y0;
        float right = p.x1;
        for (const OcrLine& o : lines) {
            const float oc = (o.y0 + o.y1) / 2;
            if (oc < p.y0 - 6 * h || oc > ln.y1 + 6 * h) continue;
            if (std::fabs(o.x0 - p.x0) * aspect > 1.5f * h) continue;
            right = std::max(right, o.x1);
        }
        // First word of ln (CJK: one character ~ the line height).
        float word;
        const bool cjk = !ln.text.empty() && isCjk(ln.text.front()) && !p.text.empty() && isCjk(p.text.back());
        if (cjk) {
            word = (ln.lineH > 0 ? ln.lineH : ln.y1 - ln.y0) / aspect;
        } else {
            const size_t sp = ln.text.find(L' ');
            const size_t n = sp == std::wstring::npos ? ln.text.size() : sp;
            word = (ln.x1 - ln.x0) * static_cast<float>(n + 1) / std::max<size_t>(1, ln.text.size());
        }
        // CJK wraps at any character, but lines of a photographed label end a
        // character or two early (line-break rules, perspective, uneven lines):
        // two characters of slack for long lines (not for menu items / labels).
        const bool longLine = (p.x1 - p.x0) * aspect > 10 * h;
        return p.x1 + word > right - (cjk && longLine ? 2.f : 0.3f) * h / aspect;
    };
    // Height across the line and the vertical gap between two lines, also for
    // tilted lines (a photographed label): measured along each line's own
    // centre line at the middle of their horizontal overlap.
    auto height = [](const OcrLine& l) { return l.lineH > 0 ? l.lineH : l.y1 - l.y0; };
    auto centreY = [&](const OcrLine& l, float x) {
        const float slope = std::tan(l.angle) * aspect;  // dy / dx in 0..1 units
        return (l.y0 + l.y1) / 2 + slope * (x - (l.x0 + l.x1) / 2);
    };
    // The text band of a line (a tilted line's box is much taller than its glyphs).
    auto cardTop = [&](const OcrLine& l) { return l.lineH > 0 ? std::max(l.y0, (l.y0 + l.y1) / 2 - 0.55f * l.lineH) : l.y0; };
    auto cardBottom = [&](const OcrLine& l) { return l.lineH > 0 ? std::min(l.y1, (l.y0 + l.y1) / 2 + 0.55f * l.lineH) : l.y1; };
    for (const OcrLine& ln : lines) {
        const float h = height(ln);
        int target = -1;
        for (size_t k = 0; k < open.size(); ++k) {
            const OcrLine& p = open[k].last;
            const float ph = height(p);
            if (h <= 0 || ph <= 0 || h / ph > 1.5f || ph / h > 1.5f) continue;  // glyph boxes: "order." is short
            if (p.vertical || ln.vertical) continue;                              // columns stay apart
            if (p.bg >= 0 && ln.bg >= 0 && std::fabs(p.bg - ln.bg) > 0.3f) continue;  // white on black vs black on white
            const float xm = (std::max(p.x0, ln.x0) + std::min(p.x1, ln.x1)) / 2;
            const float gap = (centreY(ln, xm) - h / 2) - (centreY(p, xm) + ph / 2), hm = std::max(h, ph);
            if (gap < -0.3f * hm || gap > 0.9f * hm) continue;  // glyph heights (PaddleOCR): wide UI line spacing still joins
            const float ov = std::min(p.x1, ln.x1) - std::max(p.x0, ln.x0);
            const float minW = std::min(p.x1 - p.x0, ln.x1 - ln.x0);
            if (ov < 0.5f * minW) continue;
            // Left edges near each other (or centred lines).
            const float dl = std::fabs(p.x0 - ln.x0) * aspect, dc = std::fabs((p.x0 + p.x1) - (ln.x0 + ln.x1)) * 0.5f * aspect;
            if (dl > 1.6f * hm && dc > 1.6f * hm) continue;
            if (endsSentence(p.text) && !(p.script == Lang::En && ln.script == Lang::En)) continue;
            if (p.rowLabel >= 0 && ln.rowLabel >= 0 && p.rowLabel != ln.rowLabel) continue;  // two rows of a label table
            if (separateItem(p, ln)) continue;
            if (!wrapped(p, ln)) continue;  // a separate UI line (label, price, status)
            // Same line spacing as the block so far (a table row below a wrapped cell).
            const float pitch = centreY(ln, xm) - centreY(p, xm);
            if (open[k].pitch > 0 && pitch > 1.3f * open[k].pitch) continue;
            target = static_cast<int>(k);
            break;
        }
        if (target >= 0) {
            Block& b = out[open[target].block];
            // Korean wraps at the spaces between words: one comes back between hangul lines.
            const bool hangulWrap = !b.text.empty() && !ln.text.empty() && isHangul(b.text.back()) && isHangul(ln.text.front());
            const bool space = hangulWrap || (!b.text.empty() && !isCjk(b.text.back()) && !ln.text.empty() && !isCjk(ln.text.front()));
            if (space) b.text += L' ';
            b.text += ln.text;
            b.x0 = std::min(b.x0, ln.x0);
            b.y0 = std::min(b.y0, ln.y0);
            b.x1 = std::max(b.x1, ln.x1);
            b.y1 = std::max(b.y1, ln.y1);
            b.cy0 = std::min(b.cy0, cardTop(ln));
            b.cy1 = std::max(b.cy1, cardBottom(ln));
            b.conf = std::min(b.conf, ln.conf);
            b.lineH = std::max(b.lineH, height(ln));
            ++b.lines;
            const float xm = (std::max(open[target].last.x0, ln.x0) + std::min(open[target].last.x1, ln.x1)) / 2;
            const float pitch = centreY(ln, xm) - centreY(open[target].last, xm);
            open[target].pitch = open[target].pitch > 0 ? std::min(open[target].pitch, pitch) : pitch;
            open[target].last = ln;
        } else {
            Block b;
            b.text = ln.text;
            b.x0 = ln.x0;
            b.y0 = ln.y0;
            b.x1 = ln.x1;
            b.y1 = ln.y1;
            b.cy0 = cardTop(ln);
            b.cy1 = cardBottom(ln);
            b.conf = ln.conf;
            b.lineH = height(ln);
            b.lines = 1;
            out.push_back(std::move(b));
            open.push_back({out.size() - 1, ln, 0.f});
        }
        // Forget blocks far above (keeps the search short and local).
        open.erase(std::remove_if(open.begin(), open.end(),
                                  [&](const Open& o) { return ln.y0 - o.last.y1 > 3 * height(o.last); }),
                   open.end());
    }
    for (Block& b : out) b.lang = detectScript(b.text);
    return out;
}

}  // namespace pm::translate
