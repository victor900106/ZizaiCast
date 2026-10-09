// Pluggable translation engines and the escalation order of the on-screen
// translation (launch/_work/tr_arch/ARCHITECTURE.md §3.7, owner decisions
// (5) and (7): accuracy first).  Stable interface: other engines implement
// ITranslator in their own files and are found through the factories below.
//
// Escalation for one segment (translate/src/translator.cpp, Escalator):
//   0. rules (glossary, field labels, kanji shown in 繁體, numbers kept)
//   1. Bergamot (bergamot.dll; ja/ko -> zh-Hant through English)
//   checks (qe): negation kept, numbers kept, placeholders, script
//   2. failed -> the source split into clauses, each translated again
//   3. still failed -> local LLM            (only if installed: installed())
//   4. still failed -> online translation   (only if the user turned it on)
//   5. still failed -> high-risk sentence templates (保存方法 / 注意 / allergens)
//   6. still failed -> the best text, with the checked key facts (negation /
//      prohibition, numbers, dates) written out from the source, and the
//      original kept for "tap to compare" (TrHypothesis::verified / uncertain)
//
// Implementing an engine (e.g. translate/src/llm_engine.cpp,
// translate/src/online_engine.cpp - picked up by translate/CMakeLists.txt by
// file name, llm_engine*.cpp / online_engine*.cpp):
//   extern "C" pm::translate::ITranslator* pm_create_local_llm_translator();
//   extern "C" pm::translate::ITranslator* pm_create_online_translator();
// return a new object (the caller owns it; nullptr = not available).  The
// library has default definitions returning nullptr (MSVC /alternatename),
// so a build without those files links and simply skips the step.
#pragma once

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pm/translate.h"

namespace pm::translate {

// One segment to translate (a block, a table value, a clause).
struct TrRequest {
    std::wstring text;                 // source text (may contain placeholders ZQA..ZQZ, kept as is)
    Lang src = Lang::Unknown, tgt = Lang::Unknown;
    std::wstring context;              // the text around it (previous segment / block), "" if none
    std::wstring heading;              // its table label / section heading (栄養成分表示, 保存方法), "" if none
    std::vector<std::pair<std::wstring, std::wstring>> terms;  // glossary hints: source term -> target term
    std::vector<std::wstring> keep;    // substrings that must appear unchanged (numbers, placeholders)
    bool negation = false;             // the source has a negation / prohibition (keep it)
    std::wstring pivot;                // Bergamot's English of text, when known: an LLM's last-resort
                                       // fallback (better than its own English); not part of any cache key
    double budgetMs = 0;               // > 0: the engine should not take longer (an LLM skips its retries
                                       // when they would not fit); 0 = no limit
};

struct TrHypothesis {
    std::wstring text;                 // the translation ("" = none)
    std::string engine;                // ITranslator::id() of the engine that produced it
    double ms = 0;                     // time spent on this request (share of a batch)
    bool pivot = false;                // went through English
    std::wstring pivotText;            // the English in between, when known
    // Set by the escalator:
    int step = 0;                      // escalation step that produced it (see the header comment)
    bool uncertain = false;            // still failed a check after every step
    std::wstring verified;             // step 6: the checked key facts, in the target language
    std::vector<std::string> flags;    // "neg-missing", "neg-scope", "num-missing:0.05", "placeholder"
};

struct TrCost {
    double msPerChar = 0;              // typical, warm (per source character)
    double msPerRequest = 0;           // fixed part of one request (prompt, warm-up): ms ~ msPerRequest + msPerChar * chars
    double loadMs = 0;                 // first use
    int memoryMB = 0;
};

class ITranslator {
public:
    virtual ~ITranslator() = default;
    virtual const char* id() const = 0;  // "bergamot", "llm-qwen3-1.7b-q4", "online-deepl"
    // The pair is possible (direct: without a pivot language).
    virtual bool supports(Lang src, Lang tgt, bool* direct = nullptr) const = 0;
    // Ready to run now (models downloaded, user consent / key given).
    virtual bool installed(Lang src, Lang tgt) const = 0;
    // Bytes to download before installed() (0 if none / not downloadable).
    virtual uint64_t missingBytes(Lang /*src*/, Lang /*tgt*/) const { return 0; }
    // Sends text off the PC (only used when the user turned it on).
    virtual bool isOnline() const { return false; }
    virtual TrCost cost() const { return {}; }
    // out: one hypothesis per request (text "" when that one failed).  Blocking;
    // called from the translation worker thread only, one call at a time.
    virtual bool translate(const std::vector<TrRequest>& in, std::vector<TrHypothesis>& out, std::wstring* err) = 0;
};

// Settings of the escalation (ScreenTranslator / tests).
struct EscalationConfig {
    bool splitClauses = true;          // step 2
    bool localLlm = true;              // step 3, when an LLM engine is installed
    bool online = false;               // step 4: only when the user turned online translation on
    bool templates = true;             // step 5
    bool markVerified = true;          // step 6
    double llmBudgetMs = 20000;        // per picture: escalations stop using the LLM after this
    bool collectAlt = false;           // tests (pm_translate_test --eval): the LLM on every ja / ko piece too, kept in TextInfo::alt
    // Time budget (owner: a picture in 2-3 s, LLM included).  deferLlm: the
    // LLM is not called inside the checks (step 3) but queued; the caller
    // shows the result, then Escalator::runPending(deadline) sends the most
    // suspicious / shortest pieces while each is predicted (ITranslator::cost)
    // to end before the deadline, and shows the improved result.
    bool deferLlm = true;
    double targetMs = 2500;            // from the start of the picture: no LLM request predicted to end later
    double capMs = 3000;               // hard cap (runPending never starts a request after it)
    bool llmShortItems = false;        // also queue short items (<= 10 characters) at priority 1
};

// The engines found in this build (nullptr when not built in / not available).
std::unique_ptr<ITranslator> createLocalLlmTranslator();
std::unique_ptr<ITranslator> createOnlineTranslator();

}  // namespace pm::translate

extern "C" pm::translate::ITranslator* pm_create_local_llm_translator();
extern "C" pm::translate::ITranslator* pm_create_online_translator();
