// ScreenTranslator: the 「翻譯畫面」 flow (freeze -> grab -> OCR -> blocks ->
// models (consent + download) -> Bergamot -> overlay) on a worker thread.
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <regex>
#include <mutex>
#include <thread>

#include "pm/i18n.h"
#include "pm/translate.h"
#include "pm/video_window.h"
#include "text_util.h"

namespace pm::translate {

using pm::i18n::S;
using pm::i18n::tr;

namespace {
double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
std::wstring num(double v, int decimals) {
    wchar_t b[32];
    swprintf_s(b, decimals ? L"%.1f" : L"%.0f", v);
    return b;
}
std::wstring squeezeSpaces(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s)
        if (!iswspace(c)) o += c;
    return o;
}

}  // namespace

// A translation worth a card: not empty, not the original again, and in the
// target's script (a Japanese / Korean source must not come back as kana /
// hangul, a zh-Hant / ja / ko target needs CJK characters when the source had
// some).
bool translatedOk(const std::wstring& src, const std::wstring& tx, Lang from, Lang tgt) {
    if (tx.empty() || squeezeSpaces(tx) == squeezeSpaces(src)) return false;
    const ScriptCount s = countScripts(src), t = countScripts(tx);
    if (t.letters() == 0 && t.digits == 0) return false;
    if (tgt == Lang::ZhHant || tgt == Lang::En) {
        if (t.kana > 0 && t.kana * 4 >= t.letters()) return false;  // came back Japanese
        if (t.hangul > 0 && t.hangul * 4 >= t.letters()) return false;
    }
    if (tgt == Lang::En && t.han * 2 > t.letters()) return false;
    if ((tgt == Lang::ZhHant || tgt == Lang::Ja || tgt == Lang::Ko) && from != Lang::En && s.kana + s.hangul + s.han >= 3 &&
        t.kana + t.hangul + t.han == 0)
        return false;  // a CJK sentence came back as Latin only: the model gave up
    // One Latin word spelled out in Chinese sounds (OSENBEI -> 「奧森貝」, a
    // romanised name on a Japanese / Korean picture): no card.
    if (tgt == Lang::ZhHant && from == Lang::En && src.find(L' ') == std::wstring::npos && t.han >= 2 &&
        translitRun(tx) * 10 >= t.han * 7)
        return false;
    return true;
}

namespace {

// Background and text colour around a block (picture px, BGRA rows of the
// crop at cx0, cy0): the background = per-channel median of a ring just
// outside the box, the text = mean of the 15 % of pixels inside it that
// differ most from that.  0xRRGGBB.
void sampleColors(const std::vector<uint8_t>& px, int cw, int ch, int cx0, int cy0, float X0, float Y0, float X1, float Y1,
                  float lineHpx, uint32_t& bg, uint32_t& fg) {
    const int x0 = static_cast<int>(X0) - cx0, y0 = static_cast<int>(Y0) - cy0;
    const int x1 = static_cast<int>(std::ceil(X1)) - cx0, y1 = static_cast<int>(std::ceil(Y1)) - cy0;
    const int m = std::max(2, static_cast<int>(lineHpx * 0.3f));
    std::vector<uint8_t> ring[3];
    const int step = std::max(1, static_cast<int>(std::sqrt(static_cast<double>((x1 - x0 + 2 * m) * (y1 - y0 + 2 * m)) / 40)));
    auto at = [&](int x, int y) { return &px[(static_cast<size_t>(y) * cw + x) * 4]; };
    for (int y = y0 - m; y < y1 + m; y += step)
        for (int x = x0 - m; x < x1 + m; x += step) {
            if (x < 0 || y < 0 || x >= cw || y >= ch) continue;
            if (x >= x0 && x < x1 && y >= y0 && y < y1) continue;
            const uint8_t* q = at(x, y);
            for (int c = 0; c < 3; ++c) ring[c].push_back(q[c]);
        }
    int b[3] = {245, 245, 245};
    for (int c = 0; c < 3 && !ring[c].empty(); ++c) {
        auto mid = ring[c].begin() + ring[c].size() / 2;
        std::nth_element(ring[c].begin(), mid, ring[c].end());
        b[c] = *mid;
    }
    std::vector<std::pair<int, uint32_t>> in;  // distance, BGR
    const int st2 = std::max(1, step / 2);
    for (int y = std::max(0, y0); y < std::min(ch, y1); y += st2)
        for (int x = std::max(0, x0); x < std::min(cw, x1); x += st2) {
            const uint8_t* q = at(x, y);
            const int d = std::abs(q[0] - b[0]) + std::abs(q[1] - b[1]) + std::abs(q[2] - b[2]);
            in.push_back({d, static_cast<uint32_t>(q[0] | (q[1] << 8) | (q[2] << 16))});
        }
    int f[3] = {b[0] > 128 ? 20 : 240, b[1] > 128 ? 20 : 240, b[2] > 128 ? 20 : 240};
    if (!in.empty()) {
        const size_t k = std::max<size_t>(1, in.size() * 15 / 100);
        std::nth_element(in.begin(), in.begin() + (k - 1), in.end(), [](auto& a, auto& c) { return a.first > c.first; });
        if (in[k - 1].first > 60) {
            long long acc[3] = {0, 0, 0};
            for (size_t i = 0; i < k; ++i)
                for (int c = 0; c < 3; ++c) acc[c] += (in[i].second >> (8 * c)) & 255;
            for (int c = 0; c < 3; ++c) f[c] = static_cast<int>(acc[c] / static_cast<long long>(k));
        }
    }
    bg = static_cast<uint32_t>((b[2] << 16) | (b[1] << 8) | b[0]);
    fg = static_cast<uint32_t>((f[2] << 16) | (f[1] << 8) | f[0]);
}

bool katakanaOnly(const std::wstring& t) {
    int k = 0;
    for (wchar_t c : t) {
        if (iswspace(c) || c == L'・' || c == L'ー') continue;
        if (c >= 0x30A1 && c <= 0x30FA) ++k;
        else return false;
    }
    return k > 0;
}

}  // namespace

