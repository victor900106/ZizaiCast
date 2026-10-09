// Text helpers of pm_translate: scripts, UTF-8, Chinese variants, punctuation.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "pm/translate.h"

namespace pm::translate {

struct ScriptCount {
    int kana = 0, hangul = 0, han = 0, latin = 0, digits = 0, other = 0;
    ScriptCount& operator+=(const ScriptCount& o) {
        kana += o.kana;
        hangul += o.hangul;
        han += o.han;
        latin += o.latin;
        digits += o.digits;
        other += o.other;
        return *this;
    }
    int letters() const { return kana + hangul + han + latin; }
};
ScriptCount countScripts(const std::wstring& s);
bool isCjk(wchar_t c);  // kana, hangul, Han, CJK punctuation / full-width forms

std::wstring toSimplified(const std::wstring& s);
// Han-only text: ZhHant / ZhHans by which conversion leaves it unchanged
// (Unknown when both or neither do).
Lang chineseVariant(const std::wstring& s);

std::string toUtf8(const std::wstring& s);
std::wstring fromUtf8(const std::string& s);

// Recogniser output that looks like Japanese kana / Korean hangul read by a
// Chinese or English recogniser (radical-like stand-ins: 卜 丩 匚 乇 亻 冫 …,
// bopomofo): the right OCR language is probably not installed.
bool looksMisread(const std::wstring& allText);

// Chinese output: ASCII , ? ! : ; ( ) next to CJK characters -> full width.
std::wstring fullWidthPunctuation(const std::wstring& s);

}  // namespace pm::translate

namespace pm::translate {
class Engine;
// Block texts of one source language -> tgt with the fixes for labels, signs
// and menus (label_text.cpp): one-word glossary, table fields split at their
// labels (「賞味期限 枠外下部に記載」 -> 「賞味期限：標示於框外下方」), company /
// product names kept through placeholders (株式会社美十, ゴディバ -> GODIVA),
// misleading kana spellings (うすく -> 薄く).  out: one entry per input ("" =
// nothing worth showing).
bool translateTexts(Engine& engine, Lang src, Lang tgt, const std::vector<std::wstring>& in, std::vector<std::wstring>& out,
                    std::wstring* err);
// The one-word glossary alone (nullptr: no entry); exact match, spaced-out
// headings 「메 뉴」 too.
const wchar_t* glossary(const std::wstring& text, Lang tgt);
// The data glossary (glossary_data.cpp; PM_GLOSSARY or <exe dir>\glossary):
// a whole block (spaces ignored) -> zh-Hant, nullptr when none / off.
const std::wstring* dataGlossary(const std::wstring& text, Lang src, Lang tgt);
// Its s-mode terms inside a text (word boundaries, longest first, no overlaps).
struct GlossaryHit {
    size_t pos, len;
    std::wstring zh;
};
std::vector<GlossaryHit> dataGlossaryHits(const std::wstring& text, Lang src, Lang tgt);

// The blocks of one picture worth translating, by source language (screen
// language: kana -> ja, hangul -> ko, else src): the main text of a photo,
// no OCR noise / bare numbers / brand tokens (screen_translator.cpp; used by
// ScreenTranslator and pm_translate_test --eval).  trustText: the lines are
// ground truth (no confidence filter).  why: per block the reason it is left
// out ("" = picked).  screenLang: the ja / ko / zh screen language or Unknown.
std::map<Lang, std::vector<size_t>> pickBlocks(std::vector<Block>& blocks, Lang src, Lang tgt, float aspect, bool trustText,
                                               std::vector<std::string>* why = nullptr, Lang* screenLang = nullptr);
// A translation worth a card: not empty, not the source again, in the target's script.
bool translatedOk(const std::wstring& src, const std::wstring& tx, Lang from, Lang tgt);

// A table field name of any language (名称, 内容量, 熱量, 제품명, Input …): label_text.cpp.
bool isFieldLabel(const std::wstring& text);

// ---- preprocess.cpp (rules before / instead of the engine) ----
// Japanese kanji text shown in Traditional Chinese: false friends and label
// words by word (焼菓子 -> 烘焙點心, 脂質 -> 脂肪), then shinjitai -> 繁體 (焼 -> 燒).
std::wstring convertJapaneseKanji(const std::wstring& s);
// Characters of s in Japanese-only forms (shinjitai that are neither Simplified
// nor Traditional Chinese: 県 猟 駅 売 歳 …).
int japaneseOnlyKanji(const std::wstring& s);
// Signs of Chinese in kanji-only text: Traditional forms Japanese writes
// differently (會 說 譯 點 灣), Chinese function words (這 們 的 了 是 嗎 一定).
int chineseSignals(const std::wstring& s);
// A postal address (〒, 601-8446, 京都府…市…区…35-2, 서울시 …구 …로).
bool looksLikeAddress(const std::wstring& t);
// High-risk label sentences (保存方法 / 注意 / allergens) whose slots are all
// known terms -> the translation, without the engine.
bool applyTemplate(const std::wstring& text, Lang src, Lang tgt, std::wstring& out);
// Nutrition "per serving" headers with their numbers (1袋（2枚）あたり -> 每1袋（2片））.
bool quantityPhrase(const std::wstring& text, Lang src, Lang tgt, std::wstring& out);

// ---- qe.cpp (checks of a translation) ----
struct NegCues {
    int count = 0;                    // negation / prohibition cues in the source
    std::vector<std::wstring> words;  // the cues (ない, 避け, 금지, not …)
    bool listScope = false;           // a list before an "avoid" (A、B、Cを避けて)
    bool under = false;               // 未満 / under: 以下 counts as kept
    bool prohibit = false;            // a prohibition / avoidance (禁止, 避け, ご遠慮, 불가, 마세요): the target
                                      // needs 不 / 勿 / 禁 / 避 / 無法 …, not just 無 / 未 / 沒 (「無空氣的米飯」)
};
NegCues negationCues(const std::wstring& src, Lang lang);
// The target keeps the negation (zh-Hant / en targets; others: true).
bool negationKept(const NegCues& cues, const std::wstring& tx, Lang tgt);
std::vector<std::wstring> numbersIn(const std::wstring& s);  // 3,980 -> 3980; 0.05; 26.12.09
// Every number of src is in tx.
bool numbersKept(const std::wstring& src, const std::wstring& tx);
// translator.cpp: every number with a unit in src (3.7 g, 42kcal, 380円, 100만원)
// keeps a unit after it in tx (公克 / 大卡 / 日圓 / 萬韓元 … accepted);
// repairUnits puts the lost ones back after a bare number.
bool unitsKept(const std::wstring& src, const std::wstring& tx);
std::wstring repairUnits(const std::wstring& src, const std::wstring& tx);
}  // namespace pm::translate

