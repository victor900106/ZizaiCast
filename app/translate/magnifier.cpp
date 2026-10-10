// 自在投影 app: translate/magnifier.cpp — translate（放大鏡、翻譯的 UI 接線）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "translate/translate.h"

namespace pm_app {

// The picture went away (or came): translation closed, zoom back to 1×.
void syncViewTools() {
    const bool on = viewAvailable();
    if (on == g.viewToolsOn) return;
    g.viewToolsOn = on;
    if (on) return;
    if (g.translator) g.translator->close();
    g.window->resetMagnifier();
    g.window->setFrozen(false);
    g.userFrozen = false;
}

// A translation ended (closed, nothing found, live mode on): the picture goes
// back to live unless the user froze it with 凍結.  ScreenTranslator only
// unfreezes what it froze itself, which misses e.g. Ctrl+L during a region
// selection (the selection froze it): the picture then stayed frozen while
// the phone went on.
void returnToLive(const char* why) {
    if (!g.window->viewState().frozen) return;
    if (g.userFrozen) {
        g.log->write("info", std::string("translation ") + why + ": picture stays frozen (凍結 is on)");
        return;
    }
    g.window->setFrozen(false);
    g.log->write("info", std::string("picture back to live (translation ") + why + ")");
    refreshPausedTitle();
}

// Menus / shortcuts / toolbar: nothing to act on without a picture.
bool needPicture() {
    if (viewAvailable()) return true;
    g.window->showToast(tr(S::MenuNeedPicture));
    return false;
}

void zoomBy(int steps) {
    if (needPicture()) g.window->zoomStep(steps);
}

void zoomReset() {
    if (needPicture()) g.window->resetMagnifier();
}

// Toolbar 放大鏡: 1× → 2× → 4× → 1× (fine steps: Ctrl+wheel / Ctrl+= / Ctrl+-).
void magnifierCycle() {
    if (!needPicture()) return;
    const float z = g.window->viewState().zoom;
    if (z < 1.95f) g.window->setZoom(2);
    else if (z < 3.95f) g.window->setZoom(4);
    else g.window->resetMagnifier();
}

void setFilterOption(int f, bool toast) {
    if (f < 0 || f > 4) return;
    if (f != g.settings.filter) {
        g.settings.filter = f;
        saveSettings();
    }
    g.window->setFilter(kFilters[f]);
    g.log->write("info", std::string("filter ") + Settings::kFilterKeys[f]);
    if (toast) g.window->showToast(fmt(S::FilterToast, {tr(kFilterNames[f])}));
}

void toggleFreeze() {
    if (!needPicture()) return;
    const bool on = !g.window->viewState().frozen;
    g.window->setFrozen(on);
    g.userFrozen = on;
    g.log->write("info", on ? "picture frozen" : "picture unfrozen");
    refreshPausedTitle();
}

}  // namespace pm_app
