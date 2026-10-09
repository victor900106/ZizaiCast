// Label / sign / menu text fixes around the translation engine: glossary,
// table fields, names kept, misleading kana spellings.  See docs/translate.md.
#include <windows.h>

#include <algorithm>
#include <functional>
#include <chrono>
#include <cstdio>
#include <regex>
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
        {L"Wet Floor", L"小心地滑", nullptr}, {L"EXIT", L"出口", nullptr},  {L"Exit", L"結束", nullptr},  {L"PUSH", L"推", nullptr},
        {L"PULL", L"拉", nullptr},       {L"注意", L"注意", L"Caution"},   {L"주의", L"注意", L"Caution"},
        // Sign prohibitions the models loop on or reverse (NO ADMISSION … ->
        // 「無免免免免」, No Woman Admitted -> 「沒有女性會接受」); any case.
        {L"No admission except on business", L"非公莫入", nullptr}, {L"No admittance except on business", L"非公莫入", nullptr},
        {L"No admission", L"禁止進入", nullptr}, {L"No admittance", L"禁止進入", nullptr}, {L"No entry", L"禁止進入", nullptr},
        {L"Keep out", L"禁止進入", nullptr}, {L"Do not enter", L"請勿進入", nullptr}, {L"No trespassing", L"禁止擅入", nullptr},
        {L"No woman admitted", L"禁止女性進入", nullptr}, {L"No women admitted", L"禁止女性進入", nullptr},
        {L"No smoking", L"禁止吸菸", nullptr}, {L"No parking", L"禁止停車", nullptr}, {L"No photography", L"禁止攝影", nullptr},
        {L"No photos", L"禁止拍照", nullptr}, {L"No food or drink", L"禁止飲食", nullptr}, {L"No pets", L"禁止攜帶寵物", nullptr},
        {L"Staff only", L"員工專用", nullptr}, {L"Authorized personnel only", L"非相關人員禁止進入", nullptr},
        {L"Notice", L"公告", nullptr}, {L"Caution", L"注意", nullptr}, {L"Warning", L"警告", nullptr}, {L"Danger", L"危險", nullptr},
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
        {L"공기밥 추가", L"加點白飯", L"Extra rice"}, {L"공기밥 별도", L"白飯另計", L"Rice not included"},
        {L"포장 가능", L"可外帶", L"Takeout available"}, {L"포장 불가", L"不可外帶", L"No takeout"},
        {L"お通し代", L"小菜費", L"Table charge"}, {L"치킨", L"炸雞", L"Fried chicken"},
        {L"소주", L"燒酒", L"Soju"}, {L"맥주", L"啤酒", L"Beer"},
        {L"막걸리", L"馬格利米酒", L"Makgeolli"}, {L"잡채", L"韓式炒冬粉", L"Japchae"},
        {L"해물파전", L"海鮮煎餅", L"Seafood pancake"},
    };
    if (tgt != Lang::ZhHant && tgt != Lang::En) return nullptr;  // ja / ko targets: the engine's own words
    std::wstring t;  // 메 뉴 (spaced-out headings)
    for (wchar_t c : text)
        if (!iswspace(c) || !(isCjk(t.empty() ? 0 : t.back()))) t += c;
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    // Exact first; then Latin entries in any case (NO SMOKING, No Smoking) -
    // but an all-caps sign word (EXIT, PUSH) only for an all-caps text: the
    // 「Exit」 of an app is 結束, not 出口.
    for (const auto& g : kG)
        if (text == g.src || t == g.src) {
            const wchar_t* r = tgt == Lang::ZhHant ? g.zh : g.en;
            if (r) return r;
        }
    bool latin = !t.empty(), upper = true;
    for (wchar_t c : t) latin = latin && c < 0x80, upper = upper && !(c >= L'a' && c <= L'z');
    if (!latin) return nullptr;
    for (const auto& g : kG) {
        bool capsKey = true;
        for (const wchar_t* c = g.src; *c; ++c) capsKey = capsKey && !(*c >= L'a' && *c <= L'z');
        if (capsKey && !upper) continue;
        if (_wcsicmp(t.c_str(), g.src) == 0) {
            const wchar_t* r = tgt == Lang::ZhHant ? g.zh : g.en;
            if (r) return r;
        }
    }
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
    {Lang::Ja, L"殺菌方法", L"殺菌方法", L"Sterilization"},
    {Lang::Ja, L"原産国名", L"原產國", L"Country of origin"},
    {Lang::Ja, L"原料原産地名", L"原料原產地", L"Origin of ingredients"},
    {Lang::Ja, L"使用上の注意", L"使用注意事項", L"Precautions"},
    {Lang::Ja, L"製造所所在地", L"製造廠地址", L"Factory address"},
    {Lang::Ja, L"添加物", L"添加物", L"Additives"},
    {Lang::Ja, L"アレルギー物質", L"過敏原", L"Allergens"},
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
    // P0 (eval set): labels of manuals / notices / tables, and the
    // owner's label read as 製 造 (者 lost).
    {Lang::Ja, L"製造", L"製造", L"Made by"},
    {Lang::Ja, L"調理方法", L"調理方法", L"Preparation"},
    {Lang::Ja, L"作り方", L"調理方法", L"Preparation"},
    {Lang::Ja, L"お問い合わせ", L"聯絡我們", L"Contact"},
    {Lang::Ja, L"お問合せ", L"聯絡我們", L"Contact"},
    {Lang::Ja, L"お客様相談室", L"消費者服務中心", L"Customer service"},
    {Lang::Ja, L"受付時間", L"服務時間", L"Hours"},
    {Lang::Ja, L"営業時間", L"營業時間", L"Opening hours"},
    {Lang::Ja, L"定休日", L"公休日", L"Closed on"},
    {Lang::Ja, L"使用温度", L"使用溫度", L"Operating temperature"},
    {Lang::Ja, L"使用温度範囲", L"使用溫度範圍", L"Operating temperature"},
    {Lang::Ja, L"定格", L"額定", L"Rating"},
    {Lang::Ja, L"定格入力", L"額定輸入", L"Rated input"},
    {Lang::Ja, L"定格出力", L"額定輸出", L"Rated output"},
    {Lang::Ja, L"入力", L"輸入", L"Input"},
    {Lang::Ja, L"出力", L"輸出", L"Output"},
    {Lang::Ja, L"容量", L"容量", L"Capacity"},
    {Lang::Ja, L"電池容量", L"電池容量", L"Battery capacity"},
    {Lang::Ja, L"質量", L"重量", L"Weight"},
    {Lang::Ja, L"重量", L"重量", L"Weight"},
    {Lang::Ja, L"ナトリウム", L"鈉", L"Sodium"},
    {Lang::Ja, L"糖類", L"糖", L"Sugars"},
    {Lang::Ja, L"飽和脂肪酸", L"飽和脂肪", L"Saturated fat"},
    {Lang::Ko, L"총 내용량", L"總內容量", L"Net contents"},
    {Lang::Ko, L"총내용량", L"總內容量", L"Net contents"},
    {Lang::Ko, L"지방", L"脂肪", L"Fat"},
    {Lang::Ko, L"포화지방", L"飽和脂肪", L"Saturated fat"},
    {Lang::Ko, L"트랜스지방", L"反式脂肪", L"Trans fat"},
    {Lang::Ko, L"콜레스테롤", L"膽固醇", L"Cholesterol"},
    {Lang::Ko, L"입력", L"輸入", L"Input"},
    {Lang::Ko, L"출력", L"輸出", L"Output"},
    {Lang::Ko, L"정격", L"額定", L"Rating"},
    {Lang::Ko, L"정격 입력", L"額定輸入", L"Rated input"},
    {Lang::Ko, L"소비전력", L"耗電量", L"Power consumption"},
    {Lang::Ko, L"소비 전력", L"耗電量", L"Power consumption"},
    {Lang::Ko, L"사용 온도", L"使用溫度", L"Operating temperature"},
    {Lang::Ko, L"사용온도", L"使用溫度", L"Operating temperature"},
    {Lang::Ko, L"고객센터", L"客服中心", L"Customer service"},
    {Lang::Ko, L"용량", L"容量", L"Capacity"},
    {Lang::Ko, L"중량", L"重量", L"Weight"},
    {Lang::Ko, L"영업시간", L"營業時間", L"Opening hours"},
    {Lang::Ko, L"알레르기", L"過敏原", L"Allergens"},
    {Lang::En, L"Ingredients", L"成分", nullptr},
    {Lang::En, L"Input", L"輸入", nullptr},
    {Lang::En, L"Output", L"輸出", nullptr},
    {Lang::En, L"Serving size", L"每份份量", nullptr},
    {Lang::En, L"Servings per container", L"每包裝份數", nullptr},
    {Lang::En, L"Calories", L"熱量", nullptr},
    {Lang::En, L"Total Fat", L"總脂肪", nullptr},
    {Lang::En, L"Saturated Fat", L"飽和脂肪", nullptr},
    {Lang::En, L"Trans Fat", L"反式脂肪", nullptr},
    {Lang::En, L"Cholesterol", L"膽固醇", nullptr},
    {Lang::En, L"Sodium", L"鈉", nullptr},
    {Lang::En, L"Total Carbohydrate", L"總碳水化合物", nullptr},
    {Lang::En, L"Dietary Fiber", L"膳食纖維", nullptr},
    {Lang::En, L"Total Sugars", L"總糖", nullptr},
    {Lang::En, L"Protein", L"蛋白質", nullptr},
    {Lang::En, L"Contains", L"含有", nullptr},
    {Lang::En, L"Best by", L"最佳賞味日期", nullptr},
    {Lang::En, L"Best before", L"最佳賞味日期", nullptr},
    {Lang::En, L"Operating temperature", L"使用溫度", nullptr},
    {Lang::En, L"Capacity", L"容量", nullptr},
    {Lang::En, L"Weight", L"重量", nullptr},
    {Lang::En, L"Net Wt", L"淨重", nullptr},
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
    // Dishes the pivot turns into nonsense (焼き鳥 -> 「雅基托里」, だし巻き玉子 -> 「田之木」).
    {L"焼き鳥", L"烤雞肉串", L"yakitori"}, {L"焼鳥", L"烤雞肉串", L"yakitori"}, {L"盛り合わせ", L"拼盤", L"platter"},
    {L"盛合せ", L"拼盤", L"platter"}, {L"三点盛り", L"三點拼盤", L"platter of three"}, {L"刺身", L"生魚片", L"sashimi"},
    {L"お造り", L"生魚片", L"sashimi"}, {L"だし巻き玉子", L"高湯玉子燒", L"dashi rolled omelet"},
    {L"だし巻き卵", L"高湯玉子燒", L"dashi rolled omelet"}, {L"玉子焼き", L"玉子燒", L"rolled omelet"},
    {L"卵焼き", L"玉子燒", L"rolled omelet"}, {L"お通し代", L"小菜費", L"table charge (otoshi)"}, {L"お通し", L"小菜", L"otoshi (table charge appetizer)"},
    {L"ラストオーダー", L"最後點餐", L"last order"}, {L"生ビール（中）", L"生啤酒（中杯）", L"draft beer (medium)"},
    {L"生ビール（大）", L"生啤酒（大杯）", L"draft beer (large)"}, {L"生ビール（小）", L"生啤酒（小杯）", L"draft beer (small)"},
    {L"生ビール", L"生啤酒", L"draft beer"},
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

}  // namespace

