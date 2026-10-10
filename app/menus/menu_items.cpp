// 自在投影 app: menus/menu_items.cpp — menus（組選單項目）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "share/share.h"
#include "translate/translate.h"
#include "connect/connect.h"
#include "view/view.h"
#include "menus/menus.h"

namespace pm_app {

MenuItem checkItem(UINT id, const wchar_t* text, wchar_t icon, bool on, std::wstring right) {
    MenuItem m = MenuItem::command(id, text, icon, std::move(right));
    m.checkable = true;
    m.checked = on;
    return m;
}
void appendNotes(std::vector<MenuItem>& v, const std::wstring& text, size_t width) {
    const size_t first = v.size();
    appendNotesRaw(v, text, width);
    // A stub last row (「投影。」, one word) joins the row above (0.7.2).
    if (v.size() >= first + 2) {
        const std::wstring& last = v.back().text;
        const bool spaced = pm::i18n::en() || pm::i18n::lang() == pm::i18n::Lang::Ko;
        if (last.size() <= (spaced ? 12u : 4u)) {
            const std::wstring& prev = v[v.size() - 2].text;
            const bool latinEdge = (!prev.empty() && prev.back() < 0x80 && iswalnum(prev.back())) ||
                                   (last.front() < 0x80 && iswalnum(last.front()));
            std::wstring joined = prev + (spaced || latinEdge ? L" " : L"") + last;
            v.pop_back();
            v.back().text = std::move(joined);
        }
    }
}
void appendNotesRaw(std::vector<MenuItem>& v, const std::wstring& text, size_t width) {
    if (pm::i18n::en() || pm::i18n::lang() == pm::i18n::Lang::Ko) {
        const size_t maxLine = pm::i18n::en() ? 52 : 34;
        std::wstring line, word;
        auto flush = [&]() {
            if (!line.empty()) v.push_back(MenuItem::note(line));
            line.clear();
        };
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i < text.size() && text[i] != L' ') {
                word += text[i];
                continue;
            }
            if (!line.empty() && line.size() + 1 + word.size() > maxLine) flush();
            if (!word.empty()) line += (line.empty() ? L"" : L" ") + word;
            word.clear();
        }
        flush();
        return;
    }
    if (pm::i18n::lang() == pm::i18n::Lang::Ja) width = width * 3 / 2;  // kana run longer: break at 、。」 instead
    auto punct = [](wchar_t c) {
        return c == L'，' || c == L'：' || c == L'；' || c == L'。' || c == L'、' || c == L'）' || c == L'」';
    };  // ，：；。、）」
    const bool ja = pm::i18n::lang() == pm::i18n::Lang::Ja;
    auto emit = [&](std::wstring s) {
        while (!s.empty() && s.front() == L' ') s.erase(s.begin());
        while (!s.empty() && s.back() == L' ') s.pop_back();
        if (!s.empty()) v.push_back(MenuItem::note(s));
    };
    std::wstring line;
    size_t lastSpace = std::wstring::npos;  // in `line`: a break between two Latin words
    for (size_t i = 0; i < text.size(); ++i) {
        line += text[i];
        // 日本語 (0.7.2): also after a particle (「…」を / …には), a phrase end,
        // instead of in the middle of インストール|しました.
        const bool soft = punct(text[i]) || text[i] == L'→' ||
                          (ja && std::wstring(L"をがはにで").find(text[i]) != std::wstring::npos);
        if (text[i] == L' ') lastSpace = line.size();
        // Never start a row with punctuation (日本語: nor with a particle such as を / が).
        const bool nextPunct = i + 1 < text.size() &&
                               (punct(text[i + 1]) || (ja && std::wstring(L"をがはにでともの").find(text[i + 1]) != std::wstring::npos));
        if (nextPunct) continue;
        if (soft && line.size() >= width / 2) {
            emit(line);
            line.clear();
            lastSpace = std::wstring::npos;
        } else if (line.size() >= width) {
            // Full row: not inside a Latin word (「Andr|oid」) when a space is near (0.7.2).
            const bool inWord = i + 1 < text.size() && iswalnum(text[i]) && iswalnum(text[i + 1]) && text[i] < 0x80;
            if (inWord && lastSpace != std::wstring::npos && lastSpace >= width / 3) {
                emit(line.substr(0, lastSpace));
                line.erase(0, lastSpace);
            } else {
                emit(line);
                line.clear();
            }
            lastSpace = std::wstring::npos;
        }
    }
    while (!line.empty() && line.front() == L' ') line.erase(line.begin());
    if (!line.empty()) v.push_back(MenuItem::note(line));
}

