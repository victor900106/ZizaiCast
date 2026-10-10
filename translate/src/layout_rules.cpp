// Table structure of a picture's lines (P0 of launch/_work/tr_arch/ARCHITECTURE.md
// §3.4, rules only): cells cut inside a detected line at wide character
// gaps, detector fragments of one line joined, groupLines for paragraphs,
// then the cells of one row joined into a key-value block (label + value).
// 0.7.2 had no rows: 「名 称 焼菓子」, 「熱 量 42kcal」 were five blocks, each
// dropped as "one letter" / "kanji only" / "number" (the owner's label).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "pm/translate.h"
#include "text_util.h"

namespace pm::translate {

namespace {

float hOf(const OcrLine& l) { return l.lineH > 0 ? l.lineH : l.y1 - l.y0; }
float hOf(const Block& b) { return b.lineH > 0 ? b.lineH : (b.y1 - b.y0) / std::max(1, b.lines); }
bool isLetterCjk(wchar_t c) {
    return (c >= 0x3041 && c <= 0x30FA) || c == 0x30FC || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) ||
           c == 0x3005 || isHangul(c);
}
std::wstring noSpace(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s)
        if (!iswspace(c) && c != 0x3000) o += c;
    return o;
}
// Joins two pieces of one line: no space between CJK characters (but one
// before / after hangul: Korean writes spaces), one between Latin words.
std::wstring joinText(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return a + b;
    const wchar_t x = a.back(), y = b.front();
    if (isHangul(x) || isHangul(y)) return a + L" " + b;
    if (isCjk(x) && isCjk(y)) return a + b;
    if (isCjk(x) != isCjk(y) && (iswdigit(x) || iswdigit(y))) return a + b;  // 8袋, 〒601
    return a + L" " + b;
}

// 1) Cells: a line cut where two characters are far apart (more than 2.5
// glyph heights between their centres: an empty table cell, a tab stop).  A
// spaced-out label (名　　称: Han only, up to 6 characters, even gaps) stays one.
std::vector<OcrLine> splitCells(const std::vector<OcrLine>& in, float aspect) {
    std::vector<OcrLine> out;
    for (const OcrLine& l : in) {
        const size_t n = l.text.size();
        if (l.vertical || l.charX.size() != n || n < 2) {
            out.push_back(l);
            continue;
        }
        const float h = hOf(l);
        std::vector<size_t> idx;  // non-space characters
        for (size_t i = 0; i < n; ++i)
            if (!iswspace(l.text[i])) idx.push_back(i);
        std::vector<size_t> cuts;  // cut before idx[k]
        float dmin = 1e9f, dmax = 0;
        bool hanOnly = idx.size() <= 6;
        for (size_t k = 0; k < idx.size(); ++k) hanOnly = hanOnly && l.text[idx[k]] >= 0x4E00 && l.text[idx[k]] <= 0x9FFF;
        for (size_t k = 1; k < idx.size(); ++k) {
            const float d = (l.charX[idx[k]] - l.charX[idx[k - 1]]) * aspect;
            dmin = std::min(dmin, d), dmax = std::max(dmax, d);
            if (d > 2.5f * h) cuts.push_back(k);
        }
        if (cuts.empty() || (hanOnly && dmax < 1.6f * dmin)) {
            out.push_back(l);
            continue;
        }
        cuts.push_back(idx.size());
        size_t from = 0;
        for (size_t c : cuts) {
            const size_t b = idx[from], e = idx[c - 1] + 1;
            OcrLine p = l;
            p.text = l.text.substr(b, e - b);
            p.charX.assign(l.charX.begin() + b, l.charX.begin() + e);
            const float half = 0.55f * h / aspect;
            p.x0 = std::max(l.x0, p.charX.front() - half);
            p.x1 = std::min(l.x1, p.charX.back() + half);
            p.script = detectScript(p.text);
            out.push_back(std::move(p));
            from = c;
        }
    }
    return out;
}