std::map<Lang, std::vector<size_t>> pickBlocks(std::vector<Block>& blocks, Lang src, Lang tgt, float aspect, bool trustText,
                                               std::vector<std::string>* why, Lang* screenLang) {
    if (why) why->assign(blocks.size(), {});
    auto skip = [&](size_t i, const char* r) {
        if (why) (*why)[i] = r;
    };
    // Screen language: kana anywhere -> Japanese (kanji-only labels too),
    // hangul -> Korean; otherwise the recogniser's / the block's own.
    ScriptCount all;
    for (const auto& b : blocks) all += countScripts(b.text);
    Lang cjk = Lang::Unknown;
    if (src == Lang::Ja || src == Lang::Ko || src == Lang::ZhHans || src == Lang::ZhHant) cjk = src;
    else if (all.kana >= 2) cjk = Lang::Ja;
    else if (all.hangul >= 2) cjk = Lang::Ko;
    else {
        // No kana: Japanese-only character forms (県 猟 駅 売, neither Simplified
        // nor Traditional Chinese) still make it a Japanese picture
        // (銃猟禁止区域 / 茨城県 was "already target" Chinese).
        int jaOnly = 0;
        for (const auto& b : blocks) jaOnly += japaneseOnlyKanji(b.text);
        if (jaOnly >= 1) cjk = Lang::Ja;
    }
    if (screenLang) *screenLang = cjk;
    // The main text (a label, a menu, a screen): blocks linked by gaps
    // under 3 line heights; the group with the most letters.  Short
    // blocks far from it on a photo are mostly packaging around the label
    // (a brand on a snack box, 「share happi」) or misreads.
    std::vector<int> group(blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i) group[i] = static_cast<int>(i);
    std::function<int(int)> root = [&](int i) { return group[i] == i ? i : group[i] = root(group[i]); };
    for (size_t i = 0; i < blocks.size(); ++i)
        for (size_t j = i + 1; j < blocks.size(); ++j) {
            const Block &a = blocks[i], &c = blocks[j];
            const float dx = std::max(0.f, std::max(a.x0, c.x0) - std::min(a.x1, c.x1)) * aspect;
            const float dy = std::max(0.f, std::max(a.y0, c.y0) - std::min(a.y1, c.y1));
            const float lh = std::max(std::min(a.lineH, c.lineH), 0.004f);  // big print does not reach far
            if (std::max(dx, dy) < 3 * lh) group[root(static_cast<int>(i))] = root(static_cast<int>(j));
        }
    std::map<int, int> groupLetters, groupBlocks;
    for (size_t i = 0; i < blocks.size(); ++i) {
        groupLetters[root(static_cast<int>(i))] += countScripts(blocks[i].text).letters();
        groupBlocks[root(static_cast<int>(i))]++;
    }
    int mainGroup = -1;
    for (const auto& [g, n] : groupLetters)
        if (mainGroup < 0 || n > groupLetters[mainGroup]) mainGroup = g;
    const bool photoLike = mainGroup >= 0 && groupBlocks[mainGroup] >= 5 && groupLetters[mainGroup] >= 60;
    std::map<Lang, std::vector<size_t>> byLang;
    // Phone status bars (the clock row: 16:34 · icons · 5G · battery), also
    // of a screenshot inside the picture: never text to translate.
    std::vector<char> statusBar(blocks.size(), 0);
    {
        static const std::wregex clock(L"^\\s*[0-9]{1,2}:[0-9]{2}(\\s|$)");
        for (size_t i = 0; i < blocks.size(); ++i) {
            const Block& c = blocks[i];
            if (c.lines != 1 || !std::regex_search(c.text, clock) || countScripts(c.text).letters() > 4) continue;
            const float cy = (c.y0 + c.y1) / 2, hh = std::max(c.y1 - c.y0, 0.004f);
            for (size_t k = 0; k < blocks.size(); ++k) {
                const Block& o = blocks[k];
                const float oc = (o.y0 + o.y1) / 2;
                if (o.lines == 1 && std::fabs(oc - cy) < 0.6f * hh && countScripts(o.text).letters() <= 5) statusBar[k] = 1;
            }
        }
        static const std::wregex radio(L"^[^A-Za-z]{0,4}(5G|4G|LTE|3G|Wi-?Fi|[0-9]{1,3}%)[^A-Za-z]{0,4}$");
        for (size_t i = 0; i < blocks.size(); ++i)
            if (blocks[i].y1 < 0.05f && countScripts(blocks[i].text).letters() <= 5 && std::regex_search(blocks[i].text, radio)) statusBar[i] = 1;
    }
    // A display name next to its @handle (ぽんこつ over @multi_wotakun): kept as
    // written - the pivot invents a transliteration (魔科特).
    std::vector<char> displayName(blocks.size(), 0);
    for (size_t i = 0; i < blocks.size(); ++i) {
        const Block& a = blocks[i];
        const ScriptCount na = countScripts(a.text);
        if (a.lines != 1 || na.letters() > 10 || a.text.find(L'@') != std::wstring::npos || na.kana + na.latin + na.han + na.hangul == 0) continue;
        for (size_t k = 0; k < blocks.size(); ++k) {
            const Block& h = blocks[k];
            if (k == i || h.text.empty() || h.text[0] != L'@') continue;
            const float lh = std::max(a.y1 - a.y0, 0.004f);
            if (h.y0 >= a.y1 - 0.3f * lh && h.y0 - a.y1 < 1.2f * lh && std::fabs(h.x0 - a.x0) * aspect < 2 * lh) displayName[i] = 1;
        }
    }
    for (size_t i = 0; i < blocks.size(); ++i) {
        Block& b = blocks[i];
        const ScriptCount n = countScripts(b.text);
        const bool isolated = photoLike && root(static_cast<int>(i)) != mainGroup;
        if (statusBar[i]) {
            skip(i, "status bar");
            continue;
        }
        if (displayName[i]) {
            skip(i, "display name");
            continue;
        }
        // A table row / field (賞味期限 26.12.09 on a sticker away from the label) is never noise.
        const bool row = b.labelLen > 0 || isFieldLabel(b.text);
        // Words of a sentence away from the main text are still text: a
        // note under a table (※この表示値は目安です。, 1袋（2枚）あたり), an
        // app's title / button (채팅 설정, 다시 시도).  Logos are mostly one
        // word: katakana / Latin / a few kanji / one hangul word.
        int hira = 0;
        for (wchar_t c : b.text) hira += c >= 0x3041 && c <= 0x309F;
        const bool sentenceLike = hira >= 2 || (n.hangul >= 2 && b.text.find(L' ') != std::wstring::npos);
        if (isolated && !row && !glossary(b.text, tgt) && !(sentenceLike && b.conf >= 0.8f)) {
            if (n.letters() <= 10) {  // 福奇, share happi, a logo
                skip(i, "isolated short");
                continue;
            }
            if (b.conf < 0.88f && n.letters() <= 14) {  // unsure, away from the text
                skip(i, "isolated unsure");
                continue;
            }
            if (katakanaOnly(b.text) && n.letters() <= 10) {  // ポッキー: a brand name
                skip(i, "isolated brand");
                continue;
            }
        }
        if (b.conf < 0.66f && !trustText) {  // mostly misread (a label too small / blurred to read)
            skip(i, "low confidence");
            continue;
        }
        if (n.letters() == 0) {  // 9:41, 5G, $89.99 …
            skip(i, "no letters");
            continue;
        }
        if (n.letters() < 2) {  // one character: a cut-off word or noise
            skip(i, "one letter");
            continue;
        }
        if (n.other > n.letters() + n.digits) {  // mostly symbols: OCR noise
            skip(i, "symbols");
            continue;
        }
        if (!row && n.digits >= n.letters() && n.letters() <= 3) {  // 468g(39g×12개), 2180 kJ, 12:30 PM (a row keeps its value)
            skip(i, "number");
            continue;
        }
        // Addresses (〒919-1552 福井県…) are kept as written by translateTexts
        // (Traditional forms), not sent to the engine (0.7.2 dropped them).
        if (cjk != Lang::Unknown && !row && n.letters() == n.latin && !glossary(b.text, tgt)) {
            // A short capitalised token on a Japanese / Korean picture
            // (camera UI "HEIF", a brand fragment): nothing to translate.
            bool lower = false, space = false;
            int words = 1;
            for (wchar_t c : b.text) lower |= c >= L'a' && c <= L'z', space |= c == L' ', words += c == L' ';
            // A brand / slogan word on Japanese / Korean packaging (happi,
            // share happi, Pocky): not text to translate.
            // Two words or more are a line to translate (English dish names, NO ADMISSION).
            // Single words are translated; a romanised name spelled out in Chinese
            // sounds gets no card (translatedOk).  Only 1-3 letter tokens (LTE, 5G, OK) are left.
            (void)lower, (void)space, (void)words;
            if (n.latin <= 3) {
                skip(i, "latin token");
                continue;
            }
        }
        // Chinese on a Japanese / Korean picture (a zh-TW app around a Japanese
        // tweet: 這名日本繪師一定是誤會臺灣了, 顯示翻譯) is Chinese - in the target
        // language it is left alone (0.7.5 sent it through ja -> en -> zh-Hant).
        // (Mostly Chinese: a bilingual label line Total fat/總脂肪 0g/克 is translated as before.)
        const int zhSig = n.kana == 0 && n.hangul == 0 && n.han > 0 && japaneseOnlyKanji(b.text) == 0 ? chineseSignals(b.text) : 0;
        const bool chinese = zhSig > 0 && n.latin <= 2 * n.han && (n.latin < n.han || zhSig >= 4);
        if (chinese && (tgt == Lang::ZhHant || cjk != Lang::Unknown)) {
            if (b.lang != Lang::ZhHans) b.lang = Lang::ZhHant;
            if (tgt == Lang::ZhHant) {
                skip(i, "already target");
                continue;
            }
        }
        if (!chinese && n.han > 0 && n.kana == 0 && n.hangul == 0 && n.latin * 2 < n.han && cjk != Lang::Unknown) {
            // Kanji / hanja only (賞味期限, 脂質, 焼菓子, 株式会社美十, an
            // address): Japanese -> zh-Hant shows them in Traditional forms
            // with the label words (translateTexts: 脂質 -> 脂肪, 焼 -> 燒; the
            // pivot would mangle them: 美十 -> 美州); unchanged text (錄影 of
            // a zh-TW camera app) gets no card (translatedOk).  Korean hanja:
            // left as before.
            if (tgt == Lang::ZhHant && cjk == Lang::Ko) {  // rows too (a zh-TW camera UI 錄影 拍照 read as a row)
                skip(i, "kanji only");
                continue;
            }
            b.lang = cjk;  // 設定, 一般 on a Japanese screen
        }
        // Kanji with a little kana on a Japanese screen (赏味期限枠外下部に記載,
        // a row 脂質 1.5g): Japanese, not Chinese.
        // On a Japanese picture "Chinese" is Japanese written without kana too
        // (熱量572Kcal、蛋白質16.2g、脂質38.2g …, 脂質 9.2 g were "already target").
        if (cjk == Lang::Ja && !chinese && (b.lang == Lang::ZhHant || b.lang == Lang::ZhHans) && n.hangul == 0) b.lang = Lang::Ja;
        // A row with a Latin value (熱量 42kcal, 김치찌개 9,000원 read as English): its label's language.
        if (row && b.lang == Lang::En && cjk == Lang::Ja && n.kana + n.han > 0) b.lang = Lang::Ja;
        if (row && b.lang == Lang::En && cjk == Lang::Ko && n.hangul > 0) b.lang = Lang::Ko;
        if (b.lang == Lang::ZhHant && tgt == Lang::ZhHant) {
            skip(i, "already target");
            continue;
        }
        // zh-Hans -> zh-Hant is a character conversion: nothing to show if it
        // changes nothing (or the "Chinese" is a misread Japanese / Korean picture).
        if (b.lang == Lang::ZhHans && tgt == Lang::ZhHant && (toTraditional(b.text) == b.text || cjk == Lang::Ja)) {
            skip(i, "already target");
            continue;
        }
        if (b.lang == Lang::Unknown || b.lang == tgt) {
            skip(i, "already target");
            continue;
        }
        if (b.lang == Lang::En && n.latin < 2) {
            skip(i, "one letter");
            continue;
        }
        byLang[b.lang].push_back(i);
    }
    return byLang;
}

