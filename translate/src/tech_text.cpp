// Technical messages around the translation engine (English -> zh-Hant): a
// work chat's exception reports and test results - part numbers, test codes,
// identifiers, 「[標籤] 值」 lines, Chinese and English side by side.  The
// rules here decide what is kept as written; label_text.cpp (translateTextsEx)
// applies them.  0.7.9 sent every such line through the engine: codes were
// recased (PN: -> Pn:, QX/V7/RF/FAIL -> Qx/v7/rf/fail) or respelled (a
// letter of a model code or an identifier changed), team acronyms translated
// as everyday words, bracket labels read as sentences ([Owner] as a person
// who owns something) and the Chinese half of a bilingual label translated
// again through English (the 日期 of [日期Date] doubled).
#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#include "pm/translate.h"
#include "text_util.h"

namespace pm::translate {

namespace {

bool asciiLetter(wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'); }
bool asciiDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }
bool hanChar(wchar_t c) { return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0xF900 && c <= 0xFAFF); }
bool spaceChar(wchar_t c) { return c == L' ' || c == L'\t' || c == 0x3000; }

std::wstring squeezeLower(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (spaceChar(c)) {
            if (!o.empty() && o.back() != L' ') o += L' ';
            continue;
        }
        o += static_cast<wchar_t>(std::towlower(c));
    }
    while (!o.empty() && o.back() == L' ') o.pop_back();
    return o;
}

// Field labels of exception reports / test reports in English ([Date] 03/14,
// [Responsible person] TE): the zh-TW words of a factory floor.  Matched
// whole, case and spacing ignored.
struct TechLabel {
    const wchar_t* en;
    const wchar_t* zh;
};
const TechLabel kTechLabels[] = {
    {L"Date", L"日期"},
    {L"Time", L"時間"},
    {L"Line", L"線別"},
    {L"Model", L"機種"},
    {L"Product", L"產品"},
    {L"Project", L"專案"},
    {L"Customer", L"客戶"},
    {L"Station", L"站別"},
    {L"Stage", L"站別"},
    {L"Shift", L"班別"},
    {L"Location", L"地點"},
    {L"Area", L"區域"},
    {L"Responsible person", L"負責人"},
    {L"Person in charge", L"負責人"},
    {L"Owner", L"負責人"},
    {L"PIC", L"負責人"},
    {L"Reporter", L"回報人"},
    {L"Problem description", L"問題描述"},
    {L"Issue description", L"問題描述"},
    {L"Description", L"描述"},
    {L"Problem", L"問題"},
    {L"Issue", L"問題"},
    {L"Symptom", L"不良現象"},
    {L"Defect", L"不良現象"},
    {L"Failure mode", L"失效模式"},
    {L"Whether to stop the line", L"是否停線"},
    {L"Stop the line", L"是否停線"},
    {L"Line stop", L"是否停線"},
    {L"Line down", L"是否停線"},
    {L"Treatment action", L"處理動作"},
    {L"Action", L"處理動作"},
    {L"Corrective action", L"矯正措施"},
    {L"Containment action", L"圍堵措施"},
    {L"Root cause", L"根本原因"},
    {L"Status", L"狀態"},
    {L"Result", L"結果"},
    {L"Impact", L"影響"},
    {L"Priority", L"優先級"},
    {L"Next step", L"下一步"},
    {L"Due date", L"預計完成日"},
    {L"ETA", L"預計完成"},
    {L"Remark", L"備註"},
    {L"Remarks", L"備註"},
    {L"Note", L"備註"},
    {L"Part number", L"料號"},
    {L"PN", L"料號"},
    {L"P/N", L"料號"},
    {L"Serial number", L"序號"},
    {L"SN", L"序號"},
    {L"S/N", L"序號"},
    {L"Lot", L"批號"},
    {L"Lot number", L"批號"},
    {L"Qty", L"數量"},
    {L"Quantity", L"數量"},
    {L"Input", L"投入數"},
    {L"Output", L"產出數"},
    {L"Yield", L"良率"},
    {L"Version", L"版本"},
};

