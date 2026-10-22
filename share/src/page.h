// The phone-side page of 傳到手機 (HTML, UTF-8): pictures with a long-press
// hint, videos with a download button and save-to-Photos steps, and
// 「全部下載（ZIP）」 (every file as one ZIP, with how to get them into Photos /
// the gallery) -- complete without script. Its script (app.js, same origin
// only) swaps the ZIP for 「全部儲存」 / per-item 儲存 through
// navigator.share({files}) where the browser can share files (a secure
// context only, so not the LAN page's http; iOS: one sheet → 「儲存 N 張影像」
// → Photos).
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

// 0.8.3: texts not in the string table yet (UTF-8, in the page language) and
// the 「電腦剪貼簿」 card (clip: shown, clipText: the PC's text now).
struct PageExtra {
    std::string clipTitle, clipCopy, clipCopied, clipRefresh, clipEmpty, clipFail, clipNote, downloadFile, fileTip;
    bool clip = false;
    std::string clipText;
};

// kind: "ios" | "android" | "other" (both sets of instructions).  Files that
// are neither pictures nor videos get a plain file card (name, size, 下載檔案).
std::string sharePage(int lang, const std::wstring& appName, const PagePalette& pal,
                      const std::vector<PageFile>& files, const std::string& kind, const std::string& untilHHMM,
                      int ttlSeconds, const PageExtra* extra = nullptr);
// /<token>/clip: {"text":"…"}
std::string clipJson(const std::string& textUtf8);
std::string expiredPage(int lang, const std::wstring& appName, const PagePalette& pal);
// /<token>/app.js
const std::string& shareScript();
std::string htmlEscape(const std::string& s);
std::string jsonEscape(const std::string& s);  // without the quotes; '<' as < (safe inside <script>)
std::string sizeText(uint64_t bytes);

}  // namespace pm::share