struct ScreenTranslator::Impl {
    VideoWindow& win;
    Callbacks cb;
    std::thread worker;
    std::mutex m;
    std::condition_variable cv;
    struct Job {
        bool region = false;
        float x0 = 0, y0 = 0, x1 = 1, y1 = 1;
        bool liveRun = false;
        uint64_t gen = 0;
        bool first = false;  // the magnified (visible) part of a whole-screen job: shown, then the whole picture follows
    };
    std::deque<Job> jobs;
    bool stop = false;
    // State (guarded by m; UI thread changes it).
    Lang target = defaultTarget(), source = Lang::Unknown;
    bool active = false, busy = false, original = false, live = false, frozeByUs = false;
    int liveSec = 5;
    uint64_t gen = 0;  // bumped by close(): results of older runs are dropped
    Timing timing;
    std::vector<Item> items;
    Engine engine;  // worker thread only
    Escalator escalator{engine};  // worker thread only: checks + escalation (pm/translator.h)
    bool onlineAllowed = true;    // guarded by m; the user's switch is online::activeMode() (read per picture)
    std::atomic<bool> cancelDownload{false};
    bool ocrDeclined = false;  // worker thread: the OCR model download was refused (this session)
    // ---- 即時翻譯 (live mode): change-driven (worker thread only) ----
    // A 64-column luma thumbnail of the picture; a run starts when the
    // picture differs from the one last translated and has been still for
    // kSettleMs; nothing is grabbed while no new picture is decoded.
    struct Thumb {
        std::vector<uint8_t> y;
        int w = 0, h = 0;
    };
    Thumb lastThumb, doneThumb;          // last grabbed / last translated
    long long lastFrames = -1;           // VideoWindow::Stats::framesDecoded at the last look
    double lastGrabMs = 0, lastChangeMs = 0, movingSinceMs = 0, lastRunMs = -1e9;
    bool dirty = false, overlayHidden = false;
    bool lastLookStill = false;          // the last look found no change
    bool framesSinceLook = false;        // new pictures decoded since the last look
    LiveStats liveStats;                 // guarded by m

