// Prompts for the local LLM engine and the clean-up of its answers
// (llm_engine.h).  The wording was chosen by pm_llm_eval on
// translate/testdata/eval (see translate/tools/llm_eval.py for the runs):
// rules in English, the target named with its script and region, worked
// examples as earlier turns (few-shot), greedy decoding.
#include <cwctype>
#include <regex>

#include "llm_engine.h"
#include "text_util.h"

namespace pm::translate::llm {

namespace {

const char* langEn(Lang l) {
    switch (l) {
    case Lang::Ja: return "Japanese";
    case Lang::Ko: return "Korean";
    case Lang::En: return "English";
    case Lang::ZhHans: return "Simplified Chinese";
    case Lang::ZhHant: return "Traditional Chinese";
    default: return "the source language";
    }
}

const char* targetEn(Lang l) {
    switch (l) {
    case Lang::ZhHant: return "Traditional Chinese as written in Taiwan (繁體中文，台灣用語)";
    case Lang::En: return "English";
    case Lang::Ja: return "Japanese";
    case Lang::Ko: return "Korean";
    default: return langEn(l);
    }
}

std::string systemText(PromptStyle style, Lang src, Lang tgt) {
    std::string s = std::string("You translate text read from a photo or a phone screen (product labels, menus, signs, apps, "
                                "manuals) from ") +
                    langEn(src) + " into " + targetEn(tgt) +
                    ".\nRules:\n"
                    "- Answer with the translation only: no explanations, notes, romanization or the original text.\n"
                    "- Translate every part, including text in brackets, in the same order.\n"
                    "- Keep every negation and prohibition: ない, ず, 避け, 禁止, 않, 못, 금지, not, do not, never must stay "
                    "negative (不, 勿, 避免, 禁止, 請勿). Never turn \"avoid X\" into \"store in X\".\n"
                    "- Copy numbers, units, dates and codes exactly as written (78g, 42kcal, 26.12.09).\n"
                    "- Words like ZQA, ZQB, ZQC are placeholders: copy each one unchanged.\n"
                    "- Lines \"Context:\" and \"Heading:\" only explain where the text is; do not translate them.\n";
    if (tgt == Lang::ZhHant) s += "- Use Traditional Chinese characters and Taiwan wording only.\n";
    if (style == PromptStyle::Json) s += "- Answer as JSON: {\"t\": \"<translation>\"}\n";
    return s;
}

struct Example {
    Lang src;
    const char* text;
    const char* zh;
    const char* en;
};
// Not from the evaluation set: other wording of the same risks (a negation
// over a list, a placeholder, a table row, a Korean prohibition).
const Example kExamples[] = {
    {Lang::Ja, "お子様の手の届かない所に保管し、火気や湿気の多い場所では使用しないでください。（ZQA回まで）",
     "請存放於兒童拿不到的地方，勿在有火源或濕氣重的地方使用。（最多ZQA次）",
     "Keep out of the reach of children and do not use near fire or in humid places. (Up to ZQA times)"},
    {Lang::Ko, "개봉 후에는 냉장 보관하시고, 전자레인지에 넣지 마세요.", "開封後請冷藏保存，請勿放入微波爐。",
     "After opening, keep refrigerated and do not put it in the microwave."},
    {Lang::En, "Do not refreeze after thawing. Sodium 120mg", "解凍後請勿再次冷凍。鈉 120mg",
     "Do not refreeze after thawing. Sodium 120mg"},
};

std::string userText(const TrRequest& rq, bool strict = false, const std::wstring& pivot = {}) {
    std::string u;
    if (!rq.heading.empty()) u += "Heading: " + toUtf8(rq.heading) + "\n";
    if (!rq.context.empty()) u += "Context: " + toUtf8(rq.context) + "\n";
    if (!rq.terms.empty()) {
        u += "Terms:";
        for (const auto& [a, b] : rq.terms) u += " " + toUtf8(a) + " = " + toUtf8(b) + ";";
        u += "\n";
    }
    if (!pivot.empty()) u += "English machine translation (may be wrong; translate from the original): " + toUtf8(pivot) + "\n";
    if (strict && rq.tgt == Lang::ZhHant) {
        // The retry after a copied source: asked in Chinese, the source
        // language named, which the small model follows more reliably.
        const char* from = rq.src == Lang::Ja ? "日文" : rq.src == Lang::Ko ? "韓文" : rq.src == Lang::En ? "英文" : "原文";
        u += std::string("請把下面的") + from + "完整翻譯成繁體中文（台灣用語），不可保留假名或韓文字母，只輸出譯文：\n" +
             toUtf8(rq.text);
        return u;
    }
    u += "Translate:\n" + toUtf8(rq.text);
    if (strict) u += std::string("\n(Write the whole answer in ") + targetEn(rq.tgt) + ".)";
    return u;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += c;
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o;
}

struct Turns {
    std::string sysStart, sysEnd, userStart, userEnd, botStart, botEnd;
    bool systemInUser = false;  // gemma3: no system role, the rules go first in the user turn
};
Turns turnsFor(const std::string& family) {
    if (family == "gemma3")
        return {"", "", "<start_of_turn>user\n", "<end_of_turn>\n", "<start_of_turn>model\n", "<end_of_turn>\n", true};
    if (family == "gemma4") return {"<|turn>system\n", "<turn|>\n", "<|turn>user\n", "<turn|>\n", "<|turn>model\n", "<turn|>\n"};
    // chatml (Qwen3 / Qwen3.5): thinking off = an empty think block.
    return {"<|im_start|>system\n", "<|im_end|>\n", "<|im_start|>user\n", "<|im_end|>\n",
            "<|im_start|>assistant\n<think>\n\n</think>\n\n", "<|im_end|>\n"};
}

}  // namespace

PromptParts buildPrompt(const char* family, PromptStyle style, const TrRequest& rq, bool strict, const std::wstring& pivot) {
    const Turns t = turnsFor(family ? family : "chatml");
    const std::string sys = systemText(style, rq.src, rq.tgt);
    PromptParts p;
    std::string firstUser;  // gemma3: the rules are glued to the first user turn
    if (t.systemInUser) firstUser = sys + "\n";
    else p.prefix = t.sysStart + sys + t.sysEnd;
    if (style == PromptStyle::FewShot && (rq.tgt == Lang::ZhHant || rq.tgt == Lang::En)) {
        for (const auto& e : kExamples) {
            if (e.src == rq.tgt) continue;
            TrRequest ex;
            ex.text = fromUtf8(e.text);
            const std::string ans = rq.tgt == Lang::ZhHant ? e.zh : e.en;
            p.prefix += t.userStart + firstUser + userText(ex) + t.userEnd + t.botStart + ans + t.botEnd;
            firstUser.clear();
        }
    }
    p.suffix = t.userStart + firstUser + userText(rq, strict, pivot) + t.userEnd + t.botStart;
    if (style == PromptStyle::Json) {
        p.suffix += "";
        p.grammar = "root ::= \"{\\\"t\\\": \\\"\" ch* \"\\\"}\"\n"
                    "ch ::= [^\"\\\\\\x00-\\x1f] | \"\\\\\" [\"\\\\/nt]\n";
    }
    return p;
}

std::wstring cleanOutput(const std::string& raw, const TrRequest& rq, PromptStyle style) {
    std::string s = raw;
    // Reasoning the model wrote anyway.
    if (size_t e = s.rfind("</think>"); e != std::string::npos) s = s.substr(e + 8);
    for (;;) {
        const size_t a = s.find("<|channel>");
        if (a == std::string::npos) break;
        const size_t b = s.find("<channel|>", a);
        s.erase(a, b == std::string::npos ? std::string::npos : b + 10 - a);
    }
    for (const char* tok : {"<|im_end|>", "<end_of_turn>", "<turn|>", "<|endoftext|>", "<eos>"})
        if (size_t p = s.find(tok); p != std::string::npos) s.erase(p);
    if (style == PromptStyle::Json) {
        const size_t k = s.find("\"t\"");
        if (k != std::string::npos) {
            size_t q = s.find('"', s.find(':', k) + 1);
            std::string v;
            for (size_t i = q == std::string::npos ? s.size() : q + 1; i < s.size(); ++i) {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    const char c = s[++i];
                    v += c == 'n' ? '\n' : c == 't' ? '\t' : c;
                } else if (s[i] == '"') break;
                else v += s[i];
            }
            s = v;
        }
    }
    std::wstring w = fromUtf8(s);
    auto trim = [](std::wstring& x) {
        const wchar_t* ws = L" \t\r\n　";
        const size_t a = x.find_first_not_of(ws);
        if (a == std::wstring::npos) {
            x.clear();
            return;
        }
        x = x.substr(a, x.find_last_not_of(ws) - a + 1);
    };
    trim(w);
    // A label in front of the answer.
    static const std::wregex label(L"^(譯文|翻譯|翻译|譯|繁體中文|繁体中文|中文|Translation|Translated text|English)\\s*[:：]\\s*",
                                   std::regex::icase);
    w = std::regex_replace(w, label, L"");
    const bool srcMultiLine = rq.text.find(L'\n') != std::wstring::npos;
    if (!srcMultiLine) {
        // A note after a blank line, then the remaining line breaks of a one-line source.
        if (size_t p = w.find(L"\n\n"); p != std::wstring::npos) w.erase(p);
        trim(w);
        std::wstring j;
        for (wchar_t c : w) {
            if (c == L'\r') continue;
            if (c == L'\n') {
                if (rq.tgt == Lang::En || rq.tgt == Lang::Ko) j += L' ';
                continue;
            }
            j += c;
        }
        w = j;
    }
    // Wrapping quotes the source does not have.
    auto wrapped = [&](wchar_t a, wchar_t b) {
        return w.size() >= 2 && w.front() == a && w.back() == b && !(rq.text.size() && rq.text.front() == a);
    };
    for (auto [a, b] : {std::pair{L'「', L'」'}, {L'"', L'"'}, {L'“', L'”'}, {L'\'', L'\''}, {L'『', L'』'}})
        if (wrapped(a, b)) {
            w = w.substr(1, w.size() - 2);
            break;
        }
    trim(w);
    // Left (partly) untranslated: kana / hangul in a Chinese or English
    // answer.  "" lets the engine retry, then the escalator go on, instead of
    // showing the source as its translation.
    if (rq.tgt == Lang::ZhHant || rq.tgt == Lang::En) {
        size_t foreign = 0, letters = 0;
        for (wchar_t c : w) {
            const bool kana = c >= 0x3041 && c <= 0x30FF && c != 0x30FC && c != 0x30FB;
            const bool hangul = (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F);
            if (kana || hangul) ++foreign;
            if (iswalpha(c) || c >= 0x3040) ++letters;
        }
        if (foreign >= 2 && foreign * 5 >= letters) return {};
    }
    if (rq.tgt == Lang::ZhHant) {
        // Japanese kanji words the model left as they are (食塩相当量, 炭水化物, 地鶏塩焼):
        // the same word / shinjitai conversion as the rules (preprocess.cpp).
        if (rq.src == Lang::Ja) w = convertJapaneseKanji(w);
        w = fullWidthPunctuation(toTraditional(w));
    }
    return w;
}

}  // namespace pm::translate::llm
