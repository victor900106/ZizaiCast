// The engines behind pm/translator.h and the escalation of a failed check
// (ARCHITECTURE.md §3.7 / §3.8; owner decisions (5) and (7)): Bergamot first,
// then the source split into clauses, a local LLM (if installed), online
// translation (if the user turned it on), the high-risk templates, and at
// last the checked key facts written out with the best translation.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <chrono>
#include <cstdio>
#include <deque>
#include <regex>
#include <unordered_map>

#include "pm/i18n.h"  // verifiedFacts: its labels (TrFacts*) in the target language
#include "pm/translator.h"
#include "text_util.h"
#ifdef PM_TRANSLATE_HAVE_ONLINE  // translate/CMakeLists.txt: online_engine*.cpp built in
#include "pm/online_translate.h"
#endif

// Default factories: no LLM / online engine in this build.  An engine file
// (llm_engine*.cpp / online_engine*.cpp) defining the real function wins:
// MSVC uses an /alternatename only when the symbol is defined nowhere else.
extern "C" pm::translate::ITranslator* pm_default_no_translator() { return nullptr; }
#if defined(_M_X64) || defined(_M_ARM64)
#pragma comment(linker, "/alternatename:pm_create_local_llm_translator=pm_default_no_translator")
#pragma comment(linker, "/alternatename:pm_create_online_translator=pm_default_no_translator")
#else
#pragma comment(linker, "/alternatename:_pm_create_local_llm_translator=_pm_default_no_translator")
#pragma comment(linker, "/alternatename:_pm_create_online_translator=_pm_default_no_translator")
#endif

