// Label / sign / menu text fixes around the translation engine: glossary,
// table fields, names kept, misleading kana spellings.  See docs/translate.md.
#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "pm/translate.h"
#include "text_util.h"

namespace pm::translate {

// One-word UI labels the sentence models get wrong without context (オン ->
// 「上」, 켬 -> 「肯」, 취소 -> 「已取消」): exact matches, before the engine.
const wchar_t* glossary(const std::wstring& text, Lang tgt) {
    struct G {
        const wchar_t* src;
        const wchar_t* zh;
        const wchar_t* en;
    };
    static const G kG[] = {
        {L"オン", L"開啟", L"On"},       {L"オフ", L"關閉", L"Off"},       {L"켬", L"開啟", L"On"},
        {L"끔", L"關閉", L"Off"},        {L"On", L"開啟", nullptr},        {L"Off", L"關閉", nullptr},
        {L"ON", L"開啟", nullptr},       {L"OFF", L"關閉", nullptr},       {L"キャンセル", L"取消", L"Cancel"},
        {L"취소", L"取消", L"Cancel"},   {L"Cancel", L"取消", nullptr},    {L"OK", L"確定", nullptr},
        {L"完了", L"完成", L"Done"},     {L"완료", L"完成", L"Done"},      {L"Done", L"完成", nullptr},
        {L"戻る", L"返回", L"Back"},     {L"뒤로", L"返回", L"Back"},      {L"Back", L"返回", nullptr},
        {L"次へ", L"下一步", L"Next"},   {L"다음", L"下一步", L"Next"},    {L"Next", L"下一步", nullptr},
        {L"はい", L"是", L"Yes"},        {L"いいえ", L"否", L"No"},        {L"예", L"是", L"Yes"},
        {L"아니요", L"否", L"No"},       {L"閉じる", L"關閉", L"Close"},   {L"닫기", L"關閉", L"Close"},
        {L"Menu", L"選單", nullptr},     {L"確認", L"確認", L"Confirm"},   {L"확인", L"確認", L"OK"},
        {L"메뉴", L"菜單", L"Menu"},     {L"メニュー", L"菜單", L"Menu"},  {L"お品書き", L"菜單", L"Menu"},
        // Signs: one-word warnings the models read as verbs / names (CAUTION -> 「提高」).
        {L"CAUTION", L"注意", nullptr},  {L"WARNING", L"警告", nullptr},   {L"DANGER", L"危險", nullptr},
        {L"Wet Floor", L"小心地滑", nullptr}, {L"EXIT", L"出口", nullptr},  {L"PUSH", L"推", nullptr},
        {L"PULL", L"拉", nullptr},       {L"注意", L"注意", L"Caution"},   {L"주의", L"注意", L"Caution"},
        // Menus: dish names the models transliterate through English
        // (된장찌개 -> 「美索江」, 味噌ラーメン -> 「米蘇·拉門」).
        {L"ラーメン", L"拉麵", L"Ramen"}, {L"醤油ラーメン", L"醬油拉麵", L"Soy sauce ramen"},
        {L"味噌ラーメン", L"味噌拉麵", L"Miso ramen"}, {L"塩ラーメン", L"鹽味拉麵", L"Salt ramen"},
        {L"とんこつラーメン", L"豚骨拉麵", L"Tonkotsu ramen"}, {L"つけ麺", L"沾麵", L"Tsukemen"},
        {L"うどん", L"烏龍麵", L"Udon"}, {L"そば", L"蕎麥麵", L"Soba"},
        {L"天ぷらそば", L"天婦羅蕎麥麵", L"Tempura soba"}, {L"天ぷら", L"天婦羅", L"Tempura"},
        {L"寿司", L"壽司", L"Sushi"}, {L"刺身", L"生魚片", L"Sashimi"},
        {L"唐揚げ", L"日式炸雞", L"Fried chicken (karaage)"}, {L"唐揚げ定食", L"日式炸雞定食", L"Karaage set meal"},
        {L"餃子", L"煎餃", L"Gyoza"}, {L"焼き鳥", L"烤雞肉串", L"Yakitori"},
        {L"親子丼", L"親子丼（雞肉蛋蓋飯）", L"Chicken and egg rice bowl"}, {L"カツ丼", L"豬排蓋飯", L"Pork cutlet rice bowl"},
        {L"とんかつ", L"炸豬排", L"Pork cutlet"}, {L"カレーライス", L"咖哩飯", L"Curry rice"},
        {L"ライス", L"白飯", L"Rice"}, {L"生ビール", L"生啤酒", L"Draft beer"},
        {L"枝豆", L"毛豆", L"Edamame"}, {L"お好み焼き", L"大阪燒", L"Okonomiyaki"},
        {L"たこ焼き", L"章魚燒", L"Takoyaki"}, {L"味噌汁", L"味噌湯", L"Miso soup"},
        {L"김치찌개", L"泡菜鍋", L"Kimchi stew"}, {L"된장찌개", L"大醬湯", L"Soybean paste stew"},
        {L"순두부찌개", L"嫩豆腐鍋", L"Soft tofu stew"}, {L"부대찌개", L"部隊鍋", L"Army stew"},
        {L"비빔밥", L"拌飯", L"Bibimbap"}, {L"돌솥비빔밥", L"石鍋拌飯", L"Stone pot bibimbap"},
        {L"불고기", L"韓式烤牛肉", L"Bulgogi"}, {L"불고기 정식", L"韓式烤牛肉套餐", L"Bulgogi set meal"},
        {L"제육볶음", L"辣炒豬肉", L"Spicy stir-fried pork"}, {L"삼겹살", L"烤五花肉", L"Pork belly"},
        {L"갈비", L"排骨", L"Galbi (ribs)"}, {L"갈비탕", L"排骨湯", L"Short rib soup"},
        {L"삼계탕", L"人蔘雞湯", L"Ginseng chicken soup"}, {L"냉면", L"冷麵", L"Cold noodles"},
        {L"떡볶이", L"辣炒年糕", L"Tteokbokki"}, {L"김밥", L"韓式海苔飯捲", L"Gimbap"},
        {L"라면", L"泡麵（拉麵）", L"Ramyeon"}, {L"공기밥", L"白飯", L"Bowl of rice"},
        {L"공기밥 추가", L"加點白飯", L"Extra rice"}, {L"치킨", L"炸雞", L"Fried chicken"},
        {L"소주", L"燒酒", L"Soju"}, {L"맥주", L"啤酒", L"Beer"},
        {L"막걸리", L"馬格利米酒", L"Makgeolli"}, {L"잡채", L"韓式炒冬粉", L"Japchae"},
        {L"해물파전", L"海鮮煎餅", L"Seafood pancake"},
    };
    if (tgt != Lang::ZhHant && tgt != Lang::En) return nullptr;  // ja / ko targets: the engine's own words
    std::wstring t;  // 메 뉴 (spaced-out headings)
    for (wchar_t c : text)
        if (!iswspace(c) || !(isCjk(t.empty() ? 0 : t.back()))) t += c;
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    for (const auto& g : kG)
        if (text == g.src || t == g.src) return tgt == Lang::ZhHant ? g.zh : g.en;
    return nullptr;
}

namespace {

bool isHira(wchar_t c) { return c >= 0x3041 && c <= 0x309F; }
bool isKata(wchar_t c) { return (c >= 0x30A1 && c <= 0x30FA) || c == 0x30FC || c == 0x30FB; }
bool isHan(wchar_t c) { return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || c == 0x3005; }
bool isLatinAlnum(wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'); }
bool isSpace(wchar_t c) { return c == L' ' || c == 0x3000 || c == L'\t'; }

// Field labels of food labels / tables (Japanese, Korean): the label is
// shown translated and its value translated on its own, so a table row read as
// one line (「賞味期限 枠外下部に記載」) does not become one wrong sentence.
struct Field {
    Lang src;
    const wchar_t* label;
    const wchar_t* zh;
    const wchar_t* en;
};
const Field kFields[] = {
    {Lang::Ja, L"名称", L"名稱", L"Name"},
    {Lang::Ja, L"品名", L"品名", L"Product"},
    {Lang::Ja, L"商品名", L"商品名稱", L"Product name"},
    {Lang::Ja, L"原材料名", L"原材料", L"Ingredients"},
    {Lang::Ja, L"原材料", L"原材料", L"Ingredients"},
    {Lang::Ja, L"内容量", L"內容量", L"Net contents"},
    {Lang::Ja, L"賞味期限", L"賞味期限", L"Best before"},
    {Lang::Ja, L"消費期限", L"消費期限", L"Use by"},
    {Lang::Ja, L"保存方法", L"保存方法", L"Storage"},
    {Lang::Ja, L"製造者", L"製造商", L"Manufacturer"},
    {Lang::Ja, L"販売者", L"販售商", L"Seller"},
    {Lang::Ja, L"製造所", L"製造廠", L"Factory"},
    {Lang::Ja, L"加工者", L"加工商", L"Processor"},
    {Lang::Ja, L"輸入者", L"進口商", L"Importer"},
    {Lang::Ja, L"原産国名", L"原產國", L"Country of origin"},
    {Lang::Ja, L"原料原産地名", L"原料原產地", L"Origin of ingredients"},
    {Lang::Ja, L"栄養成分表示", L"營養成分標示", L"Nutrition facts"},
    {Lang::Ja, L"熱量", L"熱量", L"Energy"},
    {Lang::Ja, L"エネルギー", L"熱量", L"Energy"},
    {Lang::Ja, L"たんぱく質", L"蛋白質", L"Protein"},
    {Lang::Ja, L"蛋白質", L"蛋白質", L"Protein"},
    {Lang::Ja, L"脂質", L"脂肪", L"Fat"},
    {Lang::Ja, L"炭水化物", L"碳水化合物", L"Carbohydrate"},
    {Lang::Ja, L"糖質", L"糖質", L"Sugars"},
    {Lang::Ja, L"食物繊維", L"膳食纖維", L"Dietary fibre"},
    {Lang::Ja, L"食塩相当量", L"食鹽相當量", L"Salt equivalent"},
    {Lang::Ja, L"アレルギー物質", L"過敏原", L"Allergens"},
    {Lang::Ko, L"제품명", L"品名", L"Product name"},
    {Lang::Ko, L"식품유형", L"食品類型", L"Food type"},
    {Lang::Ko, L"내용량", L"內容量", L"Net contents"},
    {Lang::Ko, L"원재료명", L"原材料", L"Ingredients"},
    {Lang::Ko, L"보관방법", L"保存方法", L"Storage"},
    {Lang::Ko, L"소비기한", L"消費期限", L"Use by"},
    {Lang::Ko, L"유통기한", L"有效期限", L"Best before"},
    {Lang::Ko, L"제조원", L"製造商", L"Manufacturer"},
    {Lang::Ko, L"판매원", L"販售商", L"Seller"},
    {Lang::Ko, L"열량", L"熱量", L"Energy"},
    {Lang::Ko, L"단백질", L"蛋白質", L"Protein"},
    {Lang::Ko, L"탄수화물", L"碳水化合物", L"Carbohydrate"},
    {Lang::Ko, L"나트륨", L"鈉", L"Sodium"},
    {Lang::Ko, L"당류", L"糖", L"Sugars"},
};

// Whole phrases of labels the models get wrong (目安 -> 「開眼期」, 記載 ->
// 「架子」), before the engine; matched without spaces and leading bullets.
struct Phrase {
    const wchar_t* src;
    const wchar_t* zh;
    const wchar_t* en;
};
const Phrase kPhrases[] = {
    {L"この表示値は目安です。", L"此標示值為參考值。", L"These values are estimates."},
    {L"この表示値は目安です", L"此標示值為參考值", L"These values are estimates"},
    {L"枠外下部に記載", L"標示於框外下方", L"See below the frame"},
    {L"枠外上部に記載", L"標示於框外上方", L"See above the frame"},
    {L"枠外右部に記載", L"標示於框外右側", L"See right of the frame"},
    {L"枠外左部に記載", L"標示於框外左側", L"See left of the frame"},
    {L"枠外に記載", L"標示於框外", L"See outside the frame"},
    {L"底面に記載", L"標示於底部", L"See the bottom"},
    {L"裏面に記載", L"標示於背面", L"See the back"},
    {L"側面に記載", L"標示於側面", L"See the side"},
    {L"上部に記載", L"標示於上方", L"See the top"},
    {L"別途記載", L"另行標示", L"Shown separately"},
    {L"包装の裏面に記載", L"標示於包裝背面", L"See the back of the package"},
};

// Source spellings that mislead the models (うすく -> 「烏蘇庫」, read as a name),
// and label words the recogniser often gets one stroke wrong (photos: ハッ橋
// for 八ッ橋, たんばく質, 膨張削, 力力才マス; measured on the owner's label).
const std::pair<const wchar_t*, const wchar_t*> kRespell[] = {
    {L"うすく焼", L"薄く焼"},         {L"うす焼", L"薄焼"},             {L"うすく仕上", L"薄く仕上"},
    {L"かるく", L"軽く"},             {L"ハッ橋", L"八ッ橋"},           {L"ハっ橋", L"八ッ橋"},
    {L"ハツ橋", L"八ツ橋"},           {L"たんばく質", L"たんぱく質"},   {L"ココアバウダー", L"ココアパウダー"},
    {L"力力才マス", L"カカオマス"},   {L"カカ才マス", L"カカオマス"},   {L"力カオマス", L"カカオマス"},
    {L"膨張削", L"膨張剤"},           {L"加工油指", L"加工油脂"},       {L"秒糖", L"砂糖"},
    {L"プルーム現象", L"ブルーム現象"}, {L"プルームを", L"ブルームを"}, {L"暖勇", L"暖房"},
    {L"仕上けまし", L"仕上げまし"},   {L"を含心", L"を含む"},           {L"記载", L"記載"},
    {L"赏味期限", L"賞味期限"},       {L"棒外下部", L"枠外下部"},       {L"棒外上部", L"枠外上部"},
    {L"荣養成分", L"栄養成分"},       {L"米養成分", L"栄養成分"},       {L"荣善成分", L"栄養成分"},
    // Coating written as コーチング (coaching) on labels: the models say 「教練」.
    {L"チョコレートコーチング", L"チョコレートコーティング"},
};

// Names: written the way a reader of the target knows them (kept otherwise).
struct Name {
    const wchar_t* src;
    const wchar_t* zh;
    const wchar_t* en;
};
const Name kNames[] = {
    {L"ゴディバ", L"GODIVA", L"GODIVA"}, {L"コディバ", L"GODIVA", L"GODIVA"}, {L"ゴデイバ", L"GODIVA", L"GODIVA"},
    {L"ジャパン", L"Japan", L"Japan"},   {L"ジャバン", L"Japan", L"Japan"},   {L"八ッ橋", L"八橋", L"Yatsuhashi"},
    {L"八ツ橋", L"八橋", L"Yatsuhashi"}, {L"生八ッ橋", L"生八橋", L"Nama Yatsuhashi"}, {L"株式会社", L"株式會社", L"Co., Ltd."},
    {L"有限会社", L"有限會社", L"Ltd."}, {L"合同会社", L"合同會社", L"LLC"}, {L"(株)", L"(株)", L"Co., Ltd."},
    {L"（株）", L"（株）", L"Co., Ltd."}, {L"㈱", L"(株)", L"Co., Ltd."},
};
const wchar_t* const kCompany[] = {L"株式会社", L"有限会社", L"合同会社", L"（株）", L"(株)", L"㈱"};

std::wstring squeeze(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s)
        if (!isSpace(c)) o += c;
    return o;
}

const Phrase* phrase(const std::wstring& t) {
    const std::wstring q = squeeze(t);
    for (const auto& p : kPhrases)
        if (q == p.src) return &p;
    return nullptr;
}

// Label at s[i..] (its characters may be spaced out: 「内 容 量」, 「名　　称」):
// end index after it, or 0.
size_t matchLabel(const std::wstring& s, size_t i, const wchar_t* label) {
    size_t k = i;
    for (const wchar_t* l = label; *l; ++l) {
        if (k > i)
            while (k < s.size() && isSpace(s[k])) ++k;
        if (k >= s.size() || s[k] != *l) return 0;
        ++k;
    }
    return k;
}

struct Piece {
    const Field* field = nullptr;  // nullptr: text before the first label
    std::wstring value;
};

// Splits a table line at its field labels; empty if there is none.
std::vector<Piece> splitFields(const std::wstring& s, Lang src) {
    std::vector<Piece> out;
    Piece cur;
    size_t i = 0, last = 0;
    bool any = false;
    while (i < s.size()) {
        const bool boundary = i == 0 || isSpace(s[i - 1]) || s[i - 1] == L'|';
        const Field* hit = nullptr;
        size_t end = 0;
        if (boundary) {
            for (const auto& f : kFields) {
                if (f.src != src) continue;
                const size_t e = matchLabel(s, i, f.label);
                if (!e) continue;
                // The label must end the cell: a space, a colon or the end;
                // directly followed by text only at the start of the line and
                // not by a particle (保存方法は… is a sentence).
                const wchar_t nx = e < s.size() ? s[e] : 0;
                const bool sep = !nx || isSpace(nx) || nx == L'：' || nx == L':';
                const bool glued = i == 0 && nx && !isHira(nx) && wcslen(f.label) >= 3 && !(nx >= 0xAC00 && nx <= 0xD7AF);
                if (!sep && !glued) continue;
                if (e > end) end = e, hit = &f;
            }
        }
        if (hit) {
            cur.value = s.substr(last, i - last);
            if (any || !squeeze(cur.value).empty()) out.push_back(cur);
            any = true;
            cur = {};
            cur.field = hit;
            i = end;
            while (i < s.size() && (isSpace(s[i]) || s[i] == L'：' || s[i] == L':')) ++i;
            last = i;
            continue;
        }
        ++i;
    }
    if (!any) return {};
    cur.value = s.substr(last);
    out.push_back(cur);
    for (auto& p : out) {
        while (!p.value.empty() && isSpace(p.value.back())) p.value.pop_back();
        while (!p.value.empty() && isSpace(p.value.front())) p.value.erase(0, 1);
    }
    // A lone label with nothing else is not a table line (the glossary handles it).
    if (out.size() == 1 && out[0].value.empty()) return {};
    return out;
}

// Names to keep through the engine: companies (ゴディバ ジャパン株式会社,
// 株式会社美十), kanji names with a small katakana (八ッ橋), known brands.
struct Entity {
    size_t pos, len;
};
std::vector<Entity> findEntities(const std::wstring& s, Lang src) {
    std::vector<Entity> out;
    if (src != Lang::Ja) return out;
    auto nameChar = [](wchar_t c) { return isKata(c) || isHan(c) || isLatinAlnum(c) || c == L'&' || c == L'＆'; };
    for (const wchar_t* co : kCompany) {
        const size_t cl = wcslen(co);
        for (size_t p = s.find(co); p != std::wstring::npos; p = s.find(co, p + cl)) {
            size_t b = p;  // name before: katakana / kanji / Latin words, spaces between them
            while (b > 0 && (nameChar(s[b - 1]) || (isSpace(s[b - 1]) && b >= 2 && nameChar(s[b - 2]) && b < p))) --b;
            while (b < p && isSpace(s[b])) ++b;
            size_t e = p + cl;  // name after (no spaces)
            if (b == p)
                while (e < s.size() && nameChar(s[e])) ++e;
            if (b == p && e == p + cl) continue;  // 株式会社 alone
            out.push_back({b, e - b});
        }
    }
    for (const auto& n : kNames) {
        const std::wstring ns = n.src;
        if (ns.find(L"会社") != std::wstring::npos || ns[0] == L'(' || ns[0] == L'（' || ns[0] == L'㈱') continue;
        for (size_t p = s.find(ns); p != std::wstring::npos; p = s.find(ns, p + ns.size())) out.push_back({p, ns.size()});
    }
    // 八ッ橋-like: kanji + ッ / ヶ + kanji.
    for (size_t i = 1; i + 1 < s.size(); ++i)
        if ((s[i] == L'ッ' || s[i] == L'ヶ') && isHan(s[i - 1]) && isHan(s[i + 1])) {
            size_t b = i - 1, e = i + 2;
            while (b > 0 && isHan(s[b - 1])) --b;
            while (e < s.size() && isHan(s[e])) ++e;
            out.push_back({b, e - b});
        }
    // Overlaps: the earliest, longest one wins.
    std::sort(out.begin(), out.end(), [](const Entity& a, const Entity& b) { return a.pos != b.pos ? a.pos < b.pos : a.len > b.len; });
    std::vector<Entity> keep;
    for (const auto& e : out)
        if (keep.empty() || e.pos >= keep.back().pos + keep.back().len) keep.push_back(e);
    return keep;
}

// An entity as the target's reader knows it: known parts mapped (ゴディバ ->
// GODIVA, 株式会社 -> 株式會社), the rest kept (Chinese).  complete = false:
// English with an unknown (kana / kanji) part, left to the engine.
std::wstring renderEntity(const std::wstring& e, Lang tgt, bool* complete) {
    std::wstring o;
    *complete = true;
    for (size_t i = 0; i < e.size();) {
        const Name* best = nullptr;
        size_t bl = 0;
        for (const auto& n : kNames) {
            const size_t nl = wcslen(n.src);
            if (nl > bl && e.compare(i, nl, n.src) == 0) best = &n, bl = nl;
        }
        if (best) {
            const wchar_t* r = tgt == Lang::ZhHant ? best->zh : tgt == Lang::En ? best->en : best->src;
            // Two Latin names side by side (GODIVA Japan): a space between them.
            if (!o.empty() && o.back() != L' ' && r[0] != L',' && (tgt == Lang::En || (isLatinAlnum(o.back()) && isLatinAlnum(r[0]))))
                o += L' ';
            o += r;
            i += bl;
            continue;
        }
        if (tgt == Lang::En && !isSpace(e[i]) && !isLatinAlnum(e[i])) *complete = false;
        o += e[i++];
    }
    if (tgt == Lang::ZhHant) o = toTraditional(o);
    return o;
}

const wchar_t* fieldText(const Field& f, Lang tgt) { return tgt == Lang::ZhHant ? f.zh : f.en; }

// A field value worth the engine (Chinese target: kanji / numbers / units are
// readable as they are: 焼菓子, 8袋（16枚）, 42kcal).
bool needsEngine(const std::wstring& v, Lang tgt) {
    if (tgt != Lang::ZhHant) return true;
    const ScriptCount n = countScripts(v);
    return n.kana + n.hangul > 0;
}

}  // namespace

bool translateTexts(Engine& engine, Lang src, Lang tgt, const std::vector<std::wstring>& in, std::vector<std::wstring>& out,
                    std::wstring* err) {
    out.assign(in.size(), {});
    const bool fixes = tgt == Lang::ZhHant || tgt == Lang::En;
    // Pieces sent to the engine (deduplicated); each input is assembled from
    // literal text and engine results.
    std::vector<std::wstring> engineIn;
    auto ask = [&](const std::wstring& t) {
        for (size_t i = 0; i < engineIn.size(); ++i)
            if (engineIn[i] == t) return i;
        engineIn.push_back(t);
        return engineIn.size() - 1;
    };
    struct Part {
        std::wstring lit;       // literal text (label, kept value, bullet, newline)
        size_t eng = SIZE_MAX;  // or an engine result
        std::vector<std::pair<std::wstring, std::wstring>> ph;  // placeholder -> name, in that result
        std::wstring plain;     // the same piece without placeholders (translated only if one got lost)
    };
    std::vector<std::vector<Part>> plan(in.size());
    // Placeholders the models copy unchanged (measured: ZQA / ZQB survive
    // ja -> en -> zh-Hant; X1 or [1] change the sentence around them).
    static const wchar_t* const kPh[] = {L"ZQA", L"ZQB", L"ZQC", L"ZQD", L"ZQE", L"ZQF"};
    // One value / sentence: glossary, phrase, or the engine with names protected.
    auto addText = [&](std::vector<Part>& parts, std::wstring t) {
        // Leading bullets / marks stay as they are (※, ●, ・).
        std::wstring lead;
        while (!t.empty() && (wcschr(L"※●・◆■□★☆*＊○◎", t[0]) || isSpace(t[0]))) {
            if (!isSpace(t[0])) lead += t[0];
            t.erase(0, 1);
        }
        if (t.empty()) return;
        if (!lead.empty()) parts.push_back({lead + (tgt == Lang::En ? L" " : L"")});
        if (const wchar_t* g = glossary(t, tgt)) {
            parts.push_back({g});
            return;
        }
        if (fixes) {
            if (const Phrase* p = phrase(t)) {
                parts.push_back({tgt == Lang::ZhHant ? p->zh : p->en});
                return;
            }
            for (const auto& f : kFields)
                if (f.src == src && squeeze(t) == f.label) {
                    parts.push_back({fieldText(f, tgt)});
                    return;
                }
        }
        if (src == Lang::Ja)
            for (const auto& [a, b] : kRespell)
                for (size_t p = t.find(a); p != std::wstring::npos; p = t.find(a, p + 1)) t.replace(p, wcslen(a), b);
        Part part;
        std::wstring prot = t;
        if (fixes) {
            const auto ents = findEntities(t, src);
            if (!ents.empty() && ents.size() <= std::size(kPh)) {
                prot.clear();
                size_t at = 0, k = 0;
                for (const auto& e : ents) {
                    bool complete = true;
                    std::wstring r = renderEntity(t.substr(e.pos, e.len), tgt, &complete);
                    if (!complete) continue;  // English: an unknown name goes through the engine
                    prot += t.substr(at, e.pos - at);
                    prot += kPh[k];
                    part.ph.push_back({kPh[k], r});
                    ++k;
                    at = e.pos + e.len;
                }
                prot += t.substr(at);
                if (part.ph.size() == 1 && squeeze(prot) == part.ph[0].first) {  // nothing but the name
                    parts.push_back({part.ph[0].second});
                    return;
                }
            }
        }
        part.eng = ask(prot);
        if (!part.ph.empty()) part.plain = t;
        parts.push_back(std::move(part));
    };
    for (size_t i = 0; i < in.size(); ++i) {
        auto& parts = plan[i];
        std::wstring line = in[i];
        if (src == Lang::Ja)  // misread label words first: 赏味期限 must still be a field label
            for (const auto& [x, y] : kRespell)
                for (size_t p = line.find(x); p != std::wstring::npos; p = line.find(x, p + 1)) line.replace(p, wcslen(x), y);
        const auto fields = fixes ? splitFields(line, src) : std::vector<Piece>{};
        if (fields.empty()) {
            addText(parts, line);
            continue;
        }
        bool first = true;
        for (const auto& f : fields) {
            if (!first) parts.push_back({L"\n"});
            first = false;
            if (!f.field) {
                addText(parts, f.value);
                continue;
            }
            parts.push_back({std::wstring(fieldText(*f.field, tgt)) + (f.value.empty() ? L"" : tgt == Lang::En ? L": " : L"：")});
            if (!needsEngine(f.value, tgt)) parts.push_back({tgt == Lang::ZhHant ? toTraditional(f.value) : f.value});
            else addText(parts, f.value);
        }
    }
    std::vector<std::wstring> res;
    if (!engineIn.empty() && !engine.translate(src, tgt, engineIn, res, err)) return false;
    res.resize(engineIn.size());
    // A placeholder lost or doubled by the engine: that piece again without names kept.
    std::vector<std::wstring> retryIn, retryOut;
    std::vector<std::pair<size_t, size_t>> retryAt;
    for (size_t i = 0; i < in.size(); ++i)
        for (size_t k = 0; k < plan[i].size(); ++k) {
            const Part& p = plan[i][k];
            if (p.eng == SIZE_MAX || p.ph.empty()) continue;
            bool ok = true;
            for (const auto& ph : p.ph) {
                const size_t at = res[p.eng].find(ph.first);
                ok = ok && at != std::wstring::npos && res[p.eng].find(ph.first, at + 1) == std::wstring::npos;
            }
            if (!ok) retryIn.push_back(p.plain), retryAt.push_back({i, k});
        }
    if (!retryIn.empty() && !engine.translate(src, tgt, retryIn, retryOut, err)) return false;
    for (size_t j = 0; j < retryAt.size() && j < retryOut.size(); ++j) {
        Part& p = plan[retryAt[j].first][retryAt[j].second];
        p.eng = SIZE_MAX;
        p.lit = retryOut[j];
    }
    for (size_t i = 0; i < in.size(); ++i) {
        std::wstring o;
        for (const auto& p : plan[i]) {
            if (p.eng == SIZE_MAX) {
                o += p.lit;
                continue;
            }
            std::wstring r = res[p.eng];
            while (!r.empty() && isSpace(r.back())) r.pop_back();
            bool ok = true;
            for (const auto& [ph, ent] : p.ph) {
                const size_t at = r.find(ph);
                if (at == std::wstring::npos || r.find(ph, at + 1) != std::wstring::npos) {
                    ok = false;
                    break;
                }
                size_t b = at, e = at + ph.size();
                // No spaces between a Chinese name and Chinese text.
                if (tgt != Lang::En) {
                    if (b > 0 && r[b - 1] == L' ' && isCjk(ent.front())) --b;
                    if (e < r.size() && r[e] == L' ' && isCjk(ent.back())) ++e;
                }
                r.replace(b, e - b, ent);
            }
            if (!ok) r.clear();
            o += r;
        }
        while (!o.empty() && (isSpace(o.back()) || o.back() == L'\n')) o.pop_back();
        out[i] = std::move(o);
    }
    return true;
}

}  // namespace pm::translate
