// Rules that run before / instead of the engine (P0 of
// launch/_work/tr_arch/ARCHITECTURE.md §3.6): Japanese kanji shown in
// Traditional Chinese (ConvertScript), label terms, high-risk sentence
// templates (保存方法 / 注意 / アレルギー: the ones whose negation the
// ja -> en -> zh-Hant pivot loses), addresses kept.
#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

#include "pm/translate.h"
#include "text_util.h"

namespace pm::translate {

namespace {

// Japanese shinjitai -> Traditional forms (the ones LCMapStringEx leaves:
// it maps Simplified Chinese, not Japanese).  Ambiguous ones (弁 予 余 台 欠
// 芸) are left out; words with them are in kKanjiWords.
const wchar_t kShin[] =
    L"亜亞悪惡圧壓囲圍医醫隠隱栄榮営營駅驛塩鹽縁緣応應欧歐桜櫻奥奧横橫温溫穏穩仮假価價画畫会會壊壞懐懷絵繪拡擴"
    L"覚覺学學楽樂渇渴巻卷陥陷寛寬歓歡缶罐観觀関關気氣帰歸亀龜戯戲犠犧旧舊拠據挙舉峡峽挟挾狭狹暁曉区區駆驅径徑茎莖恵惠"
    L"掲揭渓溪経經継繼軽輕鶏雞撃擊剣劍険險圏圈検檢権權献獻県縣験驗厳嚴広廣効效鉱礦号號国國黒黑砕碎済濟斎齋剤劑雑雜参參"
    L"惨慘桟棧蚕蠶賛贊残殘糸絲歯齒児兒辞辭湿濕実實写寫釈釋寿壽収收従從渋澀獣獸縦縱粛肅処處叙敘奨獎将將焼燒称稱証證乗乘"
    L"浄淨剰剩畳疊縄繩壌壤嬢孃譲讓醸釀触觸嘱囑寝寢尽盡図圖粋粹酔醉随隨髄髓数數枢樞瀬瀨声聲斉齊静靜窃竊摂攝専專浅淺戦戰"
    L"践踐銭錢潜潛繊纖禅禪双雙壮壯争爭荘莊捜搜挿插巣巢装裝層層総總騒騷増增蔵藏臓臟属屬続續堕墮体體対對帯帶滞滯滝瀧択擇"
    L"沢澤担擔単單胆膽団團断斷弾彈遅遲虫蟲昼晝鋳鑄庁廳徴徵聴聽鉄鐵転轉点點伝傳灯燈当當党黨盗盜稲稻闘鬥徳德独獨読讀届屆"
    L"弐貳悩惱脳腦廃廢拝拜売賣麦麥発發髪髮抜拔蛮蠻浜濱払拂仏佛辺邊変變歩步宝寶豊豐没沒毎每万萬満滿黙默弥彌訳譯薬藥誉譽"
    L"揺搖様樣謡謠来來頼賴乱亂覧覽竜龍両兩猟獵緑綠塁壘涙淚励勵礼禮隷隸霊靈齢齡恋戀炉爐労勞楼樓録錄湾灣内內戸戶黄黃歳歲"
    L"姫姬郷鄉戻戾脱脫説說鋭銳税稅閲閱悦悅呉吳娯娛晩晚錬鍊緒緒巣巢潟潟賎賤銭錢斎齋";

// Japanese kanji words read differently in Chinese, and label words with a
// Taiwanese term (applied before the character mapping, longest first).
struct KW {
    const wchar_t* ja;
    const wchar_t* zh;
};
const KW kKanjiWords[] = {
    {L"関係者以外立入禁止", L"非相關人員禁止進入"}, {L"女人禁制", L"禁止女性進入"}, {L"出入禁止", L"禁止出入"},
    {L"銃猟禁止区域", L"禁止持槍狩獵區域"}, {L"一品料理", L"單點料理"}, {L"別紙記載", L"詳見另附說明"},
    {L"野菜果実", L"蔬菜、水果"}, {L"野菜", L"蔬菜"}, {L"果実", L"水果"}, {L"玉葱", L"洋蔥"}, {L"立入禁止", L"禁止進入"}, {L"土足厳禁", L"禁止穿鞋進入"},
    {L"土足禁止", L"禁止穿鞋進入"}, {L"撮影禁止", L"禁止攝影"}, {L"駐車禁止", L"禁止停車"}, {L"禁煙", L"禁菸"},
    {L"喫煙所", L"吸菸區"}, {L"焼菓子", L"烘焙點心"}, {L"冷奴", L"涼拌豆腐"}, {L"定休日", L"公休日"},
    {L"本日休業", L"本日公休"}, {L"休業日", L"公休日"}, {L"営業時間", L"營業時間"}, {L"受付時間", L"服務時間"},
    {L"取扱説明書", L"使用說明書"}, {L"取扱注意", L"小心輕放"}, {L"定格", L"額定"}, {L"使用温度", L"使用溫度"},
    {L"要冷蔵", L"需冷藏"}, {L"要冷凍", L"需冷凍"}, {L"税込", L"含稅"}, {L"税抜", L"未稅"}, {L"無料", L"免費"},
    {L"有料", L"收費"}, {L"予約", L"預約"}, {L"予定", L"預定"}, {L"弁当", L"便當"}, {L"化粧室", L"化妝室"},
    {L"改札口", L"剪票口"}, {L"乗車券", L"車票"}, {L"切符", L"車票"}, {L"両替", L"換錢"}, {L"非常口", L"緊急出口"},
    {L"脂質", L"脂肪"}, {L"炭水化物", L"碳水化合物"}, {L"食塩相当量", L"食鹽相當量"}, {L"食物繊維", L"膳食纖維"},
    {L"原材料名", L"原材料"}, {L"内容量", L"內容量"}, {L"製造所", L"製造廠"}, {L"製造者", L"製造商"},
    {L"販売者", L"販售商"}, {L"輸入者", L"進口商"}, {L"原産国名", L"原產國"}, {L"栄養成分表示", L"營養成分標示"},
    {L"膨張剤", L"膨脹劑"}, {L"着色料", L"色素"}, {L"小麦粉", L"小麥粉"}, {L"即席", L"即食"}, {L"日本酒", L"日本酒"},
    {L"株式会社", L"株式會社"}, {L"有限会社", L"有限會社"},
    {L"月曜日", L"星期一"}, {L"火曜日", L"星期二"}, {L"水曜日", L"星期三"}, {L"木曜日", L"星期四"}, {L"金曜日", L"星期五"},
    {L"土曜日", L"星期六"}, {L"日曜日", L"星期日"}, {L"土日祝", L"週六、日及國定假日"}, {L"祝日", L"國定假日"},
    {L"祝祭日", L"國定假日"}, {L"年中無休", L"全年無休"}, {L"毎日", L"每天"},
    // App / settings words read differently in Chinese (eval: 言語 日本語 破棄).
    {L"日本語", L"日文"}, {L"中国語", L"中文"}, {L"韓国語", L"韓文"}, {L"英語", L"英文"}, {L"言語", L"語言"},
    {L"破棄", L"捨棄"}, {L"削除", L"刪除"}, {L"編集", L"編輯"}, {L"検索", L"搜尋"}, {L"送信", L"傳送"}, {L"受信", L"接收"},
    {L"履歴", L"紀錄"}, {L"非表示", L"隱藏"}, {L"新規", L"新增"}, {L"完了", L"完成"}, {L"再試行", L"重試"},
    {L"有効期限", L"有效期限"}, {L"有効期間", L"有效期間"}, {L"接続", L"連線"}, {L"有効", L"啟用"}, {L"無効", L"停用"}, {L"動画", L"影片"}, {L"写真", L"照片"},
    {L"画面", L"畫面"}, {L"表示", L"顯示"},
};

}  // namespace

int chineseSignals(const std::wstring& s) {
    // Traditional forms Japanese writes in shinjitai (會 說 譯 點 灣 繪 …): the
    // right-hand side of kShin where it differs from the Japanese form.
    static const std::wstring trad = [] {
        std::wstring t;
        for (size_t i = 0; kShin[i] && kShin[i + 1]; i += 2)
            if (kShin[i] != kShin[i + 1]) t += kShin[i + 1];
        return t;
    }();
    // Chinese function words and characters Japanese hardly uses.
    static const std::wstring words = L"這們嗎呢吧麼沒還讓給從誰她您啊喔哦欸囉哪噢啦嘛呀咱倆甭甚臺貼隨";
    // A Traditional-only form counts 2 (Chinese for sure); function words 1
    // each, so one 的 / 了 (目的地, 終了 are Japanese) is not enough alone.
    int n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (trad.find(c) != std::wstring::npos) n += 2;
        else if (words.find(c) != std::wstring::npos) n += 1;
        else if ((c == L'的' || c == L'了' || c == L'是') && !(i > 0 && wcschr(L"目終完修満魅投校具標", s[i - 1])) &&
                 !(c == L'是' && i + 1 < s.size() && s[i + 1] == L'非'))
            n += 1;
    }
    for (const wchar_t* w : {L"一定", L"一下", L"可以", L"已經", L"就是", L"不是", L"還是", L"小時", L"分鐘", L"我們", L"你們", L"他們",
                             L"怎麼", L"什麼", L"這個", L"那個", L"旅遊", L"大家", L"網友"})
        for (size_t p = s.find(w); p != std::wstring::npos; p = s.find(w, p + 1)) ++n;
    return n >= 2 ? n : 0;
}