namespace pm::translate {

std::unique_ptr<ITranslator> createLocalLlmTranslator() { return std::unique_ptr<ITranslator>(pm_create_local_llm_translator()); }
std::unique_ptr<ITranslator> createOnlineTranslator() { return std::unique_ptr<ITranslator>(pm_create_online_translator()); }

namespace {

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// Bergamot (Engine) as an ITranslator.
class BergamotTranslator : public ITranslator {
public:
    explicit BergamotTranslator(Engine& e) : e_(e) {}
    const char* id() const override { return "bergamot"; }
    bool supports(Lang src, Lang tgt, bool* direct) const override {
        bool ok = true;
        const auto p = ModelStore::pairsFor(src, tgt, &ok);
        if (direct) *direct = p.size() == 1;
        return ok;
    }
    bool installed(Lang src, Lang tgt) const override {
        return Engine::available() && ModelStore::missingBytes(ModelStore::pairsFor(src, tgt)) == 0;
    }
    uint64_t missingBytes(Lang src, Lang tgt) const override { return ModelStore::missingBytes(ModelStore::pairsFor(src, tgt)); }
    bool translate(const std::vector<TrRequest>& in, std::vector<TrHypothesis>& out, std::wstring* err) override {
        out.assign(in.size(), {});
        // One engine call per language pair.
        for (size_t i = 0; i < in.size(); ++i) {
            if (!out[i].engine.empty()) continue;
            std::vector<size_t> idx;
            std::vector<std::wstring> texts, res;
            for (size_t j = i; j < in.size(); ++j)
                if (in[j].src == in[i].src && in[j].tgt == in[i].tgt) idx.push_back(j), texts.push_back(in[j].text);
            const double t0 = nowMs();
            if (!e_.translate(in[i].src, in[i].tgt, texts, res, err)) return false;
            const double ms = (nowMs() - t0) / std::max<size_t>(1, idx.size());
            const bool pivot = ModelStore::pairsFor(in[i].src, in[i].tgt).size() > 1;
            for (size_t k = 0; k < idx.size(); ++k) {
                out[idx[k]].text = k < res.size() ? res[k] : L"";
                out[idx[k]].engine = id();
                out[idx[k]].ms = ms;
                out[idx[k]].pivot = pivot;
            }
        }
        return true;
    }

private:
    Engine& e_;
};

// Source rewrites that keep a negation's scope through the pivot (measured on
// the owner's label: 「…場所を避けて常温で保存」 -> 「將室溫存放於遠離…的環境中」,
// 「…場所を避け、常温で保存」 -> 「存放於室溫下，避免陽光直射、高溫及潮濕場所」).
std::wstring rewriteForScope(const std::wstring& s, Lang src) {
    std::wstring t = s;
    if (src == Lang::Ja) {
        static const std::wregex avoid(L"を避けて(?!、)");
        t = std::regex_replace(t, avoid, L"を避け、");
        static const std::wregex nai(L"ないで(?![、ください下])");
        t = std::regex_replace(t, nai, L"ないで、");
    } else if (src == Lang::Ko) {
        static const std::wregex avoid(L"피하고 ");
        t = std::regex_replace(t, avoid, L"피하고, ");
    }
    return t;
}

// Clauses: sentences, bracketed notes (（タイ製造）), Japanese て、/ので、/が、
// joints.  Lists (直射日光、高温多湿) are not cut.
std::vector<std::wstring> clauses(const std::wstring& s, Lang src) {
    std::vector<std::wstring> out;
    std::wstring cur;
    auto flush = [&] {
        if (!cur.empty()) out.push_back(cur);
        cur.clear();
    };
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if ((c == L'（' || c == L'(') && depth == 0) flush();
        cur += c;
        if (c == L'（' || c == L'(') ++depth;
        if ((c == L'）' || c == L')') && depth > 0 && --depth == 0) flush();
        if (depth) continue;
        if (c == L'。' || c == L'！' || c == L'？' || ((c == L'.' || c == L'!' || c == L'?') && i + 1 < s.size() && s[i + 1] == L' '))
            flush();
        else if (src == Lang::Ja && c == L'、' && cur.size() >= 3) {
            const std::wstring tail = cur.substr(cur.size() - 3, 2);
            if (cur[cur.size() - 2] == L'て' || cur[cur.size() - 2] == L'で' || tail == L"ので" || tail == L"から" ||
                cur[cur.size() - 2] == L'が' || tail == L"避け")
                flush();
        } else if (src == Lang::Ko && c == L',') {
            flush();
        } else if (src == Lang::Ko && c == L' ' && cur.size() >= 3) {
            // Notes run together on one line (찌개류는 1인분 주문 불가 공기밥 별도):
            // a clause ends with a prohibition / a sentence-final ending.
            std::wstring w = cur.substr(0, cur.size() - 1);
            for (const wchar_t* e : {L"불가", L"금지", L"불가능", L"습니다", L"세요", L"시오", L"니다"})
                if (w.size() >= wcslen(e) && w.compare(w.size() - wcslen(e), std::wstring::npos, e) == 0) {
                    flush();
                    break;
                }
        }
    }
    flush();
    return out;
}

bool hasLetters(const std::wstring& s) { return countScripts(s).letters() > 0; }

// Brackets of the source must come back (a note like （タイ製造） merged into the noun).
int bracketCount(const std::wstring& s) {
    int n = 0;
    for (wchar_t c : s) n += c == L'（' || c == L'(';
    return n;
}

// Cue words of the source in the target language (step 6).
std::wstring glossCue(const std::wstring& c, Lang tgt) {
    struct G {
        const wchar_t *cue, *zh, *en;
    };
    static const G k[] = {
        {L"避け", L"避免", L"avoid"},         {L"控え", L"避免", L"avoid"},         {L"禁止", L"禁止", L"prohibited"},
        {L"厳禁", L"嚴禁", L"strictly prohibited"}, {L"不可", L"不可", L"not allowed"}, {L"ご遠慮", L"請勿", L"please do not"},
        {L"おやめ", L"請勿", L"please do not"}, {L"やめて", L"請勿", L"do not"}, {L"無用", L"不需要", L"not needed"},
        {L"未開封", L"未開封", L"unopened"},  {L"できません", L"無法", L"cannot"}, {L"しないで", L"請勿", L"do not"},
        {L"ないで", L"不要", L"do not"},      {L"ません", L"不", L"not"},           {L"ない", L"不", L"not"},
        {L"なく", L"不", L"not"},             {L"ずに", L"不", L"without"},         {L"せず", L"不", L"without"},
        {L"않", L"不", L"not"},               {L"못하", L"無法", L"cannot"},        {L"못 ", L"無法", L"cannot"},
        {L"금지", L"禁止", L"prohibited"},    {L"마세요", L"請勿", L"do not"},      {L"마십시오", L"請勿", L"do not"},
        {L"말아", L"請勿", L"do not"},        {L"말고", L"不要", L"do not"},        {L"피하", L"避免", L"avoid"},
        {L"피해", L"避免", L"avoid"},         {L"삼가", L"請避免", L"avoid"},       {L"불가", L"不可", L"not allowed"},
        {L"없", L"沒有", L"none"},            {L"안 되", L"不可", L"not allowed"},  {L"not", L"不", L"not"},
        {L"n't", L"不", L"not"},              {L"never", L"絕不", L"never"},        {L"avoid", L"避免", L"avoid"},
        {L"away from", L"遠離", L"away from"}, {L"without", L"沒有", L"without"},   {L"prohibit", L"禁止", L"prohibited"},
        {L"cannot", L"無法", L"cannot"},      {L"unable", L"無法", L"unable"},      {L"keep out", L"請勿進入", L"keep out"},
        {L"no", L"無／不", L"no"},
    };
    for (const auto& g : k)
        if (c == g.cue) return tgt == Lang::En ? g.en : g.zh;
    return c;
}

// Numbers with their units as written in the source: 0.05g, 42kcal, 26.12.09, 0120-8284-39.
std::vector<std::wstring> quantities(const std::wstring& s) {
    std::vector<std::wstring> out;
    static const std::wregex re(
        L"[0-9０-９][0-9０-９.,:/\\-〜~]*\\s?(?:kcal|kJ|mg|μg|mL|ml|kg|g|L|V|W|Hz|mAh|A|cm|mm|m|%|％|℃|°C|°F|円|원|袋|枚|個|本|개|장|인분|分|秒|時間|歳|세|분)?");
    for (std::wsregex_iterator it(s.begin(), s.end(), re), e; it != e; ++it) {
        std::wstring q = it->str();
        while (!q.empty() && (q.back() == L' ' || q.back() == L'.' || q.back() == L',' || q.back() == L'-')) q.pop_back();
        if (!q.empty() && std::find(out.begin(), out.end(), q) == out.end()) out.push_back(q);
    }
    return out;
}

}  // namespace