bool isFieldLabel(const std::wstring& text) {
    std::wstring q;
    for (wchar_t c : text)
        if (!isSpace(c) && c != L'：' && c != L':') q += c;
    for (const auto& f : kFields) {
        std::wstring l;
        for (const wchar_t* p = f.label; *p; ++p)
            if (!isSpace(*p)) l += *p;
        if (_wcsicmp(q.c_str(), l.c_str()) == 0) return true;
    }
    return false;
}

namespace {

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
        // Bullets start a field too (●賞味期限：… ●保存方法：… on one line).
        const bool boundary = i == 0 || isSpace(s[i - 1]) || s[i - 1] == L'|' || wcschr(L"●■◆○◎・", s[i - 1]);
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
                // An English label followed by words is prose ("Protein bars
                // are on sale" is not 蛋白質　…): a colon, or an amount after it.
                if (f.label[0] < 0x80 && nx != L':' && nx != L'：') {
                    size_t k = e;
                    while (k < s.size() && isSpace(s[k])) ++k;
                    if (k < s.size() && !iswdigit(s[k]) && !wcschr(L"<.(~0", s[k])) continue;
                }
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
    std::wstring zh;  // data glossary term: its zh-Hant ("" = rendered by renderEntity)
};
// Tokens never translated (kept through a placeholder): @handles, #hashtags,
// URLs, e-mail addresses, Latin / digit tokens with digits or _ that start
// with a letter (ACG-ZONE5, v2_1, CRITICAL_PROCESS_DIED - not 100mL / 0g:
// units are localised to 毫升 / 克), and in Japanese / Korean text ASCII acronyms (MRT, ATM, JR).  The
// pivot turned @multi_wotakun into @tim_wotakun and split MRT into 「MR肉品」.
std::vector<Entity> protectedTokens(const std::wstring& s, Lang src, Lang tgt) {
    std::vector<Entity> out;
    static const std::wregex re(
        L"(https?://[^\\s　]+|www\\.[^\\s　]+|[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}|[@＠][A-Za-z0-9_.]{2,}|[#＃][^\\s　#＃、。,]{1,40}|"
        L"\\b(?=[A-Za-z0-9_-]*[0-9_])(?=[A-Za-z0-9_-]*[A-Za-z])(?:[A-Za-z][A-Za-z0-9_-]+|[0-9][A-Za-z0-9-]*_[A-Za-z0-9_-]*)\\b|\\b[A-Z][A-Z0-9&]{1,5}\\b)");
    int capsWords = 0;
    {
        static const std::wregex caps(L"\\b[A-Z]{2,}\\b");
        for (std::wsregex_iterator it(s.begin(), s.end(), caps), e; it != e; ++it) ++capsWords;
    }
    for (std::wsregex_iterator it(s.begin(), s.end(), re), e; it != e; ++it) {
        const std::wstring t = it->str();
        const bool acronym = t.size() <= 6 && std::all_of(t.begin(), t.end(), [](wchar_t c) { return (c >= L'A' && c <= L'Z') || iswdigit(c) || c == L'&'; });
        if (acronym && src != Lang::Ja && src != Lang::Ko) continue;  // English text: the engine knows its acronyms
        if (acronym && capsWords >= 2) continue;  // an all-caps English phrase (MADE IN KOREA), not an acronym
        // A nutrient and its amount (Na1171mg, B12 2.4µg): 鈉1171毫克 - the units are localised.
        static const std::wregex amount(L"[0-9](mg|g|kg|mcg|ug|ml|mL|l|L|kcal|Kcal|kJ|cm|mm|km|m)$");
        if (std::regex_search(t, amount)) continue;
        std::wstring r = t;
        if (tgt == Lang::ZhHant) {  // zh-TW names of transit / everyday acronyms
            static const std::pair<const wchar_t*, const wchar_t*> kZh[] = {{L"MRT", L"捷運"}, {L"ATM", L"ATM"}, {L"JR", L"JR"}, {L"PC", L"電腦"}};
            for (const auto& [a, z] : kZh)
                if (t == a) r = z;
        }
        out.push_back({static_cast<size_t>(it->position()), t.size(), r});
    }
    // Transit lines: 2호선 / 2号線 -> 2號線 (the pivot read 「兩條線」).
    if (tgt == Lang::ZhHant && (src == Lang::Ko || src == Lang::Ja)) {
        static const std::wregex line(L"([0-9]{1,2})\\s?(호선|号線|號線)");
        for (std::wsregex_iterator it(s.begin(), s.end(), line), e; it != e; ++it) {
            const size_t pos = static_cast<size_t>(it->position()), len = it->str().size();
            bool overlap = false;
            for (const auto& o : out) overlap |= pos < o.pos + o.len && o.pos < pos + len;
            if (!overlap) out.push_back({pos, len, (*it)[1].str() + L"號線"});
        }
    }
    // Japanese large-number units: 3.1万 is 3.1萬 (the pivot: 「310萬」 via "3.1 million").
    if (tgt == Lang::ZhHant && src == Lang::Ja) {
        static const std::wregex man(L"([0-9]+(?:[.,][0-9]+)?)\\s?([万億])");
        for (std::wsregex_iterator it(s.begin(), s.end(), man), e; it != e; ++it) {
            const size_t pos = static_cast<size_t>(it->position()), len = it->str().size();
            bool overlap = false;
            for (const auto& o : out) overlap |= pos < o.pos + o.len && o.pos < pos + len;
            if (!overlap) out.push_back({pos, len, (*it)[1].str() + ((*it)[2].str() == L"万" ? L"萬" : L"億")});
        }
    }
    if (tgt == Lang::ZhHant && (src == Lang::Ko || src == Lang::Ja)) {
        std::sort(out.begin(), out.end(), [](const Entity& a, const Entity& b) { return a.pos < b.pos; });
    }
    return out;
}

