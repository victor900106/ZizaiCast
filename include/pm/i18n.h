// UI language (中文 / English) for 自在投影 / Zizai Cast: one string table
// (pm/i18n_strings.inc) shared by app/ and video/, header-only.
//
//   pm::i18n::setLang(pm::i18n::Lang::En);
//   window.showToast(pm::i18n::tr(S::ShotSaved));
//   pm::i18n::fmt(S::RecSaved, {file})            // "{0}" "{1}" … placeholders
//
// The current language is a process-wide atomic: the render thread (video/)
// reads it every frame, the UI thread (app/) sets it. Every user-visible
// string of app/ and video/ lives in the table; see docs/app.md "Language".
#pragma once

#include <atomic>
#include <cstddef>
#include <initializer_list>
#include <string>

namespace pm::i18n {

enum class Lang : int { ZhTW = 0, En = 1 };

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

inline std::atomic<int>& langSlot() {
    static std::atomic<int> v{0};
    return v;
}
inline Lang lang() { return static_cast<Lang>(langSlot().load(std::memory_order_relaxed)); }
inline bool en() { return lang() == Lang::En; }
inline void setLang(Lang l) { langSlot().store(static_cast<int>(l), std::memory_order_relaxed); }

// The string in the current language (or in `l`).
inline const wchar_t* tr(S id, Lang l) {
    const Entry& e = kTable[static_cast<size_t>(id)];
    return l == Lang::En ? e.en : e.zh;
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

// UI font for this language (CJK text falls back automatically) and the
// DirectWrite locale name.
inline const wchar_t* uiFont() { return en() ? L"Segoe UI" : L"Microsoft JhengHei UI"; }
inline const wchar_t* localeName() { return en() ? L"en-us" : L"zh-TW"; }

}  // namespace pm::i18n