// A submenu row that shows its current value on the right (畫質　高).
MenuItem valueSubmenu(const wchar_t* text, wchar_t icon, std::vector<MenuItem> items, std::wstring value) {
    MenuItem m = MenuItem::submenu(text, icon, std::move(items));
    m.right = std::move(value);
    return m;
}

// 關閉視窗時 ▸ 每次詢問 / 背景待命 (0.7.8; was 縮到右下角) / 結束程式 (radio).
std::vector<MenuItem> closeItems() {
    std::vector<MenuItem> v;
    for (int c = 0; c < 3; ++c) {
        MenuItem m = MenuItem::command(CmdCloseAsk + c, tr(kCloseActNames[c]));
        m.radio = true;
        m.checked = g.settings.closeButton == c;
        v.push_back(std::move(m));
    }
    return v;
}

// 畫質 ▸ 標準 / 高 · 建議 / 最高 (radio; without HEVC only 標準, and a note why).
std::vector<MenuItem> qualityItems() {
    std::vector<MenuItem> v;
    const wchar_t* names[3] = {tr(S::QualityStd), tr(S::QualityHigh), tr(S::QualityMax)};
    const wchar_t* sizes[3] = {L"1920×1080", L"2560×1440", L"3840×2160"};
    for (int q = 0; q < 3; ++q) {
        MenuItem m = MenuItem::command(CmdQualityStandard + q, names[q], 0, sizes[q]);
        m.radio = true;
        m.checked = effectiveQuality() == q;
        m.enabled = q == 0 || g.hevc;
        v.push_back(std::move(m));
    }
    if (!g.hevc) v.push_back(MenuItem::note(tr(S::QualityHevcNote)));
    v.push_back(MenuItem::note(tr(S::QualityNote)));  // 0.7.8: what the choice trades off
    return v;
}

// 新手機連線時 ▸ 接手 / 保持目前 (radio).
std::vector<MenuItem> takeoverItems() {
    std::vector<MenuItem> v;
    MenuItem takeNew = MenuItem::command(CmdTakeoverNew, tr(S::TakeoverNew), 0, tr(S::TakeoverNewNote));
    takeNew.radio = true;
    takeNew.checked = !g.settings.takeoverKeep;
    v.push_back(std::move(takeNew));
    MenuItem keep = MenuItem::command(CmdTakeoverKeep, tr(S::TakeoverKeep), 0, tr(S::TakeoverKeepNote));
    keep.radio = true;
    keep.checked = g.settings.takeoverKeep;
    v.push_back(std::move(keep));
    return v;
}

// 0.7.8 UX (p7): 右鍵行為 ▸ 返回（預設）/ 開啟選單 (radio) + a note that
// Shift+right click always opens the menu. Android pictures only.
MenuItem rightClickItem() {
    std::vector<MenuItem> sub;
    MenuItem back = MenuItem::command(CmdRightClickBack, tr(S::RightClickBack));
    back.radio = true;
    back.checked = !g.settings.rightClickMenu;
    sub.push_back(std::move(back));
    MenuItem menu = MenuItem::command(CmdRightClickMenu, tr(S::RightClickMenu));
    menu.radio = true;
    menu.checked = g.settings.rightClickMenu;
    sub.push_back(std::move(menu));
    sub.push_back(MenuItem::note(tr(S::RightClickNote)));
    return valueSubmenu(tr(S::MenuRightClick), kIcoBack, std::move(sub),
                        shortLabel(tr(g.settings.rightClickMenu ? S::RightClickMenu : S::RightClickBack)));
}