std::wstring verifiedFacts(const std::wstring& src, Lang lang, Lang tgt) {
    // The labels and separators come from the UI string table (TrFacts*),
    // in the reader's (target) language - not the UI language.
    using pm::i18n::S;
    const pm::i18n::Lang ul = tgt == Lang::En   ? pm::i18n::Lang::En
                              : tgt == Lang::Ja ? pm::i18n::Lang::Ja
                              : tgt == Lang::Ko ? pm::i18n::Lang::Ko
                                                : pm::i18n::Lang::ZhTW;
    const std::wstring sep = pm::i18n::tr(S::TrFactsSep, ul);
    const NegCues cues = negationCues(src, lang);
    std::wstring neg, num;
    std::vector<std::wstring> seen;
    for (const auto& w : cues.words) {
        const std::wstring g = glossCue(w, tgt);
        if (std::find(seen.begin(), seen.end(), g) != seen.end()) continue;
        seen.push_back(g);
        neg += (neg.empty() ? L"" : sep) + pm::i18n::fill(pm::i18n::tr(S::TrFactsCue, ul), {w, g});
    }
    for (std::wstring q : quantities(src)) {
        // Korean counters in the reader's words (1인분 -> 1人份).
        static const struct {
            const wchar_t *ko, *zh, *en;
        } kC[] = {{L"인분", L"人份", L" serving(s)"}, {L"만원", L"萬韓元", L"0,000 won"}, {L"원", L"韓元", L" won"}, {L"개", L"個", L" pcs"}, {L"장", L"張", L" sheets"},
                  {L"세", L"歲", L" years old"}, {L"분", L"分鐘", L" min"}};
        if (lang == Lang::Ko)
            for (const auto& c : kC)
                if (q.size() > wcslen(c.ko) && q.compare(q.size() - wcslen(c.ko), std::wstring::npos, c.ko) == 0) {
                    q = q.substr(0, q.size() - wcslen(c.ko)) + (tgt == Lang::En ? c.en : c.zh);
                    break;
                }
        num += (num.empty() ? L"" : sep) + q;
    }
    const std::wstring n = neg.empty() ? L"" : pm::i18n::fill(pm::i18n::tr(S::TrFactsNeg, ul), {neg});
    const std::wstring m = num.empty() ? L"" : pm::i18n::fill(pm::i18n::tr(S::TrFactsNum, ul), {num});
    if (n.empty() || m.empty()) return n + m;
    return pm::i18n::fill(pm::i18n::tr(S::TrFactsJoin, ul), {n, m});
}

