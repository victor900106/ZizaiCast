// 自在投影 app: translate/translate_actions.cpp — translate（放大鏡、翻譯的 UI 接線）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "translate/translate.h"

namespace pm_app {

// settings translate_last -> the language (Unknown: empty / unknown).
pm::translate::Lang translateLastSource() {
    using L = pm::translate::Lang;
    for (L l : {L::Ja, L::Ko, L::En, L::ZhHans, L::ZhHant})
        if (toWide(g.settings.translateLast) == pm::translate::langTag(l)) return l;
    return L::Unknown;
}
// A phone picture is on screen: for users who have translated before
// (translate_last, or the OCR models downloaded), load the OCR and the last
// used models now, at a low priority, so the first 翻譯整個畫面 does not wait
// for them (0.7.8: the first translation of a session was 0.7-3.8 s, later
// ones ~0.4 s; 0.8.3: a cold OCR alone is 1.2-1.9 s).
void prewarmTranslation() {
    if (!g.translator) return;
    const pm::translate::Lang last = translateLastSource();
    if (g.settings.translateLast.empty() && !pm::translate::PaddleOcr::modelsInstalled()) return;
    g.translator->prewarm(last);
    // 0.8.3: and the local AI model when the last pictures were Japanese /
    // Korean (the only sources it is asked about; nothing when off / not
    // downloaded / short of memory; released again after 5 idle minutes).
    if (last == pm::translate::Lang::Ja || last == pm::translate::Lang::Ko) pm::ui::trset::warmUpLocalAi();
}

void translateCommand(UINT cmd) {
    if (!g.translator) return;
    pm::translate::ScreenTranslator& t = *g.translator;
    // 本機 AI 翻譯 (0.8.3): not loaded here any more.  Loading it at the same
    // moment as the picture's OCR slowed the OCR down (GPU and CPU shared:
    // +0.5 s, +1 s while Vulkan builds its pipelines after an install) and a
    // first picture never had time left for it anyway; the translator loads
    // it in the background right after a picture that wanted it (Japanese /
    // Korean pieces), and prewarmTranslation() when a phone connects.
    switch (cmd) {
    case CmdTranslateToggle:
        if (t.active()) {
            t.close();
            g.log->write("info", "translate closed (Ctrl+L / toolbar)");
            returnToLive("closed");
            break;
        }
        [[fallthrough]];
    case CmdTranslateScreen:
        if (!needPicture()) break;
        t.setTarget(translateTarget());
        g.log->write("info", "translate screen");
        t.translateScreen();
        break;
    case CmdTranslateRegion:
        if (!needPicture()) break;
        t.setTarget(translateTarget());
        g.log->write("info", "translate region (drag)");
        t.translateRegion();
        break;
    case CmdTranslateOriginal:
        if (t.active()) t.setShowOriginal(!t.showOriginal());
        break;
    case CmdTranslateLive:
        if (!t.live() && !needPicture()) break;
        t.setTarget(translateTarget());
        {
            const bool cold = !t.live() && !t.active();
            t.setLive(!t.live(), kLiveSeconds);
            g.log->write("info", t.live() ? (cold ? "translate live on (first run: whole picture)" : "translate live on") : "translate live off");
        }
        if (t.live()) returnToLive("live mode");  // live follows the moving picture
        break;
    case CmdTranslateClose:
        t.close();
        g.log->write("info", "translate closed (menu)");
        returnToLive("closed");
        break;
    case CmdTrLayoutAuto:
    case CmdTrLayoutInPlace:
    case CmdTrLayoutList: {
        g.settings.translateLayout = static_cast<int>(cmd - CmdTrLayoutAuto);
        saveSettings();
        g.window->setTextOverlayStyle(g.settings.translateLayout, g.settings.translateDark);
        static const S names[3] = {S::MenuTrLayoutAuto, S::MenuTrLayoutInPlace, S::MenuTrLayoutList};
        g.window->showToast(fmt(S::TrLayoutToast, {tr(names[g.settings.translateLayout])}));
        g.log->write("info", std::string("translate layout ") + Settings::kLayoutKeys[g.settings.translateLayout]);
        break;
    }
    case CmdTrDarkCards:
        g.settings.translateDark = !g.settings.translateDark;
        saveSettings();
        g.window->setTextOverlayStyle(g.settings.translateLayout, g.settings.translateDark);
        g.log->write("info", g.settings.translateDark ? "translate cards dark" : "translate cards lens");
        break;
    case CmdTrTargetZh:
    case CmdTrTargetEn:
    case CmdTrTargetJa:
    case CmdTrTargetKo: {
        g.settings.translateTo = cmd == CmdTrTargetZh ? 1 : cmd == CmdTrTargetEn ? 2 : cmd == CmdTrTargetJa ? 3 : 4;
        saveSettings();
        t.setTarget(translateTarget());
        g.window->showToast(fmt(S::TrTargetToast, {pm::translate::langName(translateTarget())}));
        break;
    }
    }
    refreshToolbar();
}

}  // namespace pm_app