    static Thumb thumbOf(const std::vector<uint8_t>& bgra, int w, int h) {
        Thumb t;
        t.w = 64;
        t.h = std::clamp(static_cast<int>(std::lround(64.0 * h / std::max(1, w))), 8, 256);
        t.y.resize(static_cast<size_t>(t.w) * t.h);
        for (int ty = 0; ty < t.h; ++ty)
            for (int tx = 0; tx < t.w; ++tx) {
                // A few samples per cell (cheap: ~64 x 140 x 4 pixels).
                int acc = 0;
                for (int k = 0; k < 4; ++k) {
                    const int x = std::min(w - 1, static_cast<int>((tx + (k & 1 ? 0.75 : 0.25)) * w / t.w));
                    const int y = std::min(h - 1, static_cast<int>((ty + (k & 2 ? 0.75 : 0.25)) * h / t.h));
                    const uint8_t* p = &bgra[(static_cast<size_t>(y) * w + x) * 4];
                    acc += (p[0] * 29 + p[1] * 150 + p[2] * 77) >> 8;
                }
                t.y[static_cast<size_t>(ty) * t.w + tx] = static_cast<uint8_t>(acc / 4);
            }
        return t;
    }
    // Share of thumbnail cells that changed by more than 12 levels (0..1; 1 when the sizes differ).
    static double thumbDiff(const Thumb& a, const Thumb& b) {
        if (a.w != b.w || a.h != b.h || a.y.empty()) return 1;
        size_t n = 0;
        for (size_t i = 0; i < a.y.size(); ++i) n += std::abs(a.y[i] - b.y[i]) > 12;
        return static_cast<double>(n) / a.y.size();
    }
    static constexpr double kSettleMs = 300, kMinGapMs = 2000, kChanged = 0.004;
    // One look at the picture (worker thread, live mode, idle): true = translate now.
    bool livePoll() {
        const double now = nowMs();
        const long long frames = win.stats().framesDecoded;
        if (frames != lastFrames) {
            lastFrames = frames;
            framesSinceLook = true;
            // A new picture: look at it (at most 5 times a second; twice a
            // second when nothing changed for 2 s - a phone that keeps
            // sending the same screen; once a second while it has been
            // moving for 3 s - a playing video).
            const double every = movingSinceMs > 0 && now - movingSinceMs > 3000 ? 1000
                                 : now - lastChangeMs > 2000                       ? 500
                                                                                   : 200;
            if (now - lastGrabMs >= every) {
                lastGrabMs = now;
                framesSinceLook = false;
                std::vector<uint8_t> px;
                int w = 0, h = 0;
                if (win.grabPicture(px, w, h) && w > 0 && h > 0) {
                    Thumb t = thumbOf(px, w, h);
                    {
                        std::lock_guard lk(m);
                        ++liveStats.grabs;
                    }
                    lastLookStill = thumbDiff(t, lastThumb) <= kChanged;
                    if (!lastLookStill) {
                        lastChangeMs = now;
                        if (movingSinceMs <= 0) movingSinceMs = now;
                        dirty = thumbDiff(t, doneThumb) > kChanged;
                        // The overlay no longer matches the moving picture: hide it.
                        if (!overlayHidden && dirty) {
                            win.setTextOverlay({});
                            overlayHidden = true;
                            std::lock_guard lk(m);
                            ++liveStats.hidden;
                        }
                    }
                    lastThumb = std::move(t);
                }
            }
        }
        // Settled: kSettleMs since the last change, and a look since then
        // found it still (or no new picture came at all) - a video looked at
        // once a second never settles.
        const bool still = now - lastChangeMs >= kSettleMs && (lastLookStill || !framesSinceLook);
        if (still) movingSinceMs = 0;
        // Back to the translated picture (a video corner stopped) with the
        // overlay hidden: shown again by a run (from the caches).
        if (still && overlayHidden && !dirty) dirty = true;
        if (!dirty || !still) return false;
        if (now - lastRunMs < kMinGapMs) return false;  // at most one run per 2 s
        dirty = false;
        lastRunMs = now;
        std::lock_guard lk(m);
        ++liveStats.runs;
        liveStats.lastSettleMs = lastChangeMs;
        return true;
    }
    std::vector<std::pair<Lang, Lang>> warmPairs;  // worker thread: translated once (models loaded)

    Impl(VideoWindow& w, Callbacks c) : win(w), cb(std::move(c)) {
        worker = std::thread([this] { run(); });
    }
    ~Impl() {
        {
            std::lock_guard lk(m);
            stop = true;
        }
        cancelDownload = true;
        cv.notify_all();
        if (worker.joinable()) worker.join();
    }