namespace {

// A decoder loop: one to four characters four times or more in a row
// (NO ADMISSION … -> 「無免免免免」, Notice -> 「請，請，請，…」), not in the source.
bool repetitionLoop(const std::wstring& t, const std::wstring& src) {
    for (size_t len = 1; len <= 4; ++len)
        for (size_t i = 0; i + len * 4 <= t.size(); ++i) {
            bool rep = true;
            for (size_t k = 1; k < 4 && rep; ++k) rep = t.compare(i + k * len, len, t, i, len) == 0;
            if (!rep) continue;
            const std::wstring piece = t.substr(i, len);
            bool letter = false;
            for (wchar_t c : piece) letter |= iswalpha(c) || isCjk(c);
            if (letter && src.find(piece + piece + piece) == std::wstring::npos) return true;
        }
    return false;
}

// Runs of one to four characters repeated three times or more: kept once.
std::wstring collapseLoops(std::wstring t) {
    for (size_t len = 1; len <= 4; ++len)
        for (size_t i = 0; i + len * 3 <= t.size(); ++i) {
            size_t k = 1;
            while (i + (k + 1) * len <= t.size() && t.compare(i + k * len, len, t, i, len) == 0) ++k;
            if (k >= 3) t.erase(i + len, (k - 1) * len);
        }
    return t;
}

// Units after a number: the source's unit (or its zh-Hant / English name)
// must follow the same number in the translation (Protein 3.7 g -> 「蛋白質 3.7」,
// 100만원 -> 「100人」 lost them).
struct UnitName {
    const wchar_t* unit;
    const wchar_t* shown;           // inserted by repairUnits (zh-Hant)
    const wchar_t* accepted[6];
};
const UnitName kUnits[] = {
    {L"만원", L"萬韓元", {L"萬韓元", L"万韓元", L"萬元", L"萬圓", L"0,000", L"만원"}},
    {L"kcal", L"kcal", {L"kcal", L"大卡", L"千卡", L"卡", L"Kcal", L"calories"}},
    {L"kJ", L"kJ", {L"kJ", L"千焦", L"KJ", nullptr}},
    {L"mg", L"mg", {L"mg", L"毫克", nullptr}},
    {L"kg", L"kg", {L"kg", L"公斤", L"千克", nullptr}},
    {L"mL", L"mL", {L"ml", L"mL", L"毫升", L"ML", nullptr}},
    {L"ml", L"ml", {L"ml", L"mL", L"毫升", L"ML", nullptr}},
    {L"g", L"g", {L"g", L"公克", L"克", L"G", nullptr}},
    {L"円", L"日圓", {L"円", L"日圓", L"日元", L"圓", L"yen", nullptr}},
    {L"원", L"韓元", {L"원", L"韓元", L"won", L"韓圜", nullptr}},
    {L"%", L"%", {L"%", L"％", L"percent", nullptr}},
};

struct Qty {
    std::wstring number;
    const UnitName* unit;
};
std::vector<Qty> quantitiesWithUnits(const std::wstring& s) {
    std::vector<Qty> out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (!iswdigit(s[i]) || (i > 0 && (iswdigit(s[i - 1]) || s[i - 1] == L'.' || s[i - 1] == L','))) continue;
        size_t e = i;
        while (e < s.size() && (iswdigit(s[e]) || ((s[e] == L'.' || s[e] == L',') && e + 1 < s.size() && iswdigit(s[e + 1])))) ++e;
        size_t u = e;
        while (u < s.size() && s[u] == L' ') ++u;
        for (const auto& un : kUnits) {
            const size_t n = wcslen(un.unit);
            if (s.compare(u, n, un.unit) != 0) continue;
            // g of "go" / "grams" is a word, not the unit symbol.
            if (iswalpha(un.unit[n - 1]) && u + n < s.size() && iswalpha(s[u + n]) && s.compare(u, 5, L"grams") != 0) break;
            out.push_back({s.substr(i, e - i), &un});
            break;
        }
        i = e;
    }
    return out;
}
std::wstring noCommas(std::wstring s) {
    s.erase(std::remove(s.begin(), s.end(), L','), s.end());
    return s;
}
// The positions of number n in t (as written or without thousands commas), not inside a longer number.
std::vector<size_t> numberAt(const std::wstring& t, const std::wstring& n) {
    std::vector<size_t> at;
    for (const std::wstring& v : {n, noCommas(n)}) {
        for (size_t p = t.find(v); p != std::wstring::npos; p = t.find(v, p + 1)) {
            const size_t e = p + v.size();
            if ((p > 0 && (iswdigit(t[p - 1]) || t[p - 1] == L'.')) || (e < t.size() && iswdigit(t[e]))) continue;
            at.push_back(e);
        }
        if (!at.empty()) break;
    }
    return at;
}
bool unitAfter(const std::wstring& t, size_t e, const UnitName& u, const std::wstring& before) {
    size_t k = e;
    while (k < t.size() && (t[k] == L' ' || t[k] == 0x3000)) ++k;
    for (const wchar_t* a : u.accepted)
        if (a && t.compare(k, wcslen(a), a) == 0) return true;
    // ₩5,000 / ¥380 / 5,000 韓元 written before.
    return (u.unit == std::wstring(L"원") || u.unit == std::wstring(L"만원")) ? before.find(L'₩') != std::wstring::npos
           : u.unit == std::wstring(L"円")                                     ? before.find_first_of(L"¥￥") != std::wstring::npos
                                                                                : false;
}

}  // namespace

bool unitsKept(const std::wstring& src, const std::wstring& tx) {
    for (const Qty& q : quantitiesWithUnits(src)) {
        const auto at = numberAt(tx, q.number);
        if (at.empty()) continue;  // the number itself: numbersKept
        bool ok = false;
        for (size_t e : at) ok |= unitAfter(tx, e, *q.unit, tx.substr(0, e));
        if (!ok) return false;
    }
    return true;
}

std::wstring repairUnits(const std::wstring& src, const std::wstring& tx) {
    std::wstring t = tx;
    for (const Qty& q : quantitiesWithUnits(src)) {
        const auto at = numberAt(t, q.number);
        bool ok = false;
        for (size_t e : at) ok |= unitAfter(t, e, *q.unit, t.substr(0, e));
        if (ok || at.empty()) continue;
        // The first bare occurrence (not followed by a word: 「100人」 is not repaired).
        for (size_t e : at)
            if (e >= t.size() || !(iswalpha(t[e]) || isCjk(t[e]))) {
                t.insert(e, q.unit->shown);
                break;
            }
    }
    return t;
}


std::vector<std::string> checkTranslation(const std::wstring& src, const std::wstring& tx, Lang lang, Lang tgt) {
    std::vector<std::string> f;
    if (tx.empty()) return {"empty"};
    const NegCues cues = negationCues(src, lang);
    if (!negationKept(cues, tx, tgt)) f.push_back(cues.listScope ? "neg-scope" : "neg-missing");
    if (!numbersKept(src, tx)) f.push_back("num-missing");
    else if (!unitsKept(src, tx)) f.push_back("unit-missing");
    if (repetitionLoop(tx, src)) f.push_back("repeat");
    if (bracketCount(src) > bracketCount(tx) && hasLetters(src)) {
        // A bracketed note with words (タイ製造) lost; bare numbers in brackets do not count.
        static const std::wregex note(L"[（(][^）)]*[\\u3040-\\u30ff\\u4e00-\\u9fff\\uac00-\\ud7afA-Za-z][^）)]*[）)]");
        if (std::regex_search(src, note)) f.push_back("paren-lost");
    }
    return f;
}