// 設定 ▸ (both menus; set-and-forget): 開機自動啟動 ✓, 按 X 時 ▸, 連線需要
// PIN 碼 ✓, 畫質 ▸, 新手機連線時 ▸, 主題 ▸, 語言 ▸ — each submenu row shows
// its current value. 影音同步 is deliberately not offered (see docs/app.md).
std::vector<MenuItem> settingsItems() {
    std::vector<MenuItem> v;
    if (applyPendingNow()) {  // 0.7.8 UX (p9): changes held for the session's end
        v.push_back(MenuItem::note(tr(S::ApplyPendingNote)));
        v.push_back(MenuItem::command(CmdApplyNow, tr(S::ApplyNow), kIcoSync));
        v.push_back(MenuItem::separator());
    }
    // 0.7.8: a plain one-line note under the two switches (what they do).
    v.push_back(checkItem(CmdAutostart, tr(S::OptAutostart), kIcoPower, g.autostart));
    v.push_back(MenuItem::note(tr(S::OptAutostartNote)));
    v.push_back(valueSubmenu(tr(S::MenuCloseAction), kIcoExit, closeItems(),
                             tr(kCloseActNames[g.settings.closeButton % 3])));
    v.push_back(checkItem(CmdPin, tr(S::OptPin), kIcoLock, g.settings.requirePin));
    v.push_back(MenuItem::note(tr(S::OptPinNote)));
    v.push_back(MenuItem::separator());
    const S qualityNames[3] = {S::QualityStd, S::QualityHigh, S::QualityMax};
    v.push_back(valueSubmenu(tr(S::MenuQuality), kIcoDisplay, qualityItems(),
                             shortLabel(tr(qualityNames[effectiveQuality() % 3]))));
    v.push_back(valueSubmenu(tr(S::MenuTakeover), kIcoFlip, takeoverItems(),
                             tr(g.settings.takeoverKeep ? S::TakeoverKeep : S::TakeoverNew)));
    // 0.7.8 UX (p7); --dev DevCommand 950 bit 0 (adb ok) shows it too, for screenshots without adb
    if (g.androidOk || (g.testWays >= 0 && (g.testWays & 1))) v.push_back(rightClickItem());
    v.push_back(MenuItem::separator());
    v.push_back(themeItem());
    v.push_back(languageItem());
    if (audioAdvertItemVisible()) {
        v.push_back(MenuItem::separator());
        v.push_back(checkItem(CmdAudioAdvertAB, L"實驗：只提供螢幕鏡像（不當 AirPlay 喇叭）", kIcoFlip,
                              g.settings.advertiseAudio != 1,
                              g.settings.advertiseAudio == 2 ? L"=2" : L""));
    }
    return v;
}

// Miracast that cannot work on this PC: one row 「Miracast：這台電腦不支援 ▸」;
// the explanation and 如何啟用 Miracast open beside it on hover (never a
// block of wrapped notes in the menu itself).
MenuItem miracastUnavailableItem() {
    std::vector<MenuItem> sub;
    appendNotes(sub, g.miracastReason.empty() ? std::wstring(tr(S::MiracastNoPc)) : moduleText(g.miracastReason), 30);
    sub.push_back(MenuItem::separator());
    sub.push_back(MenuItem::command(CmdMiracastHelp, tr(S::MenuMiracastHelp), kIcoHelp));
    return MenuItem::submenu(tr(S::MenuMiracastNo), kIcoCast, std::move(sub));
}

