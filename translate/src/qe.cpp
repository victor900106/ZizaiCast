// Rule checks of a translation (P0 of launch/_work/tr_arch/ARCHITECTURE.md
// §3.8): negation kept (and its scope over a list), numbers kept.  Microseconds,
// no model.  translateTexts re-translates what fails (rewritten source,
// clause by clause) and marks what still fails.
#include <algorithm>
#include <regex>
#include <string>
#include <vector>

#include "pm/translate.h"
#include "text_util.h"

namespace pm::translate {

namespace {


std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

}  // namespace

// ---- Negation checks, driven by the tables below -------------------------
// To add a negation word or a false friend, add it to a table; the two
// functions (negationCues for the source, negationKept for the target) only
// walk the rows.
namespace {

using Words = std::vector<const wchar_t*>;

// A source language's cue table.
struct SourceNegRules {
    Lang lang;
    bool fold;                // look at the lower-cased text (English)
    Words notNegations;       // look like a cue but are not (blanked first): 少ない / 危ない, obligations
    const wchar_t* cue;       // regex of the cue words (the matches are NegCues::words)
    Words underWords;         // any of them: 未満 / "under" (NegCues::under)
    const wchar_t* sign;      // regex of sign prohibitions (No smoking, Do not enter): one "prohibit" cue; nullptr: none
    const wchar_t* avoid;     // the "avoid" word whose list scope matters (A、B、Cを避けて); nullptr: none
    const wchar_t* listSeps;  // separators before it that make a list
    std::wregex cueRe, signRe;  // compiled once (filled by sourceRules)
};

const std::vector<SourceNegRules>& sourceRules() {
    static const std::vector<SourceNegRules> rules = [] {
        std::vector<SourceNegRules> v = {
            {Lang::Ja,
             false,
             // Not negations: 少ない / 危ない, and obligations (〜なければならない =
             // must: 「必須」 has no negation word and is right).
             {L"少ない", L"少なく", L"危ない", L"危なく", L"いただけない", L"かもしれない", L"なければならない", L"なければなりません",
              L"なければいけない", L"なければいけません", L"なくてはならない", L"なくてはなりません", L"なくてはいけない",
              L"なくてはいけません", L"ないといけない", L"ないといけません", L"ないとならない"},
             // 無断 (無断転載禁止, 無断で使用できません) / 許可なく / 禁じます: a
             // prohibition the translation must keep (禁止 / 不得 / 不可), not
             // 「未經許可使用」 alone.
             L"(無断|許可なく|禁じ|ない|なく|ません|ずに|せず|避け|控え|禁止|厳禁|不可|ご遠慮|おやめ|やめて|無用|未開封|できません|しないで|ないで|禁制|お断り|対象外)",
             {L"未満"},
             nullptr,
             L"避け",
             L"、・や",
             {},
             {}},
            {Lang::Ko,
             false,
             {},
             L"(않|못하|못 |금지|마세요|마십시오|말아|말고|말 것|피하|피해|삼가|불가|없습니다|없어|없음|없는|안 되|안 돼)",
             {},
             nullptr,
             L"피하",
             L",",
             {},
             {}},
            {Lang::En,
             true,
             {},
             L"(\\bnot\\b|n't|\\bno\\b|\\bnever\\b|\\bavoid|away from|\\bwithout\\b|prohibit|\\bcannot\\b|\\bunable\\b|keep out|\\bnor\\b)",
             {L"under ", L"less than"},
             // Sign prohibitions (No Woman Admitted, NO ADMISSION, No smoking, Do not
             // enter): a prohibition, not an absence (「沒有女性會接受」 passed before).
             L"(^|\\b)(no\\b.*\\b(admitted|allowed|permitted|admission|entry|smoking|parking|photos?|photography|food|pets)\\b|do not|don't|keep out|forbidden|prohibited|not allowed|not permitted)",
             nullptr,
             nullptr,
             {},
             {}},
        };
        for (SourceNegRules& r : v) {
            r.cueRe.assign(r.cue);
            if (r.sign) r.signRe.assign(r.sign);
        }
        return v;
    }();
    return rules;
}

// Cue words that are a prohibition (NegCues::prohibit), not just an absence.
const Words& prohibitCues() {
    static const Words w = {L"避け", L"控え", L"禁止", L"厳禁", L"不可", L"ご遠慮", L"おやめ", L"やめて", L"禁制", L"お断り", L"対象外",
                            L"禁じ", L"無断", L"許可なく", L"no admission", L"admitted", L"allowed", L"permitted", L"no entry",
                            L"no smoking", L"no parking", L"keep out", L"prohibit", L"do not", L"don't", L"forbidden", L"しないで",
                            L"ないで", L"금지", L"마세요", L"마십시오", L"말아", L"말고", L"말 것", L"피하", L"피해", L"삼가", L"불가",
                            L"안 되", L"안 돼"};
    return w;
}

// Verb negations of a source: two of them need two in the target (値段表示の
// ない露店で買わない: "no price shown" and "do not buy").
const Words& verbNegCues() {
    static const Words w = {L"ない", L"なく", L"ません", L"ずに", L"せず", L"않"};
    return w;
}

// The zh-Hant target's table.
struct ZhNegRules {
    Words notNegations;       // words with a negation character that do not negate (分別 「separately」
                              // let 「불가」 go unnoticed: 「將魷魚分別訂購…」 for 「1인분 주문 불가」), blanked first
    Words negWords;           // any of them: the target negates
    Words underWords;         // NegCues::under: 以下 counts as kept
    const wchar_t* negChars;  // a verb-negation pair needs two of these
    Words prohibitWords;      // a prohibition needs one of these, not an absence (無 / 未 / 沒 / 非)
    Words broken;             // 「將室溫存放於…」 (store "the room temperature"): the pivot's broken
                              // reading of …を避けて常温で保存, with or without a list
    std::vector<std::wregex> listScopeBad;  // list scope (直射日光、高温多湿な場所を避けて常温で保存): the pivot
                                            // makes 「存放於遠離陽光直射、高溫且潮濕的環境中」 - a storage place
                                            // governing the avoided list, read as "store in a hot, humid place"
};

const ZhNegRules& zhRules() {
    static const ZhNegRules r = {
        {L"分別", L"特別", L"區別", L"個別", L"類別", L"性別", L"級別", L"告別", L"別人", L"別的", L"別處", L"未來", L"無線", L"免費",
         L"防水", L"防曬", L"防腐", L"預防", L"消防", L"非常", L"無論", L"不論", L"不久", L"不斷", L"不錯", L"差不多", L"不同"},
        {L"不", L"勿", L"禁", L"避", L"遠離", L"無", L"未", L"沒", L"別", L"免", L"防", L"非", L"莫", L"拒", L"停止", L"否", L"切忌", L"以外"},
        {L"以下", L"未滿", L"不到", L"低於", L"不足"},
        L"不勿禁避無未沒別免非莫拒否",
        {L"不", L"勿", L"禁", L"避", L"遠離", L"別", L"以免", L"防止", L"莫", L"拒", L"停止", L"切忌", L"無法", L"以外", L"謝絕", L"恕"},
        {L"將室溫", L"將常溫"},
        {std::wregex(L"(存放|保存|儲存|放置|置於|放在|保管)(於|在)[^，。；]{0,4}(遠離|避開|避免)[^，。；]*[、和及與且][^，。；]*(環境|地方|場所|處)"),
         std::wregex(L"將室溫")},
    };
    return r;
}

// The English target keeps a negation / limit word.
const std::wregex& enNegation() {
    static const std::wregex re(
        L"(\\bnot\\b|n't|\\bno\\b|\\bnever\\b|\\bavoid|away from|\\bwithout\\b|prohibit|\\bcannot\\b|\\bunable\\b|\\bkeep out|\\bunder\\b|\\bnor\\b|\\bonly\\b|\\bexcept)");
    return re;
}

bool anyIn(const std::wstring& s, const Words& words) {
    for (const wchar_t* w : words)
        if (s.find(w) != std::wstring::npos) return true;
    return false;
}

void blank(std::wstring& s, const Words& words) {
    for (const wchar_t* w : words)
        for (size_t p = s.find(w); p != std::wstring::npos; p = s.find(w)) s.replace(p, wcslen(w), L"＿");
}

}  // namespace

NegCues negationCues(const std::wstring& src, Lang lang) {
    NegCues n;
    for (const SourceNegRules& r : sourceRules()) {
        if (r.lang != lang) continue;
        std::wstring s = r.fold ? lower(src) : src;
        blank(s, r.notNegations);
        for (std::wsregex_iterator it(s.begin(), s.end(), r.cueRe), e; it != e; ++it) {
            const std::wstring w = it->str();
            ++n.count;
            n.words.push_back(w);
            for (const wchar_t* k : prohibitCues()) n.prohibit |= w == k;
        }
        if (anyIn(s, r.underWords)) n.under = true;
        if (r.sign && std::regex_search(s, r.signRe)) {
            ++n.count;
            n.words.push_back(L"prohibit");
            n.prohibit = true;
        }
        if (r.avoid) {
            const size_t av = s.find(r.avoid);
            if (av != std::wstring::npos) {
                const std::wstring before = s.substr(0, av);
                for (const wchar_t* sep = r.listSeps; *sep; ++sep) n.listScope |= before.find(*sep) != std::wstring::npos;
            }
        }
    }
    return n;
}

bool negationKept(const NegCues& cues, const std::wstring& tx, Lang tgt) {
    if (cues.count == 0) return true;
    if (tgt == Lang::ZhHant) {
        const ZhNegRules& r = zhRules();
        std::wstring t = tx;
        blank(t, r.notNegations);
        const bool neg = anyIn(t, r.negWords) || (cues.under && anyIn(tx, r.underWords));
        if (!neg) return false;
        // Two verb negations in the source: the target needs two as well - 「在沒有
        // 標價的攤位購買」 kept the first and lost the instruction.
        int verbNeg = 0;
        for (const auto& w : cues.words)
            for (const wchar_t* v : verbNegCues()) verbNeg += w == v;
        if (verbNeg >= 2) {
            int n = 0;
            for (wchar_t c : t) n += wcschr(r.negChars, c) != nullptr;
            if (n < 2) return false;
        }
        // A prohibition needs a prohibition word, not an absence.
        if (cues.prohibit && !anyIn(t, r.prohibitWords)) return false;
        if (anyIn(tx, r.broken)) return false;
        if (cues.listScope)
            for (const std::wregex& re : r.listScopeBad)
                if (std::regex_search(tx, re)) return false;
        return true;
    }
    if (tgt == Lang::En) return std::regex_search(lower(tx), enNegation());
    return true;  // ja / ko targets: not checked
}


std::vector<std::wstring> numbersIn(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    auto flush = [&] {
        while (!cur.empty() && (cur.back() == L'.' || cur.back() == L',')) cur.pop_back();
        if (!cur.empty()) {
            std::wstring n;
            for (wchar_t c : cur)
                if (c != L',') n += c;  // 3,980 = 3980
            // 08 = 8 (10月01日 -> 10月1日), but keep 0.05 and 26.12.09 as they are
            if (n.find(L'.') == std::wstring::npos)
                while (n.size() > 1 && n[0] == L'0') n.erase(0, 1);
            out.push_back(n);
        }
        cur.clear();
    };
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c >= 0xFF10 && c <= 0xFF19) c = static_cast<wchar_t>(c - 0xFF10 + L'0');
        if (iswdigit(c)) cur += c;
        else if ((c == L'.' || c == L',') && !cur.empty() && i + 1 < s.size() && iswdigit(s[i + 1])) cur += c;
        else flush();
    }
    flush();
    return out;
}

bool numbersKept(const std::wstring& src, const std::wstring& tx) {
    std::vector<std::wstring> a = numbersIn(src), b = numbersIn(tx);
    // Small numbers written in Chinese (3歳 -> 三歲, 2枚 -> 兩片, 10 -> 十).
    static const std::wstring zh = L"零一二三四五六七八九";
    for (size_t i = 0; i < tx.size(); ++i) {
        const wchar_t c = tx[i];
        const size_t d = zh.find(c);
        if (c == L'十') b.push_back(L"10");
        else if (c == L'兩') b.push_back(L"2");
        else if (d != std::wstring::npos) b.push_back(std::to_wstring(d));
    }
    for (const auto& n : a) {
        auto it = std::find(b.begin(), b.end(), n);
        if (it == b.end()) return false;
        b.erase(it);
    }
    return true;
}

}  // namespace pm::translate
