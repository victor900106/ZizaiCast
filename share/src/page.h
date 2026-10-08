// The phone-side page of 傳到手機 (HTML, UTF-8): pictures with a long-press
// hint, videos with a download button and save-to-Photos steps -- complete
// without script. Its script (app.js, same origin only) adds 「全部儲存」 /
// per-item 儲存 through navigator.share({files}) where the browser can share
// files (iOS: one sheet → 「儲存 N 張影像」 → Photos), and on a live share
// (自動傳到手機) long-polls list?n=K so new captures appear by themselves.
// Texts come from pm/i18n_strings.inc (Pg* entries).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pm::share {

struct PagePalette {
    uint32_t background, cardTop, cardBottom, fg, dim, accent;
    bool light;
};
struct PageFile {
    int index;
    std::string name;  // UTF-8
    bool video;
    uint64_t size;
    std::string type;  // Content-Type
};

// kind: "ios" | "android" | "other" (both sets of instructions). live: the
// newest file first, 「即時更新」, an empty state, the live footer
// (ttlSeconds = the cap).
std::string sharePage(int lang, const std::wstring& appName, const PagePalette& pal,
                      const std::vector<PageFile>& files, const std::string& kind, const std::string& untilHHMM,
                      int ttlSeconds, bool live = false);
std::string expiredPage(int lang, const std::wstring& appName, const PagePalette& pal, bool live = false);
// /<token>/list: {"live":…,"n":N,"files":[{"i","name","video","size","sizeText","type"}…]}
std::string listJson(const std::vector<PageFile>& files, bool live);
// /<token>/app.js
const std::string& shareScript();
std::string htmlEscape(const std::string& s);
std::string jsonEscape(const std::string& s);  // without the quotes; '<' as < (safe inside <script>)
std::string sizeText(uint64_t bytes);

}  // namespace pm::share
