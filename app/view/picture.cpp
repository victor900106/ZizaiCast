// 自在投影 app: view/picture.cpp — view（視窗動作、檢視選項）。
// 拆檔 0.7.9：自 app/main.cpp 原樣搬出

#include "core/base/app_state.h"
#include "core/base/base.h"
#include "help/help.h"
#include "share/share.h"
#include "capture/capture.h"
#include "translate/translate.h"
#include "connect/connect.h"
#include "view/view.h"

namespace pm_app {

// ---- 畫面: rotation / flip / iPhone frame ----------------------------------------
void applyView(bool refit) {
    g.window->setRotation(g.settings.rotation);
    g.window->setMirrored(g.settings.mirrored);
    g.window->setDeviceFrame(g.settings.deviceFrame);
    // Give the renderer a moment to take the new shape before fitting to it.
    if (refit) SetTimer(g.hwnd, kFitTimer, 60, nullptr);
}

void rotateView(int quarterTurns) {
    g.settings.rotation = (g.settings.rotation + quarterTurns + 4) % 4;
    saveSettings();
    applyView(true);
    g.window->showToast(fmt(S::RotateToast, {std::to_wstring(g.settings.rotation * 90)}));
}

void toggleMirror() {
    g.settings.mirrored = !g.settings.mirrored;
    saveSettings();
    applyView(false);
    g.window->showToast(tr(g.settings.mirrored ? S::FlipOn : S::FlipOff));
}

void resetView() {
    const bool changed = g.settings.rotation != 0 || g.settings.mirrored;
    g.settings.rotation = 0;
    g.settings.mirrored = false;
    saveSettings();
    g.window->resetMagnifier();  // 還原 also ends the magnifier (zoom 1×)
    applyView(changed);
    g.window->showToast(tr(S::ViewResetToast));
}

void toggleDeviceFrame() {
    g.settings.deviceFrame = !g.settings.deviceFrame;
    saveSettings();
    applyView(true);
    g.window->showToast(tr(g.settings.deviceFrame ? S::FrameOn : S::FrameOff));
}

// Idle screen / overlays (video) and the popup menus follow the theme.
void applyTheme() {
    const pm::VideoWindow::Theme t = kThemes[g.settings.theme & 3];
    g.window->setTheme(t);
    const std::array<uint32_t, 3> sw = pm::VideoWindow::themeSwatch(t);
    pm::ui::setPalette(sw[0], sw[1], sw[2]);
    g.pairPanel.retheme();
    g.aboutPanel.retheme();
    g.updatePanel.retheme();
    g.sharePanel.retheme();
    g.shareChip.retheme();
    g.applyChip.retheme();
    g.sharePicker.retheme();
    g.askPanel.retheme();
    g.closePanel.retheme();
    g.keysPanel.retheme();
    pm::ui::trset::retheme();
}

void setTheme(int t) {
    if (t < 0 || t > 3) return;
    const bool changed = t != g.settings.theme;
    g.settings.theme = t;
    if (changed) saveSettings();
    applyTheme();
    g.window->showToast(fmt(S::ThemeToast, {tr(kThemeNames[t])}));
}

}  // namespace pm_app