// 2) Detector fragments of one text line (a long label line read as two or
// three boxes: 「チョコレートコーチング」「(タイ製造)」「(砂糖、…」): same
// baseline and size, under a character apart, and the right one not the
// start of a column (a table's value column, where the left one is its label).
std::vector<OcrLine> mergeFragments(std::vector<OcrLine> ls, float aspect) {
    // b's left edge is a column: two more lines nearby start there too.
    auto startsColumn = [&](size_t bi, size_t ai) {
        const OcrLine& b = ls[bi];
        const float h = hOf(b), cy = (b.y0 + b.y1) / 2;
        int n = 0;
        for (size_t k = 0; k < ls.size(); ++k) {
            if (k == bi || k == ai) continue;
            const OcrLine& o = ls[k];
            const float oy = (o.y0 + o.y1) / 2;
            if (std::fabs(oy - cy) < 0.6f * h || std::fabs(oy - cy) > 6 * h) continue;
            if (std::fabs(o.x0 - b.x0) * aspect < 0.4f * h) ++n;
        }
        return n >= 2;
    };
    auto cjkLetters = [](const std::wstring& t) {
        int n = 0;
        for (wchar_t c : t) {
            if (iswspace(c)) continue;
            if (!isLetterCjk(c)) return -1;
            ++n;
        }
        return n;
    };
    std::vector<char> spacedOut(ls.size(), 0);
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t a = 0; a < ls.size() && !changed; ++a)
            for (size_t b = 0; b < ls.size() && !changed; ++b) {
                if (a == b) continue;
                const OcrLine &A = ls[a], &B = ls[b];
                if (A.vertical || B.vertical || A.text.empty() || B.text.empty()) continue;
                const float ha = hOf(A), hb = hOf(B), h = std::max(ha, hb);
                if (ha / hb > 1.35f || hb / ha > 1.35f) continue;
                if (A.bg >= 0 && B.bg >= 0 && std::fabs(A.bg - B.bg) > 0.3f) continue;
                const float ov = std::min(A.y1, B.y1) - std::max(A.y0, B.y0);
                if (ov < 0.6f * std::min(A.y1 - A.y0, B.y1 - B.y0)) continue;
                if (B.x0 < A.x0 + 0.5f * (A.x1 - A.x0)) continue;  // B to the right of A
                const float gap = (B.x0 - A.x1) * aspect;
                // Spaced-out label characters (名 称, 熱 量, 脂 質, 製 造): one
                // or two characters each, up to ~3 characters apart.
                const int na = cjkLetters(A.text), nb = cjkLetters(B.text);
                bool spacedLabel = na > 0 && nb > 0 && nb <= 2 && (na == 1 || (spacedOut[a] && na <= 4)) && gap < 3.2f * h &&
                                         gap > -0.3f * h;
                // A row of three or more single-glyph boxes (a grid of kanji
                // tiles 日 一 国, a tab bar's icons あ ▱ 単 字) is not a spaced-out
                // label: never joined into a pseudo-word (會年, 單字).
                if (spacedLabel && na == 1 && nb == 1 && !spacedOut[a]) {
                    int singles = 0;
                    for (size_t k = 0; k < ls.size(); ++k) {
                        const OcrLine& o = ls[k];
                        if (o.vertical || noSpace(o.text).size() != 1) continue;
                        const float oc = (o.y0 + o.y1) / 2, ac = (A.y0 + A.y1) / 2;
                        if (std::fabs(oc - ac) < 0.5f * h && hOf(o) / h > 0.7f && hOf(o) / h < 1.4f) ++singles;
                    }
                    if (singles >= 3) spacedLabel = false;
                    // Icons over their own labels (a tab bar: 単 over 単語, 字 over 漢字):
                    // each glyph has a line of its own right below it.
                    auto ownLineBelow = [&](const OcrLine& g, const OcrLine& other) {
                        for (const auto& o : ls) {
                            if (&o == &g || &o == &other || o.y0 < g.y1 - 0.2f * h || o.y0 - g.y1 > 1.2f * h) continue;
                            const bool underG = std::min(o.x1, g.x1) - std::max(o.x0, g.x0) > 0;
                            const bool underOther = std::min(o.x1, other.x1) - std::max(o.x0, other.x0) > 0;
                            if (underG && !underOther) return true;
                        }
                        return false;
                    };
                    if (ownLineBelow(A, B) && ownLineBelow(B, A)) spacedLabel = false;
                }
                if (!spacedLabel) {
                    if (gap > 0.9f * h || gap < -0.8f * h) continue;
                    // A Japanese label and its English gloss side by side (文法 Grammar,
                    // 漢字 Kanji): two texts, not one line (「文法文」, 「康司」).
                    auto latinOnly = [](const std::wstring& t) {
                        bool any = false;
                        for (wchar_t c : t) {
                            if (iswspace(c)) continue;
                            if (!((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'))) return false;
                            any = true;
                        }
                        return any;
                    };
                    if (((na > 0 && latinOnly(B.text)) || (nb > 0 && latinOnly(A.text))) && gap > 0.25f * h) continue;
                    if (startsColumn(b, a)) continue;
                }
                static const bool dbgMerge = std::getenv("PM_LAYOUT_DEBUG") != nullptr;
                if (dbgMerge) std::fprintf(stderr, "[layout] merge %ls + %ls (gap %.2f h%s)\n", A.text.c_str(), B.text.c_str(), gap / h, spacedLabel ? ", spaced" : "");
                OcrLine m = A;
                m.text = spacedLabel ? A.text + B.text : joinText(A.text, B.text);
                if (A.charX.size() == A.text.size() && B.charX.size() == B.text.size() &&
                    m.text.size() == A.text.size() + B.text.size()) {
                    m.charX.insert(m.charX.end(), B.charX.begin(), B.charX.end());
                } else {
                    m.charX.clear();
                }
                m.x0 = std::min(A.x0, B.x0), m.x1 = std::max(A.x1, B.x1);
                m.y0 = std::min(A.y0, B.y0), m.y1 = std::max(A.y1, B.y1);
                m.conf = std::min(A.conf, B.conf);
                m.lineH = std::max(A.lineH, B.lineH);
                if (A.bg >= 0 && B.bg >= 0) m.bg = (A.bg + B.bg) / 2;
                m.script = detectScript(m.text);
                ls[a] = std::move(m);
                spacedOut[a] = spacedLabel ? 1 : 0;
                ls.erase(ls.begin() + b);
                spacedOut.erase(spacedOut.begin() + b);
                changed = true;
            }
    }
    return ls;
}

// A value cell made of a number: 42kcal, 0.4g, 8袋（16枚）, 380円, 9,000원, 26.12.09, 100-240V ~ 50/60Hz.
bool numberLike(const std::wstring& t) {
    const ScriptCount n = countScripts(t);
    return n.digits > 0 && n.letters() <= n.digits + 4 && n.kana <= 2;
}

// A row's first cell: a field name / short heading, not a sentence or a number.
bool labelLike(const Block& b) {
    if (b.lines != 1 || b.labelLen) return false;
    const std::wstring t = noSpace(b.text);
    if (t.empty() || numberLike(t)) return false;
    const wchar_t e = t.back();
    if (e == L'。' || e == L'.' || e == L'!' || e == L'?' || e == L'！' || e == L'？' || e == L'、' || e == L',') return false;
    const ScriptCount n = countScripts(t);
    if (n.letters() == 0) return false;
    if (n.latin == 0) return t.size() <= 10;
    int words = 1;
    for (wchar_t c : b.text) words += c == L' ';
    return words <= 3 && b.text.size() <= 24;
}

}  // namespace