int japaneseOnlyKanji(const std::wstring& s) {
    int n = 0;
    for (wchar_t c : s)
        for (size_t i = 0; kShin[i] && kShin[i + 1]; i += 2)
            if (kShin[i] == c) {
                // 区 is Simplified Chinese too (區): not Japanese-only.
                if (toTraditional(std::wstring(1, c)) == std::wstring(1, c)) ++n;
                break;
            }
    // Japanese sign words in characters Chinese shares (Chinese says 禁止… first).
    for (const wchar_t* w : {L"女人禁制", L"立入禁止", L"撮影禁止", L"駐車禁止", L"出入禁止", L"土足禁止", L"一品料理"})
        if (s.find(w) != std::wstring::npos) ++n;
    return n;
}

std::wstring convertJapaneseKanji(const std::wstring& s) {
    std::wstring t = s;
    for (const auto& w : kKanjiWords)
        for (size_t p = t.find(w.ja); p != std::wstring::npos; p = t.find(w.ja, p + wcslen(w.zh))) t.replace(p, wcslen(w.ja), w.zh);
    for (wchar_t& c : t)
        for (size_t i = 0; kShin[i] && kShin[i + 1]; i += 2)
            if (kShin[i] == c) {
                c = kShin[i + 1];
                break;
            }
    return toTraditional(t);
}

