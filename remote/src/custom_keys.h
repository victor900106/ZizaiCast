#pragma once
// 0.8.4 自訂快捷鍵: the buttons the owner added to the phone page's keyboard panel, kept on the PC (the app's
// settings.ini, one list for the 主帳號 whatever device it signs in from). The page sends the list, this
// module decides what is allowed: at most 12 buttons, each a name (<= 40 bytes, no control characters) and
// 1..5 key names from the table the injector understands (keyNameKnown, input.cpp), at most one of them not a
// modifier. Anything else refuses the whole list (a tampered page cannot smuggle a key in).
//
// Format (compact JSON, ASCII only so it fits one settings.ini line):
//   [{"n":"Clipboard","c":["Meta","V"],"t":"dClip"}, ...]
//   n  name shown on the button ("" = show the combination, or the built-in name "t" in the page's language)
//   c  key names, canonical order: Control, Alt, Shift, Meta, then the one other key
//   t  optional: a built-in name of the three default buttons (dClip / dReopen / dLock), translated by the page
#include <string>

namespace pm::remote {

constexpr size_t kMaxCustomKeys = 12;
constexpr size_t kMaxCustomKeyName = 40;  // bytes (UTF-8)

// The three buttons a fresh install starts with: Win+V (剪貼簿歷史), Ctrl+Shift+T (the Task Manager already has
// its own fixed button), Win+L (鎖定).
std::string defaultCustomKeys();

// Checks `json` and writes its canonical form to `out`. false = refused (`why`: parse / count / name / key / combo / type).
bool sanitizeCustomKeys(const std::string& json, std::string& out, std::string* why = nullptr);

}  // namespace pm::remote