std::vector<Entity> findEntities(const std::wstring& s, Lang src, Lang tgt) {
    std::vector<Entity> out = protectedTokens(s, src, tgt);
    auto inToken = [&](size_t pos, size_t len) {
        for (const auto& t : out)
            if (pos < t.pos + t.len && t.pos < pos + len) return true;
        return false;
    };
    const size_t nTok = out.size();
    for (auto& h : dataGlossaryHits(s, src, tgt))
        if (!inToken(h.pos, h.len)) out.push_back({h.pos, h.len, std::move(h.zh)});
    (void)nTok;
    if (src != Lang::Ja) {
        std::sort(out.begin(), out.end(), [](const Entity& a, const Entity& b) { return a.pos < b.pos; });
        return out;
    }
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
            out.push_back({b, e - b, {}});
        }
    }
    for (const auto& n : kNames) {
        const std::wstring ns = n.src;
        if (ns.find(L"会社") != std::wstring::npos || ns[0] == L'(' || ns[0] == L'（' || ns[0] == L'㈱') continue;
        for (size_t p = s.find(ns); p != std::wstring::npos; p = s.find(ns, p + ns.size())) out.push_back({p, ns.size(), {}});
    }
    // 八ッ橋-like: kanji + ッ / ヶ + kanji.
    for (size_t i = 1; i + 1 < s.size(); ++i)
        if ((s[i] == L'ッ' || s[i] == L'ヶ') && isHan(s[i - 1]) && isHan(s[i + 1])) {
            size_t b = i - 1, e = i + 2;
            while (b > 0 && isHan(s[b - 1])) --b;
            while (e < s.size() && isHan(s[e])) ++e;
            out.push_back({b, e - b, {}});
        }
    // Overlaps: the earliest, longest one wins (the same span: the built-in names before the data glossary).
    std::sort(out.begin(), out.end(), [](const Entity& a, const Entity& b) {
        return a.pos != b.pos ? a.pos < b.pos : a.len != b.len ? a.len > b.len : a.zh.empty() && !b.zh.empty();
    });
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

const wchar_t* fieldText(const Field& f, Lang tgt) { return tgt == Lang::ZhHant ? f.zh : f.en ? f.en : f.label; }