// A postal address (〒, a 123-4567 code, or a prefecture + city / ward with a
// house number): kept as written (Traditional characters), never through
// the engine (京都市南区西九条 -> 「西庫霍，米奈庫」).
bool looksLikeAddress(const std::wstring& t) {
    if (t.find(L'〒') != std::wstring::npos) return true;
    static const std::wregex code(L"(^|[^0-9])[0-9]{3}-[0-9]{4}([^0-9]|$)");
    static const std::wregex jp(L"(都|道|府|県)[^、。]{1,8}(市|区|郡|町|村)[^、。]*[0-9０-９]");
    static const std::wregex kr(L"(시|도) [^ ]+(구|군) [^ ]+(로|길|동)");
    bool han = false;  // 601-8446 is a Japanese postal code only next to kanji (not 1-800-555-0142)
    for (wchar_t c : t) han |= c >= 0x4E00 && c <= 0x9FFF;
    return (han && std::regex_search(t, code)) || std::regex_search(t, jp) || std::regex_search(t, kr);
}

namespace {

// Label terms for the templates' slots (only exact items: a slot with an
// unknown item leaves the sentence to the engine).
struct Term {
    Lang src;
    const wchar_t* s;
    const wchar_t* zh;
    const wchar_t* en;
};
const Term kTerms[] = {
    {Lang::Ja, L"直射日光", L"陽光直射", L"direct sunlight"}, {Lang::Ja, L"日光", L"陽光", L"sunlight"},
    {Lang::Ja, L"高温多湿", L"高溫潮濕", L"heat and humidity"}, {Lang::Ja, L"高温", L"高溫", L"high temperatures"},
    {Lang::Ja, L"多湿", L"潮濕", L"humidity"}, {Lang::Ja, L"湿気", L"濕氣", L"moisture"}, {Lang::Ja, L"火気", L"火源", L"open flames"},
    {Lang::Ja, L"冷暗所", L"陰涼處", L"a cool, dark place"}, {Lang::Ja, L"涼しい所", L"陰涼處", L"a cool place"},
    {Lang::Ja, L"涼しい場所", L"陰涼處", L"a cool place"}, {Lang::Ja, L"常温", L"常溫", L"room temperature"},
    {Lang::Ja, L"冷蔵庫", L"冰箱", L"the refrigerator"}, {Lang::Ja, L"冷凍庫", L"冷凍庫", L"the freezer"},
    {Lang::Ja, L"子供", L"兒童", L"children"}, {Lang::Ja, L"子ども", L"兒童", L"children"}, {Lang::Ja, L"お子様", L"兒童", L"children"},
    {Lang::Ja, L"小さなお子様", L"幼童", L"small children"}, {Lang::Ja, L"乳幼児", L"嬰幼兒", L"infants"},
    {Lang::Ja, L"卵", L"蛋", L"egg"}, {Lang::Ja, L"乳", L"乳", L"milk"}, {Lang::Ja, L"乳成分", L"乳成分", L"milk"},
    {Lang::Ja, L"小麦", L"小麥", L"wheat"}, {Lang::Ja, L"えび", L"蝦", L"shrimp"}, {Lang::Ja, L"かに", L"蟹", L"crab"},
    {Lang::Ja, L"そば", L"蕎麥", L"buckwheat"}, {Lang::Ja, L"落花生", L"花生", L"peanuts"}, {Lang::Ja, L"くるみ", L"核桃", L"walnuts"},
    {Lang::Ja, L"大豆", L"大豆", L"soy"}, {Lang::Ja, L"ごま", L"芝麻", L"sesame"},
    {Lang::Ko, L"직사광선", L"陽光直射", L"direct sunlight"}, {Lang::Ko, L"고온다습한 곳", L"高溫潮濕處", L"hot, humid places"},
    {Lang::Ko, L"서늘한 곳", L"陰涼處", L"a cool place"}, {Lang::Ko, L"서늘하고 건조한 곳", L"陰涼乾燥處", L"a cool, dry place"},
    {Lang::Ko, L"건조한 곳", L"乾燥處", L"a dry place"}, {Lang::Ko, L"실온", L"室溫", L"room temperature"},
    {Lang::Ko, L"냉장", L"冷藏", L"the refrigerator"}, {Lang::Ko, L"어린이", L"兒童", L"children"}, {Lang::Ko, L"유아", L"幼兒", L"infants"},
    {Lang::Ko, L"땅콩", L"花生", L"peanuts"}, {Lang::Ko, L"대두", L"大豆", L"soy"}, {Lang::Ko, L"우유", L"牛奶", L"milk"},
    {Lang::Ko, L"밀", L"小麥", L"wheat"}, {Lang::Ko, L"난류", L"蛋", L"eggs"}, {Lang::Ko, L"계란", L"雞蛋", L"eggs"},
    {Lang::Ko, L"새우", L"蝦", L"shrimp"}, {Lang::Ko, L"게", L"蟹", L"crab"}, {Lang::Ko, L"메밀", L"蕎麥", L"buckwheat"},
    {Lang::Ko, L"돼지고기", L"豬肉", L"pork"}, {Lang::Ko, L"쇠고기", L"牛肉", L"beef"}, {Lang::Ko, L"닭고기", L"雞肉", L"chicken"},
    {Lang::Ko, L"토마토", L"番茄", L"tomato"}, {Lang::Ko, L"복숭아", L"桃子", L"peach"}, {Lang::Ko, L"고등어", L"鯖魚", L"mackerel"},
    {Lang::Ko, L"호두", L"核桃", L"walnuts"}, {Lang::Ko, L"잣", L"松子", L"pine nuts"},
    {Lang::Ko, L"찌개류", L"鍋類", L"stews"}, {Lang::Ko, L"찌개", L"鍋類", L"stews"}, {Lang::Ko, L"전골", L"火鍋", L"hot pots"},
    {Lang::Ko, L"고기류", L"肉類", L"grilled meat"}, {Lang::Ko, L"구이류", L"燒烤類", L"grilled dishes"},
    {Lang::Ko, L"탕류", L"湯類", L"soups"},
};

const Term* term(const std::wstring& s, Lang src) {
    std::wstring t = s;
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    while (!t.empty() && iswspace(t.front())) t.erase(0, 1);
    for (const auto& x : kTerms)
        if (x.src == src && t == x.s) return &x;
    return nullptr;
}

// A list slot (直射日光、高温多湿 / 땅콩, 대두): every item a known term.
bool listSlot(const std::wstring& s, Lang src, Lang tgt, std::wstring& out) {
    std::vector<std::wstring> items;
    std::wstring cur;
    auto flush = [&] {
        while (!cur.empty() && iswspace(cur.back())) cur.pop_back();
        while (!cur.empty() && iswspace(cur.front())) cur.erase(0, 1);
        if (!cur.empty()) items.push_back(cur);
        cur.clear();
    };
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (c == L'、' || c == L'・' || c == L',' || c == L'，' || c == L'や' || c == L'/') flush();
        else if (s.compare(i, 3, L"および") == 0) flush(), i += 2;
        else if (s.compare(i, 2, L"及び") == 0) flush(), i += 1;
        else cur += c;
    }
    flush();
    if (items.empty()) return false;
    out.clear();
    for (size_t k = 0; k < items.size(); ++k) {
        // 高温多湿な場所 / 高温多湿の所: the place word goes, the term stays.
        std::wstring it = items[k];
        for (const wchar_t* tail : {L"な場所", L"の場所", L"な所", L"の所", L"なところ", L"場所", L"한 곳"})
            if (it.size() > wcslen(tail) && it.compare(it.size() - wcslen(tail), std::wstring::npos, tail) == 0 &&
                !term(it, src))
                it.erase(it.size() - wcslen(tail));
        const Term* t = term(it, src);
        const bool number = !it.empty() && std::all_of(it.begin(), it.end(), [](wchar_t c) { return iswdigit(c) != 0; });
        if (!t && !number) return false;
        if (k) out += tgt == Lang::En ? (k + 1 == items.size() ? L" and " : L", ") : L"、";
        if (number) {  // a number slot (1인분): as written
            out += it;
            continue;
        }
        out += tgt == Lang::En ? t->en : t->zh;
    }
    return true;
}