#include "pm/translator.h"

namespace pm::translate {
// ---- translator.cpp (escalation of a failed check) ----
// Flags of a translation: "neg-missing", "neg-scope", "num-missing", "paren-lost", "empty".
std::vector<std::string> checkTranslation(const std::wstring& src, const std::wstring& tx, Lang lang, Lang tgt);
// Step 6: the source's negation cues and numbers, written out in tgt.
std::wstring verifiedFacts(const std::wstring& src, Lang lang, Lang tgt);
// Longest run of characters Chinese uses to spell foreign sounds (雅基托里: 4):
// a word the pivot did not know, transliterated.
int translitRun(const std::wstring& tx);
// Why an LLM answer must not replace `best` ("" = acceptable): "script" (kana /
// hangul left), "names" (a kept name changed), or
// a failed check (checkTranslation).
std::string llmGuard(const std::wstring& plain, const std::wstring& best, const std::wstring& alt,
                     const std::vector<std::wstring>& keep, Lang src, Lang tgt);

// Runs the checks on a step-1 (Bergamot) hypothesis and escalates while they
// fail (pm/translator.h: clauses, local LLM, online, templates, verified
// facts).  rq.text: what the engine saw (placeholders included); plainSrc:
// the source the checks compare with.  true: h passes the checks.
class Escalator {
public:
    Escalator(Engine& bergamot, EscalationConfig cfg = {});
    ~Escalator();
    void setConfig(const EscalationConfig& c);
    const EscalationConfig& config() const;
    void resetBudget();  // per picture; also reads the online mode (pm/online_translate.h activeMode)
    // Online translation for this picture: Off, Escalate (step 4 for the
    // pieces that failed the checks), All (every piece online first, Bergamot
    // where it fails a check or the request).  Off when cfg.online is false.
    enum class OnlineMode { Off, Escalate, All };
    OnlineMode onlineMode() const;
    // The request the escalator sends to an engine for a piece (step 3 / 4):
    // prefetchOnline must get exactly these, or its cache misses.
    static TrRequest request(const std::wstring& text, Lang src, Lang tgt);
    // One batch call for many pieces (online::prefetch); results cached by
    // the online engine.  false: nothing sent / failed (out: "" each).
    bool prefetchOnline(const std::vector<TrRequest>& in, std::vector<TrHypothesis>* out);
    // The local LLM (pm_create_local_llm_translator) on these requests now,
    // whatever the checks say; false when it is not installed / failed.
    bool llmTranslate(const std::vector<TrRequest>& in, std::vector<TrHypothesis>& out);