// A field label of src (exact, spaces / case ignored).
const Field* fieldOf(const std::wstring& text, Lang src) {
    std::wstring q;
    for (wchar_t c : text)
        if (!isSpace(c) && c != L'：' && c != L':') q += c;
    for (const auto& f : kFields) {
        if (f.src != src) continue;
        std::wstring l;
        for (const wchar_t* p = f.label; *p; ++p)
            if (!isSpace(*p)) l += *p;
        if (_wcsicmp(q.c_str(), l.c_str()) == 0) return &f;
    }
    return nullptr;
}

// Between a row's label and its value: 「標籤　值」 (owner decision (1)).
const wchar_t* rowSep(Lang tgt) { return tgt == Lang::En ? L": " : L"\x3000"; }

// Text without words: numbers, units, dates, phone numbers, prices
// (42kcal, 0.05g, 26.12.09, 0120-8284-39, ¥780, 9,000원): kept as written.
bool quantityOnly(const std::wstring& t) {
    const ScriptCount n = countScripts(t);
    if (n.digits == 0) return false;
    if (n.kana > 0 || n.hangul > 1) return false;
    if (n.hangul == 1) {  // 9,000원, 3개
        for (wchar_t c : t)
            if (c >= 0xAC00 && c <= 0xD7AF && !wcschr(L"원개장병잔인분", c)) return false;
    }
    if (n.han > 2) return false;  // 8袋（16枚）: two counters
    // Latin letters only as units (kcal, mAh, V ~ Hz, mL), not words (Best by, cup).
    static const wchar_t* const kUnits[] = {L"g", L"mg", L"kg", L"μg", L"ml", L"mL", L"L", L"l", L"kcal", L"kJ", L"cal", L"V",
                                            L"W", L"Hz", L"mAh", L"Ah", L"A", L"cm", L"mm", L"m", L"km", L"oz", L"lb", L"lbs",
                                            L"fl", L"F", L"C", L"pcs", L"x", L"X", L"h", L"min", L"s", L"kWh", L"GB", L"MB", L"TB"};
    std::wstring w;
    auto word = [&] {
        if (w.empty()) return true;
        bool ok = false;
        for (const wchar_t* u : kUnits) ok |= w == u;
        w.clear();
        return ok;
    };
    for (wchar_t c : t) {
        if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z')) w += c;
        else if (!word()) return false;
    }
    return word();
}

// An ingredient list (原材料名 values): items separated by 、 / ・ / ， (at
// least 4 outside brackets), short on average, no sentence outside brackets.
bool ingredientList(const std::wstring& s) {
    int depth = 0, seps = 0;
    size_t letters = 0;
    std::wstring outside;
    for (wchar_t c : s) {
        if (c == L'(' || c == L'（') ++depth;
        else if ((c == L')' || c == L'）') && depth) --depth;
        else {
            // Items of a bracketed sub-list count too (風味原料（さば粉末、…）).
            if (c == L'、' || c == L'，' || c == L',' || c == L'・') ++seps;
            else if (!iswspace(c)) ++letters;
            if (!depth) outside += c;
        }
    }
    if (seps < 4 || letters > static_cast<size_t>(seps + 1) * 12) return false;
    for (const wchar_t* w : {L"。", L"です", L"ます", L"ください", L"してい", L"ません"})
        if (outside.find(w) != std::wstring::npos) return false;
    return true;
}

// A field value worth the engine (Chinese target: kanji / numbers / units are
// readable as they are: 焼菓子, 8袋（16枚）, 42kcal).
bool needsEngine(const std::wstring& v, Lang tgt) {
    if (quantityOnly(v)) return false;
    if (tgt != Lang::ZhHant) return true;
    const ScriptCount n = countScripts(v);
    return n.kana + n.hangul > 0 || n.latin > 6;
}

// Literal text for the target: Japanese kanji in Traditional forms (zh-Hant), else as written.
std::wstring literal(const std::wstring& v, Lang src, Lang tgt) {
    if (tgt != Lang::ZhHant && tgt != Lang::En) return v;
    std::wstring t = v;
    if (src == Lang::Ko) {
        // ₩ read as W by the recogniser (W5,000 on a Korean menu).
        for (size_t p = 0; p + 1 < t.size(); ++p)
            if (t[p] == L'W' && iswdigit(t[p + 1]) && (p == 0 || !iswalpha(t[p - 1]))) t[p] = L'\x20A9';
        // Korean counters after a number: 9,000원 -> 9,000韓元
        static const struct {
            const wchar_t *ko, *zh, *en;
        } kC[] = {{L"원", L"韓元", L" won"}, {L"개", L"個", L" pcs"}, {L"장", L"張", L" sheets"}, {L"병", L"瓶", L" bottles"},
                  {L"잔", L"杯", L" glasses"}, {L"인분", L"人份", L" servings"}};
        for (const auto& c : kC)
            for (size_t p = t.find(c.ko); p != std::wstring::npos; p = t.find(c.ko, p + 1))
                if (p > 0 && (iswdigit(t[p - 1]) || t[p - 1] == L' ')) t.replace(p, wcslen(c.ko), tgt == Lang::En ? c.en : c.zh);
    }
    if (tgt != Lang::ZhHant) return t;
    // #hashtags, @handles and URLs as written (#台湾旅行 is a search term, not 臺灣).
    if (t.find_first_of(L"#＃@＠") == std::wstring::npos && t.find(L"://") == std::wstring::npos)
        return src == Lang::Ja ? convertJapaneseKanji(t) : toTraditional(t);
    std::wstring o;
    for (size_t i = 0; i < t.size();) {
        size_t e = i;
        while (e < t.size() && !isSpace(t[e])) ++e;
        while (e < t.size() && isSpace(t[e]) && e == i) ++e;
        const std::wstring w = t.substr(i, e - i);
        const bool keep = !w.empty() && (wcschr(L"#＃@＠", w[0]) || w.find(L"://") != std::wstring::npos);
        o += keep ? w : src == Lang::Ja ? convertJapaneseKanji(w) : toTraditional(w);
        i = e;
    }
    return o;
}

}  // namespace

bool translateTexts(Engine& engine, Lang src, Lang tgt, const std::vector<std::wstring>& in, std::vector<std::wstring>& out,
                    std::wstring* err) {
    Escalator esc(engine);
    return translateTextsEx(engine, &esc, src, tgt, in, {}, out, nullptr, err);
}