struct Template {
    Lang src;
    const wchar_t* re;  // whole text (spaces squeezed for Japanese)
    const wchar_t* zh;  // {1} {2}: slots (lists of terms)
    const wchar_t* en;
};
// High-frequency, high-risk label / notice sentences (≤ 30).  A template
// applies only when every slot is a known term.
const Template kTemplates[] = {
    {Lang::Ja, L"(?:保存方法[:：]?)?(.+?)(?:な|の)?(?:場所|所|ところ)?を避け(?:て|、)、?(常温|冷暗所|涼しい所|涼しい場所|冷蔵庫)(?:で|に)保存(?:して(?:ください|下さい))?。?",
     L"請避免{1}，於{2}保存。", L"Store in {2}, away from {1}."},
    {Lang::Ja, L"(?:保存方法[:：]?)?(.+?)(?:な|の)?(?:場所|所|ところ)?を避け(?:て|、)、?保存(?:して(?:ください|下さい))?。?",
     L"保存時請避免{1}。", L"Store away from {1}."},
    {Lang::Ja, L"(.+?)(?:な|の)?(?:場所|所|ところ)?を避けて(?:ください|下さい)。?", L"請避免{1}。", L"Avoid {1}."},
    {Lang::Ja, L"(.+?)(?:な|の)?(?:場所|所|ところ)?を避けて(?:お使い|ご使用|使用して)ください。?", L"請避免在{1}使用。",
     L"Do not use in {1}."},
    {Lang::Ja, L"(常温|冷暗所|涼しい所|涼しい場所|冷蔵庫)(?:で|に)保存(?:して(?:ください|下さい))?。?", L"請於{1}保存。",
     L"Store in {1}."},
    {Lang::Ja, L"開封後は(?:賞味期限にかかわらず)?(?:お)?早めに(?:お召し上がり|召し上がって)(?:ください|下さい)。?",
     L"開封後請儘早食用。", L"Once opened, eat as soon as possible."},
    {Lang::Ja, L"開封後は(?:お)?早めに(?:ご使用|お使い)(?:ください|下さい)。?", L"開封後請儘早使用。",
     L"Once opened, use as soon as possible."},
    {Lang::Ja, L"(?:※)?本(?:品|製品)(?:の)?製造工場では(.+?)を(?:含む|使用した)製品を(?:生産|製造)しています。?",
     L"※本產品的製造工廠也生產含有{1}的產品。", L"Made in a factory that also produces products containing {1}."},
    {Lang::Ja, L"(.+?)の手の(?:届かない|とどかない)(?:所|ところ|場所)に(?:保管|保存|置いて)(?:して)?(?:ください|下さい)。?",
     L"請存放於{1}拿不到的地方。", L"Keep out of the reach of {1}."},
    {Lang::Ja, L"賞味期限は未開封(?:状態|の状態)の期限です。?", L"賞味期限是指未開封狀態下的期限。",
     L"The best-before date applies to unopened packages."},
    {Lang::Ja, L"火気(?:のそば|の近く)?(?:で|では)?(?:使用しないで(?:ください|下さい)|厳禁)。?", L"請勿在火源附近使用。",
     L"Do not use near open flames."},
    {Lang::Ko, L"(.+?)(?:을|를|은|는)? ?피하(?:고|여|시고),? ?(서늘한 곳|서늘하고 건조한 곳|건조한 곳|실온)(?:에|에서) ?보관(?:하십시오|하세요|해 ?주세요|해 ?주십시오)\\.?",
     L"請避免{1}，存放於{2}。", L"Store in {2}, away from {1}."},
    {Lang::Ko, L"(.+?)(?:을|를)? ?피하여 보관(?:하십시오|하세요|해 ?주세요)\\.?", L"保存時請避免{1}。", L"Store away from {1}."},
    {Lang::Ko, L"(.+?)의 손이 닿지 않는 곳에 보관(?:하십시오|하세요|해 ?주세요)\\.?", L"請存放於{1}拿不到的地方。",
     L"Keep out of the reach of {1}."},
    {Lang::Ko, L"이 제품은 (.+?)(?:을|를)? 사용한 제품과 같은 제조 ?시설에서 제조(?:하고 있습니다|되었습니다|합니다)\\.?",
     L"本產品與使用{1}的產品在同一製造設施生產。", L"Made in the same facility as products that use {1}."},
    {Lang::Ko, L"(.+?)(?:은|는)? ?([0-9]+) ?인분 (?:주문|판매) ?불가(?:합니다)?\\.?", L"{1}不接受{2}人份點餐。",
     L"{1}: not served for {2} person."},
    {Lang::Ko, L"(.+?)(?:은|는)? ?([0-9]+) ?인분 이상 주문 ?가능(?:합니다)?\\.?", L"{1}需點{2}人份以上。",
     L"{1}: minimum order {2} servings."},
    {Lang::Ko, L"개봉 후(?:에는)? (?:반드시 )?냉장 보관(?:하십시오|하세요|해 ?주세요)\\.?", L"開封後請冷藏保存。",
     L"Refrigerate after opening."},
    // Signs (no slots).
    {Lang::Ja, L"(?:この先)?関係者以外立入禁止", L"非相關人員禁止進入", L"Authorized personnel only"},
    {Lang::Ja, L"駆け込み乗車は(?:おやめください|ご遠慮ください|危険です)。?", L"請勿強行衝上車。", L"Do not rush onto the train."},};

}  // namespace