// 連接手機 ▸ (0.7.8: was 「Android ▸」) 怎麼連線？ | 連接 Android（掃 QR）,
// 自動連線已配對的 Android ✓, 接受 Android 投放（Miracast）✓ — or
// 「Miracast：這台電腦不支援 ▸」. Rows that cannot do anything here are left
// out (no adb: only the greyed 連接 row says so).
std::vector<MenuItem> androidItems() {
    std::vector<MenuItem> v;
    v.push_back(MenuItem::command(CmdHowTo, tr(S::ActHowTo), kIcoHelp));
    v.push_back(MenuItem::separator());
    v.push_back(androidPairItem());
    if (g.androidOk) v.push_back(checkItem(CmdAndroidAuto, tr(S::OptAndroidAuto), kIcoConnect, g.settings.androidAuto));
    if (g.miracastUnsupported) {
        v.push_back(MenuItem::separator());
        v.push_back(miracastUnavailableItem());
    } else if (g.miracast) {
        v.push_back(MenuItem::separator());
        v.push_back(checkItem(CmdMiracast, tr(S::OptMiracast), kIcoCast, g.settings.miracast));
        if (g.settings.miracast && !g.miracastReason.empty()) appendNotes(v, moduleText(g.miracastReason), 30);
    }
    return v;
}

// 開始錄影 / 停止錄影 (while a picture is shown, or to stop; else greyed
// with 「手機連上後可用」 instead of the shortcut, 0.7.8).
MenuItem recordItem() {
    const bool rec = recording();
    MenuItem m = MenuItem::command(CmdRecord, tr(rec ? S::MenuStopRec : S::MenuStartRec), rec ? kIcoStop : kIcoRecord,
                                   L"Ctrl+R");
    m.enabled = rec || pictureShowing();
    if (!m.enabled) m.right = tr(S::MenuNeedPhone);
    m.bold = rec;
    return m;
}

// 傳到手機 ▸ (both menus; right: 「N 個未傳」): 傳送未傳的截圖／錄影 (only
// with unsent captures), 把最後一張截圖／最後一段錄影傳到手機 (only when there
// is one), 傳到手機…, 截圖／錄影後自動傳到手機 ✓.
MenuItem shareItem() {
    const size_t unsent = unsentCount();
    std::vector<MenuItem> sub;
    if (unsent)
        sub.push_back(MenuItem::command(CmdShareLast, tr(S::MenuShareUnsent), kIcoShare,
                                        fmt(S::ShareUnsentCount, {std::to_wstring(unsent)})));
    if (!lastShotFile().empty()) sub.push_back(MenuItem::command(CmdShareLastShot, tr(S::MenuShareLastShot), kIcoShare));
    if (!lastRecFile().empty()) sub.push_back(MenuItem::command(CmdShareLastRec, tr(S::MenuShareLastRec), kIcoShare));
    sub.push_back(MenuItem::command(CmdSharePick, tr(S::MenuSharePick), kIcoSharePick));
    sub.push_back(MenuItem::separator());
    sub.push_back(checkItem(CmdShareAuto, tr(S::MenuShareAuto), kIcoShare, g.settings.autoShare));
    return valueSubmenu(tr(S::ShareChip), kIcoShare, std::move(sub),
                        unsent ? fmt(S::ShareUnsentCount, {std::to_wstring(unsent)}) : std::wstring());
}

// 連接 Android（掃 QR） (greyed without the bundled adb / scrcpy-server).
MenuItem androidPairItem() {
    MenuItem m = MenuItem::command(CmdAndroidPair, tr(S::MenuAndroidPair), kIcoQr);
    m.enabled = g.androidOk;
    return m;
}