    void changed() {
        if (cb.changed) win.post([f = cb.changed] { f(); });
    }
    void notify(const std::wstring& title, const std::wstring& text, bool important) {
        if (cb.notify) {
            win.post([f = cb.notify, title, text, important] { f(title, text, important); });
            return;
        }
        if (!important) {
            win.showToast(text, 3500);
            return;
        }
        VideoWindow* w = &win;
        win.post([w, title, text] { MessageBoxW(w->hwnd(), text.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION); });
    }
    // Blocks the worker until the user answered (or stop).
    bool askDownload(Lang src, Lang tgt, double mb) {
        auto state = std::make_shared<std::pair<std::mutex, int>>();  // -1 pending, 0 no, 1 yes
        state->second = -1;
        auto cvp = std::make_shared<std::condition_variable>();
        auto answer = [state, cvp](bool yes) {
            {
                std::lock_guard lk(state->first);
                if (state->second < 0) state->second = yes ? 1 : 0;
            }
            cvp->notify_all();
        };
        if (cb.askDownload) {
            win.post([f = cb.askDownload, src, tgt, mb, answer] { f(src, tgt, mb, answer); });
        } else {
            VideoWindow* w = &win;
            win.post([w, src, tgt, mb, answer] {
                const std::wstring text = src == Lang::Unknown ? pm::i18n::fmt(S::TrOcrModelAsk, {num(mb, 0)})
                                                               : pm::i18n::fmt(S::TrModelAsk, {langName(src), langName(tgt), num(mb, 0)});
                answer(MessageBoxW(w->hwnd(), text.c_str(), tr(S::TrModelTitle), MB_YESNO | MB_ICONQUESTION) == IDYES);
            });
        }
        std::unique_lock lk(state->first);
        while (state->second < 0) {
            cvp->wait_for(lk, std::chrono::milliseconds(200));
            std::lock_guard g(m);
            if (stop) return false;
        }
        return state->second == 1;
    }

    void enqueue(Job j) {
        {
            std::lock_guard lk(m);
            j.gen = gen;
            busy = true;
            jobs.push_back(j);
        }
        cv.notify_all();
        changed();
    }