// Terms of test reports the engine reads as everyday words (top fail ->
// 「頂級失敗」 / 「最失敗者」 / 「頂點失敗」 on one screen): one zh-TW form.
struct TechTerm {
    const wchar_t* en;
    const wchar_t* zh;
};
const TechTerm kTechTerms[] = {  // (longest first)
    {L"top fail is", L"主要失敗項為"},
    {L"top fails are", L"主要失敗項為"},
    {L"top fail", L"主要失敗項"},
    {L"top fails", L"主要失敗項"},
    {L"fail rate", L"失敗率"},
    {L"failure rate", L"失敗率"},
    {L"retest rate", L"重測率"},
    {L"pass rate", L"通過率"},
    {L"first pass yield", L"首次通過率"},
    {L"root cause", L"根本原因"},
    {L"stop the line", L"停線"},
    {L"timeout", L"逾時"},  // (a results table's TimeOut column: 「時間之外」)
};

// Strips the punctuation around a word (「(QA)」, 「issue.」; 「QA/」 keeps its slash).
std::wstring stripWord(const std::wstring& w) {
    static const wchar_t* const kEdge = L",.;:!?()[]{}\"'*，。：；！？（）［］【】「」『』、";
    size_t b = 0, e = w.size();
    while (b < e && wcschr(kEdge, w[b])) ++b;
    while (e > b && wcschr(kEdge, w[e - 1])) --e;
    return w.substr(b, e - b);
}