    // ---- LLM routing under a time budget (EscalationConfig::deferLlm) ----
    // A piece the LLM may improve, queued by translateTextsEx.  prio: 3 the
    // checks still fail (uncertain), 2 transliteration garbage in Bergamot's
    // text (雅基托里), 1 a short item (<= 10 characters).  keep: names / copied terms the LLM must keep.
    struct Pending {
        Lang src = Lang::Unknown, tgt = Lang::Unknown;
        std::wstring plain, best;          // the source piece, the best text so far
        std::vector<std::wstring> keep;
        int prio = 0;
    };
    void queue(Pending p);
    size_t pendingCount() const;
    // Sends queued pieces (priority, then shortest first) while each is
    // predicted to end before deadlineMs (steady clock, nowMs()); an accepted
    // answer replaces the piece in the cache, so translateTextsEx run again
    // shows it.  Returns the number of pieces improved; the queue is emptied.
    struct RouteTrace {
        std::wstring plain, out;
        int prio = 0;
        double ms = 0, predictedMs = 0;
        std::string result;  // "accepted", "rejected:<why>", "skipped:time", "failed"
    };
    int runPending(double deadlineMs, std::vector<RouteTrace>* trace = nullptr);
    static double now();  // steady clock ms (the deadlines' clock)
    // After a picture is shown: loads the LLM (one tiny request) if a picture
    // wanted it while it was cold, so the next pictures can use it.  On a
    // thread of its own (a slow load - seconds, a minute on a busy GPU - must
    // not hold up the next picture: the next run sees it as cold until it is
    // done); wait: here, blocking (tests: deterministic).  True if started.
    bool warmUp(bool wait = false);
    bool escalate(const TrRequest& rq, const std::wstring& plainSrc, TrHypothesis& h);
    // Step 2 of the pieces whose first translation fails a check, as one
    // engine call (parallel translators) before escalate() runs for each.
    void prefetchClauses(const std::vector<TrRequest>& failing);
    static std::vector<TrRequest> clauseRequests(const TrRequest& rq, size_t& clFrom, size_t& nCl);
    // Translations that passed the checks, per (src, tgt, source piece): in
    // memory only (owner decision: screen text never written to disk), up to
    // 4096 pieces, the oldest dropped first; cleared when the engines allowed
    // change.  Doubtful results are not kept (tried again next time).
    bool cached(Lang src, Lang tgt, const std::wstring& piece, TrHypothesis& h) const;
    void remember(Lang src, Lang tgt, const std::wstring& piece, const TrHypothesis& h);
    void clearCache();
    size_t cacheSize() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Details of translateTexts' results (one per input).
struct TextInfo {
    bool uncertain = false;          // a check still fails (step 6)
    std::wstring verified;           // the checked key facts (step 6)
    int step = 0;                    // highest escalation step used
    std::vector<std::string> flags;  // checks that failed at first
    bool row = false;                // a table row (label + value)
    bool online = false;             // a piece came from online translation (the card / list shows 「線上」)
    std::wstring alt;                // EscalationConfig::collectAlt: the text with the LLM's translation of every engine piece
    double altMs = 0;                // its LLM time
    std::string altFlags;            // checks the LLM's pieces fail
    std::string engines;             // the engine of each engine piece, '+' between (tests)
};
// translateTexts with table rows (labelLen per input, 0 = none), the escalator
// and the details.  The 6-argument translateTexts = this with defaults.
bool translateTextsEx(Engine& engine, Escalator* esc, Lang src, Lang tgt, const std::vector<std::wstring>& in,
                      const std::vector<size_t>& labelLen, std::vector<std::wstring>& out, std::vector<TextInfo>* info,
                      std::wstring* err);
}  // namespace pm::translate

namespace pm::translate {
// Marks at the start of VideoWindow::TextBox::text (read by
// video/src/text_overlay.cpp, which strips them): a table row (listed as
// 「標籤　值」, owner decision (1)); a translation that still failed a check
// (its last line: the checked key facts; selecting its list row shows the
// original).
constexpr wchar_t kOverlayRow = 0xE000;
constexpr wchar_t kOverlayUncertain = 0xE001;
// A box with only this mark: text kept as written (a track list, readings) -
// no card, but the list panel stays off it.
constexpr wchar_t kOverlayKeep = 0xE002;
}  // namespace pm::translate