struct Escalator::Impl {
    Engine& engine;
    EscalationConfig cfg;
    BergamotTranslator bergamot;
    std::unique_ptr<ITranslator> llm, online;
    bool llmTried = false, onlineTried = false;
    double llmMs = 0;
    OnlineMode onlineMode = OnlineMode::Off;  // per picture (resetBudget)
    // Memory-only cache of checked translations (key: src, tgt, piece).
    std::unordered_map<std::wstring, TrHypothesis> cache;
    std::deque<std::wstring> order;
    static constexpr size_t kCacheMax = 4096;
    std::vector<Escalator::Pending> pending;  // LLM candidates of this picture
    std::unordered_map<std::wstring, TrHypothesis> clauseCache;  // step 2's batch (prefetchClauses), per picture
    bool llmWarm = false;                     // the LLM answered once (model loaded)
    bool wantWarm = false;                    // a picture had LLM work while the model was cold
    double llmLast = 0;                       // last LLM request (steady ms)
    double batchMsPerPiece = 400;             // LLM batch time per piece, learned (CPU start value)
    struct Warm {                             // warmUp()'s loader thread
        std::atomic<int> state{0};            // 0 idle, 1 loading, 2 loaded (not yet seen by runPending)
        std::atomic<double> at{0};            // when it finished
    };
    std::shared_ptr<Warm> warm = std::make_shared<Warm>();
    void takeWarm() {
        if (warm->state.load() != 2) return;
        llmWarm = true;
        llmLast = warm->at.load();
        warm->state = 0;
    }
    Impl(Engine& e, EscalationConfig c) : engine(e), cfg(c), bergamot(e) {}
};

int translitRun(const std::wstring& tx) {
    static const std::wstring k =
        L"阿埃艾安奧巴拜班邦比彼波伯布查達戴丹德迪蒂多杜爾法菲弗福伽蓋岡戈格古哈漢赫霍吉基加賈傑卡凱坎科克庫拉萊蘭勞雷里利林"
        L"隆魯羅洛馬邁曼梅蒙米密莫姆納奈南尼諾帕佩皮普奇喬切薩塞桑瑟沙舍斯蘇索塔泰坦特提托圖瓦韋維溫沃西希謝辛雅亞楊伊因尤約扎"
        L"贊澤茲祖佐";
    int best = 0, cur = 0;
    for (wchar_t c : tx) {
        cur = k.find(c) != std::wstring::npos ? cur + 1 : 0;
        best = std::max(best, cur);
    }
    return best;
}

std::string llmGuard(const std::wstring& plain, const std::wstring& best, const std::wstring& alt,
                     const std::vector<std::wstring>& keep, Lang src, Lang tgt) {
    if (alt.empty()) return "empty";
    if (tgt == Lang::ZhHant || tgt == Lang::En) {
        const ScriptCount n = countScripts(alt);
        if (n.kana + n.hangul > 0) return "script";  // a part left untranslated (油揚げ麺 copied)
    }
    auto len = [](const std::wstring& s) {
        size_t n = 0;
        for (wchar_t c : s) n += !iswspace(c);
        return n;
    };
    (void)len, (void)best;  // a shorter answer is mostly a terser right one (route_eval.py: the "shorter" guard cost 4 of 8 fixes)
    auto lower = [](std::wstring s) {
        for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
        return s;
    };
    const std::wstring la = lower(alt);
    for (const auto& k : keep)
        if (!k.empty() && la.find(lower(k)) == std::wstring::npos) return "names";  // 美十 -> Mitsui, 八橋 -> 八重橋
    const auto f = checkTranslation(plain, alt, src, tgt);
    if (!f.empty()) return f[0];
    // An LLM answer to a negated source must say it plainly (「持有…者以外者」
    // for お持ちでない方 passed the check only through 以外).
    if (tgt == Lang::ZhHant && negationCues(plain, src).count > 0) {
        bool plainNeg = false;
        for (const wchar_t* w : {L"不", L"未", L"沒", L"無", L"勿", L"禁", L"非", L"別", L"免", L"避", L"遠離", L"停止", L"拒"})
            plainNeg = plainNeg || alt.find(w) != std::wstring::npos;
        if (!plainNeg) return "neg-unclear";
    }
    return {};
}

double Escalator::now() { return nowMs(); }
bool Escalator::warmUp(bool wait) {
    Impl& m = *impl_;
    m.takeWarm();
    if ((m.llmWarm && nowMs() - m.llmLast < 270000) || !m.wantWarm || !m.llm || m.warm->state.load() == 1) return false;
    m.wantWarm = false;
    if (!m.llm->installed(Lang::Ja, Lang::ZhHant)) return false;
    auto load = [](ITranslator& t) {
        std::vector<TrHypothesis> out;
        std::wstring err;
        t.translate({request(L"はい", Lang::Ja, Lang::ZhHant)}, out, &err);
    };
    if (wait) {
        load(*m.llm);
        m.llmWarm = true;
        m.llmLast = nowMs();
        return true;
    }
    // Its own translator (the model itself is shared and loaded once): the
    // thread outlives this Escalator if need be.
    std::shared_ptr<Impl::Warm> w = m.warm;
    w->state = 1;
    std::thread([w, load] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        if (auto t = createLocalLlmTranslator()) load(*t);
        w->at = nowMs();
        w->state = 2;
    }).detach();
    return true;
}
void Escalator::queue(Pending p) { impl_->pending.push_back(std::move(p)); }
size_t Escalator::pendingCount() const { return impl_->pending.size(); }