bool translateTextsEx(Engine& engine, Escalator* esc, Lang src, Lang tgt, const std::vector<std::wstring>& in,
                      const std::vector<size_t>& labelLen, std::vector<std::wstring>& out, std::vector<TextInfo>* info,
                      std::wstring* err) {
    out.assign(in.size(), {});
    if (info) info->assign(in.size(), {});
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
        std::wstring plain;     // the same piece without placeholders (checks, retries)
        bool item = false;      // an item of an ingredient list (list mode)
    };
    std::vector<std::vector<Part>> plan(in.size());
    // Placeholders the models copy unchanged (measured: ZQA / ZQB survive
    // ja -> en -> zh-Hant; X1 or [1] change the sentence around them).
    static const wchar_t* const kPh[] = {L"ZQA", L"ZQB", L"ZQC", L"ZQD", L"ZQE", L"ZQF"};
    // One value / sentence: glossary, phrase, literal (numbers, addresses,
    // Japanese kanji), or the engine with names protected.
    auto addText = [&](std::vector<Part>& parts, std::wstring t) {
        // Leading bullets / marks stay as they are (※, ●, ・).
        std::wstring lead;
        while (!t.empty() && (wcschr(L"※●・◆■□★☆*＊○◎", t[0]) || isSpace(t[0]))) {
            if (!isSpace(t[0])) lead += t[0];
            t.erase(0, 1);
        }
        if (t.empty()) return;
        if (!lead.empty()) parts.push_back({lead + (tgt == Lang::En ? L" " : L"")});
        // A display name with its handle (ぽんこつ @multi_wotakun, OCR read as one
        // line): a name, kept as written - never an invented transliteration.
        {
            static const std::wregex nameHandle(L"^[^\\s@＠]{1,16}\\s*[@＠][A-Za-z0-9_.]{2,}$");
            if (std::regex_match(t, nameHandle)) {
                parts.push_back({t});
                return;
            }
        }
        if (const wchar_t* g = glossary(t, tgt)) {
            parts.push_back({g});
            return;
        }
        if (fixes) {
            if (const Phrase* p = phrase(t)) {
                parts.push_back({tgt == Lang::ZhHant ? p->zh : p->en});
                return;
            }
            if (const Field* f = fieldOf(t, src)) {
                parts.push_back({fieldText(*f, tgt)});
                return;
            }
            std::wstring qp;
            if (quantityPhrase(t, src, tgt, qp)) {
                parts.push_back({qp});
                return;
            }
            // High-risk sentence templates (human translations, every slot a
            // known term): rules before the engine (ARCHITECTURE.md §3.7.1).
            if (applyTemplate(t, src, tgt, qp)) {
                parts.push_back({qp});
                return;
            }
            // Kept as written (ARCHITECTURE.md policy Keep / ConvertScript):
            // numbers / units / dates / phone numbers, addresses, and for a
            // zh-Hant reader Japanese kanji-only text in Traditional forms
            // (the pivot turns 焼菓子 into 「山梨」, 膨張剤 into 「腫脹劑」).
            if (const std::wstring* g = dataGlossary(t, src, tgt)) {
                parts.push_back({*g});
                return;
            }
            const ScriptCount n = countScripts(t);
            if (quantityOnly(t) || looksLikeAddress(t) ||
                (tgt == Lang::ZhHant && src == Lang::Ja && n.kana == 0 && n.hangul == 0 && n.han > 0 && n.latin <= 2)) {
                // Data glossary terms in it (新宿駅 3番線 -> 新宿站 3番線), the rest as written.
                std::wstring o;
                size_t at = 0;
                if (!looksLikeAddress(t))
                    for (const auto& h : dataGlossaryHits(t, src, tgt)) {
                        o += literal(t.substr(at, h.pos - at), src, tgt) + h.zh;
                        at = h.pos + h.len;
                    }
                parts.push_back({o + literal(t.substr(at), src, tgt)});
                return;
            }
        }
        if (src == Lang::Ja)
            for (const auto& [a, b] : kRespell)
                for (size_t p = t.find(a); p != std::wstring::npos; p = t.find(a, p + 1)) t.replace(p, wcslen(a), b);
        Part part;
        std::wstring prot = t;
        if (fixes) {
            const auto ents = findEntities(t, src, tgt);
            if (!ents.empty() && ents.size() <= std::size(kPh)) {
                prot.clear();
                size_t at = 0, k = 0;
                for (const auto& e : ents) {
                    bool complete = true;
                    std::wstring r = e.zh.empty() ? renderEntity(t.substr(e.pos, e.len), tgt, &complete) : e.zh;
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
                // Names with numbers / brackets only (焼き鳥盛り合わせ（5本）): no engine.
                std::wstring rest = prot;
                for (const auto& [ph, r] : part.ph)
                    for (size_t p = rest.find(ph); p != std::wstring::npos; p = rest.find(ph)) rest.erase(p, ph.size());
                const ScriptCount rc = countScripts(rest);
                // ... or with a price / quantity (김치찌개9,000원, 소주 ₩5,000): the rest as written.
                const bool restQty = !part.ph.empty() && !squeeze(rest).empty() && quantityOnly(squeeze(rest));
                // Two names joined by の (MRTの飲食禁止 -> 捷運 + 禁止飲食): the names side by side.
                if (part.ph.size() >= 2 && squeeze(rest) == L"の" && tgt == Lang::ZhHant) {
                    std::wstring o = prot;
                    for (size_t q = o.find(L"の"); q != std::wstring::npos; q = o.find(L"の")) o.erase(q, 1);
                    for (const auto& [ph, r] : part.ph)
                        for (size_t q = o.find(ph); q != std::wstring::npos; q = o.find(ph)) o.replace(q, ph.size(), r);
                    parts.push_back({o});
                    return;
                }
                if (!part.ph.empty() && ((rc.kana == 0 && rc.hangul == 0 && rc.latin == 0 && rc.han <= 1) || restQty)) {
                    std::wstring o = literal(prot, src, tgt);
                    bool skewers = false;
                    for (const auto& pr : part.ph) skewers |= pr.second.find(L'串') != std::wstring::npos;
                    if (tgt == Lang::ZhHant && skewers)  // 5本 of skewers: 5串
                        for (size_t p = o.find(L"本）"); p != std::wstring::npos && p > 0 && iswdigit(o[p - 1]); p = o.find(L"本）", p + 1))
                            o[p] = L'串';
                    for (const auto& [ph, r] : part.ph)
                        for (size_t p = o.find(ph); p != std::wstring::npos; p = o.find(ph)) o.replace(p, ph.size(), r);
                    parts.push_back({o});
                    return;
                }
            }
        }
        // Checked before (this session, memory only): no engine call.
        TrHypothesis known;
        if (esc && fixes && esc->cached(src, tgt, t, known)) {
            parts.push_back({known.text});
            return;
        }
        part.eng = ask(prot);
        part.plain = t;
        parts.push_back(std::move(part));
    };
    // A table row's value: kept (numbers, kanji) or translated.
    // ---- List mode (原材料 lists: 牛肉、マッシュルーム、…、野菜・果実(玉葱、パイン)) ----
    // Each item on its own (the data glossary, the rules, else the engine
    // for that item alone), joined with 、 and the brackets kept: through the
    // pivot the whole list came back with items merged, invented twice
    // (番茄醬 番茄醬) or misread in context (玉葱 -> 芋片, みりん -> 米林).
    std::function<void(std::vector<Part>&, const std::wstring&)> addList;
    auto addItem = [&](std::vector<Part>& parts, std::wstring it) {
        while (!it.empty() && isSpace(it.back())) it.pop_back();
        while (!it.empty() && isSpace(it.front())) it.erase(0, 1);
        if (it.empty()) return;
        if (const std::wstring* g = dataGlossary(it, src, tgt)) {
            parts.push_back({*g});
            return;
        }
        // Items run together (OCR lost a 、: 赤ワイントマトケチャップフルーツチャツネ;
        // 野菜果実): split when data glossary terms cover them whole (fewest terms).
        if (it.size() <= 40) {
            const size_t n = it.size();
            std::vector<int> best(n + 1, 1 << 20), from(n + 1, -1);
            std::vector<const std::wstring*> term(n + 1, nullptr);
            best[0] = 0;
            for (size_t i = 0; i < n; ++i) {
                if (best[i] >= (1 << 20)) continue;
                for (size_t j = i + 2; j <= n; ++j)
                    if (const std::wstring* g = dataGlossary(it.substr(i, j - i), src, tgt); g && best[i] + 1 < best[j])
                        best[j] = best[i] + 1, from[j] = static_cast<int>(i), term[j] = g;
            }
            if (best[n] >= 2 && best[n] < (1 << 20)) {
                std::vector<std::wstring> seq;
                for (size_t j = n; j > 0; j = static_cast<size_t>(from[j])) seq.push_back(*term[j]);
                for (size_t k = seq.size(); k-- > 0;) parts.push_back({(k + 1 < seq.size() ? L"、" : L"") + seq[k]});
                return;
            }
        }
        const size_t before = parts.size();
        addText(parts, it);
        for (size_t k = before; k < parts.size(); ++k) parts[k].item = true;
    };
    // A bracketed sentence (原材料の一部に豚肉、大豆を含む) is one piece; a
    // bracketed list (アミノ酸等 / 玉葱、パイン) is a list again.
    auto sentenceLike = [](const std::wstring& s) {
        static const wchar_t* const kP[] = {L"の一部", L"を含む", L"に含", L"を使用", L"が含", L"については", L"です", L"ます"};
        for (const wchar_t* w : kP)
            if (s.find(w) != std::wstring::npos) return true;
        return false;
    };
    addList = [&](std::vector<Part>& parts, const std::wstring& s) {
        auto open = [](wchar_t c) { return c == L'(' || c == L'（' || c == L'【' || c == L'[' || c == L'［'; };
        auto close = [](wchar_t c) { return c == L')' || c == L'）' || c == L'】' || c == L']' || c == L'］'; };
        bool first = true;
        std::wstring cur;
        auto sep = [&] {
            if (!first) parts.push_back({tgt == Lang::En ? L", " : L"、"});
            first = false;
        };
        auto flushItem = [&] {
            std::wstring t = cur;
            cur.clear();
            while (!t.empty() && isSpace(t.front())) t.erase(0, 1);
            if (t.empty()) return;
            sep();
            addItem(parts, t);
        };
        for (size_t i = 0; i < s.size(); ++i) {
            const wchar_t c = s[i];
            if (open(c)) {
                // head（inner）: the head, then the bracket's content.
                size_t depth = 1, j = i + 1;
                for (; j < s.size() && depth; ++j) depth += open(s[j]) ? 1 : close(s[j]) ? -1 : 0;
                const std::wstring inner = s.substr(i + 1, (depth ? j : j - 1) - i - 1);
                std::wstring head = cur;
                cur.clear();
                while (!head.empty() && isSpace(head.front())) head.erase(0, 1);
                if (!head.empty()) {
                    sep();
                    // 調味料（アミノ酸等）: the whole item may be a glossary term.
                    const std::wstring whole = head + L"（" + inner + L"）";
                    if (const std::wstring* g = dataGlossary(whole, src, tgt)) {
                        parts.push_back({*g});
                        i = depth ? j - 1 : j - 1;
                        continue;
                    }
                    addItem(parts, head);
                } else {
                    sep();
                }
                parts.push_back({tgt == Lang::En ? L" (" : L"（"});
                if (sentenceLike(inner)) addText(parts, inner);
                else addList(parts, inner);
                parts.push_back({tgt == Lang::En ? L")" : L"）"});
                first = false;
                i = j - 1;
                continue;
            }
            if (c == L'、' || c == L'，' || c == L',' || c == L'・' || c == L'／' || c == L'/') {
                flushItem();
                continue;
            }
            cur += c;
        }
        flushItem();
    };
    auto addValue = [&](std::vector<Part>& parts, const std::wstring& v) {
        if (fixes && !needsEngine(v, tgt)) parts.push_back({literal(v, src, tgt)});
        else if (fixes && src == Lang::Ja && ingredientList(v)) addList(parts, v);
        else addText(parts, v);
    };
    for (size_t i = 0; i < in.size(); ++i) {
        auto& parts = plan[i];
        std::wstring line = in[i];
        size_t ll = i < labelLen.size() ? labelLen[i] : 0;
        // Degree signs read twice by the recogniser (32°℉, 40°℃).
        for (const auto& [x, y] : {std::pair{L"°℉", L"°F"}, std::pair{L"°℃", L"℃"}})
            for (size_t p = line.find(x); p != std::wstring::npos; p = line.find(x, p + 1)) {
                if (p < ll) ll -= wcslen(x) - wcslen(y);
                line.replace(p, wcslen(x), y);
            }
        if (src == Lang::Ja)  // misread label words first: 赏味期限 must still be a field label
            for (const auto& [x, y] : kRespell)
                for (size_t p = line.find(x); p != std::wstring::npos; p = line.find(x, p + 1)) {
                    if (p < ll) ll = ll + wcslen(y) - wcslen(x);
                    line.replace(p, wcslen(x), y);
                }
        // A row from the layout (label cell + value cell): 「標籤　值」.
        if (fixes && ll > 0 && ll < line.size()) {
            if (info) (*info)[i].row = true;
            std::wstring label = line.substr(0, ll), value = line.substr(ll);
            while (!value.empty() && isSpace(value.front())) value.erase(0, 1);
            while (!label.empty() && (isSpace(label.back()) || label.back() == L'：' || label.back() == L':')) label.pop_back();
            if (const Field* f = fieldOf(label, src)) parts.push_back({fieldText(*f, tgt)});
            else addText(parts, label);
            parts.push_back({rowSep(tgt)});
            const auto fields = splitFields(value, src);  // more fields in the value cell
            if (fields.empty()) {
                addValue(parts, value);
            } else {
                for (size_t k = 0; k < fields.size(); ++k) {
                    if (k) parts.push_back({tgt == Lang::En ? L"; " : L"\x3000"});
                    if (fields[k].field) parts.push_back({std::wstring(fieldText(*fields[k].field, tgt)) + rowSep(tgt)});
                    addValue(parts, fields[k].value);
                }
            }
            continue;
        }
        const auto fields = fixes ? splitFields(line, src) : std::vector<Piece>{};
        if (fields.empty()) {
            if (fixes && src == Lang::Ja && ingredientList(line)) addList(parts, line);
            else addText(parts, line);
            continue;
        }
        if (info) (*info)[i].row = true;
        bool first = true;
        for (const auto& f : fields) {
            if (!first) parts.push_back({L"\n"});
            first = false;
            if (!f.field) {
                addText(parts, f.value);
                continue;
            }
            parts.push_back({std::wstring(fieldText(*f.field, tgt)) + (f.value.empty() ? L"" : rowSep(tgt))});
            if (!f.value.empty()) addValue(parts, f.value);
        }
    }
    std::vector<std::wstring> res;
    static const bool prof = std::getenv("PM_TR_PROF") != nullptr;  // tests: where the time goes
    auto msNow = [] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    const double pt0 = msNow();
    if (!engineIn.empty() && !engine.translate(src, tgt, engineIn, res, err)) return false;
    const double pt1 = msNow();
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
    const double pt2 = msNow();
    double escMs = 0;
    int escN = 0;
    // The retried pieces: their own engine slot (no placeholders).
    for (size_t j = 0; j < retryAt.size() && j < retryOut.size(); ++j) {
        Part& p = plan[retryAt[j].first][retryAt[j].second];
        engineIn.push_back(p.plain);
        res.push_back(retryOut[j]);
        p.eng = engineIn.size() - 1;
        p.ph.clear();
    }
    // Placeholders back.
    auto restore = [&](const Part& p, std::wstring r) {
        while (!r.empty() && isSpace(r.back())) r.pop_back();
        for (const auto& [ph, ent] : p.ph) {
            const size_t at = r.find(ph);
            if (at == std::wstring::npos || r.find(ph, at + 1) != std::wstring::npos) return std::wstring();
            size_t b = at, e = at + ph.size();
            // No spaces between a Chinese name and Chinese text.
            if (tgt != Lang::En) {
                if (b > 0 && r[b - 1] == L' ' && isCjk(ent.front())) --b;
                if (e < r.size() && r[e] == L' ' && isCjk(ent.back())) ++e;
            }
            r.replace(b, e - b, ent);
        }
        // Words the English pivot flattens (帰国 "go home" -> 回家): fixed
        // where the source has the word and the answer the flat reading.
        if (tgt == Lang::ZhHant && src == Lang::Ja) {
            static const struct { const wchar_t *src, *flat, *right; } kSense[] = {
                {L"帰国", L"回家", L"回國"}, {L"帰国", L"回到家", L"回國"}, {L"帰国", L"返回家園", L"回國"}};
            for (const auto& k : kSense)
                if (p.plain.find(k.src) != std::wstring::npos)
                    for (size_t at = r.find(k.flat); at != std::wstring::npos; at = r.find(k.flat, at + 1)) r.replace(at, wcslen(k.flat), k.right);
        }
        return r;
    };
    // Checks (negation, numbers, brackets) and the escalation of what fails
    // (pm/translator.h), each engine piece once.
    std::vector<TrHypothesis> hyp(engineIn.size());
    std::vector<std::vector<std::string>> firstFlags(engineIn.size());
    std::vector<char> done(engineIn.size(), 0);
    // Step 2 (clauses, scope rewrite) of every piece that fails a check, as
    // one engine call: the per-piece calls took 2.4 s on a dense page.
    if (esc && fixes && (tgt == Lang::ZhHant || tgt == Lang::En)) {
        std::vector<TrRequest> failing;
        std::vector<char> seen(engineIn.size(), 0);
        for (const auto& parts : plan)
            for (const auto& p : parts) {
                if (p.eng == SIZE_MAX || seen[p.eng]) continue;
                seen[p.eng] = 1;
                if (checkTranslation(p.plain, restore(p, res[p.eng]), src, tgt).empty()) continue;
                TrRequest rq;
                rq.text = p.plain;
                rq.src = src;
                rq.tgt = tgt;
                failing.push_back(std::move(rq));
            }
        if (failing.size() > 1) esc->prefetchClauses(failing);
    }
    // Online translation (pm/online_translate.h; the mode read once per
    // picture by Escalator::resetBudget), one batch call per call here:
    //   All       every piece, taken where it passes the checks (Bergamot for the rest);
    //   Escalate  the pieces whose Bergamot result fails a check, so that the
    //             escalator's step 4 for each of them is a cache hit.
    // The requests are Escalator::request() - what step 4 sends.
    std::vector<std::wstring> onlineText(engineIn.size());
    if (esc && fixes && esc->onlineMode() != Escalator::OnlineMode::Off) {
        const bool all = esc->onlineMode() == Escalator::OnlineMode::All;
        std::vector<TrRequest> rqs;
        std::vector<size_t> slot;
        std::vector<char> seen(engineIn.size(), 0);
        for (const auto& parts : plan)
            for (const auto& p : parts) {
                if (p.eng == SIZE_MAX || seen[p.eng]) continue;
                seen[p.eng] = 1;
                if (!all && checkTranslation(p.plain, restore(p, res[p.eng]), src, tgt).empty()) continue;
                rqs.push_back(Escalator::request(p.plain, src, tgt));
                slot.push_back(p.eng);
            }
        std::vector<TrHypothesis> got;
        if (esc->prefetchOnline(rqs, &got) || all)
            for (size_t k = 0; k < slot.size() && k < got.size(); ++k) onlineText[slot[k]] = got[k].text;
    }
    for (size_t i = 0; i < in.size(); ++i)
        for (auto& p : plan[i]) {
            if (p.eng == SIZE_MAX) continue;
            TrHypothesis& h = hyp[p.eng];
            if (!done[p.eng]) {
                done[p.eng] = 1;
                h.text = restore(p, res[p.eng]);
                h.engine = "bergamot";
                h.step = 1;
                if (esc && fixes && esc->onlineMode() == Escalator::OnlineMode::All && !onlineText[p.eng].empty() &&
                    checkTranslation(p.plain, onlineText[p.eng], src, tgt).empty()) {
                    h.text = onlineText[p.eng];  // online first (mode All), it passed the checks
                    h.engine = "online";
                    h.step = 4;
                    esc->remember(src, tgt, p.plain, h);
                } else if (esc && fixes) {
                    TrRequest rq;
                    rq.text = p.plain;
                    rq.src = src;
                    rq.tgt = tgt;
                    const double e0 = msNow();
                    if (!esc->escalate(rq, p.plain, h) || h.step > 1) ++escN;
                    escMs += msNow() - e0;
                    esc->remember(src, tgt, p.plain, h);
                    // LLM candidates (time-budgeted, Escalator::runPending): the
                    // checks still fail, or Bergamot spelled an unknown word out.
                    const EscalationConfig& ec = esc->config();
                    if (ec.deferLlm && ec.localLlm && (src == Lang::Ja || src == Lang::Ko)) {
                        // prio 1 (EscalationConfig::llmShortItems, off): short items (<= 10
                        // characters) - +8 units on eval_web's perfect lines but -1 / -2 on
                        // OCR lines (fragments), so not by default (route_eval.py).
                        size_t chars = 0;
                        for (wchar_t c : p.plain) chars += !iswspace(c);
                        // An ingredient item in katakana the engine translated (no glossary
                        // term: マンゴチャツネ -> 「芒果聊天」, a transliteration or an odd word).
                        size_t kata = 0;
                        for (wchar_t c : p.plain) kata += (c >= 0x3041 && c <= 0x30FA) || c == 0x30FC;  // kana (すりごま too)
                        const bool itemKana = p.item && kata >= 3 && kata * 10 >= chars * 6;
                        // Casual Japanese (a tweet: …ってツイートで…, 何コレ…, じゃん, w):
                        // the pivot loses its structure (台湾行くって… -> 「我將前往臺灣」).
                        static const std::wregex casual(L"(って|じゃん|コレ|ヤバ|マジ|ww|草$|かも|よね|だね|けど)");
                        const bool casualJa = src == Lang::Ja && chars >= 8 && std::regex_search(p.plain, casual);
                        const int prio = h.uncertain ? 3 : (translitRun(h.text) >= 3 || itemKana) ? 2
                                       : ((ec.llmShortItems && chars <= 10) || casualJa) ? 1 : 0;
                        if (prio) {
                            Escalator::Pending pd;
                            pd.src = src, pd.tgt = tgt, pd.plain = p.plain, pd.best = h.text, pd.prio = prio;
                            for (const auto& ph : p.ph) pd.keep.push_back(ph.second);
                            esc->queue(std::move(pd));
                        }
                    }
                    if (h.step > 1) firstFlags[p.eng] = checkTranslation(p.plain, restore(p, res[p.eng]), src, tgt);
                }
            }
            if (info) {
                TextInfo& ti = (*info)[i];
                ti.step = std::max(ti.step, h.step);
                ti.online |= h.engine.rfind("online", 0) == 0;
                ti.flags.insert(ti.flags.end(), firstFlags[p.eng].begin(), firstFlags[p.eng].end());
                if (h.uncertain) {
                    ti.uncertain = true;
                    if (!h.verified.empty()) ti.verified += (ti.verified.empty() ? L"" : L"\n") + h.verified;
                }
            }
        }
    for (size_t i = 0; i < in.size(); ++i) {
        std::wstring o;
        for (const auto& p : plan[i]) o += p.eng == SIZE_MAX ? p.lit : hyp[p.eng].text;
        while (!o.empty() && (isSpace(o.back()) || o.back() == L'\n')) o.pop_back();
        out[i] = std::move(o);
    }
    if (prof)
        std::fprintf(stderr, "[tr] %zu texts, %zu engine pieces (%zu chars): engine %.0f ms, %zu retried %.0f ms, %d escalated %.0f ms\n",
                     in.size(), engineIn.size() - retryIn.size(), [&] { size_t c = 0; for (const auto& s : engineIn) c += s.size(); return c; }(),
                     pt1 - pt0, retryIn.size(), pt2 - pt1, escN, escMs);
    if (info)
        for (size_t i = 0; i < in.size(); ++i)
            for (const auto& p : plan[i])
                if (p.eng != SIZE_MAX) (*info)[i].engines += ((*info)[i].engines.empty() ? "" : "+") + hyp[p.eng].engine;
    // Tests: the LLM's translation of every engine piece (routing data).
    if (info && esc && fixes && esc->config().collectAlt && (src == Lang::Ja || src == Lang::Ko) && !engineIn.empty()) {
        std::vector<TrRequest> rqs;
        std::vector<size_t> slot(engineIn.size(), SIZE_MAX);
        for (const auto& parts : plan)
            for (const auto& p : parts)
                if (p.eng != SIZE_MAX && slot[p.eng] == SIZE_MAX) {
                    slot[p.eng] = rqs.size();
                    rqs.push_back(Escalator::request(p.plain, src, tgt));
                }
        std::vector<TrHypothesis> alt;
        if (esc->llmTranslate(rqs, alt))
            for (size_t i = 0; i < in.size(); ++i) {
                std::wstring o;
                double ms = 0;
                for (const auto& p : plan[i]) {
                    if (p.eng == SIZE_MAX) {
                        o += p.lit;
                        continue;
                    }
                    const TrHypothesis& a = alt[slot[p.eng]];
                    o += a.text.empty() ? hyp[p.eng].text : a.text;
                    ms += a.ms;
                    for (const auto& f : checkTranslation(p.plain, a.text, src, tgt)) (*info)[i].altFlags += f + " ";
                }
                while (!o.empty() && (isSpace(o.back()) || o.back() == L'\n')) o.pop_back();
                (*info)[i].alt = std::move(o);
                (*info)[i].altMs = ms;
            }
    }
    return true;
}

}  // namespace pm::translate
