// UI language (繁體中文 / English / 日本語 / 한국어) for 自在投影 / Zizai Cast:
// one string table shared by app/, video/, translate/ and share/,
// header-only.
//
//   pm::i18n::setLang(pm::i18n::Lang::En);
//   window.showToast(pm::i18n::tr(S::ShotSaved));
//   pm::i18n::fmt(S::RecSaved, {file})            // "{0}" "{1}" … placeholders
//
// pm/i18n_strings.inc holds every id with its 繁體中文 and English text
// (PM_STR); pm/i18n_strings_jako.inc adds 日本語 and 한국어 (PM_JAKO, 0.7.0).
// An id missing there falls back to English.
//
// The current language is a process-wide atomic: the render thread (video/)
// reads it every frame, the UI thread (app/) sets it. Every user-visible
// string of app/ and video/ lives in the table; see docs/app.md "Language".
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <initializer_list>
#include <string>

namespace pm::i18n {

enum class Lang : int { ZhTW = 0, En = 1, Ja = 2, Ko = 3 };

enum class S : int {
#define PM_STR(id, zh, en) id,
#include "pm/i18n_strings.inc"
#undef PM_STR
    Count_
};

struct Entry {
    const wchar_t* zh;
    const wchar_t* en;
};

inline constexpr Entry kTable[] = {
#define PM_STR(id, zh, en) {zh, en},
#include "pm/i18n_strings.inc"
#undef PM_STR
};
static_assert(sizeof(kTable) / sizeof(kTable[0]) == static_cast<size_t>(S::Count_));

struct JaKo {
    S id;
    const wchar_t* ja;
    const wchar_t* ko;
};
inline constexpr JaKo kJaKo[] = {
#define PM_JAKO(id, ja, ko) {S::id, ja, ko},
#include "pm/i18n_strings_jako.inc"
#undef PM_JAKO
    {S::Count_, nullptr, nullptr},
};
// id -> row of kJaKo (-1: not translated, English is used).
inline constexpr auto kJaKoIndex = [] {
    std::array<short, static_cast<size_t>(S::Count_)> a{};
    for (auto& x : a) x = -1;
    for (size_t i = 0; i + 1 < sizeof(kJaKo) / sizeof(kJaKo[0]); ++i) a[static_cast<size_t>(kJaKo[i].id)] = static_cast<short>(i);
    return a;
}();

inline std::atomic<int>& langSlot() {
    static std::atomic<int> v{0};
    return v;
}
inline Lang lang() { return static_cast<Lang>(langSlot().load(std::memory_order_relaxed)); }
inline bool en() { return lang() == Lang::En; }
inline bool zh() { return lang() == Lang::ZhTW; }
inline void setLang(Lang l) { langSlot().store(static_cast<int>(l), std::memory_order_relaxed); }

// The string in the current language (or in `l`).
inline const wchar_t* tr(S id, Lang l) {
    const Entry& e = kTable[static_cast<size_t>(id)];
    switch (l) {
    case Lang::ZhTW: return e.zh;
    case Lang::Ja:
    case Lang::Ko: {
        const short i = kJaKoIndex[static_cast<size_t>(id)];
        if (i >= 0) {
            const wchar_t* s = l == Lang::Ja ? kJaKo[i].ja : kJaKo[i].ko;
            if (s && *s) return s;
        }
        return e.en;
    }
    default: return e.en;
    }
}
inline const wchar_t* tr(S id) { return tr(id, lang()); }

// "{0}", "{1}" … replaced by args (missing ones by nothing).
inline std::wstring fill(const std::wstring& pattern, std::initializer_list<std::wstring> args) {
    std::wstring out;
    out.reserve(pattern.size() + 32);
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == L'{' && i + 2 < pattern.size() && pattern[i + 1] >= L'0' && pattern[i + 1] <= L'9' &&
            pattern[i + 2] == L'}') {
            const size_t k = static_cast<size_t>(pattern[i + 1] - L'0');
            if (k < args.size()) out += *(args.begin() + k);
            i += 2;
            continue;
        }
        out += pattern[i];
    }
    return out;
}
inline std::wstring fmt(S id, std::initializer_list<std::wstring> args) { return fill(tr(id), args); }

// UI font for this language and the DirectWrite locale name. The locale
// picks the right CJK glyph forms and DirectWrite's font fallback (Yu Gothic
// UI / Malgun Gothic / JhengHei) for any character the font lacks.
inline const wchar_t* uiFont(Lang l) {
    switch (l) {
    case Lang::En: return L"Segoe UI";
    case Lang::Ja: return L"Yu Gothic UI";
    case Lang::Ko: return L"Malgun Gothic";
    default: return L"Microsoft JhengHei UI";
    }
}
inline const wchar_t* uiFont() { return uiFont(lang()); }
inline const wchar_t* localeName(Lang l) {
    switch (l) {
    case Lang::En: return L"en-us";
    case Lang::Ja: return L"ja-JP";
    case Lang::Ko: return L"ko-KR";
    default: return L"zh-TW";
    }
}
inline const wchar_t* localeName() { return localeName(lang()); }
// 한국어 paragraphs wrap between words, not inside one (0.7.2: DirectWrite
// breaks between any two Hangul syllables, 「들어가|지」 / 「여|기」 in the
// update window).  A WORD JOINER (U+2060: no width, nothing drawn) between
// syllables of a word; other languages unchanged.  Only for text drawn or
// measured as a whole (it shifts character indexes).
inline std::wstring keepWords(const std::wstring& s) {
    if (lang() != Lang::Ko) return s;
    auto hangul = [](wchar_t c) { return c >= 0xAC00 && c <= 0xD7A3; };
    std::wstring o;
    o.reserve(s.size() + s.size() / 2);
    for (size_t i = 0; i < s.size(); ++i) {
        o += s[i];
        if (i + 1 < s.size() && hangul(s[i]) && hangul(s[i + 1])) o += L'\u2060';
    }
    return o;
}
// Short tag for logs / settings: zh-TW, en, ja, ko.
inline const char* langKey(Lang l) {
    switch (l) {
    case Lang::En: return "en";
    case Lang::Ja: return "ja";
    case Lang::Ko: return "ko";
    default: return "zh-TW";
    }
}

}  // namespace pm::i18n