int Escalator::runPending(double deadlineMs, std::vector<RouteTrace>* trace) {
    Impl& m = *impl_;
    std::vector<Pending> q;
    q.swap(m.pending);
    if (q.empty() || !m.cfg.localLlm) return 0;
    if (!m.llmTried) {
        m.llmTried = true;
        m.llm = createLocalLlmTranslator();
    }
    if (!m.llm) return 0;
    m.takeWarm();
    // Most likely wrong first (failed checks, then garbage), shortest first.
    std::stable_sort(q.begin(), q.end(), [](const Pending& a, const Pending& b) {
        return a.prio != b.prio ? a.prio > b.prio : a.plain.size() < b.plain.size();
    });
    // One batch: the engine decodes a picture's segments side by side (parallel
    // sequences) and stops at TrRequest::budgetMs; a piece goes in while its
    // own predicted time (cost(): warm, one request) still fits before the
    // deadline, in priority order.
    const TrCost c = m.llm->cost();
    const double t0 = nowMs();
    const bool cold = !m.llmWarm || t0 - m.llmLast > 270000;  // the engine unloads after 5 idle minutes
    std::vector<RouteTrace> tr(q.size());
    std::vector<size_t> send;
    for (size_t i = 0; i < q.size(); ++i) {
        const Pending& p = q[i];
        tr[i].plain = p.plain;
        tr[i].prio = p.prio;
        tr[i].predictedMs = c.msPerRequest + c.msPerChar * static_cast<double>(p.plain.size());
        if (!m.llm->installed(p.src, p.tgt)) {
            tr[i].result = "failed:not-installed";
        } else if (cold) {
            // A cold model takes seconds to load (3.8 s measured): never inside
            // a picture's budget; warmUp() loads it after the picture is shown.
            tr[i].result = "skipped:cold";
            m.wantWarm = true;
        } else {
            // The batch's predicted time: its pieces at the measured batch
            // rate (m.batchMsPerPiece, learned per batch; starts from cost()).
            const double batch = 150 + (send.size() + 1) * std::max(m.batchMsPerPiece, 0.5 * tr[i].predictedMs);
            if (t0 + std::max(batch, tr[i].predictedMs) > deadlineMs) tr[i].result = "skipped:time";  // keeps the best verified result
            else send.push_back(i);
        }
    }
    int improved = 0;
    if (!send.empty()) {
        std::vector<TrRequest> rs;
        for (size_t i : send) {
            TrRequest r = request(q[i].plain, q[i].src, q[i].tgt);
            r.budgetMs = deadlineMs - t0;
            rs.push_back(std::move(r));
        }
        std::vector<TrHypothesis> out;
        std::wstring err;
        const bool ok = m.llm->translate(rs, out, &err);
        m.llmWarm = true;  // loaded now, whatever the answers
        m.llmLast = nowMs();
        const double ms = nowMs() - t0;
        m.llmMs += ms;
        // Measured 2.27 s for 6 short pieces, 0.84 s for 3 (CPU, 4 threads).
        m.batchMsPerPiece = 0.6 * m.batchMsPerPiece + 0.4 * std::max(50.0, (ms - 150) / send.size());
        for (size_t k = 0; k < send.size(); ++k) {
            const Pending& p = q[send[k]];
            RouteTrace& t = tr[send[k]];
            t.ms = ms;
            if (!ok || k >= out.size() || out[k].text.empty()) {
                t.result = "failed";  // or cut at the deadline
                continue;
            }
            t.out = out[k].text;
            const std::string why = llmGuard(p.plain, p.best, t.out, p.keep, p.src, p.tgt);
            bool take = why.empty();
            // Garbage: only when the LLM's text is not garbage itself.
            if (take && p.prio == 2 && translitRun(p.best) >= 3 && translitRun(t.out) >= translitRun(p.best)) take = false;
            if (take) {
                TrHypothesis h = out[k];
                h.step = 3;
                remember(p.src, p.tgt, p.plain, h);
                ++improved;
                t.result = "accepted";
            } else {
                t.result = "rejected:" + (why.empty() ? std::string("garbage") : why);
            }
        }
    }
    if (trace) trace->insert(trace->end(), tr.begin(), tr.end());
    return improved;
}

namespace {
std::wstring cacheKey(Lang src, Lang tgt, const std::wstring& piece) {
    return std::wstring(1, static_cast<wchar_t>(L'A' + static_cast<int>(src))) + static_cast<wchar_t>(L'A' + static_cast<int>(tgt)) + piece;
}
}  // namespace