// 語言 / Language submenu (both menus): 自動 / 繁體中文 / English / 日本語 /
// 한국어 (each language's name in that language).
MenuItem languageItem() {
    std::vector<MenuItem> sub;
    const S names[5] = {S::LangAuto, S::LangZh, S::LangEn, S::LangJa, S::LangKo};
    const UINT ids[5] = {CmdLangAuto, CmdLangZh, CmdLangEn, CmdLangJa, CmdLangKo};
    for (int i = 0; i < 5; ++i) {
        MenuItem m = MenuItem::command(ids[i], tr(names[i]));
        m.radio = true;
        m.checked = g.settings.language == i;
        sub.push_back(std::move(m));
    }
    return valueSubmenu(tr(S::MenuLanguage), kIcoLanguage, std::move(sub), shortLabel(tr(names[g.settings.language % 5])));
}

MenuItem aboutItem() { return MenuItem::command(CmdAbout, fmt(S::MenuAbout, {tr(S::AppName)}), kIcoInfo); }

// 開啟截圖資料夾 / 開啟錄影資料夾 (both menus; 0.7.8: two rows instead of a
// 資料夾 ▸ submenu of two).
void appendFolderItems(std::vector<MenuItem>& v) {
    v.push_back(MenuItem::command(CmdOpenShots, tr(S::MenuOpenShots), kIcoFolder));
    v.push_back(MenuItem::command(CmdOpenRecordings, tr(S::MenuOpenRecs), kIcoVideoFolder));
}

// 說明 ▸ 怎麼連線？, 快速鍵一覽 F1, 使用教學, 如何啟用 Miracast (only when Miracast has a problem), 檢查更新, 關於.
MenuItem helpItem() {
    std::vector<MenuItem> sub;
    sub.push_back(MenuItem::command(CmdHowTo, tr(S::ActHowTo), kIcoConnect));
    sub.push_back(MenuItem::command(CmdShortcuts, tr(S::MenuShortcuts), kIcoKeyboard, L"F1"));
    sub.push_back(MenuItem::command(CmdTutorial, tr(S::MenuTutorial), kIcoHelp));
    if (g.miracastUnsupported || (g.settings.miracast && !g.miracastReason.empty()))
        sub.push_back(MenuItem::command(CmdMiracastHelp, tr(S::MenuMiracastHelp), kIcoCast));
    sub.push_back(MenuItem::separator());
    sub.push_back(MenuItem::command(CmdCheckUpdate, tr(S::MenuCheckUpdate), kIcoSync));
    sub.push_back(aboutItem());
    return MenuItem::submenu(tr(S::MenuHelpSub), kIcoHelp, std::move(sub));
}

// 截圖 Ctrl+S / 開始錄影 Ctrl+R (both menus). 0.7.8: always listed (so they
// can be found); without a picture greyed with 「手機連上後可用」.
void appendCaptureItems(std::vector<MenuItem>& v) {
    MenuItem shot = MenuItem::command(CmdSnapshot, tr(S::MenuSnapshot), kIcoCamera, L"Ctrl+S");
    if (!pictureShowing()) {
        shot.enabled = false;
        shot.right = tr(S::MenuNeedPhone);
    }
    v.push_back(std::move(shot));
    v.push_back(recordItem());
}

// 放大鏡 ▸ (both menus): greyed with 「手機連上後可用」 without a picture.
MenuItem magnifierItem() {
    MenuItem m = MenuItem::submenu(tr(S::MenuMagnifier), kIcoZoom, magnifierItems());
    if (!viewAvailable()) {
        m.enabled = false;
        m.right = tr(S::MenuNeedPhone);
    }
    return m;
}

// 「更新到 vX.Y.Z」 (+ separator) at the top of both menus when a newer
// version is on offer.
void appendUpdateItem(std::vector<MenuItem>& v) {
    const std::string ver = offerVersion();
    if (ver.empty()) return;
    const bool busy = g.updateDownloading && !offerLocal();
    MenuItem m = MenuItem::command(CmdUpdateDialog, fmt(S::MenuUpdateTo, {toWide(ver)}), kIcoUpdate,
                                   busy ? tr(S::MenuDownloading) : L"");
    m.bold = true;
    m.enabled = !busy;
    v.push_back(std::move(m));
    v.push_back(MenuItem::separator());
}

}  // namespace pm_app