// "Per serving" headers of nutrition tables, numbers kept: 1袋（2枚）あたり ->
// 每1袋（2片）, 1食(78g)当たり -> 每1份（78g）, 1회 제공량 30g -> 每次份量 30g.
bool quantityPhrase(const std::wstring& text, Lang src, Lang tgt, std::wstring& out) {
    if (tgt != Lang::ZhHant && tgt != Lang::En) return false;
    std::wstring t;
    for (wchar_t c : text)
        if (!iswspace(c)) t += (c >= 0xFF10 && c <= 0xFF19) ? static_cast<wchar_t>(c - 0xFF10 + L'0') : c;
    if (t.size() > 2 && (t.front() == L'（' || t.front() == L'(') && (t.back() == L'）' || t.back() == L')'))
        t = t.substr(1, t.size() - 2);  // （1食（78g）当たり）
    if (src == Lang::Ja) {
        static const std::wregex re(L"([0-9.]+)(袋|個|枚|本|食|杯|包|箱|切れ|粒|カップ|人前)(?:[（(]([0-9.]+)(g|ml|mL|枚|個|本|粒)[）)])?(?:あたり|当たり|当り)");
        std::wsmatch m;
        if (!std::regex_match(t, m, re)) return false;
        auto unit = [&](const std::wstring& u) -> std::wstring {
            if (tgt == Lang::En) {
                if (u == L"食" || u == L"人前") return L" serving";
                if (u == L"袋") return L" bag";
                if (u == L"枚") return L" piece(s)";
                if (u == L"個" || u == L"本" || u == L"粒" || u == L"切れ") return L" piece(s)";
                if (u == L"杯" || u == L"カップ") return L" cup";
                return L" " + u;
            }
            if (u == L"食" || u == L"人前") return L"份";
            if (u == L"枚" || u == L"切れ") return L"片";
            if (u == L"本") return L"支";
            if (u == L"粒") return L"粒";
            if (u == L"カップ") return L"杯";
            return u;
        };
        std::wstring inner;
        if (m[3].matched) inner = (tgt == Lang::En ? L" (" : L"（") + m[3].str() + unit(m[4].str()) + (tgt == Lang::En ? L")" : L"）");
        if (m[4].str() == L"g" || m[4].str() == L"ml" || m[4].str() == L"mL")
            inner = (tgt == Lang::En ? L" (" : L"（") + m[3].str() + m[4].str() + (tgt == Lang::En ? L")" : L"）");
        out = (tgt == Lang::En ? L"Per " : L"每") + m[1].str() + unit(m[2].str()) + inner;
        return true;
    }
    return false;
}