    void run() {
        winrt_init();
        for (;;) {
            Job job;
            {
                std::unique_lock lk(m);
                for (;;) {
                    if (stop) return;
                    if (!jobs.empty()) break;
                    if (live && active && !busy) {
                        // 即時翻譯: when the picture changed and is still again.
                        lk.unlock();
                        const bool go = livePoll();
                        lk.lock();
                        if (go && live && active && !busy && jobs.empty()) {
                            jobs.push_back({false, 0, 0, 1, 1, true, gen});
                            busy = true;
                            break;
                        }
                        cv.wait_for(lk, std::chrono::milliseconds(100));
                        continue;
                    }
                    cv.wait(lk);
                }
                job = jobs.front();
                jobs.pop_front();
            }
            process(job);
            {
                std::lock_guard lk(m);
                busy = !jobs.empty();
            }
            changed();
        }
    }
    static void winrt_init() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }

    bool current(uint64_t g) {
        std::lock_guard lk(m);
        return g == gen && !stop;
    }
    void finish(bool ok, const Timing& t, uint64_t g, bool liveRun) {
        if (current(g) && !liveRun) win.setOverlayBusy(L"");
        if (cb.finished) win.post([f = cb.finished, ok, t] { f(ok, t); });
    }

    // Downloads model files with a progress card; false (and a message) on failure.
    bool downloadModels(const std::vector<std::string>& pairs, S busyText, uint64_t g, std::wstring* err) {
        cancelDownload = false;
        int lastPct = 0;
        if (current(g)) win.setOverlayBusy(pm::i18n::fmt(busyText, {L"0"}));  // at once, not after the first bytes
        const bool ok = ModelStore::download(pairs, [&](double f) {
            const int pct = static_cast<int>(f * 100);
            if (pct != lastPct && current(g)) {
                lastPct = pct;
                win.setOverlayBusy(pm::i18n::fmt(busyText, {std::to_wstring(pct)}));
            }
        }, &cancelDownload, err);
        if (!ok && *err != L"cancelled")
            notify(tr(S::TrModelTitle),
                   *err == L"SHA-256" ? std::wstring(tr(S::TrVerifyFailed)) : pm::i18n::fmt(S::TrDownloadFailed, {*err}), true);
        return ok;
    }

    void process(const Job& job) {
        Timing t;
        const double t0 = nowMs();
        Lang tgt, src;
        {
            std::lock_guard lk(m);
            tgt = target;
            src = source;
        }
        // 1) The picture (frozen one while frozen).
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        if (!win.grabPicture(px, w, h) || w <= 0 || h <= 0) {
            notify(tr(S::MenuTranslate), tr(S::TrNoPicture), false);
            finish(false, t, job.gen, job.liveRun);
            return;
        }
        t.grabMs = nowMs() - t0;
        if (!job.region) {  // 即時翻譯 compares later pictures with this one
            doneThumb = thumbOf(px, w, h);
            lastThumb = doneThumb;
            overlayHidden = false;
            dirty = false;
        }
        // Region: crop (content coordinates).
        int cx0 = 0, cy0 = 0, cw = w, ch = h;
        if (job.region) {
            cx0 = std::clamp(static_cast<int>(job.x0 * w), 0, w - 1);
            cy0 = std::clamp(static_cast<int>(job.y0 * h), 0, h - 1);
            cw = std::clamp(static_cast<int>(std::ceil(job.x1 * w)) - cx0, 1, w - cx0);
            ch = std::clamp(static_cast<int>(std::ceil(job.y1 * h)) - cy0, 1, h - cy0);
            std::vector<uint8_t> crop(static_cast<size_t>(cw) * ch * 4);
            for (int y = 0; y < ch; ++y)
                memcpy(crop.data() + static_cast<size_t>(y) * cw * 4, px.data() + (static_cast<size_t>(cy0 + y) * w + cx0) * 4,
                       static_cast<size_t>(cw) * 4);
            px.swap(crop);
        }
        // 2) OCR: PaddleOCR (first use: ask, download the models); Windows
        // OCR when the user said no or onnxruntime.dll is missing.
        OcrResult ocr;
        std::wstring err;
        bool paddle = false;
        if (!cb.ocrOverride && PaddleOcr::runtimeAvailable()) {
            if (!PaddleOcr::modelsInstalled() && !job.liveRun && !ocrDeclined) {
                // With a usable discrete GPU the OCR GPU add-on (DirectML, ~15 MB)
                // is part of the same download; it failing only leaves the CPU.
                const bool gpu = PaddleOcr::gpuWanted() && !ModelStore::ocrGpuInstalled();
                const uint64_t missingOcr = ModelStore::missingBytes({"ocr"}) + (gpu ? ModelStore::ocrGpuMissingBytes() : 0);
                if (!askDownload(Lang::Unknown, Lang::Unknown, missingOcr / 1e6)) {
                    ocrDeclined = true;  // this session: Windows OCR, no more asking
                } else if (!downloadModels({"ocr"}, S::TrDownloadingOcr, job.gen, &err)) {
                    finish(false, t, job.gen, false);
                    return;
                } else if (gpu) {
                    std::wstring gerr;
                    ModelStore::downloadOcrGpu(nullptr, &cancelDownload, &gerr);
                }
                if (!current(job.gen)) return;
                win.setOverlayBusy(tr(S::TrReading));
            }
            if (PaddleOcr::modelsInstalled()) {
                if (PaddleOcr::recognize(px.data(), cw, ch, ocr, &err)) paddle = true;
                else ocr = {};  // fall back to Windows OCR below
            }
        }
        if (cb.ocrOverride) {
            const float region[4] = {static_cast<float>(cx0) / w, static_cast<float>(cy0) / h,
                                     static_cast<float>(cx0 + cw) / w, static_cast<float>(cy0 + ch) / h};
            if (!cb.ocrOverride(px.data(), cw, ch, region, ocr)) {
                finish(false, t, job.gen, job.liveRun);
                return;
            }
        } else if (paddle) {
            // done
        } else if (Ocr::installed().empty()) {
            notify(tr(S::TrOcrMissingTitle), tr(S::TrOcrNone), true);
            finish(false, t, job.gen, job.liveRun);
            return;
        } else if (src != Lang::Unknown && !Ocr::available(src)) {
            notify(tr(S::TrOcrMissingTitle), pm::i18n::fmt(S::TrOcrMissing, {langName(src)}), true);
            finish(false, t, job.gen, job.liveRun);
            return;
        } else if (!Ocr::recognize(px.data(), cw, ch, src, ocr, &err)) {
            notify(tr(S::MenuTranslate), pm::i18n::fmt(S::TrFailed, {err}), false);
            finish(false, t, job.gen, job.liveRun);
            return;
        }
        t.ocrMs = ocr.ms;
        t.lines = static_cast<int>(ocr.lines.size());
        // Back to whole-picture coordinates.
        for (auto& l : ocr.lines) {
            l.x0 = (cx0 + l.x0 * cw) / w;
            l.x1 = (cx0 + l.x1 * cw) / w;
            l.y0 = (cy0 + l.y0 * ch) / h;
            l.y1 = (cy0 + l.y1 * ch) / h;
        }
        // A Japanese / Korean screen read without its recogniser: say how to add it.
        if (!cb.ocrOverride && !paddle && src == Lang::Unknown && (!Ocr::available(Lang::Ja) || !Ocr::available(Lang::Ko))) {
            std::wstring all;
            for (const auto& l : ocr.lines) all += l.text;
            if (looksMisread(all)) {
                std::wstring missing;
                for (Lang l : {Lang::Ja, Lang::Ko})
                    if (!Ocr::available(l)) missing += (missing.empty() ? L"" : L" / ") + langName(l);
                notify(tr(S::TrOcrMissingTitle), pm::i18n::fmt(S::TrOcrMissing, {missing}), true);
                finish(false, t, job.gen, job.liveRun);
                return;
            }
        }
        const double tl0 = nowMs();
        auto blocks = layoutBlocks(ocr.lines, static_cast<float>(w) / h);
        const double tl1 = nowMs();
        t.blocks = static_cast<int>(blocks.size());
        Lang cjk = Lang::Unknown;
        static const bool debug = std::getenv("PM_TR_DEBUG") != nullptr;  // tests: every block and why it is left out
        std::vector<std::string> why;
        auto byLang = pickBlocks(blocks, src, tgt, static_cast<float>(w) / h, cb.ocrOverride != nullptr, &why, &cjk);
        static const bool prof = std::getenv("PM_TR_PROF") != nullptr;
        if (prof)
            std::fprintf(stderr, "[tr] picture %dx%d: grab+crop %.0f ms, OCR %.0f ms, layout %.0f ms (%zu lines -> %zu blocks), pick %.0f ms\n", cw,
                         ch, t.grabMs, t.ocrMs, tl1 - tl0, ocr.lines.size(), blocks.size(), nowMs() - tl1);
        if (debug)
            for (size_t i = 0; i < blocks.size(); ++i)
                std::fprintf(stderr, "  [block] %s: %s\n", why[i].empty() ? "translate" : why[i].c_str(), toUtf8(blocks[i].text).c_str());
        if (byLang.empty() && job.first) return;  // nothing in the magnified part: the whole picture decides
        if (byLang.empty()) {
            Lang dominant = cjk != Lang::Unknown ? cjk : ocr.engine;
            notify(tr(S::MenuTranslate), blocks.empty() ? tr(S::TrNoText) : pm::i18n::fmt(S::TrSameLang, {langName(tgt)}), false);
            if (blocks.empty() && Ocr::installed().size() < 3) {
                // Maybe a language this PC cannot read: say how to add one.
                std::wstring missing;
                for (Lang l : {Lang::Ja, Lang::Ko})
                    if (!Ocr::available(l)) missing += (missing.empty() ? L"" : L" / ") + langName(l);
                if (!missing.empty())
                    notify(tr(S::TrOcrMissingTitle), pm::i18n::fmt(S::TrOcrMissing, {missing}), true);
            }
            (void)dominant;
            finish(false, t, job.gen, job.liveRun);
            return;
        }
        size_t biggest = 0;
        for (const auto& [l, v] : byLang)
            if (v.size() > biggest) biggest = v.size(), t.source = l;
        // 3) Models: ask once for everything missing, then download.
        if (!Engine::available(&err)) {
            notify(tr(S::MenuTranslate), tr(S::TrEngineMissing), true);
            finish(false, t, job.gen, job.liveRun);
            return;
        }
        std::vector<std::string> need;
        for (const auto& [l, v] : byLang)
            for (const auto& p : ModelStore::pairsFor(l, tgt))
                if (std::find(need.begin(), need.end(), p) == need.end()) need.push_back(p);
        const uint64_t missing = ModelStore::missingBytes(need);
        if (missing > 0) {
            if (job.liveRun) {  // live re-runs never download
                finish(false, t, job.gen, true);
                return;
            }
            if (!askDownload(t.source, tgt, missing / 1e6)) {
                notify(tr(S::MenuTranslate), tr(S::TrDeclined), false);
                finish(false, t, job.gen, false);
                return;
            }
            if (!downloadModels(need, S::TrDownloading, job.gen, &err)) {
                finish(false, t, job.gen, false);
                return;
            }
        }
        // 4) Translate, one batch per source language.
        bool firstUse = false;
        for (const auto& [l, v] : byLang)
            firstUse |= std::find(warmPairs.begin(), warmPairs.end(), std::pair{l, tgt}) == warmPairs.end();
        if (!job.liveRun && current(job.gen)) win.setOverlayBusy(tr(firstUse ? S::TrLoadingModel : S::TrBusy));
        const double tt = nowMs();
        std::vector<Item> result;
        std::vector<VideoWindow::TextBox> boxes;
        {
            EscalationConfig ec = escalator.config();
            std::lock_guard lk(m);
            ec.online = onlineAllowed;
            escalator.setConfig(ec);
        }
        const double tr0 = nowMs();
        escalator.resetBudget();
        if (prof) std::fprintf(stderr, "[tr] t+%.0f ms: translation starts (resetBudget %.0f ms)\n", nowMs() - t0, nowMs() - tr0);
        // Blocks per engine call: whole pictures for most, chunks shown as they
        // finish on dense screens (progressive).
        constexpr size_t kChunk = 24;
        bool progressive = true;
        size_t totalBlocks = 0;
        for (const auto& [l, idx] : byLang) totalBlocks += idx.size();
        auto pass = [&]() -> bool {
        result.clear();
        boxes.clear();
        for (const auto& [l, idxAll] : byLang) {
        for (size_t c0 = 0; c0 < idxAll.size(); c0 += kChunk) {
            const std::vector<size_t> idx(idxAll.begin() + c0, idxAll.begin() + std::min(idxAll.size(), c0 + kChunk));
            std::vector<std::wstring> in, out;
            std::vector<size_t> labels;
            std::vector<TextInfo> info;
            for (size_t i : idx) in.push_back(blocks[i].text), labels.push_back(blocks[i].labelLen);
            // Glossary, table rows, names, checks + escalation: label_text.cpp / translator.cpp.
            if (!translateTextsEx(engine, &escalator, l, tgt, in, labels, out, &info, &err)) {
                notify(tr(S::MenuTranslate), pm::i18n::fmt(S::TrFailed, {err}), true);
                return false;
            }
            warmPairs.push_back({l, tgt});
            for (size_t k = 0; k < idx.size() && k < out.size(); ++k) {
                const Block& b = blocks[idx[k]];
                std::wstring tx = out[k];
                while (!tx.empty() && iswspace(tx.back())) tx.pop_back();
                // Never show an untranslated card - but a table row is always
                // listed (「賞味期限　26.12.09」: owner decision (1)).
                const bool isRow = b.labelLen > 0 || (k < info.size() && info[k].row);
                if (!translatedOk(b.text, tx, l, tgt) && !(isRow && !tx.empty())) continue;
                // Marks for the overlay (text_overlay.cpp): a table row goes to the
                // list as 「標籤　值」; a translation that failed a check after
                // every escalation step carries the checked key facts on its last
                // line, and selecting its row shows the original.
                std::wstring shown = tx;
                if (k < info.size() && info[k].uncertain) {
                    if (!info[k].verified.empty()) tx += L"\n⚠ " + info[k].verified;
                    shown = std::wstring(1, kOverlayUncertain) + tx;
                } else if (k < info.size() && (info[k].row || b.labelLen)) {
                    shown = std::wstring(1, kOverlayRow) + tx;
                }
                result.push_back({b.text, tx, l, b.x0, b.y0, b.x1, b.y1, isRow, k < info.size() && info[k].uncertain,
                                  k < info.size() ? info[k].verified : std::wstring(), k < info.size() ? info[k].step : 0, k < info.size() && info[k].online});
                // A little margin around the text so the card covers it.
                const float by0 = b.cy1 > b.cy0 ? b.cy0 : b.y0, by1 = b.cy1 > b.cy0 ? b.cy1 : b.y1;
                const float lineH = (by1 - by0) / std::max(1, b.lines);
                const float mx = lineH * 0.15f * h / w, my = b.cy1 > b.cy0 ? 0.f : lineH * 0.10f;  // the glyph band is already a little larger than the glyphs
                VideoWindow::TextBox box{std::max(0.f, b.x0 - mx), std::max(0.f, by0 - my), std::min(1.f, b.x1 + mx),
                                         std::min(1.f, by1 + my), shown, b.text, b.lines};
                sampleColors(px, cw, ch, cx0, cy0, box.x0 * w, box.y0 * h, box.x1 * w, box.y1 * h, lineH * h, box.bg, box.fg);
                box.colors = true;
                boxes.push_back(std::move(box));
            }
        // Dense screens (a Wikipedia page: 84 blocks, 3.6k characters): the
        // blocks done so far are shown after each chunk (top of the page first).
        if (progressive && totalBlocks > kChunk && current(job.gen)) {
            win.setTextOverlay(boxes);
            if (t.firstMs <= 0) t.firstMs = nowMs() - t0;
            if (prof) std::fprintf(stderr, "[tr] t+%.0f ms: chunk shown\n", nowMs() - t0);
        }
        }
        }
            return true;
        };
        if (!pass()) {
            finish(false, t, job.gen, job.liveRun);
            return;
        }
        // Shown at once; then the local LLM on the queued pieces (failed checks,
        // garbage) within the picture's time budget (owner: 2-3 s in all), and
        // the improved result replaces it in place (progressive display).
        const size_t queued = escalator.pendingCount();
        if (queued && current(job.gen)) {
            win.setTextOverlay(boxes);
            progressive = false;
            if (escalator.runPending(t0 + escalator.config().targetMs) > 0 && current(job.gen) && !pass()) {
                finish(false, t, job.gen, job.liveRun);
                return;
            }
        }
        t.translateMs = nowMs() - tt;
        t.translated = static_cast<int>(result.size());
        t.totalMs = nowMs() - t0;
        if (!current(job.gen)) return;  // closed meanwhile
        win.setTextOverlay(boxes);
        {
            std::lock_guard lk(m);
            timing = t;
            items = std::move(result);
        }
        if (job.first) return;  // the whole picture comes next (same generation): not finished yet
        finish(true, t, job.gen, job.liveRun);
        // The result is on screen: load the LLM now if this picture wanted it
        // while it was cold (the next picture's budget can then use it).
        escalator.warmUp();
    }
};

