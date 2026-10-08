// Text helpers of pm_translate: scripts, UTF-8, Chinese variants, punctuation.
#pragma once

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
}  // namespace pm::translate