bool applyTemplate(const std::wstring& text, Lang src, Lang tgt, std::wstring& out) {
    if (tgt != Lang::ZhHant && tgt != Lang::En) return false;
    std::wstring t;
    for (wchar_t c : text)  // Japanese: no spaces; Korean: single spaces
        if (!(iswspace(c) && (src == Lang::Ja || (!t.empty() && t.back() == L' ')))) t += iswspace(c) ? L' ' : c;
    while (!t.empty() && t.back() == L' ') t.pop_back();
    for (const auto& tp : kTemplates) {
        if (tp.src != src) continue;
        static thread_local std::vector<std::wregex> cache;
        const size_t idx = static_cast<size_t>(&tp - kTemplates);
        if (cache.size() != std::size(kTemplates)) {
            cache.clear();
            for (const auto& x : kTemplates) cache.emplace_back(x.re);
        }
        std::wsmatch m;
        if (!std::regex_match(t, m, cache[idx])) continue;
        std::wstring r = tgt == Lang::En ? tp.en : tp.zh;
        bool ok = true;
        for (size_t g = 1; g < m.size() && ok; ++g) {
            std::wstring slot;
            if (!listSlot(m[g].str(), src, tgt, slot)) ok = false;
            const std::wstring key = L"{" + std::to_wstring(g) + L"}";
            for (size_t p = r.find(key); ok && p != std::wstring::npos; p = r.find(key)) r.replace(p, key.size(), slot);
        }
        if (!ok) continue;
        out = r;
        return true;
    }
    return false;
}

// ---- Data glossary (ja / ko -> zh-Hant, Taiwan usage) ----
// Proper nouns, stations, dishes, signs: Wikidata (CC0), hand-curated lists,
// OpenCC-converted labels (launch/_work/glossary, SOURCES.md), compiled in
// from glossary_data.inc (translate/tools/make_glossary_inc.py).  Tests:
// PM_GLOSSARY=DIR reads DIR\glossary_ja.tsv / glossary_ko.tsv instead,
// PM_GLOSSARY=off turns it off.
//   mode x  the whole block only (signs, ordinary words: 立入禁止, 금연)
//   mode s  also inside a text, kept through a placeholder (소주,
//           홍대입구역, おでん), only at a word boundary.

namespace {

struct GlossaryRow {
    const char* src;
    const char* zh;
    char mode;
    const char* kind;
};
#include "glossary_data.inc"

struct GEntry {
    std::wstring zh;
    bool sub = false;
    bool food = false;  // dish / food / drink / ingredient: common words too (수정과 = "editing and")
    bool qty = false;   // English mode q: inside a text only when the rest of it is an amount (Sodium 87.2 mg)
};
struct GTable {
    std::unordered_map<std::wstring, GEntry> exact;  // key: no spaces
    size_t maxSub = 0;                               // longest s-mode key
};

std::wstring gNoSpaces(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s)
        if (c != L' ' && c != 0x3000 && c != L'\t') o += c;
    return o;
}