namespace {
std::vector<Block> layoutStraight(const std::vector<OcrLine>& lines, float aspect);

// The dominant baseline angle of a tilted photo (a label shot at an angle:
// -17 degrees): median of the horizontal lines' angles, when most agree.
// 0 = straight enough (< 3 degrees) or no agreement.
float dominantAngle(const std::vector<OcrLine>& lines) {
    std::vector<float> a;
    for (const auto& l : lines)
        if (!l.vertical && l.text.size() >= 3) a.push_back(l.angle);
    if (a.size() < 4) return 0;
    std::sort(a.begin(), a.end());
    const float med = a[a.size() / 2];
    size_t close = 0;
    for (float x : a) close += std::fabs(x - med) < 0.08f;
    if (std::fabs(med) < 0.05f || close * 10 < a.size() * 7 || std::fabs(med) > 0.6f) return 0;
    return med;
}
}  // namespace

// A tilted picture: the lines turned into an upright frame (around the
// picture's centre), the rows and cells found there, the blocks' boxes turned
// back (their axis-aligned bounds in the picture).  0.7.x paired the label
// cells of a -17 degree label with the next row's values (品原材料名…450g).
// Icons the recogniser reads as symbol characters (Safari's bookmark button
// as 「□」 / ⎗ next to the URL, a lock, a share arrow): not text.  Dropped:
// symbol characters fonts mostly lack (technical symbols, dingbats, emoji,
// private use, unassigned), and a lone symbol standing as a word of its own
// (「www.streetfighter.com □」) - a leading bullet (● 名称, ・, ※) stays.
namespace {
bool rareSymbol(wchar_t c) {
    return (c >= 0x2300 && c <= 0x23FF) || (c >= 0x2194 && c <= 0x21FF) || (c >= 0x2700 && c <= 0x27BF && c != 0x2713 && c != 0x2714) ||
           (c >= 0x2B00 && c <= 0x2BFF) || (c >= 0xE000 && c <= 0xF8FF) || (c >= 0xD800 && c <= 0xDFFF) || c == 0xFFFD ||
           (c >= 0x2600 && c <= 0x26FF && c != 0x2605 && c != 0x2606 && c != 0x260E);
}
bool symbolGlyph(wchar_t c) {
    return rareSymbol(c) || (c >= 0x25A0 && c <= 0x25FF) || c == 0x2605 || c == 0x2606 || c == 0x260E || c == L'口';
}
}  // namespace