Escalator::Escalator(Engine& e, EscalationConfig cfg) : impl_(std::make_unique<Impl>(e, cfg)) {}
Escalator::~Escalator() = default;
void Escalator::setConfig(const EscalationConfig& c) {
    // Other engines allowed: earlier results may no longer be the best.
    if (c.localLlm != impl_->cfg.localLlm || c.online != impl_->cfg.online || c.splitClauses != impl_->cfg.splitClauses ||
        c.templates != impl_->cfg.templates)
        clearCache();
    impl_->cfg = c;
}
bool Escalator::cached(Lang src, Lang tgt, const std::wstring& piece, TrHypothesis& h) const {
    if (impl_->cfg.collectAlt) return false;  // tests: every piece through the engines
    const auto it = impl_->cache.find(cacheKey(src, tgt, piece));
    if (it == impl_->cache.end()) return false;
    h = it->second;
    return true;
}
void Escalator::remember(Lang src, Lang tgt, const std::wstring& piece, const TrHypothesis& h) {
    if (h.uncertain || h.text.empty()) return;
    Impl& m = *impl_;
    const std::wstring k = cacheKey(src, tgt, piece);
    if (m.cache.insert_or_assign(k, h).second) m.order.push_back(k);
    while (m.order.size() > Impl::kCacheMax) {
        m.cache.erase(m.order.front());
        m.order.pop_front();
    }
}
void Escalator::clearCache() {
    impl_->cache.clear();
    impl_->order.clear();
}
size_t Escalator::cacheSize() const { return impl_->cache.size(); }
const EscalationConfig& Escalator::config() const { return impl_->cfg; }
void Escalator::resetBudget() {
    impl_->llmMs = 0;
    impl_->pending.clear();
    impl_->clauseCache.clear();
    // The online mode in effect for this picture (Off unless the user turned
    // it on, gave a key and the provider is not backed off).
#ifdef PM_TRANSLATE_HAVE_ONLINE
    switch (online::activeMode()) {
    case online::Mode::All: impl_->onlineMode = OnlineMode::All; break;
    case online::Mode::Escalate: impl_->onlineMode = OnlineMode::Escalate; break;
    default: impl_->onlineMode = OnlineMode::Off; break;
    }
#else
    impl_->onlineMode = OnlineMode::Off;
#endif
    if (!impl_->cfg.online) impl_->onlineMode = OnlineMode::Off;
}
Escalator::OnlineMode Escalator::onlineMode() const { return impl_->onlineMode; }
bool Escalator::llmTranslate(const std::vector<TrRequest>& in, std::vector<TrHypothesis>& out) {
    Impl& m = *impl_;
    out.assign(in.size(), {});
    if (in.empty()) return true;
    if (!m.llmTried) {
        m.llmTried = true;
        m.llm = createLocalLlmTranslator();
    }
    if (!m.llm || !m.llm->installed(in[0].src, in[0].tgt)) return false;
    std::wstring err;
    return m.llm->translate(in, out, &err);
}
TrRequest Escalator::request(const std::wstring& text, Lang src, Lang tgt) {
    TrRequest r;
    r.text = text;
    r.src = src;
    r.tgt = tgt;
    r.negation = negationCues(text, src).count > 0;
    for (const auto& h : dataGlossaryHits(text, src, tgt)) r.terms.push_back({text.substr(h.pos, h.len), h.zh});
    return r;
}
bool Escalator::prefetchOnline(const std::vector<TrRequest>& in, std::vector<TrHypothesis>* out) {
    if (out) out->assign(in.size(), {});
    if (in.empty() || impl_->onlineMode == OnlineMode::Off) return false;
#ifdef PM_TRANSLATE_HAVE_ONLINE
    return online::prefetch(in, out) == online::Status::Ok;
#else
    return false;
#endif
}

// Step 2's engine requests for a piece: ALL-CAPS English in sentence case,
// the scope rewrite, then (from clFrom) its nCl clauses when it has several.
std::vector<TrRequest> Escalator::clauseRequests(const TrRequest& rq, size_t& clFrom, size_t& nCl) {
    std::vector<TrRequest> rs;
    // ALL-CAPS English (signs) sends the models into loops: in sentence case.
    if (rq.src == Lang::En) {
        int up = 0, low = 0;
        for (wchar_t c : rq.text) up += iswupper(c) != 0, low += iswlower(c) != 0;
        if (up > 3 && low * 4 < up) {
            TrRequest r = rq;
            bool startOf = true;
            for (wchar_t& c : r.text) {
                c = startOf ? static_cast<wchar_t>(towupper(c)) : static_cast<wchar_t>(towlower(c));
                if (iswalpha(c)) startOf = false;
                if (c == L'.' || c == L'!' || c == L'?') startOf = true;
            }
            rs.push_back(r);
        }
    }
    const std::wstring rw = rewriteForScope(rq.text, rq.src);
    if (rw != rq.text) {
        TrRequest r = rq;
        r.text = rw;
        rs.push_back(r);
    }
    const auto cl = clauses(rw, rq.src);
    clFrom = rs.size();
    nCl = cl.size() > 1 ? cl.size() : 0;
    if (cl.size() > 1)
        for (const auto& c : cl) {
            TrRequest r = rq;
            r.text = c;
            rs.push_back(r);
        }
    return rs;
}

void Escalator::prefetchClauses(const std::vector<TrRequest>& failing) {
    Impl& m = *impl_;
    if (!m.cfg.splitClauses) return;
    std::vector<TrRequest> all;
    for (const auto& rq : failing) {
        size_t a = 0, b = 0;
        for (auto& r : clauseRequests(rq, a, b))
            if (!m.clauseCache.count(cacheKey(r.src, r.tgt, r.text))) all.push_back(std::move(r));
    }
    if (all.empty()) return;
    std::vector<TrHypothesis> out;
    std::wstring err;
    const double t0 = nowMs();
    const bool ok = m.bergamot.translate(all, out, &err);  // one call: the engine splits it between its translators
    static const bool prof = std::getenv("PM_TR_PROF") != nullptr;
    if (prof) std::fprintf(stderr, "[tr] step 2 batch: %zu failing pieces, %zu requests, %.0f ms\n", failing.size(), all.size(), nowMs() - t0);
    if (!ok) return;
    for (size_t k = 0; k < all.size() && k < out.size(); ++k) m.clauseCache[cacheKey(all[k].src, all[k].tgt, all[k].text)] = out[k];
}