ScreenTranslator::ScreenTranslator(VideoWindow& win, Callbacks cb) : impl_(std::make_unique<Impl>(win, std::move(cb))) {}

ScreenTranslator::~ScreenTranslator() {
    close();
    impl_.reset();
}

void ScreenTranslator::setTarget(Lang tgt) {
    std::lock_guard lk(impl_->m);
    impl_->target = (tgt == Lang::En || tgt == Lang::ZhHant || tgt == Lang::Ja || tgt == Lang::Ko) ? tgt : Lang::ZhHant;
}
Lang ScreenTranslator::target() const {
    std::lock_guard lk(impl_->m);
    return impl_->target;
}
void ScreenTranslator::setSource(Lang src) {
    std::lock_guard lk(impl_->m);
    impl_->source = src;
}
void ScreenTranslator::setOnlineAllowed(bool on) {
    std::lock_guard lk(impl_->m);
    impl_->onlineAllowed = on;
}
Lang ScreenTranslator::source() const {
    std::lock_guard lk(impl_->m);
    return impl_->source;
}

void ScreenTranslator::translateScreen() {
    bool isLive;
    {
        std::lock_guard lk(impl_->m);
        if (impl_->busy) return;
        isLive = impl_->live;
        impl_->active = true;
        impl_->original = false;
    }
    impl_->win.cancelRegionSelect();
    impl_->win.setTextOverlayOriginal(false);
    impl_->win.setTextOverlay({});
    if (!isLive && !impl_->win.viewState().frozen) {
        impl_->win.setFrozen(true);
        std::lock_guard lk(impl_->m);
        impl_->frozeByUs = true;
    }
    impl_->win.setOverlayBusy(tr(S::TrReading));
    // Magnified: the part on screen first (a dense page took 30 s+ as a
    // whole), then the whole picture replaces it.
    const VideoWindow::ViewState vs = impl_->win.viewState();
    static const bool noPart = std::getenv("PM_TR_NO_PART") != nullptr;  // tests: the whole picture only
    if (vs.zoom > 1.05f && !noPart) {
        Impl::Job part;
        part.region = part.first = true;
        const float half = 0.5f / vs.zoom;
        part.x0 = std::clamp(vs.centerX - half, 0.f, 1.f), part.x1 = std::clamp(vs.centerX + half, 0.f, 1.f);
        part.y0 = std::clamp(vs.centerY - half, 0.f, 1.f), part.y1 = std::clamp(vs.centerY + half, 0.f, 1.f);
        if (part.x1 - part.x0 > 0.02f && part.y1 - part.y0 > 0.02f) impl_->enqueue(part);
    }
    impl_->enqueue({});
}