// English keys: lower case, curly apostrophes straight, whitespace runs -> one space, trimmed (NO SMOKING = No smoking).
bool gEnSpace(wchar_t c) { return c == L' ' || c == 0x3000 || c == L'\t' || c == L'\n' || c == L'\r' || c == 0xA0; }
bool gEnWord(wchar_t c) { return iswalnum(c) || c == L'\'' || c == L'-'; }
wchar_t gEnFold(wchar_t c) { return c == 0x2019 || c == 0x2018 ? L'\'' : static_cast<wchar_t>(towlower(c)); }
std::wstring gEnKey(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (gEnSpace(c)) {
            if (!o.empty() && o.back() != L' ') o += L' ';
            continue;
        }
        o += gEnFold(c);
    }
    while (!o.empty() && o.back() == L' ') o.pop_back();
    return o;
}

void gAdd(GTable& t, const std::string& src, const std::string& zh, char mode, const std::string& kind, bool en = false) {
    const std::wstring s = fromUtf8(src), key = en ? gEnKey(s) : gNoSpaces(s);
    if (key.size() < 2 || t.exact.count(key)) return;  // the first row of a key wins (sorted by priority)
    GEntry e{fromUtf8(zh), mode == 's' || (en && mode == 'q'),
             kind == "dish" || kind == "food" || kind == "drink" || kind == "ingredient", en && mode == 'q'};
    if (e.sub && (en || key.size() == s.size())) t.maxSub = std::max(t.maxSub, key.size());
    else e.sub = false;  // a spaced key: whole blocks only
    t.exact.emplace(key, std::move(e));
}

void gLoadFile(GTable& t, const std::wstring& path, bool en = false) {
    std::ifstream f(path, std::ios::binary);
    std::string ln;
    while (std::getline(f, ln)) {
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        if (ln.empty() || ln[0] == '#') continue;
        std::vector<std::string> c;
        size_t at = 0;
        for (size_t p; (p = ln.find('\t', at)) != std::string::npos; at = p + 1) c.push_back(ln.substr(at, p - at));
        c.push_back(ln.substr(at));
        if (c.size() < 4 || c[0] == "src") continue;
        gAdd(t, c[0], c[1], c[2].empty() ? 'x' : c[2][0], c[3], en);
    }
}

const GTable* gTable(Lang src) {
    static GTable ja, ko, en;
    static std::once_flag once;
    std::call_once(once, [] {
        wchar_t env[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"PM_GLOSSARY", env, MAX_PATH);
        const std::wstring dir = n > 0 && n < MAX_PATH ? std::wstring(env, n) : std::wstring();
        if (dir == L"off") return;
        if (!dir.empty()) {
            gLoadFile(ja, dir + L"\\glossary_ja.tsv");
            gLoadFile(ko, dir + L"\\glossary_ko.tsv");
            gLoadFile(en, dir + L"\\glossary_en.tsv", true);
            return;
        }
        for (const auto& r : kGlossaryJa) gAdd(ja, r.src, r.zh, r.mode, r.kind);
        for (const auto& r : kGlossaryKo) gAdd(ko, r.src, r.zh, r.mode, r.kind);
        for (const auto& r : kGlossaryEn) gAdd(en, r.src, r.zh, r.mode, r.kind, true);
    });
    const GTable* t = src == Lang::Ja ? &ja : src == Lang::Ko ? &ko : src == Lang::En ? &en : nullptr;
    return t && !t->exact.empty() ? t : nullptr;
}

