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
// 「文法 Grammar」, 「漢字 Kanji」, 「JLPTクイズ Quiz」: a short CJK label, then one or two
// capitalised English words (its gloss).
bool labelWithGloss(const std::wstring& t) {
    static const std::wregex re(L"^\\s*([^\\x00-\\x7F\\s][^\\s]{0,9}|[A-Z]{2,5}[^\\x00-\\x7F\\s]{1,8})\\s*([A-Z][a-z]{2,14}(\\s[A-Z][a-z]{2,14})?)\\s*$");
    return std::regex_match(t, re);
}

bool translatedOk(const std::wstring& src, const std::wstring& tx, Lang from, Lang tgt) {
    if (tx.empty() || squeezeSpaces(tx) == squeezeSpaces(src)) return false;
    const ScriptCount s = countScripts(src);
    // (kana kept in quotes on purpose - 「よん」, 「なな」 on a page about the
    // language - does not make the answer "came back Japanese")
    std::wstring unq;
    for (size_t i = 0, depth = 0; i < tx.size(); ++i) {
        if (tx[i] == L'「') ++depth;
        else if (tx[i] == L'」' && depth) --depth;
        else if (!depth) unq += tx[i];
    }
    const ScriptCount t = countScripts(unq.empty() ? tx : unq);
    if (t.letters() == 0 && t.digits == 0) return false;
    // A grammar pattern's name kept, its note converted (〜なり〜なり（選択） ->
    // ～なり～なり（選擇）): a card (label_text.cpp keeps the kana on purpose).
    if ((src[0] == L'〜' || src[0] == L'～' || src[0] == L'~') && (tx[0] == L'～' || tx[0] == L'〜')) return true;
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
    // An English proper name (one capitalised word: YASMINE, Ingrid - a game
    // character, a brand) is kept as written: spelled out in Chinese sounds
    // (雅辛, 英格里德) it is invented, not translated (owner 0.7.6).  The test:
    // the answer is mostly name-sound characters.
    if (tgt == Lang::ZhHant && from == Lang::En && t.han >= 2 && t.han <= 6 && (t.latin == 0 || src.find(L'-') != std::wstring::npos)) {
        // (J-nihongo: a hyphenated name starting with a capital counts too.)
        const bool hyphen = src.find(L'-') != std::wstring::npos && src.find(L' ') == std::wstring::npos && iswupper(src[0]);
        bool cap = s.latin >= 2 && s.latin == s.letters();
        int words = 0;
        for (size_t i = 0; i < src.size(); ++i)
            if (iswalpha(src[i]) && (i == 0 || !iswalpha(src[i - 1]))) {
                ++words;
                cap = cap && (iswupper(src[i]) || hyphen);
            }
        static const std::wstring kName =
            L"阿埃艾安奧巴拜班邦比彼波伯布查達戴丹德迪蒂多杜爾法菲弗福伽蓋岡戈格古哈漢赫霍吉基加賈傑卡凱坎科克庫拉萊蘭勞雷里利林"
            L"隆魯羅洛馬邁曼梅蒙米密莫姆納奈南尼諾帕佩皮普奇喬切薩塞桑瑟沙舍斯蘇索塔泰坦特提托圖瓦韋維溫沃西希謝辛雅亞楊伊因尤約扎"
            L"贊澤茲祖佐士美瑞莉娜麗絲琳珊娃妮婭莎蕾薇歷英麥黛琪潔裡娟婷詹露蒂芙明俊洪戈";
        int nm = 0;
        for (wchar_t c : tx) nm += kName.find(c) != std::wstring::npos;
        if (cap && words <= 2 && nm == t.han) return false;  // every character a name sound (伊瓦拉基縣 keeps its 縣)
    }
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

// One line of a credits / track list: 「ARTIST - TITLE」 in capitals or
// Title Case (BANGLES - WALK LIKE AN EGYPTIAN, Diana Ross - I'm Coming Out).
// Band names and song titles are names: the engine made 手鐲 of Bangles,
// 誘惑 of The Temptations, 綠色 of Al Green (owner, a GTA 6 tracklist).
// 2: a spaced separator and words on both sides; 1: OASIS-CIGARETTES / BELINDA CARLISLE- (only
// inside a list found from 2s: Tram-Train, XD-7, MAIZURU 1- alone are not credits).
int creditLine(const std::wstring& raw) {
    std::wstring t = raw;
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    while (!t.empty() && iswspace(t.front())) t.erase(0, 1);
    if (t.size() < 4 || t.size() > 80) return 0;
    for (wchar_t c : t)
        if (c >= 0x2E80) return 0;  // Latin lines only
    auto dash = [](wchar_t c) { return c == L'-' || c == 0x2013 || c == 0x2014; };
    // The separator: a dash with a space beside it (B-52'S - LOVE SHACK), else the first dash (OASIS-CIGARETTES).
    size_t sep = std::wstring::npos;
    bool spaced = false;
    for (size_t i = 0; i < t.size(); ++i)
        if (dash(t[i]) && ((i > 0 && iswspace(t[i - 1])) || (i + 1 < t.size() && iswspace(t[i + 1])))) { sep = i; spaced = true; break; }
    if (sep == std::wstring::npos)
        for (size_t i = 1; i < t.size(); ++i)
            if (dash(t[i])) { sep = i; break; }
    if (sep == std::wstring::npos || sep == 0) return 0;
    const std::wstring left = t.substr(0, sep), right = t.substr(sep + 1);
    // Names, not steps: 「Step 1 - Open Settings」, 「3 - Pour the water」.
    static const std::wregex steps(L"^\\s*((step|chapter|part|section|day|level|no\\.?|q|a)\\b.*|[0-9.\\s]*)$", std::regex::icase);
    if (std::regex_match(left, steps)) return 0;
    // A side is a name when it has no small letters (ALL CAPS) or most of its words start with a capital.
    auto named = [](const std::wstring& side, bool mayBeEmpty) {
        int words = 0, caps = 0, letters = 0, lower = 0;
        bool inWord = false;
        for (wchar_t c : side) {
            if (iswalpha(c)) {
                ++letters;
                if (iswlower(c)) ++lower;
                if (!inWord) { ++words; if (iswupper(c)) ++caps; inWord = true; }
            } else if (iswspace(c) || c == L'(' || c == L'/') inWord = false;
        }
        if (letters == 0) return mayBeEmpty;
        if (lower == 0) return true;
        return words <= 8 && caps * 10 >= words * 7;
    };
    int leftWords = 0;
    for (size_t i = 0; i < left.size(); ++i)
        if (!iswspace(left[i]) && (i == 0 || iswspace(left[i - 1]))) ++leftWords;
    if (leftWords > 5 || !named(left, false) || !named(right, true)) return 0;
    int rightLetters = 0;
    for (wchar_t c : right) rightLetters += iswalpha(c) ? 1 : 0;
    return spaced && rightLetters >= 2 ? 2 : 1;
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
    // A grid of kanji tiles (a kanji list: 日 一 国 / 会 人 年, each in a box of
    // its own): display content, not words - the recogniser joins neighbouring
    // tiles (自連発, 献維) and the pivot made 「會年」 of them.  A tile block is
    // kanji only, 1-4 characters set wide apart (each tile a box: 1.2+ glyph
    // heights a character); a grid is 4+ of them in 2+ rows close together.
    std::vector<char> kanjiTile(blocks.size(), 0);
    {
        std::vector<char> tileLike(blocks.size(), 0);
        for (size_t i = 0; i < blocks.size(); ++i) {
            const Block& b = blocks[i];
            std::wstring t;
            for (wchar_t c : b.text)
                if (!iswspace(c)) t += c;
            const ScriptCount n = countScripts(t);
            if (b.lines != 1 || t.empty() || t.size() > 4 || n.han != static_cast<int>(t.size())) continue;
            const float h = std::max(b.y1 - b.y0, 0.004f);
            const float perChar = (b.x1 - b.x0) * aspect / t.size() / h;
            tileLike[i] = t.size() == 1 || perChar >= 1.2f;
        }
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (!tileLike[i]) continue;
            const Block& b = blocks[i];
            const float h = std::max(b.y1 - b.y0, 0.004f), cy = (b.y0 + b.y1) / 2;
            int close = 0, otherRow = 0, wide = 0;
            for (size_t k = 0; k < blocks.size(); ++k) {
                if (!tileLike[k]) continue;
                const Block& o = blocks[k];
                const float oh = std::max(o.y1 - o.y0, 0.004f), oc = (o.y0 + o.y1) / 2;
                if (oh / h > 1.3f || h / oh > 1.3f) continue;
                const float dx = std::max(0.f, std::max(b.x0, o.x0) - std::min(b.x1, o.x1)) * aspect;
                if (std::fabs(oc - cy) > 4 * h || dx > 6 * h) continue;
                ++close;
                otherRow += std::fabs(oc - cy) > 0.7f * h;
                std::wstring t;
                for (wchar_t c : o.text)
                    if (!iswspace(c)) t += c;
                wide += t.size() >= 2;  // (tileLike: set wide apart)
            }
            // (single characters only: 6+ of them, a kanji chart)
            // (two rows of spaced-out kanji are tiles too: a card with 献 維 浜 / 墨 邦 遣)
            if (otherRow >= 1 && ((close >= 4 && (wide >= 1 || close >= 6)) || wide >= 2)) kanjiTile[i] = 1;
        }
    }
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
    // A credits / track list (three or more 「ARTIST - TITLE」 lines): names, kept as written.
    std::vector<char> credit(blocks.size(), 0);
    {
        int n = 0;
        for (size_t i = 0; i < blocks.size(); ++i)
            if (blocks[i].lines == 1)
                if (const int k = creditLine(blocks[i].text)) { credit[i] = 1; n += k == 2; }
        if (n < 3) std::fill(credit.begin(), credit.end(), 0);
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
    // A dictionary card (headword / reading / English gloss: 売り切れ over
    // うりきれ over sold out): the kana line right under a larger headword is
    // its READING - never translated (the pivot read うりきれ as 「尿道」); the
    // English gloss under it is learning content, kept as written.
    std::vector<char> reading(blocks.size(), 0), gloss(blocks.size(), 0);
    auto under = [&](const Block& top, const Block& b) {
        const float h = std::max(b.y1 - b.y0, 0.004f);
        const float ov = std::min(top.x1, b.x1) - std::max(top.x0, b.x0);
        return b.y0 >= top.y1 - 0.25f * h && b.y0 - top.y1 < 1.5f * h && ov > 0.5f * std::min(top.x1 - top.x0, b.x1 - b.x0);
    };
    for (size_t i = 0; i < blocks.size(); ++i) {
        const Block& r = blocks[i];
        const ScriptCount nr = countScripts(r.text);
        // Furigana: hiragana only, no punctuation (してください。 / ・チーズ are text).
        bool hira = r.lines == 1 && nr.kana >= 2 && nr.kana == nr.letters() && nr.kana <= 12;
        for (wchar_t c : r.text) hira = hira && ((c >= 0x3041 && c <= 0x309F) || c == 0x30FC || iswspace(c));
        if (!hira) continue;
        for (size_t k = 0; k < blocks.size(); ++k) {
            const Block& w = blocks[k];
            const ScriptCount nw = countScripts(w.text);
            if (k == i || nw.latin > 0 || (nw.han == 0 && nw.kana < 2)) continue;
            // (one line: its box, as the reading's - the glyph band lineH is smaller)
            const float hl = w.lines == 1 || w.lineH <= 0 ? (w.y1 - w.y0) / std::max(1, w.lines) : w.lineH;
            if (hl < 1.4f * (r.y1 - r.y0)) continue;  // furigana: a much smaller type
            // Ruby inside a sentence line: tight above its first line, within its
            // span (いま over 今 in 今の給料じゃ…).
            if (nw.han > 0 && r.y1 <= w.y0 + 0.3f * hl && w.y0 - r.y1 < 0.5f * hl && r.x0 >= w.x0 - 0.2f * hl / aspect &&
                r.x1 <= w.x1 + 0.2f * hl / aspect)
                reading[i] = 1;
            if (w.lines != 1 || nw.letters() > 10) continue;
            // Under the headword (a dictionary card), or above a word with kanji
            // (furigana: へいばんがた over 平板型, くだもの over 果物が), aligned on it:
            // left edges or centres (not a tab bar's label over the page behind it).
            const float hw = w.y1 - w.y0;
            const bool aligned = std::fabs(r.x0 - w.x0) * aspect < 0.6f * hw ||
                                 std::fabs((r.x0 + r.x1) - (w.x0 + w.x1)) * 0.5f * aspect < 0.6f * hw;
            if (aligned && (under(w, r) || (nw.han > 0 && under(r, w)))) reading[i] = 1;
        }
    }
    for (size_t i = 0; i < blocks.size(); ++i) {
        const Block& g = blocks[i];
        const ScriptCount ng = countScripts(g.text);
        if (g.lines > 2 || ng.latin == 0 || ng.latin != ng.letters() || ng.latin > 40) continue;
        for (size_t k = 0; k < blocks.size(); ++k)
            if (reading[k] && under(blocks[k], g)) gloss[i] = 1;
        // An English sentence right under a Japanese / Korean sentence (an example
        // and its translation on a learning page): learning content, kept as
        // written (owner 0.7.7).  Short English (a sign's NO SMOKING, a dish
        // name) is still translated.
        int words = 1;
        for (wchar_t c : g.text) words += c == L' ';
        // An example sentence: ends with . ? ! (or a quote), has lower case, and
        // the line above is a sentence too - a bilingual sign's English
        // (Bicycle riding is prohibited, MADE IN KOREA) is translated.
        std::wstring gt = g.text;
        while (!gt.empty() && iswspace(gt.back())) gt.pop_back();
        bool lowerCase = false;
        for (wchar_t c : gt) lowerCase |= c >= L'a' && c <= L'z';
        const bool exampleEn = !gt.empty() && lowerCase && wcschr(L".?!\"'”’)", gt.back()) != nullptr;
        if ((cjk == Lang::Ja || cjk == Lang::Ko) && words >= 4 && exampleEn)
            for (size_t k = 0; k < blocks.size(); ++k) {
                const Block& w = blocks[k];
                const ScriptCount nw = countScripts(w.text);
                if (k == i || nw.kana + nw.hangul < 3 || nw.letters() < 8 || nw.latin * 2 > nw.letters()) continue;
                std::wstring wt = w.text;
                while (!wt.empty() && iswspace(wt.back())) wt.pop_back();
                if (wt.empty() || !wcschr(L"。！？」』.!?", wt.back())) continue;
                const float h = std::max(w.lineH > 0 ? w.lineH : (w.y1 - w.y0) / std::max(1, w.lines), 0.004f);
                if (g.y0 >= w.y1 - 0.3f * h && g.y0 - w.y1 < 1.8f * h && std::fabs(g.x0 - w.x0) * aspect < 1.5f * h) gloss[i] = 1;
            }
        // One English word in small type over / beside a larger Japanese / Korean
        // label (Grammar over 文法, 読解 Reading): the label's gloss - kept.
        if ((cjk == Lang::Ja || cjk == Lang::Ko) && g.lines == 1 && g.text.find(L' ') == std::wstring::npos && ng.latin <= 14 &&
            iswupper(g.text[0]))  // Grammar, Kanji (not gourmet, (Beer))
            for (size_t k = 0; k < blocks.size(); ++k) {
                const Block& w = blocks[k];
                const ScriptCount nw = countScripts(w.text);
                if (k == i || w.lines != 1 || nw.han + nw.kana + nw.hangul < 2 || nw.letters() > 12 || nw.latin > 0) continue;
                if ((w.y1 - w.y0) < 2.0f * (g.y1 - g.y0)) continue;  // a much smaller type (a bilingual sign's English is translated)
                const float h = w.y1 - w.y0;
                const bool above = g.y1 <= w.y0 + 0.2f * h && w.y0 - g.y1 < 0.8f * h && std::fabs(g.x0 - w.x0) * aspect < 1.5f * h;
                const bool beside = std::fabs((g.y0 + g.y1) / 2 - (w.y0 + w.y1) / 2) < 0.5f * h && g.x0 > w.x1 && (g.x0 - w.x1) * aspect < 1.5f * h;
                if (above || beside) gloss[i] = 1;
            }
    }
    // A menu / tab bar / button row (ファイル(F) 編集(E) 表示(V), ホーム はじめる 文法):
    // three or more short items on one baseline are UI text, not packaging
    // around a label - since 0.7.7 they are separate items, not rows.
    auto uiRowAt = [&](size_t i) {
        const Block& b = blocks[i];
        if (b.lines != 1 || b.conf < 0.8f || countScripts(b.text).letters() > 12) return false;
        int onRow = 0;
        for (const Block& o : blocks) {
            const float h = std::max(b.y1 - b.y0, 0.004f), oh = o.y1 - o.y0;
            const int ol = countScripts(o.text).letters();
            if (o.lines == 1 && ol >= 2 && ol <= 12 && std::fabs((o.y0 + o.y1) / 2 - (b.y0 + b.y1) / 2) < 0.4f * h && oh > 0.7f * h &&
                oh < 1.4f * h)
                ++onRow;
        }
        return onRow >= 3;
    };
    // A page about the Japanese language (readings over the words, kana quoted
    // as examples: 「がくせいが」「あいさつが」): its kana example words are
    // learning content, kept as written - the pivot spelled them out in sound
    // characters (加古塞伊, 奧哈那米).  Signals: 2+ furigana readings, or 3+
    // quoted hiragana words on the picture.
    // 3+ blocks with the vocabulary of language teaching (拍, 助詞, 音読み …) count too.
    int kanaQuotes = 0, readings = 0, terms = 0;
    {
        static const std::wregex quoted(L"[「（(][ぁ-ゖー・]{1,10}[」）)]");
        static const std::wregex term(L"(拍|アクセント|助詞|文型|ふりがな|読み方|音読み|訓読み|ひらがな|カタカナ|例文|発音|活用|品詞)");
        for (size_t i = 0; i < blocks.size(); ++i) {
            readings += reading[i];
            for (std::wsregex_iterator it(blocks[i].text.begin(), blocks[i].text.end(), quoted), e; it != e; ++it) ++kanaQuotes;
            terms += std::regex_search(blocks[i].text, term);
        }
    }
    const bool learningPage = cjk == Lang::Ja && (readings >= 2 || kanaQuotes >= 3 || terms >= 3);
    setLearningPage(learningPage);
    // A screen (a phone's status bar, crisp text: the recogniser is sure of
    // most lines) is not a photographed package: its short labels away from
    // the main text (ログイン, TOPへ, 12画, a tab) are UI text, translated.
    bool screenLike = std::any_of(statusBar.begin(), statusBar.end(), [](char c) { return c != 0; });
    {
        std::vector<float> conf;
        for (const auto& b : blocks) conf.push_back(b.conf);
        if (conf.size() >= 5 && !trustText) {  // (--gt boxes carry no confidence)
            std::nth_element(conf.begin(), conf.begin() + conf.size() / 2, conf.end());
            screenLike = screenLike || conf[conf.size() / 2] >= 0.95f;
        }
    }
    for (size_t i = 0; i < blocks.size(); ++i) {
        Block& b = blocks[i];
        const ScriptCount n = countScripts(b.text);
        const bool isolated = photoLike && !screenLike && root(static_cast<int>(i)) != mainGroup;
        if (reading[i] || gloss[i]) {
            skip(i, reading[i] ? "reading" : "gloss");
            continue;
        }
        if (learningPage && !uiRowAt(i)) {
            // An example word: hiragana only (with ・ or spaces between the morae:
            // き・っ・て), not a UI label on a tab bar (はじめる).
            std::wstring t;
            for (wchar_t c : b.text)
                if (!iswspace(c)) t += c;
            bool hira = !t.empty() && t.size() <= 14;
            for (wchar_t c : t) hira = hira && ((c >= 0x3041 && c <= 0x3096) || c == 0x30FC || c == 0x30FB);
            if (hira && !dataGlossary(b.text, cjk, tgt) && !glossary(b.text, tgt)) {  // (ふりがな: a UI toggle, in the glossary)
                skip(i, "example word");
                continue;
            }
        }
        if (kanjiTile[i]) {
            skip(i, "kanji tile");
            continue;
        }
        if (statusBar[i]) {
            skip(i, "status bar");
            continue;
        }
        if (credit[i]) {
            skip(i, "name list");
            continue;
        }
        // A URL / domain alone (the browser's address bar: www.streetfighter.com,
        // j-nihongo.com): nothing to translate.
        {
            static const std::wregex url(L"^\\s*(\\S{1,2}\\s+)?(https?://)?([A-Za-z0-9][A-Za-z0-9-]*\\.)+[A-Za-z]{2,}(/\\S*)?\\s*$");
            if (std::regex_match(b.text, url) && b.text.find(L'.') != std::wstring::npos) {
                skip(i, "address");
                continue;
            }
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
        // A menu / tab bar / button row (ファイル(F) 編集(E) 表示(V), ホーム はじめる 文法):
        // three or more short items on one baseline are UI text, not packaging
        // around a label - since 0.7.7 they are separate items, not rows.
        const bool uiRow = uiRowAt(i);
        if (isolated && !row && !uiRow && !glossary(b.text, tgt) && !(sentenceLike && b.conf >= 0.8f)) {
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
        if (!row && n.digits >= n.letters() && n.letters() <= 3 && n.han < 2) {  // 468g(39g×12개), 2180 kJ, 12:30 PM (a row keeps its value)
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
        // A Japanese / Korean label with its English gloss on one line (文法 Grammar,
        // 漢字 Kanji, はじめる Start): the label's language - label_text.cpp keeps
        // the gloss as written (0.7.7: 「文法文」, 「康司」 through the English engine).
        if (b.lang == Lang::En && (cjk == Lang::Ja || cjk == Lang::Ko) && labelWithGloss(b.text)) b.lang = cjk;
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

PicturePlan planPicture(const std::vector<OcrLine>& lines, Lang src, Lang tgt, float aspect, bool trustText, bool legacy) {
    PicturePlan plan;
    plan.blocks = legacy ? groupLines(lines, aspect) : layoutBlocks(lines, aspect);
    plan.byLang = pickBlocks(plan.blocks, src, tgt, aspect, trustText, &plan.why, &plan.screenLang);
    return plan;
}

bool cardWorthy(const Block& b, const std::wstring& tx, const TextInfo* info, Lang from, Lang tgt) {
    std::wstring t = tx;
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    // Never show an untranslated card - but a table row is always listed
    // (「賞味期限　26.12.09」: owner decision (1)).
    const bool isRow = b.labelLen > 0 || (info && info->row);
    return translatedOk(b.text, t, from, tgt) || (isRow && !t.empty());
}

std::wstring cardText(const std::wstring& tx, const TextInfo* info) {
    return info && info->uncertain && !info->verified.empty() ? tx + L"\n⚠ " + info->verified : tx;
}

bool translatePlan(Engine& engine, Escalator* esc, const PicturePlan& plan, Lang tgt, size_t chunk, const PlanHooks& hooks,
                   std::wstring* err) {
    if (chunk == 0) chunk = kPlanChunk;
    for (const auto& [l, idxAll] : plan.byLang) {
        if (hooks.beforeLang && !hooks.beforeLang(l)) return false;
        for (size_t c0 = 0; c0 < idxAll.size(); c0 += chunk) {
            const std::vector<size_t> idx(idxAll.begin() + c0, idxAll.begin() + std::min(idxAll.size(), c0 + chunk));
            std::vector<std::wstring> in, out;
            std::vector<size_t> labels;
            std::vector<TextInfo> info;
            for (size_t i : idx) in.push_back(plan.blocks[i].text), labels.push_back(plan.blocks[i].labelLen);
            // Glossary, table rows, names, checks + escalation: label_text.cpp / translator.cpp.
            const bool ok = hooks.legacy ? translateTexts(engine, l, tgt, in, out, err)
                                         : translateTextsEx(engine, esc, l, tgt, in, labels, out, &info, err);
            if (!ok) return false;
            if (hooks.chunkDone && !hooks.chunkDone(l, idx, in, out, info)) return false;
        }
    }
    return true;
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
    // prewarm(): asked (guarded by m), done (worker thread).
    bool warmAsked = false, warmDone = false;
    Lang warmSrc = Lang::Unknown, warmTgt = Lang::Unknown;
    bool ocrWarm = false;
    // State (guarded by m; UI thread changes it).
    Lang target = defaultTarget(), source = Lang::Unknown;
    bool active = false, busy = false, original = false, live = false, frozeByUs = false;
    int liveSec = 5;
    uint64_t gen = 0;  // bumped by close(): results of older runs are dropped
    Timing timing;
    std::vector<Item> items;
    Engine engine;  // worker thread only
    Escalator escalator{engine};  // worker thread only: checks + escalation (pm/translator.h)
    std::unique_ptr<Escalator> earlyEsc;  // the early top part's (process()): checks only
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
    static constexpr bool kOverlayFollows = true;  // 0.7.8 即時翻譯: VideoWindow::setTextOverlayLive
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
                        // The overlay no longer matches the moving picture: hide it - 0.7.8: no,
                        // it follows scrolling and hides itself otherwise (video/src/live_overlay.cpp).
                        if (!overlayHidden && dirty && !kOverlayFollows) {
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
            bool warmNow = false;
            Lang wsrc = Lang::Unknown, wtgt = Lang::Unknown;
            {
                std::unique_lock lk(m);
                for (;;) {
                    if (stop) return;
                    if (!jobs.empty()) break;
                    if (warmAsked) {
                        warmAsked = false;
                        warmNow = true;
                        wsrc = warmSrc, wtgt = target;
                        break;
                    }
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
                if (!warmNow) {
                    job = jobs.front();
                    jobs.pop_front();
                }
            }
            if (warmNow) {
                warm(wsrc, wtgt);
                continue;
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

    // prewarm(): the OCR sessions and the src -> tgt chain, at a low priority.
    void warm(Lang src, Lang tgt) {
        const bool pair = src != Lang::Unknown && src != tgt &&
                          std::find(warmPairs.begin(), warmPairs.end(), std::pair{src, tgt}) == warmPairs.end();
        const bool ocr = !ocrWarm && !cb.ocrOverride && PaddleOcr::ready();
        if (!ocr && !pair) return;
        HANDLE th = GetCurrentThread();
        const int prio = GetThreadPriority(th);
        SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL);
        const double t0 = nowMs();
        std::wstring err;
        if (ocr) ocrWarm = PaddleOcr::warmUp(&err);
        const double t1 = nowMs();
        bool pairOk = false;
        if (pair && Engine::available() && ModelStore::missingBytes(ModelStore::pairsFor(src, tgt)) == 0) {
            std::vector<std::wstring> out;
            pairOk = engine.translate(src, tgt, {L"OK"}, out, &err);
            if (pairOk) warmPairs.push_back({src, tgt});
        }
        SetThreadPriority(th, prio);
        std::fprintf(stderr, "[tr] warm: OCR %s %.0f ms, %ls -> %ls %s %.0f ms\n", ocr ? (ocrWarm ? "loaded" : "failed") : "skipped",
                     t1 - t0, langTag(src), langTag(tgt), pair ? (pairOk ? "loaded" : "not loaded") : "skipped", nowMs() - t1);
    }

    bool current(uint64_t g) {
        std::lock_guard lk(m);
        return g == gen && !stop;
    }
    void finish(bool ok, const Timing& t, uint64_t g, bool liveRun) {
        if (current(g) && !liveRun) win.setOverlayBusy(L"");
        if (cb.finished) {
            Timing tt = t;
            tt.live = liveRun;
            win.post([f = cb.finished, ok, tt] { f(ok, tt); });
        }
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
        // A translated block -> its result item and its card (nothing when the
        // translation failed).
        auto card = [&](const Block& b, std::wstring tx, const TextInfo* inf, Lang l, Lang to, std::vector<Item>& res,
                        std::vector<VideoWindow::TextBox>& bx) {
            while (!tx.empty() && iswspace(tx.back())) tx.pop_back();
            if (!cardWorthy(b, tx, inf, l, to)) return;
            const bool isRow = b.labelLen > 0 || (inf && inf->row);
            // Marks for the overlay (text_overlay.cpp): a table row goes to the
            // list as 「標籤　值」; a translation that failed a check after
            // every escalation step carries the checked key facts on its last
            // line, and selecting its row shows the original.
            std::wstring shown = tx;
            if (inf && inf->uncertain) {
                tx = cardText(tx, inf);
                shown = std::wstring(1, kOverlayUncertain) + tx;
            } else if (inf && (inf->row || b.labelLen)) {
                shown = std::wstring(1, kOverlayRow) + tx;
            }
            res.push_back({b.text, tx, l, b.x0, b.y0, b.x1, b.y1, isRow, inf && inf->uncertain,
                           inf ? inf->verified : std::wstring(), inf ? inf->step : 0, inf && inf->online});
            // A little margin around the text so the card covers it.
            const float by0 = b.cy1 > b.cy0 ? b.cy0 : b.y0, by1 = b.cy1 > b.cy0 ? b.cy1 : b.y1;
            const float lineH = (by1 - by0) / std::max(1, b.lines);
            const float mx = lineH * 0.15f * h / w, my = b.cy1 > b.cy0 ? 0.f : lineH * 0.10f;  // the glyph band is already a little larger than the glyphs
            VideoWindow::TextBox box{std::max(0.f, b.x0 - mx), std::max(0.f, by0 - my), std::min(1.f, b.x1 + mx),
                                     std::min(1.f, by1 + my), shown, b.text, b.lines};
            sampleColors(px, cw, ch, cx0, cy0, box.x0 * w, box.y0 * h, box.x1 * w, box.y1 * h, lineH * h, box.bg, box.fg);
            box.colors = true;
            bx.push_back(std::move(box));
        };
        // Dense pictures: the top part's lines come early from the OCR (the
        // split is at a gap between paragraphs); its first blocks are
        // translated and shown on another thread while the rest is read (the
        // engine is idle meanwhile; joined before the worker uses it again).
        // The whole picture then goes through the usual steps and replaces it.
        // Own escalator: checks only (no LLM / online), the picture's own one
        // stays as without it.  Off unless PM_TR_EARLY=1: measured on the E-cores (PM_OVERLAY_FIRST_SHOT) it did not show the first cards earlier (the early thread takes CPU from the OCR of the rest), see the commit message.
        std::thread earlyThread;
        double earlyMs = 0;
        struct Joiner {
            std::thread& th;
            ~Joiner() {
                if (th.joinable()) th.join();
            }
        } joinEarly{earlyThread};
        static const bool earlyOn = std::getenv("PM_TR_EARLY") && std::getenv("PM_TR_EARLY")[0] == '1';
        std::function<void(std::vector<OcrLine>)> early;
        if (earlyOn && !job.region && !cb.ocrOverride)
            early = [&](std::vector<OcrLine> top) {
                if (!current(job.gen) || earlyThread.joinable()) return;
                earlyThread = std::thread([&, top = std::move(top)]() {
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);  // the OCR of the rest first
                    const float aspect = static_cast<float>(w) / h;
                    auto eb = layoutBlocks(top, aspect);
                    std::vector<std::string> ewhy;
                    auto eby = pickBlocks(eb, src, tgt, aspect, false, &ewhy);
                    std::vector<std::string> need;
                    for (const auto& [l, v] : eby)
                        for (const auto& pr : ModelStore::pairsFor(l, tgt)) need.push_back(pr);
                    if (eby.empty() || !Engine::available() || ModelStore::missingBytes(need) > 0) return;  // never downloads
                    if (!earlyEsc) {
                        earlyEsc = std::make_unique<Escalator>(engine);
                        EscalationConfig ec = earlyEsc->config();
                        ec.localLlm = ec.online = false;
                        earlyEsc->setConfig(ec);
                    }
                    earlyEsc->resetBudget();
                    std::vector<Item> res;
                    std::vector<VideoWindow::TextBox> bx;
                    size_t left = 24;  // as the first chunk of the whole picture
                    for (const auto& [l, idxAll] : eby) {
                        if (!left) break;
                        const std::vector<size_t> idx(idxAll.begin(), idxAll.begin() + std::min(idxAll.size(), left));
                        left -= idx.size();
                        std::vector<std::wstring> in, out;
                        std::vector<size_t> labels;
                        std::vector<TextInfo> info;
                        std::wstring e;
                        for (size_t i : idx) in.push_back(eb[i].text), labels.push_back(eb[i].labelLen);
                        if (!translateTextsEx(engine, earlyEsc.get(), l, tgt, in, labels, out, &info, &e)) return;
                        for (size_t k = 0; k < idx.size() && k < out.size(); ++k)
                            card(eb[idx[k]], out[k], k < info.size() ? &info[k] : nullptr, l, tgt, res, bx);
                    }
                    if (bx.empty() || !current(job.gen)) return;
                    for (size_t i = 0; i < eb.size() && i < ewhy.size(); ++i)  // kept as written: as pass() below
                        if (ewhy[i] == "name list" || ewhy[i] == "reading" || ewhy[i] == "gloss" || ewhy[i] == "display name")
                            bx.push_back({eb[i].x0, eb[i].y0, eb[i].x1, eb[i].y1, std::wstring(1, kOverlayKeep), eb[i].text, eb[i].lines});
                    win.setTextOverlay(bx);
                    earlyMs = nowMs() - t0;
                    static const bool prof = std::getenv("PM_TR_PROF") != nullptr;
                    if (prof) std::fprintf(stderr, "[tr] t+%.0f ms: top part shown early (%zu cards, %zu lines)\n", earlyMs, res.size(), top.size());
                });
            };
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
                if (PaddleOcr::recognize(px.data(), cw, ch, ocr, &err, src, early)) paddle = true;
                else ocr = {};  // fall back to Windows OCR below
            }
        }
        if (earlyThread.joinable()) earlyThread.join();  // the engine is the worker's again
        if (earlyMs > 0) t.firstMs = earlyMs;
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
        // Layout + pick (planPicture: the same call as pm_translate_test --eval).
        const PicturePlan plan = planPicture(ocr.lines, src, tgt, static_cast<float>(w) / h, cb.ocrOverride != nullptr);
        const auto& blocks = plan.blocks;
        const auto& byLang = plan.byLang;
        const auto& why = plan.why;
        const Lang cjk = plan.screenLang;
        t.blocks = static_cast<int>(blocks.size());
        static const bool debug = std::getenv("PM_TR_DEBUG") != nullptr;  // tests: every block and why it is left out
        static const bool prof = std::getenv("PM_TR_PROF") != nullptr;
        if (prof)
            std::fprintf(stderr, "[tr] picture %dx%d: grab+crop %.0f ms, OCR %.0f ms, layout + pick %.0f ms (%zu lines -> %zu blocks)\n", cw, ch,
                         t.grabMs, t.ocrMs, nowMs() - tl0, ocr.lines.size(), blocks.size());
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
        bool progressive = true;
        const size_t totalBlocks = plan.picked();
        auto pass = [&]() -> bool {
        result.clear();
        boxes.clear();
        PlanHooks hooks;  // translatePlan: the same call as pm_translate_test --eval
        hooks.chunkDone = [&](Lang l, const std::vector<size_t>& idx, const std::vector<std::wstring>&, const std::vector<std::wstring>& out,
                              const std::vector<TextInfo>& info) {
            warmPairs.push_back({l, tgt});
            for (size_t k = 0; k < idx.size() && k < out.size(); ++k)
                card(blocks[idx[k]], out[k], k < info.size() ? &info[k] : nullptr, l, tgt, result, boxes);
        // Dense screens (a Wikipedia page: 84 blocks, 3.6k characters): the
        // blocks done so far are shown after each chunk (top of the page first).
        if (progressive && totalBlocks > kPlanChunk && current(job.gen)) {
            win.setTextOverlay(boxes);
            if (t.firstMs <= 0) t.firstMs = nowMs() - t0;
            if (prof) std::fprintf(stderr, "[tr] t+%.0f ms: chunk shown\n", nowMs() - t0);
        }
            return true;
        };
        if (!translatePlan(engine, &escalator, plan, tgt, kPlanChunk, hooks, &err)) {
            notify(tr(S::MenuTranslate), pm::i18n::fmt(S::TrFailed, {err}), true);
            return false;
        }
            // Text deliberately kept as written (a name / track list, readings, an
            // English gloss): the overlay keeps its list panel off it (kOverlayKeep).
            for (size_t i = 0; i < blocks.size() && i < why.size(); ++i)
                if (why[i] == "name list" || why[i] == "reading" || why[i] == "gloss" || why[i] == "display name") {
                    const Block& b = blocks[i];
                    boxes.push_back({b.x0, b.y0, b.x1, b.y1, std::wstring(1, kOverlayKeep), b.text, b.lines});
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

void ScreenTranslator::prewarm(Lang src) {
    {
        std::lock_guard lk(impl_->m);
        if (impl_->warmDone && impl_->warmSrc == src && impl_->warmTgt == impl_->target) return;  // asked already
        impl_->warmAsked = impl_->warmDone = true;
        impl_->warmSrc = src, impl_->warmTgt = impl_->target;
    }
    impl_->cv.notify_all();
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
    bool unfreeze = false, first = false;
    {
        std::lock_guard lk(impl_->m);
        impl_->live = on;
        impl_->liveSec = std::clamp(seconds, 2, 60);
        if (on && impl_->frozeByUs) {  // live follows the moving picture
            impl_->frozeByUs = false;
            unfreeze = true;
        }
        // Turned on with nothing translated yet (the first translation after
        // the app started): the live loop only watches an active translation,
        // and its re-runs never ask to download - so 0.7.6 did nothing until
        // 翻譯整個畫面 had run once.  The first run is a full one: the same
        // set-up (the download consent, the models) as 翻譯整個畫面.
        first = on && !impl_->active && !impl_->busy;
    }
    if (unfreeze) impl_->win.setFrozen(false);
    impl_->win.setTextOverlayLive(on);  // 0.7.8: in place, follows scrolling (video/src/live_overlay.cpp)
    if (first) translateScreen();  // (live: the picture is not frozen)
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
    impl_->win.setTextOverlayLive(false);
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