std::wstring dropIconGlyphs(const std::wstring& t) {
    std::wstring o;
    for (wchar_t c : t)
        if (!rareSymbol(c)) o += c;
    // へ べ ぺ look the same in hiragana and katakana: inside a katakana word
    // (レべル read for レベル - then 「條線」) the katakana one.
    auto kata = [](wchar_t c) { return (c >= 0x30A1 && c <= 0x30FA) || c == 0x30FC; };
    for (size_t i = 1; i + 1 < o.size(); ++i)
        if ((o[i] == 0x3078 || o[i] == 0x3079 || o[i] == 0x307A) && kata(o[i - 1]) && kata(o[i + 1])) o[i] = static_cast<wchar_t>(o[i] + 0x60);
    // A lone symbol word after text (not a leading bullet).
    std::wstring r;
    for (size_t i = 0; i < o.size(); ++i) {
        const bool lone = symbolGlyph(o[i]) && i > 0 && (o[i - 1] == L' ' || o[i - 1] == 0x3000) &&
                          (i + 1 == o.size() || o[i + 1] == L' ' || o[i + 1] == 0x3000);
        if (!lone) r += o[i];
    }
    while (!r.empty() && (r.back() == L' ' || r.back() == 0x3000)) r.pop_back();
    size_t b = 0;
    while (b < r.size() && (r[b] == L' ' || r[b] == 0x3000)) ++b;
    return r.substr(b);
}