void ScreenTranslator::translateRegion() {
    {
        std::lock_guard lk(impl_->m);
        if (impl_->busy) return;
    }
    const bool wasFrozen = impl_->win.viewState().frozen;
    if (!wasFrozen) impl_->win.setFrozen(true);  // the user selects on a still picture
    Impl* im = impl_.get();
    impl_->win.beginRegionSelect([im, wasFrozen](bool ok, float x0, float y0, float x1, float y1) {
        if (!ok) {
            bool keep;
            {
                std::lock_guard lk(im->m);
                keep = im->active;
            }
            if (!wasFrozen && !keep) im->win.setFrozen(false);
            return;
        }
        {
            std::lock_guard lk(im->m);
            im->active = true;
            im->original = false;
            if (!wasFrozen) im->frozeByUs = true;
        }
        im->win.setTextOverlayOriginal(false);
        im->win.setTextOverlay({});
        im->win.setOverlayBusy(tr(S::TrReading));
        Impl::Job j;
        j.region = true;
        j.x0 = x0, j.y0 = y0, j.x1 = x1, j.y1 = y1;
        im->enqueue(j);
    });
}

void ScreenTranslator::setShowOriginal(bool on) {
    {
        std::lock_guard lk(impl_->m);
        impl_->original = on;
    }
    impl_->win.setTextOverlayOriginal(on);
    impl_->changed();
}
bool ScreenTranslator::showOriginal() const {
    std::lock_guard lk(impl_->m);
    return impl_->original;
}

void ScreenTranslator::setLive(bool on, int seconds) {
    bool unfreeze = false;
    {
        std::lock_guard lk(impl_->m);
        impl_->live = on;
        impl_->liveSec = std::clamp(seconds, 2, 60);
        if (on && impl_->frozeByUs) {  // live follows the moving picture
            impl_->frozeByUs = false;
            unfreeze = true;
        }
    }
    if (unfreeze) impl_->win.setFrozen(false);
    impl_->cv.notify_all();
    impl_->changed();
}
ScreenTranslator::LiveStats ScreenTranslator::liveStats() const {
    std::lock_guard lk(impl_->m);
    return impl_->liveStats;
}
bool ScreenTranslator::live() const {
    std::lock_guard lk(impl_->m);
    return impl_->live;
}
bool ScreenTranslator::active() const {
    std::lock_guard lk(impl_->m);
    return impl_->active || impl_->busy;
}
bool ScreenTranslator::busy() const {
    std::lock_guard lk(impl_->m);
    return impl_->busy;
}

void ScreenTranslator::close() {
    bool unfreeze;
    {
        std::lock_guard lk(impl_->m);
        ++impl_->gen;
        impl_->jobs.clear();
        impl_->active = false;
        impl_->live = false;
        impl_->original = false;
        unfreeze = std::exchange(impl_->frozeByUs, false);
    }
    impl_->cancelDownload = true;
    impl_->win.cancelRegionSelect();
    impl_->win.setOverlayBusy(L"");
    impl_->win.setTextOverlay({});
    impl_->win.setTextOverlayOriginal(false);
    if (unfreeze) impl_->win.setFrozen(false);
    impl_->cv.notify_all();
    impl_->changed();
}

ScreenTranslator::Timing ScreenTranslator::lastTiming() const {
    std::lock_guard lk(impl_->m);
    return impl_->timing;
}

std::vector<ScreenTranslator::Item> ScreenTranslator::lastItems() const {
    std::lock_guard lk(impl_->m);
    return impl_->items;
}

}  // namespace pm::translate