bool Escalator::escalate(const TrRequest& rq, const std::wstring& plainSrc, TrHypothesis& h) {
    Impl& m = *impl_;
    if (rq.tgt != Lang::ZhHant && rq.tgt != Lang::En) return true;  // checks only for zh-Hant / en targets
    h.flags = checkTranslation(plainSrc, h.text, rq.src, rq.tgt);
    if (h.flags.empty()) return true;
    const TrHypothesis first = h;
    auto accept = [&](const std::wstring& t, int step, const char* eng) {
        if (t.empty()) return false;
        if (!checkTranslation(plainSrc, t, rq.src, rq.tgt).empty()) return false;
        h.text = t;
        h.step = step;
        h.engine = eng;
        h.flags.clear();
        return true;
    };
    // 1b) Units dropped after kept numbers (3.7 g -> 3.7): put back.
    if (std::find(h.flags.begin(), h.flags.end(), "unit-missing") != h.flags.end() &&
        accept(repairUnits(plainSrc, first.text), 2, first.engine.c_str()))
        return true;
    // 2) Clauses: the scope rewrite on the whole text, then clause by clause
    // (already translated in one batch by prefetchClauses when it ran).
    if (m.cfg.splitClauses) {
        size_t clFrom = 0, nCl = 0;
        const std::vector<TrRequest> rs = clauseRequests(rq, clFrom, nCl);
        std::vector<TrHypothesis> out;
        std::wstring err;
        bool have = !rs.empty();
        for (const auto& r : rs) have = have && m.clauseCache.count(cacheKey(r.src, r.tgt, r.text));
        if (have) {
            for (const auto& r : rs) out.push_back(m.clauseCache[cacheKey(r.src, r.tgt, r.text)]);
        } else {
            have = !rs.empty() && m.bergamot.translate(rs, out, &err);
        }
        if (have) {
            for (size_t k = 0; k < clFrom && k < out.size(); ++k)
                if (accept(out[k].text, 2, "bergamot")) return true;
            if (nCl > 1) {
                std::wstring joined;
                for (size_t k = clFrom; k < out.size(); ++k) {
                    std::wstring t = hasLetters(rs[k].text) ? out[k].text : rs[k].text;
                    while (!t.empty() && iswspace(t.back())) t.pop_back();
                    // zh: the pieces' own sentence ends, else 「，」 between clauses.
                    if (!joined.empty()) {
                        const wchar_t e = joined.back();
                        const bool open = t.front() == L'（' || t.front() == L'(';
                        if (rq.tgt == Lang::En) joined += L' ';
                        else if (!open && e != L'。' && e != L'，' && e != L'）' && e != L')' && e != L'！' && e != L'？') joined += L'，';
                    }
                    joined += t;
                }
                if (rq.tgt == Lang::ZhHant) joined = fullWidthPunctuation(joined);
                if (accept(joined, 2, "bergamot")) return true;
            }
        }
    }
    // 3) Local LLM, 4) online: only when present / allowed.
    auto tryEngine = [&](std::unique_ptr<ITranslator>& e, bool& tried, std::unique_ptr<ITranslator> (*make)(), int step) {
        if (!tried) {
            tried = true;
            e = make();
        }
        if (!e || !e->installed(rq.src, rq.tgt)) return false;
        if (step == 3 && m.llmMs > m.cfg.llmBudgetMs) return false;
        std::vector<TrHypothesis> out;
        std::wstring err;
        // The same request as prefetchOnline's (the online cache key: text, langs, context, keep, terms).
        const TrRequest r = request(rq.text, rq.src, rq.tgt);
        const double t0 = nowMs();
        const bool ok = e->translate({r}, out, &err);
        if (step == 3) m.llmMs += nowMs() - t0;
        return ok && !out.empty() && accept(out[0].text, step, e->id());
    };
    if (m.cfg.localLlm && !m.cfg.deferLlm && tryEngine(m.llm, m.llmTried, &createLocalLlmTranslator, 3)) return true;
    if (m.onlineMode != OnlineMode::Off && tryEngine(m.online, m.onlineTried, &createOnlineTranslator, 4)) return true;
    // 5) Templates.
    if (m.cfg.templates) {
        std::wstring t;
        if (applyTemplate(plainSrc, rq.src, rq.tgt, t) && accept(t, 5, "rules")) return true;
    }
    // 6) The best text, the checked key facts written out.
    h = first;
    if (repetitionLoop(h.text, plainSrc)) h.text = collapseLoops(h.text);  // 無免免免免 -> 無免
    h.step = 6;
    h.uncertain = true;
    if (m.cfg.markVerified) h.verified = verifiedFacts(plainSrc, rq.src, rq.tgt);
    return false;
}

}  // namespace pm::translate