std::vector<Block> layoutBlocks(const std::vector<OcrLine>& linesIn, float aspect) {
    std::vector<OcrLine> lines;
    for (const auto& l : linesIn) {
        OcrLine c = l;
        c.text = dropIconGlyphs(l.text);
        if (!c.text.empty()) lines.push_back(std::move(c));
    }
    const float th = dominantAngle(lines);
    if (th == 0) return layoutStraight(lines, aspect);
    const float c = std::cos(th), s = std::sin(th);
    const float xc = 0.5f * aspect, yc = 0.5f;  // picture centre, in picture-height units
    std::vector<OcrLine> up = lines;
    for (OcrLine& l : up) {
        if (l.vertical) continue;
        const float X = (l.x0 + l.x1) / 2 * aspect - xc, Y = (l.y0 + l.y1) / 2 - yc;
        // The line's own length and thickness from its axis-aligned box.
        const float ca = std::fabs(std::cos(l.angle)), sa = std::fabs(std::sin(l.angle)), d = ca * ca - sa * sa;
        const float w = (l.x1 - l.x0) * aspect, h = l.y1 - l.y0;
        float L = d > 0.3f ? (w * ca - h * sa) / d : w, H = d > 0.3f ? (h * ca - w * sa) / d : h;
        if (L <= 0 || H <= 0) L = w, H = h;
        const float Xr = c * X + s * Y, Yr = -s * X + c * Y;
        const float cx = (l.x0 + l.x1) / 2;
        for (float& x : l.charX) x = (Xr + xc + (x - cx) * aspect / std::max(0.3f, std::cos(l.angle))) / aspect;
        l.x0 = (Xr - L / 2 + xc) / aspect, l.x1 = (Xr + L / 2 + xc) / aspect;
        l.y0 = Yr - H / 2 + yc, l.y1 = Yr + H / 2 + yc;
        l.angle -= th;
    }
    static const bool dbg = std::getenv("PM_LAYOUT_DEBUG") != nullptr;
    if (dbg)
        for (const auto& l : up)
            std::fprintf(stderr, "[layout] upright %.3f %.3f %.3f %.3f h %.4f %s\n", l.x0, l.y0, l.x1, l.y1, l.lineH, toUtf8(l.text).c_str());
    std::vector<Block> bs = layoutStraight(up, aspect);
    auto back = [&](float& x0, float& y0, float& x1, float& y1) {
        float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
        for (int k = 0; k < 4; ++k) {
            const float Xr = (k & 1 ? x1 : x0) * aspect - xc, Yr = (k & 2 ? y1 : y0) - yc;
            const float X = c * Xr - s * Yr, Y = s * Xr + c * Yr;
            bx0 = std::min(bx0, X), bx1 = std::max(bx1, X), by0 = std::min(by0, Y), by1 = std::max(by1, Y);
        }
        x0 = std::clamp((bx0 + xc) / aspect, 0.f, 1.f), x1 = std::clamp((bx1 + xc) / aspect, 0.f, 1.f);
        y0 = std::clamp(by0 + yc, 0.f, 1.f), y1 = std::clamp(by1 + yc, 0.f, 1.f);
    };
    for (Block& b : bs) {
        float gx0 = b.x0, gx1 = b.x1;
        if (b.cy1 > b.cy0) back(gx0, b.cy0, gx1, b.cy1);
        back(b.x0, b.y0, b.x1, b.y1);
    }
    return bs;
}