std::vector<std::wstring> words(const std::wstring& t) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : t) {
        if (spaceChar(c)) {
            if (!cur.empty()) out.push_back(cur), cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// A number, date, time, percentage or range (03/14, 8:15, 3.50%, 7-2): no letters.
bool numberWord(const std::wstring& w) {
    bool digit = false;
    for (wchar_t c : w) {
        if (asciiDigit(c)) digit = true;
        else if (!wcschr(L".,:/%-+~#", c)) return false;
    }
    return digit;
}

// Symbols between codes (-, &, ->, ***, /): no letters or digits.
bool symbolWord(const std::wstring& w) {
    for (wchar_t c : w)
        if (asciiLetter(c) || asciiDigit(c) || hanChar(c)) return false;
    return true;
}

}  // namespace

bool acronymWord(const std::wstring& raw) {
    const std::wstring w = stripWord(raw);
    if (w.size() < 2 || w.size() > 6) return false;
    int letters = 0;
    for (wchar_t c : w) {
        if (c >= L'A' && c <= L'Z') ++letters;
        else if (!asciiDigit(c) && c != L'&') return false;
    }
    return letters >= 2;
}

bool codeToken(const std::wstring& raw) {
    const std::wstring w = stripWord(raw);
    if (w.empty() || std::any_of(w.begin(), w.end(), [](wchar_t c) { return c > 0x7F; })) return false;  // ASCII codes only
    // A word with an apostrophe (QA's, TE's, can't): 0.7.10 read QA's as capitals then small
    // letters (NVMe) and kept 「QA's」 in the translation; the acronym alone is kept.
    if (w.find(L'\'') != std::wstring::npos) return false;
    if (w.find(L"://") != std::wstring::npos || w.rfind(L"www.", 0) == 0) return true;
    int upper = 0, lower = 0, digits = 0, under = 0, joins = 0, humps = 0;
    bool glued = false;  // letters touching digits (5F, v2); humps: a small letter then a capital (iPhone, CheckDdrTrainLog)
    for (size_t i = 0; i < w.size(); ++i) {
        const wchar_t c = w[i];
        if (c >= L'A' && c <= L'Z') ++upper;
        else if (c >= L'a' && c <= L'z') ++lower;
        else if (asciiDigit(c)) ++digits;
        else if (c == L'_') ++under;
        else if (c == L'/' || c == L'-' || c == L'.' || c == L':') ++joins;
        if (i > 0) {
            humps += (w[i - 1] >= L'a' && w[i - 1] <= L'z') && (c >= L'A' && c <= L'Z');
            glued |= (asciiDigit(w[i - 1]) && asciiLetter(c)) || (asciiLetter(w[i - 1]) && asciiDigit(c));
        }
    }
    const int letters = upper + lower;
    if (letters == 0) return false;
    {
        // A number with a unit in small letters (100mg, 3pm): the units are localised (毫克).
        size_t k = 0;
        while (k < w.size() && (asciiDigit(w[k]) || w[k] == L'.' || w[k] == L',')) ++k;
        if (k > 0 && w.size() - k <= 4 && lower == static_cast<int>(w.size() - k)) return false;
    }
    if (under > 0) return true;                                  // ZX9_DDR_MEM002.004, v2_1
    if (digits > 0 && (upper > 0 || glued)) return true;         // KM220P, Q7-RX12, 731.2KQX4.0081, 4410278K (not 2-point)
    // CheckDdrTrainLog, QXFWStatus, iPhone - not TimeOut, YouTube (one hump after a capital: a word)
    if (humps >= 2 || (humps == 1 && (std::iswlower(w[0]) || upper >= 3))) return true;
    if (upper >= 2 && lower > 0) {
        // NVMe, LEDs (capitals, then small letters): not Hello
        size_t k = 0;
        while (k < w.size() && std::iswupper(w[k])) ++k;
        if (k >= 2) return true;
    }
    if (lower == 0 && upper >= 2 && joins > 0 && (w.find(L'/') != std::wstring::npos || w.find(L'-') != std::wstring::npos)) {
        // QX/RF/FAIL, QA/, ICT-QA - not words in capitals (GUN-HUNTING, ALL-ALUMINIUM)
        size_t run = 0, longest = 0;
        for (wchar_t c : w) run = asciiLetter(c) ? run + 1 : 0, longest = std::max(longest, run);
        return longest <= 5;
    }
    return false;
}

std::vector<std::pair<size_t, size_t>> codeSpans(const std::wstring& s) {
    std::vector<std::pair<size_t, size_t>> out;
    static const wchar_t* const kEdge = L",.;:!?()[]{}\"'*，。：；！？（）［］【】「」『』、";
    for (size_t i = 0; i < s.size();) {
        if (spaceChar(s[i]) || isCjk(s[i])) {
            ++i;
            continue;
        }
        size_t e = i;
        while (e < s.size() && !spaceChar(s[e]) && !isCjk(s[e])) ++e;
        size_t b = i, f = e;
        while (b < f && wcschr(kEdge, s[b])) ++b;
        while (f > b && wcschr(kEdge, s[f - 1])) --f;
        const std::wstring w = s.substr(b, f - b);
        // Slash-joined: whole when every part is a code / acronym (MI-like A100/A200,
        // QX/V7/RF/FAIL); else its code parts alone (Photo/V850: the word is translated).
        bool whole = codeToken(w);
        std::vector<std::pair<size_t, size_t>> parts;
        if (whole && w.find(L'/') != std::wstring::npos) {
            for (size_t p = 0; p <= w.size();) {
                size_t q = w.find(L'/', p);
                if (q == std::wstring::npos) q = w.size();
                const std::wstring part = w.substr(p, q - p);
                if (!part.empty()) {
                    if (codeToken(part)) parts.push_back({b + p, q - p});
                    else if (!acronymWord(part) && !numberWord(part)) whole = false;
                }
                p = q + 1;
            }
        }
        if (whole) out.push_back({b, f - b});
        else
            for (const auto& pr : parts) out.push_back(pr);
        i = e;
    }
    return out;
}

bool techLine(const std::wstring& t) {
    // A strong code: underscores, 2+ letters with 2+ digits, or 2+ humps (not B12, V800, iPhone, COVID).
    for (const auto& [pos, len] : codeSpans(t)) {
        const std::wstring w = t.substr(pos, len);
        int letters = 0, digits = 0, humps = 0;
        for (size_t i = 0; i < w.size(); ++i) {
            letters += asciiLetter(w[i]);
            digits += asciiDigit(w[i]);
            humps += i > 0 && w[i - 1] >= L'a' && w[i - 1] <= L'z' && w[i] >= L'A' && w[i] <= L'Z';
        }
        if (w.find(L"://") == std::wstring::npos && (w.find(L'_') != std::wstring::npos || (letters >= 2 && digits >= 2) || humps >= 2))
            return true;
    }
    // A report's [label] (a known one, or Chinese and English).
    for (size_t p = t.find_first_of(L"[【［"); p != std::wstring::npos; p = t.find_first_of(L"[【［", p + 1)) {
        BracketLine b;
        if (parseBracketLine(t.substr(p), b) && (!b.labelZh.empty() || techLabel(b.label, Lang::ZhHant))) return true;
    }
    return false;
}

bool shoutedLine(const std::wstring& t) {
    // 0.7.10 kept a work chat's message in capitals as written (every word an
    // acronym / a code: PLEASE CHECK THE KX200 SAMPLES ASAP).  Team names and
    // codes are short (PN QA TE RD KX200); a sentence has longer words.
    int caps = 0, longCaps = 0;
    for (const auto& w0 : words(t)) {
        const std::wstring w = stripWord(w0);
        if (w.size() < 2 || !std::all_of(w.begin(), w.end(), [](wchar_t c) { return c >= L'A' && c <= L'Z'; })) continue;
        ++caps;
        longCaps += w.size() >= 5;
    }
    return caps >= 3 && longCaps >= 2;
}

bool codeLine(const std::wstring& t) {
    int code = 0;
    const auto ws = words(t);
    if (ws.empty() || shoutedLine(t)) return false;
    for (const auto& w : ws) {
        if (symbolWord(w) || numberWord(stripWord(w))) continue;
        if (codeToken(w)) {
            ++code;
            continue;
        }
        if (acronymWord(w)) continue;
        return false;
    }
    return code > 0;
}

const wchar_t* techLabel(const std::wstring& label, Lang tgt) {
    if (tgt != Lang::ZhHant) return nullptr;
    const std::wstring q = squeezeLower(label);
    for (const auto& l : kTechLabels)
        if (q == squeezeLower(l.en)) return l.zh;
    // Letters the OCR confuses in a label (0.7.10 sent [ModeI] to the engine: 「[M00 和 M0 的]」):
    // a capital I / 1 / | after a small letter is an l (ModeI), a leading 1 a T (1ime).
    std::wstring f = label;
    for (size_t i = 0; i < f.size(); ++i)
        if (wcschr(L"I1|", f[i]) && i > 0 && f[i - 1] >= L'a' && f[i - 1] <= L'z') f[i] = L'l';
    if (!f.empty() && f[0] == L'1' && f.size() > 1 && f[1] >= L'a' && f[1] <= L'z') f[0] = L'T';
    if (f != label) {
        const std::wstring fq = squeezeLower(f);
        for (const auto& l : kTechLabels)
            if (fq == squeezeLower(l.en)) return l.zh;
    }
    return nullptr;
}

std::vector<GlossaryHit> techTermHits(const std::wstring& text, Lang src, Lang tgt) {
    std::vector<GlossaryHit> out;
    if (src != Lang::En || tgt != Lang::ZhHant) return out;
    std::wstring low;
    for (wchar_t c : text) low += static_cast<wchar_t>(std::towlower(c));
    for (const auto& t : kTechTerms) {
        const std::wstring k = t.en;
        for (size_t p = low.find(k); p != std::wstring::npos; p = low.find(k, p + 1)) {
            const size_t e = p + k.size();
            if ((p > 0 && asciiLetter(low[p - 1])) || (e < low.size() && asciiLetter(low[e]))) continue;  // whole words
            bool overlap = false;
            for (const auto& h : out) overlap |= p < h.pos + h.len && h.pos < e;
            if (!overlap) out.push_back({p, k.size(), t.zh});
        }
    }
    std::sort(out.begin(), out.end(), [](const GlossaryHit& a, const GlossaryHit& b) { return a.pos < b.pos; });
    return out;
}

std::vector<std::wstring> splitBracketItems(const std::wstring& t) {
    std::vector<std::wstring> out;
    auto open = [](wchar_t c) { return c == L'[' || c == L'【' || c == L'［'; };
    auto close = [](wchar_t c) { return c == L']' || c == L'】' || c == L'］'; };
    size_t from = 0;
    int depth = 0;
    for (size_t i = 0; i < t.size(); ++i) {
        if (open(t[i])) {
            // A new 「[標籤]」 item: at depth 0, after a space or a closed bracket's value.
            if (depth == 0 && i > from && (spaceChar(t[i - 1]) || close(t[i - 1]) || asciiDigit(t[i - 1]))) {
                std::wstring piece = t.substr(from, i - from);
                while (!piece.empty() && spaceChar(piece.back())) piece.pop_back();
                if (!piece.empty()) out.push_back(piece);
                from = i;
            }
            ++depth;
        } else if (close(t[i]) && depth > 0) {
            --depth;
        }
    }
    std::wstring last = t.substr(from);
    while (!last.empty() && spaceChar(last.back())) last.pop_back();
    if (!last.empty()) out.push_back(last);
    // Only report items: a later one is a known / bilingual label ([Time]8:15,
    // [狀態 Status]) - not buttons in a sentence (Press [OK] then [Cancel]).
    bool report = false;
    for (size_t k = 1; k < out.size() && !report; ++k) {
        BracketLine b;
        report = parseBracketLine(out[k], b) && (!b.labelZh.empty() || techLabel(b.label, Lang::ZhHant));
    }
    if (!report) return {t};
    return out;
}

bool parseBracketLine(const std::wstring& t, BracketLine& out) {
    out = {};
    if (t.empty() || !(t[0] == L'[' || t[0] == L'【' || t[0] == L'［')) return false;
    out.open = t[0];
    size_t c = std::wstring::npos;
    for (size_t i = 1; i < t.size(); ++i)
        if (t[i] == L']' || t[i] == L'】' || t[i] == L'］') {
            c = i;
            break;
        } else if (t[i] == L'[' || t[i] == L'【' || t[i] == L'［') {
            return false;
        }
    std::wstring inner = t.substr(1, c == std::wstring::npos ? std::wstring::npos : c - 1);
    std::wstring rest = c == std::wstring::npos ? L"" : t.substr(c + 1);
    if (c != std::wstring::npos) out.close = t[c];
    // A label with its colon inside the bracket ([Problem description:]), or
    // the closing bracket lost and the value after the colon ([Corrective action:QA/).
    const size_t colon = inner.find_first_of(L":：");
    if (colon != std::wstring::npos) {
        std::wstring after = inner.substr(colon + 1);
        while (!after.empty() && spaceChar(after.front())) after.erase(0, 1);
        if (!after.empty() && c != std::wstring::npos) return false;  // [a: b] is not a label
        out.colonInside = true;
        if (!after.empty()) rest = after;
        inner = inner.substr(0, colon);
    }
    while (!inner.empty() && spaceChar(inner.back())) inner.pop_back();
    while (!inner.empty() && spaceChar(inner.front())) inner.erase(0, 1);
    // The closing bracket read as a letter ([Treatment Actionl): a known label before it.
    if (!out.close && !out.colonInside && inner.size() > 2 && wcschr(L"lI|1", inner.back()) &&
        techLabel(inner.substr(0, inner.size() - 1), Lang::ZhHant)) {
        inner.pop_back();
        out.close = L']';
    }
    while (!rest.empty() && spaceChar(rest.front())) rest.erase(0, 1);
    if (!rest.empty() && (rest[0] == L':' || rest[0] == L'：')) {
        out.colonAfter = true;
        rest.erase(0, 1);
        while (!rest.empty() && spaceChar(rest.front())) rest.erase(0, 1);
    }
    if (inner.empty() || inner.size() > 40) return false;
    // A label is a few words, not a sentence in brackets.
    if (words(inner).size() > 6 && countScripts(inner).han == 0) return false;
    // Chinese and English side by side ([日期 Date], [負責人Responsible person]):
    // the Chinese, its first run (the rest is its English gloss).
    size_t h = 0;
    while (h < inner.size() && !hanChar(inner[h])) ++h;
    if (h < inner.size()) {
        size_t e = h;
        while (e < inner.size() && (hanChar(inner[e]) || (isCjk(inner[e]) && !spaceChar(inner[e])))) ++e;
        out.labelZh = inner.substr(h, e - h);
    }
    out.label = inner;
    out.value = rest;
    return true;
}

std::vector<MixedPiece> splitMixed(const std::wstring& t, bool glosses) {
    // Runs: Chinese (Han, CJK punctuation) and the rest (Latin, digits, ASCII).
    struct Run {
        std::wstring s;
        bool cjk;
    };
    std::vector<Run> runs;
    for (wchar_t c : t) {
        const bool cj = isCjk(c) && c != 0x3000;
        if (runs.empty() || (runs.back().cjk != cj && !(spaceChar(c) && !runs.back().cjk))) runs.push_back({std::wstring(1, c), cj});
        else runs.back().s += c;
    }
    // English words of a run in a row (plain words: letters only, not codes,
    // not acronyms): please send the log to QA -> 5.
    auto phrase = [](const std::wstring& s, bool* capFirst, bool* pure) {
        int best = 0, cur = 0, plain = 0, other = 0;
        bool first = true, cap = false;
        for (const auto& w0 : words(s)) {
            const std::wstring w = stripWord(w0);
            bool letters = !w.empty();
            for (wchar_t c : w) letters = letters && (asciiLetter(c) || c == L'\'');
            const bool word = letters && !codeToken(w) && (!acronymWord(w) || w == L"I");
            if (word) {
                if (first) cap = std::iswupper(w[0]) != 0;
                ++cur, ++plain;
                best = std::max(best, cur);
            } else {
                cur = 0;
                if (!symbolWord(w0)) ++other;
            }
            first = false;
        }
        if (capFirst) *capFirst = cap;
        if (pure) *pure = other == 0 && plain > 0;
        return best;
    };
    std::vector<MixedPiece> out;
    for (size_t i = 0; i < runs.size(); ++i) {
        const Run& r = runs[i];
        if (r.cjk) {
            out.push_back({r.s, MixedPiece::Keep});
            continue;
        }
        bool cap = false, pure = false;
        const int n = phrase(r.s, &cap, &pure);
        // A Chinese label's English gloss right after it (品質異常 Quality
        // alert, 林小明 Ming): the Chinese says it already.
        // Only at the end: English between Chinese words is a name (請到 Build Hall 二樓).
        // A gloss is a few words after a short label - not a sentence (設定 Changes
        // are saved automatically. is translated).
        const bool afterHan = i > 0 && runs[i - 1].cjk && hanChar(runs[i - 1].s.back()) && runs[i - 1].s.size() <= 8;
        const bool atEnd = i + 1 == runs.size();
        const bool sentence = r.s.find_first_of(L".!?") != std::wstring::npos;
        if (glosses && afterHan && atEnd && pure && cap && !sentence && n >= 1 && n <= 3) {
            out.push_back({r.s, MixedPiece::Drop});
            continue;
        }
        out.push_back({r.s, n >= 3 ? MixedPiece::Translate : MixedPiece::Keep});
    }
    return out;
}

}  // namespace pm::translate
