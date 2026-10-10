// 自在投影 app: menus/menu_view_translate.cpp — menus（組選單項目）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "share/share.h"
#include "translate/translate.h"
#include "connect/connect.h"
#include "view/view.h"
#include "menus/menus.h"

namespace pm_app {

std::vector<MenuItem> viewItems() {
    std::vector<MenuItem> v;
    v.push_back(MenuItem::command(CmdRotateRight, tr(S::MenuRotateRight), kIcoRotate, L"Ctrl+→"));
    v.push_back(MenuItem::command(CmdRotateLeft, tr(S::MenuRotateLeft), kIcoRotate, L"Ctrl+←"));
    v.push_back(checkItem(CmdMirror, tr(S::MenuFlip), kIcoFlip, g.settings.mirrored, L"Ctrl+H"));
    MenuItem reset = MenuItem::command(CmdResetView, tr(S::MenuResetView), kIcoReset, L"Ctrl+0");
    reset.enabled = g.settings.rotation != 0 || g.settings.mirrored || g.window->viewState().zoom > 1.01f;
    v.push_back(std::move(reset));
    if (g.settings.rotation)
        v.push_back(MenuItem::note(fmt(S::MenuRotatedNote, {std::to_wstring(g.settings.rotation * 90)})));
    v.push_back(MenuItem::separator());
    v.push_back(checkItem(CmdDeviceFrame, tr(S::MenuDeviceFrame), kIcoPhone, g.settings.deviceFrame, L"Ctrl+F"));
    return v;
}

std::vector<MenuItem> themeItems() {
    std::vector<MenuItem> v;
    for (int t = 0; t < 4; ++t) {
        MenuItem m = MenuItem::command(CmdTheme0 + t, tr(kThemeNames[t]));
        m.radio = true;
        m.checked = g.settings.theme == t;
        const std::array<uint32_t, 3> sw = pm::VideoWindow::themeSwatch(kThemes[t]);
        m.hasSwatch = true;
        for (int i = 0; i < 3; ++i) m.swatch[i] = sw[i];
        v.push_back(std::move(m));
    }
    return v;
}
MenuItem themeItem() {
    const int t = g.settings.theme % 4;
    return valueSubmenu(tr(S::MenuTheme), kIcoTheme, themeItems(), tr(kThemeNames[t]));
}

// 放大鏡 ▸ 放大 / 縮小 / 還原 1×, colours (radio), 凍結畫面.
std::vector<MenuItem> magnifierItems() {
    const bool on = viewAvailable();
    const pm::VideoWindow::ViewState vs = g.window->viewState();
    std::vector<MenuItem> v;
    if (!on) v.push_back(MenuItem::note(tr(S::MenuNeedPicture)));
    MenuItem in = MenuItem::command(CmdZoomIn, tr(S::MenuZoomIn), kIcoZoomIn, L"Ctrl+=");
    in.enabled = on && vs.zoom < 7.99f;
    v.push_back(std::move(in));
    MenuItem out = MenuItem::command(CmdZoomOut, tr(S::MenuZoomOut), kIcoZoomOut, L"Ctrl+-");
    out.enabled = on && vs.zoom > 1.01f;
    v.push_back(std::move(out));
    MenuItem reset = MenuItem::command(CmdZoomReset, tr(S::MenuZoomReset), kIcoReset, L"Ctrl+Shift+0");
    reset.enabled = on && vs.zoom > 1.01f;
    v.push_back(std::move(reset));
    if (on && vs.zoom > 1.01f) v.push_back(MenuItem::note(fmt(S::MagZoomFmt, {zoomText(vs.zoom).substr(0, zoomText(vs.zoom).size() - 1)})));
    v.push_back(MenuItem::separator());
    v.push_back(MenuItem::caption(tr(S::MenuFilter)));
    for (int f = 0; f < 5; ++f) {
        // Ctrl+K cycles the colours: named in the caption, not on 原色's row (0.7.2).
        MenuItem m = MenuItem::command(CmdFilter0 + f, tr(kFilterNames[f]));
        m.radio = true;
        m.checked = g.settings.filter == f;
        m.enabled = on;
        v.push_back(std::move(m));
    }
    v.push_back(MenuItem::separator());
    MenuItem fr = checkItem(CmdFreeze, tr(S::MenuFreeze), kIcoFreeze, on && vs.frozen, L"Ctrl+P");
    fr.enabled = on;
    v.push_back(std::move(fr));
    return v;
}

// 管理翻譯模型 ▸ one row per language the user reads FROM, for the current
// target (「日文 → 繁體中文　已下載 · 104 MB」: the total of the files it needs,
// pivot pairs included), 文字辨識（PaddleOCR）, 開啟模型資料夾, 刪除全部. A
// downloaded row is deleted after a confirm, keeping files that another
// downloaded language still needs.
std::vector<MenuItem> modelItems() {
    std::vector<MenuItem> v;
    v.push_back(MenuItem::note(tr(S::TrModelsNote)));
    bool any = false;
    for (int i = 0; i < kModelPairCount; ++i) any |= dirBytes(modelPairDir(i)) > 0;
    for (int k = 0; k < 5; ++k) {
        if (kModelSources[k] == translateTarget()) continue;
        bool ok = false;
        const std::vector<int> pairs = sourcePairs(kModelSources[k], &ok);
        if (!ok) continue;
        std::wstring right;
        bool ready = false;
        if (pairs.empty()) {
            right = tr(S::TrModelNoDownload);  // 簡體中文 → 繁體中文: converted without a model
        } else {
            ready = sourceDownloaded(kModelSources[k]);
            std::vector<std::string> names;
            unsigned long long have = 0;
            for (int i : pairs) names.push_back(kModelPairs[i]), have += dirBytes(modelPairDir(i));
            right = ready ? fmt(S::TrModelReady, {mbText(have)})
                          : fmt(S::TrModelMissing, {mbText(pm::translate::ModelStore::missingBytes(names))});
        }
        const SharedModels sh = sourceSharedModels(k);
        if (ready && !sh.pairs.empty()) right += fmt(S::TrModelSharedBy, {sh.users});  // （日文也會用到）
        MenuItem m = MenuItem::command(CmdTrDeleteLang0 + k, modelSourceLabel(k), ready ? kIcoDelete : 0, right);
        m.enabled = !sourceDeletablePairs(k).empty() || !sh.pairs.empty();
        v.push_back(std::move(m));
    }
    {  // 文字辨識（PaddleOCR）
        const int i = modelPairIndex("ocr");
        const unsigned long long have = dirBytes(modelPairDir(i));
        const bool ready = pm::translate::ModelStore::installed(kModelPairs[i]);
        const std::wstring right = ready ? fmt(S::TrModelReady, {mbText(have)})
                                         : fmt(S::TrModelMissing, {mbText(pm::translate::ModelStore::missingBytes({kModelPairs[i]}))});
        MenuItem m = MenuItem::command(CmdTrDelete0 + i, pairLabel(i), ready ? kIcoDelete : 0, right);
        m.enabled = have > 0;
        v.push_back(std::move(m));
    }
    v.push_back(MenuItem::separator());
    v.push_back(MenuItem::command(CmdTrModelsFolder, tr(S::TrModelsFolder), kIcoFolder));
    MenuItem all = MenuItem::command(CmdTrDeleteAll, tr(S::TrModelsDeleteAll), kIcoDelete);
    all.enabled = any;
    v.push_back(std::move(all));
    return v;
}

// 翻譯 ▸ [手機畫面出現後才能使用], 翻譯整個畫面, 框選翻譯, 顯示原文 ✓
// [先按「翻譯整個畫面」], 即時翻譯 ✓, [關閉翻譯] | 翻成 ▸ (right: the target),
// 顯示方式 ▸ (right: the layout), 進階翻譯設定 ▸ (0.7.8: the everyday choices
// one level up, the optional engines and models out of the way).
std::vector<MenuItem> translateItems() {
    const bool on = viewAvailable() && g.translator;
    const bool active = on && g.translator->active();
    std::vector<MenuItem> v;
    if (!viewAvailable()) v.push_back(MenuItem::note(tr(S::MenuNeedPicture)));
    MenuItem screen = MenuItem::command(CmdTranslateScreen, tr(S::MenuTrScreen), kIcoTranslate, active ? L"" : L"Ctrl+L");
    screen.enabled = on && !g.translator->busy();
    v.push_back(std::move(screen));
    MenuItem region = MenuItem::command(CmdTranslateRegion, tr(S::MenuTrRegion), kIcoRegion, L"Ctrl+Shift+L");
    region.enabled = on && !g.translator->busy();
    v.push_back(std::move(region));
    MenuItem orig = checkItem(CmdTranslateOriginal, tr(S::MenuTranslateOriginal), kIcoOriginal,
                              active && g.translator->showOriginal(), L"Ctrl+O");
    orig.enabled = active;
    v.push_back(std::move(orig));
    if (on && !active) v.push_back(MenuItem::note(tr(S::MenuTrNeedTranslate)));  // why 顯示原文 is grey
    MenuItem live = checkItem(CmdTranslateLive, tr(S::MenuTrLive), kIcoLive,
                              on && g.translator->live());
    live.enabled = on;
    v.push_back(std::move(live));
    if (active) {
        MenuItem close = MenuItem::command(CmdTranslateClose, tr(S::MenuTranslateClose), kIcoExit, L"Ctrl+L");
        close.bold = true;
        v.push_back(std::move(close));
    }
    v.push_back(MenuItem::separator());
    v.push_back(valueSubmenu(tr(S::MenuTrTargetRow), kIcoLanguage, translateTargetItems(),
                             pm::translate::langName(translateTarget())));
    const S layoutNames[3] = {S::MenuTrLayoutAuto, S::MenuTrLayoutInPlace, S::MenuTrLayoutList};
    v.push_back(valueSubmenu(tr(S::MenuTrLayoutRow), kIcoDisplay, translateLayoutItems(),
                             shortLabel(tr(layoutNames[g.settings.translateLayout % 3]))));
    v.push_back(MenuItem::submenu(tr(S::MenuTrAdvanced), kIcoSettings, translateAdvancedItems()));
    return v;
}

// 翻譯 ▸ 翻成 ▸ (radio). Targets with an offline model from English (other
// sources pivot via English): 繁體中文, English, 日本語, 한국어 — each named in
// its own language.
std::vector<MenuItem> translateTargetItems() {
    std::vector<MenuItem> v;
    const pm::translate::Lang target = translateTarget();
    const struct {
        UINT id;
        S name;
        pm::translate::Lang lang;
    } targets[4] = {{CmdTrTargetZh, S::LangZh, pm::translate::Lang::ZhHant},
                    {CmdTrTargetEn, S::LangEn, pm::translate::Lang::En},
                    {CmdTrTargetJa, S::LangJa, pm::translate::Lang::Ja},
                    {CmdTrTargetKo, S::LangKo, pm::translate::Lang::Ko}};
    for (const auto& t : targets) {
        MenuItem m = MenuItem::command(t.id, tr(t.name));
        m.radio = true;
        m.checked = target == t.lang;
        v.push_back(std::move(m));
    }
    return v;
}

// 翻譯 ▸ 顯示方式 ▸ 自動 / 原位顯示 / 清單顯示 (radio), 深色方框 ✓ (0.7.1).
std::vector<MenuItem> translateLayoutItems() {
    std::vector<MenuItem> v;
    const UINT layouts[3] = {CmdTrLayoutAuto, CmdTrLayoutInPlace, CmdTrLayoutList};
    const S layoutNames[3] = {S::MenuTrLayoutAuto, S::MenuTrLayoutInPlace, S::MenuTrLayoutList};
    for (int i = 0; i < 3; ++i) {
        MenuItem m = MenuItem::command(layouts[i], tr(layoutNames[i]));
        m.radio = true;
        m.checked = g.settings.translateLayout == i;
        v.push_back(std::move(m));
    }
    v.push_back(MenuItem::separator());
    v.push_back(checkItem(CmdTrDarkCards, tr(S::MenuTrDarkCards), 0, g.settings.translateDark));
    return v;
}

// 翻譯 ▸ 進階翻譯設定 ▸ (0.7.8; was part of 翻譯設定 ▸): a note that none of
// it is needed, 本機 AI 翻譯…, 線上翻譯（選用）…, 管理翻譯模型 ▸.
std::vector<MenuItem> translateAdvancedItems() {
    std::vector<MenuItem> v;
    v.push_back(MenuItem::note(tr(S::MenuTrAdvancedNote)));
    if (pm::ui::trset::available()) {  // the optional engines' settings (0.7.4)
        v.push_back(checkItem(CmdTrLocalAi, tr(S::MenuTrLocalAi), kIcoLocalAi, pm::ui::trset::localAiOn()));
        v.push_back(checkItem(CmdTrOnline, tr(S::MenuTrOnline), kIcoOnline, pm::ui::trset::onlineEnabled()));
        v.push_back(MenuItem::separator());
    }
    v.push_back(MenuItem::submenu(tr(S::MenuTrModels), kIcoModels, modelItems()));
    return v;
}

}  // namespace pm_app