namespace {
std::vector<Block> layoutStraight(const std::vector<OcrLine>& lines, float aspect) {
    std::vector<OcrLine> ls = mergeFragments(splitCells(lines, aspect), aspect);
    // Row hints: the field label (内容量, 殺菌方法 …) to the left of a line, on
    // its row.  Two lines with different labels are different rows (a label
    // table's value cells are not one paragraph: 気密性容器… / 別紙記載).
    for (size_t i = 0; i < ls.size(); ++i) {
        OcrLine& l = ls[i];
        if (l.vertical || isFieldLabel(l.text)) continue;
        float best = 1e9f;
        for (size_t k = 0; k < ls.size(); ++k) {
            const OcrLine& f = ls[k];
            if (k == i || f.vertical || !isFieldLabel(f.text) || f.x1 > l.x0 + 0.01f) continue;
            const float fc = (f.y0 + f.y1) / 2, gap = l.x0 - f.x1;
            if (fc < l.y0 || fc > l.y1 || gap * aspect > 8 * std::max(hOf(f), hOf(l))) continue;
            if (gap < best) best = gap, l.rowLabel = static_cast<int>(k);
        }
    }
    std::vector<Block> bs = groupLines(ls, aspect);
    std::vector<char> spaced(bs.size(), 0);  // made of single spaced-out characters (名 称)
    auto rowBand = [](const Block& a, const Block& b) {
        // a (one line) sits in b's row: its centre inside b (several lines) or most of it overlapping.
        const float cy = (a.y0 + a.y1) / 2;
        if (b.lines > 1) return cy > b.y0 && cy < b.y1;
        const float ov = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
        return ov >= 0.5f * std::min(a.y1 - a.y0, b.y1 - b.y0);
    };
    auto join = [&](size_t ai, size_t bi, bool row, int kind) {
        Block& A = bs[ai];
        const Block& B = bs[bi];
        Block m = A;
        if (row) {
            m.labelLen = A.text.size();
            m.text = A.text + L" " + B.text;
            m.kind = kind;
            m.lines = std::max(A.lines, B.lines) + (kind == 3 ? 1 : 0);
        } else {
            m.text = A.text + B.text;
        }
        m.x0 = std::min(A.x0, B.x0), m.x1 = std::max(A.x1, B.x1);
        m.y0 = std::min(A.y0, B.y0), m.y1 = std::max(A.y1, B.y1);
        m.cy0 = std::min(A.cy0, B.cy0), m.cy1 = std::max(A.cy1, B.cy1);
        m.conf = std::min(A.conf, B.conf);
        m.lineH = std::max(A.lineH, B.lineH);
        m.lang = detectScript(m.text);
        if (m.kind == 3) m.kind = 2;
        A = std::move(m);
        bs.erase(bs.begin() + bi);
        spaced.erase(spaced.begin() + bi);
    };
    // Known field labels take their values first (別紙記載, a value, must not
    // take the next row's date before 調理方法 takes it).
    for (int pass = 0; pass < 2; ++pass)
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t a = 0; a < bs.size() && !changed; ++a) {
            const Block& A = bs[a];
            if (A.lines != 1 || A.labelLen) continue;
            if (pass == 0 && !isFieldLabel(A.text)) continue;
            const std::wstring at = noSpace(A.text);
            size_t best = SIZE_MAX;
            float bestGap = 1e9f;
            bool stacked = false;
            for (size_t b = 0; b < bs.size(); ++b) {
                if (b == a) continue;
                const Block& B = bs[b];
                const float h = std::max(hOf(A), hOf(B));
                // (a stamped date / amount may be much larger than its label: 2007.11.30)
                const float maxRatio = numberLike(B.text) ? 2.4f : 1.67f;
                if (hOf(A) / hOf(B) > maxRatio || hOf(B) / hOf(A) > maxRatio) continue;
                if (B.x0 >= A.x1 - 0.3f * h / aspect && rowBand(A, B)) {
                    // Gap in glyph heights; of two rows the label sits between,
                    // the one it overlaps most.
                    const float ov = std::min(A.y1, B.y1) - std::max(A.y0, B.y0);
                    const float g = (B.x0 - A.x1) * aspect / h - (B.lines == 1 ? ov / std::max(1e-6f, A.y1 - A.y0) : 0.f);
                    // Nothing else between them on the row.
                    bool between = false;
                    for (size_t c = 0; c < bs.size() && !between; ++c)
                        if (c != a && c != b && bs[c].x0 >= A.x1 - 0.01f && bs[c].x1 <= B.x0 + 0.01f && rowBand(bs[c], B))
                            between = true;
                    if (!between && g < bestGap) best = b, bestGap = g, stacked = false;
                } else if (B.lines == 1 && B.y0 >= A.y1 - 0.2f * h && (B.y0 - A.y1) < 1.0f * h &&
                           std::min(A.x1, B.x1) - std::max(A.x0, B.x0) > 0.5f * std::min(A.x1 - A.x0, B.x1 - B.x0) &&
                           !B.labelLen && countScripts(B.text).letters() <= countScripts(B.text).digits &&
                           countScripts(A.text).digits == 0 && labelLike(A)) {
                    if (bestGap > 0.5f) best = b, bestGap = 0.5f, stacked = true;  // 賞味期限 over 26.12.09
                }
            }
            if (best == SIZE_MAX) continue;
            const Block& B = bs[best];
            const std::wstring bt = noSpace(B.text);
            if (stacked) {
                join(a, best, true, 3);
                changed = true;
                continue;
            }
            // Spaced-out label characters: 名 称, 熱 量, 製 造 (one character, then more).
            const bool cjkA = !at.empty() && std::all_of(at.begin(), at.end(), isLetterCjk);
            const bool cjkB = !bt.empty() && std::all_of(bt.begin(), bt.end(), isLetterCjk);
            // Not glyph tiles / icons: a row of three or more single glyphs (日 一 国,
            // a tab bar's あ 単 字), or two glyphs each over a label of its own
            // (単 over 単語, 字 over 漢字) - never joined into a pseudo-word.
            bool tiles = false;
            if (at.size() == 1 && bt.size() == 1 && !spaced[a]) {
                const float h = std::max(hOf(A), hOf(B));
                int singles = 0;
                for (const Block& o : bs)
                    if (o.lines == 1 && noSpace(o.text).size() == 1 && std::fabs((o.y0 + o.y1) / 2 - (A.y0 + A.y1) / 2) < 0.5f * h &&
                        hOf(o) > 0.7f * h && hOf(o) < 1.4f * h)
                        ++singles;
                auto ownBelow = [&](const Block& g, const Block& other) {
                    for (const Block& o : bs) {
                        if (&o == &g || &o == &other || o.y0 < g.y1 - 0.2f * h || o.y0 - g.y1 > 1.2f * h) continue;
                        if (std::min(o.x1, g.x1) - std::max(o.x0, g.x0) > 0 && std::min(o.x1, other.x1) - std::max(o.x0, other.x0) <= 0) return true;
                    }
                    return false;
                };
                tiles = singles >= 3 || (ownBelow(A, B) && ownBelow(B, A));
            }
            if (!tiles && cjkA && cjkB && B.lines == 1 && !B.labelLen && bestGap < 3.2f && bt.size() <= 2 &&
                (at.size() == 1 || (spaced[a] && at.size() <= 4))) {
                join(a, best, false, 0);
                spaced[a] = 1;
                changed = true;
                continue;
            }
            if (!labelLike(A) || B.labelLen) continue;
            const bool num = numberLike(B.text);
            if (bestGap > (num ? 30.f : 6.f)) continue;
            // Far apart only for a known field name / a label ending with a colon.
            const bool colon = at.back() == L'：' || at.back() == L':';
            // A one-character label is an icon read as a letter (Safari's ✕ as
            // 「X」, Google Translate's G): never a row's label.
            if (countScripts(at).letters() < 2 && !isFieldLabel(A.text)) continue;
            if (bestGap > 2.5f && !num && !isFieldLabel(A.text) && !colon) continue;
            // Two standalone items side by side are not 「標籤　值」: a tab bar /
            // button row (ホーム | はじめる, 許可しない | 許可する), a roster of
            // names (ARJUN | YASMINE).  A text value needs a known field name or
            // a colon; English rows always do (owner 0.7.6).
            if (!num && !isFieldLabel(A.text) && !colon) {
                const float r = hOf(A) / std::max(1e-6f, hOf(B));
                if (countScripts(A.text).latin > 0 || (labelLike(B) && r < 1.35f && r > 1 / 1.35f)) continue;
            }
            join(a, best, true, num ? 2 : 1);
            changed = true;
        }
    }
    // 3) More number cells of a quantity row (나트륨 350mg | 18%, 脂質 1.5g |
    // 2%): appended to its value, so the row is listed whole.
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t a = 0; a < bs.size() && !changed; ++a) {
            const Block& A = bs[a];
            if (!A.labelLen || A.kind != 2) continue;
            size_t best = SIZE_MAX;
            float bestGap = 1e9f;
            for (size_t b = 0; b < bs.size(); ++b) {
                if (b == a) continue;
                const Block& B = bs[b];
                if (B.labelLen || B.lines != 1 || !numberLike(B.text)) continue;
                const float h = std::max(hOf(A), hOf(B));
                if (hOf(A) / hOf(B) > 1.67f || hOf(B) / hOf(A) > 1.67f) continue;
                if (B.x0 < A.x1 - 0.3f * h / aspect) continue;
                const float ov = std::min(A.y1, B.y1) - std::max(A.y0, B.y0);
                if (ov < 0.5f * (B.y1 - B.y0)) continue;
                const float g = (B.x0 - A.x1) * aspect / h;
                if (g < 12.f && g < bestGap) best = b, bestGap = g;
            }
            if (best == SIZE_MAX) continue;
            Block& M = bs[a];
            const Block& B = bs[best];
            bool between = false;  // nothing else on the row between them
            for (size_t c = 0; c < bs.size() && !between; ++c)
                if (c != a && c != best && bs[c].x0 >= M.x1 - 0.01f && bs[c].x1 <= B.x0 + 0.01f && rowBand(bs[c], B)) between = true;
            if (between) continue;
            M.text += L" " + B.text;
            M.x0 = std::min(M.x0, B.x0), M.x1 = std::max(M.x1, B.x1);
            M.y0 = std::min(M.y0, B.y0), M.y1 = std::max(M.y1, B.y1);
            M.cy0 = std::min(M.cy0, B.cy0), M.cy1 = std::max(M.cy1, B.cy1);
            M.conf = std::min(M.conf, B.conf);
            bs.erase(bs.begin() + best);
            spaced.erase(spaced.begin() + best);
            changed = true;
        }
    }
    return bs;
}
}  // namespace

}  // namespace pm::translate