bool gHira(wchar_t c) { return c >= 0x3041 && c <= 0x309F; }
bool gKata(wchar_t c) { return (c >= 0x30A1 && c <= 0x30FA) || c == 0x30FC || (c >= 0xFF66 && c <= 0xFF9F); }
bool gHan(wchar_t c) { return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || c == 0x3005; }
bool gHangul(wchar_t c) { return c >= 0xAC00 && c <= 0xD7AF; }
bool gLatin(wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'); }

// Japanese: no cut inside a run of the same script (チキン in チキンカツ, 日本
// in 日本語); hiragana after the term is fine (particles: おでんを).
bool jaBoundary(const std::wstring& s, size_t b, size_t e) {
    auto same = [](wchar_t x, wchar_t y) {
        return (gKata(x) && gKata(y)) || (gHan(x) && gHan(y)) || (gHira(x) && gHira(y)) || (gLatin(x) && gLatin(y));
    };
    if (b > 0 && same(s[b - 1], s[b])) return false;
    if (e < s.size() && same(s[e - 1], s[e]) && !gHira(s[e])) return false;
    return true;
}

// Korean: the term starts a word and ends it, or a particle / copula ends it.
size_t koParticle(const std::wstring& s, size_t e) {
    static const wchar_t* const kP[] = {L"에서", L"에게", L"으로", L"까지", L"부터", L"이랑", L"하고", L"입니다", L"이에요", L"예요",
                                        L"이다", L"처럼", L"보다", L"은", L"는", L"이", L"가", L"을", L"를", L"의", L"에", L"로",
                                        L"와", L"과", L"도", L"만", L"랑"};
    for (const wchar_t* p : kP) {
        const size_t n = wcslen(p);
        if (s.compare(e, n, p) == 0 && (e + n == s.size() || !gHangul(s[e + n]))) return n;
    }
    return 0;
}
bool koBoundary(const std::wstring& s, size_t b, size_t e) {
    if (b > 0 && gHangul(s[b - 1])) return false;
    return e == s.size() || !gHangul(s[e]) || koParticle(s, e) > 0;
}

// English: whole words only (case-insensitive, any spacing); ' and - are word characters (gluten in gluten-free is
// no hit).  Food terms not inside a sentence (more than 8 words); q terms only when the rest is an amount.
std::vector<GlossaryHit> enHits(const GTable& t, const std::wstring& s) {
    std::vector<GlossaryHit> out;
    std::wstring n;
    std::vector<size_t> at;  // n[j] came from s[at[j]]
    size_t words = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (gEnSpace(s[i])) {
            if (!n.empty() && n.back() != L' ') n += L' ', at.push_back(i);
            continue;
        }
        if (n.empty() || n.back() == L' ') ++words;
        n += gEnFold(s[i]);
        at.push_back(i);
    }
    const bool sentence = words > 8;
    std::vector<char> qty;
    for (size_t j = 0; j < n.size();) {
        if (!iswalnum(n[j]) || (j > 0 && gEnWord(n[j - 1]))) {
            ++j;
            continue;
        }
        size_t best = 0;
        const GEntry* be = nullptr;
        for (size_t e = std::min(n.size(), j + t.maxSub); e >= j + 2; --e) {
            if ((e < n.size() && gEnWord(n[e])) || n[e - 1] == L' ') continue;
            const auto it = t.exact.find(n.substr(j, e - j));
            if (it == t.exact.end() || !it->second.sub || (sentence && it->second.food)) continue;
            best = e, be = &it->second;
            break;
        }
        if (!be) {
            ++j;
            continue;
        }
        out.push_back({at[j], at[best - 1] + 1 - at[j], be->zh});
        qty.push_back(be->qty);
        j = best;
    }
    if (std::find(qty.begin(), qty.end(), 1) == qty.end()) return out;
    // The rest of the text (outside the hits): numbers, units, % and brackets only?
    std::wstring rest, w;
    size_t p = 0;
    for (const auto& h : out) rest += s.substr(p, h.pos - p) + L" ", p = h.pos + h.len;
    rest += s.substr(p);
    bool digit = false, ok = true;
    auto flush = [&] {
        static const wchar_t* const kUnit[] = {L"g", L"mg", L"mcg", L"ug", L"µg", L"μg", L"kg", L"kcal", L"kj", L"cal",
                                               L"ml", L"l", L"oz", L"fl", L"iu", L"dv", L"less", L"than", L"kcals"};
        if (w.empty()) return;
        bool unit = false;
        for (const wchar_t* u : kUnit) unit |= w == u;
        ok = ok && unit;
        w.clear();
    };
    for (wchar_t c : rest) {
        c = static_cast<wchar_t>(towlower(c));
        if (iswdigit(c)) digit = true, flush();
        else if (iswalpha(c)) w += c;
        else if (wcschr(L" \t.,%<>~/()+-*:　 ", c)) flush();
        else ok = false;
    }
    flush();
    if (ok && digit) return out;
    std::vector<GlossaryHit> keep;
    for (size_t k = 0; k < out.size(); ++k)
        if (!qty[k]) keep.push_back(std::move(out[k]));
    return keep;
}

}  // namespace

const std::wstring* dataGlossary(const std::wstring& text, Lang src, Lang tgt) {
    if (tgt != Lang::ZhHant) return nullptr;
    const GTable* t = gTable(src);
    if (!t) return nullptr;
    std::wstring k = src == Lang::En ? gEnKey(text) : gNoSpaces(text);
    while (!k.empty() && wcschr(src == Lang::En ? L"。.、,!！:： " : L"。.、,!！", k.back())) k.pop_back();
    const auto it = t->exact.find(k);
    return it == t->exact.end() ? nullptr : &it->second.zh;
}

std::vector<GlossaryHit> dataGlossaryHits(const std::wstring& s, Lang src, Lang tgt) {
    std::vector<GlossaryHit> out;
    if (tgt != Lang::ZhHant) return out;
    const GTable* t = gTable(src);
    if (!t || !t->maxSub) return out;
    if (src == Lang::En) return enHits(*t, s);
    // Food words inside a sentence are often ordinary words (수정과 배포 =
    // "editing and distribution", not 水正果): only in short menu-like lines.
    size_t letters = 0;
    for (wchar_t c : s) letters += !iswspace(c);
    const bool sentence = letters > 24;
    for (size_t i = 0; i < s.size();) {
        size_t best = 0;
        const GEntry* be = nullptr;
        for (size_t n = std::min(t->maxSub, s.size() - i); n >= 2; --n) {
            const auto it = t->exact.find(s.substr(i, n));
            if (it == t->exact.end() || !it->second.sub || (sentence && it->second.food)) continue;
            if (src == Lang::Ja ? !jaBoundary(s, i, i + n) : !koBoundary(s, i, i + n)) continue;
            best = n, be = &it->second;
            break;
        }
        if (be) {
            out.push_back({i, best, be->zh});
            i += best;
        } else {
            ++i;
        }
    }
    return out;
}

}  // namespace pm::translate
